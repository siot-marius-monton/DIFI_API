/**
 * Common definitions for IQ streaming prototype (Module A and Module B).
 * This file is duplicated in both projects; keep contents identical.
 */
#ifndef IQ_COMMON_H
#define IQ_COMMON_H

#include <stdint.h>
#include <stdio.h>
#include <rte_common.h>

/* -------------------------------------------------------------------------
 * Chunk header: at start of each IQ chunk payload in an mbuf.
 * Packed to avoid padding; both producer and consumer must use same layout.
 * ------------------------------------------------------------------------- */
#define IQ_CHUNK_MAGIC  0x48435149u  /* "IQCH" (I=0x49 Q=0x51 C=0x43 H=0x48) LE */
#define IQ_CHUNK_VERSION 1u

#define IQ_DEFAULT_SAMPLE_RATE_HZ  7680000u   /* 7.68 Msps */
#define IQ_DEFAULT_CHUNK_MS        2u
#define IQ_MAX_STREAMS             16u

struct iq_chunk_hdr {
	uint32_t magic;
	uint16_t version;
	uint16_t stream_id;
	uint64_t seq;
	uint64_t timestamp_ns;
	uint32_t payload_len;
	uint32_t reserved;
} __attribute__((__packed__));

/* -------------------------------------------------------------------------
 * Naming: ring and mempool names must match between primary and secondary.
 * Use a common prefix (CLI option, default "iqdemo").
 * Ring name: prefix_ring_<stream_id>, e.g. "iqdemo_ring_0"
 * Mempool name: prefix_mbuf, e.g. "iqdemo_mbuf"
 * ------------------------------------------------------------------------- */
#define IQ_MEMPOOL_SUFFIX "_mbuf"
#define IQ_RING_PREFIX    "_ring_"

/* Build mempool name: buffer must hold prefix + "_mbuf" + NUL */
static inline void iq_mempool_name(const char *prefix, char *out, unsigned out_len)
{
	snprintf(out, (size_t)out_len, "%s_mbuf", prefix);
}

/* Build ring name for stream_id: buffer must hold prefix + "_ring_N" + NUL */
static inline void iq_ring_name(const char *prefix, uint16_t stream_id, char *out, unsigned out_len)
{
	snprintf(out, (size_t)out_len, "%s_ring_%u", prefix, (unsigned)stream_id);
}

/* -------------------------------------------------------------------------
 * Optional shared-memory (shm) path for NXP/ARM where DPDK mempool mapping
 * fails in secondary. Chunk data lives in POSIX shm; rings pass slot indices.
 * ------------------------------------------------------------------------- */
#define IQ_SHM_SLOT_SIZE  65536u   /* max chunk size per slot; must be >= total_chunk_bytes */
#define IQ_SHM_N_SLOTS    512u
#define IQ_SHM_BASE_VA    ((uintptr_t)0x3000000000ULL)

/* POSIX shm name (leading slash; keep short for NAME_MAX). */
static inline void iq_shm_name(const char *prefix, char *out, unsigned out_len)
{
	snprintf(out, (size_t)out_len, "/%s_chunks", prefix);
}

/* Ring name for free-slot pool (used only when --use-shm). */
static inline void iq_free_ring_name(const char *prefix, char *out, unsigned out_len)
{
	snprintf(out, (size_t)out_len, "%s_free", prefix);
}

/* -------------------------------------------------------------------------
 * Chunk size math (must match between A and B for same chunk_ms and sample_rate_hz):
 *   samples_per_chunk = round(sample_rate_hz * chunk_ms / 1000.0)
 *   payload_bytes     = samples_per_chunk * 2   (I and Q bytes)
 *   total_chunk_bytes = sizeof(struct iq_chunk_hdr) + payload_bytes
 * Ensure total_chunk_bytes <= mbuf data room (e.g. 64KB).
 * ------------------------------------------------------------------------- */
static inline uint32_t iq_samples_per_chunk(uint32_t sample_rate_hz, uint32_t chunk_ms)
{
	return (uint32_t)(((uint64_t)sample_rate_hz * (uint64_t)chunk_ms + 500u) / 1000u);
}

static inline uint32_t iq_payload_bytes(uint32_t samples_per_chunk)
{
	return samples_per_chunk * 2u;
}

static inline uint32_t iq_total_chunk_bytes(uint32_t payload_bytes)
{
	return (uint32_t)sizeof(struct iq_chunk_hdr) + payload_bytes;
}

/* Deterministic payload byte at offset i (payload index), for stream_id and seq */
static inline uint8_t iq_payload_byte_at(uint16_t stream_id, uint64_t seq, uint32_t i)
{
	return (uint8_t)((stream_id & 0xFFu) ^ (uint8_t)(seq & 0xFFu) ^ (uint8_t)(i & 0xFFu));
}

#endif /* IQ_COMMON_H */
