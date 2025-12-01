**ctsIoPatternMediaStreamReceiver & ctsIoPatternMediaStreamSender — behavior and sequence**

- **Goal:** explain how `ctsIoPatternMediaStreamReceiver` and `ctsIoPatternMediaStreamSender` work, show UML sequence diagrams (Mermaid), and diagnose why the receiver may not complete when the last frame is received in passive mode (`ctsMediaStreamReceiver.cpp`).

**Overview**:
- `ctsIoPatternMediaStreamSender` and `ctsIoPatternMediaStreamReceiver` implement a UDP media-streaming pattern built on top of the cts IO pattern base classes. They track frames as numbered datagrams and use timers on the receiver-side to "render" frames after an initial buffering period.
- The base class `ctsIoPattern` and `ctsIoPatternState` track overall IO progress and completion. For UDP the state machine treats the pattern as completed when the accumulated bytes (confirmed + in-flight) equals the configured transfer size (`m_maxTransfer`). When the derived pattern signals `ctsIoPatternError::SuccessfullyCompleted` from `CompleteTaskBackToPattern`, the base will update the last error to `NO_ERROR` and call `EndStatistics()`.

**Key pieces in the code (short pointers)**
- `ctsIoPatternMediaStreamReceiver::CompleteTaskBackToPattern` (ctsIOPatternMediaStream.cpp)
  - Validates START/connection-id messages
  - Verifies data frames, tracks sequence numbers into `m_frameEntries`
  - When it sees the `receivedSequenceNumber == m_finalFrame`, it calls `EndStatistics()` (line with EndStatistics call in file)
  - It increments `m_recvNeeded` after processing each datagram so more receives will be requested.
- `ctsIoPatternMediaStreamReceiver::TimerCallback` (renderer)
  - Drives rendering frames at `m_frameRateMsPerFrame` once the initial buffer is filled.
  - When it has rendered the last frame (head sequence number > m_finalFrame) it sets `m_finishedStream = true` and sends `ctsTaskAction::Abort` to the callback to trigger connection close.
- `ctsIoPatternMediaStreamSender::GetNextTaskFromPattern` and `CompleteTaskBackToPattern`
  - Sender schedules sends with `m_baseTimeMilliseconds` offsets and increments `m_currentFrame` when a frame completes.
- `ctsIoPatternState::CompletedTask` (ctsIOPatternState.hpp)
  - For UDP: if `alreadyTransferred == m_maxTransfer` it returns `ctsIoPatternError::SuccessfullyCompleted`.
  - For UDP the base class `CompleteIo` will treat `SuccessfullyCompleted` from the pattern as an indication to update last pattern error to `NO_ERROR`.

**Why the receiver may not complete when the final frame is received in passive mode**

There are two layers to completion here: "pattern-level completion" (the cts base `ctsIoPatternState`) and the higher-level media-stream bookkeeping in the derived `ctsIoPatternMediaStreamReceiver`.

1) For UDP the base `ctsIoPatternState::CompletedTask` considers only bytes transferred (confirmed + in-flight) vs `m_maxTransfer`. It expects the derived pattern to request IO such that when all expected bytes were transferred, the base will get the numeric equality and treat it as SuccessfullyCompleted.

2) `ctsIoPatternMediaStreamReceiver` is a receiver which posts `Recv` tasks. Its `CompleteTaskBackToPattern` increments `m_recvNeeded` after processing a datagram; `GetNextTaskFromPattern` will return a `Recv` (untracked) as long as `m_recvNeeded > 0` and not all frames have been processed by the render timer.

3) Important: `ctsIoPatternMediaStreamReceiver` calls `EndStatistics()` the moment it receives the datagram that contains the final sequence number (see code). However, calling `EndStatistics()` alone does not drive the cts pattern state machine to completion. For the base `ctsIoPattern` to reach CompletedIo and signal upper layers to close the socket the following conditions must be met:
   - The base's bytes counters (m_confirmedBytes + m_inFlightBytes) must reach `m_maxTransfer` as seen by `ctsIoPatternState::CompletedTask`.
   - The derived pattern must return `ctsIoPatternError::SuccessfullyCompleted` via `CompleteTaskBackToPattern` for the last completed IO so the base can call `UpdateLastPatternError(SuccessfullyCompleted)`.

4) In `ctsIoPatternMediaStreamReceiver::CompleteTaskBackToPattern` the method does not return `ctsIoPatternError::SuccessfullyCompleted` when the last datagram is received; instead it calls `EndStatistics()` and returns `ctsIoPatternError::NoError`. The base counts bytes using `NotifyNextTask`/`CompletedTask` only when tasks are tracked (`m_trackIo` true). The receiver's `CreateUntrackedTask(...)` used by `GetNextTaskFromPattern()` marks the Recv tasks as untracked (m_trackIo = false). Because datagrams are untracked, the base's `m_confirmedBytes` and `m_inFlightBytes` are not adjusted for those receives: the base state therefore never observes the expected bytes transferred and cannot finish the pattern by itself.

5) In non-passive mode when IO is driven by `ctsMediaStreamReceiver::Start` the pattern callback wiring and the `RegisterCallback` path may send a final `Abort` or `FatalAbort` task (see TimerCallback) which closes the socket. In passive mode (`ctsMediaStreamReceiver::Start` early returns) the object never enters the IO loop and the `RegisterCallback` that's used to surface Abort isn't necessarily installed. Passive mode relies on `OnDataReceived` being called from an external path; `OnDataReceived` does call `lockedPattern->InitiateIo()` and `CompleteIo()` but again the receiver tasks are untracked.

