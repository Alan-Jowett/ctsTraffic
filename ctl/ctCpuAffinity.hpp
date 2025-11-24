/* ctCpuAffinity - helper to detect CPU affinity facilities and map shard -> affinity
   This is a minimal skeleton with runtime-detection stubs used by sharding implementation.
*/
#pragma once

#include <Windows.h>
#include <cstdint>

namespace ctl
{
    struct CpuAffinityInfo
    {
        bool SupportsCpuAffinityIoctl = false;
        uint32_t ProcessorCount = 0;
    };

    // Query runtime support for SIO_CPU_AFFINITY and other features
    CpuAffinityInfo QueryCpuAffinitySupport() noexcept;

    // Map a shard id to a processor mask (stub, simple round-robin)
    GROUP_AFFINITY MapShardToGroupAffinity(uint32_t shardId, uint32_t shardCount) noexcept;
}
