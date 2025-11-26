# Media Stream Refactor Plan

Goal
----
Extract media streaming send and receive logic into clear, reusable modules so that either the client or the server can act as the sender. Keep runtime behaviour unchanged by default and keep threading/IOCP contracts intact.

Scope
-----
- Files inspected / in-scope:
  - `ctsMediaStreamClient.cpp`, `ctsMediaStreamClient.h`
  - `ctsMediaStreamServer.cpp`, `ctsMediaStreamServer.h`
  - `ctsMediaStreamProtocol.hpp`
  - `ctsMediaStreamServerConnectedSocket.*`
  - `ctsMediaStreamServerListeningSocket.*`
  - Any helpers referenced by those files (search term: `MediaStream`, `SendBuffer`, `RateLimit`, `packet`, `payload`)

High-level approach
-------------------
1. Inventory current responsibilities: map functions and classes to "send" vs "receive" responsibilities.
2. Define stable, small interfaces for send and receive roles (`IMediaStreamSender`, `IMediaStreamReceiver`).
3. Move send-only code to `ctsMediaStreamSend.cpp/.h` and receive-only code to `ctsMediaStreamReceive.cpp/.h`.
4. Update client and server components to depend on interfaces rather than concrete implementations; allow runtime selection of which side is sender.
5. Update server connected/listening socket files to accept or create sender/receiver instances.
6. Add/adjust unit tests and integration smoke tests. Update project files.

Proposed interfaces (examples)
-----------------------------
These are minimal, illustrative signatures. Exact names and parameters will be refined during implementation to match existing types and threading contracts.

// sender interface
class IMediaStreamSender
{
public:
    virtual ~IMediaStreamSender() = default;
    // Begin streaming on the given socket or connection context
    virtual void StartSending() = 0;
    // Stop/pause sending; may block until in-flight packets are drained
    virtual void StopSending() = 0;
    // Push data (buffer ownership semantics to match current code)
    virtual bool EnqueuePayload(const uint8_t* buffer, size_t length) = 0;
};

// receiver interface
class IMediaStreamReceiver
{
public:
    virtual ~IMediaStreamReceiver() = default;
    // Called when a datagram is received
    virtual void OnDatagram(const uint8_t* buffer, size_t length) = 0;
    // Lifecycle hooks
    virtual void StartReceiving() = 0;
    virtual void StopReceiving() = 0;
};

Notes on threading and io
------------------------
- Preserve existing IOCP / RIO usage by keeping the same call sites where overlapped operations or RIO APIs are issued.
- The sender/receiver modules should not create threads silently; prefer to accept executor/queue references or operate in-caller-thread as current code does.

Migration steps (detailed)
-------------------------
1) Inventory (done first)
   - Produce a short mapping (file -> classes/functions -> role). This reduces risk when moving code.
2) Design interfaces (small header in `ctsMediaStreamProtocol.hpp` or a new `ctsMediaStreamInterfaces.hpp`)
   - Decide on ownership and lifetimes (who owns buffers; who destroys the stream objects).
3) Extract send implementation
   - Create `ctsMediaStreamSend.h` / `ctsMediaStreamSend.cpp`.
   - Move packetization and rate-limiting logic, plus send-queue management.
   - Keep internal helper static/anonymous where possible to limit public surface.
4) Extract receive implementation
   - Create `ctsMediaStreamReceive.h` / `ctsMediaStreamReceive.cpp`.
   - Move datagram parsing, validation, reassembly, and stats reporting.
5) Hook up client/server
   - Replace concrete code in `ctsMediaStreamClient.*` and `ctsMediaStreamServer.*` with calls that instantiate and use the interfaces.
   - Add runtime flag or configuration option to choose which side is the sender.
6) Update socket-layer files
   - `ctsMediaStreamServerConnectedSocket.*` and `ctsMediaStreamServerListeningSocket.*` should accept a sender/receiver instance or factory.
7) Update project files
   - Add new .cpp/.h to `ctsTraffic.vcxproj` and ensure headers are findable.
8) Tests & validation
   - Update MSTest units to reference new headers.
   - Add one integration test: client sends to server (default), and server sends to client (swapped sender) to verify symmetry.

Acceptance criteria
-------------------
- Default behaviour unchanged when roles are not swapped.
- New files compile and are included in `ctsTraffic.vcxproj`.
- Unit tests that previously covered media streaming compile; integration smoke scenarios run.

Risks and mitigations
---------------------
- Risk: Threading or lifetime changes introduce subtle data races.
  - Mitigation: Keep IO and overlapped calls at the same call sites; do not change lifetimes during first refactor iteration.
- Risk: API surface change breaks other modules.
  - Mitigation: Keep interfaces minimal and provide adapters where necessary.

Checklist (quick)
-----------------
- [ ] Inventory completed and mapped to roles
- [ ] Interface header created and reviewed
- [ ] `ctsMediaStreamSend.*` added and compiled
- [ ] `ctsMediaStreamReceive.*` added and compiled
- [ ] Client and server refactored to use interfaces
- [ ] Project files updated and solution builds
- [ ] Unit tests updated and passing

Estimated effort
----------------
- Inventory & interface design: 1-2 hours
- Extract send/receive: 2-4 hours each (depends on code entanglement)
- Integration + tests + build fixes: 1-3 hours

Next steps
----------
1. Review this document and confirm the interface style and the extraction filenames.
2. I will start the inventory step and list the exact functions/classes mapped to send/receive roles.

If you approve, say "Proceed with inventory" and I'll begin inspecting the code and update the todo progress.
