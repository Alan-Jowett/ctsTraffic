# cts-traffic — Functional Requirements Specification

**Sharded UDP receive path to eliminate IOCP enqueue bottlenecks**

**Scope:** cts-traffic project; receive-path sharding for high-volume UDP ingest

---

## Overview & Scope

**Purpose:** The cts-traffic system shall eliminate IOCP enqueue bottlenecks observed when a single UDP socket + IOCP is used for high-rate receive traffic by sharding UDP receive processing across multiple sockets and IOCPs aligned with CPU/RSS/NUMA topology. The specification is implementation‑agnostic and describes required behaviors, observability, and measurable acceptance criteria.

**In scope**
- Sharding the UDP receive path across multiple sockets and IOCPs per CPU/CPU‑group (NUMA/RSS aligned).
- Runtime configuration and telemetry required to validate and operate the sharded receive design.
- Correctness behaviors (per‑flow affinity, no cross‑shard buffer leakage) and graceful shutdown/drain.

**Out of scope**
- NIC driver redesigns or firmware changes (unless explicitly required and negotiated).
- Changes to kernel RSS hashing policies; the system shall align with RSS where available but shall not mandate kernel RSS rework.
- Send‑path performance optimizations, except where minimally required to preserve correctness during receive‑path changes.

---

## Goals & Non‑Goals

**Goals (shall)**
- The system shall scale near‑linearly with CPU count for UDP receive throughput up to a practical `Nmax`.
- The system shall preserve per‑flow affinity so packets for a given 4‑tuple are processed on a single shard.
- The system shall minimize packet drops under offered load `L`, bounding drops to ≤ `C%` under acceptance tests.
- The system shall bound receive‑to‑callback latency and provide latency percentiles for observability.

**Non‑Goals (shall not)**
- The system shall not require changes to NIC drivers or kernel RSS implementation.
- The system shall not replace kernel networking primitives; it shall use supported Winsock/AF APIs and documented OS behavior.

---

## Variables (parameterized)

These variables are used through requirements and acceptance criteria.

- `N` / `SHARD_COUNT`: number of shard sockets/IOCPs; default = number of logical processors; range 1..`Nmax`.
- `B` / `BATCH_SIZE`: completion dequeue batch size; default = 64; range 1..512.
- `R` / `OUTSTANDING_RECEIVES`: per‑socket outstanding recv calls; default = 512; range 8..8192.
- `L` / `OFFERED_LOAD`: offered test load (in Mpps or Mbps, specified per test vector).
- `A` / `LATENCY_MEDIAN_US`: target median receive‑to‑callback latency in microseconds (µs).
- `B_LAT` / `LATENCY_P99_US`: target p99 receive‑to‑callback latency in microseconds (µs).
- `C` / `DROP_PERCENT`: acceptable drop percentage under load `L`; default = 0.1 (%).
- `E` / `SCALING_TOLERANCE`: acceptable deviation from linear scaling; default = 10 (%).
- `K` / `CONFIG_APPLY_MS`: max time to apply runtime configuration change; default = 2000 (ms).
- `M` / `STICKINESS_MINUTES`: duration for flow stickiness test; default = 10 (minutes).
- `S` / `REBALANCE_SECONDS`: max time to rebalance after shard removal; default = 30 (s).
- `U` / `THROUGHPUT_LOSS_PERCENT`: acceptable throughput loss during rebalance; default = 10 (%).
- `T` / `TELEMETRY_INTERVAL_MS`: telemetry emission interval; default = 1000 (ms).
- `Q` / `NIC_QUEUE_COUNT_MIN`: minimum expected MSI‑X / hardware queues from NIC; default = 4.
- `Nmax` / `SHARD_MAX`: maximum shard count supported in tests; default = 128.
- `SKU_LIST`: list or variable enumerating supported Windows SKUs/builds.

> Note: `B_LAT` is used to denote p99 latency to avoid ambiguity with `B` (batch size).

---

## Functional Requirements

