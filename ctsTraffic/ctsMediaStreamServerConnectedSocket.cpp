/*

Copyright (c) Microsoft Corporation
All rights reserved.

Licensed under the Apache License, Version 2.0 (the ""License""); you may not use this file except in compliance with the License. You may obtain a copy of the License at http://www.apache.org/licenses/LICENSE-2.0

THIS CODE IS PROVIDED ON AN  *AS IS* BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, EITHER EXPRESS OR IMPLIED, INCLUDING WITHOUT LIMITATION ANY IMPLIED WARRANTIES OR CONDITIONS OF TITLE, FITNESS FOR A PARTICULAR PURPOSE, MERCHANTABLITY OR NON-INFRINGEMENT.

See the Apache Version 2.0 License for specific language governing permissions and limitations under the License.

*/

// parent header
#include "ctsMediaStreamServerConnectedSocket.h"
// cpp headers
#include <memory>
#include <functional>
#include <utility>
// os headers
#include <Windows.h>
#include <WinSock2.h>
// ctl headers
#include <ctSockaddr.hpp>
#include <ctString.hpp>
#include <ctTimer.hpp>
// project headers
#include "ctsWinsockLayer.h"
#include "ctsMediaStreamProtocol.hpp"

using namespace ctl;

namespace ctsTraffic
{
ctsMediaStreamServerConnectedSocket::ctsMediaStreamServerConnectedSocket(
    std::weak_ptr<ctsSocket> weakSocket,
    SOCKET sendingSocket,
    ctSockaddr remoteAddr) :
    m_weakSocket(std::move(weakSocket)),
    m_sendingSocket(sendingSocket),
    m_remoteAddr(std::move(remoteAddr)),
    m_connectTime(ctTimer::snap_qpc_as_msec())
{
    m_taskTimer.reset(CreateThreadpoolTimer(MediaStreamTimerCallback, this, ctsConfig::g_configSettings->pTpEnvironment));
    THROW_LAST_ERROR_IF(!m_taskTimer);
}

ctsMediaStreamServerConnectedSocket::~ctsMediaStreamServerConnectedSocket() noexcept
{
    // stop the TP before letting the destructor delete any member objects
    m_taskTimer.reset();
}

void ctsMediaStreamServerConnectedSocket::QueueTask(const ctsTask& task) noexcept
{
    if (const auto sharedSocket = m_weakSocket.lock())
    {
        const auto lock = m_objectGuard.lock();
        _Analysis_assume_lock_acquired_(m_objectGuard);
        if (task.m_timeOffsetMilliseconds < 2)
        {
            // in this case, immediately schedule the WSASendTo
            m_nextTask = task;
            MediaStreamTimerCallback(nullptr, this, nullptr);
        }
        else
        {
            FILETIME ftDueTime{ctTimer::convert_ms_to_relative_filetime(task.m_timeOffsetMilliseconds)};
            // assign the next task *and* schedule the timer while in *this object lock
            m_nextTask = task;
            SetThreadpoolTimer(m_taskTimer.get(), &ftDueTime, 0, 0);
        }
        _Analysis_assume_lock_released_(m_objectGuard);
    }
}

void ctsMediaStreamServerConnectedSocket::Start() noexcept
{
    const auto lock = m_objectGuard.lock();
    _Analysis_assume_lock_acquired_(m_objectGuard);
    if (m_nextTask.m_timeOffsetMilliseconds < 2)
    {
        // immediate
        MediaStreamTimerCallback(nullptr, this, nullptr);
    }
    else
    {
        FILETIME ftDueTime{ctTimer::convert_ms_to_relative_filetime(m_nextTask.m_timeOffsetMilliseconds)};
        SetThreadpoolTimer(m_taskTimer.get(), &ftDueTime, 0, 0);
    }
    _Analysis_assume_lock_released_(m_objectGuard);
}

void ctsMediaStreamServerConnectedSocket::CompleteState(uint32_t errorCode) const noexcept
{
    if (const auto sharedSocket = m_weakSocket.lock())
    {
        sharedSocket->CompleteState(errorCode);
    }
}

VOID CALLBACK ctsMediaStreamServerConnectedSocket::MediaStreamTimerCallback(PTP_CALLBACK_INSTANCE, PVOID context, PTP_TIMER) noexcept
{
    auto* thisPtr = static_cast<ctsMediaStreamServerConnectedSocket*>(context);

    // take a lock on the ctsSocket for this 'connection'
    const auto sharedSocket = thisPtr->m_weakSocket.lock();
    if (!sharedSocket)
    {
        return;
    }

    // hold a reference on the socket
    const auto lockedSocket = sharedSocket->AcquireSocketLock();
    const auto lockedPattern = lockedSocket.GetPattern();
    if (!lockedPattern)
    {
        return;
    }

    const auto lock = thisPtr->m_objectGuard.lock();
    _Analysis_assume_lock_acquired_(thisPtr->m_objectGuard);

    // post the queued IO, then loop sending/scheduling as necessary
    auto sendResults = thisPtr->PerformIo();
    auto status = lockedPattern->CompleteIo(
        thisPtr->m_nextTask,
        sendResults.m_bytesTransferred,
        sendResults.m_errorCode);

    ctsTask currentTask = thisPtr->m_nextTask;
    while (ctsIoStatus::ContinueIo == status && currentTask.m_ioAction != ctsTaskAction::None)
    {
        currentTask = lockedPattern->InitiateIo();

        switch (currentTask.m_ioAction)
        {
            case ctsTaskAction::Send:
                thisPtr->m_nextTask = currentTask;
            // if the time is less than two ms., we need to catch up on sends
            // - post the sendto immediately instead of scheduling for later
                if (thisPtr->m_nextTask.m_timeOffsetMilliseconds < 2)
                {
                        sendResults = thisPtr->PerformIo();
                    status = lockedPattern->CompleteIo(
                        thisPtr->m_nextTask,
                        sendResults.m_bytesTransferred,
                        sendResults.m_errorCode);
                }
                else
                {
                    thisPtr->QueueTask(thisPtr->m_nextTask);
                }
                break;

            case ctsTaskAction::None:
                // done until the next send completes
                break;

            case ctsTaskAction::Recv:
                [[fallthrough]];
            case ctsTaskAction::GracefulShutdown:
                [[fallthrough]];
            case ctsTaskAction::HardShutdown:
                [[fallthrough]];
            case ctsTaskAction::Abort:
                [[fallthrough]];
            case ctsTaskAction::FatalAbort:
                [[fallthrough]];
            default: // NOLINT(clang-diagnostic-covered-switch-default)
                FAIL_FAST_MSG(
                    "Unexpected task action returned from initiate_io - %d (dt %p ctsTraffic::ctsIOTask)",
                    currentTask.m_ioAction, &currentTask);
        }
    }

    if (ctsIoStatus::FailedIo == status)
    {
        // if IO has failed, we won't have anymore scheduled in the future
        // - deliberately stop processing now
        // must guarantee a failed error code is returned
        uint32_t returnedStatus = sendResults.m_errorCode;
        if (0 == returnedStatus)
        {
            returnedStatus = WSAECONNABORTED;
        }

        ctsConfig::PrintErrorInfo(
            L"MediaStream Server socket (%ws) was indicated Failed IO from the protocol - aborting this stream",
            thisPtr->m_remoteAddr.writeCompleteAddress().c_str());
        thisPtr->CompleteState(returnedStatus);
    }
    else if (ctsIoStatus::CompletedIo == status)
    {
        PRINT_DEBUG_INFO(
            L"\t\tctsMediaStreamServerConnectedSocket socket (%ws) has completed its stream - closing this 'connection'\n",
            thisPtr->m_remoteAddr.writeCompleteAddress().c_str());
        thisPtr->CompleteState(sendResults.m_errorCode);
    }
    _Analysis_assume_lock_released_(thisPtr->m_objectGuard);
}

