**CTS MediaStream Interactions**

This file documents the interactions between `ctsMediaStreamServerListeningSocket`,
`ctsMediaStreamServerImpl`, `ctsMediaStreamSender`, and the newly supported
`ctsMediaStreamReceiver` code paths. The goal is to clarify the runtime flows for both
sender and receiver roles, and to surface remaining coupling points and refactor ideas.

```mermaid
sequenceDiagram
    participant Client
    participant Listener as ctsMediaStreamServerListeningSocket
    participant IOCP as ctThreadIocp
    participant Server as ctsMediaStreamServerImpl
    participant ConnectedSender as ctsMediaStreamSender
    participant ConnectedReceiver as ctsMediaStreamReceiver
    participant ctsSocket as ctsSocket
    participant Pattern as IO Pattern (ctsIoPattern)

    %% Client -> Listening socket (common receive)
    Client->>Listener: UDP datagram (START / JOIN / DATA)
    Note right of Listener: InitiateRecv/RecvCompletion handles overlapped
    Listener->>IOCP: WSARecvFrom completion (OVERLAPPED)
    IOCP-->>Listener: invoke RecvCompletion

    %% ListeningSocket forwards raw packet to server (listener is protocol-agnostic)
    Listener->>Server: OnPacketReceived(listeningSocket, listeningAddr, remoteAddr, buffer, length)
    Note right of Server: Server parses protocol and decides actions (e.g., START)

    %% Server parsing -> decide action (start sender or receiver)
    Server->>Server: parse buffer
    alt Parsed as START (client initiating a send)
      Server->>Server: StartSender(socket, listeningAddr, remoteAddr)
    else Parsed as JOIN / RECV (client wants to receive)
      Server->>Server: StartReceiver(socket, listeningAddr, remoteAddr)
    else Other/Ignore
      Server-->>Listener: no action (listener remains agnostic)
    end

    alt Accepting ctsSocket already waiting (sender path)
      Server->>ConnectedSender: new ctsMediaStreamSender(weakSocket, socket, remoteAddr)
        Server->>ctsSocket: SetLocalSockaddr / SetRemoteSockaddr
        Server->>ctsSocket: CompleteState(NO_ERROR)  %% completes the pending ctsSocket Create
        Server-->>Listener: remove awaiting endpoint
    else No accepting socket available (sender)
        Server->>Server: push (socket, remoteAddr) to g_awaitingEndpoints
    end

    alt Accepting ctsSocket already waiting (receiver path)
      Server->>ConnectedReceiver: new ctsMediaStreamReceiver(weakSocket, socket, remoteAddr)
        Server->>ctsSocket: SetLocalSockaddr / SetRemoteSockaddr
        Server->>ctsSocket: CompleteState(NO_ERROR)
        Server-->>Listener: remove awaiting endpoint
    else No accepting socket available (receiver)
        Server->>Server: push (socket, remoteAddr) to g_awaitingEndpoints
    end

    %% ctsSocket actively asks to accept later (shared for both roles)
    ctsSocket->>Server: ctsMediaStreamServerListener(weakSocket) -> AcceptSocket(weakSocket)
    alt awaiting endpoint exists (match by role/remoteAddr)
      Server->>ConnectedSender: create ctsMediaStreamSender(weakSocket, socket, remoteAddr)
        Server->>ctsSocket: CompleteState(NO_ERROR)
    else no awaiting endpoint
        Server->>Server: push weakSocket to g_acceptingSockets
    end

    %% IO scheduling from ctsSocket / IO pattern
    ctsSocket->>Server: ctsMediaStreamServerIo(weakSocket)
    Server->>Pattern: lockedPattern->InitiateIo()  (loop until None)
    Pattern-->>Server: returns ctsTask (Send / Recv etc.)
    Server->>Server: ScheduleIo(weakSocket, task)
    alt task is Send
      Server->>ConnectedSender: find connected sender by remoteAddr and call QueueTask(task)
    else task is Recv
      Server->>ConnectedReceiver: find connected receiver and call QueueTask(task)
    end

    %% Sender-side execution
    ConnectedSender->>ConnectedSender: schedule or immediate MediaStreamTimerCallback
      ConnectedSender->>ConnectedSender: invoke PerformIo() (owns send logic)
      ConnectedSender->>ConnectedSender: WSASendTo(...) (synchronous sends performed inside `PerformIo`)

    ConnectedSender->>Pattern: lockedPattern->CompleteIo(bytes, error)
    Pattern-->>ConnectedSender: CompleteIo result (ctsIoStatus)
    loop while ContinueIo
        ConnectedSender->>Pattern: InitiateIo()
        Pattern-->>ConnectedSender: returns next ctsTask
        ConnectedSender->>ConnectedSender: QueueTask or run immediate send
    end

    %% Receiver-side execution
    ConnectedReceiver->>ConnectedReceiver: schedule or immediate MediaStreamTimerCallback
      ConnectedReceiver->>ConnectedReceiver: invoke PerformIo() (owns receive/processing logic)
      ConnectedReceiver->>ConnectedReceiver: may call WSARecvFrom/ProcessPacket

    ConnectedReceiver->>Pattern: lockedPattern->CompleteIo(bytes, error)
    Pattern-->>ConnectedReceiver: CompleteIo result (ctsIoStatus)
    loop while ContinueIo
        ConnectedReceiver->>Pattern: InitiateIo()
        Pattern-->>ConnectedReceiver: returns next ctsTask
        ConnectedReceiver->>ConnectedReceiver: QueueTask or run immediate receive
    end

    alt CompletedIo (either role)
        ConnectedSender->>ctsSocket: CompleteState(errorCode)  %% marks socket done -> closes
        ConnectedReceiver->>ctsSocket: CompleteState(errorCode)
        ctsSocket->>Server: ctsMediaStreamServerClose(weak_ptr) -> RemoveSocket(remoteAddr)
        Server->>Server: erase connected socket from g_connectedSockets
    else FailedIo
        ConnectedSender->>ctsSocket: CompleteState(errorCode)
        ConnectedReceiver->>ctsSocket: CompleteState(errorCode)
        ctsSocket->>Server: ctsMediaStreamServerClose -> RemoveSocket
    end

```

