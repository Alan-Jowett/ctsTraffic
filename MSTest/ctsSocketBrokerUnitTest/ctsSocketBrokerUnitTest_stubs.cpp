// Minimal stubs to satisfy linker for unit testing ctsSocketBroker behavior.
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <optional>
#include <vector>
#include "..\..\ctl\ctSockaddr.hpp"
#include "..\..\ctl\ctRawIocpShard.hpp"
#include "..\..\ctl\ctCpuAffinity.hpp"

// Test hook: record when an affinity ioctl would have been invoked on a socket
#include "ctsSocketBrokerUnitTest_hooks.h"

namespace ctsTestHook {
    std::vector<AffinityRecord> AppliedAffinityRecords;
}

namespace ctsTraffic::ctsConfig {
    void PrintErrorIfFailed(_In_ PCSTR, uint32_t) noexcept {}
    void PrintErrorInfoOverride(_In_ PCWSTR) noexcept {}
    int SetPreBindOptions(unsigned __int64, const ctl::ctSockaddr&) { return NO_ERROR; }
    SOCKET CreateSocket(int af, int type, int protocol, unsigned long) { return ::WSASocketW(af, type, protocol, nullptr, 0, WSA_FLAG_OVERLAPPED); }
}

namespace ctl {
    // minimal ctRawIocpShard implementation used by tests
    ctRawIocpShard::ctRawIocpShard(uint32_t id) noexcept : m_id(id) {}
    ctRawIocpShard::~ctRawIocpShard() noexcept {}
    bool ctRawIocpShard::Initialize(SOCKET listenSocketHint, uint32_t outstandingReceives) noexcept { m_socket = listenSocketHint; (void)outstandingReceives; return true; }
    bool ctRawIocpShard::StartWorkers(uint32_t workerCount) noexcept { (void)workerCount; return true; }
    void ctRawIocpShard::Shutdown() noexcept { if (m_socket != INVALID_SOCKET) { closesocket(m_socket); m_socket = INVALID_SOCKET; } }

    CpuAffinityInfo QueryCpuAffinitySupport() noexcept
    {
        CpuAffinityInfo info{};
        info.ProcessorGroupCount = 1;
        info.LogicalProcessorCount = 1;
        info.SupportsCpuAffinityIoctl = true; // pretend it's supported for unit tests
        info.SupportsGQCSEx = true;
        return info;
    }

    std::optional<std::vector<GroupAffinity>> ComputeShardAffinities(uint32_t shardCount, CpuAffinityPolicy) noexcept
    {
        if (shardCount == 0) return std::nullopt;
        std::vector<GroupAffinity> result(shardCount);
        for (uint32_t i = 0; i < shardCount; ++i)
        {
            result[i].Group = 0;
            result[i].Mask = 1ULL;
        }
        return result;
    }
}

// Provide a test shim that production code calls as WSAIoctl (remapped by forced include)
extern "C" int __stdcall Test_WSAIoctl(SOCKET s, DWORD dwIoControlCode, LPVOID lpvInBuffer, DWORD cbInBuffer,
    LPVOID lpvOutBuffer, DWORD cbOutBuffer, LPDWORD lpcbBytesReturned, LPWSAOVERLAPPED lpOverlapped, LPWSAOVERLAPPED_COMPLETION_ROUTINE lpCompletionRoutine)
{
    UNREFERENCED_PARAMETER(s);
    UNREFERENCED_PARAMETER(dwIoControlCode);
    UNREFERENCED_PARAMETER(lpvOutBuffer);
    UNREFERENCED_PARAMETER(cbOutBuffer);
    UNREFERENCED_PARAMETER(lpcbBytesReturned);
    UNREFERENCED_PARAMETER(lpOverlapped);
    UNREFERENCED_PARAMETER(lpCompletionRoutine);

    // If the input buffer matches the expected affinity layout, record the parameters
    if (lpvInBuffer != nullptr && cbInBuffer >= sizeof(WORD) + sizeof(KAFFINITY))
    {
        // input buffer layout: WORD Group; KAFFINITY Mask;
        const auto pGroup = static_cast<WORD*>(lpvInBuffer);
        const auto pMask = reinterpret_cast<KAFFINITY*>(reinterpret_cast<BYTE*>(lpvInBuffer) + sizeof(WORD));
        ctsTestHook::AffinityRecord rec{};
        rec.ShardId = 0; // broker doesn't supply shard id here; test asserts count and values
        rec.Group = *pGroup;
        rec.Mask = *pMask;
        ctsTestHook::AppliedAffinityRecords.push_back(rec);
        return 0; // pretend success
    }

    // fallback: indicate not implemented for other ioctls
    WSASetLastError(WSAEOPNOTSUPP);
    return SOCKET_ERROR;
}
