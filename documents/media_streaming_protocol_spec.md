
Media Streaming Protocol — Conformance Specification

Status: Informational

Abstract

This document specifies a minimal, precise, and implementation-independent protocol for discovery and media delivery over UDP datagrams. It prescribes on-wire datagram layouts, parsing and validation rules, sender and receiver behaviors, fragmentation semantics, timing fields, and error handling that an implementation MUST follow to be conformant. The specification intentionally avoids platform or source-code references.

1. Terminology

- Initiator: the endpoint that requests a stream from a Responder (typically the receiver).
- Responder: the endpoint that supplies media data to an Initiator (typically the sender).
- Datagram: a single UDP packet delivered as an atomic unit.
- Packet tuple: (source IP, source port, destination IP, destination port) used to identify a logical session.
- Sequence Number: unsigned 64-bit integer carried in the header and used for ordering and deduplication.
- QPC: unsigned 64-bit sender timestamp captured from a high-resolution clock at send time.
- QPF: unsigned 64-bit frequency value representing clock ticks per second for the clock used to capture QPC.
- Headered Datagram: a datagram that begins with the fixed protocol header defined in section 3 followed by application payload bytes.
- Discovery Datagram: a 5-byte UDP payload containing the ASCII token "START" used to request a stream.

2. Design Goals

- Simplicity: minimal on-wire state for discovery, simple header layout for timing and ordering.
- Interoperability: precise byte offsets and field sizes to allow independent implementations to interoperate.
- Defensive parsing: clear validation rules preventing ambiguous parsing between discovery and headered datagrams.
- Stateless discovery: discovery is one-way and does not require an acknowledgement to begin streaming.

3. Byte Order and Encoding

All multi-byte integer fields in this protocol MUST be transmitted using little-endian byte order. Implementations MUST parse and generate fields using little-endian encoding; no alternate endianness is permitted for interoperable implementations.

4. Datagram Types

An implementation MUST recognize the following datagram forms and disambiguate them exactly as specified:

- Discovery Datagram: payload length == 5 bytes and payload equals ASCII "START" (0x53 0x54 0x41 0x52 0x54). This form contains no protocol header and is strictly the five bytes only.
- Headered Datagram (standard data): begins with the 26-byte protocol header (section 5) followed by payload bytes.
- Connection-id Datagram: a control datagram consisting of a 2-byte ProtocolFlag value (0x1000) followed immediately by a NUL-terminated connection identifier string (37 bytes canonical). This form is NOT the same as a headered datagram and does NOT include the 26-byte data header. See section 7 for exact rules.

Implementations MUST first test for discovery datagrams (exact-length and exact-content) and the connection-id minimal form before attempting to parse the 26-byte headered format. Order of checks is: discovery (5 bytes), connection-id (>=39 bytes with ProtocolFlag==0x1000), then headered datagram (>=26 bytes with ProtocolFlag==0x0000 or ProtocolFlag==0x0001).


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
  - 0x0000 — Standard data/control datagram with no fragmentation metadata.
  - 0x0001 — Standard data/control datagram with the Fragmentation flag set; a 16-byte Fragment Metadata block MUST follow the fixed header.

  Implementations MUST reject headered datagrams whose ProtocolFlag is any value other than 0x0000 or 0x0001 and MUST record a validation error when such datagrams are received.

- SequenceNumber (8 bytes): unsigned 64-bit integer assigned once per logical send. All fragments produced while partitioning the logical buffer MUST use the same SequenceNumber.

- QPC (8 bytes): unsigned 64-bit clock sample captured by the sender at or immediately prior to transmit of the datagram. Interpreted with QPF.

- QPF (8 bytes): unsigned 64-bit ticks-per-second value to convert QPC into elapsed time.


6. Sequence Assignment and Fragmentation Semantics

- Sequence number granularity: The sender MUST allocate a single SequenceNumber for each logical send (logical buffer). All fragments produced while partitioning that logical buffer MUST carry the same SequenceNumber.

