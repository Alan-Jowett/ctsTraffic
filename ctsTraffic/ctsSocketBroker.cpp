/*

Copyright (c) Microsoft Corporation
All rights reserved.

Licensed under the Apache License, Version 2.0 (the ""License""); you may not use this file except in compliance with the License. You may obtain a copy of the License at http://www.apache.org/licenses/LICENSE-2.0

THIS CODE IS PROVIDED ON AN  *AS IS* BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, EITHER EXPRESS OR IMPLIED, INCLUDING WITHOUT LIMITATION ANY IMPLIED WARRANTIES OR CONDITIONS OF TITLE, FITNESS FOR A PARTICULAR PURPOSE, MERCHANTABLITY OR NON-INFRINGEMENT.

See the Apache Version 2.0 License for specific language governing permissions and limitations under the License.

*/

// parent header
#include "ctsSocketBroker.h"
// cpp headers
#include <memory>
#include <iterator>
#include <stdexcept>
// os headers
#include <Windows.h>
#include <winsock2.h>
#include <mstcpip.h>
// wil headers
#include <wil/stl.h>
#include <wil/resource.h>
#include <wil/win32_helpers.h>
// project headers
#include "ctsConfig.h"
#include "ctsSocketState.h"
// need ctsSocket methods in implementation
#include "ctsSocket.h"
// for unique_socket
#include <wil/resource.h>
// affinity helpers
#include "../ctl/ctCpuAffinity.hpp"

