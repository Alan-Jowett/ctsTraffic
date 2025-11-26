/*

Copyright (c) Microsoft Corporation
All rights reserved.

Licensed under the Apache License, Version 2.0 (the ""License""); you may not use this file except in compliance with the License. You may obtain a copy of the License at http://www.apache.org/licenses/LICENSE-2.0

THIS CODE IS PROVIDED ON AN  *AS IS* BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, EITHER EXPRESS OR IMPLIED, INCLUDING WITHOUT LIMITATION ANY IMPLIED WARRANTIES OR CONDITIONS OF TITLE, FITNESS FOR A PARTICULAR PURPOSE, MERCHANTABLITY OR NON-INFRINGEMENT.

See the Apache Version 2.0 License for specific language governing permissions and limitations under the License.

*/

/*
  Media Stream Interfaces

  Minimal interfaces for sender and receiver roles used by the media-stream
  refactor. The goal is to introduce a small, stable abstraction boundary so
  sender/receiver implementations can be moved into separate translation units
  without changing the protocol/IO semantics.

  Ownership and lifetime rules are documented below and must be preserved by
  implementations to avoid introducing races or hidden-threading behavior.
*/

#pragma once

#include <memory>
#include <functional>
#include <cstdint>
#include <WinSock2.h>

#include "ctsIOTask.hpp"
#include "ctsSocket.h"
#include "ctsMediaStreamProtocol.hpp"
#include "ctsWinsockLayer.h"

namespace ctsTraffic
{
// Forward: ctSockaddr is in ctl::ctSockaddr (included via protocol/header files)

// NOTE: These interfaces purposely remain small and procedural.
// - Do not add thread creation or hidden background work to implementations.
// - Caller is responsible for scheduling and calling the appropriate methods
//   on the expected threads (for example: timer callback for sync server sends
//   or the IO completion thread for async client sends).

class IMediaStreamSender
{
public:
    virtual ~IMediaStreamSender() = default;

    // Initialize the sender for a particular connection context.
    // - `controlSocket` is the weak reference to the ctsSocket used to report
    //   completion of the connection state. Implementations should lock the
    //   weak_ptr only for short-lived operations and must not assume it remains
    //   valid beyond a single call.
    // - `sendingSocket` is the OS SOCKET that should be used for datagram
    //   sends. The sender does NOT own this SOCKET and must not close it.
    // - `remoteAddr` is the target address for outgoing datagrams.
    virtual bool Initialize(
        std::weak_ptr<ctsSocket> controlSocket,
        SOCKET sendingSocket,
        const ctl::ctSockaddr& remoteAddr) noexcept = 0;

    // Perform a synchronous send for the provided task. This mirrors the
    // server-side code path that performs synchronous WSASendTo calls inside
    // the timer callback. The returned `wsIOResult` encapsulates bytes
    // transferred and an error code (consistent with `ctsWinsockLayer` helpers).
    // - Must complete before returning (synchronous contract).
    virtual wsIOResult SendSync(const ctsTask& task) noexcept = 0;

    // Initiate an asynchronous send. This mirrors the client code path which
    // uses an async send helper and a completion callback.
    // - Implementations should return a `wsIOResult` to communicate whether
    //   the operation completed inline or is pending; if pending, the provided
    //   completion callback must be invoked when finished.
    // - The completion callback must be invoked exactly once if the operation
    //   returns WSA_IO_PENDING; it may be invoked inline if the operation
    //   completes synchronously.
    virtual wsIOResult SendAsync(const ctsTask& task, std::function<void(wsIOResult)>&& completion) noexcept = 0;

    // Lifecycle helpers. Start/Stop are lightweight and must not spawn threads.
    virtual void Start() noexcept = 0;
    virtual void Stop() noexcept = 0;
};

class IMediaStreamReceiver
{
public:
    virtual ~IMediaStreamReceiver() = default;

    // Called when a datagram is received on a listening socket. The receiver
    // may synchronously parse the datagram and take any protocol action
    // required (for example: processing a START, or interpreting a data
    // frame). Implementations must NOT hold the `buffer` pointer beyond the
    // lifetime of this call; if retention is required the data must be copied.
    virtual void OnDatagram(const char* buffer, uint32_t length, const ctl::ctSockaddr& remoteAddr) noexcept = 0;

    // Lifecycle helpers. Start/Stop must be cheap and not create threads.
    virtual void Start() noexcept = 0;
    virtual void Stop() noexcept = 0;
};

/*
Ownership and lifetime summary
--------------------------------
- Buffers passed in `ctsTask` or as raw pointers to `OnDatagram`:
  - Caller (the IO layer / ctsThreadIocp) owns the memory backing the buffer.
  - Implementations must treat buffer pointers as transient and not reference
    them after the call returns. To retain payloads, explicitly copy into
    owned storage (for example, a std::vector or a heap allocation).

- SOCKET ownership:
  - The `sendingSocket` passed to `Initialize` is not owned by the sender.
  - Do not call `closesocket` on that SOCKET; the owner (server listener)
    manages its lifecycle.

- Stream object ownership:
  - Higher-level components (server `ctsMediaStreamServerImpl`, the
    `ctsMediaStreamServerConnectedSocket`, or client code) are responsible
    for creating and owning sender/receiver instances. Use `std::shared_ptr`
    to manage shared lifetime when multiple components may hold references.
  - Implementations should be safe to destroy on any thread; destruction must
    not block waiting for background work because implementations must not
    create hidden background threads. If an implementation needs to cancel
    in-flight operations, `Stop()` should be used by the owner prior to
    destruction.

Threading contract
------------------
- Callers must preserve the existing IO threading model:
  - `SendSync` will be called from the server timer callback (synchronous)
    and must complete before returning.
  - `SendAsync` will be called from client code paths that expect async
    behaviour and a completion callback on the IO thread.
  - `OnDatagram` will be invoked from the listening socket's IO completion
    handling thread — keep handlers efficient.

Adapters & factories
--------------------
Implementations can expose factory functions (free functions or static
creators) that return `std::shared_ptr<IMediaStreamSender>` /
`std::shared_ptr<IMediaStreamReceiver>` to simplify construction. Doing so
keeps ownership explicit and makes wiring into `ctsMediaStreamServerImpl` and
`ctsMediaStreamServerConnectedSocket` straightforward.

*/

} // namespace ctsTraffic
