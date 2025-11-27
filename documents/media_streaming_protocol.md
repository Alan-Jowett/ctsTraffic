Media Streaming Protocol — Implementation-Accurate Specification

Status: Informational

Abstract

This document is a standalone specification that describes the on-wire behavior of the media streaming protocol used by the current implementation. It is self-contained and does not require access to source code. The document specifies packet formats, discovery behavior, fragmentation and sequence-number rules, validation requirements, and the observable state transitions for initiating and responding endpoints. It also provides practical guidance for developers who need to validate serialization/deserialization and ensure telemetry correctness.

1. Terminology

- Initiator: the endpoint that requests a stream from a remote Responder (typically the receiver).
- Responder: the endpoint that supplies media data to an Initiator (typically the sender).
- Datagram: a single UDP packet delivered as an atomic unit.
- Packet tuple: (source IP, source port, destination IP, destination port) used to identify a logical session.
- Sequence Number: an 8-byte unsigned integer carried in the packet header and used for ordering and deduplication.
- QPC: an 8-byte sender timestamp captured using a high-resolution performance counter (QueryPerformanceCounter-like).
- QPF: an 8-byte frequency value representing the ticks-per-second of the high-resolution counter.

2. Overview

There are two on-wire datagram forms in active use:

- Discovery datagram (one-shot): a short, fixed-length UDP payload used to request a stream. The discovery datagram contains only the ASCII token "START" (five bytes) and no additional protocol header fields.
- Headered datagram (standard data): a datagram that begins with a fixed 26-byte protocol header followed by application payload bytes. This header carries a protocol flag, a sequence number, and sender timing values (QPC and QPF).

Implementations conforming to this specification should treat the discovery datagram and headered datagram as distinct on-wire formats and apply the appropriate parsing and validation rules for each.

3. Byte order and canonicalization

All multi-byte integer fields in headered datagrams are transmitted in little-endian byte order in the current implementation. This includes the 2-byte protocol flag and all 8-byte integer fields. Implementers who require platform-independent network byte order SHOULD explicitly convert fields to network byte order before transmission and convert them back on receipt; such conversions are outside the scope of this document but are recommended for multi-platform interoperability.

QPC / QPF capture and semantics

- Where QPF is obtained: the sender captures the high-resolution frequency using a call that wraps `QueryPerformanceFrequency` (the implementation uses an InitOnce-backed helper to cache the frequency). In code this is exposed as `snap_qpf()` (ctTimer). Because `QueryPerformanceFrequency` is stable for the running OS instance, the returned QPF is effectively constant for the process (and system uptime) — callers retrieve it per-send but it will normally be identical across calls.
- Where QPC is obtained: the sender captures a `QueryPerformanceCounter` value at send time. The send composer refreshes the QPC value just before a WSABUF array for a fragment is returned to the caller (i.e., per-fragment the code calls `QueryPerformanceCounter` to write the latest QPC into the header buffer). As a result, fragments from the same logical send may carry different QPC values (one per fragment) even though they share the same SequenceNumber.
- Values written on the wire: the sender writes the raw 64-bit `QueryPerformanceCounter` value (QPC) and the 64-bit frequency value (QPF) as captured; there is no additional adjustment (e.g., base-time subtraction) applied before transmission in the current implementation.
- Rollover/overflow handling: the implementation does not perform special handling for counter wrap/overflow — the code treats QPC/QPF as 64-bit integers and consumers convert them to elapsed time by performing arithmetic such as `(qpc * 1000) / qpf` to obtain milliseconds. In practice `QueryPerformanceCounter` on modern platforms is 64-bit and wrap-around is not realistic for normal runtime durations, and the cached QPF avoids repeated frequency queries.
- Receiver interpretation: receivers interpret QPC and QPF as raw performance-counter ticks and ticks-per-second respectively. They convert sender and receiver QPC values into elapsed times (ms) using QPF as the divisor to compute timing and jitter metrics.

Note: because QPF is obtained from the system frequency helper, it is stable across the process; however the sender records it per-send and includes it in every datagram so receivers have the explicit frequency value to use even if receiver-side frequency differs (e.g., due to platform differences or testing harnesses).

4. Headered datagram layout (on-wire)

Headered datagrams use the following fixed on-wire byte layout (offsets are zero-based):

- ProtocolFlag: 2 bytes, offset 0
- SequenceNumber: 8 bytes, offset 2
- QPC: 8 bytes, offset 10
- QPF: 8 bytes, offset 18
- Payload: variable, offset 26

