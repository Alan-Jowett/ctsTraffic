Media Streaming Protocol — 3-Way Handshake Conformance Specification

Status: Informational

Abstract

This document specifies a minimal, precise, and implementation-independent protocol for discovery, session establishment using a three-way handshake, and media delivery over UDP datagrams. It preserves the existing headered datagram and control-frame formats while replacing the original single-way `START` discovery token with a TCP-style 3-way handshake that is implemented using headered control datagrams. The handshake is integrated into the same headered datagram namespace (ProtocolFlag values) so session establishment, control messages, and data frames share a unified parsing model.

1. Terminology

- Initiator: the endpoint that requests a stream from a Responder (typically the receiver).
- Responder: the endpoint that supplies media data to an Initiator (typically the sender).
- Datagram: a single UDP packet delivered as an atomic unit.
- Packet tuple: (source IP, source port, destination IP, destination port) used to identify a logical session.
- Sequence Number: unsigned 64-bit integer carried in the header and used for ordering and deduplication.
- QPC: unsigned 64-bit sender timestamp captured from a high-resolution clock at send time.
- QPF: unsigned 64-bit frequency value representing clock ticks per second for the clock used to capture QPC.
- Headered Datagram: a datagram that begins with the fixed protocol header defined in section 4 followed by application payload bytes.
- Control Frame: a headered datagram with a reserved control ProtocolFlag value indicating the payload is a control message rather than media data.

2. Design Goals

- Simplicity: minimal on-wire state for discovery and session establishment, and simple header layout for timing and ordering.
- Interoperability: precise byte offsets and field sizes to allow independent implementations to interoperate.
- Defensive parsing: clear validation rules preventing ambiguous parsing between control and data datagrams.
- Unified framing: session establishment is performed using the same headered framing as data and other control messages (no separate "START" discovery token).

3. Byte Order and Encoding

All multi-byte integer fields in this protocol MUST be transmitted using little-endian byte order. Implementations MUST parse and generate fields using little-endian encoding; no alternate endianness is permitted for interoperable implementations.

4. Datagram Types and Disambiguation

An implementation MUST recognize the following datagram forms and disambiguate them exactly as specified. Implementations MUST test received datagrams in this order:

- Connection-id Datagram: a control datagram consisting of ProtocolFlag value `0x1000` followed immediately by a NUL-terminated connection identifier string (37 bytes canonical). See section 7.
- Headered Datagram (control or data): begins with the 26-byte protocol header (section 5) followed by payload bytes. Control frames are indicated by reserved ProtocolFlag values described below.

Notes:
- The previous specification used a 5-byte "START" discovery token that had no header. This document replaces that token with a 3-way handshake implemented as headered control frames. Implementations that previously relied on the lone 5-byte discovery payload MUST be updated to accept the new handshake frames; however, for backward compatibility a receiver MAY optionally accept the legacy 5-byte discovery token if explicitly configured to do so (see section 14).

5. Header Layout (on-wire)

Headered Datagrams use the following fixed on-wire byte layout (offsets are zero-based):

- ProtocolFlag: 2 bytes, offset 0
- SequenceNumber: 8 bytes, offset 2
- QPC: 8 bytes, offset 10
- QPF: 8 bytes, offset 18
- Payload: variable, offset 26

Total fixed header length: 26 bytes.

Field descriptions:

- ProtocolFlag (2 bytes): unsigned 16-bit value distinguishing datagram subtypes within the headered format. For headered datagrams this field MUST be one of the following values:
  - `0x0000` — Standard data datagram with no fragmentation metadata.
  - `0x0001` — Standard data datagram with the Fragmentation flag set; a 16-byte Fragment Metadata block MUST follow the fixed header.
  - `0x0100` — Control frame: `SYN` (Initiator -> Responder) — begins a session handshake. The payload is a `SYN` control body (section 6.1).
  - `0x0101` — Control frame: `SYN-ACK` (Responder -> Initiator) — acknowledges `SYN` and conveys responder state. The payload is a `SYN-ACK` control body (section 6.2).
  - `0x0102` — Control frame: `ACK` (Initiator -> Responder) — completes the handshake. The payload is an `ACK` control body (section 6.3).

