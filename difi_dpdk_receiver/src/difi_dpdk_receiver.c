/*
 * difi_dpdk_receiver: DPDK primary process. Creates shared mempool and one
 * SPSC ring per stream; dequeues IQ chunks, overwrites the chunk header with
 * a DIFI data header (zero-copy for payload), and sends DIFI over UDP.
 * Data: 8-bit IQ at 7.68 Msps, up to 16 streams.
 * Uses sendmmsg() to send one packet per stream in a single syscall (batch).
 */
#define _GNU_SOURCE

#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>
#include <sys/socket.h>
#include <sys/uio.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>

#include <rte_eal.h>
#include <rte_errno.h>
#include <rte_launch.h>
#include <rte_lcore.h>
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

/* Dedicated send core: pool of contiguous buffers for drain -> send_ring -> send worker.
 * DPDK ring capacity is count-1; use ring size > pool size so initial fill of pool_ring succeeds. */
#define SEND_POOL_SIZE    4096
#define SEND_RING_SIZE    8192
#define SEND_BATCH_MAX    16

struct send_item {
	uint8_t  *buf;
	uint16_t  stream_id;
};

static struct send_item *g_send_pool;
static struct rte_ring *g_pool_ring;   /* available send_items (send worker produces, drain consumes) */
static struct rte_ring *g_send_ring;   /* to-send (drain produces, send worker consumes) */
static uint32_t g_packet_len;          /* DIFI_HEADER_BYTES + g_payload_bytes for copy */

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
static uint32_t g_samples_per_chunk = 0;  /* if > 0, use directly (overrides chunk_ms) */
static char     g_file_prefix[32] = "iqdemo";
static char     g_dest_addr[64] = "127.0.0.1";
static uint16_t g_dest_port     = 50000;
static int      g_eob_on_exit   = 0;  /* send context packet with EOB on exit */
static int      g_eos_on_exit   = 0;  /* send context packet with EOS on exit */
static int      g_no_send       = 0;  /* if set, drain rings but do not send UDP (for bottleneck testing) */

static struct rte_ring *g_rings[IQ_MAX_STREAMS];

static int g_udp_sock = -1;
static struct sockaddr_in g_dest_saddr;

/* Pre-calculated for fixed payload: packet size in 32-bit words */
static uint16_t g_packet_size_words;
static uint32_t g_payload_bytes;
static uint32_t g_total_chunk_bytes;

/* Pre-filled DIFI header: Class ID (12 bytes) and header word0 with seq=0 (4 bytes, host order) */
static uint8_t  g_class_id_blob[12];
static uint32_t g_word0_template;

/* Pre-allocated DIFI header buffers (one per stream); used with sendmsg iovec to avoid touching mbuf payload */
static uint8_t *g_mbuf_header_bufs[IQ_MAX_STREAMS];

/* Per-stream stats: inbound = dequeued from rings, outbound = sent over UDP */
static uint64_t g_dequeued[IQ_MAX_STREAMS];  /* inbound: chunks received from producer */
static uint64_t g_sent[IQ_MAX_STREAMS];     /* outbound: DIFI packets sent */
static uint64_t g_seq_errors[IQ_MAX_STREAMS];
static uint64_t g_inbound_errors;   /* dequeued but not sent (validation fail or no pool buffer) */
static uint64_t g_outbound_errors;  /* sendmmsg/sendto failure or partial send */
static uint64_t g_last_tsc;
static uint64_t g_last_dequeued_total;
static uint64_t g_last_sent_total;
static uint64_t g_start_tsc;  /* TSC at start of consumer loop (for duration) */
static uint64_t g_tsc_in_send_interval;  /* TSC ticks spent in send_packet/send_packet_iov during current 1s interval (Step 3 bottleneck) */