- Fragment composition (extension): To enable unambiguous reassembly, this specification defines an optional per-fragment metadata block that immediately follows the 26-byte header when the sender sets the Fragmentation flag described below. The base header alone does not contain fragment positioning metadata; therefore, senders MUST choose one of the following strategies and advertise/implement it consistently:
  - Inline Fragment Metadata (recommended, normative): The sender MUST set bit 0x0001 in the ProtocolFlag (i.e., ProtocolFlag & 0x0001 != 0) to indicate that a 16-byte Fragment Metadata block follows the fixed 26-byte header. The metadata block layout (little-endian) is:
    - FragmentOffset: 8 bytes (unsigned 64-bit) — byte offset of this fragment's payload within the original logical buffer.
    - TotalLength: 8 bytes (unsigned 64-bit) — total byte length of the original logical buffer.
    The application payload begins immediately after the Fragment Metadata block at offset 42 (26 + 16).

  - Out-of-Band Reassembly (alternate): If a sender does NOT set the fragmentation flag (bit 0x0001 is clear), the sender MUST ensure each logical buffer fits within a single datagram and therefore no reassembly is necessary. Implementations that choose this strategy MUST document the maximum logical buffer size they support (see section 12 for limits).

- Header per fragment: Every fragment sent as a headered datagram MUST include the full 26-byte header. If the fragmentation flag is set, the 16-byte Fragment Metadata block MUST immediately follow the header and precede the fragment payload.

- QPC/QPF and fragments: QPC is captured per-fragment and MAY differ between fragments that share a SequenceNumber. QPF MAY remain constant for the logical send.

 - Logical timestamp selection (normative): To avoid ambiguity and inconsistent playback jitter, the sender SHOULD populate QPC in each fragment with a per-fragment capture timestamp but the receiver MUST use the QPC value from the fragment whose FragmentOffset == 0 (the fragment that contains the first byte of the logical buffer) as the canonical "logical timestamp" for the reassembled logical buffer. If the sender elects not to include fragmentation metadata (i.e., each logical send fits in one datagram), the receiver uses the only fragment's QPC as the logical timestamp.

- Minimum payload per fragment: The sender MUST ensure no transmitted fragment contains zero payload bytes. When fragmentation metadata is included, the sender MUST account for the additional 16 bytes when calculating fragment payload sizes.

- Logical send size restriction: A logical send whose total size is less than or equal to the fixed header length (or header plus fragment metadata when the fragmentation flag is set) MUST be rejected by the sender.


7. Connection-id Datagram (control)

- Format: The connection-id datagram is a minimal control datagram and is NOT a headered data datagram. Its on-wire layout is:
  - ProtocolFlag: 2 bytes (value 0x1000) at offset 0
  - ConnectionIdString: 37 bytes at offsets 2..38 inclusive — canonical form is a 36-character UUID ASCII string followed by a NUL (0x00) terminator.

- Minimum length: Receivers MUST validate that a connection-id datagram has length >= 39 bytes (2 + 37) before attempting extraction.

- Extraction: Receivers MUST copy the first 37 bytes immediately following the ProtocolFlag and treat them as the NUL-terminated connection id string. Any trailing bytes beyond the 37-byte canonical field MAY be ignored.

- Semantics/Purpose: Connection-id datagrams provide an explicit, minimal control mechanism to associate a packet tuple with an opaque connection identifier used for correlation, logging, or out-of-band telemetry. Typical approved uses:
  - Responder sends a connection-id to the Initiator shortly after accepting a discovery request to provide a stable identifier for the stream.
  - Either endpoint may send a connection-id as a control message to update or reassign the identifier for a tuple when practical.
  - Receivers use the connection id to correlate metrics, logs, and to map incoming packets to higher-level session state when tuple- or port-based correlation is insufficient.

Implementations MUST NOT assume connection-id datagrams imply the presence of a 26-byte header or of sequence/telemetry fields; the two forms are disjoint by design.

 - Validation and treatment: Receivers MUST treat the connection-id field as an opaque identifier for correlation. Receivers MAY perform optional syntax validation (for example, validate the 36-byte portion matches conventional UUID textual syntax with hyphens) for diagnostics or policy enforcement, but MUST NOT reject or drop the datagram solely because the identifier does not parse as a UUID. Implementations MUST accept any 37-byte NUL-terminated string as a valid connection id for mapping and correlation purposes.

 - Session semantics: A change in the observed connection-id for a packet tuple SHOULD be interpreted as a session re-identification for that tuple. Receivers MUST treat different connection-id values as distinct logical sessions even when the 4-tuple is identical.

 - Security note: Connection-id values are supplied by remote endpoints and can be adversarially controlled. Receivers MUST NOT treat connection-id values as secrets or use them for access control decisions.


