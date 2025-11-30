/*
   ctsMediaStreamReceiver: handle receiving/parsing media stream
*/
#pragma once

#include <memory>
#include "ctsSocket.h"

namespace ctsTraffic
{
class ctsMediaStreamReceiver
{
public:
    explicit ctsMediaStreamReceiver(const std::shared_ptr<ctsSocket>& socket, bool passiveReceive = false) noexcept;
    ~ctsMediaStreamReceiver() noexcept = default;

    // Start processing IO on the connected socket using IOCP
    void Start() noexcept;

    // non-copyable
    ctsMediaStreamReceiver(const ctsMediaStreamReceiver&) = delete;
    ctsMediaStreamReceiver& operator=(const ctsMediaStreamReceiver&) = delete;
    ctsMediaStreamReceiver(ctsMediaStreamReceiver&&) = delete;
    ctsMediaStreamReceiver& operator=(ctsMediaStreamReceiver&&) = delete;

    void OnDataReceived(const ctl::ctSockaddr& remoteAddress, const char* buffer, uint32_t bufferLength) noexcept;

private:
    std::shared_ptr<ctsSocket> m_socket;

    bool m_passiveReceive = false;

    // internal helpers copied from former ctsMediaStreamClient implementation
    struct IoImplStatus
    {
        int m_errorCode = 0;
        bool m_continueIo = false;
    };

    static IoImplStatus IoImpl(
        const std::shared_ptr<ctsSocket>& sharedSocket,
        SOCKET socket,
        const std::shared_ptr<ctsIoPattern>& lockedPattern,
        const ctsTask& task) noexcept;

    static void IoCompletionCallback(
        _In_ OVERLAPPED* pOverlapped,
        const std::weak_ptr<ctsSocket>& weakSocket,
        const ctsTask& task) noexcept;
};
} // namespace ctsTraffic
