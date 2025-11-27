// parent header
#include "ctsMediaUploadServerConnectedSocket.h"
// cpp headers
#include <memory>
#include <functional>
#include <utility>
#include <deque>
#include <vector>
// os headers
#include <Windows.h>
#include <WinSock2.h>
// ctl headers
#include <ctSockaddr.hpp>
#include <ctString.hpp>
#include <ctTimer.hpp>
// project headers
#include "ctsWinsockLayer.h"

using namespace ctl;

namespace ctsTraffic
{
ctsMediaUploadServerConnectedSocket::ctsMediaUploadServerConnectedSocket(
    std::weak_ptr<ctsSocket> weakSocket,
    SOCKET sendingSocket,
    ctSockaddr remoteAddr,
    ctsMediaUploadConnectedSocketIoFunctor ioFunctor) :
    m_weakSocket(std::move(weakSocket)),
    m_ioFunctor(std::move(ioFunctor)),
    m_sendingSocket(sendingSocket),
    m_remoteAddr(std::move(remoteAddr)),
    m_connectTime(ctTimer::snap_qpc_as_msec())
{
    m_taskTimer.reset(CreateThreadpoolTimer(MediaUploadTimerCallback, this, ctsConfig::g_configSettings->pTpEnvironment));
    THROW_LAST_ERROR_IF(!m_taskTimer);
}

ctsMediaUploadServerConnectedSocket::~ctsMediaUploadServerConnectedSocket() noexcept
{
    m_taskTimer.reset();
}

void ctsMediaUploadServerConnectedSocket::ScheduleTask(const ctsTask& task) noexcept
{
    if (const auto sharedSocket = m_weakSocket.lock())
    {
        const auto lock = m_objectGuard.lock();
        _Analysis_assume_lock_acquired_(m_objectGuard);
        if (task.m_timeOffsetMilliseconds < 2)
        {
            m_nextTask = task;
            MediaUploadTimerCallback(nullptr, this, nullptr);
        }
        else
        {
            FILETIME ftDueTime{ctTimer::convert_ms_to_relative_filetime(task.m_timeOffsetMilliseconds)};
            m_nextTask = task;
            SetThreadpoolTimer(m_taskTimer.get(), &ftDueTime, 0, 0);
        }
        _Analysis_assume_lock_released_(m_objectGuard);
    }
}

void ctsMediaUploadServerConnectedSocket::CompleteState(uint32_t errorCode) const noexcept
{
    if (const auto sharedSocket = m_weakSocket.lock())
    {
        sharedSocket->CompleteState(errorCode);
    }
}

void ctsMediaUploadServerConnectedSocket::EnqueueReceivedDatagram(const char* buffer, uint32_t length) noexcept
{
    try
    {
        const auto lock = m_objectGuard.lock();
        std::vector<char> v;
        v.assign(buffer, buffer + length);
        m_recvQueue.emplace_back(std::move(v));
        // kick the timer so the connected socket will process pending Recv tasks
        SetThreadpoolTimer(m_taskTimer.get(), nullptr, 0, 0);
    }
    catch (...)
    {
        ctsConfig::PrintThrownException();
    }
}

VOID CALLBACK ctsMediaUploadServerConnectedSocket::MediaUploadTimerCallback(PTP_CALLBACK_INSTANCE, PVOID context, PTP_TIMER) noexcept
{
    auto* thisPtr = static_cast<ctsMediaUploadServerConnectedSocket*>(context);

    const auto sharedSocket = thisPtr->m_weakSocket.lock();
    if (!sharedSocket)
    {
        return;
    }

    const auto lockedSocket = sharedSocket->AcquireSocketLock();
    const auto lockedPattern = lockedSocket.GetPattern();
    if (!lockedPattern)
    {
        return;
    }

    const auto lock = thisPtr->m_objectGuard.lock();
    _Analysis_assume_lock_acquired_(thisPtr->m_objectGuard);

    // service Recv tasks first using any queued received datagrams
    ctsTask currentTask = thisPtr->m_nextTask;
    auto status = ctsIoStatus::ContinueIo;

    if (currentTask.m_ioAction == ctsTaskAction::Recv)
    {
        std::vector<char> datagram;
        {
            const auto recvLock = thisPtr->m_objectGuard.lock();
            _Analysis_assume_lock_acquired_(thisPtr->m_objectGuard);
            if (!thisPtr->m_recvQueue.empty())
            {
                datagram = std::move(thisPtr->m_recvQueue.front());
                thisPtr->m_recvQueue.pop_front();
            }
        }

        if (!datagram.empty())
        {
            status = lockedPattern->CompleteIo(currentTask, static_cast<uint32_t>(datagram.size()), 0);
        }
    }

    // if still continuing, handle send requests as before
    wsIOResult sendResults{};
    if (status == ctsIoStatus::ContinueIo)
    {
        sendResults = thisPtr->m_ioFunctor(thisPtr);
        status = lockedPattern->CompleteIo(
            thisPtr->m_nextTask,
            sendResults.m_bytesTransferred,
            sendResults.m_errorCode);

        while (ctsIoStatus::ContinueIo == status && thisPtr->m_nextTask.m_ioAction != ctsTaskAction::None)
        {
            currentTask = lockedPattern->InitiateIo();

            switch (currentTask.m_ioAction)
            {
                case ctsTaskAction::Send:
                    thisPtr->m_nextTask = currentTask;
                    if (thisPtr->m_nextTask.m_timeOffsetMilliseconds < 2)
                    {
                        sendResults = thisPtr->m_ioFunctor(thisPtr);
                        status = lockedPattern->CompleteIo(
                            thisPtr->m_nextTask,
                            sendResults.m_bytesTransferred,
                            sendResults.m_errorCode);
                    }
                    else
                    {
                        thisPtr->ScheduleTask(thisPtr->m_nextTask);
                    }
                    break;

                case ctsTaskAction::None:
                    break;

                case ctsTaskAction::Recv:
                    // Attempt to satisfy Recv from queued datagrams
                    {
                        std::vector<char> datagram;
                        const auto lockRecv = thisPtr->m_objectGuard.lock();
                        _Analysis_assume_lock_acquired_(thisPtr->m_objectGuard);
                        if (!thisPtr->m_recvQueue.empty())
                        {
                            datagram = std::move(thisPtr->m_recvQueue.front());
                            thisPtr->m_recvQueue.pop_front();
                        }
                        if (!datagram.empty())
                        {
                            status = lockedPattern->CompleteIo(currentTask, static_cast<uint32_t>(datagram.size()), 0);
                        }
                        else
                        {
                            thisPtr->m_nextTask = currentTask;
                        }
                    }
                    break;

                default:
                    FAIL_FAST_MSG(
                        "Unexpected task action returned from initiate_io - %d (dt %p ctsTraffic::ctsIOTask)",
                        currentTask.m_ioAction, &currentTask);
            }
        }
    }

    if (ctsIoStatus::FailedIo == status)
    {
        uint32_t returnedStatus = sendResults.m_errorCode;
        if (0 == returnedStatus)
        {
            returnedStatus = WSAECONNABORTED;
        }

        ctsConfig::PrintErrorInfo(
            L"MediaUpload Server socket (%ws) was indicated Failed IO from the protocol - aborting this stream",
            thisPtr->m_remoteAddr.writeCompleteAddress().c_str());
        thisPtr->CompleteState(returnedStatus);
    }
    else if (ctsIoStatus::CompletedIo == status)
    {
        PRINT_DEBUG_INFO(
            L"\t\tctsMediaUploadServerConnectedSocket socket (%ws) has completed its stream - closing this 'connection'\n",
            thisPtr->m_remoteAddr.writeCompleteAddress().c_str());
        thisPtr->CompleteState(sendResults.m_errorCode);
    }
    _Analysis_assume_lock_released_(thisPtr->m_objectGuard);
}
} // namespace
