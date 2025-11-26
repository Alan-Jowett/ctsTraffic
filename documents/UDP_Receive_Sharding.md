# UDP Receive Sharding — Design & Implementation

Status: Draft

Overview
--------
This document describes the design and implementation of the IOCP-backed UDP receive sharding implemented in `ctl::ctThreadIocp_shard`.

Goals
-----
- Efficiently receive UDP packets using overlapped I/O and an IO Completion Port (IOCP).
- Scale receive processing across multiple worker threads.
- Affinitize worker threads to CPU groups and masks to improve cache locality and load distribution on systems with processor groups.
- Provide a safe API for request allocation/cancellation and avoid resource leaks on construction failure.

Key Components
--------------
- `ctl::ctThreadIocp_shard` (header: `ctl/ctThreadIocp_shard.hpp`)
  - Creates an IO Completion Port and associates a user-provided HANDLE or SOCKET with it.
  - Spawns a configurable number of `std::thread` workers.
  - Exposes `new_request(std::function<void(OVERLAPPED*)>)` and `cancel_request(const OVERLAPPED*)` for callers to allocate and cancel overlapped-backed callback objects.
  - Exposes `post_completion(...)` for posting custom completion packets (useful for tests).

- `ctThreadIocpCallbackInfo`
  - Small wrapper that contains an `OVERLAPPED` plus a `std::function<void(OVERLAPPED*)>` callback.
  - `new_request` returns the embedded `OVERLAPPED*` and caller is responsible for passing that to Winsock APIs that accept an `OVERLAPPED*`.

Design Details
--------------
1. IOCP Creation and Initialization
   - An IOCP is created using `CreateIoCompletionPort(INVALID_HANDLE_VALUE, ...)` into a local `wil::unique_handle`.
   - The user-provided handle (typically a UDP `SOCKET`) is associated with the IOCP via `CreateIoCompletionPort(_handle, iocp.get(), 0, 0)`.
   - Worker threads are created after association succeeds.
   - Only after successful association and thread creation is the local `iocp` moved into the member `m_iocp`. This avoids leaking the handle on constructor failure.

2. Worker Threads and Affinity
   - Worker threads run `WorkerLoop(size_t index)`.
   - If a list of `GroupAffinity` entries was provided by the caller, each worker computes which affinity to use by `index % m_groupAffinities.size()` and calls `SetThreadGroupAffinity` with the chosen `GROUP_AFFINITY`.
   - This ensures threads can be pinned to processor groups and masks, which is important on systems with more than 64 logical processors.

3. Request Lifecycle
   - `new_request` allocates a `ctThreadIocpCallbackInfo` with the provided callback and zeroes its `OVERLAPPED` before returning the pointer.
   - The caller passes that `OVERLAPPED*` to Winsock (e.g., `WSARecvFrom`) as the overlapped structure.
   - When IO completes, the worker loop receives the completion via `GetQueuedCompletionStatus` and passes the `OVERLAPPED*` back to the stored callback. The `ctThreadIocpCallbackInfo` is deleted by the worker after callback returns.

4. Cancelation
   - If the API that accepted the `OVERLAPPED*` fails immediately (i.e., not pending), the caller must call `cancel_request` to free the `ctThreadIocpCallbackInfo` and avoid memory leaks.
   - `cancel_request` safely deletes the callback info.

5. Shutdown
   - Calling destructor or `ShutdownAndJoin` sets `m_shutdown` and posts null completions equal to the worker count to wake them.
   - Workers exit their loop when `m_shutdown` is observed and a null `OVERLAPPED*` is dequeued.
   - `wil::unique_handle` ensures the IOCP handle is closed automatically.

Testing
-------
- Unit tests in `MSTest/ctsThreadIocpShardUnitTest` exercise:
  - Successful overlapped receive flow via `WSARecvFrom` + sendto trigger.
  - `cancel_request` to confirm immediate failure path frees the request and does not invoke the callback.

Limitations & Notes
-------------------
- The API expects callers to observe Winsock semantics: when an overlapped consumer API returns `SOCKET_ERROR` with `WSAGetLastError() == WSA_IO_PENDING`, the request is pending and will complete via IOCP; otherwise the caller must call `cancel_request`.
- Thread affinity is best-effort; `SetThreadGroupAffinity` return value is ignored.
- On systems without processor-group aware APIs (older Windows versions), behavior may differ; the project now documents a Windows 10+ requirement.

Security & Stability
--------------------
- Use of structured exception handling in the worker loop mirrors previous behavior to avoid swallowing SEH exceptions and to propagate them in a controlled way.
- RAII is used for the IOCP handle to prevent leaks in the presence of exceptions during initialization.

Files
-----
- `ctl/ctThreadIocp_shard.hpp` — main implementation header (lightweight, header-only behavior for this component).
- `MSTest/ctsThreadIocpShardUnitTest/ctsThreadIocpShardUnitTest.cpp` — unit tests that validate receive and cancel paths.

Rationale
---------
- Using IOCP with overlapped Winsock APIs provides the most scalable receive path for high packet rates and high concurrency.
- Sharding worker threads and affinitizing by group/mask improves CPU cache locality and scaling on multi-socket/multi-group systems.

Worker Loop and Batching
------------------------
- The worker loop uses `GetQueuedCompletionStatusEx` to retrieve multiple completions per syscall. Completions are processed in batches using a `std::vector<OVERLAPPED_ENTRY>` sized by the `batchSize` constructor parameter.
- Each completion is dispatched via a single helper (`ProcessOverlapped`) which contains the SEH-protected invocation of the stored callback and deletion of the callback info object. This ensures handling matches the ctThreadIocp behavior.

Future Work
-----------
- Add instrumentation to measure per-shard receive rates and to expose metrics.
- Allow dynamic resizing of worker threads based on runtime load.
- Add stronger validation of supplied `GroupAffinity` entries and expose an API to compute affinities from user constraints.

