# cts-traffic — Sharded UDP Receive Development Plan

This document describes the design and concrete development tasks to implement an opt-in sharded UDP receive path in `cts-traffic`. It converts the requirements in `documents/ShardedUdpReceiveFRS.md` into a developer-focused implementation plan, lists affected files, low-level API steps, testing, telemetry, and rollout strategy.

## Summary

- Goal: Add an opt-in receive-sharding mode (`EnableRecvSharding`) that creates one UDP socket + one IOCP per shard, applies per-socket CPU affinity (where supported), uses `SO_REUSEADDR` (or platform-appropriate fallback), and runs dedicated worker threads per shard using raw IOCP APIs (not the NT thread pool).
- Default behavior remains unchanged: sharding is disabled by default.

## High-level approach

- Introduce a new raw IOCP-based shard manager that creates `SHARD_COUNT` sockets and `SHARD_COUNT` IOCPs.
- For each shard:
  - Create UDP socket with `WSA_FLAG_OVERLAPPED`.
  - Call `setsockopt(..., SO_REUSEADDR, ...)` and optionally `SO_REUSE_UNICASTPORT` when available.
  - Attempt kernel-side CPU affinity via `WSAIoctl(..., SIO_CPU_AFFINITY, ...)` (feature-detect at runtime).
  - Associate the socket with a shard-specific IOCP using `CreateIoCompletionPort`.
  - Start one or more dedicated worker threads pinned to shard CPU(s) using `SetThreadAffinityMask` / `SetThreadGroupAffinity`.
  - Worker threads batch-dequeue completions using `GetQueuedCompletionStatusEx` (batch size = `BATCH_SIZE`) and process each batch end-to-end.
  - Maintain `OUTSTANDING_RECEIVES` overlapped `WSARecvFrom` calls per socket; re-post on completion.

## Concrete file-level changes

- New files to add:
  - `ctl/ctRawIocpShard.hpp` and `ctl/ctRawIocpShard.cpp` — shard RAII class: socket create, IOCP create, worker thread lifecycle, outstanding receives, and telemetry hooks.
  - `ctl/ctCpuAffinity.hpp` (optional) — helper to map shard -> CPU mask/group and to detect `SIO_CPU_AFFINITY` support.

- Existing files to update:
  - `ctsConfig.h` / `ctsConfig.cpp` — add config flags:
    - `EnableRecvSharding` (bool, default: false)
    - `ShardCount` (int, default: logical processor count)
    - `OutstandingReceives` (int)
    - `BatchSize` (int)
    - `ShardWorkerCount` (int)
    - `ShardAffinityPolicy` (enum)  
      - NOTE: this maps to `ctsConfig::ctsConfigSettings::ShardAffinityPolicy` in code (was previously referred to as `AffinityPolicy` in some docs).
  - `ctsSocket.cpp` / `ctsSocket.h` (or `ctsSocketBroker.*`) — add hooks to instantiate the shard manager and delegate receives to shards when enabled.
  - `ctsSocketBroker.cpp` / `ctsSocketBroker.h` — manage shard instances and provide mapping helpers to route application-level work or look up shard per 4‑tuple for test hooks.
  - `ctsStatistics.hpp` / telemetry code — add per-shard counters and periodic emit.
  - `ctsTraffic.cpp` — parse new flags and create shard manager at startup when enabled.
  - `ctsTraffic.vcxproj` — include new source files.
  - Tests & scripts: `MSTest/*` and `TestScripts/*` — add unit & acceptance tests and scripts for sharded runs.

## Low-level API and algorithm details

- Socket creation per shard:
  - `WSASocketW(AF_INET/AF_INET6, SOCK_DGRAM, IPPROTO_UDP, nullptr, 0, WSA_FLAG_OVERLAPPED)`
  - `setsockopt(socket, SOL_SOCKET, SO_REUSEADDR, ...)` (check return, log failures)
  - Optionally attempt `SO_REUSE_UNICASTPORT` if available on target SKU
  - `bind()` to the configured listen address/port
  - If supported, call `WSAIoctl(socket, SIO_CPU_AFFINITY, &affinity, sizeof(affinity), NULL, 0, &bytes, NULL, NULL)` with per-shard affinity

- IOCP and worker threads:
  - Create per-shard IOCP: `CreateIoCompletionPort(INVALID_HANDLE_VALUE, NULL, 0, 0)`
  - Associate socket: `CreateIoCompletionPort((HANDLE)socket, shardIocp, (ULONG_PTR)shardId, 0)`
  - Worker thread loop:
    - Pre-allocate `OVERLAPPED_ENTRY entries[BATCH_SIZE]`
    - `GetQueuedCompletionStatusEx(shardIocp, entries, BATCH_SIZE, &numRemoved, INFINITE, FALSE)`
    - Process each `entries[i]` fully before next dequeue
  - Graceful shutdown: `CancelIoEx`, wait for drain within configured timeout, close IOCP and sockets