Comments:

- The total fixed header length is 26 bytes. Receivers MUST validate that a datagram that claims to be a headered datagram contains at least 26 bytes.
- The ProtocolFlag field distinguishes datagram types. A value designated for data frames indicates the headered layout above; another reserved value is used to identify connection-id style datagrams with a different (longer) minimum length.

5. Connection-id datagram minimum length

Some control datagrams contain a connection identifier string whose canonical representation is a 36-character UUID plus a terminating NUL (37 bytes). When used with the 2-byte ProtocolFlag field, the minimum length for such a connection-id datagram is 39 bytes (2 + 37). Receivers MUST validate connection-id datagrams to be at least 39 bytes long before attempting to parse the identifier.

Clarification: receiver vs sender behavior

- Receiver: the implementation validates connection-id datagrams by enforcing a minimum length of 39 bytes; any datagram with `completedBytes >= 39` and a ProtocolFlag value indicating a connection-id frame is accepted and the connection identifier bytes are copied (first 37 bytes after the 2-byte ProtocolFlag).
- Sender: the implemented sender constructs connection-id datagrams using exactly 39 bytes (2-byte ProtocolFlag + 37-byte NUL-terminated UUID string). The sender-side constructor asserts that the task buffer length equals 39 when preparing the connection-id datagram. Therefore, on the wire you will typically observe an exact 39-byte datagram for connection-id frames even though receivers accept any length >= 39.

This clarification explains why the documentation states 39 bytes as a "minimum" while the sender in practice emits exactly 39 bytes.

6. Discovery behavior (Initiator → Responder)

The discovery handshake is a one-shot token exchange:

1. The Initiator sends a single UDP datagram whose entire payload is the ASCII token "START" (5 bytes). No header fields are present in this discovery datagram.
2. Upon receipt of a datagram whose payload length is exactly five bytes and whose contents match the ASCII letters "S T A R T" in that order, the Responder treats that packet tuple as a request to start a stream for that tuple.

Behavioral notes:

- The Initiator does not wait for an explicit application-layer acknowledgment before receiving media; the discovery token is one-way. The Initiator may begin waiting for incoming headered datagrams after sending the discovery token.

Additional implementation details (discovery matching and initiator behavior):

- Exact match: the server-side extractor performs a byte-wise comparison (`memcmp`) of the received payload against the ASCII string "START" and only treats the datagram as discovery when the received length equals exactly five bytes and the bytes match exactly (case-sensitive, uppercase ASCII `S T A R T`). Packets that differ in case or content will not be treated as discovery.
- Length sensitivity: discovery detection is strict about length — datagrams shorter or longer than 5 bytes are not accepted as the discovery token by the server-listening path. Larger datagrams whose first five bytes equal `START` are not treated as discovery; they are parsed via the normal headered-datagram path instead.
- Initiator send behavior: the Initiator sends the single 5-byte discovery datagram and then waits for media. In the receiver pattern the Initiator will also periodically resend the same 5-byte `"START"` token (via a scheduled timer) until it observes buffered frames from the Responder; these resends are identical control packets (no additional control frames are sent by the Initiator before media is received). In short: the Initiator may retransmit the `"START"` token while waiting, but it does not send other protocol control messages prior to receiving media.
- If the Responder accepts the request it will allocate resources and begin transmitting headered datagrams to the Initiator's tuple. If the Responder rejects the request (for example, due to resource constraints) it may drop the datagram or take implementation-specific actions; there is no guaranteed on-wire error response mandated by this specification.

7. Sequence number semantics and fragmentation

This specification defines sequence-number behavior for fragmented and unfragmented sends. The implementation assigns sequence numbers at the logical-send granularity rather than per physical datagram.

- The SequenceNumber field is an 8-byte unsigned integer included in each headered datagram.
- Sequence numbers are assigned once per logical send request (per logical buffer). When a sender prepares to transmit a logical buffer, it obtains a single SequenceNumber value and tags every datagram produced while fragmenting that buffer with that same SequenceNumber. In short: sequence numbers increment per logical send, not per fragment.
- When a logical media buffer is larger than the allowed payload per datagram and must be split across multiple datagrams (fragmentation), each fragment is transmitted as a separate headered datagram but carries the same SequenceNumber value for that logical buffer.

Implications:

