/*
 * difi_dpdk_receiver: DPDK primary process. Creates shared mempool and one
 * SPSC ring per stream; dequeues IQ chunks, overwrites the chunk header with
 * a DIFI data header (zero-copy for payload), and sends DIFI over UDP.
 * Data: 8-bit IQ at 7.68 Msps, up to 16 streams.
 */

#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/socket.h>
#include <sys/uio.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>

#include <rte_eal.h>
#include <rte_errno.h>
#include <rte_mbuf.h>
#include <rte_mempool.h>
#include <rte_ring.h>
#include <rte_cycles.h>

#include "common.h"
#include "difi.h"

#define RING_SIZE         512
#define MBUF_POOL_SIZE    4096
#define MBUF_DATA_SIZE    65535

#define DIFI_HEADER_BYTES  32

/* Big-endian stores (used in hot path; no difi_fill_data_header_i8) */
static inline void store_be32(uint8_t *p, uint32_t val)
{
	p[0] = (uint8_t)((val >> 24) & 0xFF);
	p[1] = (uint8_t)((val >> 16) & 0xFF);
	p[2] = (uint8_t)((val >> 8) & 0xFF);
	p[3] = (uint8_t)(val & 0xFF);
}
static inline void store_be64(uint8_t *p, uint64_t val)
{
	store_be32(p, (uint32_t)(val >> 32));
	store_be32(p + 4, (uint32_t)(val & 0xFFFFFFFFULL));
}

static volatile int g_quit;

static void sigint_handler(int sig)
{
	(void)sig;
	g_quit = 1;
}

/* App options */
static uint16_t g_streams       = 16;
static uint32_t g_chunk_ms      = IQ_DEFAULT_CHUNK_MS;
static char     g_file_prefix[32] = "iqdemo";
static int      g_use_shm       = 0;
static char     g_dest_addr[64] = "127.0.0.1";
static uint16_t g_dest_port     = 50000;

static struct rte_ring *g_rings[IQ_MAX_STREAMS];
static struct rte_ring *g_free_ring;
static void    *g_shm_base;
static int      g_shm_fd = -1;

static int g_udp_sock = -1;
static struct sockaddr_in g_dest_saddr;

/* Pre-calculated for fixed payload: packet size in 32-bit words */
static uint16_t g_packet_size_words;
static uint32_t g_payload_bytes;
static uint32_t g_total_chunk_bytes;

/* Pre-filled DIFI header: Class ID (12 bytes) and header word0 with seq=0 (4 bytes, host order) */
static uint8_t  g_class_id_blob[12];
static uint32_t g_word0_template;

/* Pre-allocated send buffers for --use-shm (one per stream; header pre-filled, only seq+ts+payload updated) */
static uint8_t *g_shm_difi_bufs[IQ_MAX_STREAMS];

/* Pre-allocated DIFI header buffers for mbuf path (one per stream); used with sendmsg iovec to avoid touching mbuf payload */
static uint8_t *g_mbuf_header_bufs[IQ_MAX_STREAMS];

/* Per-stream stats */
static uint64_t g_sent[IQ_MAX_STREAMS];
static uint64_t g_seq_errors[IQ_MAX_STREAMS];
static uint64_t g_last_tsc;
static uint64_t g_last_sent_total;

static int parse_app_args(int argc, char **argv)
{
	for (int i = 0; i < argc; i++) {
		if (strcmp(argv[i], "--streams") == 0 && i + 1 < argc) {
			g_streams = (uint16_t)atoi(argv[++i]);
			if (g_streams > IQ_MAX_STREAMS) g_streams = IQ_MAX_STREAMS;
		} else if (strcmp(argv[i], "--chunk-ms") == 0 && i + 1 < argc) {
			g_chunk_ms = (uint32_t)atoi(argv[++i]);
		} else if (strcmp(argv[i], "--file-prefix") == 0 && i + 1 < argc) {
			snprintf(g_file_prefix, sizeof(g_file_prefix), "%s", argv[++i]);
		} else if (strcmp(argv[i], "--use-shm") == 0) {
			g_use_shm = 1;
		} else if (strcmp(argv[i], "--dest") == 0 && i + 1 < argc) {
			const char *dest = argv[++i];
			const char *colon = strrchr(dest, ':');
			if (colon && colon != dest) {
				size_t host_len = (size_t)(colon - dest);
				if (host_len >= sizeof(g_dest_addr)) host_len = sizeof(g_dest_addr) - 1;
				memcpy(g_dest_addr, dest, host_len);
				g_dest_addr[host_len] = '\0';
				g_dest_port = (uint16_t)atoi(colon + 1);
				if (g_dest_port == 0) g_dest_port = 50000;
			} else {
				snprintf(g_dest_addr, sizeof(g_dest_addr), "%s", dest);
			}
		}
	}
	return 0;
}

