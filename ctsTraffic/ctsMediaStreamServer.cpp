/*

Copyright (c) Microsoft Corporation
All rights reserved.

Licensed under the Apache License, Version 2.0 (the ""License""); you may not use this file except in compliance with the License. You may obtain a copy of the License at http://www.apache.org/licenses/LICENSE-2.0

THIS CODE IS PROVIDED ON AN  *AS IS* BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, EITHER EXPRESS OR IMPLIED, INCLUDING WITHOUT LIMITATION ANY IMPLIED WARRANTIES OR CONDITIONS OF TITLE, FITNESS FOR A PARTICULAR PURPOSE, MERCHANTABLITY OR NON-INFRINGEMENT.

See the Apache Version 2.0 License for specific language governing permissions and limitations under the License.

*/

#include <unordered_map>
// cpp headers
#include <memory>
#include <vector>
#include <algorithm>
// os headers
#include <Windows.h>
#include <WinSock2.h>
#include <intrin.h>
// ctl headers
#include <ctSockaddr.hpp>
#include <ctThreadIocp.hpp>
#include <ctThreadIocp_shard.hpp>
#include <ctCpuAffinity.hpp>
#include <mstcpip.h>
// project headers
#include "ctsConfig.h"
#include "ctsSocket.h"
#include "ctsIOTask.hpp"
#include "ctsWinsockLayer.h"
#include "ctsMediaStreamServer.h"
#include "ctsMediaStreamServerConnectedSocket.h"
#include "ctsMediaStreamServerListeningSocket.h"
#include "ctsMediaStreamProtocol.hpp"
// wil headers always included last
#include <wil/stl.h>
#include <wil/resource.h>

namespace ctsTraffic
{
    // Called to 'accept' incoming connections
    void ctsMediaStreamServerListener(const std::weak_ptr<ctsSocket>& weakSocket) noexcept try
    {
        ctsMediaStreamServerImpl::InitOnce();
        // ctsMediaStreamServerImpl will complete the ctsSocket object
        // when a client request comes in to be 'accepted'
        ctsMediaStreamServerImpl::AcceptSocket(weakSocket);
    }
    catch (...)
    {
        const auto error = ctsConfig::PrintThrownException();
        if (const auto sharedSocket = weakSocket.lock())
        {
            sharedSocket->CompleteState(error);
        }
    }

    // Called initiate IO on a datagram socket
    void ctsMediaStreamServerIo(const std::weak_ptr<ctsSocket>& weakSocket) noexcept
    {
        const auto sharedSocket(weakSocket.lock());
        if (!sharedSocket)
        {
            return;
        }

        // hold a reference on the socket
        const auto lockedSocket = sharedSocket->AcquireSocketLock();
        const auto lockedPattern = lockedSocket.GetPattern();
        if (!lockedPattern)
        {
            return;
        }

        ctsTask nextTask;
        try
        {
            ctsMediaStreamServerImpl::InitOnce();

            do
            {
                nextTask = lockedPattern->InitiateIo();
                if (nextTask.m_ioAction != ctsTaskAction::None)
                {
                    // Inline scheduling logic (previously in ctsMediaStreamServerImpl::ScheduleIo)
                    auto sharedSocketLocal = weakSocket.lock();
                    if (!sharedSocketLocal)
                    {
                        THROW_WIN32_MSG(WSAECONNABORTED, "ctsSocket already freed");
                    }

                    const auto sharedConnectedSocket = ctsMediaStreamServerImpl::FindConnectedSocket(sharedSocketLocal->GetRemoteSockaddr());
                    if (!sharedConnectedSocket)
                    {
                        ctsConfig::PrintErrorInfo(
                            L"ctsMediaStreamServer - failed to find the socket with remote address %ws in our connected socket list to continue sending datagrams",
                            sharedSocketLocal->GetRemoteSockaddr().writeCompleteAddress().c_str());
                        THROW_WIN32_MSG(ERROR_INVALID_DATA, "ctsSocket was not found in the connected sockets to continue sending datagrams");
                    }

                    // Call into connected socket without holding the global lock
                    sharedConnectedSocket->QueueTask(nextTask);
                }
            } while (nextTask.m_ioAction != ctsTaskAction::None);
        }
        catch (...)
        {
            const auto error = ctsConfig::PrintThrownException();
            if (nextTask.m_ioAction != ctsTaskAction::None)
            {
                // must complete any IO that was requested but not scheduled
                lockedPattern->CompleteIo(nextTask, 0, error);
                if (0 == sharedSocket->GetPendedIoCount())
                {
                    sharedSocket->CompleteState(error);
                }
            }
        }
    }