- Receivers must treat each headered datagram as a potentially fragment-bearing item. Because fragments of the same logical buffer share a SequenceNumber, reassembly or detection of fully-received logical buffers must not rely solely on unique SequenceNumber values per datagram; additional application-level bookkeeping (such as byte-counts or explicit fragment metadata) is required to reconstruct larger buffers.

Implementation notes (fragmentation behavior)

- Where the logical buffer is split: the sender partitions the logical buffer inside the send-request iterator used to produce per-datagram send requests. The iterator calculates each fragment's payload length based on the configured maximum datagram size and the remaining bytes to send.

- Where headers are constructed for each fragment: the sender composes the protocol header fields (ProtocolFlag, SequenceNumber, QPC, QPF) as separate scatter/gather buffers that are included with every fragment. Each WSABUF array produced for a fragment contains the full header pieces followed by that fragment's payload bytes.

- Does each fragment include a complete header: yes — every datagram sent by the iterator contains the full fixed header (26 bytes) followed by that fragment's payload. The header is included as the first WSABUF entries for every fragment so header and payload are delivered atomically in the same datagram.

- Minimum payload per fragment and final-fragment rules:
  - The sender requires the logical buffer to be larger than the fixed header length (i.e., the logical buffer must be > 26 bytes) when creating a send request; a send-request with total bytes less than or equal to the header length is rejected by the sender-side constructor.
  - The fragmenting iterator adjusts fragment payload sizes so that any remaining bytes after sending the current fragment will be large enough to contain a future header plus at least one payload byte. Concretely, the iterator reduces the current fragment's payload when the bytes remaining for subsequent fragments would otherwise be <= the header length; this guarantees the next fragment will have at least one payload byte.
  - As a result, the final fragment in the sequence is never emitted with zero payload bytes — every transmitted fragment contains the complete header plus at least one payload byte.

- Relation to documented minimum-length constraints:
  - The fixed header length (26 bytes) remains the minimum headered-datagram size on the wire; however, per the sender's requirement a logical buffer must be strictly larger than the header to be sent at all.
  - Because the iterator enforces at least one payload byte per fragment, receivers do not receive header-only datagrams as valid fragments during normal fragmentation of a logical buffer.

8. Receive-side validation and parsing rules

Upon receiving a UDP datagram, a conforming receiver MUST perform the following checks, in order:

1. If the datagram length equals exactly five bytes, compare the payload to the ASCII string "START". If it matches, treat as discovery (see section 6) and do not attempt header parsing.
2. If not a discovery datagram, read the first two bytes as the ProtocolFlag. If the length is less than 2 bytes, reject the packet.
3. If the ProtocolFlag indicates a headered datagram, validate that the datagram length is at least 26 bytes. If it is shorter, drop the datagram and record a validation error.
4. Parse the SequenceNumber (8 bytes at offset 2), QPC (8 bytes at offset 10), and QPF (8 bytes at offset 18) using little-endian interpretation for multi-byte integers in the current implementation.
5. After parsing the header, the application payload begins at offset 26 and has length equal to (datagram length − 26).

Security note: all parsed fields are untrusted input. Receivers MUST perform bounds checks and defensive copies for any values used to index data structures or allocate memory.

9. Initiator state transitions (observable behavior)

This section describes the observable state transitions an Initiator experiences when requesting and receiving a stream.

- Idle: no active stream requested.
- Requesting: the Initiator sends the single 5-byte discovery datagram containing "START" and then transitions to one of the following states:
  - Waiting: the Initiator remains in a passive receive state, waiting for headered datagrams from the Responder. This is the default after sending the discovery token; it is entered when the Initiator has no immediate received headered datagram to process.
  - Receiving: if headered datagrams arrive immediately (i.e., the Responder began streaming without delay), the Initiator transitions to Receiving and processes incoming datagrams per section 8.
- Closed: terminal state when the session is torn down by either endpoint or the application decides to stop the session.

Notes:

- The transition from Requesting to Waiting is triggered when no headered datagram is yet received after the discovery token is sent. The transition from Waiting to Receiving occurs when the first valid headered datagram arrives from the Responder's tuple.

10. Duplicate, replay, and ordering behavior

- Receivers must use the SequenceNumber to place datagrams into order and to detect duplicates. A sliding-window or bounded recent-sequence cache is recommended to limit memory while providing deduplication.
- Receivers should ignore and not re-process datagrams whose SequenceNumber indicates they were already accepted for playback or control processing.

11. Error handling and telemetry

