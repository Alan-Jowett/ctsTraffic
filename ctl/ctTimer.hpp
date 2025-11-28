/**
 * @file
 * @brief Helper functions for working with high-resolution timers (QPC/QPF)
 *
 * This header provides helpers used to convert milliseconds into relative
 * `FILETIME` values and to safely capture the `QueryPerformanceCounter`
 * value as milliseconds. The internal `Details` namespace caches the
 * `QueryPerformanceFrequency` value via `InitOnceExecuteOnce` to avoid
 * repeated system calls.
 */

// ReSharper disable CppInconsistentNaming
#pragma once

// os headers
#include <Windows.h>
#include <wil/win32_helpers.h>

namespace ctl {
namespace ctTimer {
namespace Details
{
    /**
     * @brief Initialization guard for the cached QPF value.
     */
    // ReSharper disable once CppZeroConstantCanBeReplacedWithNullptr
    static INIT_ONCE g_qpfInitOnce = INIT_ONCE_STATIC_INIT;

    /**
     * @brief Cached QueryPerformanceFrequency value.
     */
    static LARGE_INTEGER g_qpf;

    /**
     * @brief InitOnce callback to capture the QPF on first use.
     *
     * @param initOnce [in] Initialization state pointer (unused)
     * @param param [in] User param (unused)
     * @param context [out] Context (unused)
     * @return BOOL TRUE on success.
     */
    static BOOL CALLBACK QpfInitOnceCallback(_In_ PINIT_ONCE /*initOnce*/, _In_ PVOID /*param*/, _In_ PVOID* /*context*/) noexcept
    {
        QueryPerformanceFrequency(&g_qpf);
        return TRUE;
    }

} // namespace Details

/**
 * @brief Create a negative `FILETIME` representing a relative time specified in milliseconds.
 *
 * @param milliseconds [in] Milliseconds to convert to a negative `FILETIME` representing a relative time.
 * @return FILETIME Negative FILETIME suitable for APIs such as `SetThreadpoolTimer`.
 */
inline FILETIME convert_ms_to_relative_filetime(int64_t milliseconds) noexcept
{
    return wil::filetime::from_int64(-1 * wil::filetime::convert_msec_to_100ns(milliseconds));
}

/**
 * @brief Return the cached `QueryPerformanceFrequency` value.
 *
 * Uses `InitOnceExecuteOnce` to ensure the frequency is queried only once.
 * @return int64_t Frequency (`QPF`) value.
 */
inline int64_t snap_qpf() noexcept
{
    InitOnceExecuteOnce(&Details::g_qpfInitOnce, Details::QpfInitOnceCallback, nullptr, nullptr);
    return Details::g_qpf.QuadPart;
}

#ifdef CTSTRAFFIC_UNIT_TESTS
/**
 * @brief Unit-test shim which returns a deterministic QPC value.
 */
inline int64_t snap_qpc_as_msec() noexcept
{
    return 0;
}
#else
/**
 * @brief Capture the current `QueryPerformanceCounter` value and convert to milliseconds.
 *
 * @return int64_t Milliseconds derived from `QueryPerformanceCounter`.
 */
inline int64_t snap_qpc_as_msec() noexcept
{
    InitOnceExecuteOnce(&Details::g_qpfInitOnce, Details::QpfInitOnceCallback, nullptr, nullptr);
    LARGE_INTEGER qpc;
    QueryPerformanceCounter(&qpc);
    // multiplying by 1000 as (qpc / qpf) == seconds
    return qpc.QuadPart * 1000LL / Details::g_qpf.QuadPart;
}
#endif

} // namespace ctTimer
} // namespace ctl
