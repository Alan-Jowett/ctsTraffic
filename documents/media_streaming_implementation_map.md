Media Streaming Protocol — Implementation Mapping

Purpose

This document maps the normative specification `documents/media_streaming_protocol_spec.md` to the current implementation behavior as described in `documents/media_streaming_implementation.md`. For each normative section it records where the implementation implements the behavior (files / helpers / subsystems referenced by the implementation document), whether the implementation conforms, and any important divergences or gaps that an implementer must address to reach full conformance.

How to read this file

- Status: **Conformant** means the implementation implements the spec text for that item.
  **Partial** means the implementation implements part of it or implements it differently but interoperable in common cases.
  **Divergent** means the implementation behavior contradicts the normative text or is incomplete in a way that affects interoperability.
- Locations: references come from `documents/media_streaming_implementation.md` (function and file names referenced there).

Mapping

1. Terminology
- Mapping: terminology used in the spec (Initiator/Responder, SequenceNumber, QPC/QPF) is present conceptually in the implementation document.
- Status: Conformant
- Notes: No divergence.

2. Design Goals
- Mapping: Implementation is designed around the same goals (simple discovery token, headered datagrams, timing telemetry).
- Status: Conformant

3. Byte Order and Encoding (Spec requires little-endian)
- Mapping: Implementation composes header by pointing WSABUF entries to in-memory integer values and transmits host/native endianness (Windows little-endian).
  - Implementation references: `ctsMediaStreamSendRequests` (composer), send-side description in `documents/media_streaming_implementation.md`.
- Status: Partial (conformant on Windows; implementation relies on host/native endianness)
- Divergence: The implementation uses host/native order (little-endian on Windows). The spec mandates little-endian; this matches on Windows but implementers targeting non-Windows platforms must ensure they use little-endian explicitly. The implementation's on-wire behavior will be conformant only on little-endian platforms or if serialization is updated for explicit endianness.

4. Datagram Types (Discovery, Headered, Connection-id)
- Mapping: Implementation recognizes the discovery token "START" and a separate headered datagram form. It also documents connection-id datagram behavior and minimum lengths.
  - Implementation references: server listening path (tuple accept / START detection), connection-id constant `ctsStatistics::ConnectionIdLength` (37), sender asserts 39-byte task buffer.
- Status: Conformant
- Notes: Implementation emits 39-byte connection-id datagrams and receivers accept any length >=39 — this matches the spec's minimal connection-id form.

5. Header Layout (on-wire offsets: ProtocolFlag @0, SequenceNumber @2, QPC @10, QPF @18)
- Mapping: Sender composes header in the order ProtocolFlag, SequenceNumber, QPC, QPF, then payload (WSABUF ordering).
  - Implementation references: `ctsMediaStreamSendRequests` (composer) and send-side notes in the implementation doc.
- Status: Conformant for sender composition
- Divergence (receiver helpers): Some receiver helpers incorrectly read QPC/QPF from the wrong offsets:
  - `GetSequenceNumberFromTask` correctly copies the 8-byte SequenceNumber from `task.m_buffer + task.m_bufferOffset + 2`.
  - `GetQueryPerfCounterFromTask` and `GetQueryPerfFrequencyFromTask` currently copy 8 bytes from the same start location as the sequence helper (offset 2) instead of offsets 10 and 18 respectively — this misreads telemetry values.
  - Higher-level consumers sometimes perform direct pointer-casts reading at `task.m_buffer + 8` and `task.m_buffer + 16` (fixed offsets relative to buffer start) which also do not line up with authoritative offsets when `m_bufferOffset != 0`.
- Action required: Update the QPC/QPF helper implementations to read from offsets 10 and 18 and update legacy consumers to use the helpers or correct offsets; normalize `GetProtocolHeaderFromTask` to respect `m_bufferOffset`.

6. Sequence Assignment and Fragmentation Semantics
- Mapping: Implementation assigns SequenceNumber per logical send and reuses across fragments. The sender partitions logical buffers into fragments and includes the full header per fragment. The iterator enforces that subsequent fragments will have at least one payload byte (no zero-length fragments). The implementation captures QPF once per logical send and captures QPC per-fragment.
  - Implementation references: send iterator behavior, `ctsMediaStreamSendRequests` constructor capturing QPF once, iterator capturing QPC per-fragment.