- Validation failures (length too short, malformed header, invalid protocol flag) must result in the datagram being dropped and appropriate diagnostics incremented.
- The QPC and QPF fields are intended for telemetry (timing and jitter estimates). They MUST NOT be used as security-critical values.

12. Configuration options (local runtime settings)

The following runtime configuration options influence how a Responder produces and schedules media data. These settings are local to the process and are not carried or negotiated on the wire.

- `bitspersecond`: target transmit rate in bits per second. Determines the long-term average throughput and, together with `framerate`, how many bytes are produced per logical frame.
- `framerate`: logical frames per second produced by the Responder. Influences how bytes are grouped into frames and how frequently frames are produced.
- `streamlength`: intended duration of the stream in seconds. Controls how many total bytes/frames the Responder will produce before stopping.

Testing implications:

- Because these options are local, interoperability tests must configure both endpoints appropriately. Tests that measure on-wire throughput or framing behavior should compute expected totals from these values rather than expecting them to be advertised over the protocol.

13. Interoperability recommendations

- For multi-platform interoperability, consider standardizing on network byte order (big-endian) for multi-byte fields and update both sender and receiver to perform explicit conversions.
- When fragmenting, consider adding explicit metadata (e.g., fragment index or a per-frame identifier) if receivers need to reassemble larger logical frames from per-datagram fragments. The current protocol places the burden of reassembly on higher-layer logic.

14. For Developers

This brief section explains how to validate header serialization/deserialization and why consistent offsets matter, without requiring access to source code.

Validation checklist:

1. Round-trip test: construct an in-memory datagram by writing the header fields in little-endian order at the offsets specified in section 4, append a known payload, and then parse the datagram using the same offsets. The parsed SequenceNumber, QPC, and QPF should exactly match the values written.

2. Integration test: send the constructed datagram over UDP from a test sender to the production receiver and assert that the receiver accepts the packet (not a validation error) and that telemetry reports the expected SequenceNumber and timing values.

3. Fragmentation test: produce a logical buffer larger than the datagram payload limit and send it as multiple headered datagrams, each tagged with the same SequenceNumber assigned to that logical buffer. Verify the receiver sees the expected number of datagrams and that sequence numbers are monotonic across logical sends.

Why consistent offsets matter:

- Telemetry correctness: timing metrics (time-in-flight, jitter) rely on accurate QPC and QPF extraction. If offsets used to read telemetry fields are inconsistent with offsets used to write them, timing calculations will be invalid.
- Robustness: using named constants for header lengths and documenting offsets prevents inadvertent misreads when the header layout changes.

Appendix A — Minimum length summary

- Discovery datagram: exactly 5 bytes (payload == "START").
- Headered datagram: at least 26 bytes (2-byte ProtocolFlag + 8-byte SequenceNumber + 8-byte QPC + 8-byte QPF).
- Connection-id datagram: at least 39 bytes (2-byte ProtocolFlag + 37-byte connection id string including terminating NUL).

Appendix B — Security considerations

This protocol specification provides no confidentiality or integrity protection. It assumes operation in environments where the network is trustworthy or where additional transport-layer protections (e.g., IPsec) are applied. Implementers requiring security should add authenticated encryption and replay protection at an appropriate layer.

Authors: protocol design team
Media Streaming Protocol — Current Implementation (RFC-style)

Status: Informational

Note: This document was updated to reflect the current repository state (helpers and receiver reads corrected). It describes the active on-wire behavior as implemented in the current codebase.

Abstract

This document describes the current on-wire media streaming protocol implementation used by the media-streaming components. It captures the exact active behavior on the wire today: how a session is discovered, the datagram header format used for media delivery, validation rules, and the state machines for Initiator and Responder. The text intentionally avoids referring to specific source files — it is a behavioral specification that implementers can use to understand and reproduce the current protocol.

1. Terminology

- Initiator: the endpoint that initiates a request to begin streaming (typically the receiver endpoint).
- Responder: the endpoint that transmits media after the session is established (typically the sender endpoint).
- Packet tuple: (src IP, src port, dst IP, dst port) — used to identify a session on the wire.
- Sequence Number (Seq): 8-byte unsigned integer used to order and deduplicate datagrams.
- QPC: 8-byte high-resolution counter value recorded by the sender in each datagram.
- QPF: 8-byte value recording the frequency of the sender's high-resolution counter.

2. Overview

The implementation uses two different on-wire datagram formats depending on purpose:

