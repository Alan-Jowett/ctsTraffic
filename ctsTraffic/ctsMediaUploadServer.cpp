/*
Upload server implementation - based on ctsMediaStreamServer.cpp
*/
// cpp headers
#include <memory>
#include <vector>
#include <algorithm>
// os headers
#include <Windows.h>
#include <WinSock2.h>
// ctl headers
#include <ctSockaddr.hpp>
// project headers
#include "ctsConfig.h"
#include "ctsSocket.h"
#include "ctsIOTask.hpp"
#include "ctsWinsockLayer.h"
#include "ctsMediaUploadServer.h"
#include "ctsMediaUploadServerConnectedSocket.h"
#include "ctsMediaUploadServerListeningSocket.h"
#include "ctsMediaUploadProtocol.hpp"
// wil headers always included last
#include <wil/stl.h>
#include <wil/resource.h>

namespace ctsTraffic
{
    void ctsMediaUploadServerListener(const std::weak_ptr<ctsSocket>& weakSocket) noexcept try
    {
        ctsMediaUploadServerImpl::InitOnce();
        ctsMediaUploadServerImpl::AcceptSocket(weakSocket);
    }
    catch (...)
    {
        const auto error = ctsConfig::PrintThrownException();
        if (const auto sharedSocket = weakSocket.lock())
        {
            sharedSocket->CompleteState(error);
        }
    }

