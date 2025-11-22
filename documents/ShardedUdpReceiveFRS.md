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

# cts-traffic — Functional Requirements Specification

Sharded UDP receive for cts-traffic (IOCP enqueue contention mitigation)

**Revision date:** 2025-11-22  
**Scope:** cts-traffic codebase — receive-path sharding for high-volume UDP ingest

---

## Overview & Scope

Purpose: The cts-traffic system shall eliminate IOCP enqueue bottlenecks observed when a single UDP socket + IOCP is used for high-rate receive traffic by sharding UDP receive processing across multiple sockets and IOCPs aligned with cts-traffic runtime abstractions (e.g., `ctsSocket`, `ctsSocketState`, `ctThreadIocp`) and CPU/RSS/NUMA topology. This document scopes functional requirements to the cts-traffic design referenced in the project's `documents/DesignSpec.md`.

In scope
- Changes required in cts-traffic to support sharded receive: socket lifecycle (`ctsSocket`), state manager (`ctsSocketState`), broker semantics (`ctsSocketBroker`), configuration (`ctsConfig`), and IO completion integration (`ctThreadIocp` / IOCP wrappers).
- Telemetry additions in the cts-traffic telemetry surface (ETW/counters) to report per-shard metrics.
- Runtime configuration controls to enable and tune sharding behavior.

Out of scope
- NIC driver or kernel RSS implementation changes.
- Major redesign of send path unless required for correctness during receive sharding.

---

## Goals & Non‑Goals

Goals (shall)
- Integrate sharding within cts-traffic so receive throughput scales with SHARD_COUNT up to Nmax on target hardware.
- Preserve existing cts-traffic observable semantics (per-connection ConnectionId and per-connection stats) while ensuring per-flow affinity across shards.
- Provide telemetry, counters, and events compatible with cts-traffic logging/ETW conventions.

Non-Goals (shall not)
- The change shall not require kernel or NIC driver modifications.
- The change shall not alter existing public command-line flags or behavior unless explicitly gated by new configuration options in `ctsConfig`.

---

## Variables (parameterized)

- `N` / `SHARD_COUNT`: number of shards (sockets + IOCPs) — default: number of logical processors; range 1..`Nmax`.
- `B` / `BATCH_SIZE`: completion dequeue batch size — default: 64; range: 1..512.
- `R` / `OUTSTANDING_RECEIVES`: outstanding receives per shard — default: 512; range: 8..8192.
- `L` / `OFFERED_LOAD`: test load vector (Mpps/Mbps).
- `A` / `LATENCY_MEDIAN_US`, `B_LAT` / `LATENCY_P99_US` — latency targets.
- `C` / `DROP_PERCENT` — acceptable drop percent under load `L`.
- `T` / `TELEMETRY_INTERVAL_MS` — default 1000 ms.
- Other variables as previously defined in design doc.

---

## Functional Requirements (cts-traffic scoped)

FR-001 (CTS Socket Sharding)
- `ctsSocket` (or a new receive-side socket abstraction) shall support instantiation of `SHARD_COUNT` UDP sockets that are logically tied to a single logical listener (same bind address/port semantics) and expose a shard identifier to the upper layer `ctsSocketState`/broker.
- The mapping from logical connection (ConnectionId) to shard shall be deterministic and observable.

FR-002 (Per-Shard IOCP / `ctThreadIocp` Association)
- Each receive socket shall be associated with its own `ctThreadIocp` instance or an IOCP instance logically dedicated to the shard. The `ctThreadIocp` wrapper shall expose batched dequeuing (GetQueuedCompletionStatusEx semantics) with batch size `B`.

FR-003 (Integration with `ctsSocketState` and Broker)
- `ctsSocketState` shall be able to select and bind its receive context to a specific shard so that per-connection state and reporting remain consistent. `ctsSocketBroker` shall manage shard lifecycle (create/destroy) when scaling SHARD_COUNT up or down.

FR-004 (Affinity & NUMA alignment)
- `ctsConfig` shall expose CPU/NUMA affinity configuration for shards. The cts-traffic runtime shall prefer binding shard workers and `ctThreadIocp` to CPUs and NUMA nodes compatible with RSS mapping for target NICs.

FR-005 (Outstanding Receives & Backpressure within cts-traffic)
- cts-traffic shall maintain at least `OUTSTANDING_RECEIVES` per shard and shall implement a control loop that monitors per-shard backlog and drop metrics and adjusts `RecvBufferSize` and `OUTSTANDING_RECEIVES` at runtime.

FR-006 (Observability & Telemetry)
- cts-traffic shall add per-shard telemetry (ETW/counters) consistent with existing telemetry schema: per-shard pps, per-shard drop counts, per-shard backlog length, per-shard dequeue latency histogram, and events for shard add/remove/fallback.

FR-007 (Graceful Shutdown & Drain)
- cts-traffic shall coordinate shutdown so `ctsSocketBroker` initiates per-shard drain, `ctsSocket` cancels or waits on outstanding receives, and `ctThreadIocp` drains completions within `D` ms before process exit.

