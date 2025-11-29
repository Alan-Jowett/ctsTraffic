**CTS MediaStream Interactions**

This file shows a sequence diagram (Mermaid) for the interactions between
`ctsMediaStreamServerListeningSocket`, `ctsMediaStreamServerImpl` and
`ctsMediaStreamServerConnectedSocket`. The goal is to understand these
interactions so we can reduce coupling between them.

```mermaid
sequenceDiagram
    participant Client
    participant Listener as ctsMediaStreamServerListeningSocket
    participant IOCP as ctThreadIocp
    participant Server as ctsMediaStreamServerImpl
    participant Connected as ctsMediaStreamServerConnectedSocket
    participant ctsSocket as ctsSocket
    participant Pattern as IO Pattern (ctsIoPattern)

    %% Client -> Listening socket
    Client->>Listener: UDP datagram (START)
    Note right of Listener: InitiateRecv/RecvCompletion handles overlapped
    Listener->>IOCP: WSARecvFrom completion (OVERLAPPED)
    IOCP-->>Listener: invoke RecvCompletion

    %% ListeningSocket forwards raw packet to server (listener is protocol-agnostic)
    Listener->>Server: OnPacketReceived(listeningSocket, listeningAddr, remoteAddr, buffer, length)
    Note right of Server: Server parses protocol and decides actions (e.g., START)

    %% Server parsing -> decide action
    Server->>Server: parse buffer
    alt Parsed as START
      Server->>Server: Start(socket, listeningAddr, remoteAddr)
    else Other/Ignore
      Server-->>Listener: no action (listener remains agnostic)
    end

    alt Accepting ctsSocket already waiting
      Server->>Connected: new ctsMediaStreamServerConnectedSocket(weakSocket, socket, remoteAddr)
        Server->>ctsSocket: SetLocalSockaddr / SetRemoteSockaddr
        Server->>ctsSocket: CompleteState(NO_ERROR)  %% completes the pending ctsSocket Create
        Server-->>Listener: remove awaiting endpoint
    else No accepting socket available
        Server->>Server: push (socket, remoteAddr) to g_awaitingEndpoints
    end

    %% ctsSocket actively asks to accept later
    ctsSocket->>Server: ctsMediaStreamServerListener(weakSocket) -> AcceptSocket(weakSocket)
    alt awaiting endpoint exists
      Server->>Connected: create ctsMediaStreamServerConnectedSocket(weakSocket, socket, remoteAddr)
        Server->>ctsSocket: CompleteState(NO_ERROR)
    else no awaiting endpoint
        Server->>Server: push weakSocket to g_acceptingSockets
    end

    %% IO scheduling from ctsSocket / IO pattern
    ctsSocket->>Server: ctsMediaStreamServerIo(weakSocket)
    Server->>Pattern: lockedPattern->InitiateIo()  (loop until None)
    Pattern-->>Server: returns ctsTask (Send / Recv etc.)
    Server->>Server: ScheduleIo(weakSocket, task)
    Server->>Connected: find connected socket by remoteAddr and call QueueTask(task)

    Connected->>Connected: schedule or immediate MediaStreamTimerCallback
      Connected->>Connected: invoke PerformIo() (owns send logic)
      Connected->>Connected: WSASendTo(...) (synchronous sends performed inside `PerformIo`)

    Connected->>Pattern: lockedPattern->CompleteIo(bytes, error)
    Pattern-->>Connected: CompleteIo result (ctsIoStatus)
    loop while ContinueIo
        Connected->>Pattern: InitiateIo()
        Pattern-->>Connected: returns next ctsTask
        Connected->>Connected: QueueTask or run immediate send
    end

    alt CompletedIo
        Connected->>ctsSocket: CompleteState(errorCode)  %% marks socket done -> closes
        ctsSocket->>Server: ctsMediaStreamServerClose(weak_ptr) -> RemoveSocket(remoteAddr)
        Server->>Server: erase connected socket from g_connectedSockets
    else FailedIo
        Connected->>ctsSocket: CompleteState(errorCode)
        ctsSocket->>Server: ctsMediaStreamServerClose -> RemoveSocket
    end

```

**Notes / observations**
- **Who owns what:** `ctsMediaStreamServerImpl` manages global vectors:
    - `g_listeningSockets`, `g_connectedSockets`, `g_acceptingSockets`, `g_awaitingEndpoints`.
    - `ctsMediaStreamServerListeningSocket` performs low-level recv and forwards raw packets
      to the server via a callback (`m_packetCallback`). The server implements
      `OnPacketReceived(...)` which parses packets and invokes `Start(...)` for START messages.
    - `g_connectedSockets` is now an `std::unordered_map` keyed by `ctl::ctSockaddr` for O(1)
      lookups instead of a vector scan.
  - `ctsMediaStreamServerConnectedSocket` owns per-connection timers and scheduling.

**Direct coupling points (after refactor / remaining):**
- The listening socket is protocol-agnostic and forwards raw datagrams via an injected
  callback (`m_packetCallback`) — parsing and decision logic now live in `Server::OnPacketReceived`.
- The connected socket no longer depends on a server-supplied `ConnectedSocketIo` functor —
  its send logic is self-contained in `ctsMediaStreamServerConnectedSocket::PerformIo()`.
- `ServerImpl` still holds and mutates the global connection containers (now including an
  unordered_map for `g_connectedSockets`) protected by a single lock, making the server
  the central coordinator and potential contention hotspot.

**Suggestions to reduce coupling**
- Introduce interfaces/events: the current change injects a callback; consider defining a small
  interface (e.g., `IListenerHandler::OnPacketReceived(...)`) if you prefer compile-time
  decoupling over `std::function`.
- Replace global vectors with an injected ConnectionManager (interface) to avoid static globals
  and allow unit-testable, pluggable implementations.
 - Use a factory or builder to create `ConnectedSocket` instances (inject `IConnectedSocketFactory`),
   so `ServerImpl` doesn't `new` the concrete class directly.
 - Limit lock scope and split responsibilities: use per-connection locks or concurrent maps
   for `g_connectedSockets` and `g_awaitingEndpoints` to reduce contention and simplify reasoning.
 - If desired, replace `PerformIo` with an injected `ISender` interface later to allow pluggable
   send strategies; current change keeps the send logic close to the connection to minimize coupling.
- Consider a message-passing queue for START/ACCEPT events between Listener and Server,
  which decouples timing and allows easier sharding/testing.

References: the diagram was produced from reading `ctsMediaStreamServerListeningSocket.cpp`,
`ctsMediaStreamServer.cpp` and `ctsMediaStreamServerConnectedSocket.cpp` in this repository.
