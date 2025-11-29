/*

Copyright (c) Microsoft Corporation
All rights reserved.

Licensed under the Apache License, Version 2.0 (the ""License""); you may not use this file except in compliance with the License. You may obtain a copy of the License at http://www.apache.org/licenses/LICENSE-2.0

THIS CODE IS PROVIDED ON AN  *AS IS* BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, EITHER EXPRESS OR IMPLIED, INCLUDING WITHOUT LIMITATION ANY IMPLIED WARRANTIES OR CONDITIONS OF TITLE, FITNESS FOR A PARTICULAR PURPOSE, MERCHANTABLITY OR NON-INFRINGEMENT.

See the Apache Version 2.0 License for specific language governing permissions and limitations under the License.

*/

#pragma once

// cpp headers
#include <memory>
// os headers
#include <Windows.h>
#include <WinSock2.h>
// ctl headers
#include <ctSockaddr.hpp>
// project headers
#include "ctsIOTask.hpp"
#include "ctsSocket.h"
#include "ctsWinsockLayer.h"
// wil headers always included last
#include <wil/resource.h>

namespace ctsTraffic
{
class ctsMediaStreamSender;

class ctsMediaStreamSender
{
private:
    // the CS is mutable, so we can take a lock / release a lock in const methods
    mutable wil::critical_section m_objectGuard{ctsConfig::ctsConfigSettings::c_CriticalSectionSpinlock};
    _Guarded_by_(m_objectGuard) ctsTask m_nextTask;

    wil::unique_threadpool_timer m_taskTimer{};

    // this weak_socket is the weak reference to the ctsSocket tracked by ctsSocketState & ctsSocketBroker
    // used to complete the state when finished and take a shared_ptr when needing to take a reference
    const std::weak_ptr<ctsSocket> m_weakSocket;

    // perform IO on the socket (previously delegated to server via ConnectedSocketIo)
    wsIOResult PerformIo() noexcept;

    // sending_socket is a shared socket from the datagram server
    // that (potentially) many connected datagram sockets will send from
    // thus it's not owned by this class
    const SOCKET m_sendingSocket;
    const ctl::ctSockaddr m_remoteAddr;

    int64_t m_sequenceNumber = 0LL;
    const int64_t m_connectTime = 0LL;

    ctsTask GetNextTask() const noexcept
    {
        const auto lock = m_objectGuard.lock();
        return m_nextTask;
    }

    int64_t IncrementSequence() noexcept
    {
        return InterlockedIncrement64(&m_sequenceNumber);
    }

    SOCKET GetSendingSocket() const noexcept
    {
        return m_sendingSocket;
    }

    const ctl::ctSockaddr& GetRemoteAddress() const noexcept
    {
        return m_remoteAddr;
    }


    int64_t GetStartTime() const noexcept
    {
        return m_connectTime;
    }

    void CompleteState(uint32_t errorCode) const noexcept;

    // Enqueue a task produced by the IO pattern for this connection.
    // This sets the next task and either posts an immediate send or schedules
    // the threadpool timer as appropriate.
    void QueueTask(const ctsTask& task) noexcept;

public:
    ctsMediaStreamSender(
        std::weak_ptr<ctsSocket> weakSocket,
        SOCKET sendingSocket,
        const ctl::ctSockaddr& remoteAddr);

    ~ctsMediaStreamSender() noexcept;


    // Called by the server to let the connected socket acquire the socket lock,
    // call the IO pattern's InitiateIo repeatedly, and enqueue any resulting
    // tasks. This moves InitiateIo into the connected socket to decouple the
    // server from protocol internals.
    void Start() noexcept;

    // non-copyable
    ctsMediaStreamSender(const ctsMediaStreamSender&) = delete;
    ctsMediaStreamSender& operator=(const ctsMediaStreamSender&) = delete;
    ctsMediaStreamSender(ctsMediaStreamSender&&) = delete;
    ctsMediaStreamSender& operator=(ctsMediaStreamSender&&) = delete;

private:
    static VOID CALLBACK MediaStreamTimerCallback(PTP_CALLBACK_INSTANCE, PVOID context, PTP_TIMER) noexcept;
};
}