static int open_udp_socket(void)
{
	int s = socket(AF_INET, SOCK_DGRAM, 0);
	if (s < 0) {
		perror("socket");
		return -1;
	}
	memset(&g_dest_saddr, 0, sizeof(g_dest_saddr));
	g_dest_saddr.sin_family = AF_INET;
	g_dest_saddr.sin_port = htons(g_dest_port);
	if (inet_pton(AF_INET, g_dest_addr, &g_dest_saddr.sin_addr) != 1) {
		fprintf(stderr, "Invalid destination address: %s\n", g_dest_addr);
		close(s);
		return -1;
	}
	return s;
}

/* Convert timestamp_ns to DIFI integer sec + fractional picoseconds */
static void timestamp_ns_to_difi(uint64_t timestamp_ns, uint32_t *ts_sec, uint64_t *ts_ps)
{
	*ts_sec = (uint32_t)(timestamp_ns / 1000000000ULL);
	*ts_ps  = (timestamp_ns % 1000000000ULL) * 1000ULL;
}

/* Pre-compute DIFI header constants (Class ID + word0 template with seq=0). Call once after g_packet_size_words is set. */
static void init_difi_header_templates(void)
{
	/* Class ID: OUI 0x7C386C (DIFI_DEFAULT_OUI), reserved, InfoClass 0x0000, PacketClass 0x0000, reserved 4 */
	g_class_id_blob[0] = (uint8_t)((DIFI_DEFAULT_OUI >> 16) & 0xFF);
	g_class_id_blob[1] = (uint8_t)((DIFI_DEFAULT_OUI >> 8) & 0xFF);
	g_class_id_blob[2] = (uint8_t)(DIFI_DEFAULT_OUI & 0xFF);
	g_class_id_blob[3] = 0;
	g_class_id_blob[4] = (DIFI_INFO_CLASS_STANDARD >> 8) & 0xFF;
	g_class_id_blob[5] = DIFI_INFO_CLASS_STANDARD & 0xFF;
	g_class_id_blob[6] = (DIFI_PACKET_CLASS_STANDARD >> 8) & 0xFF;
	g_class_id_blob[7] = DIFI_PACKET_CLASS_STANDARD & 0xFF;
	g_class_id_blob[8] = g_class_id_blob[9] = g_class_id_blob[10] = g_class_id_blob[11] = 0;

	/* Header word 0: PTYPE=0x1, ClassID present, TSM=0, TSI=1, TSF=2, seq=0, packet_size_words */
	g_word0_template = ((uint32_t)DIFI_PTYPE_SIGNAL_DATA << 28)
		| 0x08000000u
		| ((uint32_t)DIFI_TSM_FINE << 24)
		| ((uint32_t)DIFI_TSI_UTC << 22)
		| ((uint32_t)DIFI_TSF_PICOSECONDS << 20)
		| (0u << 16)
		| (uint32_t)g_packet_size_words;
}

/* Write only the variable parts of the DIFI header (word0 with seq, stream_id, timestamp). Rest must be pre-filled or written once. */
static inline void write_difi_header_variable(uint8_t *buf, uint32_t stream_id, uint8_t seq,
	uint32_t ts_sec, uint64_t ts_ps)
{
	uint32_t word0 = g_word0_template | ((uint32_t)(seq & 0xF) << 16);
	store_be32(buf + 0, word0);
	store_be32(buf + 4, stream_id);
	store_be32(buf + 20, ts_sec);
	store_be64(buf + 24, ts_ps);
}