    // Called to remove that socket from the tracked vector of connected sockets
    void ctsMediaStreamServerClose(const std::weak_ptr<ctsSocket>& weakSocket) noexcept try
    {
        ctsMediaStreamServerImpl::InitOnce();

        if (const auto sharedSocket = weakSocket.lock())
        {
            ctsMediaStreamServerImpl::RemoveSocket(sharedSocket->GetRemoteSockaddr());
        }
    }
    catch (...)
    {
    }


    namespace ctsMediaStreamServerImpl
    {
        // The actual send logic was moved into ctsMediaStreamServerConnectedSocket::PerformIo()

        static std::vector<std::unique_ptr<ctsMediaStreamServerListeningSocket>> g_listeningSockets; // NOLINT(clang-diagnostic-exit-time-destructors)

        static wil::critical_section g_socketVectorGuard{ ctsConfig::ctsConfigSettings::c_CriticalSectionSpinlock }; // NOLINT(cppcoreguidelines-interfaces-global-init, clang-diagnostic-exit-time-destructors)

        _Guarded_by_(g_socketVectorGuard) static std::unordered_map<ctl::ctSockaddr, std::shared_ptr<ctsMediaStreamServerConnectedSocket>> g_connectedSockets; // NOLINT(clang-diagnostic-exit-time-destructors)

        // weak_ptr<> to ctsSocket objects ready to accept a connection
        _Guarded_by_(g_socketVectorGuard) static std::vector<std::weak_ptr<ctsSocket>> g_acceptingSockets; // NOLINT(clang-diagnostic-exit-time-destructors)

        // endpoints that have been received from clients not yet matched to ctsSockets
        _Guarded_by_(g_socketVectorGuard) static std::vector<std::pair<SOCKET, ctl::ctSockaddr>> g_awaitingEndpoints; // NOLINT(clang-diagnostic-exit-time-destructors)


        // Singleton values used as the actual implementation for every 'connection'

        // ReSharper disable once CppZeroConstantCanBeReplacedWithNullptr
        static INIT_ONCE g_initImpl = INIT_ONCE_STATIC_INIT;

