/*

Copyright (c) Microsoft Corporation
All rights reserved.

Licensed under the Apache License, Version 2.0 (the "License"); you may not use this file except in compliance with the License. You may obtain a copy of the License at http://www.apache.org/licenses/LICENSE-2.0

THIS CODE IS PROVIDED ON AN  *AS IS* BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, EITHER EXPRESS OR IMPLIED, INCLUDING WITHOUT LIMITATION ANY IMPLIED WARRANTIES OR CONDITIONS OF TITLE, FITNESS FOR A PARTICULAR PURPOSE, MERCHANTABLITY OR NON-INFRINGEMENT.

See the Apache Version 2.0 License for specific language governing permissions and limitations under the License.

*/

// ReSharper disable CppInconsistentNaming
#pragma once

// cpp headers
#include <utility>
#include <functional>
// os headers
#include <excpt.h>
#include <Windows.h>
#include <WinSock2.h>
// wil headers
#include <wil/resource.h>


#include "ctThreadIocp_base.hpp"

/**
 * @file
 * @brief Thread-pool based OVERLAPPED I/O helper using the Windows Threadpool APIs.
 *
 * This header defines `ctThreadIocp`, a helper that creates a thread-pool IO
 * object and provides a safe, callback-based pattern for using `OVERLAPPED*`
/*
Copyright (c) Microsoft Corporation
All rights reserved.

Licensed under the Apache License, Version 2.0 (the "License"); you may not use this file except in compliance with the License. You may obtain a copy of the License at http://www.apache.org/licenses/LICENSE-2.0

THIS CODE IS PROVIDED ON AN  *AS IS* BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, EITHER EXPRESS OR IMPLIED, INCLUDING WITHOUT LIMITATION ANY IMPLIED WARRANTIES OR CONDITIONS OF TITLE, FITNESS FOR A PARTICULAR PURPOSE, MERCHANTABLITY OR NON-INFRINGEMENT.

See the Apache Version 2.0 License for specific language governing permissions and limitations under the License.
*/

// ReSharper disable CppInconsistentNaming
#pragma once

// cpp headers
#include <utility>
#include <functional>
// os headers
#include <excpt.h>
#include <Windows.h>
#include <WinSock2.h>
// wil headers
#include <wil/resource.h>


#include "ctThreadIocp_base.hpp"

/**
 * @file
 * @brief Thread-pool based OVERLAPPED I/O helper using the Windows Threadpool APIs.
 *
 * This header defines `ctThreadIocp`, a helper that creates a thread-pool IO
 * object and provides a safe, callback-based pattern for using `OVERLAPPED*`
 * with Win32 asynchronous APIs. It also provides the small helper
 * `ctThreadIocpCallbackInfo` which pairs an `OVERLAPPED` with its completion
 * callback.
 */

namespace ctl
{
// typedef used for the std::function to be given to ctThreadIocpCallbackInfo
using ctThreadIocpCallback_t = std::function<void(OVERLAPPED*)>;

/**
 * @brief Structure pairing an `OVERLAPPED` with its completion callback.
 */
struct ctThreadIocpCallbackInfo
{
    OVERLAPPED ov{};
    PVOID padding{}; // padding to maintain alignment
    ctThreadIocpCallback_t callback;

    /**
     * @param _callback [in] Callback to invoke when the OVERLAPPED completes.
     */
    explicit ctThreadIocpCallbackInfo(ctThreadIocpCallback_t&& _callback) noexcept : callback(std::move(_callback))
    {
        ZeroMemory(&ov, sizeof ov);
    }

    ~ctThreadIocpCallbackInfo() noexcept = default;
    ctThreadIocpCallbackInfo(const ctThreadIocpCallbackInfo&) = delete;
    ctThreadIocpCallbackInfo& operator=(const ctThreadIocpCallbackInfo&) = delete;
    ctThreadIocpCallbackInfo(ctThreadIocpCallbackInfo&&) = delete;
    ctThreadIocpCallbackInfo& operator=(ctThreadIocpCallbackInfo&&) = delete;
};

static_assert(sizeof(ctThreadIocpCallbackInfo) == sizeof(OVERLAPPED) + sizeof(PVOID) + sizeof(ctThreadIocpCallback_t));

/**
 * @brief Helper encapsulating the Windows threadpool IO APIs for OVERLAPPED I/O.
 */
class ctThreadIocp : public ctThreadIocp_base
{
public:
    explicit ctThreadIocp(HANDLE _handle, _In_opt_ PTP_CALLBACK_ENVIRON _ptp_env = nullptr) :
        m_tpIo(CreateThreadpoolIo(_handle, IoCompletionCallback, nullptr, _ptp_env))
    {
        THROW_LAST_ERROR_IF_NULL(m_tpIo);
    }

    explicit ctThreadIocp(SOCKET _socket, _In_opt_ PTP_CALLBACK_ENVIRON _ptp_env = nullptr) :
        m_tpIo(CreateThreadpoolIo(reinterpret_cast<HANDLE>(_socket), IoCompletionCallback, nullptr, _ptp_env))
    {
        THROW_LAST_ERROR_IF_NULL(m_tpIo);
    }

    ~ctThreadIocp() noexcept override = default;

    ctThreadIocp(ctThreadIocp&& rhs) noexcept : m_tpIo(std::move(rhs.m_tpIo)) {}
    ctThreadIocp& operator=(ctThreadIocp&& rhs) noexcept { m_tpIo = std::move(rhs.m_tpIo); return *this; }

    OVERLAPPED* new_request(std::function<void(OVERLAPPED*)> _callback) const override
    {
        auto* new_callback = new ctThreadIocpCallbackInfo(std::move(_callback));
        StartThreadpoolIo(m_tpIo.get());
        ZeroMemory(&new_callback->ov, sizeof OVERLAPPED);
        return &new_callback->ov;
    }

    void cancel_request(const OVERLAPPED* pOverlapped) const noexcept override
    {
        CancelThreadpoolIo(m_tpIo.get());
        const auto* const old_request = reinterpret_cast<const ctThreadIocpCallbackInfo*>(pOverlapped);
        delete old_request;
    }

    ctThreadIocp() = delete;
    ctThreadIocp(const ctThreadIocp&) = delete;
    ctThreadIocp& operator=(const ctThreadIocp&) = delete;

private:
    wil::unique_threadpool_io m_tpIo{};

    static void CALLBACK IoCompletionCallback(
        PTP_CALLBACK_INSTANCE /*_instance*/,
        PVOID /*_context*/,
        PVOID _overlapped,
        ULONG /*_ioResult*/,
        ULONG_PTR /*_numberOfBytesTransferred*/,
        PTP_IO /*_io*/) noexcept
    {
        const EXCEPTION_POINTERS* exr = nullptr;
        __try
        {
            const auto* _request = static_cast<ctThreadIocpCallbackInfo*>(_overlapped);
            _request->callback(static_cast<OVERLAPPED*>(_overlapped));
            delete _request;
        }
        __except (exr = GetExceptionInformation(), EXCEPTION_EXECUTE_HANDLER)
        {
            __try
            {
                RaiseException(
                    exr->ExceptionRecord->ExceptionCode,
                    EXCEPTION_NONCONTINUABLE,
                    exr->ExceptionRecord->NumberParameters,
                    exr->ExceptionRecord->ExceptionInformation);
            }
            __except (EXCEPTION_EXECUTE_HANDLER)
            {
                __debugbreak();
            }
        }
    }
};
} // namespace ctl

