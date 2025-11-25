/*

Copyright (c) Microsoft Corporation
All rights reserved.

Licensed under the Apache License, Version 2.0 (the ""License""); you may not use this file except in compliance with the License. You may obtain a copy of the License at http://www.apache.org/licenses/LICENSE-2.0

THIS CODE IS PROVIDED ON AN  *AS IS* BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, EITHER EXPRESS OR IMPLIED, INCLUDING WITHOUT LIMITATION ANY IMPLIED WARRANTIES OR CONDITIONS OF TITLE, FITNESS FOR A PARTICULAR PURPOSE, MERCHANTABLITY OR NON-INFRINGEMENT.

See the Apache Version 2.0 License for specific language governing permissions and limitations under the License.

*/

/* ctCpuAffinity - helper to detect CPU affinity facilities and map shard -> affinity
    This header provides a small, testable interface for the sharded receive
    implementation to detect kernel capabilities (SIO_CPU_AFFINITY) and compute
    per-shard affinity mappings. Implementations can be platform-optimized in
    a corresponding .cpp file.
*/

#pragma once

#include <Windows.h>
#include <cstdint>
#include <vector>
#include <optional>
#include <string>

namespace ctl
{
    enum class CpuAffinityPolicy : uint8_t
    {
        PerCpu,
        PerGroup,
        RssAligned,
        Manual
    };

    struct CpuAffinityInfo
    {
        bool SupportsCpuAffinityIoctl = false; // WSAIoctl(SIO_CPU_AFFINITY) support
        bool SupportsGQCSEx = false;          // GetQueuedCompletionStatusEx available
        uint32_t LogicalProcessorCount = 1;    // total logical processors
        uint32_t ProcessorGroupCount = 1;     // number of processor groups
    };

    // Query runtime support for CPU affinity features. Non-throwing.
    CpuAffinityInfo QueryCpuAffinitySupport() noexcept;

    // Represents a group + affinity mask for thread/socket affinity operations.
    struct GroupAffinity
    {
        WORD Group = 0;
        KAFFINITY Mask = 0;
    };

    // Compute per-shard affinity mapping given a shard count and policy.
    // Returns an optional vector of length == shardCount when mapping is possible.
    std::optional<std::vector<GroupAffinity>> ComputeShardAffinities(uint32_t shardCount, CpuAffinityPolicy policy) noexcept;

    // Parse a policy name (case-insensitive) to CpuAffinityPolicy.
    std::optional<CpuAffinityPolicy> ParsePolicyName(const std::wstring& name) noexcept;

    // Human-readable formatting helper for logging.
    std::wstring FormatGroupAffinity(const GroupAffinity& g) noexcept;
}
