/*

Copyright (c) Microsoft Corporation
All rights reserved.

Licensed under the Apache License, Version 2.0 (the ""License""); you may not use this file except in compliance with the License. You may obtain a copy of the License at http://www.apache.org/licenses/LICENSE-2.0

THIS CODE IS PROVIDED ON AN  *AS IS* BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, EITHER EXPRESS OR IMPLIED, INCLUDING WITHOUT LIMITATION ANY IMPLIED WARRANTIES OR CONDITIONS OF TITLE, FITNESS FOR A PARTICULAR PURPOSE, MERCHANTABLITY OR NON-INFRINGEMENT.

See the Apache Version 2.0 License for specific language governing permissions and limitations under the License.

*/

#pragma once

/**
 * @file ctsIOPatternRateLimitPolicy.hpp
 * @brief Rate-limiting policies for IO patterns (throttle or don't throttle).
 */

// ctl headers
#include <ctTimer.hpp>
// project headers
#include "ctsConfig.h"
#include "ctsIOTask.hpp"

namespace ctsTraffic
{
/** Tag type: throttle sends to meet bytes/sec target */
using ctsIOPatternRateLimitThrottle = struct ctsIOPatternRateLimitThrottle_t;

/** Tag type: do not throttle sends */
using ctsIOPatternRateLimitDontThrottle = struct ctsIOPatternRateLimitDontThrottle_t;

template <typename Protocol>
struct ctsIOPatternRateLimitPolicy
{
    /**
     * @brief Update the task's `m_timeOffsetMilliseconds` based on rate limiting.
     *
     * Specializations provide either a no-op (don't throttle) or the throttle
     * behavior.
     *
     * @param task [in,out] The task whose time offset may be updated.
     * @param bufferSize [in] The number of bytes associated with this task.
     */
    void update_time_offset(ctsTask&, const int64_t& bufferSize) noexcept = delete;
};


///
/// ctsIOPatternRateLimitDontThrottle
///
template <>
struct ctsIOPatternRateLimitPolicy<ctsIOPatternRateLimitDontThrottle>
{
    // ReSharper disable once CppMemberFunctionMayBeStatic
    /**
     * @brief No-op rate limiter; leaves `task.m_timeOffsetMilliseconds` untouched.
     *
     * @param task [in,out] The task to consider (ignored).
     * @param bufferSize [in] The buffer size (ignored).
     */
    void update_time_offset(ctsTask&, const int64_t&) const noexcept
    {
        // no-op
    }
};

///
/// ctsIOPatternRateLimitThrottle
///
template <>
struct ctsIOPatternRateLimitPolicy<ctsIOPatternRateLimitThrottle>
{
private:
    const uint64_t m_bytesSendingPerQuantum;
    const int64_t m_quantumPeriodMs{ctsConfig::g_configSettings->TcpBytesPerSecondPeriod};
    uint64_t m_bytesSentThisQuantum{0};
    int64_t m_quantumStartTimeMs{ctl::ctTimer::snap_qpc_as_msec()};

public:
    ctsIOPatternRateLimitPolicy() noexcept :
        m_bytesSendingPerQuantum(ctsConfig::GetTcpBytesPerSecond() * ctsConfig::g_configSettings->TcpBytesPerSecondPeriod / 1000LL)
    {
#ifdef CTSTRAFFIC_UNIT_TESTS
        PRINT_DEBUG_INFO(
            L"\t\tctsIOPatternRateLimitPolicy: BytesSendingPerQuantum - %llu, QuantumPeriodMs - %lld\n",
            m_bytesSendingPerQuantum, m_quantumPeriodMs);
#endif
    }

    /**
     * @brief Calculate and set `task.m_timeOffsetMilliseconds` to respect the
     *        configured TCP bytes-per-second throttling.
     *
     * @param task [in,out] Task to update; only `Send` tasks are adjusted.
     * @param bufferSize [in] Number of bytes this task would send.
     */
    void update_time_offset(ctsTask& task, uint64_t bufferSize) noexcept
    {
        if (task.m_ioAction != ctsTaskAction::Send)
        {
            return;
        }

        task.m_timeOffsetMilliseconds = 0LL;
        const auto currentTimeMs(ctl::ctTimer::snap_qpc_as_msec());

        if (m_bytesSentThisQuantum < m_bytesSendingPerQuantum)
        {
            if (currentTimeMs < m_quantumStartTimeMs + m_quantumPeriodMs)
            {
                if (currentTimeMs > m_quantumStartTimeMs)
                {
                    // time is in the current quantum
                    m_bytesSentThisQuantum += bufferSize;
                }
                else
                {
                    // time is still in a prior quantum
                    task.m_timeOffsetMilliseconds = this->NewQuantumStartTime() - currentTimeMs;
                    m_bytesSentThisQuantum += bufferSize;
                }
            }
            else
            {
                // time is already in a new quantum - start over
                m_bytesSentThisQuantum = bufferSize;
                m_quantumStartTimeMs += (currentTimeMs - m_quantumStartTimeMs);
            }
        }
        else
        {
            // have already fulfilled the prior quantum
            const auto new_quantum_start_time_ms = this->NewQuantumStartTime();

            if (currentTimeMs < new_quantum_start_time_ms)
            {
                task.m_timeOffsetMilliseconds = new_quantum_start_time_ms - currentTimeMs;
                m_bytesSentThisQuantum = bufferSize;
                m_quantumStartTimeMs = new_quantum_start_time_ms;
            }
            else
            {
                m_bytesSentThisQuantum = bufferSize;
                m_quantumStartTimeMs += (currentTimeMs - m_quantumStartTimeMs);
            }
        }
#ifdef CTSTRAFFIC_UNIT_TESTS
        PRINT_DEBUG_INFO(
            L"\t\tctsIOPatternRateLimitPolicy\n"
            L"\tcurrent_time_ms: %lld\n"
            L"\tquantum_start_time_ms: %lld\n"
            L"\tbytes_sent_this_quantum: %llu\n",
            currentTimeMs,
            m_quantumStartTimeMs,
            m_bytesSentThisQuantum);
#endif
    }

private:
    [[nodiscard]] int64_t NewQuantumStartTime() const
    {
        return m_quantumStartTimeMs + static_cast<int64_t>(m_bytesSentThisQuantum / m_bytesSendingPerQuantum * m_quantumPeriodMs);
    }
};
}