8. Discovery Procedure

- Initiator behavior: To request a stream, the Initiator MUST send a single UDP datagram whose entire payload is the ASCII string "START" (5 bytes). The discovery datagram MUST contain no header and MUST be exactly five bytes long.

- Responder behavior: Upon receipt of a datagram whose length equals exactly five bytes and contents equal the ASCII string "START", the Responder MUST treat the packet tuple as a request to start a stream. The Responder MAY allocate resources and, if resources are available, begin transmitting headered datagrams to the Initiator's tuple.

- Retransmission: The Initiator MAY retransmit the discovery datagram periodically while waiting for media; retransmissions are identical 5-byte payloads.

- Disambiguation priority: Implementations MUST test for discovery datagrams first (exact-length + exact-content) to avoid ambiguous parsing.


9. Packet Reception and Validation

When a receiver receives a UDP datagram it MUST perform these checks in order:

1. If datagram length == 5, compare payload to ASCII "START". If equal, process as discovery (section 8) and stop further parsing.
2. If datagram length < 2, drop and record a validation error.
3. Read the 2-byte ProtocolFlag at offset 0 (little-endian).
  - If ProtocolFlag == 0x1000 (connection-id): verify datagram length >= 39; if shorter, drop and record a validation error. Otherwise extract the first 37 bytes after the ProtocolFlag as the NUL-terminated connection id string and process it (section 7).
  - Else if ProtocolFlag == 0x0000 or ProtocolFlag == 0x0001 (i.e., valid headered data frames with optional fragmentation flag): verify datagram length >= 26; if shorter, drop and record a validation error. Then parse the header fields as below.
  - Otherwise: treat ProtocolFlag as unknown, drop datagram and record a validation error.
4. For headered datagrams: parse SequenceNumber (offset 2), QPC (offset 10), QPF (offset 18) as unsigned little-endian integers. Determine whether the fragmentation flag is set (ProtocolFlag bit 0x0001). If set, verify datagram length >= 42 (26 + 16) and parse the 16-byte Fragment Metadata block: FragmentOffset (8 bytes) and TotalLength (8 bytes). The application payload begins at offset 26 (no-fragmentation) or 42 (with fragment metadata) and has length (datagram length − payload_offset).
5. All parsed fields MUST be validated as untrusted input before use.

Receivers MUST not rely on any implicit bufferOffset; all offsets in this specification are absolute within the datagram payload as received from the transport.


10. Ordering, Deduplication, and Reassembly

- Ordering: Receivers MUST use SequenceNumber to place logical sends in order. SequenceNumber identifies a logical send (which may be carried by one or more fragments). Implementations MAY use a bounded reordering buffer or sliding window to tolerate out-of-order delivery.

- Deduplication: Receivers MUST detect and discard datagrams whose SequenceNumber indicates the logical send has already been accepted and processed. Implementations SHOULD maintain a bounded recent-sequence cache keyed by packet tuple and SequenceNumber.

- Reassembly (normative when fragmentation metadata used): When the sender indicates fragmentation (ProtocolFlag bit 0x0001), each fragment includes FragmentOffset and TotalLength. The receiver MUST:
  - Allocate or map a reassembly buffer sized to TotalLength (subject to buffer limits, section 12).
  - Place the fragment payload at FragmentOffset within the reassembly buffer.
  - Mark the byte-range [FragmentOffset, FragmentOffset + fragment_payload_length) as received.
  - When the union of received ranges equals TotalLength, the logical buffer is complete; the receiver MUST then present the reassembled logical buffer to the application and release reassembly state for the SequenceNumber.
  - If overlapping fragments are received, the receiver MUST accept the first-arriving bytes for each position and ignore overwrites from later duplicates for deduplication purposes.

- Reassembly without fragmentation metadata: If the sender elects the Out-of-Band Reassembly strategy (i.e., it does not set the fragmentation flag and ensures logical sends are single-datagram), the receiver MUST treat each headered datagram as a complete logical send and no reassembly is required.