Implementations MUST reject headered datagrams whose ProtocolFlag is any unsupported value and MUST record a validation error when such datagrams are received.

- SequenceNumber (8 bytes): unsigned 64-bit integer assigned once per logical send. For control frames that are not associated with a logical media send, SequenceNumber SHOULD be set to zero.

- QPC (8 bytes) and QPF (8 bytes): same semantics as in the base protocol — QPC is a sender clock sample; QPF is ticks-per-second for that clock. Control frames MAY populate these fields for telemetry but SHOULD set them to zero if not meaningful for the control message.

6. Session Establishment — 3-Way Handshake

This section defines a simple three-way handshake (SYN, SYN-ACK, ACK) carried inside headered control frames. The handshake associates the packet tuple with a session identifier and provides explicit semantics for deduplication flush and SequenceNumber reuse. The handshake is intentionally minimal and integrates with existing connection-id control datagrams.

6.1. SYN (Initiator -> Responder)

- Datagram: headered datagram with `ProtocolFlag == 0x0100`.
- SequenceNumber: SHOULD be zero.
- Payload (control body), little-endian layout:
  - Version: 1 byte — protocol version (current value = 1).
  - Flags: 1 byte — reserved bits for future use; currently 0.
  - Reserved: 2 bytes — NUL for alignment.
  - DesiredConnectionId: 37 bytes — optional NUL-terminated ASCII string the Initiator prefers as connection id (may be all zeros/NULs to request a responder-assigned id).
  - OptionalOptions: variable — optional TLV-encoded options (length-prefixed) allowed but MUST be ignored by receivers that do not understand them.

- Semantics: The Initiator sends `SYN` to request a stream from the Responder. Sending `SYN` indicates the Initiator's willingness to accept the Responder's stream on the tuple from which `SYN` was sent.

- Retransmission: The Initiator MAY retransmit `SYN` periodically if no `SYN-ACK` is received; implementations SHOULD apply exponential backoff and limit retry attempts to avoid amplification.

6.2. SYN-ACK (Responder -> Initiator)

- Datagram: headered datagram with `ProtocolFlag == 0x0101`.
- SequenceNumber: SHOULD be zero.
- Payload (control body), little-endian layout:
  - Version: 1 byte — protocol version (current value = 1).
  - Flags: 1 byte — bitfield; bit 0 (0x01) = `Accept` (1=accept, 0=reject), other bits reserved.
  - Reserved: 2 bytes — NUL for alignment.
  - AssignedConnectionId: 37 bytes — NUL-terminated ASCII connection id assigned by Responder (if Accept=1). If Reject, this field MAY be all zeros or contain a diagnostic string.
  - ReceiverOptions: variable — optional TLV-encoded options describing limits (e.g., max reassembly size) that the Responder supports; unknown TLVs MUST be ignored by the Initiator.

- Semantics: On receipt of a valid `SYN`, the Responder MAY allocate resources and decide whether to accept or reject. If accepting, the Responder returns `SYN-ACK` with `Accept=1` and provides an `AssignedConnectionId`. The Responder SHOULD send `SYN-ACK` from the port and address it will use for subsequent streaming and SHOULD begin sending media datagrams once the handshake completes (see section 6.4 for ordering rules).

6.3. ACK (Initiator -> Responder)

- Datagram: headered datagram with `ProtocolFlag == 0x0102`.
- SequenceNumber: SHOULD be zero.
- Payload (control body), little-endian layout:
  - Version: 1 byte — protocol version (current value = 1).
  - Flags: 1 byte — reserved for future use.
  - Reserved: 2 bytes — NUL for alignment.
  - ConfirmedConnectionId: 37 bytes — the NUL-terminated connection id the Initiator acknowledges (typically the `AssignedConnectionId` from `SYN-ACK`).
  - OptionalOptions: variable — optional TLV-encoded options, MUST be ignored if unknown.

