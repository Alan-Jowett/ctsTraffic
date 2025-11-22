# cts-traffic — Functional Requirements Specification

**Sharded UDP receive path to eliminate IOCP enqueue bottlenecks**

**Revision date:** 2025-11-22  
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

**FR‑008 (Runtime Configuration)**
- The system shall expose runtime configuration for: `SHARD_COUNT` (N), `BATCH_SIZE` (B), `OUTSTANDING_RECEIVES` (R), `RecvBufferSize`, CPU masks per shard, and NUMA node affinity. Configuration shall be dynamically changeable (subject to NFR constraints).

**FR‑009 (Telemetry & Metrics)**
- The system shall emit telemetry at interval `T` (`TELEMETRY_INTERVAL_MS`) including: per‑shard pps, latency percentiles (p50/p90/p99) for receive‑to‑callback, drops, backlog length, and state change events (shard added/removed/fallback). Telemetry shall include timestamps and shard identifiers.

**FR‑010 (Compatibility & Graceful Degradation)**
- The system shall be deployable on Windows SKUs specified in `SKU_LIST`. If a requested OS capability (e.g., SIO_CPU_AFFINITY) is unavailable, the system shall detect this at startup and shall degrade gracefully by either reducing `SHARD_COUNT`, falling back to single‑socket operation or enabling an alternative supported mode while emitting an explicit capability‑loss event.

**FR‑011 (Security & Buffer Safety)**
- The system shall validate source addresses per configured policies (ACLs, allow/deny lists) at receive time and shall ensure buffers are isolated to shards to prevent cross‑shard leakage; any cross‑shard buffer reuse shall be controlled by a safe transfer protocol with explicit ownership transfer and audit logging.

**FR‑012 (Flow Rebalance)**
- When shard topology changes (shard addition, removal, or CPU mask change), the system shall rebalance new incoming flow assignments without violating FR‑004 for steady flows, and shall provide observable rebalancing events including estimated rebalance completion time.

**FR‑013 (Error Reporting)**
- The system shall surface persistent or transient IOCP enqueue errors, socket binding conflicts, and resource exhaustion events to telemetry and logs with error codes and suggested remediation instructions.

---

## Non‑Functional Requirements

**NFR‑001 (Throughput)**
- The system shall achieve aggregate throughput ≥ `X` Mpps (configurable per test `L`) with ≤ `Y%` CPU overhead per shard measured relative to an idle baseline.

**NFR‑002 (Latency)**
- Median receive‑to‑callback latency shall be ≤ `A` (`LATENCY_MEDIAN_US`). p99 receive‑to‑callback latency shall be ≤ `B_LAT` (`LATENCY_P99_US`) under the steady offered load `L`.

**NFR‑003 (Reliability)**
- The system shall run a continuous soak test of 24 hours with no deadlocks and shall maintain packet drop rate ≤ `C%` at offered load `L`.

**NFR‑004 (Scalability)**
- Measured throughput shall scale within ±`E%` of linear scaling as `SHARD_COUNT` increases from 1 to `SHARD_MAX` (`Nmax`) for representative hardware up to `Nmax`.

**NFR‑005 (Operability)**
- Runtime configuration changes (valid changes to `SHARD_COUNT`, `BATCH_SIZE`, `OUTSTANDING_RECEIVES`, CPU/NUMA masks) shall be applied and effective within `K` milliseconds without a process restart, except for changes requiring resource reallocation beyond available capacity (which shall be rejected with a clear error).

**NFR‑006 (Diagnostics & Instrumentation)**
- The system shall expose low‑overhead diagnostics (ETW/provider, counters) with fixed provider name and event IDs (placeholders in this specification), and the telemetry shall be sufficient to reconstruct per‑shard packet counts, drop counts, and completion latency percentiles.

---

## Interfaces & Configuration