/* Send one DIFI packet (header already written in buf); buf is total_len bytes */
static int send_packet(const uint8_t *buf, uint32_t total_len)
{
	ssize_t n = sendto(g_udp_sock, buf, (size_t)total_len, 0,
	                   (const struct sockaddr *)&g_dest_saddr, sizeof(g_dest_saddr));
	if (n < 0 || (uint32_t)n != total_len) {
		if (n < 0) perror("sendto");
		return -1;
	}
	return 0;
}

/* Zero-copy send: header (pre-filled buffer) + payload (pointer into mbuf) via sendmsg iovec. No memcpy. */
static int send_packet_iov(const uint8_t *header, uint32_t header_len,
                           const uint8_t *payload, uint32_t payload_len)
{
	struct iovec iov[2];
	iov[0].iov_base = (void *)header;
	iov[0].iov_len  = (size_t)header_len;
	iov[1].iov_base = (void *)payload;
	iov[1].iov_len  = (size_t)payload_len;

	struct msghdr msg = {0};
	msg.msg_name    = (void *)&g_dest_saddr;
	msg.msg_namelen = sizeof(g_dest_saddr);
	msg.msg_iov     = iov;
	msg.msg_iovlen  = 2;

	ssize_t n = sendmsg(g_udp_sock, &msg, 0);
	uint32_t total = header_len + payload_len;
	if (n < 0 || (uint32_t)n != total) {
		if (n < 0) perror("sendmsg");
		return -1;
	}
	return 0;
}

