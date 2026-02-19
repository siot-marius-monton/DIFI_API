# DIFI API

A test and reference implementation for streaming **IQ data** over **DIFI** (Digital IF Interoperability) using a **DPDK-based pipeline**. Producer processes enqueue IQ chunks into shared rings; a primary process drains the rings, wraps the data in DIFI packets, and sends them over UDP.

## Purpose

- **Validate** a high-throughput path from in-memory IQ chunks to DIFI over UDP.
- **Provide a reference** for integrating third-party software with the receiver via DPDK rings.
- **Exercise** DPDK multi-process (primary/secondary) with shared mempool and rings, optional dedicated send core, and batched send (sendmmsg).

Data is 8-bit interleaved I/Q at **7.68 Msps**, with up to **16 streams** and configurable chunk size (time- or sample-based).

## Architecture (high level)

- **Primary process** (`difi_dpdk_receiver`): Creates a DPDK mempool and one SPSC ring per stream. Drains chunks from the rings, validates a 32-byte header, overwrites it with a DIFI header, and sends UDP packets. Can use a second lcore as a dedicated send worker (internal send_ring / pool_ring, sendmmsg batching).
- **Secondary process** (e.g. `sender_C_example`): Attaches to the primary’s mempool and rings, produces IQ chunks (e.g. deterministic test data), and enqueues mbuf pointers to the per-stream rings.
- **Optional**: A DIFI receiver (e.g. `difi_recv` from DIFI_C_Lib) on the same or another host receives and decodes the UDP stream.

All shared structures (chunk header, ring/mempool names, sample rate) are defined in a common header used by both primary and producer.

## Source code layout

| Path | Description |
|------|-------------|
| **difi_dpdk_receiver/** | DPDK primary: mempool, per-stream rings, drain loop, DIFI encapsulation, UDP send. Optional dedicated send core. |
| **sender_C_example/** | DPDK secondary: reference producer; allocates mbufs, fills chunk header + IQ payload, enqueues to rings. Optional multi-worker. |
| **sender_cpp_example/** | Alternative C++ producer (secondary). |
| **module_b/** | Validation-only consumer (no DIFI send); shares the same ring/mempool layout. |
| **single_process_demo/** | Single-process demo (no multi-process DPDK). |
| **docs/** | Architecture, dataflow, run instructions, third-party integration guide. |
| **run_multi_process.sh** | Script to start primary then secondary with shared EAL/app options. |

Shared definitions (chunk header, ring names, etc.): `difi_dpdk_receiver/include/common.h` and `sender_C_example/include/common.h` (kept in sync).

## Prerequisites

- **DPDK** with `pkg-config` (e.g. `libdpdk.pc`).
- **Hugepages** configured (e.g. `echo 1024 | sudo tee /sys/kernel/mm/hugepages/hugepages-2048kB/nr_hugepages`).
- **DIFI_C_Lib** (optional), for `difi_recv`; difi_dpdk_receiver can build it as a submodule when present.

## Build

From the repo root:

```bash
# Receiver (primary)
cd difi_dpdk_receiver && mkdir -p build && cd build && cmake .. && make && cd ../..

# Sender (secondary)
cd sender_C_example && mkdir -p build && cd build && cmake .. && make && cd ../..
```

Binaries: `difi_dpdk_receiver/build/difi_dpdk_receiver`, `sender_C_example/build/sender_C_example`.

## Run

**Quick start:** From the repo root, run the primary then the secondary (e.g. use the script):

```bash
sudo ./run_multi_process.sh
```

Start the **primary first**; the secondary blocks until it can attach. Use **separate lcores** for primary and secondary (e.g. `-l 0` for receiver, `-l 1` for sender). For details, options, and troubleshooting see the docs below.

## Documentation

| Document | Contents |
|----------|----------|
| [docs/Architecture_and_How_To_Run.md](docs/Architecture_and_How_To_Run.md) | Architecture, build, run, EAL/app options, stats, troubleshooting. |
| [docs/DIFI_IQ_Dataflow.md](docs/DIFI_IQ_Dataflow.md) | End-to-end dataflow, formats, rates, deployment, stats and errors. |
| [docs/Third_Party_Integration_DPDK_Rings.md](docs/Third_Party_Integration_DPDK_Rings.md) | How to integrate a third-party producer with the receiver via DPDK rings. |
| [difi_dpdk_receiver/README.md](difi_dpdk_receiver/README.md) | Receiver build, options, and usage. |
