Media Streaming Protocol — Implementation Migration Plan

Goal

Migrate the implementation from the legacy discovery model that used a 5-byte `START` discovery token to the new, integrated 3‑way handshake (SYN / SYN-ACK / ACK) defined in `media_streaming_protocol_spec_3way.md`.

Summary of required changes

- Replace all places that send the legacy `START` payload with logic to send a headered `SYN` control frame.
- Add encoder/decoder helpers for the SYN / SYN-ACK / ACK control frames and integrate them into the existing headered datagram parsing flow.
- Update receiver parsing to recognize the new control flags (`0x0100`, `0x0101`, `0x0102`) and to perform handshake state transitions (allocate/assign ConnectionId, flush dedup caches, etc.).
- Ensure connection-id datagram (`0x1000`) remains supported and add guidance for the responder to send its connection-id immediately after `SYN-ACK` if it begins sending media prior to receiving `ACK`.
- Add a backward-compatibility configuration option to accept legacy 5-byte `START` payloads if desired.

High-level migration steps (recommended order)

1. Add ProtocolFlag constants and control-frame helpers
   - Files: `ctsMediaStreamProtocol.hpp`
   - Actions:
     - Define new ProtocolFlag constants (e.g., `c_udpDatagramFlagSyn = 0x0100`, `c_udpDatagramFlagSynAck = 0x0101`, `c_udpDatagramFlagAck = 0x0102`).
     - Add fixed sizes and helper prototypes for control frames (encoder/decoder) in the header file.
     - Add `MakeSynTask`, `MakeSynAckTask`, `MakeAckTask` functions similar in shape to `MakeConnectionIdTask` but which produce headered datagrams (26-byte header + control body).
     - Add `ParseControlFrame` helper to validate control frame payloads and extract fields (Version, Flags, 37-byte connection id, optional TLVs).

2. Add control frame implementations
   - Files: `ctsMediaStreamProtocol.hpp` (inline small helpers) or new `ctsMediaStreamProtocol.cpp` if preferred for implementation code.
   - Actions:
     - Implement `MakeSynTask`/`MakeSynAckTask`/`MakeAckTask` to populate a `ctsTask` that will be queued on the socket sending path. These should set the ProtocolFlag in the first 2 bytes and follow the 26-byte header layout for headered datagrams. For control frames SequenceNumber/QPC/QPF SHOULD be zero.
     - Implement `ParseControlFrame` to run during receive parsing when ProtocolFlag is a control value.

3. Update send-side code to use SYN instead of legacy START
   - Files: `ctsIOPatternMediaStream.cpp`, potentially other IO pattern or client-init code that sends `START`.
   - Locations:
     - `ctsIOPatternMediaStream.cpp` — replace `c_startBuffer[] = "START"` and its `resendTask` / Send logic with calls to produce a `SYN` control task (using `MakeSynTask`).
     - Any other call-sites that construct a task with `g_udpDatagramStartString` or `MediaStreamAction::START` (see `ctsMediaStreamProtocol.hpp`) should be updated to build a SYN headered frame instead.
   - Actions:
     - Replace send of the 5-byte payload with an enqueued headered `SYN` task.
     - Ensure retransmission/backoff logic is preserved but operates on the SYN task.

4. Update receive-side parsing and handshake state machine
   - Files: `ctsMediaStreamProtocol.hpp`, `ctsIOPatternMediaStream.cpp`, `ctsIOPattern.cpp`, `ctsSocketBroker.cpp`/`ctsSocket.cpp` (where inbound datagrams are parsed/handled).
   - Locations and functions to update (based on grep results):
     - `ctsMediaStreamProtocol.hpp` — function(s): `ValidateBufferLengthFromTask`, `GetSequenceNumberFromTask`, `MakeConnectionIdTask`, `GetConnectionIdFromTask`, and the switch that handles `MediaStreamAction::START`.
     - `ctsIOPatternMediaStream.cpp` — where tasks are created/received and where `ctsMediaStreamMessage::SetConnectionIdFromTask` and `GetSequenceNumberFromTask` are used.
     - Receiver code that currently checks for the 5-byte `START` token (search results: `ctsMediaStreamProtocol.hpp` lines that compare `inputBuffer` to `g_udpDatagramStartString`, and `ctsIOPatternMediaStream.cpp` where `c_startBuffer` is used). Replace these checks with logic that:
       - Parses the 2-byte ProtocolFlag at offset 0 for incoming datagrams and uses the headered datagram logic order (connection-id versus headered datagram).
       - If ProtocolFlag denotes a control frame (`0x0100`..`0x0102`), invoke `ParseControlFrame` and handle handshake state transitions: when `SYN` arrives, generate/send `SYN-ACK`; when `SYN-ACK` arrives, generate/send `ACK`; when `ACK` arrives, bind the tuple to the connection-id and flush dedup caches as specified.
   - Actions:
     - Implement a small handshake state table per tuple (or reuse existing session mapping structures) to track whether `SYN` received/sent, `SYN-ACK` sent/received, and `ACK` confirmed.
     - Ensure reassembly/deduplication caches are flushed when connection-id changes or handshake completes per new rules.
     - Add handling to accept connection-id datagrams and map them to tuple state.

