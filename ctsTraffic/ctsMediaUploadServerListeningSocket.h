/*
Upload listening socket header
*/
#pragma once

// cpp headers
#include <array>
#include <memory>
// os headers
#include <Windows.h>
// ctl headers
#include <ctSockaddr.hpp>
#include <ctThreadIocp.hpp>
// project headers
#include "ctsConfig.h"

namespace ctsTraffic
{
class ctsMediaUploadServerListeningSocket
{
private:
    static constexpr size_t c_recvBufferSize = 1024;

    std::shared_ptr<ctl::ctThreadIocp> m_threadIocp;

    mutable wil::critical_section m_listeningSocketLock{ctsConfig::ctsConfigSettings::c_CriticalSectionSpinlock};
    _Requires_lock_held_(m_listeningSocketLock) wil::unique_socket m_listeningSocket;

    const ctl::ctSockaddr m_listeningAddr;
    std::array<char, c_recvBufferSize> m_recvBuffer{};
    DWORD m_recvFlags{};
    ctl::ctSockaddr m_remoteAddr;
    int m_remoteAddrLen{};
    bool m_priorFailureWasConnectionReset = false;

    void RecvCompletion(OVERLAPPED* pOverlapped) noexcept;

public:
    ctsMediaUploadServerListeningSocket(
        wil::unique_socket&& listeningSocket,
        ctl::ctSockaddr listeningAddr);

    ~ctsMediaUploadServerListeningSocket() noexcept;

    SOCKET GetSocket() const noexcept;

    ctl::ctSockaddr GetListeningAddress() const noexcept;

    void InitiateRecv() noexcept;

    ctsMediaUploadServerListeningSocket(const ctsMediaUploadServerListeningSocket&) = delete;
    ctsMediaUploadServerListeningSocket& operator=(const ctsMediaUploadServerListeningSocket&) = delete;
    ctsMediaUploadServerListeningSocket(ctsMediaUploadServerListeningSocket&&) = delete;
    ctsMediaUploadServerListeningSocket& operator=(ctsMediaUploadServerListeningSocket&&) = delete;
};
}
