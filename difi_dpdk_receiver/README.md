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

Start **difi_dpdk_receiver first** (primary), then start the sender (secondary) with the same EAL options. Use setarch if you see ASLR warnings.

**Terminal 1 (primary):**

```bash
sudo setarch $(uname -m) -R ./build/difi_dpdk_receiver \
  --proc-type=primary --file-prefix=iqdemo --base-virtaddr=0x2000000000 --legacy-mem -- \
  --streams 16 --chunk-ms 2 --dest 127.0.0.1:50000
```

**Terminal 2 (secondary, after primary is running):**

```bash
sudo setarch $(uname -m) -R ./sender_C_example/build/sender_C_example \
  --proc-type=secondary --file-prefix=iqdemo --base-virtaddr=0x2000000000 --legacy-mem -- \
  --streams 16 --chunk-ms 2
```

### Application options (after `--`)

| Option | Description | Default |
|--------|-------------|---------|
| `--streams N` | Number of streams (1–16) | 16 |
| `--chunk-ms N` | Chunk duration in ms (must match sender) | 2 |
| `--file-prefix P` | Prefix for ring/mempool names (match sender) | iqdemo |
| `--dest host:port` | UDP destination for DIFI packets | 127.0.0.1:50000 |
| `--use-shm` | Use POSIX shm for chunk data (NXP/ARM compatibility) | off |

EAL options (before `--`) must match the sender: `--proc-type=primary`, `--file-prefix`, `--base-virtaddr`, `--legacy-mem`.

## Optional: run script

From the DIFI_API directory you can run the receiver and sender together (same idea as `run_multi_process.sh` but for the DIFI receiver):

```bash
# Build both first, then from DIFI_API:
sudo ./difi_dpdk_receiver/run_difi_receiver.sh
```

See `run_difi_receiver.sh` in this directory for the exact commands.

## Output

- Every 1 second: DIFI packets sent per second and destination.
- On Ctrl+C: total DIFI packets sent.

## Zero-copy

With mbufs (default): the first 32 bytes of each dequeued mbuf (the IQ chunk header) are overwritten with the DIFI data header; the IQ payload is not copied. The same buffer is then sent with `sendto()`. With `--use-shm`, the receiver builds each DIFI packet in a temporary buffer (header + copy of payload) because shm slots are not one contiguous packet.