- Missing fragment handling: The receiver MUST implement a reassembly timeout (section 12). If a logical send is incomplete when the timeout expires, the receiver MUST discard partial reassembly state for that SequenceNumber and record a validation/timeout metric.


11. Timing Fields and Interpretation

- QPC and QPF are diagnostic fields for telemetry and timing calculations. QPC is an unsigned 64-bit clock sample captured by the sender at transmit time; QPF is an unsigned 64-bit ticks-per-second value for that clock.

- Receivers MUST convert QPC to elapsed time using QPF as the divisor. Example for milliseconds: elapsed_ms = (QPC * 1000) / QPF. Implementations MUST use integer arithmetic that accounts for 64-bit values and avoid overflow by applying appropriate scaling.

- QPC/QPF are NOT security mechanisms and MUST NOT be used for authentication or replay protection.

- Implementations MUST handle wraparound per unsigned 64-bit arithmetic semantics; however, wraparound is expected to be rare in practical runtimes.


12. Limits, Timeouts, Error Handling and Telemetry

- Maximum datagram size: The on-wire maximum UDP payload an implementation MUST accept is 65507 bytes (IPv4/IPv6 maximums accounting for headers). Senders MUST NOT transmit datagrams with a payload larger than 65507 bytes. Implementations SHOULD use a conservative default maximum transmit payload (for example 1200 bytes) unless Path MTU discovery has validated larger payloads are safe.

 - Fragmentation payload calculation: When the fragmentation metadata block is used, the sender MUST compute the per-fragment application payload capacity as:

   FragmentPayloadSize = MTU - IP_Overhead - UDP_Overhead - HeaderLength - FragMetadataLength

   Where:
   - `MTU` is the local path MTU in bytes for the link (RECOMMENDED default 1500 bytes on Ethernet).
   - `IP_Overhead` is the IP header size (commonly 20 bytes for IPv4 without options; 40 bytes for IPv6 without extension headers).
   - `UDP_Overhead` is 8 bytes.
   - `HeaderLength` is 26 bytes (protocol header length).
   - `FragMetadataLength` is 16 bytes when fragmentation metadata is present, otherwise 0.

   Example (IPv4 conservative default): for MTU=1500, IP_Overhead=20, UDP_Overhead=8, HeaderLength=26, FragMetadataLength=16:

   FragmentPayloadSize = 1500 - 20 - 8 - 26 - 16 = 1430 bytes

   Implementations should take care to compute the fragment payload size using the actual MTU and header sizes on the path when possible.

- Reassembly buffer limits: To prevent resource exhaustion, receivers MUST enforce a per-sequence maximum reassembly buffer size (RECOMMENDED default 16 MiB) and a global maximum concurrent reassembly memory budget (RECOMMENDED default 64 MiB). If allocating a reassembly buffer would exceed these limits, the receiver MUST reject the reassembly attempt and record a resource-exhaustion metric.

- Reassembly timeout: Receivers MUST abort and discard incomplete reassembly state for a SequenceNumber if it remains incomplete for more than a configurable timeout (RECOMMENDED default 5000 ms). The receiver MUST record a timeout metric when this occurs.

- Unknown or invalid ProtocolFlag: Drop datagram and increment validation error metrics.

- Short header or malformed datagram: Drop datagram, increment validation error metric, and do not attempt reassembly.

- Duplicate datagrams: Detect by SequenceNumber and tuple and increment duplicate metric when duplicates are observed. Duplicate fragments that do not provide new byte ranges are ignored.

 - Deduplication cache and session restarts: Because senders may restart and reuse SequenceNumber ranges, implementations MUST adopt logic to avoid false-positive deduplication across distinct logical sessions. Receivers MUST flush or reset deduplication state for a packet tuple when either:
   - A discovery `START` datagram is received for the tuple, or
   - A connection-id datagram is received for the tuple with a connection-id value different from the current one associated with that tuple.

   Implementations SHOULD include the connection-id in the deduplication key when available (i.e., key = (tuple, connection-id, SequenceNumber)). If the connection-id is unknown, fall back to (tuple, SequenceNumber) but follow the flush rules above when a new connection-id or START token is observed.