Each Functional Requirement (FR‑###) is a statement of required behavior; all FRs are measurable or have associated observability requirements.

**FR‑001 (Socket Sharding)**
- The system shall create `SHARD_COUNT` (`N`) UDP sockets bound to the same local IP and port semantics such that each socket is addressable for receive operations by a distinct processing shard.  
- Default: `SHARD_COUNT` = number of logical processors. Range: 1..`Nmax`.

**FR‑002 (CPU Affinity)**
- Prior to binding a socket for receive sharding, the system shall assign a processor affinity mask to that socket’s receive processing context (via a supported OS facility). The mapping shall ensure distinct, non‑overlapping CPU masks per shard unless explicit configuration allows overlap.

**FR‑003 (IOCP Association & Batch Dequeue)**
- Each shard’s socket shall be associated with a distinct IOCP (or logically distinct completion queue) for that shard. Worker threads assigned to that shard shall dequeue completion events in batches using a batch size of `B` (`BATCH_SIZE`) and shall process each batch end‑to‑end before dequeuing the next batch to reduce enqueue contention.

**FR‑004 (RSS / NUMA Alignment & Flow Affinity)**
- The system shall align shard assignment to RSS CPU sets and NUMA nodes when available: a shard shall prefer CPUs and memory local to an RSS/NUMA domain.  
- The system shall ensure that packets for a given 4‑tuple (src IP, src port, dst IP, dst port) are processed by a single shard under steady state (flow stickiness), except during rebalance or failover events which must be observable.

**FR‑005 (Outstanding Receives & Back‑Pressure)**
- Each shard shall maintain at least `OUTSTANDING_RECEIVES` (`R`) pending receive operations to keep the NIC/stack supplied and shall dynamically adjust the per‑socket receive buffer sizes (`RecvBufferSize` / `SO_RCVBUF`) in response to measured drop rates and queue backlogs to avoid drops under load `L`.

**FR‑006 (Per‑Shard Observability of Loss & Queues)**
- The system shall record and emit per‑shard counters for: received packets (pps), dropped packets, receive enqueue failures, IOCP enqueue contention indicators, and per‑shard completion queue depth at `TELEMETRY_INTERVAL_MS` (`T`) intervals.

**FR‑007 (Shutdown and Drain)**
- The system shall support graceful shutdown: upon shutdown request the system shall stop accepting new application work, complete or cancel outstanding receives, and drain in‑flight completions such that all completions in transit are either delivered to the application callback or accounted as drops within `D` milliseconds (where `D` is specified per shutdown policy). The system shall emit a final per‑shard drain report.
# Minimal cts-traffic FRS — core requirements for a short-lived test app

Purpose: Provide the smallest set of functional requirements needed for cts-traffic to test sharded UDP receive behavior in short-duration test runs. This document intentionally omits long-term operational, scalability, and production hardening requirements.

Scope
- Short test runs only: quick verification and benchmarking (minutes to hours).
- Minimal observability to validate basic correctness and relative performance.
- Maintain compatibility with existing cts-traffic test flags; changes must be opt-in.

Core variables
- `SHARD_COUNT` (N): number of shards; default = logical CPU count; reasonable test range 1..32.
- `OUTSTANDING_RECEIVES` (R): outstanding receives per shard; default = 256.
- `BATCH_SIZE` (B): IOCP dequeue batch size; default = 64.
- `TELEMETRY_INTERVAL_MS` (T): telemetry interval; default = 1000.

Core functional requirements
- FR-1 (Enable Sharding): cts-traffic shall be able to enable or disable receive sharding at process start via config/flag (`EnableRecvSharding`). Default: disabled.
- FR-2 (Per-shard Sockets): When enabled, create `SHARD_COUNT` UDP sockets bound to the configured listen address/port such that each socket is serviced by a distinct shard.
- FR-3 (Per-shard IOCP): Each shard shall use a dedicated IOCP or logically separate completion handling so shard workers dequeue completions independently with batch size `B`.
- FR-4 (Outstanding Receives): Each shard shall maintain at least `OUTSTANDING_RECEIVES` outstanding recv calls; this ensures the NIC/stack remain supplied during short tests.
- FR-5 (Basic Observability): Emit minimal per-shard counters at interval `T`: received packets, dropped packets, and a single dequeue-latency sample (median or mean). Counters may be simple in-memory counters exported via existing cts-traffic logging or a single ETW event.
- FR-6 (Graceful Test Shutdown): On test shutdown, drain outstanding completions for a short configurable timeout (default 2s) and report basic per-shard counts.

Acceptance criteria (short tests)
- AC-1 (Correctness): With `EnableRecvSharding=true` and a single continuous flow test (single 4-tuple), all packets for that flow are observed by the same shard for the duration of the test (no cross-shard processing) unless shards are intentionally reconfigured during the test.
- AC-2 (Basic Throughput Signal): Enabling sharding should show a non-regressive change in aggregate receive rate for short tests (sharded run should not perform worse than single-socket baseline by more than an easily-observable margin; exact numbers left to test harness).
- AC-3 (Telemetry Presence): Per-shard counters (received/dropped) are emitted at interval `T` and are sufficient to compare shard behavior during the short test.

Implementation notes (developer-facing)
- Make changes localized: prefer additions to `ctsSocket`, `ctsSocketBroker`, and `ctThreadIocp` with minimal cross-cutting changes.
- Keep feature behind an opt-in flag (`EnableRecvSharding`) and keep defaults unchanged.
- Testing hooks: expose a simple mapping (ConnectionId -> shard id) in logs to validate flow stickiness in tests.

# cts-traffic — Stripped requirements (test-app scope)

Purpose: A very small, focused set of requirements for cts-traffic as a short-lived test application. Only essentials included to enable quick verification of sharded UDP receive behavior.

Essentials
- Enable/disable sharding via an opt-in config flag (`EnableRecvSharding`). Default: off.
- When enabled, create `SHARD_COUNT` receive sockets and per-shard completion handling.
- Maintain `OUTSTANDING_RECEIVES` per shard (default 256) so receives remain outstanding for short tests.
- Emit basic per-shard counters: received and dropped packets (simple logs or a single ETW event).
- On shutdown, drain outstanding completions for a short timeout (default 2s) and print per-shard counts.

Minimal variables (defaults for tests)
- `SHARD_COUNT`: default = logical CPU count, clamp to 1..32 for test runs.
- `OUTSTANDING_RECEIVES`: default = 256.
- `BATCH_SIZE`: default = 64.

Acceptance criteria (short tests)
- AC-1: With `EnableRecvSharding=true` and a single-flow test, packets for that flow are observed by a single shard for the duration of the test.
- AC-2: Per-shard counters (received/dropped) are emitted and sufficient to inspect per-shard behavior during short runs.

Developer notes
- Keep changes minimal and behind `EnableRecvSharding`. Touch `ctsSocket`, `ctsSocketBroker`, and `ctThreadIocp` only where required.
- Provide a lightweight test hook: an option to log ConnectionId → shard mapping to verify stickiness.

End of stripped requirements
