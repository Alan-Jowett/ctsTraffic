# Outstanding Work and Design Decisions — Sharded UDP Receive

Purpose: a living checklist of remaining implementation tasks and unresolved design choices for `EnableRecvSharding`. Remove or mark items as resolved in this file as work and decisions are completed.

---

## Outstanding Work (actionable)

- [ ] Batched dequeue support in worker threads
  - Spec: prefer `GetQueuedCompletionStatusEx` with `BatchSize` but fallback to `GetQueuedCompletionStatus` (FR‑003).
  - Files: `ctl/ctRawIocpShard.cpp`
  - Priority: Medium
  - Suggested approach: implement `GetQueuedCompletionStatusEx` based loop with `ctsConfig::BatchSize`, test fallback path on older SKUs.

- [ ] Telemetry & per-shard counters
  - Spec: emit per-shard counters at `TelemetryIntervalMs` (FR‑006 / AC‑3).
  - Files: `ctsStatistics.hpp/cpp`, `ctl/ctRawIocpShard.cpp`, telemetry emit code
  - Priority: High
  - Suggested approach: add minimal per-shard counters (packetsReceived, packetsDropped, recvEnqueueFailures, dequeueDepth sample, dequeueLatency sample) with a simple periodic emitter (reuse existing cts telemetry or ETW events).

- [ ] Flow stickiness test hook and acceptance tests
  - Spec: provide mapping (4‑tuple -> shard id) and acceptance test for stickiness (AC‑1).
  - Files: test harness under `MSTest/*`, `ctsSocketBroker.cpp` (lookup helper)
  - Priority: High
  - Suggested approach: expose a function in `ctsSocketBroker` to resolve 4‑tuple -> shard id for test verification; add unit/integration test(s) that send flow traffic and verify stickiness.

- [ ] Dynamic `SO_RCVBUF` tuning & drop accounting
  - Spec: adjust receive buffer sizes in response to drops/backpressure (FR‑005).
  - Files: `ctl/ctRawIocpShard.cpp`, `ctsConfig.*`, telemetry
  - Priority: Medium
  - Suggested approach: instrument drop/recv failure counters, and implement a conservative tuning policy (increase `SO_RCVBUF` on sustained drops, with upper bound).

- [ ] Final drain reporting and shutdown deadline enforcement
  - Spec: drain outstanding IO and report per-shard counts within `D` ms (FR‑007).
  - Files: `ctl/ctRawIocpShard.cpp`, `ctsSocketBroker.cpp`
  - Priority: Medium
  - Suggested approach: in `Shutdown()` enforce configurable timeout (use `WaitForSingleObject` or timed joins), collect and emit final per-shard drain/summary event.

- [ ] Add unit tests for shard lifecycle, re-post failure, and edge cases
  - Files: `MSTest/ctsRawIocpShardUnitTest/*`
  - Priority: High
  - Suggested approach: extend existing unit test project to cover: Initialize with bound socket, multiple outstanding receives, re-post failure path, Shutdown drain behavior.

- [ ] Integrate new sources into build system and CI
  - Files: `ctsTraffic.vcxproj`, solution `ctsTraffic.sln`, CI workflows
  - Priority: High
  - Suggested approach: ensure new files are included in relevant projects and CI runs the new unit tests.

---

## Outstanding Design Decisions (require explicit resolution)

- Decision: Socket ownership model (Broker-owned vs Adopted)
  - Options:
    - A: Broker proactively creates N sockets and shards own them (recommended). Deterministic binding, simpler stickiness guarantees.
    - B: Broker adopts sockets created by existing `ctsSocket` lifecycle. Less invasive but may not guarantee per-shard semantics.
  - Status: undecided

- Decision: Shard selection & mapping policy
  - Options:
    - Per-CPU round-robin assignment for sockets
    - Deterministic 4‑tuple -> shard mapping (software-side) for stickiness
    - Rely on kernel binding and RSS for stickiness
  - Status: undecided (recommend deterministic mapping or broker-owned sockets plus RSS alignment)

- Decision: Affinity application method
  - Options:
    - Use `WSAIoctl(SIO_CPU_AFFINITY)` per socket (if supported)
    - Set thread affinity for worker threads via `SetThreadAffinityMask`/`SetThreadGroupAffinity`
    - Both (preferred): apply kernel affinity and pin worker threads for best control
  - Status: undecided (recommend both where supported)

- Decision: Batch API choice
  - Options:
    - Use `GetQueuedCompletionStatusEx` (preferred) with `BatchSize` and fallback path
    - Use single `GetQueuedCompletionStatus` loop (simpler, less efficient)
  - Status: undecided (implement `GetQueuedCompletionStatusEx` + fallback)

- Decision: Telemetry interface and minimum counters
  - Options:
    - Integrate into existing `ctsStatistics` and ETW pipeline (preferred for consistency)
    - Emit simple log/CSV output for initial validation, postpone full ETW integration
  - Status: undecided (recommend ETW/ctsStatistics integration if quick, otherwise minimal interim logging)

- Decision: Shutdown timeout semantics
  - Options:
    - Hard deadline `D` ms, force-close after deadline and report drops
    - Best-effort drain with increasing backoff (no hard deadline)
  - Status: undecided (spec requires a deadline; recommend configurable hard deadline with clear reporting)

---

## How to progress

- To mark an item resolved, edit this document and either check the box or remove the entry. For design decisions, add a short note with the chosen option and the commit that implemented it.
- When implementing code, add a reference in the commit message to the doc path `documents/OutstandingWorkAndDecisions.md` and the specific bullet resolved.

---

File created by automation on behalf of the `udp_scale` branch. Update and iterate as design/code evolves.
