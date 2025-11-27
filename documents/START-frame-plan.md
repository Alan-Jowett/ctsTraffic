# TCP-style 3-Way Handshake Plan for Media Stream (Initiator-init)


This document describes how to change the media stream protocol from the existing single-frame (one-way) `START` handshake into a TCP-style 3-way handshake initiated by the initiator. The plan includes the handshake sequence, suggested frame formats, state machines, tests, and deployment notes.

Note: this document uses the terms `initiator` and `responder` for the two roles. In the current codebase the *receiver* plays the `initiator` role and the *sender* plays the `responder` role; that mapping is called out where filenames are referenced. The mapping may be changed in future refactors.

## Goals

- Implement a robust 3-way handshake so the receiver reliably confirms readiness before the sender transmits media.
- Add tests, logging, and monitoring to ensure correctness and observability.

## Handshake overview

 - Step 1 (SYN / START): Initiator -> Responder
   - The initiator sends a `START` (SYN) frame to indicate it is ready. The session is identified implicitly by the source/destination IP:port tuple for the packet flow.
 - Step 2 (SYN-ACK / START_ACK): Responder -> Initiator
   - The responder validates the packet source/destination tuple, reserves necessary resources, and responds with `START_ACK` to the same tuple.
 - Step 3 (ACK): Initiator -> Responder
   - The initiator acknowledges `START_ACK` with `START_CONFIRM` (ACK). After this, the responder begins streaming media.

Notes:
- The naming `START`, `START_ACK`, `START_CONFIRM` is suggested and can be mapped to existing frame enums.
- `START_ACK` can include a flag indicating whether the sender will begin immediately or wait for `START_CONFIRM` — use this to support faster modes if desired.

## Suggested frame fields

- Common fields for each control frame:
  - `FrameType` (enum): `START`, `START_ACK`, `START_CONFIRM`, `ERROR`.
  - `Version`: protocol version for negotiation.
  - `Flags`: bitflags for options (e.g., `ACK_REQUIRED`).
  - `Payload`: optional structured TLV for extension data; stream parameters are negotiated out-of-band and are not part of this control handshake.

## Code locations to inspect and update

- `ctsTraffic/ctsMediaStreamProtocol.*` — frame definitions and serialization routines.
 - `ctsTraffic/ctsMediaStreamReceiver.*` — (current initiator) construct and send `START` and `START_CONFIRM`.
 - `ctsTraffic/ctsMediaStreamSender.*` — (current responder) receive `START`, send `START_ACK`, and begin streaming after `START_CONFIRM`.
- `ctsTraffic/ctsMediaStreamServerListeningSocket.*` — any server glue that wires frames to sockets.
- `ctsTraffic/ctsMediaStream*.h` — state enums and shared data structures.

Search for existing one-way START handling (strings: `START`, `Start`, `FrameType`, `StreamStart`) to find the exact current locations.

## Step-by-step implementation plan

1) Review existing 1-way START implementation
  - Locate the code that builds and sends the current `START` frame and where the counterpart consumes it.
  - Identify any implicit assumptions (responder immediately streams on receiving START, no ACK expected).

2) Define new frame types and wire serialization
   - Add `START_ACK` and `START_CONFIRM` to the `FrameType` enum in `ctsMediaStreamProtocol`.
   - Define TLV or fixed-field layout for the frames and implement (de)serialization functions.

3) Extend state machines
  - Responder states: `Idle` -> `PendingStartRequest` -> `PendingConfirm` -> `Streaming` -> `Closed`.
  - Initiator states: `ReadyToStart` -> `AwaitingStartAck` -> `AwaitingStartConfirm` (if needed) -> `Streaming` -> `Closed`.

4) Implement responder behavior
  - On `START`:
    - Validate `Version` and verify the packet's source/destination IP:port tuple identifies a valid session.
    - Allocate/reserve resources (buffers, encoders, scheduling) as needed.
    - Reply with `START_ACK` to the same source/destination tuple.
    - Move to `PendingConfirm` state.
  - On `START_CONFIRM`:
    - Verify the packet arrived on the expected tuple and transition to `Streaming`.
    - Begin normal media sends.
  - On resource failures, reply with `ERROR` frame and go to `Closed`.

5) Implement initiator behavior
  - When ready, build `START` with `StreamId` and send it.
  - Move to `AwaitingStartAck`; start timeout/retry timer.
  - On `START_ACK` from responder:
    - Verify the echoed `StreamId` matches.
    - Send `START_CONFIRM` (ACK) and move to `Streaming`.
  - If `START_ACK` not received within retry limits, mark error and abort.


6) Timeouts, retries, and error codes
  - Define reasonable timeouts (e.g., 500ms - 2s) and exponential backoff for `START` retries.
  - Define retry counts (e.g., 3 attempts) and clear error codes for `ERROR` frames.

8) Tests
  - Unit tests for frame (de)serialization and edge cases.
  - Responder unit tests for state transitions on `START`, `START_ACK`, `START_CONFIRM` and on error frames.
  - Initiator unit tests for retry logic and failure modes.
  - Integration tests: two-process tests where an initiator initiates START and a responder accepts.

9) Instrumentation
   - Add logging at state transitions and for frame send/receive events.
   - Counters/metrics for attempts, successes, failures, time-to-stream.

10) Deployment notes
  - Run unit and integration tests locally and in CI.
  - Deploy the changes and monitor logs/metrics for handshake success rates and time-to-stream.
  - Roll out progressively if desired, monitoring for anomalies.

## Notes on compatibility

This plan intentionally does not include backward-compatibility strategies. Implementations should assume both endpoints understand the new 3-way handshake.

## Example pseudo-code

Initiator:

```
start_stream():
  sid = generateStreamId()
  start = build_frame(START, {version, sid, flags})
  send(start)
  state = AwaitingStartAck
  wait(timeout)
  if receive(START_ACK):
    verify_echoed_sid()
    send(build_frame(START_CONFIRM, {sid}))
    state = Streaming
  else if retries_exhausted:
    fail()
```

Responder:

```
on_receive(frame):
  if frame.type == START:
    if validate_and_can_allocate():
      reserve_resources()
      send(build_frame(START_ACK, {version, sid=frame.sid}))
      state = PendingConfirm
    else:
      send(build_frame(ERROR, {code}))
  else if frame.type == START_CONFIRM and state == PendingConfirm:
    if match_sid(frame):
      state = Streaming
      begin_media_send()
```

## Manual test checklist

- Build and run an initiator and a responder locally.
- Confirm via logs or packet capture the 3-way handshake sequence: `START` -> `START_ACK` -> `START_CONFIRM`.
- Verify the responder does not send media before `START_CONFIRM`.

- Test negative cases: malformed `START`, `START_ACK` with mismatched `StreamId`, and ensure proper `ERROR` frames and teardown.

- Verify behavior under network loss, duplicate frames, and corrupted frames.

## Acceptance criteria

- The 3-way handshake completes reliably in normal network conditions.
- Responder does not transmit media before receiving `START_CONFIRM`.
- Tests cover positive and negative scenarios.

---

If you'd like, I can now scan the repository to find the exact files and functions to update, then prepare a concrete patch that adds the new frame types and skeleton state-machine changes behind a feature flag.
