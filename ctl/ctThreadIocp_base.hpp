/**
 * @file ctThreadIocp_base.hpp
 * @brief Minimal abstract interface used by ctThreadIocp-like objects.
 *
 * This header declares `ctl::ctThreadIocp_base`, an abstract base class that
 * provides the minimal contract required by callers that allocate and manage
 * OVERLAPPED structures for asynchronous I/O backed by an IO completion port.
 *
 * Existing implementation details and the original explanatory comments are
 * preserved below; the method parameter annotations mark ownership and intent
 * using [in]/[out]/[in,out] where appropriate.
 *
 * @copyright Copyright (c) Microsoft Corporation
 * All rights reserved.
 *
 * Licensed under the Apache License, Version 2.0 (the "License"); you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at http://www.apache.org/licenses/LICENSE-2.0
 *
 * THIS CODE IS PROVIDED ON AN *AS IS* BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, EITHER EXPRESS OR IMPLIED, INCLUDING WITHOUT LIMITATION ANY IMPLIED WARRANTIES OR CONDITIONS OF TITLE, FITNESS FOR A PARTICULAR PURPOSE, MERCHANTABILITY OR NON-INFRINGEMENT.
 *
 * See the Apache Version 2.0 License for specific language governing permissions and limitations under the License.
 */

#pragma once

#include <functional>
#include <Windows.h>

namespace ctl
{
/**
 * @brief Abstract base exposing the minimal contract used by callers.
 *
 * Implementations provide ownership and lifecycle management for
 * `OVERLAPPED*` instances used with Win32 asynchronous APIs. Callers obtain
 * request objects via `new_request` and must notify the implementation via
 * `cancel_request` when the request will not be used due to synchronous
 * failure (error != ERROR_IO_PENDING).
 */
class ctThreadIocp_base
{
public:
    /**
     * @brief Virtual destructor.
     */
    virtual ~ctThreadIocp_base() noexcept = default;

    /**
     * @brief Allocate an `OVERLAPPED*` for use with Win32 async APIs.
     *
     * The returned pointer is owned by the implementation and must not be
     * deleted by the caller. The caller provides a callback that the
     * implementation will invoke when the operation completes.
     *
     * @param _callback [in] A function called by the implementation when the
     *        overlapped operation associated with the returned `OVERLAPPED*`
     *        completes. The callback receives the `OVERLAPPED*` that was
     *        originally returned from this method.
     * @return OVERLAPPED* [out] Pointer to an `OVERLAPPED` owned by the
     *         implementation. The caller must not free this pointer.
     *
     * @note If the Win32 API call that consumes the returned `OVERLAPPED*`
     *       fails synchronously with an error other than `ERROR_IO_PENDING`,
     *       the caller MUST call `cancel_request` to allow the
     *       implementation to reclaim any resources associated with the
     *       overlapped object.
     */
    virtual OVERLAPPED* new_request(std::function<void(OVERLAPPED*)> _callback) const = 0;

    /**
     * @brief Notify the implementation that an allocated request will not be used.
     *
     * This function should be called when the Win32 API that was given the
     * `OVERLAPPED*` returned by `new_request` fails synchronously (i.e., the
     * error is not `ERROR_IO_PENDING`). It does not cancel in-flight I/O;
     * instead it allows the implementation to free or recycle resources
     * associated with the overlapped request.
     *
     * @param pOverlapped [in] The `OVERLAPPED*` previously returned from
     *        `new_request` that will not be used.
     */
    virtual void cancel_request(const OVERLAPPED* pOverlapped) const noexcept = 0;
};

} // namespace ctl
