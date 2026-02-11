# DIFI API Test: Architecture and How to Run

This document describes the architecture of the DIFI IQ pipeline test and how to build and run it.

---

## 1. Architecture Overview

The test runs a **two-process DPDK pipeline** on one host:

- **Primary process (difi_dpdk_receiver):** Creates shared memory (mempool or POSIX shm), one **SPSC ring per stream**, and a **free ring** for shm slot recycling. It dequeues IQ chunks from the rings, wraps them in DIFI headers, and sends UDP packets to a configurable destination.
- **Secondary process (sender_C_example):** Attaches to the primary's mempool/rings (or shm + rings), produces deterministic 8-bit IQ chunks, and enqueues them into the per-stream rings.

A separate **DIFI receiver** (e.g. `difi_recv` from DIFI_C_Lib) can run on the same host or a remote host to receive and decode the UDP stream.

[Diagram and rest of doc - truncated for length; full content in repo]
