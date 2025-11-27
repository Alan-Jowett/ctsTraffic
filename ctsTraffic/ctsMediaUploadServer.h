/*
Upload server header - based on ctsMediaStreamServer.h
*/
#pragma once

// cpp headers
#include <memory>
// ctl headers
#include <ctSockaddr.hpp>
// project headers
#include "ctsSocket.h"
#include "ctsIOTask.hpp"

namespace ctsTraffic { namespace ctsMediaUploadServerImpl
    {
        void InitOnce();
        void ScheduleIo(const std::weak_ptr<ctsSocket>& weakSocket, const ctsTask& task);
        void AcceptSocket(const std::weak_ptr<ctsSocket>& weakSocket);
        void RemoveSocket(const ctl::ctSockaddr& targetAddr);
        void Start(SOCKET socket, const ctl::ctSockaddr& localAddr, const ctl::ctSockaddr& targetAddr);
        // Dispatch an incoming datagram to the server implementation
        void DispatchDatagram(SOCKET listeningSocket, const ctl::ctSockaddr& remoteAddr, const char* buffer, uint32_t length);
    }

    void ctsMediaUploadServerListener(const std::weak_ptr<ctsSocket>& weakSocket) noexcept;
    void ctsMediaUploadServerIo(const std::weak_ptr<ctsSocket>& weakSocket) noexcept;
    void ctsMediaUploadServerClose(const std::weak_ptr<ctsSocket>& weakSocket) noexcept;
}