- Telemetry: Implementations SHOULD record metrics for received bytes, received datagrams, validation errors, duplicate frames, sequence gaps, reassembly timeouts, and resource rejections.


13. Security Considerations

- This protocol provides no confidentiality, integrity, or replay protection. Implementations that require those properties MUST layer appropriate transport- or application-layer protections (e.g., DTLS, IPsec, or application-level authenticated encryption).

- Receivers MUST validate all input sizes and bounds before using them to index memory or allocate resources and MUST enforce reassembly limits (section 12) to avoid denial-of-service via large or fragmented logical sends.


14. Configuration Parameters (Local Only)

The following settings are local to each implementation and are NOT carried or negotiated on the wire:

- `bitspersecond`: target transmit rate in bits per second.
- `framerate`: logical frames produced per second.
- `streamlength`: duration in seconds for a stream.

Senders use these parameters to determine frame sizes and pacing. Tests and interoperable deployments MUST coordinate these parameters out-of-band.


15. Interoperability Recommendations

- This specification mandates little-endian encoding for all multi-byte fields to avoid coordination problems. Implementations MUST comply.

- To reduce fragmentation overhead and improve robustness across diverse networks, prefer conservative default datagram sizes (e.g., 1200 bytes) and use Path MTU discovery before sending larger payloads.

- When extending this protocol, use reserved ProtocolFlag values and document extensions clearly; deploy only when both endpoints agree to the extension.


16. Examples

- Discovery: Initiator sends a single UDP datagram with payload bytes [0x53,0x54,0x41,0x52,0x54] (ASCII "START"). Responder validates length == 5 and exact content match, then may begin streaming.

- Connection-id: Responder sends ProtocolFlag 0x1000 followed by a 37-byte connection id: [0x00 0x10] + ASCII UUID + 0x00. Receiver extracts the 37 bytes after the flag as the connection id string.

- Headered datagram (no fragmentation metadata):
  - ProtocolFlag (0x0000) => bytes 00 00
  - SequenceNumber (0x0000000000000005) => bytes 05 00 00 00 00 00 00 00
  - QPC (example) => eight little-endian bytes
  - QPF (example) => eight little-endian bytes
  - Payload bytes follow at offset 26.

- Headered datagram (with fragmentation metadata):
  - ProtocolFlag with fragmentation bit set (0x0001) => bytes 01 00
  - SequenceNumber => 8 bytes
  - QPC => 8 bytes
  - QPF => 8 bytes
  - FragmentOffset => 8 bytes
  - TotalLength => 8 bytes
  - Payload begins at offset 42.


17. Conformance Tests (suggested)

- Round-trip serialization test: serialize header fields and (when used) fragmentation metadata at specified offsets and verify parsing yields identical values.
- Discovery recognition test: verify datagrams length 5 with payload "START" are treated as discovery; datagrams length 6 beginning with "START" are not.
- Connection-id test: verify datagrams with ProtocolFlag==0x1000 and length >=39 yield correct 37-byte connection id extraction.
- Header length validation test: verify headered datagrams shorter than 26 bytes are rejected.
- Fragmentation and reassembly tests: verify that when the fragmentation flag is set, fragments can be reassembled using FragmentOffset and TotalLength; verify timeouts and buffer limits are enforced.
- Sequence monotonicity: verify sender increments SequenceNumber once per logical send and that receivers observe monotonic increases.

- Logical timestamp test: verify that when a logical buffer is fragmented, the receiver uses the QPC value from the fragment whose FragmentOffset == 0 as the canonical logical timestamp for the reassembled buffer.
- Deduplication flush test: verify the receiver flushes deduplication state for a tuple when it receives a `START` datagram or a connection-id datagram with a different connection-id, allowing a restarted sender that reuses SequenceNumbers to be accepted.


18. Appendix: Minimum Length Summary

- Discovery datagram: exactly 5 bytes (payload == "START").
- Headered datagram: at least 26 bytes (2-byte ProtocolFlag + 8-byte SequenceNumber + 8-byte QPC + 8-byte QPF). If fragmentation metadata is present, add 16 bytes (total >= 42).
- Connection-id datagram: at least 39 bytes (2-byte ProtocolFlag + 37-byte NUL-terminated connection id string).

Authors: protocol specification team
