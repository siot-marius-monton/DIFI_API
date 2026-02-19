# difi_dpdk_receiver – DIFI over UDP from DPDK IQ rings

DPDK **primary** process that creates the shared mempool and one SPSC ring per stream (same as module_b), dequeues IQ chunks from the rings, overwrites the chunk header with a DIFI data header (zero-copy for payload), and sends DIFI packets over **standard UDP**. Compatible with sender_C_example (or sender_cpp_example) as the secondary producer.

- **Data**: Fixed-point 8-bit IQ at **7.68 Msps**, up to **16 streams** (configurable).
- **Performance**: In-place DIFI header write (no payload copy); pre-calculated header fields.

## Build

Requires DPDK with `pkg-config` (e.g. `libdpdk.pc`). DIFI_C_Lib is built automatically as a subdirectory (expected at `../../DIFI_C_Lib` or `../DIFI_C_Lib`).

```bash
cd difi_dpdk_receiver
mkdir build && cd build
cmake ..
make
```

Executable: `build/difi_dpdk_receiver`.

## Hugepages

Same as module_b: ensure hugepages are configured before first run (e.g. `echo 1024 | sudo tee /sys/kernel/mm/hugepages/hugepages-2048kB/nr_hugepages` and mount `/mnt/huge` if needed).

## Run

Start **difi_dpdk_receiver first** (primary), then start the sender (secondary) with the same EAL memory options. Use setarch if you see ASLR warnings.

**Recommended: run each process on a separate CPU core** using the EAL option `-l` (lcore list). This avoids contention and gives zero dropped packets. With this configuration, **16 streams and 1 worker** run fine with no drops.

**Terminal 1 (primary) – start this first; pin to core 0 with `-l 0`:**

```bash
sudo setarch $(uname -m) -R ./build/difi_dpdk_receiver \
  --proc-type=primary --file-prefix=iqdemo --base-virtaddr=0x2000000000 --legacy-mem -m 512 -l 0 -- \
  --streams 16 --chunk-ms 2 --dest 127.0.0.1:50000
```

**Terminal 2 (secondary) – pin to core 1 with `-l 1`; same EAL memory options as primary:**

```bash
sudo setarch $(uname -m) -R ./sender_C_example/build/sender_C_example \
  --proc-type=secondary --file-prefix=iqdemo --base-virtaddr=0x2000000000 --legacy-mem -m 512 -l 1 -- \
  --streams 16 --chunk-ms 2
```

The `-l` value can differ between primary and secondary (each process has its own lcores). Use any free cores, e.g. `-l 2` and `-l 3` instead of 0 and 1.

**If sender_C_example segfaults:** The primary must be started **with setarch first** so its memory is at a fixed address. Use the **exact same** EAL options on both (including `-m 512`). Or use **`--use-shm`** for both to avoid shared mempool mapping: add `--use-shm` after `--chunk-ms 2` on both commands; then setarch is still recommended but the segfault from ASLR is avoided.

### Application options (after `--`)

| Option | Description | Default |
|--------|-------------|---------|
| `--streams N` | Number of streams (1–16) | 16 |
| `--chunk-ms N` | Chunk duration in ms (must match sender) | 2 |
| `--samples-per-chunk N` | IQ samples per chunk (overrides chunk-ms; must match sender; use for low latency) | off |
| `--file-prefix P` | Prefix for ring/mempool names (match sender) | iqdemo |
| `--dest host:port` | UDP destination for DIFI packets | 127.0.0.1:50000 |
| `--use-shm` | Use POSIX shm for chunk data (NXP/ARM compatibility) | off |
| `--eob-on-exit` | On exit, send one DIFI context packet per stream with End-of-Burst (SEI) set | off |
| `--eos-on-exit` | On exit, send one DIFI context packet per stream with End-of-Stream (SEI) set | off |

On exit, the application prints performance metrics separately for **inbound** (chunks dequeued from producer rings) and **outbound** (DIFI packets sent over UDP): chunk/packet counts, bytes (wire and payload), throughput (chunks/packets per second and Mbps), and per-stream breakdown. Outbound section includes theoretical rate and utilization %.

**Throughput:** Theoretical payload rate is 7.68 Msps × 2 bytes/sample (8-bit I+Q) × 16 streams ≈ **1966 Mbps**. Achieved rate can be lower because (1) the sender is real-time rate-limited (one chunk per stream every `chunk_ms`), so max packet rate is 8000/s for 16 streams at 2 ms chunks; (2) the single-threaded receiver may not drain the rings at 8000/s, so the pipeline runs at the slower of the two. The sender supports **`--workers N`** to partition streams across N lcores (give at least N lcores via EAL, e.g. `-l 0,1,2,3` for 4 workers); with rate limit this can approach 8000/s. To approach 2 Gbps, run the sender with **`--no-rate-limit`** and ensure the receiver keeps up (e.g. sufficient CPU); both must sustain ~8000 packets/s.

