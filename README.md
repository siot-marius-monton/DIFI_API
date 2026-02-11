# DIFI_API

DIFI-compliant transport over 10 GbE: a two-process DPDK pipeline that sends 8-bit IQ at 7.68 Msps over UDP with configurable chunk size, MTU 9000 awareness, and batch send (sendmmsg).

## Contents

| Component | Description |
|-----------|-------------|
| **difi_dpdk_receiver** | DPDK primary: creates mempool/shm and per-stream rings, dequeues IQ chunks, adds DIFI headers, sends UDP. |
| **sender_C_example** | DPDK secondary: produces IQ chunks, enqueues to rings; optional multi-worker and rate limit. |
| **docs/** | Architecture, dataflow, spec compliance, and Confluence update content. |
| **run_multi_process.sh** | One-command script to run primary then secondary (setarch, EAL, app options). |

## Quick start

1. **Prerequisites:** DPDK (`pkg-config libdpdk`), hugepages. For building the receiver: [DIFI_C_Lib](https://github.com/DPDK/DIFI_C_Lib) as sibling `DIFI_C_Lib` or `difi_dpdk_receiver/DIFI_C_Lib`.
2. **Build:**
   ```bash
   cd difi_dpdk_receiver && mkdir -p build && cd build && cmake .. && make && cd ../..
   cd sender_C_example && mkdir -p build && cd build && cmake .. && make && cd ../..
   ```
3. **Run:** From repo root, `sudo ./run_multi_process.sh` (or see [docs/Architecture_and_How_To_Run.md](docs/Architecture_and_How_To_Run.md)).

## Documentation

- [Architecture and How to Run](docs/Architecture_and_How_To_Run.md)
- [DIFI IQ Dataflow](docs/DIFI_IQ_Dataflow.md)
- [Spec Compliance Check](docs/Spec_Compliance_Check.md)

## License

See repository license file.