    wsIOResult ctsMediaStreamServerConnectedSocket::PerformIo() noexcept
    {
        const SOCKET socket = this->GetSendingSocket();
        if (INVALID_SOCKET == socket)
        {
            return wsIOResult(WSA_OPERATION_ABORTED);
        }

        const ctl::ctSockaddr& remoteAddr(this->GetRemoteAddress());
        const ctsTask nextTask = this->GetNextTask();

        wsIOResult returnResults;
        if (ctsTask::BufferType::UdpConnectionId == nextTask.m_bufferType)
        {
            WSABUF wsaBuffer{};
            wsaBuffer.buf = nextTask.m_buffer;
            wsaBuffer.len = nextTask.m_bufferLength;

            const auto sendResult = WSASendTo(
                socket,
                &wsaBuffer,
                1,
                &returnResults.m_bytesTransferred,
                0,
                remoteAddr.sockaddr(),
                remoteAddr.length(),
                nullptr,
                nullptr);

            if (SOCKET_ERROR == sendResult)
            {
                const auto error = WSAGetLastError();
                ctsConfig::PrintErrorInfo(
                    L"WSASendTo(%Iu, %ws) for the Connection-ID failed [%d]",
                    socket,
                    remoteAddr.writeCompleteAddress().c_str(),
                    error);
                return wsIOResult(error);
            }
        }
        else
        {
            const auto sequenceNumber = this->IncrementSequence();
            ctsMediaStreamSendRequests sendingRequests(
                nextTask.m_bufferLength, // total bytes to send
                sequenceNumber,
                nextTask.m_buffer);
            for (auto& sendRequest : sendingRequests)
            {
                DWORD bytesSent{};
                const auto sendResult = WSASendTo(
                    socket,
                    sendRequest.data(),
                    static_cast<DWORD>(sendRequest.size()),
                    &bytesSent,
                    0,
                    remoteAddr.sockaddr(),
                    remoteAddr.length(),
                    nullptr,
                    nullptr);
                if (SOCKET_ERROR == sendResult)
                {
                    const auto error = WSAGetLastError();
                    if (WSAEMSGSIZE == error)
                    {
                        uint32_t bytesRequested = 0;
                        for (const auto& wsaBuffer : sendRequest)
                        {
                            bytesRequested += wsaBuffer.len;
                        }
                        ctsConfig::PrintErrorInfo(
                            L"WSASendTo(%Iu, seq %lld, %ws) failed with WSAEMSGSIZE : attempted to send datagram of size %u bytes",
                            socket,
                            sequenceNumber,
                            remoteAddr.writeCompleteAddress().c_str(),
                            bytesRequested);
                    }
                    else
                    {
                        ctsConfig::PrintErrorInfo(
                            L"WSASendTo(%Iu, seq %lld, %ws) failed [%d]",
                            socket,
                            sequenceNumber,
                            remoteAddr.writeCompleteAddress().c_str(),
                            error);
                    }
                    return wsIOResult(error);
                }

                returnResults.m_bytesTransferred += bytesSent;
                PRINT_DEBUG_INFO(
                    L"\t\tctsMediaStreamServer sending seq number %lld (%u sent-bytes, %u frame-bytes)\n",
                    sequenceNumber, bytesSent, returnResults.m_bytesTransferred);
            }
        }

        return returnResults;
    }

} // namespace ctsTraffic
