/*

Copyright (c) Microsoft Corporation
All rights reserved.

Licensed under the Apache License, Version 2.0 (the ""License""); you may not use this file except in compliance with the License. You may obtain a copy of the License at http://www.apache.org/licenses/LICENSE-2.0

THIS CODE IS PROVIDED ON AN  *AS IS* BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, EITHER EXPRESS OR IMPLIED, INCLUDING WITHOUT LIMITATION ANY IMPLIED WARRANTIES OR CONDITIONS OF TITLE, FITNESS FOR A PARTICULAR PURPOSE, MERCHANTABLITY OR NON-INFRINGEMENT.

See the Apache Version 2.0 License for specific language governing permissions and limitations under the License.

*/

/**
 * @file ctCpuAffinity.hpp
 * @brief Helpers for detecting CPU affinity capabilities and mapping shards to CPU affinity.
 *
 * This header provides a small, testable interface used by the sharded receive
 * implementation to detect kernel capabilities (for example SIO_CPU_AFFINITY)
 * and to compute per-shard affinity mappings. Implementations may be
 * platform-optimized in a corresponding .cpp file.
 */

#pragma once

#include <cstdint>
#include <cwctype>
#include <vector>
#include <optional>
#include <string>

#include <winsock2.h>
#include <ws2def.h>
#include <mstcpip.h>

#include <wil/resource.h>

namespace ctl
{
	/**
	 * @internal
	 * @brief Internal helper utilities for CPU group/processor calculations.
	 *
	 * These helpers are implementation details and are not intended to be
	 * used directly by external components.
	 */
	namespace details {
		/**
		 * @brief Retrieve the active logical processor counts for each processor group.
		 *
		 * @return A vector where each element is the number of active logical
		 * processors in the corresponding processor group. The vector length is
		 * equal to the active processor group count.
		 */
		inline std::vector<uint32_t> GetProcessorCountsPerGroup() noexcept
		{
			std::vector<uint32_t> counts;
			const auto groupCount = GetActiveProcessorGroupCount();
			counts.reserve(groupCount);
			for (WORD g = 0; g < groupCount; ++g)
			{
				const auto proc_count = GetActiveProcessorCount(g);
				WI_ASSERT(proc_count > 0);
				counts.push_back(proc_count);
			}
			return counts;
		}

		/**
		 * @brief Convert a global logical CPU index to a processor group and local index.
		 *
		 * @param[in] globalIndex Global (0-based) logical processor index across all groups.
		 * @param[in] perGroupCounts Vector of per-group processor counts (as returned by GetProcessorCountsPerGroup).
		 * @param[out] outGroup Receives the processor group containing the requested logical processor.
		 * @param[out] outIndex Receives the local index within the returned processor group.
		 *
		 * The function will clamp to a valid group/index if the provided index is out of range.
		 */
		inline void GlobalCpuIndexToGroupAndIndex(uint32_t globalIndex, const std::vector<uint32_t>& perGroupCounts, _Out_ WORD* outGroup, _Out_ uint32_t* outIndex) noexcept
		{
			uint32_t acc = 0;
			for (WORD g = 0; g < perGroupCounts.size(); ++g)
			{
				const uint32_t c = perGroupCounts[g];
				if (globalIndex < acc + c)
				{
					*outGroup = g;
					*outIndex = globalIndex - acc;
					return;
				}
				acc += c;
			}
			// fallback to last group
			if (!perGroupCounts.empty())
			{
				*outGroup = static_cast<WORD>(perGroupCounts.size() - 1);
				*outIndex = perGroupCounts.back() ? (perGroupCounts.back() - 1) : 0;
			}
			else
			{
				*outGroup = 0;
				*outIndex = 0;
			}
		}
	}

	/**
	 * @brief Policies for computing shard -> CPU affinity mappings.
	 */
	enum class CpuAffinityPolicy : uint8_t
	{
		/** Assign each shard to a single logical CPU. */
		PerCpu,
		/** Assign each shard to an entire processor group (all CPUs in the group). */
		PerGroup,
		/** Align shards in a manner suitable for RSS (distribution across CPUs). */
		RssAligned,
		/** Manual mapping - requires external configuration. */
		Manual
	};

	/**
	 * @brief Runtime information about CPU affinity support on the host.
	 */
	struct CpuAffinityInfo
	{
		/** Total logical processors available across all groups. */
		uint32_t LogicalProcessorCount = 1;
		/** Number of processor groups reported by the OS. */
		uint32_t ProcessorGroupCount = 1;
		/** True if `WSAIoctl` with `SIO_CPU_AFFINITY` is supported. */
		bool SupportsCpuAffinityIoctl = false;
	};