- Discovery control datagram (one-shot): a small, legacy discovery token used to request a stream. This datagram is sent as a bare, short UDP payload containing the ASCII token "START" (5 bytes) with no additional per-datagram header fields.

- Standard data/control datagram (per-media payload): used for media payload and any full-header control frames. These datagrams carry a 26-byte header followed by a payload.

The code currently uses the bare discovery token for session initiation (Initiator sends a single short UDP datagram with payload "START"). Media datagrams themselves always use the full header layout documented in section 3.

3. Standard datagram header (on-wire layout)

All media datagrams that carry the standard header follow this exact layout. In the current implementation multi-byte integer fields are written using the host/native byte order (little-endian on common Windows x86/x64 builds):

- ProtocolFlag (2 bytes) @ offset 0
- SequenceNumber (8 bytes) @ offset 2
- Qpc (8 bytes) @ offset 10
- Qpf (8 bytes) @ offset 18
- Payload (variable) @ offset 26

Field semantics and prescribed values:

- ProtocolFlag (2 bytes)
  - `0x0000` — standard data/control datagram that includes the full header fields above.
  - `0x1000` — connection-id datagram (different layout and minimum length; these are handled differently and are out of scope for the simple discovery flow).

- SequenceNumber (8 bytes)
  - Unsigned 64-bit sequence number used for ordering and deduplication of datagrams within a packet tuple.
  - In the current implementation the sender assigns a sequence number once per logical send request and reuses that same sequence number across any fragments produced while partitioning that logical buffer into datagrams. In other words, `IncrementSequence()` is called once per logical send and that returned sequence value tags all datagrams produced for that buffer.

- Qpc (8 bytes)
  - High-resolution counter captured at send time for telemetry and timing calculations.

- Qpf (8 bytes)
  - The frequency (ticks/second) corresponding to the QPC counter value.

Minimum header length for standard datagrams is 26 bytes (2 + 8 + 8 + 8). Implementations MUST verify datagrams claiming to include the standard header contain at least 26 bytes.

Important implementation detail: the code composes the header by pointing `WSABUF` entries at in-memory integer values (see the send composer). Therefore the bytes on the wire reflect host/native endianness (little-endian on Windows) unless the implementation explicitly converts fields during serialization. If canonical network byte order is desired, the send and receive code must be updated to use explicit endianness conversion.

4. Discovery handshake (current behavior)

The active implementation uses a simple one-way discovery handshake to request a stream:

1. Initiator -> Responder: send a single UDP datagram whose ENTIRE payload is the exact ASCII string "START" (5 bytes). No header fields (ProtocolFlag/Seq/Qpc/Qpf) are present in this discovery datagram.

2. Responder: when a datagram is received whose payload is exactly the 5-byte ASCII token "START", the Responder treats that tuple as a stream request and attempts to allocate streaming resources (scheduling, buffers, sender-side state). If successful, the Responder begins sending media datagrams to the Initiator's tuple using the standard header format.

Notes on discovery behavior:
- The discovery token is matched only when the received datagram length equals 5 and the payload bytes equal the ASCII characters `S T A R T`.
- The Initiator sends the bare "START" token once (one-shot). The current code path used by the Initiator does not implement an explicit 3-way handshake or wait/ACK loop before receiving media datagrams.
- If the Responder cannot allocate resources for the requested stream, it may drop the request or perform an out-of-band failure action (implementation-specific). The current simple behavior is to not send a guaranteed protocol-level ACK for the discovery token.

Implementation note: the repository defines a connection identifier string length constant `ctsStatistics::ConnectionIdLength` equal to 37 (36 characters for a UUID plus a terminating NUL). When used inside a connection-id datagram, the overall header length for the connection-id form is therefore `2 + ctsStatistics::ConnectionIdLength == 39` bytes. The exact connection-id datagram layout is handled separately from the standard 26-byte data header.

5. Media frame construction (send-side rules)

Senders producing media datagrams MUST follow these rules to construct each datagram:

1. Prepare header fields in host/native byte order (current implementation) in the following sequence: ProtocolFlag (2 bytes) set to `0x0000`, SequenceNumber (8 bytes), Qpc (8 bytes), Qpf (8 bytes).
2. Append the media payload bytes immediately after the 26-byte header.
3. Transmit the complete header+payload in a single UDP send operation so header and payload arrive atomically in the same datagram. When a logical buffer is larger than the datagram payload capacity, the implementation partitions the buffer; the same SequenceNumber value (assigned once per logical send) tags all fragments produced for that buffer.

