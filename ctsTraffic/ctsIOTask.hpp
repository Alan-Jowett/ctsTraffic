/*

Copyright (c) Microsoft Corporation
All rights reserved.

Licensed under the Apache License, Version 2.0 (the ""License""); you may not use this file except in compliance with the License. You may obtain a copy of the License at http://www.apache.org/licenses/LICENSE-2.0

THIS CODE IS PROVIDED ON AN  *AS IS* BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, EITHER EXPRESS OR IMPLIED, INCLUDING WITHOUT LIMITATION ANY IMPLIED WARRANTIES OR CONDITIONS OF TITLE, FITNESS FOR A PARTICULAR PURPOSE, MERCHANTABLITY OR NON-INFRINGEMENT.

See the Apache Version 2.0 License for specific language governing permissions and limitations under the License.

*/

#pragma once

/**
 * @file ctsIOTask.hpp
 * @brief Definition of the `ctsTask` structure used to describe IO work.
 *
 * `ctsTask` describes the next IO operation the pattern should perform and
 * contains buffer information, offsets, and bookkeeping fields used by the
 * verification and rate-limit subsystems.
 */

// os headers
// ReSharper disable once CppUnusedIncludeDirective
#include <WinSock2.h>
#include <MSWSock.h>
#include <Windows.h>

// ** NOTE ** should not include any local project cts headers - to avoid circular references

namespace ctsTraffic
{
// The ctsIOTask struct instructs the caller on what action to perform
// - and provides it the buffer it should use to send/recv data
/**
 * @brief Action requested by a `ctsTask`.
 */
enum class ctsTaskAction : std::uint8_t
{
    None,
    Send,
    Recv,
    GracefulShutdown,
    HardShutdown,
    Abort,
    FatalAbort
};

struct ctsTask
{
    /**
     * @brief Millisecond offset to postpone this task.
     *
     * [in,out] Consumers may set `m_timeOffsetMilliseconds` to delay posting
     * the IO (used by rate-limiters). Default is 0.
     */
    int64_t m_timeOffsetMilliseconds = 0LL;

    /**
     * @brief Registered I/O buffer identifier when using RIO.
     */
    RIO_BUFFERID m_rioBufferid = RIO_INVALID_BUFFERID;

    /**
     * @brief Pointer to the buffer to use for the IO request.
     *
     * [in,out] For receive operations this buffer will be written into. For
     * send operations it will be read from. The buffer is sized by
     * `m_bufferLength`.
     */
    _Field_size_full_(m_bufferLength) char* m_buffer = nullptr;

    /**
     * @brief The length in bytes of `m_buffer`.
     */
    uint32_t m_bufferLength = 0UL;

    /**
     * @brief Offset into `m_buffer` where the IO should start.
     */
    uint32_t m_bufferOffset = 0UL;

    /**
     * @brief Expected pattern offset used for verification.
     */
    uint32_t m_expectedPatternOffset = 0UL;

    /**
     * @brief The I/O action requested for this task.
     */
    ctsTaskAction m_ioAction = ctsTaskAction::None;

    // (internal) flag identifying the type of buffer
    enum class BufferType : std::uint8_t
    {
        Null,
        TcpConnectionId,
        UdpConnectionId,
        CompletionMessage,
        Static,
        Dynamic
    } m_bufferType = BufferType::Null;

    /**
     * @brief Whether the task should be tracked/verified by the pattern.
     */
    bool m_trackIo = false;

    /**
     * @brief Convert an action enum to a human readable string.
     *
     * @param action [in] The action to stringify.
     * @return Wide string name of the action.
     */
    static PCWSTR PrintTaskAction(const ctsTaskAction& action) noexcept
    {
        switch (action)
        {
            case ctsTaskAction::None:
                return L"None";
            case ctsTaskAction::Send:
                return L"Send";
            case ctsTaskAction::Recv:
                return L"Recv";
            case ctsTaskAction::GracefulShutdown:
                return L"GracefulShutdown";
            case ctsTaskAction::HardShutdown:
                return L"HardShutdown";
            case ctsTaskAction::Abort:
                return L"Abort";
            case ctsTaskAction::FatalAbort:
                return L"FatalAbort";
        }

        return L"Unknown IOAction";
    }
};
} // namespace