**Runtime parameters (names, defaults, ranges)**
- `SHARD_COUNT` (N): integer, default = logical CPU count, range 1..`Nmax`.
- `BATCH_SIZE` (B): integer, default = 64, range 1..512.
- `OUTSTANDING_RECEIVES` (R): integer, default = 512, range 8..8192.
- `RECV_BUFFER_SIZE`: bytes, default = 4 MiB per socket, range 64 KiB..64 MiB.
- `CPU_MASKS[]`: list of CPU affinity masks per shard; default = mapped automatically to logical CPUs.
- `NUMA_NODE[]`: list of NUMA node IDs per shard; default = auto (closest node).
- `SHARD_REBALANCE_POLICY`: enum {Immediate, Gradual} default = Gradual.
- `TELEMETRY_INTERVAL_MS` (T): integer, default = 1000.

**Control plane**
- Configuration sources: JSON/YAML configuration file, environment variables, and command-line overrides; precedence shall be CLI > env > file.
- The system shall validate configuration at load and before applying runtime changes and shall emit validation errors.

**Telemetry / ETW**
- The system shall publish an ETW provider with a stable provider name placeholder (e.g., `cts-traffic/RecvShard`), unique event IDs for state transitions and errors, and counters for pps, latency p50/p90/p99, drops, backlog length. Concrete IDs shall be documented in an operational manifest shipped with the build.

---

## Data / Control Flows (textual diagrams)

**Packet path (text)**
- NIC → hardware RSS queue → kernel receive path → socket bound to shard → IOCP associated with shard → worker thread (dequeues up to `B` completions) → application receive callback → application processing.

**Back‑pressure control (text)**
- If per‑shard backlog (completion queue depth or measured enqueue latency) exceeds configured thresholds, the shard controller shall:
  - increase `RECV_BUFFER_SIZE` (subject to system limits), and/or
  - increase `OUTSTANDING_RECEIVES` (`R`) up to configured max, and/or
  - emit a warning telemetry event; actions and thresholds are configurable.

**Rebalance flow (text)**
- Rebalance trigger (operator request or topology change) → controller acquires shard reconfiguration lock → new CPU/NUMA mapping validated → routing updates applied for new incoming flows → telemetry event sequence emitted → finalization and release.

---

## Failure Modes & Recovery

**Unavailable SIO_CPU_AFFINITY or equivalent OS capability**
- Detection: startup capability probe shall detect unavailability and emit capability‑loss event.  
- Recovery/fallback: the system shall fall back to a reduced‑shard or single‑socket mode and shall optionally enable an alternative supported optimized path (e.g., RIO or larger `OUTSTANDING_RECEIVES`). The fallback mode shall be observable and logged.

**IOCP saturation / enqueue contention**
- Detection: sustained high completion queue depths, increased dequeue latency, or IOCP enqueue failures.  
- Recovery: the controller shall adjust `BATCH_SIZE` (increase `B`), increase per‑socket `OUTSTANDING_RECEIVES`, or increase `RECV_BUFFER_SIZE`. The controller shall emit an alert event and suggest human remediation if automatic adjustments do not restore acceptable metrics.

**NUMA misalignment**
- Detection: high remote memory access counters, elevated latency for certain shards.  
- Recovery: the system shall warn and propose a corrected CPU/NUMA mask; if automatic remediation is enabled, the system shall attempt to rebind shards to local NUMA nodes and report the outcome.

**Shard failure (e.g., worker crash)**
- Detection: shard heartbeat missing, repeated error events.  
- Recovery: the system shall mark shard as offline, reassign incoming flows per policy, and if configured, spawn replacement shard worker; telemetry shall include time to recovery.

---

## Acceptance Criteria (AC‑###)

**AC‑001 (Throughput & Latency)**
- Given `SHARD_COUNT` = number of logical processors and offered load `L` (test vector), the system shall sustain the load with p99 latency ≤ `B_LAT` µs and drop rate ≤ `C%` during a sustained run of duration `P` minutes (test duration specified by test plan).

