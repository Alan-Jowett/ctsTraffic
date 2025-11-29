/*

Copyright (c) Microsoft Corporation
All rights reserved.

Licensed under the Apache License, Version 2.0 (the ""License""); you may not use this file except in compliance with the License. You may obtain a copy of the License at http://www.apache.org/licenses/LICENSE-2.0

THIS CODE IS PROVIDED ON AN  *AS IS* BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, EITHER EXPRESS OR IMPLIED, INCLUDING WITHOUT LIMITATION ANY IMPLIED WARRANTIES OR CONDITIONS OF TITLE, FITNESS FOR A PARTICULAR PURPOSE, MERCHANTABLITY OR NON-INFRINGEMENT.

See the Apache Version 2.0 License for specific language governing permissions and limitations under the License.

*/

// cpp headers
#include <memory>
#include <vector>
#include <variant>
// os headers
#include <Windows.h>
#include <WinSock2.h>
// ctl headers
#include <ctSockaddr.hpp>
// project headers
#include "ctsMediaStreamProtocol.hpp"
#include "ctsMediaStreamClient.h"
#include "ctsMediaStreamReceiver.h"
#include "ctsMediaStreamSender.h"
#include "ctsWinsockLayer.h"
#include "ctsIOTask.hpp"
#include "ctsIOPattern.h"
#include "ctsSocket.h"
#include "ctsConfig.h"
// wil headers always included last
#include <wil/stl.h>
#include <wil/resource.h>

namespace ctsTraffic
{
    using mediaStreamerPtr = std::variant<std::shared_ptr<ctsMediaStreamSender>, std::shared_ptr<ctsMediaStreamReceiver>>;

    _Guarded_by_(g_socketVectorGuard) static std::vector<mediaStreamerPtr> g_activeMediaStreamers;
    static wil::critical_section g_socketVectorGuard{ ctsConfig::ctsConfigSettings::c_CriticalSectionSpinlock }; // NOLINT(cppcoreguidelines-interfaces-global-init, clang-diagnostic-exit-time-destructors)

    // The function that is registered with ctsTraffic to run Winsock IO using IO Completion Ports
    // - with the specified ctsSocket
    void ctsMediaStreamClientIo(const std::weak_ptr<ctsSocket>& weakSocket) noexcept
    {
        // This client only initiates the connect (START) flow. Receiving/parsing is handled
        // by `mediaStreamer` after the START handshake completes.
        const auto sharedSocket(weakSocket.lock());
        if (!sharedSocket)
        {
            return;
        }
        
        if (ctsConfig::IoPatternType::MediaStreamPull == ctsConfig::g_configSettings->IoPattern)
        {
            // Start the connected-socket handler to manage receiving/parsing
            const auto connectedSocket = std::make_shared<ctsMediaStreamReceiver>(sharedSocket);
            connectedSocket->Start();

            const auto lockConnectedObject = g_socketVectorGuard.lock();
            g_activeMediaStreamers.push_back(connectedSocket);
        }
        else if (ctsConfig::IoPatternType::MediaStreamPush == ctsConfig::g_configSettings->IoPattern)
        {
            // Start the connected-socket handler to manage sending
            SOCKET socket = INVALID_SOCKET;
            {
                const auto lockedSocket = sharedSocket->AcquireSocketLock();
                socket = lockedSocket.GetSocket();
            }

            const auto connectedSocket = std::make_shared<ctsMediaStreamSender>(sharedSocket, socket,
                sharedSocket->GetRemoteSockaddr());
            connectedSocket->Start();

            const auto lockConnectedObject = g_socketVectorGuard.lock();
            g_activeMediaStreamers.push_back(connectedSocket);
        }
    }

    // The function that is registered with ctsTraffic to 'connect' to the target server by sending a START command
    // using IO Completion Ports
    void ctsMediaStreamClientConnect(const std::weak_ptr<ctsSocket>& weakSocket) noexcept
    {
        // attempt to get a reference to the socket
        const auto sharedSocket(weakSocket.lock());
        if (!sharedSocket)
        {
            return;
        }

        {
            // hold a reference on the socket
            const auto lockedSocket = sharedSocket->AcquireSocketLock();
            if (lockedSocket.GetSocket() == INVALID_SOCKET)
            {
                sharedSocket->CompleteState(WSAECONNABORTED);
                return;
            }

            const auto socket = lockedSocket.GetSocket();
            const ctl::ctSockaddr targetAddress(sharedSocket->GetRemoteSockaddr());
            const ctsTask startTask = ctsMediaStreamMessage::Construct(MediaStreamAction::START);

            // Perform a synchronous WSASendTo for the START message (simulate connect)
            WSABUF buf;
            buf.buf = startTask.m_buffer + startTask.m_bufferOffset;
            buf.len = static_cast<ULONG>(startTask.m_bufferLength - startTask.m_bufferOffset);

            int sendFlags = 0;
            DWORD bytesSent = 0;
            int wsaret = WSASendTo(
                socket,
                &buf,
                1,
                reinterpret_cast<LPDWORD>(&bytesSent),
                sendFlags,
                targetAddress.sockaddr(),
                static_cast<int>(targetAddress.length()),
                nullptr,
                nullptr);

            if (wsaret == 0)
            {
                // set the local and remote addresses on the socket object
                ctl::ctSockaddr localAddr;
                auto localAddrLen = localAddr.length();
                if (0 == getsockname(socket, localAddr.sockaddr(), &localAddrLen))
                {
                    sharedSocket->SetLocalSockaddr(localAddr);
                }
                sharedSocket->SetRemoteSockaddr(targetAddress);

                ctsConfig::SetPostConnectOptions(socket, targetAddress);

                ctsConfig::PrintNewConnection(localAddr, targetAddress);

                PRINT_DEBUG_INFO(
                    L"\t\tctsMediaStreamClient sent its START message to %ws\n",
                    targetAddress.writeCompleteAddress().c_str());

                sharedSocket->CompleteState(NO_ERROR);
            }
            else
            {
                const int error = WSAGetLastError();
                ctsConfig::PrintErrorIfFailed("WSASendTo (START request)", error);
                sharedSocket->CompleteState(error);
                return;
            }
        }
    }

} // namespace
