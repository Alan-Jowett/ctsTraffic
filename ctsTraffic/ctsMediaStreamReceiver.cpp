/*

Copyright (c) Microsoft Corporation
All rights reserved.

Licensed under the Apache License, Version 2.0 (the ""License""); you may not use this file except in compliance with the License. You may obtain a copy of the License at http://www.apache.org/licenses/LICENSE-2.0

THIS CODE IS PROVIDED ON AN  *AS IS* BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, EITHER EXPRESS OR IMPLIED, INCLUDING WITHOUT LIMITATION ANY IMPLIED WARRANTIES OR CONDITIONS OF TITLE, FITNESS FOR A PARTICULAR PURPOSE, MERCHANTABLITY OR NON-INFRINGEMENT.

See the Apache Version 2.0 License for specific language governing permissions and limitations under the License.

*/

// cpp headers
#include <memory>
// os headers
#include <Windows.h>
#include <WinSock2.h>
// ctl headers
#include <ctSockaddr.hpp>
// project headers
#include "ctsMediaStreamProtocol.hpp"
#include "ctsMediaStreamReceiver.h"
#include "ctsWinsockLayer.h"
#include "ctsIOTask.hpp"
#include "ctsIOPattern.h"
#include "ctsSocket.h"
#include "ctsConfig.h"
// wil headers always included last
#include <wil/stl.h>
#include <wil/resource.h>
#include <unordered_map>
#include <string>

namespace ctsTraffic
{
    struct IoImplStatus
    {
        int m_errorCode = 0;
        bool m_continueIo = false;
    };

    // Internal implementation functions
    static IoImplStatus ctsMediaStreamReceiverIoImpl(
        const std::shared_ptr<ctsSocket>& sharedSocket,
        SOCKET socket,
        const std::shared_ptr<ctsIoPattern>& lockedPattern,
        const ctsTask& task) noexcept;

    static void ctsMediaStreamReceiverIoCompletionCallback(
        _In_ OVERLAPPED* pOverlapped,
        const std::weak_ptr<ctsSocket>& weakSocket,
        const ctsTask& task
    ) noexcept;

    static void ctsMediaStreamReceiverConnectionCompletionCallback(
        _In_ OVERLAPPED* pOverlapped,
        const std::weak_ptr<ctsSocket>& weakSocket,
        const ctl::ctSockaddr& targetAddress
    ) noexcept;

    // Simple in-memory handshake tracking per remote address (translation unit scope)
    static std::unordered_map<std::wstring, HandshakeInfo> g_handshakeMap;

    void ctsMediaStreamReceiver(const std::weak_ptr<ctsSocket>& weakSocket) noexcept
    {
        const auto sharedSocket(weakSocket.lock());
        if (!sharedSocket)
        {
            return;
        }

        const auto lockedSocket = sharedSocket->AcquireSocketLock();
        const auto lockedPattern = lockedSocket.GetPattern();
        if (!lockedPattern || lockedSocket.GetSocket() == INVALID_SOCKET)
        {
            return;
        }

        lockedPattern->RegisterCallback(
            [weakSocket](const ctsTask& task) noexcept
            {
                const auto lambdaSharedSocket(weakSocket.lock());
                if (!lambdaSharedSocket)
                {
                    return;
                }

                const auto lambdaLockedSocket = lambdaSharedSocket->AcquireSocketLock();
                const auto lambdaLockedPattern = lambdaLockedSocket.GetPattern();
                if (!lambdaLockedPattern || lambdaLockedSocket.GetSocket() == INVALID_SOCKET)
                {
                    return;
                }

                if (lambdaSharedSocket->IncrementIo() > 1)
                {
                    const IoImplStatus status = ctsMediaStreamReceiverIoImpl(
                        lambdaSharedSocket, lambdaLockedSocket.GetSocket(), lambdaLockedPattern, task);
                    if (lambdaSharedSocket->DecrementIo() == 0)
                    {
                        lambdaSharedSocket->CompleteState(status.m_errorCode);
                    }
                }
                else
                {
                    lambdaSharedSocket->DecrementIo();
                }
            });

        sharedSocket->IncrementIo();
        IoImplStatus status = ctsMediaStreamReceiverIoImpl(
            sharedSocket,
            lockedSocket.GetSocket(),
            lockedPattern,
            lockedPattern->InitiateIo());
        while (status.m_continueIo)
        {
            status = ctsMediaStreamReceiverIoImpl(
                sharedSocket,
                lockedSocket.GetSocket(),
                lockedPattern,
                lockedPattern->InitiateIo());
        }
        if (0 == sharedSocket->DecrementIo())
        {
            sharedSocket->CompleteState(status.m_errorCode);
        }
    }