- Status: Conformant for sequence assignment and fragment header inclusion.
- Divergence: The implementation does NOT include per-fragment metadata (FragmentOffset and TotalLength) in the on-wire layout; reassembly is left to higher-layer logic and the implementation documents do not provide a per-fragment index. The normative spec recommends and defines a 16-byte Fragment Metadata block (and a fragmentation flag) for unambiguous reassembly. Because the current implementation lacks this metadata, reassembly logic must rely on byte-counting or out-of-band knowledge — this is a functional divergence that the spec addressed.
- Action required: Either (A) augment the implementation to add the fragmentation flag + metadata fields as specified, or (B) document explicitly how the current code performs reassembly (if application-layer framing exists) so interoperable implementations can replicate that behavior.

7. Connection-id Datagram (control)
- Mapping: Implementation uses a 37-byte canonical connection-id string and emits exactly 39-byte datagrams. Receivers accept any length >=39 and copy the first 37 bytes after the ProtocolFlag. The implementation's sender asserts 39-byte length.
  - Implementation references: `ctsStatistics::ConnectionIdLength` constant (37) and sender-side constructor assertion for 39 bytes.
- Status: Conformant
- Notes: Implementation and spec agree on minimal form and receiver behavior (accept >=39, copy first 37 bytes). The mapping confirms Option A (minimal control frame) is used in practice.

8. Discovery Procedure
- Mapping: Implementation recognizes discovery datagrams by exact 5-byte match to "START" on the server listening path and treats them as one-shot session requests. The Initiator resends periodically until media arrives.
  - Implementation references: server listening path extractor performing memcmp for "START", initiator resend behavior described in implementation doc.
- Status: Conformant

9. Packet Reception and Validation
- Mapping: Implementation uses `ValidateBufferLengthFromTask` to validate buffer sizes. Client receive path enforces `completedBytes >= 2` and then validates based on ProtocolFlag.
  - Implementation references: `ctsMediaStreamMessage::ValidateBufferLengthFromTask`, `ctsIOPatternMediaStream.cpp` receive logic.