- Outstanding receives:
  - Issue `OUTSTANDING_RECEIVES` overlapped `WSARecvFrom` calls per socket
  - On completion, process packet and immediately re-post a new `WSARecvFrom`
  - Keep overlapped and buffer objects shard-local to avoid cross-shard buffer leaks

- Thread pinning:
  - Use `SetThreadAffinityMask` (or `SetThreadGroupAffinity`) to pin worker threads to desired CPU(s)
  - `ShardAffinityPolicy` controls mapping: `PerCpu`, `PerGroup`, `RssAligned`, `Manual`

## Telemetry & observability

- Emit per-shard counters at `TelemetryIntervalMs`:
  - `Shard{Id}.PacketsReceived` (pps)
  - `Shard{Id}.PacketsDropped`
  - `Shard{Id}.RecvEnqueueFailures`
  - `Shard{Id}.IOCPEQueueDepth` (sampled)
  - `Shard{Id}.DequeueLatency` (sample or percentile)
- Emit events on:
  - Shard creation (id, affinity, listen addr/port)
  - SIO_CPU_AFFINITY success/failure
  - Fallbacks and capability detection results
- Provide a test hook/API to resolve a 4‑tuple -> `ShardId` mapping for stickiness verification

## Edge cases, fallbacks & compatibility

- Feature detection: on startup detect support for `SIO_CPU_AFFINITY`, `GetQueuedCompletionStatusEx`, and `SO_REUSE_UNICASTPORT`.
- Fallback behaviors:
  - If multiple binds are unsupported, log and fall back to single-socket mode.
  - If affinity ioctl fails, continue with sharding (if binds succeeded) but log that affinity is not applied.
  - For OSes lacking `GetQueuedCompletionStatusEx`, use single-dequeue loop with `GetQueuedCompletionStatus`.
- Keep sharding opt-in and off by default for regression safety.

## Testing plan

- Unit tests:
  - `ctl` unit tests for `ctRawIocpShard` lifecycle: create/destroy, post/complete receives.
  - Mock or integration tests for `WSAIoctl(SIO_CPU_AFFINITY)` path.
- Integration tests / acceptance:
  - AC-1 stickiness: single 4‑tuple flow -> verify all packets map to same `ShardId`.
  - Throughput baseline vs sharded: measure aggregate pps and ensure non-regressive behavior.
  - Telemetry emission: validate per-shard counters at `T` intervals.
- Performance tests:
  - Scaling tests with `SHARD_COUNT = 1,2,CPUcount,Nmax` and verify `SCALING_TOLERANCE`.
  - Rebalance/failover tests: remove/add shards and measure rebalance time and throughput loss.

## Implementation order (incremental)

1. Add new config flags in `ctsConfig` and parse CLI flags.
2. Add capability-detection helper (`ctl/ctCpuAffinity.hpp`).
3. Implement `ctl/ctRawIocpShard` with minimal feature set: socket creation, IOCP association, worker thread loop, and re-post receives.
4. Wire shard manager into `ctsSocketBroker` / `ctsSocket` behind `EnableRecvSharding`.
5. Add per-shard telemetry and periodic emit.
6. Add unit tests and a simple acceptance test for stickiness.
7. Add scripts & perf harness for throughput and scaling tests.
8. Iterate on tuning `OUTSTANDING_RECEIVES`, `BATCH_SIZE`, and worker counts.

## Acceptance criteria mapping

- FR-001 .. FR-007: Implement per-shard sockets, CPU affinity where supported, IOCP association, outstanding receives, per-shard telemetry, and graceful shutdown.
- AC-1 (Correctness): Provide test hook and acceptance test proving 4‑tuple stickiness.
- AC-2 (Basic Throughput Signal): Provide scripts and measurement comparing single-socket baseline and sharded runs.
- AC-3 (Telemetry Presence): Per-shard counters are emitted at configured interval `T`.

## Estimated effort

- Prototype (core `ctRawIocpShard` + config + simple wiring + one test): 2–4 days.
- Full integration (tests, telemetry, fallbacks, perf tuning): 1–2 weeks depending on testing infrastructure and validation across Windows SKUs.

## Next steps

- If you approve, I can scaffold the new `ctl/ctRawIocpShard.hpp`/`.cpp` skeleton and add config flags in `ctsConfig` so you can review the initial implementation quickly.

---

File: `documents/ShardedUdpReceiveDevPlan.md`