6) The immediate consequence is: if all datagrams are received in Datagram untracked mode, the base pattern's byte counters never reach the `m_maxTransfer` threshold and the base never returns `ctsIoStatus::CompletedIo`. Calling `EndStatistics()` alone on the derived object only ends the timing window but does not change the pattern state in the base. The `ctsIoPatternMediaStreamReceiver::TimerCallback` calls `Abort` once it detects processing all frames (head past final), but that is only scheduled if the timer runs and the receiver's timers are started (the code schedules timers only if the initial buffer fills and base_time_milliseconds is set by the first InitiateIo). In passive mode timers may not be started the same way.

Summary of likely root causes:
- The receiver uses untracked Recv tasks so the base `ctsIoPatternState` is not notified of bytes transferred and cannot reach SuccessfullyCompleted based on byte counts.
- `EndStatistics()` called in the derived class is insufficient — the derived pattern must either return SuccessfullyCompleted from `CompleteTaskBackToPattern` at the right time or post a final tracked task or signal an `Abort`/`FatalAbort` that the IO loop in `ctsMediaStreamReceiver` will treat as completion.
- In passive mode `ctsMediaStreamReceiver::Start` is not called, so the receiver never registers the callback or runs the IO loop that would normally run timers and post abort tasks.

Practical recommendations to fix or observe the behavior
- Option A (recommended minimal change): When the receiver detects the final frame (receivedSequenceNumber == m_finalFrame), return `ctsIoPatternError::SuccessfullyCompleted` from `CompleteTaskBackToPattern` for that completion — or convert that specific recv task to a tracked task so the base counts it. This ensures the base state machine will see the final bytes and return CompletedIo.

- Option B: Keep Recv untracked but post a final `ctsTask` with `m_ioAction = ctsTaskAction::Abort` (or `FatalAbort`) via `SendTaskToCallback` so the IO loop will close the socket. The receiver already does this path in `TimerCallback` when the renderer finishes, but in passive mode the timers or registration may not be active. Ensuring timers are started even in passive mode or ensuring `OnDataReceived` can schedule the Abort when it sees final frame would help.

- Option C: Start the media-stream receiver timers and RegisterCallback even in passiveReceive mode so the timer-driven rendering and final abort path still runs. This might be the least invasive if the design intended passive mode to still do render/timers.

Mermaid sequence diagrams

- Normal active run (sender -> receiver, receiver renders frames -> causes Abort when final frame is processed):

sequenceDiagram
    participant Sender as ctsIoPatternMediaStreamSender
    participant UDP as UDP Socket
    participant Receiver as ctsIoPatternMediaStreamReceiver
    participant Base as ctsIoPattern
    Sender->>UDP: send datagram(seq N)
    UDP->>Receiver: WSARecvFrom completion
    Receiver->>Base: InitiateIo/CompleteIo (untracked recv)
    Receiver->>Receiver: Store frame in m_frameEntries
    alt receivedSequence == m_finalFrame
        Receiver->>Receiver: EndStatistics()
        Receiver->>Receiver: (Timer later) RenderFrame() until head > final
        Receiver->>Base: Send Abort via callback (ctsTaskAction::Abort)
        Base->>Socket: Close and CompletedIo
    end

- Passive mode via OnDataReceived (no Start):

sequenceDiagram
    participant Remote as RemoteSender
    participant App as Host app invoking OnDataReceived
    participant Receiver as ctsIoPatternMediaStreamReceiver
    participant Base as ctsIoPattern
    Remote->>App: UDP datagram arrives
    App->>Receiver: OnDataReceived(buffer)
    Receiver->>Base: InitiateIo() (returns Recv task untracked)
    Receiver->>Base: CompleteIo(...) // CompleteTaskBackToPattern returns NoError
    alt last frame detected
        Receiver->>Receiver: EndStatistics()
        note right of Receiver: No tracked IO change -> Base still thinks Not Completed
        Receiver->>App: no Abort posted => socket stays open
    end

What to inspect when debugging runtime
- Check whether `ctsMediaStreamReceiver::Start()` was called and timers created. If passiveReceive is true, `Start()` returns early. That disables the RegisterCallback and initial IO-loop that normally wires up timers and may post the Abort.
- Log when `CompleteTaskBackToPattern` sees the final sequence number and whether it returns SuccessfullyCompleted or NoError.
- Log base `ctsIoPatternState` counters (`m_confirmedBytes`, `m_inFlightBytes`) — or inspect `ctsIoPattern::CompleteIo` path to see whether the base observed the bytes.

Concrete code pointers where change helps
- In `ctsIoPatternMediaStreamReceiver::CompleteTaskBackToPattern` (ctsIOPatternMediaStream.cpp near the code that calls `EndStatistics()`), consider setting the return value to `ctsIoPatternError::SuccessfullyCompleted` when the datagram contains the final sequence number. This will propagate to `ctsIoPattern::CompleteIo` and allow the base to update its pattern error and close the socket.

Alternately, ensure passive mode still registers the callback and/or starts the timers so that the existing `TimerCallback` path issues an `Abort` and closes the socket.

Appendix — Suggested snippet (conceptual)
- Option (make final recv return SuccessfullyCompleted):

```cpp
// inside CompleteTaskBackToPattern when (receivedSequenceNumber == m_finalFrame)
EndStatistics();
return ctsIoPatternError::SuccessfullyCompleted;
```

- Option (post Abort in OnDataReceived when final frame is observed and no timer):

```cpp
if (receivedSequenceNumber == m_finalFrame)
{
    ctsTask abortTask; abortTask.m_ioAction = ctsTaskAction::Abort; SendTaskToCallback(abortTask);
}
```


---

Document prepared by GitHub Copilot (assistant). If you want, I can open a patch and add a short comment in the code with the recommended fix or create a small PR with the change. 