Large media buffers that must be split into multiple datagrams are partitioned so that each datagram contains its own 26-byte header followed by that fragment's payload bytes. When splitting, ensure every datagram reserves space for the header and that the partitioning logic avoids producing a final fragment too small to contain header plus at least one payload byte.

6. Receive-side validation and parsing

On receipt of a UDP datagram, the receiver MUST perform the following validation and parsing steps:

1. If the datagram length equals exactly 5 bytes, inspect the 5 payload bytes; if they match the ASCII string "START" exactly, treat this as the discovery request and perform the discovery handling described in section 4.
2. Otherwise, for datagrams expected to carry the standard header, verify the received length is >= 26 bytes. If not, reject and drop the datagram and record a validation error.
3. Read ProtocolFlag (2 bytes). If ProtocolFlag == `0x0000`, parse the following fields using the host/native byte order convention used by the sender (current implementation uses little-endian on Windows): SequenceNumber (8 bytes), Qpc (8 bytes), Qpf (8 bytes). If ProtocolFlag == `0x1000`, handle as a connection-id datagram per local policy.
4. After header extraction, the application payload begins at byte offset 26.

Implementation note: helper functions exist that extract `SequenceNumber`, `Qpc`, and `Qpf` from incoming buffers (commonly named `GetSequenceNumberFromTask`, `GetQueryPerfCounterFromTask`, `GetQueryPerfFrequencyFromTask`). In the current codebase these helpers read from offsets `2`, `10`, and `18` respectively and are used by consumers for telemetry and timing calculations.

Important notes:
- SequenceNumber is used for placing incoming media fragments into the correct playback slot and for deduplication of control actions. Receivers SHOULD maintain a small per-tuple cache of recently processed sequence numbers and ignore duplicate processing of control tokens or datagrams with sequence numbers that have already been accepted.
- Qpc/Qpf are diagnostic — they are provided to compute send timestamps and in-flight time estimates. They MUST NOT be used for authentication or security.

7. Control content and tokens

- The only discovery/control token used by the current implementation is the short discovery token `"START"` (5 bytes) sent as a bare payload with no header. No other ASCII control tokens are present on the wire for discovery in the current implementation.
- Within headered datagrams, the payload is treated as media payload. There is no header-based control token format used for session negotiation in the current behavior.

8. State machines (behavioral description)

Responder (server) state machine (high level):

- Idle: listening for incoming discovery datagrams.
- On discovery (`"START"` received for a tuple): attempt to allocate resources for a new stream. If allocation succeeds, begin sending media datagrams to the initiator tuple using the standard header; transition to Streaming. If allocation fails, remain in Idle or take error handling actions.
- Streaming: actively sending media datagrams to the Initiator; continue until the session is closed or an unrecoverable error occurs.
- Closed: terminal state after teardown.

Initiator (client) state machine (high level):

- Ready: when the local logic decides to request a stream, send a single discovery datagram containing the exact ASCII token `"START"` to the Responder's tuple; then move to Waiting or directly to Receiving depending on local logic.
- Receiving: when media datagrams begin arriving (headered datagrams with ProtocolFlag `0x0000`), process sequence numbers and place payload fragments into the receive buffer for playback.
- Closed: terminal state when the session is torn down.

Note: the current implementation does not implement a multi-step TCP-like 3-way handshake; discovery is performed by a single one-shot token and the Responder may begin streaming immediately.

9. Duplicate and replay handling

- Because media datagrams carry a 64-bit SequenceNumber and shares that space for ordering, implementers MUST deduplicate and ignore processing of datagrams with SequenceNumbers that have already been accepted for control actions. Receivers SHOULD treat retransmitted datagrams as harmless for telemetry but not re-trigger state transitions.
- A sliding-window or bounded recent-sequence cache is recommended to limit memory while providing deduplication.

10. Error handling and telemetry

- If a received datagram fails validation (incorrect size, invalid ProtocolFlag, malformed header), the datagram MUST be dropped and a diagnostic counter incremented.
- If the Responder cannot accept a discovery request due to resource constraints, the implementation may drop the request or optionally perform an out-of-band error response. The current simple behavior is to not send a guaranteed protocol-level error response for discovery failures.
- Implementers SHOULD record metrics for: received bits, bytes per datagram, sequence gaps, duplicate frames, and validation failures.

11. Interoperability notes