static int parse_app_args(int argc, char **argv)
{
	for (int i = 0; i < argc; i++) {
		if (strcmp(argv[i], "--streams") == 0 && i + 1 < argc) {
			g_streams = (uint16_t)atoi(argv[++i]);
			if (g_streams > IQ_MAX_STREAMS) g_streams = IQ_MAX_STREAMS;
		} else if (strcmp(argv[i], "--chunk-ms") == 0 && i + 1 < argc) {
			g_chunk_ms = (uint32_t)atoi(argv[++i]);
		} else if (strcmp(argv[i], "--samples-per-chunk") == 0 && i + 1 < argc) {
			g_samples_per_chunk = (uint32_t)atoi(argv[++i]);
		} else if (strcmp(argv[i], "--file-prefix") == 0 && i + 1 < argc) {
			snprintf(g_file_prefix, sizeof(g_file_prefix), "%s", argv[++i]);
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
		} else if (strcmp(argv[i], "--eob-on-exit") == 0) {
			g_eob_on_exit = 1;
		} else if (strcmp(argv[i], "--eos-on-exit") == 0) {
			g_eos_on_exit = 1;
		} else if (strcmp(argv[i], "--no-send") == 0) {
			g_no_send = 1;
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
	if (g_no_send)
		return 0;
	uint64_t tsc_before = rte_rdtsc();
	ssize_t n = sendto(g_udp_sock, buf, (size_t)total_len, 0,
	                   (const struct sockaddr *)&g_dest_saddr, sizeof(g_dest_saddr));
	g_tsc_in_send_interval += (rte_rdtsc() - tsc_before);
	if (n < 0 || (uint32_t)n != total_len) {
		if (n < 0) perror("sendto");
		return -1;
	}
	return 0;
}

/* Send one DIFI context packet per stream with optional EOB/EOS in SEI (on exit). */
static void send_sei_context_packets_on_exit(void)
{
	uint8_t ctx_buf[256];
	difi_context_t ctx;
	difi_result_t res;
	size_t len;
	uint16_t s;

	for (s = 0; s < g_streams; s++) {
		uint32_t stream_id = (uint32_t)s;
		res = difi_init_standard_context(
			&ctx,
			stream_id,
			0,                                    /* reference_point */
			(uint64_t)IQ_DEFAULT_SAMPLE_RATE_HZ,  /* bandwidth_hz */
			0,                                    /* if_ref_hz */
			2400000000ULL,                        /* rf_ref_hz */
			0,                                    /* if_band_offset_hz */
			(int16_t)(-30.0 * 256),               /* reference_level_dbm */
			(int16_t)(20.0 * 256),                /* gain_db */
			(uint64_t)IQ_DEFAULT_SAMPLE_RATE_HZ,  /* sample_rate_hz */
			0, 0, 0,                              /* ts_adjust_ps, ts_cal_time_s, state_event_flags */
			(uint16_t)DIFI_PAYLOAD_FORMAT_I8);
		if (res != DIFI_OK)
			continue;
		if (g_eob_on_exit)
			difi_context_set_eob(&ctx, true);
		if (g_eos_on_exit)
			difi_context_set_eos(&ctx, true);
		if (!g_eob_on_exit && !g_eos_on_exit)
			continue;
		res = difi_pack_context_class0(&ctx, ctx_buf, sizeof(ctx_buf), &len);
		if (res != DIFI_OK)
			continue;
		send_packet(ctx_buf, (uint32_t)len);
	}
}

/* Send one standard context packet per stream at startup so difi_recv knows payload is 8-bit before first data. */
static void send_startup_context_packets(void)
{
	uint8_t ctx_buf[256];
	difi_context_t ctx;
	difi_result_t res;
	size_t len;
	uint16_t s;

	for (s = 0; s < g_streams; s++) {
		uint32_t stream_id = (uint32_t)s;
		res = difi_init_standard_context(
			&ctx,
			stream_id,
			0,                                    /* reference_point */
			(uint64_t)IQ_DEFAULT_SAMPLE_RATE_HZ,  /* bandwidth_hz */
			0,                                    /* if_ref_hz */
			2400000000ULL,                        /* rf_ref_hz */
			0,                                    /* if_band_offset_hz */
			(int16_t)(-30.0 * 256),               /* reference_level_dbm */
			(int16_t)(20.0 * 256),                /* gain_db */
			(uint64_t)IQ_DEFAULT_SAMPLE_RATE_HZ,  /* sample_rate_hz */
			0, 0, 0,                              /* ts_adjust_ps, ts_cal_time_s, state_event_flags */
			(uint16_t)DIFI_PAYLOAD_FORMAT_I8);
		if (res != DIFI_OK)
			continue;
		res = difi_pack_context_class0(&ctx, ctx_buf, sizeof(ctx_buf), &len);
		if (res != DIFI_OK)
			continue;
		send_packet(ctx_buf, (uint32_t)len);
	}
}

/* Zero-copy send: header (pre-filled buffer) + payload (pointer into mbuf) via sendmsg iovec. No memcpy. */
static int send_packet_iov(const uint8_t *header, uint32_t header_len,
	const uint8_t *payload, uint32_t payload_len)
{
	if (g_no_send)
		return 0;
	uint64_t tsc_before = rte_rdtsc();
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
	g_tsc_in_send_interval += (rte_rdtsc() - tsc_before);
	uint32_t total = header_len + payload_len;
	if (n < 0 || (uint32_t)n != total) {
		if (n < 0) perror("sendmsg");
		return -1;
	}
	return 0;
}

/* Dedicated send core: dequeue from g_send_ring, sendmmsg in batches, return to g_pool_ring */
static int send_worker(void *arg)
{
	(void)arg;
	static struct mmsghdr msgvec[SEND_BATCH_MAX];
	static struct iovec iovs[SEND_BATCH_MAX];
	static struct send_item *batch_items[SEND_BATCH_MAX];
	unsigned int n;

	while (!g_quit || rte_ring_count(g_send_ring) > 0) {
		n = 0;
		while (n < SEND_BATCH_MAX) {
			struct send_item *item;
			if (rte_ring_sc_dequeue(g_send_ring, (void **)&item) != 0)
				break;
			batch_items[n] = item;
			memset(&msgvec[n].msg_hdr, 0, sizeof(struct msghdr));
			msgvec[n].msg_hdr.msg_name = (void *)&g_dest_saddr;
			msgvec[n].msg_hdr.msg_namelen = sizeof(g_dest_saddr);
			iovs[n].iov_base = item->buf;
			iovs[n].iov_len  = (size_t)g_packet_len;
			msgvec[n].msg_hdr.msg_iov = &iovs[n];
			msgvec[n].msg_hdr.msg_iovlen = 1;
			n++;
		}
		if (n == 0) {
			if (!g_quit)
				continue;
			break;
		}
		uint64_t tsc_before = rte_rdtsc();
		int sent = sendmmsg(g_udp_sock, msgvec, n, 0);
		__atomic_fetch_add(&g_tsc_in_send_interval, (rte_rdtsc() - tsc_before), __ATOMIC_RELAXED);
		for (int i = 0; i < sent; i++)
			__atomic_fetch_add(&g_sent[batch_items[i]->stream_id], 1, __ATOMIC_RELAXED);
		if (sent >= 0 && sent < (int)n)
			__atomic_fetch_add(&g_outbound_errors, (unsigned int)n - (unsigned int)sent, __ATOMIC_RELAXED);
		else if (sent < 0)
			__atomic_fetch_add(&g_outbound_errors, n, __ATOMIC_RELAXED);
		for (unsigned int i = 0; i < n; i++) {
			while (rte_ring_sp_enqueue(g_pool_ring, batch_items[i]) != 0)
				;
		}
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

	if (g_samples_per_chunk > 0)
		samples_per_chunk = g_samples_per_chunk;
	else
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
			"Chunk size %u > mbuf data size %u; reduce --chunk-ms or --samples-per-chunk\n",
			(unsigned)g_total_chunk_bytes, (unsigned)MBUF_DATA_SIZE);
	}

	if (!g_no_send) {
		g_udp_sock = open_udp_socket();
		if (g_udp_sock < 0)
			rte_exit(EXIT_FAILURE, "Failed to open UDP socket\n");
	} else {
		g_udp_sock = -1;
	}

	iq_mempool_name(g_file_prefix, name, sizeof(name));
	if (!rte_pktmbuf_pool_create(name, MBUF_POOL_SIZE, 0, 0,
			MBUF_DATA_SIZE, rte_socket_id()))
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

	for (s = 0; s < g_streams; s++) {
		iq_ring_name(g_file_prefix, s, name, sizeof(name));
		g_rings[s] = rte_ring_create(name, RING_SIZE, rte_socket_id(),
			RING_F_SP_ENQ | RING_F_SC_DEQ);
		if (!g_rings[s])
			rte_exit(EXIT_FAILURE, "ring create %s failed: %s\n", name, rte_strerror(rte_errno));
	}

	g_packet_len = DIFI_HEADER_BYTES + g_payload_bytes;

	unsigned int n_lcores = rte_lcore_count();
	int use_dedicated_send = (n_lcores >= 2 && !g_no_send);

	if (use_dedicated_send) {
		g_send_pool = calloc(SEND_POOL_SIZE, sizeof(struct send_item));
		if (!g_send_pool)
			rte_exit(EXIT_FAILURE, "malloc send_pool failed\n");
		for (unsigned int i = 0; i < SEND_POOL_SIZE; i++) {
			g_send_pool[i].buf = malloc((size_t)g_packet_len);
			if (!g_send_pool[i].buf)
				rte_exit(EXIT_FAILURE, "malloc send_pool[%u].buf failed\n", i);
		}
		snprintf(name, sizeof(name), "%s_difi_pool", g_file_prefix);
		g_pool_ring = rte_ring_create(name, SEND_RING_SIZE, rte_socket_id(),
			RING_F_SP_ENQ | RING_F_SC_DEQ);
		if (!g_pool_ring)
			rte_exit(EXIT_FAILURE, "pool_ring create failed: %s\n", rte_strerror(rte_errno));
		snprintf(name, sizeof(name), "%s_difi_send", g_file_prefix);
		g_send_ring = rte_ring_create(name, SEND_RING_SIZE, rte_socket_id(),
			RING_F_SP_ENQ | RING_F_SC_DEQ);
		if (!g_send_ring)
			rte_exit(EXIT_FAILURE, "send_ring create failed: %s\n", rte_strerror(rte_errno));
		for (unsigned int i = 0; i < SEND_POOL_SIZE; i++) {
			while (rte_ring_sp_enqueue(g_pool_ring, &g_send_pool[i]) != 0)
				;
		}
	}

	memset(g_dequeued, 0, sizeof(g_dequeued));
	memset(g_sent, 0, sizeof(g_sent));
	memset(g_seq_errors, 0, sizeof(g_seq_errors));
	g_inbound_errors = 0;
	g_outbound_errors = 0;
	g_last_tsc = rte_rdtsc();
	g_start_tsc = g_last_tsc;
	g_last_dequeued_total = 0;
	g_last_sent_total = 0;

	printf("difi_dpdk_receiver (primary): streams=%u samples_per_chunk=%u dest=%s:%u%s%s%s%s\n",
		(unsigned)g_streams, (unsigned)samples_per_chunk, g_dest_addr, (unsigned)g_dest_port,
		g_eob_on_exit ? " eob-on-exit" : "",
		g_eos_on_exit ? " eos-on-exit" : "",
		g_no_send ? " NO-SEND (drain only)" : "",
		use_dedicated_send ? " dedicated-send" : "");

	/* Send one standard context per stream so difi_recv knows payload is 8-bit before first data */
	if (g_udp_sock >= 0)
		send_startup_context_packets();

	__atomic_store_n(&g_tsc_in_send_interval, 0, __ATOMIC_RELAXED);

	unsigned int send_lcore_id = RTE_MAX_LCORE;
	if (use_dedicated_send) {
		send_lcore_id = rte_get_next_lcore(rte_lcore_id(), 0, 0);
		if (send_lcore_id >= RTE_MAX_LCORE)
			rte_exit(EXIT_FAILURE, "need 2 lcores for dedicated send (e.g. -l 0,1)\n");
		rte_eal_remote_launch(send_worker, NULL, send_lcore_id);
	}

	/* Batch for single-thread path (no dedicated send) */
	static struct mmsghdr msgvec[IQ_MAX_STREAMS];
	static struct iovec iovs[IQ_MAX_STREAMS][2];
	static uint16_t batch_stream_ids[IQ_MAX_STREAMS];
	static void *batch_objs[IQ_MAX_STREAMS];

	/* Consumer loop */
	while (!g_quit) {
		unsigned int batch_count = 0;

		for (s = 0; s < g_streams && batch_count < IQ_MAX_STREAMS; s++) {
			void *obj;
			if (rte_ring_sc_dequeue(g_rings[s], &obj) != 0)
				continue;

			g_dequeued[s]++;

			{
				struct rte_mbuf *chunk_mbuf = (struct rte_mbuf *)obj;
				struct iq_chunk_hdr *hdr = rte_pktmbuf_mtod(chunk_mbuf, struct iq_chunk_hdr *);

				if (hdr->magic != IQ_CHUNK_MAGIC || hdr->version != IQ_CHUNK_VERSION) {
					rte_pktmbuf_free(chunk_mbuf);
					g_inbound_errors++; continue;
				}
				if (hdr->stream_id >= g_streams || hdr->payload_len != g_payload_bytes) {
					rte_pktmbuf_free(chunk_mbuf);
					g_inbound_errors++; continue;
				}

				uint8_t *payload_ptr = rte_pktmbuf_mtod(chunk_mbuf, uint8_t *) + sizeof(struct iq_chunk_hdr);
				uint32_t ts_sec;
				uint64_t ts_ps;
				timestamp_ns_to_difi(hdr->timestamp_ns, &ts_sec, &ts_ps);
				write_difi_header_variable(g_mbuf_header_bufs[s], (uint32_t)hdr->stream_id,
					(uint8_t)(hdr->seq & 0xF), ts_sec, ts_ps);

				if (use_dedicated_send) {
					struct send_item *item;
					if (rte_ring_sc_dequeue(g_pool_ring, (void **)&item) == 0) {
						memcpy(item->buf, g_mbuf_header_bufs[s], DIFI_HEADER_BYTES);
						memcpy(item->buf + DIFI_HEADER_BYTES, payload_ptr, (size_t)g_payload_bytes);
						item->stream_id = s;
						while (rte_ring_sp_enqueue(g_send_ring, item) != 0)
							;
					} else
						g_inbound_errors++;
					rte_pktmbuf_free(chunk_mbuf);
				} else {
					batch_stream_ids[batch_count] = s;
					batch_objs[batch_count] = (void *)chunk_mbuf;
					memset(&msgvec[batch_count].msg_hdr, 0, sizeof(struct msghdr));
					msgvec[batch_count].msg_hdr.msg_name = (void *)&g_dest_saddr;
					msgvec[batch_count].msg_hdr.msg_namelen = sizeof(g_dest_saddr);
					iovs[batch_count][0].iov_base = g_mbuf_header_bufs[s];
					iovs[batch_count][0].iov_len  = DIFI_HEADER_BYTES;
					iovs[batch_count][1].iov_base = payload_ptr;
					iovs[batch_count][1].iov_len  = (size_t)g_payload_bytes;
					msgvec[batch_count].msg_hdr.msg_iov = &iovs[batch_count][0];
					msgvec[batch_count].msg_hdr.msg_iovlen = 2;
					batch_count++;
				}
			}
		}

		if (!use_dedicated_send) {
			if (batch_count > 0 && !g_no_send) {
				uint64_t tsc_before = rte_rdtsc();
				int sent = sendmmsg(g_udp_sock, msgvec, batch_count, 0);
				__atomic_fetch_add(&g_tsc_in_send_interval, (rte_rdtsc() - tsc_before), __ATOMIC_RELAXED);
				if (sent > 0) {
					for (unsigned int i = 0; i < (unsigned int)sent && i < batch_count; i++)
						g_sent[batch_stream_ids[i]]++;
				}
				if (sent >= 0 && (unsigned int)sent < batch_count)
					g_outbound_errors += (unsigned int)batch_count - (unsigned int)sent;
				else if (sent < 0)
					g_outbound_errors += (unsigned int)batch_count;
			}
			for (unsigned int i = 0; i < batch_count; i++)
				rte_pktmbuf_free((struct rte_mbuf *)batch_objs[i]);
		}

		/* Stats every 1 second */
		{
			uint64_t tsc_now = rte_rdtsc();
			if (tsc_now - g_last_tsc >= tsc_hz) {
				uint64_t total_dq = 0, total_sent = 0;
				for (s = 0; s < g_streams; s++) {
					total_dq += g_dequeued[s];
					total_sent += __atomic_load_n(&g_sent[s], __ATOMIC_RELAXED);
				}
				double sec = (double)(tsc_now - g_last_tsc) / (double)tsc_hz;
				uint64_t d_dq = total_dq - g_last_dequeued_total;
				uint64_t d_sent = total_sent - g_last_sent_total;
				uint64_t interval_tsc = tsc_now - g_last_tsc;
				uint64_t tsc_send = __atomic_exchange_n(&g_tsc_in_send_interval, 0, __ATOMIC_RELAXED);
				double pct_send = (interval_tsc > 0) ? (100.0 * (double)tsc_send / (double)interval_tsc) : 0.0;
				uint64_t out_err = __atomic_load_n(&g_outbound_errors, __ATOMIC_RELAXED);
				double inbound_err_pct = (total_dq > 0) ? (100.0 * (double)g_inbound_errors / (double)total_dq) : 0.0;
				double outbound_err_pct = (total_sent + out_err > 0) ? (100.0 * (double)out_err / (double)(total_sent + out_err)) : 0.0;
				g_last_tsc = tsc_now;
				g_last_dequeued_total = total_dq;
				g_last_sent_total = total_sent;
				printf("DIFI RX: inbound %" PRIu64 "/s, outbound %" PRIu64 "/s (dest %s:%u) time_in_send %.1f%% in_err %.2f%% out_err %.2f%%\n",
					(uint64_t)((double)d_dq / sec), (uint64_t)((double)d_sent / sec),
					g_dest_addr, (unsigned)g_dest_port, pct_send, inbound_err_pct, outbound_err_pct);
			}
		}
	}

	if (use_dedicated_send && send_lcore_id < RTE_MAX_LCORE)
		rte_eal_wait_lcore(send_lcore_id);

	/* Optional: send context packets with EOB/EOS before exit */
	if (g_udp_sock >= 0 && (g_eob_on_exit || g_eos_on_exit))
		send_sei_context_packets_on_exit();

	/* Final summary and performance metrics: inbound vs outbound */
	{
		uint64_t total_dequeued = 0, total_sent = 0;
		for (s = 0; s < g_streams; s++) {
			total_dequeued += g_dequeued[s];
			total_sent += __atomic_load_n(&g_sent[s], __ATOMIC_RELAXED);
		}
		uint64_t end_tsc = rte_rdtsc();
		uint64_t duration_tsc = (end_tsc > g_start_tsc) ? (end_tsc - g_start_tsc) : 0;
		double duration_sec = (double)duration_tsc / (double)tsc_hz;

		/* Inbound: chunks from producer (ring payload = header + payload) */
		uint64_t inbound_bytes = total_dequeued * (uint64_t)(sizeof(struct iq_chunk_hdr) + g_payload_bytes);
		uint64_t inbound_payload = total_dequeued * (uint64_t)g_payload_bytes;
		double inbound_pps = (duration_sec > 0.0) ? ((double)total_dequeued / duration_sec) : 0.0;
		double inbound_mbps_wire = (duration_sec > 0.0) ? ((double)inbound_bytes * 8.0 / 1e6 / duration_sec) : 0.0;
		double inbound_mbps_payload = (duration_sec > 0.0) ? ((double)inbound_payload * 8.0 / 1e6 / duration_sec) : 0.0;

		/* Outbound: DIFI packets sent over UDP */
		uint64_t outbound_bytes = total_sent * (DIFI_HEADER_BYTES + g_payload_bytes);
		uint64_t outbound_payload = total_sent * (uint64_t)g_payload_bytes;
		double outbound_pps = (duration_sec > 0.0) ? ((double)total_sent / duration_sec) : 0.0;
		double outbound_mbps_wire = (duration_sec > 0.0) ? ((double)outbound_bytes * 8.0 / 1e6 / duration_sec) : 0.0;
		double outbound_mbps_payload = (duration_sec > 0.0) ? ((double)outbound_payload * 8.0 / 1e6 / duration_sec) : 0.0;

		double theoretical_mbps = (double)IQ_DEFAULT_SAMPLE_RATE_HZ * 2.0 * (double)g_streams * 8.0 / 1e6;
		double utilization_pct = (theoretical_mbps > 0.0) ? (100.0 * outbound_mbps_payload / theoretical_mbps) : 0.0;

		printf("\n=== difi_dpdk_receiver final ===\n");
		printf("Duration:         %.3f s\n\n", duration_sec);

		uint64_t total_out_err = __atomic_load_n(&g_outbound_errors, __ATOMIC_RELAXED);
		double in_err_pct = (total_dequeued > 0) ? (100.0 * (double)g_inbound_errors / (double)total_dequeued) : 0.0;
		double out_err_pct = (total_sent + total_out_err > 0) ? (100.0 * (double)total_out_err / (double)(total_sent + total_out_err)) : 0.0;

		printf("--- Inbound (from producer, ring dequeue) ---\n");
		printf("Chunks:           %" PRIu64 "\n", total_dequeued);
		printf("Errors:          %" PRIu64 " (%.2f%%)\n", g_inbound_errors, in_err_pct);
		printf("Bytes:            %" PRIu64 " (wire), %" PRIu64 " (payload)\n", inbound_bytes, inbound_payload);
		printf("Throughput:       %.1f chunks/s, %.2f Mbps (wire), %.2f Mbps (payload)\n\n",
			inbound_pps, inbound_mbps_wire, inbound_mbps_payload);

		printf("--- Outbound (to network, UDP send) ---\n");
		printf("Packets sent:     %" PRIu64 "\n", total_sent);
		printf("Errors:          %" PRIu64 " (%.2f%%)\n", total_out_err, out_err_pct);
		printf("Bytes sent:      %" PRIu64 " (wire), %" PRIu64 " (payload)\n", outbound_bytes, outbound_payload);
		printf("Throughput:       %.1f packets/s, %.2f Mbps (wire), %.2f Mbps (payload)\n",
			outbound_pps, outbound_mbps_wire, outbound_mbps_payload);
		printf("Theoretical:      %.2f Mbps (%.0f Msps x 2 B x %u streams); utilization %.1f%%\n\n",
			theoretical_mbps, (double)IQ_DEFAULT_SAMPLE_RATE_HZ / 1e6, (unsigned)g_streams, utilization_pct);

		if (g_streams <= 16) {
			printf("Per-stream inbound (dequeued): ");
			for (s = 0; s < g_streams; s++)
				printf("%" PRIu64 "%s", g_dequeued[s], (s + 1 < g_streams) ? ", " : "\n");
			printf("Per-stream outbound (sent):   ");
			for (s = 0; s < g_streams; s++)
				printf("%" PRIu64 "%s", __atomic_load_n(&g_sent[s], __ATOMIC_RELAXED), (s + 1 < g_streams) ? ", " : "\n");
		}
	}

	if (g_udp_sock >= 0)
		close(g_udp_sock);
	if (use_dedicated_send && g_send_pool) {
		for (unsigned int i = 0; i < SEND_POOL_SIZE; i++)
			free(g_send_pool[i].buf);
		free(g_send_pool);
		g_send_pool = NULL;
	}
	for (s = 0; s < g_streams; s++)
		free(g_mbuf_header_bufs[s]);
	rte_eal_cleanup();
	return 0;
}