        static BOOL CALLBACK InitOnceImpl(PINIT_ONCE, PVOID, PVOID*) noexcept try
        {
            // 'listen' to each address
            for (const auto& addr : ctsConfig::g_configSettings->ListenAddresses)
            {
                if (ctsConfig::g_configSettings->EnableRecvSharding)
                {
                    const auto shardCount = ctsConfig::GetShardCount();
                    const auto affinityPolicy = ctsConfig::GetCpuAffinityPolicy();

                    // compute per-shard affinities (round-robin per-cpu)
                    std::optional<std::vector<ctl::GroupAffinity>> maybeAffinities = ctl::ComputeShardAffinities(shardCount, affinityPolicy);

                    for (uint32_t shard = 0; shard < shardCount; ++shard)
                    {
                        wil::unique_socket listening(ctsConfig::CreateSocket(addr.family(), SOCK_DGRAM, IPPROTO_UDP, ctsConfig::g_configSettings->SocketFlags));

                        auto error = ctsConfig::SetPreBindOptions(listening.get(), addr);
                        if (error != NO_ERROR)
                        {
                            THROW_WIN32_MSG(error, "SetPreBindOptions (ctsMediaStreamServer)");
                        }

                        // capture the socket value before moved into the vector
                        const SOCKET listeningSocketToPrint(listening.get());

                        // If we have affinity mapping, attempt to apply SIO_CPU_AFFINITY for this socket
                        if (maybeAffinities && shard < static_cast<uint32_t>(maybeAffinities->size()))
                        {
                            const ctl::GroupAffinity& ga = (*maybeAffinities)[shard];
                            // Per MSDN, SIO_CPU_AFFINITY expects a USHORT processor index (0-based)
                            // as its input buffer. Compute the local processor index from the
                            // GroupAffinity mask (single-bit expected) using a CPU-friendly
                            // bit-scan instruction and pass that as a USHORT.
                            unsigned int localIndex = 0;
                            const KAFFINITY mask = ga.Mask;
                            unsigned long idx = 0;
#if defined(__MACHINEX86)
							if (_BitScanForward(&idx, static_cast<unsigned long>(mask)))
							{
								localIndex = static_cast<unsigned int>(idx);
							}
#else
                            if (_BitScanForward64(&idx, static_cast<unsigned long long>(mask)))
                            {
                                localIndex = static_cast<unsigned int>(idx);
                            }
#endif
                            USHORT processorIndex = static_cast<USHORT>(localIndex);
                            DWORD bytes = 0;
                            const int rc = WSAIoctl(listening.get(), SIO_CPU_AFFINITY, &processorIndex, sizeof(processorIndex), nullptr, 0, &bytes, nullptr, nullptr);
                            if (rc != 0)
                            {
                                const auto erl = WSAGetLastError();
                                ctsConfig::PrintErrorInfo(L"WSAIoctl(SIO_CPU_AFFINITY) failed for shard %d with error %d", shard, erl);
                            }
                            else
                            {
                                PRINT_DEBUG_INFO(L"ctsMediaStreamServer - applied SIO_CPU_AFFINITY (group %u, cpu %u) to socket %Iu\n", ga.Group, processorIndex, listeningSocketToPrint);
                            }
                        }

                        if (SOCKET_ERROR == bind(listening.get(), addr.sockaddr(), addr.length()))
                        {
                            error = WSAGetLastError();
                            char addrBuffer[ctl::ctSockaddr::FixedStringLength]{};
                            addr.writeAddress(addrBuffer);
                            THROW_WIN32_MSG(error, "bind %hs (ctsMediaStreamServer)", addrBuffer);
                        }

                        // create a shard implementation for this socket with configured worker count
                        // pass the IOCP batch size from config; only applicable when sharding is enabled
                        // If we computed per-shard affinities, pass the shard's affinity so worker threads
                        // are affinitized consistently with the socket's SIO_CPU_AFFINITY above.
                        std::vector<ctl::GroupAffinity> workerAffinities;
                        if (maybeAffinities && shard < static_cast<uint32_t>(maybeAffinities->size()))
                        {
                            workerAffinities.push_back((*maybeAffinities)[shard]);
                        }

                        const auto threadIocp = std::make_shared<ctl::ctThreadIocp_shard>(
                            listening.get(),
                            static_cast<size_t>(ctsConfig::g_configSettings->ShardWorkerCount),
                            std::move(workerAffinities),
                            static_cast<size_t>(ctsConfig::g_configSettings->IocpBatchSize));

                        g_listeningSockets.emplace_back(
                            std::make_unique<ctsMediaStreamServerListeningSocket>(
                                std::move(listening),
                                addr,
                                threadIocp,
                                [](SOCKET socket, const ctl::ctSockaddr& local, const ctl::ctSockaddr& remote, const char* buf, uint32_t len) {
                                    ctsMediaStreamServerImpl::OnPacketReceived(socket, local, remote, buf, len);
                                }));

                        PRINT_DEBUG_INFO(
                            L"\t\tctsMediaStreamServer - Receiving datagrams on %ws (shard %d, %Iu)\n",
                            addr.writeCompleteAddress().c_str(),
                            shard,
                            listeningSocketToPrint);
                    }
                }
                else
                {
                    wil::unique_socket listening(ctsConfig::CreateSocket(addr.family(), SOCK_DGRAM, IPPROTO_UDP, ctsConfig::g_configSettings->SocketFlags));

                    auto error = ctsConfig::SetPreBindOptions(listening.get(), addr);
                    if (error != NO_ERROR)
                    {
                        THROW_WIN32_MSG(error, "SetPreBindOptions (ctsMediaStreamServer)");
                    }

                    if (SOCKET_ERROR == bind(listening.get(), addr.sockaddr(), addr.length()))
                    {
                        error = WSAGetLastError();
                        char addrBuffer[ctl::ctSockaddr::FixedStringLength]{};
                        addr.writeAddress(addrBuffer);
                        THROW_WIN32_MSG(error, "bind %hs (ctsMediaStreamServer)", addrBuffer);
                    }

                    // capture the socket value before moved into the vector
                    const SOCKET listeningSocketToPrint(listening.get());

                    // non-sharded: original behavior (threadpool IO manager)
                    const auto threadIocp = std::make_shared<ctl::ctThreadIocp>(listening.get(), ctsConfig::g_configSettings->pTpEnvironment);

                    g_listeningSockets.emplace_back(
                        std::make_unique<ctsMediaStreamServerListeningSocket>(
                            std::move(listening),
                            addr,
                            threadIocp,
                            [](SOCKET socket, const ctl::ctSockaddr& local, const ctl::ctSockaddr& remote, const char* buf, uint32_t len) {
                                ctsMediaStreamServerImpl::OnPacketReceived(socket, local, remote, buf, len);
                            }));

                    PRINT_DEBUG_INFO(
                        L"\t\tctsMediaStreamServer - Receiving datagrams on %ws (%Iu)\n",
                        addr.writeCompleteAddress().c_str(),
                        listeningSocketToPrint);
                }
            }

            if (g_listeningSockets.empty())
            {
                throw std::exception("ctsMediaStreamServer invoked with no listening addresses specified");
            }

            // initiate the recv's in the 'listening' sockets
            for (const auto& listener : g_listeningSockets)
            {
                listener->Start();
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
                throw std::runtime_error("ctsMediaStreamServerListener could not be instantiated");
            }
        }