FR-008 (Configuration & Command Line)
- `ctsConfig` shall accept new flags / config keys to enable sharding and set `SHARD_COUNT`, `BATCH_SIZE`, `OUTSTANDING_RECEIVES`, per-shard CPU masks, and `TELEMETRY_INTERVAL_MS`. CLI precedence and runtime change semantics shall match existing `ctsConfig` behavior.

FR-009 (Compatibility & Fallback)
- On SKUs where per-socket CPU affinity cannot be established, cts-traffic shall detect capability absence at startup and publish a capability-loss event; behavior shall fall back to single-socket receive with enhanced `OUTSTANDING_RECEIVES` tuning.

FR-010 (Security & Buffer Safety)
- cts-traffic shall ensure buffer ownership semantics remain safe when shard-local buffers are used; `ctsSocket` shall not expose raw shard-local buffers to other shards without ownership handoff.

FR-011 (Testing Hooks)
- cts-traffic shall provide test hooks or telemetry markers to correlate ConnectionId → shard id mappings to support AC‑002 (flow stickiness) verification.

---

## Non‑Functional Requirements (cts-traffic scoped)

NFR-001 (Throughput)
- On supported hardware and with `SHARD_COUNT` set to logical CPU count, cts-traffic shall show aggregated UDP receive throughput improvement vs baseline single-socket runs; test targets (X Mpps) shall be validated per hardware profile.

NFR-002 (Latency)
- Median receive-to-callback latency p50 ≤ `A` and p99 ≤ `B_LAT` under load `L` (as measured by existing cts-traffic measurement harness).

NFR-003 (Reliability & Soak)
- The modified cts-traffic executable shall run 24-hour soak tests with no deadlocks and drop rate ≤ `C%` at offered load `L`.

NFR-004 (Operability)
- Runtime changes to `SHARD_COUNT`, `BATCH_SIZE`, and other tunables shall be applied using existing `ctsConfig` mechanisms and shall be effective within `K` ms where feasible.

---

## Interfaces & Configuration (cts-traffic)

- Add configuration keys to `ctsConfig::ctsConfigSettings` and parsing in `ctsConfig.cpp`: `EnableRecvSharding` (bool), `ShardCount`, `ShardBatchSize`, `ShardOutstandingReceives`, `ShardCpuMasks[]`, `ShardNumaNodes[]`, `ShardRebalancePolicy`.
- `ctsTraffic` startup sequence shall validate capabilities and log chosen default shard topology.
- Telemetry provider shall extend existing cts-traffic provider and add per-shard event types and counters.

---

## Data / Control Flows (textual)

Packet path (cts-traffic): NIC → kernel RSS → AFD → `ctsSocket` (shard X) → `ctThreadIocp` (shard X) → shard worker → `ctsSocketState` callback → pattern handling.

Backpressure control (cts-traffic): per-shard monitor → adjust `ctsSocket` recv buffer sizes via `ctsWinsockLayer` helpers and tune `OUTSTANDING_RECEIVES` through configuration update path in `ctsConfig`.

Rebalance (cts-traffic): `ctsSocketBroker` coordinates rebind of new sockets, updates `ctsSocketState` mappings, and emits events for telemetry.

---

## Failure Modes & Recovery (cts-traffic)

- If per-shard `ctThreadIocp` cannot be created, log and fall back to single-socket receive and increase `OUTSTANDING_RECEIVES`.
- If IOCP enqueue failures increase, `ctsSocketBroker` shall increase batch size `B` and `OUTSTANDING_RECEIVES`; emit alerts.
- NUMA misalignment detection via local metrics; propose mask correction via logs and telemetry.

---

## Acceptance Criteria (cts-traffic)

AC-001 (Throughput/Latency)
- With `SHARD_COUNT` = logical CPUs and offered load `L`, modified cts-traffic shall meet `B_LAT` p99 latency and drop ≤ `C%` over `P` minutes.

AC-002 (Flow Stickiness)
- The mapping ConnectionId → shard id shall be stable for `M` minutes of continuous traffic and verifiable via telemetry markers.

AC-003 (Shard Removal)
- Disabling a shard at runtime shall complete rebalance within `S` seconds with throughput loss ≤ `U%`.

AC-004 (Telemetry)
- Per-shard counters exported at `T` intervals and sum validation holds within measurement tolerance.

---

## Constraints & Assumptions (cts-traffic)

- Changes shall be made primarily in `ctsSocket`, `ctsSocketBroker`, `ctsSocketState`, `ctsConfig`, and `ctThreadIocp`.
- Keep backward compatibility: default behavior shall remain single-socket unless `EnableRecvSharding` is enabled.
- Tests and harnesses may require updates to correlate ConnectionId ↔ shard id; provide test hooks.

---

## Glossary & References

See project `documents/DesignSpec.md` for existing design conventions. Reference Winsock IOCP and WSAIoctl docs for platform calls.

---

*End of cts-traffic scoped specification.*
