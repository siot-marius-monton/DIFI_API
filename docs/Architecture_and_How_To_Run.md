# DIFI API Test: Architecture and How to Run

This document describes the architecture of the DIFI IQ pipeline test and how to build and run it.

---

## 1. Architecture Overview

The test runs a **two-process DPDK pipeline** on one host:

- **Primary process (difi_dpdk_receiver):** Creates shared memory (mempool or POSIX shm), one **SPSC ring per stream**, and a **free ring** for shm slot recycling. It dequeues IQ chunks from the rings, wraps them in DIFI headers, and sends UDP packets to a configurable destination.
- **Secondary process (sender_C_example):** Attaches to the primary’s mempool/rings (or shm + rings), produces deterministic 8-bit IQ chunks, and enqueues them into the per-stream rings.

A separate **DIFI receiver** (e.g. `difi_recv` from DIFI_C_Lib) can run on the same host or a remote host to receive and decode the UDP stream.

```
+------------------+     shared rings + mempool/shm      +------------------------+
| sender_C_example  |  -------------------------------->  | difi_dpdk_receiver      |
| (DPDK secondary) |   enqueue chunk per stream          | (DPDK primary)          |
|                  |                                     |                        |
| - N workers      |   <--------------------------------  | - Dequeue per stream   |
| - Rate limit or  |   (free_ring: slot return when      | - DIFI header + payload |
|   no-rate-limit  |    using --use-shm)                 | - sendmsg/sendto UDP   |
+------------------+                                     +------------------------+
                                                                      |
                                                                      v
                                                               UDP (host:port)
                                                                      |
                                                                      v
                                                        +------------------------+
                                                        | difi_recv (optional)    |
                                                        | Decode / record IQ      |
                                                        +------------------------+
```

**Rings:**

- **Per-stream rings (SPSC):** One producer (sender) per stream, one consumer (receiver). Each element is either an mbuf pointer (mempool path) or a shm slot index (shm path).
- **Free ring (MP/MC when using shm):** Holds available shm slot IDs. Created by the primary with multi-producer/multi-consumer flags so multiple sender workers can take and return slots.

**Data path:**

- **Mbuf path (default):** Sender allocates mbufs from the shared mempool, fills `iq_chunk_hdr` + IQ payload, enqueues mbuf pointer to the stream ring. Receiver dequeues, overwrites the first 32 bytes with the DIFI header (zero-copy payload), sends via UDP.
- **Shm path (`--use-shm`):** Primary creates a POSIX shm segment and a free ring of slot IDs. Sender dequeues a slot ID from the free ring, fills the shm slot (header + payload), enqueues slot ID to the stream ring. Receiver dequeues slot ID, builds DIFI packet (header + copy of payload from shm), sends. Shm avoids DPDK multi-process mempool mapping issues on some platforms (e.g. NXP/ARM).

---

## 2. Components

| Component | Type | Role |
|-----------|------|------|
| **difi_dpdk_receiver** | DPDK primary | Creates mempool/shm and rings; dequeues chunks; sends DIFI over UDP. |
| **sender_C_example** | DPDK secondary | Produces IQ chunks; enqueues to per-stream rings; optional multi-worker. |
| **run_multi_process.sh** | Script | Starts primary then secondary with setarch and shared EAL/app options. |
| **difi_recv** (DIFI_C_Lib) | Optional | Receives DIFI on UDP, decodes and reports/stores IQ. |

Shared definitions (ring names, chunk header, sample rate, etc.) live in `include/common.h` in both `difi_dpdk_receiver` and `sender_C_example`.

---

## 3. Prerequisites

- **DPDK** with `pkg-config` (e.g. `libdpdk.pc`).
- **Hugepages** configured if using the default mempool path (e.g. `echo 1024 | sudo tee /sys/kernel/mm/hugepages/hugepages-2048kB/nr_hugepages` and mount `/mnt/huge` if required).
- **DIFI_C_Lib** for `difi_recv` (optional); difi_dpdk_receiver builds DIFI_C_Lib as a subdirectory when present.

---

## 4. Build

From the repo root:

```bash
# Receiver (primary)
cd difi_dpdk_receiver && mkdir -p build && cd build && cmake .. && make && cd ../..

# Sender (secondary)
cd sender_C_example && mkdir -p build && cd build && cmake .. && make && cd ../..
```

Binaries:

- `difi_dpdk_receiver/build/difi_dpdk_receiver`
- `sender_C_example/build/sender_C_example`

---

## 5. How to Run

### 5.1. One-command script (recommended)

From the **DIFI_API** directory:

```bash
sudo ./run_multi_process.sh
```

The script:

1. Starts **difi_dpdk_receiver** (primary) with setarch, EAL options, and app options (`--streams 16 --chunk-ms 2 --use-shm --dest 127.0.0.1:50000`).
2. Waits 3 seconds, then starts **sender_C_example** (secondary) with matching EAL options and app options.
3. Ctrl+C stops the sender first; the script then kills the receiver.

**Environment:**