namespace ctsTraffic
{
using namespace ctl;
using namespace std;

ctsSocketBroker::ctsSocketBroker()
{
    if (ctsConfig::g_configSettings->AcceptFunction)
    {
        // server 'accept' settings
        m_totalConnectionsRemaining = ctsConfig::g_configSettings->ServerExitLimit;
        m_pendingLimit = ctsConfig::g_configSettings->AcceptLimit;
    }
    else
    {
        // client 'connect' settings
        if (ctsConfig::g_configSettings->Iterations == MAXULONGLONG)
        {
            m_totalConnectionsRemaining = MAXULONGLONG;
        }
        else
        {
            m_totalConnectionsRemaining = ctsConfig::g_configSettings->Iterations * static_cast<ULONGLONG>(ctsConfig::g_configSettings->ConnectionLimit);
        }
        m_pendingLimit = ctsConfig::g_configSettings->ConnectionLimit;
    }

    // make sure pending_limit cannot be larger than total_connections_remaining
    if (m_pendingLimit > m_totalConnectionsRemaining)
    {
        m_pendingLimit = static_cast<uint32_t>(m_totalConnectionsRemaining);
    }

    // create our manual-reset notification event
    m_doneEvent.create(wil::EventOptions::ManualReset, nullptr);

    // If sharding is enabled, create shard objects now. We allocate shard
    // instances (one per configured ShardCount) but do not start their
    // workers until the user requests IO; this keeps startup deterministic
    // and avoids altering behavior when the feature is disabled.
    if (ctsConfig::g_configSettings->EnableRecvSharding)
    {
        uint32_t shardCount = ctsConfig::g_configSettings->ShardCount;
        if (shardCount == 0)
        {
            SYSTEM_INFO si{};
            GetSystemInfo(&si);
            shardCount = si.dwNumberOfProcessors;
        }

        m_shards.reserve(shardCount);

        // detect CPU affinity ioctl support and compute per-shard affinities
        const auto affinityInfo = ctl::QueryCpuAffinitySupport();

        // SIO_CPU_AFFINITY is mandatory when the user requests recv sharding.
        // If the kernel does not support the ioctl, log and fail loudly.
        if (!affinityInfo.SupportsCpuAffinityIoctl)
        {
            ctsConfig::PrintErrorInfoOverride(L"EnableRecvSharding requested but SIO_CPU_AFFINITY is not supported on this platform.\n");
            throw std::runtime_error("SIO_CPU_AFFINITY not supported");
        }
        std::optional<std::vector<ctl::GroupAffinity>> shardAffinities;
        if (affinityInfo.SupportsCpuAffinityIoctl)
        {
            // map config AffinityPolicy to ctl::CpuAffinityPolicy
            ctl::CpuAffinityPolicy policy = ctl::CpuAffinityPolicy::None;
            switch (ctsConfig::g_configSettings->ShardAffinityPolicy)
            {
            case ctsConfig::AffinityPolicy::PerCpu:
                policy = ctl::CpuAffinityPolicy::PerCpu;
                break;
            case ctsConfig::AffinityPolicy::PerGroup:
                policy = ctl::CpuAffinityPolicy::PerGroup;
                break;
            case ctsConfig::AffinityPolicy::RssAligned:
                policy = ctl::CpuAffinityPolicy::RssAligned;
                break;
            case ctsConfig::AffinityPolicy::Manual:
                policy = ctl::CpuAffinityPolicy::Manual;
                break;
            case ctsConfig::AffinityPolicy::None:
            default:
                policy = ctl::CpuAffinityPolicy::None;
                break;
            }

            auto computed = ctl::ComputeShardAffinities(shardCount, policy);
            if (computed && computed->size() == shardCount)
            {
                shardAffinities = std::move(*computed);
            }
            else
            {
                // couldn't compute affinities; fall back to no affinity
                shardAffinities.reset();
            }
        }
        // For each configured listen address, create shards that bind to that
        // address (or use INADDR_ANY if no listen addresses configured).
        auto add_shard_with_socket = [&](uint32_t shardId, const ctl::ctSockaddr& bindAddr) {
            m_shards.emplace_back(std::make_unique<ctl::ctRawIocpShard>(shardId));
            auto& shard = m_shards.back();

            // create and configure a socket for this shard
            wil::unique_socket s;
            try
            {
                s.reset(ctsConfig::CreateSocket(bindAddr.family(), SOCK_DGRAM, IPPROTO_UDP, ctsConfig::g_configSettings->SocketFlags));
            }
            catch (...) {
                // creation failed, leave shard uninitialized
                return;
            }

            // if we have a shard affinity mapping and SIO_CPU_AFFINITY is supported,
            // attempt to apply the kernel affinity ioctl prior to bind.
            if (shardAffinities && shardId < shardAffinities->size())
            {
                const auto& ga = (*shardAffinities)[shardId];
                // SIO_CPU_AFFINITY expects a structure describing group + mask
                struct {
                    WORD Group;
                    KAFFINITY Mask;
                } affinity;
                affinity.Group = ga.Group;
                affinity.Mask = ga.Mask;

#ifdef SIO_CPU_AFFINITY
                int rc = WSAIoctl(s.get(), SIO_CPU_AFFINITY, &affinity, sizeof(affinity), nullptr, 0, nullptr, nullptr, nullptr);
                if (rc == 0)
                {
                    PRINT_DEBUG_INFO(L"\t\tApplied SIO_CPU_AFFINITY to shard %u: %s\n", shardId, ctl::FormatGroupAffinity(ga).c_str());
                }
                else
                {
                    const auto gle = WSAGetLastError();
                    ctsConfig::PrintErrorIfFailed("WSAIoctl(SIO_CPU_AFFINITY)", gle);
                }
#else
                // SIO_CPU_AFFINITY not defined in this SDK; skip applying kernel affinity
#endif
            }

            // set pre-bind options consistent with other listening sockets
            const auto prebindErr = ctsConfig::SetPreBindOptions(s.get(), bindAddr);
            if (prebindErr != NO_ERROR)
            {
                // can't apply options; close socket and skip
                s.reset();
                return;
            }

            // bind the socket to the requested address
            if (bind(s.get(), bindAddr.sockaddr(), bindAddr.length()) == SOCKET_ERROR)
            {
                const auto gle = WSAGetLastError();
                ctsConfig::PrintErrorIfFailed("bind (shard)", gle);
                s.reset();
                return;
            }

            // attempt to initialize shard with this bound socket and start workers
            if (shard->Initialize(s.get(), ctsConfig::g_configSettings->OutstandingReceives))
            {
                shard->StartWorkers(ctsConfig::g_configSettings->ShardWorkerCount);
                // shard now owns the socket; release unique_socket ownership
                s.release();
            }
            else
            {
                // initialization failed; close the socket
                s.reset();
            }
        };

        // If there are explicit ListenAddresses, create shards for each address in round-robin
        if (!ctsConfig::g_configSettings->ListenAddresses.empty())
        {
            const auto& addrs = ctsConfig::g_configSettings->ListenAddresses;
            for (uint32_t i = 0; i < shardCount; ++i)
            {
                const auto& addr = addrs[i % addrs.size()];
                add_shard_with_socket(i, addr);
            }
        }
        else
        {
            // no explicit listen addresses: bind to INADDR_ANY / ephemeral port per family
            ctl::ctSockaddr any4; any4.reset(AF_INET, ctl::ctSockaddr::AddressType::Any);
            ctl::ctSockaddr any6; any6.reset(AF_INET6, ctl::ctSockaddr::AddressType::Any);
            for (uint32_t i = 0; i < shardCount; ++i)
            {
                // choose family based on config preference (prefer IPv4 by default)
                const auto& bindAddr = (ctsConfig::g_configSettings->Protocol == ctsConfig::ProtocolType::UDP && !ctsConfig::g_configSettings->TargetAddresses.empty() && ctsConfig::g_configSettings->TargetAddresses[0].family() == AF_INET6) ? any6 : any4;
                add_shard_with_socket(i, bindAddr);
            }
        }
    }
}

ctsSocketBroker::~ctsSocketBroker() noexcept
{
    // first signal the done event to stop work
    m_doneEvent.SetEvent();

    // next stop the TP if anything is running or queued
    m_tpFlatQueue.cancel();

    // now delete all children, guaranteeing they stop processing
    // - must do this explicitly before deleting the CS
    //   in case they were calling back while we called detach
    m_socketPool.clear();

    // shut down any shards we created
    for (auto& s : m_shards)
    {
        if (s)
        {
            s->Shutdown();
        }
    }
    m_shards.clear();
}

void ctsSocketBroker::Start()
{
    PRINT_DEBUG_INFO(
        L"\t\tStarting broker: total connections remaining (0x%llx), pending limit (0x%x)\n",
        m_totalConnectionsRemaining, m_pendingLimit);

    // must always guard access to the vector
    const auto lock = m_lock.lock();

    // only loop to pending_limit
    while (m_totalConnectionsRemaining > 0 && m_pendingSockets < m_pendingLimit)
    {
        // for outgoing connections, limit to ConnectionThrottleLimit
        // - to prevent killing the box with DPCs with too many concurrent connect attempts
        // checking first since TimerCallback might have already established connections
        if (!ctsConfig::g_configSettings->AcceptFunction &&
            m_pendingSockets >= ctsConfig::g_configSettings->ConnectionThrottleLimit)
        {
            break;
        }

        m_socketPool.push_back(make_shared<ctsSocketState>(shared_from_this()));
        (*m_socketPool.rbegin())->Start();
        ++m_pendingSockets;
        --m_totalConnectionsRemaining;
    }
}

// Broker-owned socket model implemented: shards are created and bound
// during broker construction. The adopt-on-demand path has been removed
// in favor of deterministic broker-owned sockets.

//
// SocketState is indicating the socket is now 'connected'
// - and will be pumping IO
// Update pending and active counts under guard
//
void ctsSocketBroker::InitiatingIo() noexcept
{
    const auto lock = m_lock.lock();

    FAIL_FAST_IF_MSG(
        m_pendingSockets == 0,
        "ctsSocketBroker::initiating_io - About to decrement pending_sockets, but pending_sockets == 0 (active_sockets == %u)",
        m_activeSockets);

    --m_pendingSockets;
    ++m_activeSockets;

    m_tpFlatQueue.submit([&] { RefreshSockets(); });
}

//
// SocketState is indicating the socket is now 'closed'
// Update pending or active counts (depending on prior state) under guard
//
void ctsSocketBroker::Closing(bool wasActive) noexcept
{
    const auto lock = m_lock.lock();

    if (wasActive)
    {
        FAIL_FAST_IF_MSG(
            m_activeSockets == 0,
            "ctsSocketBroker::closing - About to decrement active_sockets, but active_sockets == 0 (pending_sockets == %u)",
            m_pendingSockets);
        --m_activeSockets;
    }
    else
    {
        FAIL_FAST_IF_MSG(
            m_pendingSockets == 0,
            "ctsSocketBroker::closing - About to decrement pending_sockets, but pending_sockets == 0 (active_sockets == %u)",
            m_activeSockets);
        --m_pendingSockets;
    }

    m_tpFlatQueue.submit([&] { RefreshSockets(); });
}

bool ctsSocketBroker::Wait(DWORD milliseconds) const noexcept
{
    HANDLE arWait[2]{m_doneEvent.get(), ctsConfig::g_configSettings->CtrlCHandle};

    auto fReturn = false;
    switch (WaitForMultipleObjects(2, arWait, FALSE, milliseconds))
    {
        // we are done with our sockets, or user hit ctrl-c
        // - in either case we need to tell the caller to exit
        case WAIT_OBJECT_0:
        case WAIT_OBJECT_0 + 1:
            fReturn = true;
            break;

        case WAIT_TIMEOUT:
            fReturn = false;
            break;

        default:
            FAIL_FAST_MSG(
                "ctsSocketBroker - WaitForMultipleObjects(%p) failed [%lu]",
                arWait, GetLastError());
    }
    return fReturn;
}

//
// Timer callback to scavenge any closed sockets
// Then refresh sockets that should be created anew
//
void ctsSocketBroker::RefreshSockets() noexcept try
{
    // removedObjects will delete the closed objects outside the broker lock
    vector<shared_ptr<ctsSocketState>> removedObjects;

    auto exiting = false;
    try
    {
        const auto lock = m_lock.lock();

        exiting = 0 == m_totalConnectionsRemaining &&
                  0 == m_pendingSockets &&
                  0 == m_activeSockets;

        if (exiting)
        {
            removedObjects = std::move(m_socketPool);
        }
        else
        {
            // remove closed sockets from m_socketPool and move them to removedObjects
            for (auto it = m_socketPool.begin(); it != m_socketPool.end(); )
            {
                if ((*it)->GetCurrentState() == ctsSocketState::InternalState::Closed)
                {
                    removedObjects.emplace_back(std::move(*it));
                    it = m_socketPool.erase(it);
                }
                else
                {
                    ++it;
                }
            }

            if (!m_doneEvent.is_signaled())
            {
                // don't spin up more if the user asked to shut down
                // catch up to the expected # of pended connections
                while (m_pendingSockets < m_pendingLimit && m_totalConnectionsRemaining > 0)
                {
                    // not throttling the server accepting sockets based off total # of connections (pending + active)
                    // - only throttling total connections for outgoing connections
                    if (!ctsConfig::g_configSettings->AcceptFunction)
                    {
                        // ReSharper disable once CppRedundantParentheses
                        if ((m_pendingSockets + m_activeSockets) >= ctsConfig::g_configSettings->ConnectionLimit)
                        {
                            break;
                        }
                        // throttle pending connection attempts as specified
                        if (m_pendingSockets >= ctsConfig::g_configSettings->ConnectionThrottleLimit)
                        {
                            break;
                        }
                    }

                    m_socketPool.push_back(make_shared<ctsSocketState>(shared_from_this()));
                    (*m_socketPool.rbegin())->Start();
                    ++m_pendingSockets;
                    --m_totalConnectionsRemaining;
                }
            }
        }
    }
    catch (...)
    {
        ctsConfig::PrintThrownException();
    }

    removedObjects.clear();

    if (exiting)
    {
        SetEvent(m_doneEvent.get());
    }
}
catch (...)
{
    ctsConfig::PrintThrownException();
}
} // namespace