- Discovery token matching is exact and length-sensitive: a packet of length 5 whose bytes equal the ASCII characters `S T A R T` is interpreted as discovery; anything else is expected to use the headered datagram format.
- Mixing implementations that change the discovery behavior (e.g., moving discovery into a headered control frame) will break compatibility unless both sides are updated.

12. Recommended tests

 - Unit tests for header serialization/deserialization that reflect the chosen on-wire byte order. For the current codebase, tests should assert fields are serialized/deserialized in host/native byte order (little-endian on Windows). If the project migrates to canonical network byte order, update tests to assert big-endian expectations.
 - Validation tests for malformed and truncated datagrams.
 - Integration tests validating: discovery flow (Initiator sends discovery token; Responder begins streaming), media transfer with correct sequence placement, and behavior under packet loss with duplicate and out-of-order delivery.

13. Configuration options and their impact on the protocol

The runtime configuration options `bitspersecond`, `framerate`, and `streamlength` are not carried on the wire as part of the protocol header; they are local sender-side settings that influence how media data is produced, packetized, and scheduled. Implementers and test authors should understand how these options affect on-wire behavior and validation.

-- `bitspersecond` (bits per second)
  - Purpose: controls the target transmit rate for media payload bytes produced by the Responder (sender).
  - Impact on packetization: the sender calculates bytes-per-second from this value and uses it to determine how many bytes to place into each datagram over time. For a fixed framerate, `bitspersecond` determines payload size per frame; when the payload exceeds MTU, the sender will fragment the frame across multiple datagrams, each with its own 26-byte header and carrying the same SequenceNumber value assigned to that logical buffer.
  - Timing behavior: the send scheduler spaces outgoing datagrams so the long-term average transmit rate approximates `bitspersecond`. This pacing is enforced by per-socket scheduling timers and may affect packet inter-arrival times observed by the Initiator.
  - Tests: validate that the average payload throughput measured at the receiver approximates `bitspersecond` within an acceptable tolerance over `streamlength` seconds.

-- `framerate` (frames per second)
  - Purpose: controls the logical frame rate of media data the sender produces and how often the sender increments the logical frame counter used to partition media into units.
  - Impact on sequence numbers and packet boundaries: the code commonly maps sequence numbers to logical datagrams rather than individual media frames; however `framerate` influences how the sender groups bytes into frames and then into datagrams. For fixed `bitspersecond`, a higher `framerate` results in smaller payload per frame (and therefore potentially smaller datagrams or more fragmentation per frame).
  - Timing behavior: `framerate` influences the nominal inter-frame interval; the sender typically writes frame payloads to outgoing datagrams on a schedule based on `framerate` and the `bitspersecond` budget.
  - Tests: validate per-frame arrival timing when `framerate` is small (e.g., 30 fps) and that frames arrive at approximately the expected inter-frame interval given the configured `bitspersecond` and observed network jitter.

- `streamlength` (seconds)
  - Purpose: controls the intended duration of the generated stream from the sender (total runtime of the session).
  - Impact on sequence number space: the sender increments SequenceNumber for each logical datagram sent; `streamlength` bounds how many sequence numbers are issued in a session. Tests that verify sequence wrap/overflow behavior should choose `streamlength` large enough to trigger edge conditions if needed.
  - Session termination: when the configured `streamlength` elapses, the sender typically ceases sending and transitions to teardown or Closed state. The Initiator should treat the absence of additional datagrams after the configured period as normal session termination.
  - Tests: validate graceful termination at `streamlength` and confirm metrics (bytes, frames sent) match expected totals derived from `bitspersecond * streamlength` and `framerate * streamlength` within acceptable tolerances.


Practical notes for reproducible testing

- When measuring throughput, use a collection window significantly larger than jitter (e.g., several seconds) so short-term pacing artifacts do not skew results.
- Choose `framerate` and `bitspersecond` in tandem to avoid excessive fragmentation: smaller frames at a high `framerate` combined with high `bitspersecond` can increase header overhead (26 bytes per datagram). If precise payload-per-datagram size matters, compute frame payload = (bitspersecond / 8) / framerate and adjust accordingly.
-- SequenceNumber assignment: tests should assert monotonic increase across logical sends and deduplication behavior when duplicates are injected. In the current implementation a sequence number is assigned once per logical send and reused across any fragments produced for that logical buffer; it is not incremented per fragment.

15. Implementation notes and observed discrepancies