**AC‑002 (Flow Stickiness)**
- A flow stickiness test that sends packets for a single 4‑tuple continuously for `M` minutes shall show that all packets for that 4‑tuple are processed by exactly one shard (no inter‑shard processing) except during controlled rebalance windows; telemetry shall show a single shard identifier for that 4‑tuple across `M`.

**AC‑003 (Shard Removal Rebalance)**
- When one shard is deliberately disabled during a live test, the system shall rebalance remaining traffic such that throughput loss ≤ `U%` and the rebalance completes within `S` seconds.

**AC‑004 (Telemetry Fidelity)**
- The telemetry stream shall export per‑shard counters at interval `T` ms and the sum of per‑shard received packets shall equal the NIC total (within measurement tolerance), while reported drops shall be consistent with packet loss observed at test traffic sources.

**AC‑005 (Graceful Shutdown)**
- On graceful shutdown, the system shall drain or account for in‑flight completions and emit a drain report within `D` ms indicating counts of processed, dropped, and cancelled receives.

**AC‑006 (Compatibility Fallback)**
- When SIO_CPU_AFFINITY is unavailable on a tested SKU in `SKU_LIST`, the system shall report capability loss and operate in a documented fallback mode; acceptance requires that fallback mode be functional and telemetry identifies the degraded mode.

---

## Constraints & Assumptions

- The design assumes Windows UDP semantics (socket semantics, overlapped I/O, IOCP semantics).
- Where SIO_CPU_AFFINITY (or equivalent) is referenced, it shall be invoked before socket bind operations for correctness.
- Sharding the same port across sockets may be incompatible with `SO_REUSEADDR` or `SO_REUSE_MULTICASTPORT` on some SKUs; the system shall validate platform behavior and fail with clear diagnostics if unsupported.
- NICs are expected to have RSS enabled and provide ≥ `Q` MSI‑X/MSI queues for effective hardware queue distribution; if not, acceptance tests shall reflect hardware limits.
- The process shall run with sufficient privileges to set required socket options and CPU affinity masks.

---

## Glossary

- **IOCP**: I/O Completion Port (Windows)
- **RSS**: Receive Side Scaling (hardware hashing to queues)
- **NUMA**: Non‑Uniform Memory Access
- **shard**: a logical receive processing unit consisting of a socket, IOCP, and worker(s)
- **backlog**: queued but unprocessed receive completions
- **pps**: packets per second
- **ETW**: Event Tracing for Windows
- **AFD**: Ancillary Function Driver/Windows socket stack component (contextual)
- **4‑tuple**: (src IP, src port, dst IP, dst port)
- **RIO**: Registered I/O (reference only as a possible fallback mode)

---

## Open Questions

- `SKU_LIST`: which Windows server/client builds and KBs shall be explicitly supported and tested?
- NIC models: what NIC vendors/models and RSS queue counts shall be validated?
- Maximum shard count (`Nmax`) for realistic hardware targets — what is the intended production `Nmax`?
- Deployment footprint: memory and per‑shard resource budgets for production targets.
- Buffer sizing policy: algorithmic policy for `RECV_BUFFER_SIZE` growth/shrink during run.

---

## References (neutral placeholders)

- Winsock IOCP APIs: `CreateIoCompletionPort`, `GetQueuedCompletionStatusEx`
- Winsock control IO: `WSAIoctl(SIO_CPU_AFFINITY)` (platform capability)
- RSS documentation and NIC vendor guides
- ETW provider and performance counter conventions

(Concrete references and provider/event IDs shall be provided in an operational manifest; this document contains neutral placeholders only.)

---

## Style & Formatting Notes (conformance)

- All functional requirements use assertive “shall” language and are numbered.
- Tunables are UPPER_SNAKE_CASE with defaults and ranges where applicable.
- The document avoids implementation pseudo‑code and focuses on behavior, observability, and measurable acceptance criteria.

---

*End of specification.*