- `DIFI_DEST=host:port` — override UDP destination (default `127.0.0.1:50000`).
- `DIFI_MAX_RATE=1` — add `--no-rate-limit` to the sender for maximum throughput.

### 5.2. Manual run (two terminals)

**Order:** Start the **primary first**. The secondary blocks in EAL init until it can attach to the primary.

**Terminal 1 – primary:**

```bash
sudo setarch $(uname -m) -R ./difi_dpdk_receiver/build/difi_dpdk_receiver \
  --proc-type=primary --file-prefix=iqdemo --base-virtaddr=0x2000000000 --legacy-mem -m 512 -- \
  --streams 16 --chunk-ms 2 --use-shm --dest 127.0.0.1:50000
```

**Terminal 2 – secondary (after primary is up):**

```bash
sudo setarch $(uname -m) -R ./sender_C_example/build/sender_C_example \
  --proc-type=secondary --file-prefix=iqdemo --base-virtaddr=0x2000000000 --legacy-mem -m 512 -- \
  --streams 16 --chunk-ms 2 --use-shm
```

EAL options (before `--`) must match on both processes. The `-R` flag disables ASLR so the secondary can map the primary’s memory; without it the secondary may segfault.

### 5.3. EAL and application options

**EAL (before `--`):**

- `--proc-type=primary` | `--proc-type=secondary` — must match the process role.
- `--file-prefix=iqdemo` — prefix for DPDK multi-process socket and resource names; must be identical on both.
- `--base-virtaddr=0x2000000000` — base VA for DPDK memory; same on both.
- `--legacy-mem -m 512` — hugepage memory; same on both.
- **Multi-worker sender:** use at least N lcores, e.g. `-l 0,1,2,3` for 4 workers.

**Application (after `--`):**

| Option | Primary (receiver) | Secondary (sender) | Description |
|--------|--------------------|--------------------|-------------|
| `--streams N` | yes | yes | Number of streams (1–16); must match. |
| `--chunk-ms N` | yes | yes | Chunk duration in ms; must match. |
| `--file-prefix P` | yes | yes | Match EAL prefix. |
| `--dest host:port` | yes | no | UDP destination for DIFI packets. |
| `--use-shm` | yes | yes | Use POSIX shm for chunk data (recommended on NXP/ARM). |
| `--no-rate-limit` | no | yes | Sender produces at max rate (receiver must keep up). |
| `--workers W` | no | yes | Sender worker threads (default 1); need W lcores in EAL. |

### 5.4. Multi-worker sender

To increase sender throughput, run with multiple workers and enough lcores:

```bash
# Example: 4 workers, 4 lcores
sudo setarch $(uname -m) -R ./sender_C_example/build/sender_C_example \
  --proc-type=secondary --file-prefix=iqdemo --base-virtaddr=0x2000000000 --legacy-mem -m 512 -l 0,1,2,3 -- \
  --streams 16 --chunk-ms 2 --use-shm --workers 4
```

Streams are partitioned across workers; each per-stream ring stays single-producer. With real-time rate limit, N workers can approach ~8000 chunks/s; with `--no-rate-limit`, throughput is limited by the receiver or rings.

---

## 6. Cleanup after kill or crash

DPDK leaves runtime files under `/var/run/dpdk/<file-prefix>/`. If the primary was killed without a clean shutdown, remove that directory before starting again:

```bash
sudo rm -rf /var/run/dpdk/iqdemo
```

Then start the **primary** again, then the secondary.

---

## 7. Optional: Verify with difi_recv

To receive and decode the DIFI stream (e.g. on the same host):

1. Start **difi_recv** (from DIFI_C_Lib):  
   `./difi_recv --bind 0.0.0.0:50000`
2. Start **difi_dpdk_receiver** (primary) with `--dest 127.0.0.1:50000`.
3. Start **sender_C_example** (secondary).

Stop order: Ctrl+C on the sender, then the receiver, then difi_recv.

---

## 8. Troubleshooting

| Issue | What to do |
|-------|------------|
| Secondary segfaults | Run the **primary** with `setarch` first; use the **same** EAL options on both. Or use **`--use-shm`** on both to avoid shared mempool mapping. |
| “File exists” / “Cannot reserve memory” | Remove `/var/run/dpdk/iqdemo` (or your file-prefix) and restart primary first. |
| NXP/ARM: secondary still fails | Use `--use-shm` on both; if problems persist, use the single-process demo: `cd single_process_demo/build && sudo ./single_process_demo -l 0 -- --streams 1 --chunk-ms 2`. |
| `--workers 4` fails | Pass at least 4 lcores to EAL (e.g. `-l 0,1,2,3`). |

---

## 9. More Detail

- **Data formats, rates, and sequence:** [DIFI_IQ_Dataflow.md](DIFI_IQ_Dataflow.md)
- **Receiver options and throughput:** [difi_dpdk_receiver/README.md](../difi_dpdk_receiver/README.md)
- **Sender options and workers:** [sender_C_example/README.md](../sender_C_example/README.md)
