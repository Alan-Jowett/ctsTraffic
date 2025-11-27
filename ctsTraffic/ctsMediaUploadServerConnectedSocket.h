/*
Upload connected socket header
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
#include <deque>
#include <vector>

namespace ctsTraffic
{
class ctsMediaUploadServerConnectedSocket;
using ctsMediaUploadConnectedSocketIoFunctor = std::function<wsIOResult (ctsMediaUploadServerConnectedSocket*)>;

class ctsMediaUploadServerConnectedSocket
{
private:
    mutable wil::critical_section m_objectGuard{ctsConfig::ctsConfigSettings::c_CriticalSectionSpinlock};
    _Guarded_by_(m_objectGuard) ctsTask m_nextTask;

    wil::unique_threadpool_timer m_taskTimer{};

    const std::weak_ptr<ctsSocket> m_weakSocket;
    const ctsMediaUploadConnectedSocketIoFunctor m_ioFunctor;
    const SOCKET m_sendingSocket;
    const ctl::ctSockaddr m_remoteAddr;

    int64_t m_sequenceNumber = 0LL;
    const int64_t m_connectTime = 0LL;
    // queued datagrams received from the listening socket to satisfy Recv tasks
    std::deque<std::vector<char>> m_recvQueue;

public:
    ctsMediaUploadServerConnectedSocket(
        std::weak_ptr<ctsSocket> weakSocket,
        SOCKET sendingSocket,
        ctl::ctSockaddr remoteAddr,
        ctsMediaUploadConnectedSocketIoFunctor ioFunctor);

    ~ctsMediaUploadServerConnectedSocket() noexcept;

    const ctl::ctSockaddr& GetRemoteAddress() const noexcept
    {
        return m_remoteAddr;
    }

    SOCKET GetSendingSocket() const noexcept
    {
        return m_sendingSocket;
    }

    int64_t GetStartTime() const noexcept
    {
        return m_connectTime;
    }

    ctsTask GetNextTask() const noexcept
    {
        const auto lock = m_objectGuard.lock();
        return m_nextTask;
    }

    int64_t IncrementSequence() noexcept
    {
        return InterlockedIncrement64(&m_sequenceNumber);
    }

    void ScheduleTask(const ctsTask& task) noexcept;

    void CompleteState(uint32_t errorCode) const noexcept;

    // Called by the listening socket when a datagram is received from this remote address
    void EnqueueReceivedDatagram(const char* buffer, uint32_t length) noexcept;

    ctsMediaUploadServerConnectedSocket(const ctsMediaUploadServerConnectedSocket&) = delete;
    ctsMediaUploadServerConnectedSocket& operator=(const ctsMediaUploadServerConnectedSocket&) = delete;
    ctsMediaUploadServerConnectedSocket(ctsMediaUploadServerConnectedSocket&&) = delete;
    ctsMediaUploadServerConnectedSocket& operator=(ctsMediaUploadServerConnectedSocket&&) = delete;

private:
    static VOID CALLBACK MediaUploadTimerCallback(PTP_CALLBACK_INSTANCE, PVOID context, PTP_TIMER) noexcept;
};
}
