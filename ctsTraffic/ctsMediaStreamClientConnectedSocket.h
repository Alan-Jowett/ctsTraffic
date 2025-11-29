/*
   ctsMediaStreamClientConnectedSocket: handle receiving/parsing media stream
*/
#pragma once

#include <memory>
#include "ctsSocket.h"

namespace ctsTraffic
{
class ctsMediaStreamClientConnectedSocket
{
public:
    explicit ctsMediaStreamClientConnectedSocket(const std::shared_ptr<ctsSocket>& socket) noexcept;
    ~ctsMediaStreamClientConnectedSocket() noexcept = default;

    // Start processing IO on the connected socket using IOCP
    void Start() noexcept;

    // non-copyable
    ctsMediaStreamClientConnectedSocket(const ctsMediaStreamClientConnectedSocket&) = delete;
    ctsMediaStreamClientConnectedSocket& operator=(const ctsMediaStreamClientConnectedSocket&) = delete;
    ctsMediaStreamClientConnectedSocket(ctsMediaStreamClientConnectedSocket&&) = delete;
    ctsMediaStreamClientConnectedSocket& operator=(ctsMediaStreamClientConnectedSocket&&) = delete;
private:
    std::shared_ptr<ctsSocket> m_socket;

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