- Semantics: Receipt of `ACK` by the Responder completes the handshake. Upon receipt the Responder SHOULD consider the packet tuple bound to the acknowledged connection id and may begin or continue streaming media to the Initiator's tuple.

6.4. Handshake Timing and Ordering Rules

- Completion: The handshake is complete from the Responder's perspective when it receives the `ACK` acknowledging the `AssignedConnectionId`. The Initiator may consider the handshake complete when it receives a `SYN-ACK` with `Accept=1` (or optionally after it transmits `ACK`). Implementations SHOULD tolerate reordered or lost handshake datagrams using retransmission timers.

- Data before completion: The Responder SHOULD NOT rely on having received an `ACK` before sending media; the Responder MAY begin sending media datagrams immediately after sending `SYN-ACK` (optimistic start). To avoid ambiguity, media datagrams MUST include the AssignedConnectionId via an explicit connection-id control datagram (section 7) sent by the Responder as the first datagram after `SYN-ACK`, or the Responder may include the ConnectionId in a small control header inside media datagrams using a reserved TLV (see Appendix A for extension guidance). Receivers MUST use the presence of a connection-id datagram or an explicit connection-id in control metadata to disambiguate session identity when media arrives before `ACK`.

- Deduplication flush rule: Upon creating or changing the mapping between a packet tuple and a connection-id, implementations MUST flush deduplication and reassembly caches for that tuple. Specifically, when the Responder accepts a `SYN` and assigns a different ConnectionId for an existing tuple, the receiver MUST treat subsequent SequenceNumbers as new and not deduplicate against prior state for the same tuple. The `ACK` datagram also serves as a synchronization point for state transitions.

7. Connection-id Datagram (control)

- Format: The connection-id datagram remains a minimal control datagram and is NOT a headered data datagram payload type. Its on-wire layout is:
  - ProtocolFlag: 2 bytes (value `0x1000`) at offset 0
  - ConnectionIdString: 37 bytes at offsets 2..38 inclusive — canonical form is a 36-character UUID ASCII string followed by a NUL (0x00) terminator.

- Minimum length: Receivers MUST validate that a connection-id datagram has length >= 39 bytes (2 + 37) before attempting extraction.

- Extraction: Receivers MUST copy the first 37 bytes immediately following the ProtocolFlag and treat them as the NUL-terminated connection id string. Any trailing bytes beyond the 37-byte canonical field MAY be ignored.

- Semantics/Purpose: Connection-id datagrams provide an explicit, minimal control mechanism to associate a packet tuple with an opaque connection identifier used for correlation, logging, or out-of-band telemetry. Typical approved uses:
  - Responder sends a connection-id to the Initiator shortly after accepting a `SYN` to provide a stable identifier for the stream.
  - Either endpoint may send a connection-id as a control message to update or reassign the identifier for a tuple when practical.
  - Receivers use the connection id to correlate metrics, logs, and to map incoming packets to higher-level session state when tuple- or port-based correlation is insufficient.

Implementations MUST NOT assume connection-id datagrams imply the presence of a 26-byte header or of sequence/telemetry fields; the two forms are disjoint by design.

- Validation and treatment: Receivers MUST treat the connection-id field as an opaque identifier for correlation. Receivers MAY perform optional syntax validation (for example, validate the 36-byte portion matches conventional UUID textual syntax with hyphens) for diagnostics or policy enforcement, but MUST NOT reject or drop the datagram solely because the identifier does not parse as a UUID. Implementations MUST accept any 37-byte NUL-terminated string as a valid connection id for mapping and correlation purposes.

- Session semantics: A change in the observed connection-id for a packet tuple SHOULD be interpreted as a session re-identification for that tuple. Receivers MUST treat different connection-id values as distinct logical sessions even when the 4-tuple is identical.

- Security note: Connection-id values are supplied by remote endpoints and can be adversarially controlled. Receivers MUST NOT treat connection-id values as secrets or use them for access control decisions.

8. Packet Reception and Validation