        std::vector<ListenerInfo> GetListenerInfos() noexcept
        {
            std::vector<ListenerInfo> infos;
            infos.reserve(g_listeningSockets.size());
            for (size_t i = 0; i < g_listeningSockets.size(); ++i)
            {
                infos.push_back(ListenerInfo{
                    g_listeningSockets[i]->GetListeningAddress(),
                    g_listeningSockets[i]->GetConnectionCount(),
                    i
                });
            }
            return infos;
        }

        std::shared_ptr<ctsMediaStreamServerConnectedSocket> FindConnectedSocket(const ctl::ctSockaddr& remoteAddr) noexcept
        {
            const auto lockConnectedObject = g_socketVectorGuard.lock();
            const auto foundSocket = g_connectedSockets.find(remoteAddr);
            if (foundSocket == g_connectedSockets.end())
            {
                return nullptr;
            }
            return foundSocket->second;
        }

        // Scheduling inlined into ctsMediaStreamServerIo

        // Process a new ctsSocket from the ctsSocketBroker
        // - accept_socket takes the ctsSocket to create a new entry
        //   which will create a corresponding ctsMediaStreamServerConnectedSocket in the process
        void AcceptSocket(const std::weak_ptr<ctsSocket>& weakSocket)
        {
            if (const auto sharedSocket = weakSocket.lock())
            {
                const auto lockAwaitingObject = g_socketVectorGuard.lock();

                if (g_awaitingEndpoints.empty())
                {
                    // just add it to our accepting sockets vector under the writer lock
                    g_acceptingSockets.push_back(weakSocket);
                }
                else
                {
                    auto waitingEndpoint = g_awaitingEndpoints.rbegin();

                    const ctl::ctSockaddr waitingKey = waitingEndpoint->second;

                    const auto existingSocket = FindConnectedSocket(waitingKey);

                    if (existingSocket)
                    {
                        ctsConfig::g_configSettings->UdpStatusDetails.m_duplicateFrames.Increment();
                        PRINT_DEBUG_INFO(L"\t\tctsMediaStreamServer::accept_socket - socket with remote address %ws asked to be Started but was already established",
                            waitingEndpoint->second.writeCompleteAddress().c_str());
                        // return early if this was a duplicate request: this can happen if there is latency or drops
                        // between the client and server as they attempt to negotiating starting a new stream
                        return;
                    }

                    g_connectedSockets.emplace(
                        waitingKey,
                        std::make_shared<ctsMediaStreamServerConnectedSocket>(
                            weakSocket,
                            waitingEndpoint->first,
                            waitingEndpoint->second));

                    PRINT_DEBUG_INFO(L"\t\tctsMediaStreamServer::accept_socket - socket with remote address %ws added to connected_sockets",
                        waitingEndpoint->second.writeCompleteAddress().c_str());

                    // now complete the ctsSocket 'Create' request
                    const auto foundSocket = std::ranges::find_if(
                        g_listeningSockets,
                        [&waitingEndpoint](const std::unique_ptr<ctsMediaStreamServerListeningSocket>& listener) noexcept {
                            return listener->GetSocket() == waitingEndpoint->first;
                        });
                    FAIL_FAST_IF_MSG(
                        foundSocket == g_listeningSockets.end(),
                        "Could not find the socket (%Iu) in the waiting_endpoint from our listening sockets (%p)\n",
                        waitingEndpoint->first, &g_listeningSockets);


                ctsConfig::SetPostConnectOptions(sharedSocket->AcquireSocketLock().GetSocket(), waitingEndpoint->second);

                sharedSocket->SetLocalSockaddr((*foundSocket)->GetListeningAddress());
                sharedSocket->SetRemoteSockaddr(waitingEndpoint->second);
                sharedSocket->CompleteState(NO_ERROR);

                    ctsConfig::PrintNewConnection(sharedSocket->GetLocalSockaddr(), sharedSocket->GetRemoteSockaddr());
                    // if added to connected_sockets, can then safely remove it from the waiting endpoint
                    g_awaitingEndpoints.pop_back();
                }
            }
        }

