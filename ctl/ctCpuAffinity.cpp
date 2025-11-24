/*
 * ctCpuAffinity.cpp
 * Implementation of CPU affinity capability detection and shard -> affinity mapping.
 */

#include "ctCpuAffinity.hpp"

#include <winsock2.h>
#include <mstcpip.h>
#include <windows.h>
#include <vector>
#include <algorithm>

namespace ctl
{

    CpuAffinityInfo QueryCpuAffinitySupport() noexcept
    {
        CpuAffinityInfo info{};

        // Processor group and counts
        info.ProcessorGroupCount = static_cast<uint32_t>(GetActiveProcessorGroupCount());
        // total logical processors across groups
        uint64_t total = 0;
        for (WORD g = 0; g < info.ProcessorGroupCount; ++g)
        {
            total += GetActiveProcessorCount(g);
        }
        info.LogicalProcessorCount = static_cast<uint32_t>(total);

        // Probe SIO_CPU_AFFINITY support using a temporary UDP socket, if available
        // WSAStartup is expected to have been called by the application earlier.
#ifdef SIO_CPU_AFFINITY
        SOCKET s = WSASocketW(AF_INET, SOCK_DGRAM, IPPROTO_UDP, nullptr, 0, WSA_FLAG_OVERLAPPED);
        if (s != INVALID_SOCKET)
        {
            DWORD bytes = 0;
            // Calling WSAIoctl with SIO_CPU_AFFINITY; if it returns 0, consider supported.
            const int rc = WSAIoctl(s, SIO_CPU_AFFINITY, nullptr, 0, nullptr, 0, &bytes, nullptr, nullptr);
            info.SupportsCpuAffinityIoctl = (rc == 0);
            closesocket(s);
        }
        else
        {
            info.SupportsCpuAffinityIoctl = false;
        }
#else
        // SDK doesn't define SIO_CPU_AFFINITY; treat as not supported.
        info.SupportsCpuAffinityIoctl = false;
#endif

        // Feature-detect GetQueuedCompletionStatusEx presence conservatively
        // Assume present on supported Windows versions; leave false only if very old.
        info.SupportsGQCSEx = true;

        return info;
    }

    namespace {
        // Helper: get per-group processor counts
        static std::vector<uint32_t> GetProcessorCountsPerGroup() noexcept
        {
            std::vector<uint32_t> counts;
            const WORD groupCount = static_cast<WORD>(GetActiveProcessorGroupCount());
            counts.reserve(groupCount);
            for (WORD g = 0; g < groupCount; ++g)
            {
                counts.push_back(static_cast<uint32_t>(GetActiveProcessorCount(g)));
            }
            return counts;
        }

        // Convert a global cpu index to group and local index
        static void GlobalCpuIndexToGroupAndIndex(uint32_t globalIndex, const std::vector<uint32_t>& perGroupCounts, WORD& outGroup, uint32_t& outIndex) noexcept
        {
            uint32_t acc = 0;
            for (WORD g = 0; g < perGroupCounts.size(); ++g)
            {
                const uint32_t c = perGroupCounts[g];
                if (globalIndex < acc + c)
                {
                    outGroup = g;
                    outIndex = globalIndex - acc;
                    return;
                }
                acc += c;
            }
            // fallback to last group
            if (!perGroupCounts.empty())
            {
                outGroup = static_cast<WORD>(perGroupCounts.size() - 1);
                outIndex = perGroupCounts.back() ? (perGroupCounts.back() - 1) : 0;
            }
            else
            {
                outGroup = 0;
                outIndex = 0;
            }
        }
    }

    std::optional<std::vector<GroupAffinity>> ComputeShardAffinities(uint32_t shardCount, CpuAffinityPolicy policy) noexcept
    {
        if (shardCount == 0)
        {
            return std::nullopt;
        }

        const auto perGroup = GetProcessorCountsPerGroup();
        uint32_t totalProcessors = 0;
        for (auto c : perGroup) totalProcessors += c;
        if (totalProcessors == 0)
        {
            return std::nullopt;
        }

        std::vector<GroupAffinity> result;
        result.resize(shardCount);

        switch (policy)
        {
        case CpuAffinityPolicy::None:
            // No affinity: return masks of zero (meaning no binding)
            for (uint32_t i = 0; i < shardCount; ++i)
            {
                result[i].Group = 0;
                result[i].Mask = 0;
            }
            return result;

        case CpuAffinityPolicy::Manual:
            // Manual policy requires external mapping; indicate failure by returning nullopt
            return std::nullopt;

        case CpuAffinityPolicy::PerCpu:
        case CpuAffinityPolicy::RssAligned:
        {
            // Distribute shards across individual logical processors round-robin
            for (uint32_t i = 0; i < shardCount; ++i)
            {
                uint32_t cpuIndex = i % totalProcessors;
                WORD group = 0;
                uint32_t localIndex = 0;
                GlobalCpuIndexToGroupAndIndex(cpuIndex, perGroup, group, localIndex);
                GroupAffinity ga{};
                ga.Group = group;
                // Limit mask to 64 bits (KAFFINITY is 64-bit on x64)
                if (localIndex < (sizeof(KAFFINITY) * 8))
                {
                    ga.Mask = static_cast<KAFFINITY>(1ULL << localIndex);
                }
                else
                {
                    // fallback to first bit if index too large
                    ga.Mask = static_cast<KAFFINITY>(1ULL);
                }
                result[i] = ga;
            }
            return result;
        }

        case CpuAffinityPolicy::PerGroup:
        default:
        {
            // Assign shards to groups (full group mask) round-robin across available groups
            const size_t groupCount = perGroup.size();
            std::vector<KAFFINITY> groupMasks(groupCount, 0);
            for (size_t g = 0; g < groupCount; ++g)
            {
                KAFFINITY mask = 0;
                const uint32_t count = perGroup[g];
                for (uint32_t bi = 0; bi < count && bi < (sizeof(KAFFINITY) * 8); ++bi)
                {
                    mask |= (static_cast<KAFFINITY>(1ULL) << bi);
                }
                groupMasks[g] = mask;
            }

            for (uint32_t i = 0; i < shardCount; ++i)
            {
                const size_t g = i % groupCount;
                result[i].Group = static_cast<WORD>(g);
                result[i].Mask = groupMasks[g];
            }
            return result;
        }
        }
    }

    std::optional<CpuAffinityPolicy> ParsePolicyName(const std::wstring& name) noexcept
    {
        if (name.empty()) return CpuAffinityPolicy::None;
        std::wstring s = name;
        for (auto& c : s) c = towupper(c);
        if (s == L"NONE") return CpuAffinityPolicy::None;
        if (s == L"PERCPU" || s == L"PER_CPU") return CpuAffinityPolicy::PerCpu;
        if (s == L"PERGROUP" || s == L"PER_GROUP") return CpuAffinityPolicy::PerGroup;
        if (s == L"RSSALIGNED" || s == L"RSS_ALIGNED") return CpuAffinityPolicy::RssAligned;
        if (s == L"MANUAL") return CpuAffinityPolicy::Manual;
        return std::nullopt;
    }

    std::wstring FormatGroupAffinity(const GroupAffinity& g) noexcept
    {
        wchar_t buf[128];
        _snwprintf_s(buf, _countof(buf), L"Group=%u Mask=0x%llx", g.Group, static_cast<unsigned long long>(g.Mask));
        return std::wstring(buf);
    }

} // namespace ctl