When a receiver receives a UDP datagram it MUST perform these checks in order:

1. If datagram length < 2, drop and record a validation error.
2. Read the 2-byte ProtocolFlag at offset 0 (little-endian).
  - If ProtocolFlag == `0x1000` (connection-id): verify datagram length >= 39; if shorter, drop and record a validation error. Otherwise extract the first 37 bytes after the ProtocolFlag as the NUL-terminated connection id string and process it (section 7).
  - Else if ProtocolFlag is one of the defined headered flags (`0x0000`, `0x0001`, `0x0100`, `0x0101`, `0x0102`): verify datagram length >= 26 for headered datagrams; if shorter, drop and record a validation error. Then parse the header fields as below.
  - Otherwise: treat ProtocolFlag as unknown, drop datagram and record a validation error.
3. For headered datagrams: parse SequenceNumber (offset 2), QPC (offset 10), QPF (offset 18) as unsigned little-endian integers. Determine whether the fragmentation flag is set (ProtocolFlag bit `0x0001` for data frames). If set, verify datagram length >= 42 (26 + 16) and parse the 16-byte Fragment Metadata block: FragmentOffset (8 bytes) and TotalLength (8 bytes). The application payload begins at offset 26 (no-fragmentation) or 42 (with fragment metadata) and has length (datagram length − payload_offset).
4. If the ProtocolFlag indicates a control frame (0x0100..0x0102), parse the control body according to the control frame layout (section 6) and handle handshake state transitions, validation, and optional options TLVs.
5. All parsed fields MUST be validated as untrusted input before use.

Receivers MUST not rely on any implicit bufferOffset; all offsets in this specification are absolute within the datagram payload as received from the transport.

9. Sequence Assignment, Fragmentation, Ordering, and Reassembly

- Sequence number granularity: The sender MUST allocate a single SequenceNumber for each logical send (logical buffer). All fragments produced while partitioning that logical buffer MUST carry the same SequenceNumber.

- Fragment composition (extension): To enable unambiguous reassembly, this specification defines an optional per-fragment metadata block that immediately follows the 26-byte header when the sender sets the Fragmentation flag described below. The base header alone does not contain fragment positioning metadata; therefore, senders MUST choose one of the following strategies and advertise/implement it consistently:
  - Inline Fragment Metadata (recommended, normative): The sender MUST set bit `0x0001` in the ProtocolFlag (i.e., ProtocolFlag & `0x0001` != 0) to indicate that a 16-byte Fragment Metadata block follows the fixed 26-byte header. The metadata block layout (little-endian) is:
    - FragmentOffset: 8 bytes (unsigned 64-bit) — byte offset of this fragment's payload within the original logical buffer.
    - TotalLength: 8 bytes (unsigned 64-bit) — total byte length of the original logical buffer.
    The application payload begins immediately after the Fragment Metadata block at offset 42 (26 + 16).
@@
 - Deduplication: Receivers MUST detect and discard datagrams whose SequenceNumber indicates the logical send has already been accepted and processed. Implementations SHOULD maintain a bounded recent-sequence cache keyed by packet tuple and SequenceNumber.
@@
 - Reassembly timeout: Receivers MUST abort and discard incomplete reassembly state for a SequenceNumber if it remains incomplete for more than a configurable timeout (RECOMMENDED default 5000 ms). The receiver MUST record a timeout metric when this occurs.
@@
 - Deduplication cache and session restarts: Because senders may restart and reuse SequenceNumber ranges, implementations MUST adopt logic to avoid false-positive deduplication across distinct logical sessions. Receivers MUST flush or reset deduplication state for a packet tuple when either:
- A discovery `START` datagram is received for the tuple, or
- A connection-id datagram is received for the tuple with a connection-id value different from the current one associated with that tuple.

Implementations SHOULD include the connection-id in the deduplication key when available (i.e., key = (tuple, connection-id, SequenceNumber)). If the connection-id is unknown, fall back to (tuple, SequenceNumber) but follow the flush rules above when a new connection-id or START token is observed.
*** End Patch