        // Process the removal of a connected socket once it is completed
        // - remove_socket takes the remote address to find the socket
        void RemoveSocket(const ctl::ctSockaddr& targetAddr)
        {
            const auto foundSocket = FindConnectedSocket(targetAddr);
            if (foundSocket)
            {
                const auto lockConnectedObject = g_socketVectorGuard.lock();
                const auto it = g_connectedSockets.find(targetAddr);
                if (it != g_connectedSockets.end())
                {
                    g_connectedSockets.erase(it);
                }
            }
        }

        // Notified by listening sockets when any datagram is received.
        // The server implementation is responsible for parsing the packet contents
        // and taking appropriate action (for example, calling `Start` when a START
        // message is received).
        void OnPacketReceived(SOCKET socket, const ctl::ctSockaddr& localAddr, const ctl::ctSockaddr& remoteAddr, const char* buffer, uint32_t bufferLength)
        {
            try
            {
                const ctsMediaStreamMessage message = ctsMediaStreamMessage::Extract(buffer, bufferLength);
                switch (message.m_action)
                {
                    case MediaStreamAction::START:
                        PRINT_DEBUG_INFO(L"\t\tctsMediaStreamServer - parsed START from %ws\n", remoteAddr.writeCompleteAddress().c_str());
                        Start(socket, localAddr, remoteAddr);
                        break;

                    default:
                        ctsConfig::PrintErrorInfo(L"ctsMediaStreamServer::OnPacketReceived - unexpected action %d from %ws", static_cast<int>(message.m_action), remoteAddr.writeCompleteAddress().c_str());
                }
            }
            catch (...)
            {
                ctsConfig::PrintThrownException();
            }
        }

        // Processes the incoming START request from the client
        // - if we have a waiting ctsSocket to accept it, will add it to connected_sockets
        // - else we'll queue it to awaiting_endpoints
        void Start(SOCKET socket, const ctl::ctSockaddr& localAddr, const ctl::ctSockaddr& targetAddr)
        {
            const auto lockAwaitingObject = g_socketVectorGuard.lock();

            const ctl::ctSockaddr targetKey = targetAddr;
            const auto existingSocket = FindConnectedSocket(targetKey);
            if (existingSocket)
            {
                ctsConfig::g_configSettings->UdpStatusDetails.m_duplicateFrames.Increment();
                PRINT_DEBUG_INFO(L"\t\tctsMediaStreamServer::start - socket with remote address %ws asked to be Started but was already in connected_sockets",
                    targetAddr.writeCompleteAddress().c_str());
                // return early if this was a duplicate request: this can happen if there is latency or drops
                // between the client and server as they attempt to negotiating starting a new stream
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
                PRINT_DEBUG_INFO(L"\t\tctsMediaStreamServer::start - socket with remote address %ws asked to be Started but was already in awaiting endpoints",
                    targetAddr.writeCompleteAddress().c_str());
                // return early if this was a duplicate request: this can happen if there is latency or drops
                // between the client and server as they attempt to negotiating starting a new stream
                return;
            }

            // find a ctsSocket waiting to 'accept' a connection and complete it
            auto addToAwaiting = true;
            while (!g_acceptingSockets.empty())
            {
                auto weakInstance = *g_acceptingSockets.rbegin();
                if (const auto sharedInstance = weakInstance.lock())
                {
                    // 'move' the accepting socket to connected
                    g_connectedSockets.emplace(
                        targetKey,
                        std::make_shared<ctsMediaStreamServerConnectedSocket>(weakInstance, socket, targetAddr));

                    PRINT_DEBUG_INFO(L"\t\tctsMediaStreamServer::start - socket with remote address %ws added to connected_sockets",
                        targetAddr.writeCompleteAddress().c_str());

                    // verify is successfully added to connected_sockets before popping off accepting_sockets
                    addToAwaiting = false;
                    g_acceptingSockets.pop_back();

                ctsConfig::SetPostConnectOptions(socket, targetAddr);

                // now complete the accepted ctsSocket back to the ctsSocketState
                sharedInstance->SetLocalSockaddr(localAddr);
                sharedInstance->SetRemoteSockaddr(targetAddr);
                sharedInstance->CompleteState(NO_ERROR);

                    ctsConfig::PrintNewConnection(localAddr, targetAddr);
                    break;
                }
            }

            // if we didn't find a waiting connection to accept it, queue it for when one arrives later
            if (addToAwaiting)
            {
                PRINT_DEBUG_INFO(L"\t\tctsMediaStreamServer::start - socket with remote address %ws added to awaiting_endpoints",
                    targetAddr.writeCompleteAddress().c_str());

                // only queue it if we aren't already waiting on this address
                g_awaitingEndpoints.emplace_back(socket, targetAddr);
            }
        }

        // ConnectedSocketIo has been moved into ctsMediaStreamServerConnectedSocket::PerformIo()
    }
}