EAL options (before `--`) that must match the sender: `--proc-type=primary` / `secondary`, `--file-prefix`, `--base-virtaddr`, `--legacy-mem`, `-m`. The **`-l` (lcore list)** can and should differ: use **`-l 0`** for the receiver and **`-l 1`** for the sender so they run on separate cores (zero drops with 16 streams, 1 worker).

**Low latency:** Use `--samples-per-chunk N` on both primary and sender to fix chunk size by samples instead of time. Example: `--samples-per-chunk 256` gives chunk duration 256 / 7.68e6 ≈ **33.3 µs** (vs 2 ms at default chunk-ms). Primary: `--samples-per-chunk 256 --dest ...`; sender: `--samples-per-chunk 256` (and optionally `--use-shm`). Packet rate becomes 7.68e6/256 ≈ 30,000 packets/s per stream.

## Optional: run script

From the DIFI_API directory you can run the receiver and sender together (same idea as `run_multi_process.sh` but for the DIFI receiver):

```bash
# Build both first, then from DIFI_API:
sudo ./difi_dpdk_receiver/run_difi_receiver.sh
```

See `run_difi_receiver.sh` in this directory for the exact commands.

## Testing with difi_recv

To verify the DIFI stream, run **difi_recv** (from DIFI_C_Lib) bound to the same IP:port that **difi_dpdk_receiver** sends to. Use three terminals. The receiver sends one standard context packet (PTYPE 0x4) per stream at startup with payload format 8-bit IQ so difi_recv reports correct sample count and data rate.

**1. Build DIFI_C_Lib (if not already built):**

```bash
cd /path/to/DIFI_C_Lib
mkdir -p build && cd build
cmake ..
make
# difi_recv is at build/difi_recv
```

**2. Start difi_recv (receive DIFI on port 50000):**

```bash
# From DIFI_C_Lib/build – bind to port 50000 (default), quiet data logging optional
./difi_recv --bind 0.0.0.0:50000

# Less verbose (only context/version and stats): add -q
./difi_recv --bind 0.0.0.0:50000 -q
```

Leave this running. It will print decoded context/version packets and data-packet info (stream ID, sequence, timestamps, IQ count, optional loss).

**3. Start difi_dpdk_receiver (primary) – with setarch, `-m 512`, and `-l 0` (separate core):**

```bash
# From DIFI_API
sudo setarch $(uname -m) -R ./difi_dpdk_receiver/build/difi_dpdk_receiver \
  --proc-type=primary --file-prefix=iqdemo --base-virtaddr=0x2000000000 --legacy-mem -m 512 -l 0 -- \
  --streams 16 --chunk-ms 2 --dest 127.0.0.1:50000
```

**4. Start sender_C_example (secondary) – same EAL memory options, `-l 1` for separate core:**

```bash
sudo setarch $(uname -m) -R ./sender_C_example/build/sender_C_example \
  --proc-type=secondary --file-prefix=iqdemo --base-virtaddr=0x2000000000 --legacy-mem -m 512 -l 1 -- \
  --streams 16 --chunk-ms 2
```

If the sender segfaults, see **“If sender_C_example segfaults”** in the Run section above, or use the script: `sudo ./difi_dpdk_receiver/run_difi_receiver.sh` (it uses `--use-shm` and setarch for both).

**What you should see**

- **difi_dpdk_receiver:** “DIFI RX: sent N/s (dest 127.0.0.1:50000)” every second.
- **difi_recv:** Data packets (e.g. stream ID, seq, sample count); with 8-bit IQ it will decode as I8. Use `-q` to reduce per-packet data logs; use `-o out.raw` to write I/Q to a file.

**Stop order:** Ctrl+C **sender_C_example** first, then **difi_dpdk_receiver**, then **difi_recv**.

## Output

- Every 1 second: DIFI packets sent per second and destination.
- On Ctrl+C: total DIFI packets sent.

## Zero-copy

With mbufs (default): the first 32 bytes of each dequeued mbuf (the IQ chunk header) are overwritten with the DIFI data header; the IQ payload is not copied. The same buffer is then sent with `sendto()`. With `--use-shm`, the receiver builds each DIFI packet in a temporary buffer (header + copy of payload) because shm slots are not one contiguous packet.
