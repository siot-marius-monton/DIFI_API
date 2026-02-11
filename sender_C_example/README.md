# sender_C_example

DPDK **secondary** process that attaches to the shared mempool and rings created by **difi_dpdk_receiver** (or module_b). It produces deterministic 8-bit IQ chunks and enqueues them to the per-stream rings. The primary process (difi_dpdk_receiver) dequeues them and sends DIFI over UDP.

## Build

Requires DPDK with `pkg-config` (e.g. `libdpdk.pc`).

```bash
cd sender_C_example
mkdir build && cd build
cmake ..
make
```

Binary: `build/sender_C_example`.

## Run

**Start the primary first** (difi_dpdk_receiver or module_b), then run the sender with the **same** EAL options (`--file-prefix`, `--base-virtaddr`, `--legacy-mem`). If you see an ASLR warning, run both under `setarch`.

```bash
# Terminal 1: primary – start FIRST with setarch and -m 512
sudo setarch $(uname -m) -R ./difi_dpdk_receiver/build/difi_dpdk_receiver \
  --proc-type=primary --file-prefix=iqdemo --base-virtaddr=0x2000000000 --legacy-mem -m 512 -- \
  --streams 16 --chunk-ms 2 --dest 127.0.0.1:50000

# Terminal 2: sender (after primary is running) – same EAL options including -m 512
sudo setarch $(uname -m) -R ./sender_C_example/build/sender_C_example \
  --proc-type=secondary --file-prefix=iqdemo --base-virtaddr=0x2000000000 --legacy-mem -m 512 -- \
  --streams 16 --chunk-ms 2
```

**If you get "Segmentation fault":** The primary must be started with `setarch` first and both must use the same EAL args (including `-m 512`). Or use **`--use-shm`** on both commands (add after `--chunk-ms 2`); then run the script instead: `sudo ./difi_dpdk_receiver/run_difi_receiver.sh`.

Application options (after `--`): `--streams N`, `--chunk-ms N`, `--file-prefix P`, `--use-shm` (must match primary), `--no-rate-limit` (or `--max-rate`), `--workers W`.

**`--no-rate-limit`** / **`--max-rate`**: Produce as fast as the rings allow (no real-time throttle). Use for max throughput when the receiver can drain at ~8000 packets/s; otherwise rings fill and the sender may drop.

**`--workers W`**: Number of worker threads (default 1). Streams are partitioned across W workers so each per-stream ring stays single-producer (SPSC). For **W > 1** you must provide at least W EAL lcores, e.g. `-l 0,1,2,3` for 4 workers (main on lcore 0, workers on 1–3). If you pass only one lcore (e.g. `-l 0`) and request `--workers 4`, the sender will exit with an error. With real-time rate limit, N workers can approach ~8000 chunks/s; with `--no-rate-limit`, N workers push as fast as they can until the receiver or rings become the bottleneck.