The codebase composes and expects the 26-byte header described in section 3 (on-wire offsets 0, 2, 10, and 18). In practice, however, some receiver-side helpers and consumers are not fully consistent with that layout. The following observations reflect the current repository state and may be useful to reviewers and maintainers.

- Sender composition (authoritative): `ctsMediaStreamSendRequests` composes the datagram with `WSABUF` entries in this order: `ProtocolFlag` (2 bytes), `SequenceNumber` (8 bytes), `Qpc` (8 bytes), `Qpf` (8 bytes), then payload. On-wire offsets produced by this composer are 0, 2, 10, and 18 respectively.

- Receiver helpers (observed behavior):
  - `GetSequenceNumberFromTask` copies the 8-byte SequenceNumber from `task.m_buffer + task.m_bufferOffset + 2` (offset 2) and is consistent with the header layout.
  - `GetQueryPerfCounterFromTask` and `GetQueryPerfFrequencyFromTask` currently copy 8 bytes from the same start location as the sequence helper (i.e., from offset 2). This is inconsistent with the header layout (QPC expected at offset 10 and QPF at offset 18) and results in incorrect telemetry values when those helpers are used as-is.

- Receiver consumers (observed legacy reads):
  - Some higher-level consumers perform direct reads using pointer casts instead of the helpers. For example, the receive-side jitter/telemetry logic reads sender timing values using reinterpret_cast reads at `task.m_buffer + 8` and `task.m_buffer + 16`. These literal reads do not match the authoritative header offsets (10 and 18) and also do not include `task.m_bufferOffset`. As a result, those consumers may observe incorrect QPC/QPF values.

- `GetProtocolHeaderFromTask` usage: the protocol-flag accessor reads from `task.m_buffer` directly (not `task.m_buffer + task.m_bufferOffset`). This works for the current recv usage where offsets are zero, but it is a fragile inconsistency to be aware of if non-zero buffer offsets are ever used.

Observed validation / dispatch order (concrete)

- Server listening path (tuple accept / START detection): when the server listening socket receives a datagram it first calls a lightweight extractor that checks `inputLength == 5` and then compares the payload to the ASCII token `"START"`. If matched, the datagram is processed as the discovery request and not parsed as a headered datagram. In practice this means discovery datagrams are recognized before any header-parsing logic on the server-listen path.

- Client receive path (media receiver): the client-side receive handler does not expect to receive a discovery `"START"` token. Its processing order is:
  1. If `completedBytes == 0`, handle special-case (possible loopback close scenario).
  2. Call `ValidateBufferLengthFromTask(task, completedBytes)` which enforces `completedBytes >= 2` before any protocol-flag read and then validates the full header length based on the ProtocolFlag (e.g., `>= 26` for data frames, `>= 39` for connection-id frames).
  3. If the ProtocolFlag is the connection-id form, the connection id is extracted and the receive completes successfully.
  4. Otherwise the code advances `m_bufferOffset` by 26 and verifies payload contents, then uses the sequence-number helper to read the SequenceNumber (helper reads from offset 2).
  5. In the current codebase some consumers then read QPC/QPF via legacy pointer-casts at `task.m_buffer + 8` and `task.m_buffer + 16` instead of using offset-aware helpers (producing mismatched telemetry values).

Potential parsing/telemetry issues to be aware of

- Helpers and consumers disagree on where QPC/QPF live in the datagram; until the helpers and all consumers read the same offsets, telemetry (timing/jitter) will be wrong in consumers that do not use the corrected helper implementations.
- The mixture of `memcpy_s`-based helper reads and `reinterpret_cast` direct reads increases the chance of misaligned or incorrect field extraction. Favoring the offset-aware `memcpy_s` helpers everywhere reduces this risk.
- `GetProtocolHeaderFromTask` reading from `task.m_buffer` (without `m_bufferOffset`) is currently safe but brittle; consider normalizing callers or the helper to always use `task.m_bufferOffset` for future refactors.

Suggested next maintenance actions (no code changes applied here):

- Update `GetQueryPerfCounterFromTask` to copy QPC from the byte offset corresponding to the SequenceNumber + QPC (i.e., header offset 10).
- Update `GetQueryPerfFrequencyFromTask` to copy QPF from header offset 18.
- Replace legacy direct pointer-cast reads in consumers with calls to the corrected helpers so telemetry consumers read consistent values and respect `task.m_bufferOffset`.

These changes will not alter the on-wire layout but will make telemetry and timing calculations consistent with the documented header layout.

14. Authors: protocol design team

Authors: protocol design team