5. Maintain or add backward-compatible START support behind a flag
   - Files: `ctsConfig.h` / `ctsConfig.cpp` (or wherever runtime flags/config live). Add a boolean like `g_configSettings->AllowLegacyStart` default `false`.
   - Actions:
     - If `AllowLegacyStart` is true, keep the old 5-byte special-case logic that checks for the exact-length START payload prior to headered datagram parsing. Otherwise remove it.

6. Update send-path when responder accepts SYN
   - Files: `ctsIOPatternMediaStream.cpp`, `ctsIOPattern.cpp` (task creation/send path)
   - Actions:
     - When Responder accepts a SYN (i.e., sends `SYN-ACK` with Accept=1 and AssignedConnectionId), ensure Responder sends a `connection-id` datagram (`0x1000`) as first datagram after `SYN-ACK` if it intends to start sending media immediately prior to receiving the `ACK`.
     - Alternatively, embed connection-id in a small control TLV in media datagrams if you prefer; update senders/receivers accordingly.

7. Update tests and unit tests
   - Files: MSTest tests referencing behavior around START, discovery, or connection-id. Update or add tests to cover:
     - SYN / SYN-ACK / ACK encoding/decoding round-trip.
     - Legacy START acceptance only when the new config flag is enabled.
     - Deduplication cache flush behavior when connection-id changes.
     - Responder optimistic start (SYN-ACK + connection-id + media before ACK) scenarios.

8. Documentation and release notes
   - Files: `documents/media_streaming_protocol_spec_3way.md`, `README.md` — add migration notes and instructions for operators to enable legacy START acceptance during transition.

Targeted files and functions (concrete list)

- ctsTraffic/ctsTraffic/ctsMediaStreamProtocol.hpp
  - Add: ProtocolFlag constants (`c_udpDatagramFlagSyn`, `c_udpDatagramFlagSynAck`, `c_udpDatagramFlagAck`).
  - Add: `MakeSynTask`, `MakeSynAckTask`, `MakeAckTask`, `ParseControlFrame` prototypes and inline helpers for payload sizes.
  - Update: Remove or keep behind flag the existing `g_udpDatagramStartString` and `MediaStreamAction::START` handling. Update `ValidateBufferLengthFromTask` pathing and any `START` switch cases.
  - Keep/adjust: `MakeConnectionIdTask`, `SetConnectionIdFromTask`, `GetSequenceNumberFromTask` to interoperate with control frames.

- ctsTraffic/ctsTraffic/ctsIOPatternMediaStream.cpp
  - Replace `c_startBuffer[] = "START"` send logic with constructing and sending `SYN` control frame using `MakeSynTask`.
  - Update resend and backoff logic to operate on SYN tasks.
  - Update receive-path logic that previously expected the 5-byte START to now initiate handshake state machine on SYN.

- ctsTraffic/ctsTraffic/ctsIOPattern.cpp
  - Verify and update cases that create/expect `UdpConnectionId` tasks (send/recv) so they accept connection-id datagram interactions in handshake sequences.

- ctsTraffic/ctsTraffic/ctsSocket*.cpp (where incoming datagrams are parsed)
  - Identify the function(s) that parse raw UDP datagrams and route them to higher-level handlers. Update the parsing order: connection-id check (`0x1000`) -> headered datagram parse -> control frame handling if ProtocolFlag in control range -> legacy START only if `AllowLegacyStart`.

- ctsTraffic/ctsTraffic/ctsIOPatternMediaStreamReceiver logic
  - Update logic that uses `GetSequenceNumberFromTask` and `SetConnectionIdFromTask` to remain compatible when media arrives before handshake completion. Use connection-id mapping to disambiguate sessions.

- ctsTraffic/ctsTraffic/ctsIOPatternRateLimitPolicy.hpp (as needed)
  - No direct protocol changes expected, but verify any behavior that starts sending only after START must be updated to work with handshake semantics.