	/**
	 * @brief Query runtime support and capabilities for CPU affinity.
	 *
	 * This function queries the OS for processor group information and probes
	 * whether `SIO_CPU_AFFINITY` is supported by creating a short-lived UDP socket
	 * and issuing a `WSAIoctl` probe. The function is non-throwing.
	 *
	 * @return A `CpuAffinityInfo` populated with detected capabilities.
	 */
	inline CpuAffinityInfo QueryCpuAffinitySupport() noexcept
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
		const wil::unique_socket s{ socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP) };
		if (s)
		{
			DWORD bytes = 0;
			// Per MSDN, SIO_CPU_AFFINITY expects a USHORT processor index (0-based)
			// as its input buffer. Probe support by passing a small USHORT buffer.
			USHORT probeProcessorIndex = 0;
#if !defined(SIO_CPU_AFFINITY)
#define SIO_CPU_AFFINITY                    _WSAIOW(IOC_VENDOR,21)
#endif
			const int rc = WSAIoctl(s.get(), SIO_CPU_AFFINITY, &probeProcessorIndex, sizeof(probeProcessorIndex), nullptr, 0, &bytes, nullptr, nullptr);
			info.SupportsCpuAffinityIoctl = (rc == 0);
		}
		else
		{
			info.SupportsCpuAffinityIoctl = false;
		}

		return info;
	}

	/**
	 * @brief A processor group and affinity mask pair used for setting thread/socket affinity.
	 */
	struct GroupAffinity
	{
		/** Processor group index. */
		WORD Group = 0;
		/** Affinity mask within the group (KAFFINITY). */
		KAFFINITY Mask = 0;
	};

	/**
	 * @brief Compute a per-shard CPU affinity mapping.
	 *
	 * Given a shard count and a `CpuAffinityPolicy`, compute a vector of
	 * `GroupAffinity` entries assigning each shard to a group/mask. If the
	 * requested mapping cannot be produced (for example `shardCount == 0` or
	 * the platform reports zero processors) the function returns `std::nullopt`.
	 *
	 * @param[in] shardCount Number of shards to map.
	 * @param[in] policy The affinity policy to apply when computing mappings.
	 * @return Optional vector with `shardCount` entries on success, or `std::nullopt` on failure.
	 */
	inline std::optional<std::vector<GroupAffinity>> ComputeShardAffinities(uint32_t shardCount, CpuAffinityPolicy policy)
	{
		if (shardCount == 0)
		{
			return std::nullopt;
		}

		const auto perGroup = details::GetProcessorCountsPerGroup();
		uint32_t totalProcessors = 0;
		for (const auto c : perGroup)
		{
			totalProcessors += c;
		}
		if (totalProcessors == 0)
		{
			return std::nullopt;
		}

		std::vector<GroupAffinity> result;
		result.resize(shardCount);

		switch (policy)
		{


		case CpuAffinityPolicy::Manual:
			// Manual policy requires external mapping; indicate failure by returning nullopt
			return std::nullopt;

		case CpuAffinityPolicy::PerCpu:
		case CpuAffinityPolicy::RssAligned:
		{
			// Distribute shards across individual logical processors round-robin
			for (uint32_t i = 0; i < shardCount; ++i)
			{
				const uint32_t cpuIndex = i % totalProcessors;
				GroupAffinity ga{};
				uint32_t localIndex = 0;
				details::GlobalCpuIndexToGroupAndIndex(cpuIndex, perGroup, &ga.Group, &localIndex);
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

	/**
	 * @brief Parse a human-readable policy name into a `CpuAffinityPolicy`.
	 *
	 * The comparison is case-insensitive and accepts a small set of aliases
	 * (for example `PER_CPU` and `PERCPU`). The function returns
	 * `std::nullopt` for empty or unrecognized names, and also for the
	 * canonical "NONE" value which indicates no policy.
	 *
	 * @param[in] name Input policy string (wide string).
	 * @return Optional `CpuAffinityPolicy` when recognized, otherwise `std::nullopt`.
	 */
	inline std::optional<CpuAffinityPolicy> ParsePolicyName(const std::wstring& name) noexcept
	{
		if (name.empty())
		{
			return std::nullopt;
		}

		std::wstring s = name;
		for (auto& c : s)
		{
			// towupper requires c to be in the ASCII range
			FAIL_FAST_IF(iswascii(c) == 0);
			c = std::towupper(c);
		}
		if (s == L"NONE") return std::nullopt;
		if (s == L"PERCPU" || s == L"PER_CPU") return CpuAffinityPolicy::PerCpu;
		if (s == L"PERGROUP" || s == L"PER_GROUP") return CpuAffinityPolicy::PerGroup;
		if (s == L"RSSALIGNED" || s == L"RSS_ALIGNED") return CpuAffinityPolicy::RssAligned;
		if (s == L"MANUAL") return CpuAffinityPolicy::Manual;

		return std::nullopt;
	}

	/**
	 * @brief Format a `GroupAffinity` as a human-readable string for logging.
	 *
	 * The returned string contains the processor group index and the
	 * affinity mask formatted in hexadecimal.
	 *
	 * @param[in] g The `GroupAffinity` to format.
	 * @return A wide string describing the group and mask.
	 */
	inline std::wstring FormatGroupAffinity(const GroupAffinity& g)
	{
		wchar_t buf[128]{};
		FAIL_FAST_IF(-1 == swprintf_s(buf, _countof(buf), L"Group=%u Mask=0x%llx", g.Group, static_cast<unsigned long long>(g.Mask)));
		return buf;
	}
}
