/*

Copyright (c) Microsoft Corporation
All rights reserved.

Licensed under the Apache License, Version 2.0 (the ""License""); you may not use this file except in compliance with the License. You may obtain a copy of the License at http://www.apache.org/licenses/LICENSE-2.0

THIS CODE IS PROVIDED ON AN  *AS IS* BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, EITHER EXPRESS OR IMPLIED, INCLUDING WITHOUT LIMITATION ANY IMPLIED WARRANTIES OR CONDITIONS OF TITLE, FITNESS FOR A PARTICULAR PURPOSE, MERCHANTABLITY OR NON-INFRINGEMENT.

See the Apache Version 2.0 License for specific language governing permissions and limitations under the License.

*/

// cpp headers
#include <exception>
#include <functional>
// os headers
#include <Windows.h>
// project headers
#include "ctsMediaStreamServerListeningSocketBase.h"
#include "ctsMediaStreamServer.h"
#include "ctsMediaStreamProtocol.hpp"

namespace ctsTraffic
{
ctsMediaStreamServerListeningSocketBase::ctsMediaStreamServerListeningSocketBase(ctl::ctSockaddr listeningAddr) noexcept :
    m_listeningAddr(std::move(listeningAddr))
{
}

ctsMediaStreamServerListeningSocketBase::~ctsMediaStreamServerListeningSocketBase() noexcept = default;

ctl::ctSockaddr ctsMediaStreamServerListeningSocketBase::GetListeningAddress() const noexcept
{
    return m_listeningAddr;
}

void ctsMediaStreamServerListeningSocketBase::Shutdown() noexcept
{
    // default no-op; derived may close sockets under lock
}

void ctsMediaStreamServerListeningSocketBase::ProcessReceivedPacket(SOCKET sock, const char* buffer, size_t bytesReceived, const ctl::ctSockaddr& remoteAddr, DWORD /*recvFlags*/) noexcept
{
    try
    {
        const ctsMediaStreamMessage message(ctsMediaStreamMessage::Extract(buffer, static_cast<int>(bytesReceived)));
        switch (message.m_action)
        {
            case MediaStreamAction::START:
                PRINT_DEBUG_INFO(
                    L"\t\tctsMediaStreamServer - processing START from %ws\n",
                    remoteAddr.writeCompleteAddress().c_str());
#ifndef TESTING_IGNORE_START
                // Dispatch to pimpl outside any external locks; this mirrors the previous behavior.
                ctsMediaStreamServerImpl::Start(sock, m_listeningAddr, remoteAddr);
#endif
                break;

            default: // NOLINT(clang-diagnostic-covered-switch-default)
                FAIL_FAST_MSG("ctsMediaStreamServer - received an unexpected Action: %d", message.m_action);
        }
    }
    catch (...)
    {
        ctsConfig::PrintThrownException();
    }
}
} // namespace ctsTraffic