int main(int argc, char **argv)
{
	int ret;
	uint32_t sample_rate_hz = IQ_DEFAULT_SAMPLE_RATE_HZ;
	uint32_t samples_per_chunk;
	uint64_t tsc_hz;
	char name[64];
	uint16_t s;

	g_quit = 0;
	signal(SIGINT, sigint_handler);

	int app_argc = 0;
	char **app_argv = NULL;
	for (int i = 1; i < argc; i++) {
		if (strcmp(argv[i], "--") == 0 && i + 1 < argc) {
			app_argc = argc - (i + 1);
			app_argv = &argv[i + 1];
			break;
		}
	}
	ret = rte_eal_init(argc, argv);
	if (ret < 0)
		rte_exit(EXIT_FAILURE, "rte_eal_init failed\n");
	if (app_argv)
		parse_app_args(app_argc, app_argv);

	samples_per_chunk = iq_samples_per_chunk(sample_rate_hz, g_chunk_ms);
	g_payload_bytes   = iq_payload_bytes(samples_per_chunk);
	g_total_chunk_bytes = iq_total_chunk_bytes(g_payload_bytes);

	/* Pre-calculate DIFI packet size in 32-bit words */
	{
		size_t packet_size_bytes = DIFI_HEADER_BYTES + (size_t)g_payload_bytes;
		g_packet_size_words = (uint16_t)((packet_size_bytes + 3u) / 4u);
	}
	init_difi_header_templates();

	tsc_hz = rte_get_tsc_hz();

	if (g_total_chunk_bytes > MBUF_DATA_SIZE) {
		rte_exit(EXIT_FAILURE,
			"Chunk size %u > mbuf data size %u; reduce --chunk-ms\n",
			(unsigned)g_total_chunk_bytes, (unsigned)MBUF_DATA_SIZE);
	}
	if (g_use_shm && g_total_chunk_bytes > IQ_SHM_SLOT_SIZE) {
		rte_exit(EXIT_FAILURE,
			"Chunk size %u > shm slot size %u\n",
			(unsigned)g_total_chunk_bytes, (unsigned)IQ_SHM_SLOT_SIZE);
	}

	g_udp_sock = open_udp_socket();
	if (g_udp_sock < 0)
		rte_exit(EXIT_FAILURE, "Failed to open UDP socket\n");

	if (!g_use_shm) {
		iq_mempool_name(g_file_prefix, name, sizeof(name));
		if (!rte_pktmbuf_pool_create(name, MBUF_POOL_SIZE, 0, 0,
				MBUF_DATA_SIZE + RTE_PKTMBUF_HEADROOM, rte_socket_id()))
			rte_exit(EXIT_FAILURE, "mempool create failed: %s\n", rte_strerror(rte_errno));
		/* Pre-allocate and pre-fill one DIFI header buffer per stream for zero-copy sendmsg (no write into mbuf) */
		for (s = 0; s < g_streams; s++) {
			g_mbuf_header_bufs[s] = malloc(DIFI_HEADER_BYTES);
			if (!g_mbuf_header_bufs[s])
				rte_exit(EXIT_FAILURE, "malloc mbuf_header_buf stream %u failed\n", (unsigned)s);
			uint8_t *b = g_mbuf_header_bufs[s];
			store_be32(b + 0, g_word0_template);
			store_be32(b + 4, (uint32_t)s);
			memcpy(b + 8, g_class_id_blob, 12);
			memset(b + 20, 0, 12);
		}
	}

	for (s = 0; s < g_streams; s++) {
		iq_ring_name(g_file_prefix, s, name, sizeof(name));
		g_rings[s] = rte_ring_create(name, RING_SIZE, rte_socket_id(),
			RING_F_SP_ENQ | RING_F_SC_DEQ);
		if (!g_rings[s])
			rte_exit(EXIT_FAILURE, "ring create %s failed: %s\n", name, rte_strerror(rte_errno));
	}

	if (g_use_shm) {
		size_t shm_size = (size_t)IQ_SHM_N_SLOTS * (size_t)IQ_SHM_SLOT_SIZE;
		iq_shm_name(g_file_prefix, name, sizeof(name));
		g_shm_fd = shm_open(name, O_CREAT | O_RDWR, 0666);
		if (g_shm_fd < 0)
			rte_exit(EXIT_FAILURE, "shm_open %s failed\n", name);
		if (ftruncate(g_shm_fd, (off_t)shm_size) != 0)
			rte_exit(EXIT_FAILURE, "ftruncate shm failed\n");
		g_shm_base = mmap((void *)IQ_SHM_BASE_VA, shm_size,
			PROT_READ | PROT_WRITE, MAP_SHARED | MAP_FIXED, g_shm_fd, 0);
		if (g_shm_base == MAP_FAILED)
			rte_exit(EXIT_FAILURE, "mmap shm at %p failed (try setarch for same VA)\n", (void *)IQ_SHM_BASE_VA);
		iq_free_ring_name(g_file_prefix, name, sizeof(name));
		g_free_ring = rte_ring_create(name, (unsigned)IQ_SHM_N_SLOTS, rte_socket_id(),
			RING_F_SP_ENQ | RING_F_SC_DEQ);
		if (!g_free_ring)
			rte_exit(EXIT_FAILURE, "free ring create failed: %s\n", rte_strerror(rte_errno));
		for (uint32_t i = 0; i < IQ_SHM_N_SLOTS; i++) {
			while (rte_ring_sp_enqueue(g_free_ring, (void *)(uintptr_t)i) != 0)
				;
		}
		/* Pre-allocate and pre-fill one DIFI send buffer per stream (header constant parts only) */
		for (s = 0; s < g_streams; s++) {
			g_shm_difi_bufs[s] = malloc(DIFI_HEADER_BYTES + g_payload_bytes);
			if (!g_shm_difi_bufs[s])
				rte_exit(EXIT_FAILURE, "malloc shm_difi_buf stream %u failed\n", (unsigned)s);
			uint8_t *b = g_shm_difi_bufs[s];
			store_be32(b + 0, g_word0_template);
			store_be32(b + 4, (uint32_t)s);
			memcpy(b + 8, g_class_id_blob, 12);
			memset(b + 20, 0, 12);
		}
	}

	memset(g_sent, 0, sizeof(g_sent));
	memset(g_seq_errors, 0, sizeof(g_seq_errors));
	g_last_tsc = rte_rdtsc();
	g_last_sent_total = 0;

	printf("difi_dpdk_receiver (primary): streams=%u chunk_ms=%u dest=%s:%u%s\n",
		(unsigned)g_streams, (unsigned)g_chunk_ms, g_dest_addr, (unsigned)g_dest_port,
		g_use_shm ? " (shm)" : "");

	/* Consumer loop: dequeue, fill DIFI header in-place, send over UDP */
	while (!g_quit) {
		for (s = 0; s < g_streams; s++) {
			void *obj;
			if (rte_ring_sc_dequeue(g_rings[s], &obj) != 0)
				continue;

			if (g_use_shm) {
				uint32_t slot_id = (uint32_t)(uintptr_t)obj;
				if (slot_id >= IQ_SHM_N_SLOTS) continue;
				struct iq_chunk_hdr *hdr = (struct iq_chunk_hdr *)((char *)g_shm_base + slot_id * IQ_SHM_SLOT_SIZE);
				uint8_t *payload_ptr = (uint8_t *)(hdr + 1);

				if (hdr->magic != IQ_CHUNK_MAGIC || hdr->version != IQ_CHUNK_VERSION)
					continue;
				if (hdr->stream_id >= g_streams || hdr->payload_len != g_payload_bytes) {
					rte_ring_sp_enqueue(g_free_ring, obj);
					continue;
				}

				uint8_t *difi_buf = g_shm_difi_bufs[s];
				uint32_t ts_sec;
				uint64_t ts_ps;
				timestamp_ns_to_difi(hdr->timestamp_ns, &ts_sec, &ts_ps);
				write_difi_header_variable(difi_buf, (uint32_t)hdr->stream_id,
					(uint8_t)(hdr->seq & 0xF), ts_sec, ts_ps);
				memcpy(difi_buf + DIFI_HEADER_BYTES, payload_ptr, g_payload_bytes);
				if (send_packet(difi_buf, DIFI_HEADER_BYTES + g_payload_bytes) == 0)
					g_sent[s]++;
				rte_ring_sp_enqueue(g_free_ring, obj);
			} else {
				struct rte_mbuf *chunk_mbuf = (struct rte_mbuf *)obj;
				struct iq_chunk_hdr *hdr = rte_pktmbuf_mtod(chunk_mbuf, struct iq_chunk_hdr *);

				if (hdr->magic != IQ_CHUNK_MAGIC || hdr->version != IQ_CHUNK_VERSION) {
					rte_pktmbuf_free(chunk_mbuf);
					continue;
				}
				if (hdr->stream_id >= g_streams || hdr->payload_len != g_payload_bytes) {
					rte_pktmbuf_free(chunk_mbuf);
					continue;
				}

				uint8_t *payload_ptr = rte_pktmbuf_mtod(chunk_mbuf, uint8_t *) + DIFI_HEADER_BYTES;
				uint32_t ts_sec;
				uint64_t ts_ps;
				timestamp_ns_to_difi(hdr->timestamp_ns, &ts_sec, &ts_ps);
				write_difi_header_variable(g_mbuf_header_bufs[s], (uint32_t)hdr->stream_id,
					(uint8_t)(hdr->seq & 0xF), ts_sec, ts_ps);

				if (send_packet_iov(g_mbuf_header_bufs[s], DIFI_HEADER_BYTES, payload_ptr, g_payload_bytes) == 0)
					g_sent[s]++;
				rte_pktmbuf_free(chunk_mbuf);
			}
		}

		/* Stats every 1 second */
		{
			uint64_t tsc_now = rte_rdtsc();
			if (tsc_now - g_last_tsc >= tsc_hz) {
				uint64_t total = 0;
				for (s = 0; s < g_streams; s++)
					total += g_sent[s];
				double sec = (double)(tsc_now - g_last_tsc) / (double)tsc_hz;
				uint64_t d = total - g_last_sent_total;
				g_last_tsc = tsc_now;
				g_last_sent_total = total;
				printf("DIFI RX: sent %" PRIu64 "/s (dest %s:%u)\n",
					(uint64_t)((double)d / sec), g_dest_addr, (unsigned)g_dest_port);
			}
		}
	}

	/* Final summary */
	{
		uint64_t total = 0;
		for (s = 0; s < g_streams; s++)
			total += g_sent[s];
		printf("\n=== difi_dpdk_receiver final ===\n");
		printf("Total DIFI packets sent %" PRIu64 "\n", total);
	}

	if (g_udp_sock >= 0)
		close(g_udp_sock);
	if (g_use_shm) {
		for (s = 0; s < g_streams; s++)
			free(g_shm_difi_bufs[s]);
	} else {
		for (s = 0; s < g_streams; s++)
			free(g_mbuf_header_bufs[s]);
	}
	rte_eal_cleanup();
	return 0;
}