    void ctsMediaUploadServerIo(const std::weak_ptr<ctsSocket>& weakSocket) noexcept
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
            return;
        }

        ctsTask nextTask;
        try
        {
            ctsMediaUploadServerImpl::InitOnce();

            do
            {
                nextTask = lockedPattern->InitiateIo();
                if (nextTask.m_ioAction != ctsTaskAction::None)
                {
                    ctsMediaUploadServerImpl::ScheduleIo(weakSocket, nextTask);
                }
            } while (nextTask.m_ioAction != ctsTaskAction::None);
        }
        catch (...)
        {
            const auto error = ctsConfig::PrintThrownException();
            if (nextTask.m_ioAction != ctsTaskAction::None)
            {
                lockedPattern->CompleteIo(nextTask, 0, error);
                if (0 == sharedSocket->GetPendedIoCount())
                {
                    sharedSocket->CompleteState(error);
                }
            }
        }
    }

    void ctsMediaUploadServerClose(const std::weak_ptr<ctsSocket>& weakSocket) noexcept try
    {
        ctsMediaUploadServerImpl::InitOnce();

        if (const auto sharedSocket = weakSocket.lock())
        {
            ctsMediaUploadServerImpl::RemoveSocket(sharedSocket->GetRemoteSockaddr());
        }
    }
    catch (...)
    {
    }

    namespace ctsMediaUploadServerImpl
    {
        static wsIOResult ConnectedSocketIo(_In_ ctsMediaUploadServerConnectedSocket* connectedSocket) noexcept;

        static std::vector<std::unique_ptr<ctsMediaUploadServerListeningSocket>> g_listeningSockets;

        static wil::critical_section g_socketVectorGuard{ ctsConfig::ctsConfigSettings::c_CriticalSectionSpinlock };

        _Guarded_by_(g_socketVectorGuard) static std::vector<std::shared_ptr<ctsMediaUploadServerConnectedSocket>> g_connectedSockets;

        _Guarded_by_(g_socketVectorGuard) static std::vector<std::weak_ptr<ctsSocket>> g_acceptingSockets;

        _Guarded_by_(g_socketVectorGuard) static std::vector<std::pair<SOCKET, ctl::ctSockaddr>> g_awaitingEndpoints;

        static INIT_ONCE g_initImpl = INIT_ONCE_STATIC_INIT;

        static BOOL CALLBACK InitOnceImpl(PINIT_ONCE, PVOID, PVOID*) noexcept try
        {
            for (const auto& addr : ctsConfig::g_configSettings->ListenAddresses)
            {
                wil::unique_socket listening(ctsConfig::CreateSocket(addr.family(), SOCK_DGRAM, IPPROTO_UDP, ctsConfig::g_configSettings->SocketFlags));

                auto error = ctsConfig::SetPreBindOptions(listening.get(), addr);
                if (error != NO_ERROR)
                {
                    THROW_WIN32_MSG(error, "SetPreBindOptions (ctsMediaUploadServer)");
                }

                if (SOCKET_ERROR == bind(listening.get(), addr.sockaddr(), addr.length()))
                {
                    error = WSAGetLastError();
                    char addrBuffer[ctl::ctSockaddr::FixedStringLength]{};
                    addr.writeAddress(addrBuffer);
                    THROW_WIN32_MSG(error, "bind %hs (ctsMediaUploadServer)", addrBuffer);
                }

                const SOCKET listeningSocketToPrint(listening.get());
                g_listeningSockets.emplace_back(
                    std::make_unique<ctsMediaUploadServerListeningSocket>(std::move(listening), addr));
                PRINT_DEBUG_INFO(
                    L"\t\tctsMediaUploadServer - Receiving datagrams on %ws (%Iu)\n",
                    addr.writeCompleteAddress().c_str(),
                    listeningSocketToPrint);
            }

            if (g_listeningSockets.empty())
            {
                throw std::exception("ctsMediaUploadServer invoked with no listening addresses specified");
            }

            for (const auto& listener : g_listeningSockets)
            {
                listener->InitiateRecv();
            }

            return TRUE;
        }
        catch (...)
        {
            ctsConfig::PrintThrownException();
            return FALSE;
        }

        void InitOnce()
        {
            if (!InitOnceExecuteOnce(&g_initImpl, InitOnceImpl, nullptr, nullptr))
            {
                throw std::runtime_error("ctsMediaUploadServerListener could not be instantiated");
            }
        }

        void ScheduleIo(const std::weak_ptr<ctsSocket>& weakSocket, const ctsTask& task)
        {
            auto sharedSocket = weakSocket.lock();
            if (!sharedSocket)
            {
                THROW_WIN32_MSG(WSAECONNABORTED, "ctsSocket already freed");
            }

            std::shared_ptr<ctsMediaUploadServerConnectedSocket> sharedConnectedSocket;
            {
                const auto lockConnectedObject = g_socketVectorGuard.lock();

                const auto foundSocket = std::ranges::find_if(
                    g_connectedSockets,
                    [&sharedSocket](const std::shared_ptr<ctsMediaUploadServerConnectedSocket>& connectedSocket) noexcept {
                        return sharedSocket->GetRemoteSockaddr() == connectedSocket->GetRemoteAddress();
                    }
                );

                if (foundSocket == std::end(g_connectedSockets))
                {
                    ctsConfig::PrintErrorInfo(
                        L"ctsMediaUploadServer - failed to find the socket with remote address %ws in our connected socket list to continue sending datagrams",
                        sharedSocket->GetRemoteSockaddr().writeCompleteAddress().c_str());
                    THROW_WIN32_MSG(ERROR_INVALID_DATA, "ctsSocket was not found in the connected sockets to continue sending datagrams");
                }

                sharedConnectedSocket = *foundSocket;
            }
            sharedConnectedSocket->ScheduleTask(task);
        }

        void AcceptSocket(const std::weak_ptr<ctsSocket>& weakSocket)
        {
            if (const auto sharedSocket = weakSocket.lock())
            {
                const auto lockAwaitingObject = g_socketVectorGuard.lock();

                if (g_awaitingEndpoints.empty())
                {
                    g_acceptingSockets.push_back(weakSocket);
                }
                else
                {
                    auto waitingEndpoint = g_awaitingEndpoints.rbegin();

                    const auto existingSocket = std::ranges::find_if(
                        g_connectedSockets,
                        [&](const std::shared_ptr<ctsMediaUploadServerConnectedSocket>& connectedSocket) noexcept {
                            return waitingEndpoint->second == connectedSocket->GetRemoteAddress();
                        });

                    if (existingSocket != std::end(g_connectedSockets))
                    {
                        ctsConfig::g_configSettings->UdpStatusDetails.m_duplicateFrames.Increment();
                        PRINT_DEBUG_INFO(L"\t\tctsMediaUploadServer::accept_socket - socket with remote address %ws asked to be Started but was already established",
                            waitingEndpoint->second.writeCompleteAddress().c_str());
                        return;
                    }

                    g_connectedSockets.emplace_back(
                        std::make_shared<ctsMediaUploadServerConnectedSocket>(
                            weakSocket,
                            waitingEndpoint->first,
                            waitingEndpoint->second,
                            ConnectedSocketIo));

                    PRINT_DEBUG_INFO(L"\t\tctsMediaUploadServer::accept_socket - socket with remote address %ws added to connected_sockets",
                        waitingEndpoint->second.writeCompleteAddress().c_str());

                    const auto foundSocket = std::ranges::find_if(
                        g_listeningSockets,
                        [&waitingEndpoint](const std::unique_ptr<ctsMediaUploadServerListeningSocket>& listener) noexcept {
                            return listener->GetSocket() == waitingEndpoint->first;
                        });
                    FAIL_FAST_IF_MSG(
                        foundSocket == g_listeningSockets.end(),
                        "Could not find the socket (%Iu) in the waiting_endpoint from our listening sockets (%p)",
                        waitingEndpoint->first, &g_listeningSockets);

                    ctsConfig::SetPostConnectOptions(sharedSocket->AcquireSocketLock().GetSocket(), waitingEndpoint->second);

                    sharedSocket->SetLocalSockaddr((*foundSocket)->GetListeningAddress());
                    sharedSocket->SetRemoteSockaddr(waitingEndpoint->second);
                    sharedSocket->CompleteState(NO_ERROR);

                    ctsConfig::PrintNewConnection(sharedSocket->GetLocalSockaddr(), sharedSocket->GetRemoteSockaddr());
                    g_awaitingEndpoints.pop_back();
                }
            }
        }

        void RemoveSocket(const ctl::ctSockaddr& targetAddr)
        {
            const auto lockConnectedObject = g_socketVectorGuard.lock();

            const auto foundSocket = std::ranges::find_if(
                g_connectedSockets,
                [&targetAddr](const std::shared_ptr<ctsMediaUploadServerConnectedSocket>& connectedSocket) noexcept {
                    return targetAddr == connectedSocket->GetRemoteAddress();
                });

            if (foundSocket != std::end(g_connectedSockets))
            {
                g_connectedSockets.erase(foundSocket);
            }
        }

        void Start(SOCKET socket, const ctl::ctSockaddr& localAddr, const ctl::ctSockaddr& targetAddr)
        {
            const auto lockAwaitingObject = g_socketVectorGuard.lock();

            const auto existingSocket = std::ranges::find_if(
                g_connectedSockets,
                [&targetAddr](const std::shared_ptr<ctsMediaUploadServerConnectedSocket>& connectedSocket) noexcept {
                    return targetAddr == connectedSocket->GetRemoteAddress();
                });
            if (existingSocket != std::end(g_connectedSockets))
            {
                ctsConfig::g_configSettings->UdpStatusDetails.m_duplicateFrames.Increment();
                PRINT_DEBUG_INFO(L"\t\tctsMediaUploadServer::start - socket with remote address %ws asked to be Started but was already in connected_sockets",
                    targetAddr.writeCompleteAddress().c_str());
                return;
            }

            const auto awaitingEndpoint = std::ranges::find_if(
                g_awaitingEndpoints,
                [&targetAddr](const std::pair<SOCKET, ctl::ctSockaddr>& endpoint) noexcept {
                    return targetAddr == endpoint.second;
                });
            if (awaitingEndpoint != std::end(g_awaitingEndpoints))
            {
                ctsConfig::g_configSettings->UdpStatusDetails.m_duplicateFrames.Increment();
                PRINT_DEBUG_INFO(L"\t\tctsMediaUploadServer::start - socket with remote address %ws asked to be Started but was already in awaiting endpoints",
                    targetAddr.writeCompleteAddress().c_str());
                return;
            }

            auto addToAwaiting = true;
            while (!g_acceptingSockets.empty())
            {
                auto weakInstance = *g_acceptingSockets.rbegin();
                if (const auto sharedInstance = weakInstance.lock())
                {
                    g_connectedSockets.emplace_back(
                        std::make_shared<ctsMediaUploadServerConnectedSocket>(weakInstance, socket, targetAddr, ConnectedSocketIo));

                    PRINT_DEBUG_INFO(L"\t\tctsMediaUploadServer::start - socket with remote address %ws added to connected_sockets",
                        targetAddr.writeCompleteAddress().c_str());

                    addToAwaiting = false;
                    g_acceptingSockets.pop_back();

                    ctsConfig::SetPostConnectOptions(socket, targetAddr);

                    sharedInstance->SetLocalSockaddr(localAddr);
                    sharedInstance->SetRemoteSockaddr(targetAddr);
                    sharedInstance->CompleteState(NO_ERROR);

                    ctsConfig::PrintNewConnection(localAddr, targetAddr);
                    break;
                }
            }

            if (addToAwaiting)
            {
                PRINT_DEBUG_INFO(L"\t\tctsMediaUploadServer::start - socket with remote address %ws added to awaiting_endpoints",
                    targetAddr.writeCompleteAddress().c_str());

                g_awaitingEndpoints.emplace_back(socket, targetAddr);
            }
        }

        void DispatchDatagram(SOCKET listeningSocket, const ctl::ctSockaddr& remoteAddr, const char* buffer, uint32_t length)
        {
            const auto lockAwaitingObject = g_socketVectorGuard.lock();

            const auto existingSocket = std::ranges::find_if(
                g_connectedSockets,
                [&remoteAddr](const std::shared_ptr<ctsMediaUploadServerConnectedSocket>& connectedSocket) noexcept {
                    return remoteAddr == connectedSocket->GetRemoteAddress();
                });

            if (existingSocket != std::end(g_connectedSockets))
            {
                (*existingSocket)->EnqueueReceivedDatagram(buffer, length);
                return;
            }

            // If no connected socket exists yet, queue the endpoint so Start/Accept can match later
            g_awaitingEndpoints.emplace_back(listeningSocket, remoteAddr);
        }

        wsIOResult ConnectedSocketIo(_In_ ctsMediaUploadServerConnectedSocket* connectedSocket) noexcept
        {
            const SOCKET socket = connectedSocket->GetSendingSocket();
            if (INVALID_SOCKET == socket)
            {
                return wsIOResult(WSA_OPERATION_ABORTED);
            }

            const ctl::ctSockaddr& remoteAddr(connectedSocket->GetRemoteAddress());
            const ctsTask nextTask = connectedSocket->GetNextTask();

            wsIOResult returnResults;
            if (ctsTask::BufferType::UdpConnectionId == nextTask.m_bufferType)
            {
                WSABUF wsaBuffer;
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
                const auto sequenceNumber = connectedSocket->IncrementSequence();
                ctsMediaUploadSendRequests sendingRequests(
                    nextTask.m_bufferLength,
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
                        L"\t\tctsMediaUploadServer sending seq number %lld (%u sent-bytes, %u frame-bytes)\n",
                        sequenceNumber, bytesSent, returnResults.m_bytesTransferred);
                }
            }

            return returnResults;
        }
    }
} 