- ctsTraffic/ctsTraffic/ctsConfig.*
  - Add configuration option `AllowLegacyStart` and possibly handshake timeouts, retransmit/backoff parameters.

- Unit Tests (MSTest)
  - Update or add tests under `MSTest/` to verify handshake encode/decode and receive/send behavior.

Detailed implementation notes and guidance

- Control frame layout and framing
  - Control frames are headered datagrams and must include the 26-byte header. For control frames SequenceNumber/QPC/QPF MAY be zero.
  - Control body fixed prefix: Version (1 byte), Flags (1 byte), Reserved (2 bytes), 37-byte connection id string, optional TLVs. TLV usage should follow existing reserved-extension guidance.

- Parsing order
  - On receive, check datagram length >= 2 then read ProtocolFlag.
  - If ProtocolFlag == `0x1000`, treat as connection-id (37 byte payload) and map to tuple.
  - Else if ProtocolFlag is headered data flag or control flags (`0x0000`, `0x0001`, `0x0100`..`0x0102`), ensure datagram length >= 26 and parse header fields.
  - If ProtocolFlag in `0x0100..0x0102`, invoke handshake parsing and state transitions.
  - Only if configured to allow the legacy discovery token should the receiver also accept an exact-length (5-byte) `START` payload prior to headered parsing.

- Handshake state storage
  - Add a small per-tuple handshake state entry (in the existing tuple-to-session map) recording: `state` (none, syn-sent, syn-received, synack-sent, established), `assigned_connection_id`, `last_syn_time`, retransmit/backoff info.
  - When `SYN` is received for a tuple with no active session, the Responder should decide to accept or reject and send back a `SYN-ACK` with `AssignedConnectionId`. The Responder should set the tuple-to-connection-id mapping when it either accepts and/or receives the `ACK`.

- Deduplication flush
  - When a new connection-id is associated with a tuple (either because the Responder assigns one or an incoming connection-id datagram changes it), flush deduplication and reassembly caches for that tuple. If you already key caches by connection-id when available, this can be a simple mapping change.

- Optimistic start
  - If the Responder wishes to begin streaming immediately after sending `SYN-ACK`, it MUST include an explicit connection-id datagram as the first datagram after `SYN-ACK` (or embed the connection-id in media datagrams via a small TLV). Receivers must use that connection-id information (rather than tuple alone) to map incoming media to session state.

Testing checklist

- Unit tests:
  - Round-trip encode/decode for SYN, SYN-ACK, ACK control frames.
  - Parser tests: headered datagram parse including fragmentation flag and control flags.
  - Legacy acceptance test: legacy START accepted only when config enabled.
  - Deduplication flush tests: confirm that changing connection-id or completing handshake flushes prior dedup state.

- Integration tests:
  - End-to-end handshake: Initiator sends SYN, Responder replies SYN-ACK + connection-id, Initiator sends ACK; confirm session established and media flows.
  - Optimistic start: Responder sends SYN-ACK and connection-id and begins sending media before ACK; Initiator accepts media when connection-id arrives and/or ACK completed.
  - Retransmission/backoff: simulate lost handshake packets and ensure proper retransmit/backoff behaviour.

Rollout / compatibility guidance

- Add the new handshake behavior behind a feature gate if you cannot update all peers simultaneously. Recommended default is to enable the handshake on both Initiator and Responder in new deployments and allow legacy START only when `AllowLegacyStart` is explicitly enabled for migration.
- Document the change in release notes and in `README.md` with instructions to enable `AllowLegacyStart` temporarily during migration.

Estimated effort

- Small change (1–2 days): add constants and encoder/decoder helpers, update `ctsIOPatternMediaStream.cpp` to send SYN, and update receive parsing to route control frames to a simple handshake handler.
- Medium (2–4 days): add state-tracking, thorough tests, and integrate optimistic-start behavior robustly.
- Larger (4+ days): if you want to embed connection-id TLVs directly into media datagrams (requires header extensions), or if the codebase requires refactoring to centralize parsing, expect more time.

If you want, I can:
- Produce a targeted patch (PR) that implements the constants, helper functions, and replaces the `START` sends in `ctsIOPatternMediaStream.cpp` with `SYN` sends as a minimal-first-change.
- Add unit tests that verify frame encoding/decoding and simple handshake flow.

Tell me whether you prefer a minimal incremental patch (change sends only, add decoder stubs) or a complete implementation (helpers + handshake state + tests) and I will implement it next.