- Status: Partial
- Divergences and issues:
  - Validation ordering on server-listen path recognizes discovery before header parsing — matches spec. The client receive path does not expect discovery tokens (this asymmetry is documented in the implementation doc and acceptable for the implementation's two different code paths).
  - The implementation logs validation failures but the specific validation failure for header-too-short does not increment the global `UdpStatusDetails.m_errorFrames` counter in the current codebase; other validation errors increment metrics. The spec requires receivers record validation errors; implementation logs but may not increment the intended counter — operational divergence in telemetry.
  - Several helpers and consumers do not consistently respect `task.m_bufferOffset` when reading fields (e.g., `GetProtocolHeaderFromTask` reads from `task.m_buffer` directly). This creates fragility if non-zero buffer offsets are ever used.
- Action required: Normalize helpers to always use `task.m_bufferOffset`, and ensure validation error counters are incremented where the spec expects telemetry to record the failure.

10. Ordering, Deduplication, and Reassembly
- Mapping: Implementation recommends using a sliding-window or bounded recent-sequence cache to place datagrams in order and deduplicate. There is no explicit mention in the implementation doc of the deduplication cache flush behavior on START or connection-id change.
  - Implementation references: implementation doc's recommendation for sliding-window or bounded recent-sequence cache.
- Status: Partial / Divergent
- Divergence: The normative spec requires deduplication caches to be flushed on arrival of a `START` datagram or a new connection-id for the tuple to avoid mis-deduping after sender restarts. The implementation document does not specify this behavior; implementers should add a dedupination cache flush on these events and consider including connection-id in the deduplication key.

11. Timing Fields and Interpretation
- Mapping: Implementation writes QPC and QPF raw to the wire (64-bit values) and expects receivers to use QPF to convert QPC ticks to elapsed time. Implementation documents include examples for converting ticks to milliseconds.
  - Implementation references: send-side QPF capture behavior, per-fragment QPC refresh behavior, conversion examples.
- Status: Conformant
- Divergence: As noted above, helper misreads (QPC/QPF copied from wrong offsets) cause incorrect telemetry in some receiver code paths. This is a concrete divergence impacting telemetry correctness (not fundamental protocol semantics).

12. Limits, Timeouts, Error Handling and Telemetry
- Mapping: Implementation documents validation failures, metrics, and local configuration parameters (`bitspersecond`, `framerate`, `streamlength`). It also documents that discovery failure may be dropped without application-level ack.
  - Implementation references: various metrics and configuration documented in the implementation doc.
- Status: Partial
- Divergences / Gaps:
  - Implementation does not document (in the implementation doc) default reassembly buffer limits, global memory budgets, or explicit reassembly timeout defaults — these were added as normative recommendations in the spec and should be configured in the implementation to meet spec conformance.
  - The implementation logs validation errors but does not increment certain telemetry counters consistently for header-validation failures (see section 9 note).
- Action required: Add configuration parameters and enforcement for reassembly limits and timeouts; ensure validation and telemetry counters are incremented per the spec.

13. Security Considerations
- Mapping: Implementation doc notes protocol has no confidentiality or integrity guarantees and recommends applying transport-level protections when needed.
- Status: Conformant

14. Configuration Parameters (Local Only)
- Mapping: Implementation documents `bitspersecond`, `framerate`, `streamlength` as local sender options.
- Status: Conformant

15. Interoperability Recommendations
- Mapping: Implementation uses host-native endianness but on Windows this matches spec; implementation doc also recommends conservative MTU choices to avoid fragmentation.
- Status: Partial
- Divergence: Implementation relies on native endianness rather than explicitly enforcing little-endian in serialization; spec requires little-endian. This is currently acceptable for Windows hosts but may require changes for cross-platform implementations.

16. Examples
- Mapping: Implementation doc provides real implementation notes and examples consistent with the specification (discovery token, header ordering). The implementation doc also highlights real helper mismatches and legacy consumers.
- Status: Informative; differences are already captured above.

17. Conformance Tests (suggested)
- Mapping: Implementation document includes notes that can be used for tests; the spec adds further recommended tests. The implementation repo would benefit from adding automated tests for header serialization, fragmentation/reassembly, and the helper fixes identified above.
- Status: Partial

Summary of Required Fixes to Reach Full Conformance

1. Fix QPC/QPF helper offsets
- Update `GetQueryPerfCounterFromTask` and `GetQueryPerfFrequencyFromTask` to copy from header offsets 10 and 18 respectively (and respect `task.m_bufferOffset`). Replace legacy pointer-cast reads at `task.m_buffer + 8` and `+16` with these helpers.

2. Normalize header helpers to use `task.m_bufferOffset`
- Ensure `GetProtocolHeaderFromTask` and all header accessors use the buffer offset consistently.

3. Implement fragmentation metadata or document out-of-band reassembly
- Preferred: add the fragmentation flag (ProtocolFlag bit 0x0001) and the 16-byte Fragment Metadata block (FragmentOffset, TotalLength) to enable robust, interoperable reassembly.
- Alternative: if keeping the existing design, document precisely how reassembly occurs for interoperable implementations and provide tests that validate reassembly.

4. Deduplication cache behavior
- Add logic to flush/reset deduplication caches for a tuple upon receiving `START` or a new connection-id; include connection-id in dedup key when present.

5. Telemetry and validation counters
- Ensure header-validation failures increment the intended global telemetry counters (e.g., `UdpStatusDetails.m_errorFrames`) where diagnostics expect them.

6. Reassembly resource limits and timeouts
- Add configurable limits (per-sequence and global) and a reassembly timeout (recommended defaults in the spec) to avoid resource exhaustion.

References (from implementation doc)
- `ctsMediaStreamSendRequests` (send composer and QPF capture behavior)
- `GetSequenceNumberFromTask`, `GetQueryPerfCounterFromTask`, `GetQueryPerfFrequencyFromTask` (helpers)
- `GetProtocolHeaderFromTask` (header accessor)
- `ctsMediaStreamMessage::ValidateBufferLengthFromTask` (validation helper)
- `ctsIOPatternMediaStream.cpp` (receive-side consumers and telemetry notes)
- `ctsStatistics::ConnectionIdLength` (connection-id length constant = 37)

Notes for reviewers and maintainers

- This map is derived from the implementation-focused document `documents/media_streaming_implementation.md` and the normative spec `documents/media_streaming_protocol_spec.md` as currently present in the repository.
- The implementation document contains helpful, specific notes about where helper functions and consumer code disagree on offsets; those references were used to identify concrete code locations likely requiring change.
- Where the implementation and spec differ, I annotated specific actions. I can create a branch with targeted patches for the helper fixes and telemetry increments if you'd like — tell me which of the recommended fixes you want automated first and I will prepare patches and tests.

Authors: implementation review team