    void ctsMediaStreamReceiverConnect(const std::weak_ptr<ctsSocket>& weakSocket) noexcept
    {
        const auto sharedSocket(weakSocket.lock());
        if (!sharedSocket)
        {
            return;
        }

        const auto lockedSocket = sharedSocket->AcquireSocketLock();
        if (lockedSocket.GetSocket() == INVALID_SOCKET)
        {
            sharedSocket->CompleteState(WSAECONNABORTED);
            return;
        }

        const auto socket = lockedSocket.GetSocket();
        const ctl::ctSockaddr targetAddress(sharedSocket->GetRemoteSockaddr());
        const ctsTask startTask = ctsMediaStreamMessage::Construct(MediaStreamAction::START);

        const auto response = ctsWSASendTo(
            sharedSocket,
            lockedSocket.GetSocket(),
            startTask,
            [weakSocket, targetAddress](OVERLAPPED* ov) noexcept
            {
                ctsMediaStreamReceiverConnectionCompletionCallback(ov, weakSocket, targetAddress);
            });

        if (NO_ERROR == response.m_errorCode)
        {
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
                L"\t\tctsMediaStreamReceiver sent its START message to %ws\n",
                targetAddress.writeCompleteAddress().c_str());
        }

        if (response.m_errorCode != WSA_IO_PENDING)
        {
            sharedSocket->CompleteState(response.m_errorCode);
        }
    }

    IoImplStatus ctsMediaStreamReceiverIoImpl(
        const std::shared_ptr<ctsSocket>& sharedSocket,
        SOCKET socket,
        const std::shared_ptr<ctsIoPattern>& lockedPattern,
        const ctsTask& task) noexcept
    {
        IoImplStatus returnStatus;

        switch (task.m_ioAction)
        {
        case ctsTaskAction::Send:
            [[fallthrough]];
        case ctsTaskAction::Recv:
            {
                sharedSocket->IncrementIo();
                auto callback = [weak_reference = std::weak_ptr(sharedSocket), task](OVERLAPPED* ov) noexcept
                {
                    ctsMediaStreamReceiverIoCompletionCallback(ov, weak_reference, task);
                };

                PCSTR functionName{};
                wsIOResult result;
                if (ctsTaskAction::Send == task.m_ioAction)
                {
                    functionName = "WSASendTo";
                    result = ctsWSASendTo(sharedSocket, socket, task, std::move(callback));
                }
                else if (ctsTaskAction::Recv == task.m_ioAction)
                {
                    functionName = "WSARecvFrom";
                    result = ctsWSARecvFrom(sharedSocket, socket, task, std::move(callback));
                }
                else
                {
                    FAIL_FAST_MSG(
                        "ctsMediaStreamReceiverIoImpl: received an unexpected IOStatus in the ctsIOTask (%p)", &task);
                }

                if (WSA_IO_PENDING == result.m_errorCode)
                {
                    returnStatus.m_errorCode = static_cast<int>(result.m_errorCode);
                    returnStatus.m_continueIo = true;
                }
                else
                {
                    if (result.m_errorCode != 0)
                    {
                        PRINT_DEBUG_INFO(L"\t\tIO Failed: %hs (%u) [ctsMediaStreamReceiver]\n",
                                         functionName,
                                         result.m_errorCode);
                    }

                    switch (const auto protocolStatus = lockedPattern->CompleteIo(
                        task, result.m_bytesTransferred, result.m_errorCode))
                    {
                    case ctsIoStatus::ContinueIo:
                        returnStatus.m_errorCode = NO_ERROR;
                        returnStatus.m_continueIo = true;
                        break;

                    case ctsIoStatus::CompletedIo:
                        sharedSocket->CloseSocket();
                        returnStatus.m_errorCode = NO_ERROR;
                        returnStatus.m_continueIo = false;
                        break;

                    case ctsIoStatus::FailedIo:
                        ctsConfig::PrintErrorIfFailed(functionName, result.m_errorCode);
                        sharedSocket->CloseSocket();
                        returnStatus.m_errorCode = static_cast<int>(lockedPattern->GetLastPatternError());
                        returnStatus.m_continueIo = false;
                        break;

                    default:
                        FAIL_FAST_MSG("ctsMediaStreamReceiverIoImpl: unknown ctsSocket::IOStatus - %d\n", protocolStatus);
                    }

                    const auto ioCount = sharedSocket->DecrementIo();
                    FAIL_FAST_IF_MSG(
                        0 == ioCount,
                        "ctsMediaStreamReceiver : ctsSocket::io_count fell to zero while the Impl function was called (dt %p ctsTraffic::ctsSocket)",
                        sharedSocket.get());
                }

                break;
            }

        case ctsTaskAction::None:
            {
                returnStatus.m_errorCode = NO_ERROR;
                returnStatus.m_continueIo = false;
                break;
            }

        case ctsTaskAction::Abort:
            {
                lockedPattern->CompleteIo(task, 0, 0);
                sharedSocket->CloseSocket();

                returnStatus.m_errorCode = NO_ERROR;
                returnStatus.m_continueIo = false;
                break;
            }

        case ctsTaskAction::FatalAbort:
            {
                lockedPattern->CompleteIo(task, 0, 0);
                sharedSocket->CloseSocket();

                returnStatus.m_errorCode = static_cast<int>(lockedPattern->GetLastPatternError());
                returnStatus.m_continueIo = false;
                break;
            }

        case ctsTaskAction::GracefulShutdown:
        case ctsTaskAction::HardShutdown:
        default:
            break;
        }

        return returnStatus;
    }


    void ctsMediaStreamReceiverIoCompletionCallback(
        _In_ OVERLAPPED* pOverlapped,
        const std::weak_ptr<ctsSocket>& weakSocket,
        const ctsTask& task) noexcept
    {
        const auto sharedSocket(weakSocket.lock());
        if (!sharedSocket)
        {
            return;
        }

        const auto lockedSocket = sharedSocket->AcquireSocketLock();
        const auto lockedPattern = lockedSocket.GetPattern();
        if (!lockedPattern)
        {
            sharedSocket->DecrementIo();
            sharedSocket->CompleteState(WSAECONNABORTED);
            return;
        }

        const auto socket = lockedSocket.GetSocket();

        int gle = NO_ERROR;
        DWORD transferred = 0;
        {
            if (socket != INVALID_SOCKET)
            {
                DWORD flags;
                if (!WSAGetOverlappedResult(socket, pOverlapped, &transferred, FALSE, &flags))
                {
                    gle = WSAGetLastError();
                }
            }
            else
            {
                gle = NO_ERROR;
            }
        }

        if (gle == WSAEMSGSIZE)
        {
            ctsConfig::PrintErrorInfo(
                L"MediaStream Receiver: %ws failed with WSAEMSGSIZE: received [%u bytes]",
                task.m_ioAction == ctsTaskAction::Recv ? L"WSARecvFrom" : L"WSASendTo", transferred);
            gle = NO_ERROR;
        }

        switch (const ctsIoStatus protocolStatus = lockedPattern->CompleteIo(task, transferred, gle))
        {
        case ctsIoStatus::ContinueIo:
            {
                // If this completion was a receive, check for control frames (SYN/SYN-ACK/ACK)
                if (task.m_ioAction == ctsTaskAction::Recv && transferred >= (c_udpDatagramDataHeaderLength + c_udpDatagramControlFixedBodyLength))
                {
                    const auto protocol = ctsMediaStreamMessage::GetProtocolHeaderFromTask(task);
                        if (protocol == c_udpDatagramFlagSyn)
                        {
                            // parse and optionally extract connection id
                            char connectionId[ctsStatistics::ConnectionIdLength]{};
                            if (ctsMediaStreamMessage::ParseControlFrame(connectionId, task, transferred))
                            {
                                // record handshake state per remote address so we can track progress
                                const auto& remoteAddr = sharedSocket->GetRemoteSockaddr();
                                std::wstring remoteKey = remoteAddr.writeCompleteAddress();
                                auto& info = g_handshakeMap[remoteKey];
                                info.state = HandshakeState::SynReceived;
                                info.lastUpdate = std::chrono::steady_clock::now();
                                if (strlen(connectionId) > 0)
                                {
                                    info.assignedConnectionId = std::string(connectionId, ctsStatistics::ConnectionIdLength);
                                }

                                // build a SYN-ACK response using the same buffer length
                                const uint32_t sendBufferLength = c_udpDatagramDataHeaderLength + c_udpDatagramControlFixedBodyLength;
                                // allocate a temporary buffer on the stack to build the SYN-ACK
                                std::vector<char> tempBuffer(sendBufferLength);
                                ctsTask rawTask;
                                rawTask.m_ioAction = ctsTaskAction::Send;
                                rawTask.m_buffer = tempBuffer.data();
                                rawTask.m_bufferLength = sendBufferLength;
                                rawTask.m_bufferOffset = 0;
                                rawTask.m_bufferType = ctsTask::BufferType::Static;
                                rawTask.m_trackIo = false;

                                // Generate a responder-assigned connection id and store it
                                char assignedId[ctsStatistics::ConnectionIdLength]{};
                                // Use the existing statistics helper to generate a UUID string
                                {
                                    ctsUdpStatistics tmpStats;
                                    ctsStatistics::GenerateConnectionId(tmpStats);
                                    memcpy_s(assignedId, ctsStatistics::ConnectionIdLength, tmpStats.m_connectionIdentifier, ctsStatistics::ConnectionIdLength);
                                }
                                info.assignedConnectionId = std::string(assignedId, ctsStatistics::ConnectionIdLength);

                                // Build a SYN-ACK including the assigned connection id
                                auto synAckTask = ctsMediaStreamMessage::MakeSynAckTask(rawTask, true, assignedId);
                                // MakeSynAckTask sets Dynamic bufferType; override to Static since we're using a local buffer
                                synAckTask.m_bufferType = ctsTask::BufferType::Static;

                                // send immediately using low-level send helper
                                const auto response = ctsWSASendTo(sharedSocket, lockedSocket.GetSocket(), synAckTask, [](OVERLAPPED*) noexcept {});
                                if (response.m_errorCode != 0 && response.m_errorCode != WSA_IO_PENDING)
                                {
                                    ctsConfig::PrintErrorIfFailed("SYN-ACK send", response.m_errorCode);
                                }
                            }
                        }
                }

                IoImplStatus status;
                do
                {
                    status = ctsMediaStreamReceiverIoImpl(
                        sharedSocket,
                        lockedSocket.GetSocket(),
                        lockedPattern,
                        lockedPattern->InitiateIo());
                }
                while (status.m_continueIo);

                gle = status.m_errorCode;
                break;
            }

        case ctsIoStatus::CompletedIo:
            sharedSocket->CloseSocket();
            gle = NO_ERROR;
            break;

        case ctsIoStatus::FailedIo:
            if (gle != 0)
            {
                ctsConfig::PrintErrorInfo(
                    L"MediaStream Receiver: IO failed (%ws) with error %d",
                    task.m_ioAction == ctsTaskAction::Recv ? L"WSARecvFrom" : L"WSASendTo", gle);
            }
            else
            {
                ctsConfig::PrintErrorInfo(
                    L"MediaStream Receiver: IO succeeded (%ws) but the ctsIOProtocol failed the stream (%u)",
                    task.m_ioAction == ctsTaskAction::Recv ? L"WSARecvFrom" : L"WSASendTo",
                    lockedPattern->GetLastPatternError());
            }

            sharedSocket->CloseSocket();
            gle = static_cast<int>(lockedPattern->GetLastPatternError());
            break;

        default:
            FAIL_FAST_MSG(
                "ctsMediaStreamReceiverIoCompletionCallback: unknown ctsSocket::IOStatus - %d\n",
                protocolStatus);
        }

        if (sharedSocket->DecrementIo() == 0)
        {
            sharedSocket->CompleteState(gle);
        }
    }


    void ctsMediaStreamReceiverConnectionCompletionCallback(
        _In_ OVERLAPPED* pOverlapped,
        const std::weak_ptr<ctsSocket>& weakSocket,
        const ctl::ctSockaddr& targetAddress) noexcept
    {
        const auto sharedSocket(weakSocket.lock());
        if (!sharedSocket)
        {
            return;
        }

        int gle = NO_ERROR;
        const auto lockedSocket = sharedSocket->AcquireSocketLock();
        const auto socket = lockedSocket.GetSocket();
        if (socket == INVALID_SOCKET)
        {
            gle = WSAECONNABORTED;
        }
        else
        {
            DWORD transferred;
            DWORD flags;
            if (!WSAGetOverlappedResult(socket, pOverlapped, &transferred, FALSE, &flags))
            {
                gle = WSAGetLastError();
            }
        }

        ctsConfig::PrintErrorIfFailed("\tWSASendTo (START request)", gle);

        if (NO_ERROR == gle)
        {
            ctl::ctSockaddr localAddr;
            int localAddrLen = localAddr.length();
            if (0 == getsockname(socket, localAddr.sockaddr(), &localAddrLen))
            {
                sharedSocket->SetLocalSockaddr(localAddr);
            }
            sharedSocket->SetRemoteSockaddr(targetAddress);
            ctsConfig::SetPostConnectOptions(socket, targetAddress);
            ctsConfig::PrintNewConnection(localAddr, targetAddress);
        }

        sharedSocket->CompleteState(gle);
    }

} // namespace