**Notes / observations**
- **Who owns what:** `ctsMediaStreamServerImpl` manages global containers:
    - `g_listeningSockets`, `g_connectedSockets`, `g_acceptingSockets`, `g_awaitingEndpoints`.
    - `ctsMediaStreamServerListeningSocket` performs low-level recv and forwards raw packets
      to the server via an injected callback (`m_packetCallback`). The server implements
      `OnPacketReceived(...)` which parses packets and now routes to either `StartSender(...)`
      or `StartReceiver(...)` depending on the protocol message.
    - `g_connectedSockets` is an `std::unordered_map` keyed by `ctl::ctSockaddr` for O(1)
      lookups instead of a vector scan.
  - `ctsMediaStreamSender` and `ctsMediaStreamReceiver` each own per-connection timers and scheduling
    and contain the role-specific `PerformIo()` logic (send or receive).

**Direct coupling points (after refactor / remaining):**
- The listening socket is protocol-agnostic and forwards raw datagrams via an injected
  callback (`m_packetCallback`) — parsing and decision logic now live in `Server::OnPacketReceived`.
- The connected sockets (sender and receiver) no longer depend on a server-supplied
  `ConnectedSocketIo` functor — each role implements its own `PerformIo()`.
- `ServerImpl` still holds and mutates the global connection containers (unordered_map for
  `g_connectedSockets`) protected by a single lock, making the server the central coordinator
  and potential contention hotspot. Matching accept/waiting entries must consider role (sender vs receiver)
  and remote address.

**Suggestions to reduce coupling**
- Introduce interfaces/events: the current change injects a callback; consider defining a small
  interface (e.g., `IListenerHandler::OnPacketReceived(...)`) if you prefer compile-time
  decoupling over `std::function`.
- Replace global containers with an injected `ConnectionManager` (interface) to avoid static globals
  and allow unit-testable, pluggable implementations. The manager can provide role-aware lookup APIs
  (e.g., `FindSenderFor(remoteAddr)` / `FindReceiverFor(remoteAddr)`).
- Use a factory or builder to create `ConnectedSocket` instances (inject `IConnectedSocketFactory`),
  so `ServerImpl` doesn't `new` the concrete class directly.
- Limit lock scope and split responsibilities: use per-connection locks or concurrent maps
  for `g_connectedSockets` and `g_awaitingEndpoints` to reduce contention and simplify reasoning.
- If desired, replace `PerformIo` implementations with injected `ISender` / `IReceiver` interfaces later
  to allow pluggable strategies; the current change keeps role logic close to the connection to minimize coupling.
- Consider a message-passing queue for START/ACCEPT events between Listener and Server,
  which decouples timing and allows easier sharding/testing. Make sure events carry the intended role
  so accepting sockets can be matched correctly.

References: the diagrams and notes were produced from reading `ctsMediaStreamServerListeningSocket.cpp`,
`ctsMediaStreamServer.cpp`, `ctsMediaStreamSender.cpp`, and the recently added `ctsMediaStreamReceiver` files
in this repository.

---

**Client Path (ctsMediaStreamClient interactions)**

Below is a Mermaid sequence diagram that focuses on the `ctsMediaStreamClient` side: how
the client code registers and uses `ctsMediaStreamClientConnect`, `ctsMediaStreamClientIo`,
and `ctsMediaStreamClientClose` with a `ctsSocket` and the thread IO system.

```mermaid
sequenceDiagram
    participant ClientApp as Client (app)
    participant Socket as ctsSocket
    participant ClientModule as ctsMediaStreamClient
    participant IOWorker as ctThreadIocp
    participant Network as Network (WSA)

    Note over ClientApp,ClientModule: Client initializes and creates a `ctsSocket`
    ClientApp->>Socket: create ctsSocket (shared_ptr)
    ClientApp->>ClientModule: register handlers / start

    ClientModule->>Socket: initiate Connect (ctsMediaStreamClientConnect)
    Note right of Socket: associates with IOWorker / posts overlapped connect/send
    Socket->>Network: WSASendTo(START) [outbound START command]
    Network-->>Socket: send completion
    Socket->>ClientModule: connect completion callback

    ClientModule->>Socket: ctsMediaStreamClientIo (start IO loop)
    ClientModule->>IOWorker: post/read overlapped operations
    IOWorker->>Socket: WSARecvFrom completions (incoming DATA)
    Socket-->>ClientModule: Io completion (process recv/send completions)

    alt normal operation
      ClientModule->>Socket: schedule send/recv via PerformIo logic
      Socket->>Network: WSASendTo / WSARecvFrom
    end

    ClientApp->>ClientModule: request close
    ClientModule->>Socket: ctsMediaStreamClientClose(weakSocket)
    Socket->>Network: graceful close / cancel IO
    Socket->>ClientModule: CompleteState(errorCode)
    ClientModule->>IOWorker: unregister / stop

```

Caption: This diagram isolates the client-side lifecycle: create/connect, run IO, and close.
