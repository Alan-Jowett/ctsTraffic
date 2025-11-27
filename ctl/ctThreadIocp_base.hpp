/*

Copyright (c) Microsoft Corporation
All rights reserved.

Licensed under the Apache License, Version 2.0 (the "License"); you may not use this file except in compliance with the License. You may obtain a copy of the License at http://www.apache.org/licenses/LICENSE-2.0

THIS CODE IS PROVIDED ON AN  *AS IS* BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, EITHER EXPRESS OR IMPLIED, INCLUDING WITHOUT LIMITATION ANY IMPLIED WARRANTIES OR CONDITIONS OF TITLE, FITNESS FOR A PARTICULAR PURPOSE, MERCHANTABLITY OR NON-INFRINGEMENT.

See the Apache Version 2.0 License for specific language governing permissions and limitations under the License.

  ctThreadIocp_base.hpp

  Abstract interface describing the contract between ctThreadIocp-like
  objects and their callers.

*/
#pragma once

#include <functional>
#include <Windows.h>

namespace ctl
{
// Abstract base exposing the minimal contract used by callers.
class ctThreadIocp_base
{
public:
    virtual ~ctThreadIocp_base() noexcept = default;

    // Allocate an OVERLAPPED* that the caller will pass to Win32 APIs.
    // The returned OVERLAPPED* is owned by the implementation and must not
    // be deleted by the caller. The caller must call `cancel_request`
    // if the API call fails synchronously with an error other than
    // ERROR_IO_PENDING.
    virtual OVERLAPPED* new_request(std::function<void(OVERLAPPED*)> _callback) const = 0;

    // Notify the implementation that the OVERLAPPED* returned from
    // `new_request` will not be used because the Win32 API failed
    // synchronously (error != ERROR_IO_PENDING). This does not cancel
    // any in-flight IO; it simply lets the implementation free any
    // resources associated with the OVERLAPPED*.
    virtual void cancel_request(const OVERLAPPED* pOverlapped) const noexcept = 0;
};

} // namespace ctl
