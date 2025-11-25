/*
  ctThreadIocp_shard.hpp

  IO Completion Port backed replacement for ctThreadIocp that uses
  std::thread worker threads. Threads can be affinitized to a set of
  CPU indices provided by the caller.

*/
#pragma once

// reuse the callback info/type from the original header
#include "ctThreadIocp.hpp"

#include <atomic>
#include <thread>
#include <vector>
#include <memory>

namespace ctl
{
// ctThreadIocp_shard
// - creates an IO Completion Port and associates the provided HANDLE/SOCKET
//   with it
// - spawns `numThreads` worker threads that call GetQueuedCompletionStatus
// - when an OVERLAPPED* (that was returned from new_request) completes,
//   the stored std::function callback is invoked on a worker thread and
//   the callback info object is deleted
// - threads may be affinitized by providing a list of CPU indices; each
//   worker will get the CPU index at position (threadIndex % cpus.size())
class ctThreadIocp_shard
{
public:
    explicit ctThreadIocp_shard(HANDLE _handle, size_t numThreads = 0, const std::vector<DWORD>& cpus = {})
        : m_iocp(CreateIoCompletionPort(INVALID_HANDLE_VALUE, nullptr, 0, 0)), m_shutdown(false), m_cpus(cpus)
    {
        THROW_LAST_ERROR_IF_NULL(m_iocp);

        // associate the user handle/socket with our IOCP
        const auto assoc = CreateIoCompletionPort(_handle, m_iocp, 0, 0);
        THROW_LAST_ERROR_IF_NULL(assoc);

        if (numThreads == 0)
        {
            numThreads = std::max<size_t>(1, std::thread::hardware_concurrency());
        }

        m_workers.reserve(numThreads);
        for (size_t i = 0; i < numThreads; ++i)
        {
            m_workers.emplace_back(&ctThreadIocp_shard::WorkerLoop, this, i);
        }
    }

    explicit ctThreadIocp_shard(SOCKET _socket, size_t numThreads = 0, const std::vector<DWORD>& cpus = {})
        : ctThreadIocp_shard(reinterpret_cast<HANDLE>(_socket), numThreads, cpus) // NOLINT(performance-no-int-to-ptr)
    {
    }

    ~ctThreadIocp_shard() noexcept
    {
        ShutdownAndJoin();
    }

    ctThreadIocp_shard(const ctThreadIocp_shard&) = delete;
    ctThreadIocp_shard& operator=(const ctThreadIocp_shard&) = delete;

    // new_request: allocate a callback info and return the OVERLAPPED*
    OVERLAPPED* new_request(std::function<void(OVERLAPPED*)> _callback) const
    {
        auto* new_callback = new ctThreadIocpCallbackInfo(std::move(_callback));
        ZeroMemory(&new_callback->ov, sizeof OVERLAPPED);
        return &new_callback->ov;
    }

    // cancel_request: caller must call this if the API that was given the
    // OVERLAPPED* failed immediately with an error other than ERROR_IO_PENDING
    void cancel_request(const OVERLAPPED* pOverlapped) const noexcept
    {
        const auto* const old_request = reinterpret_cast<const ctThreadIocpCallbackInfo*>(pOverlapped);
        delete old_request;
    }

    // Post a custom completion packet for testing or to inject a completion
    bool post_completion(ULONG_PTR key = 0, DWORD bytes = 0, OVERLAPPED* ov = nullptr) const noexcept
    {
        return !!PostQueuedCompletionStatus(m_iocp, bytes, key, ov);
    }

private:
    HANDLE m_iocp{nullptr};
    std::vector<std::thread> m_workers;
    std::atomic<bool> m_shutdown;
    std::vector<DWORD> m_cpus;

    void WorkerLoop(size_t index) noexcept
    {
        // if CPU indices were provided, affinitize this thread to a single CPU
        if (!m_cpus.empty())
        {
            const DWORD cpu = m_cpus[index % m_cpus.size()];
            KAFFINITY mask = (KAFFINITY)(1ULL << cpu);
            // ignore return value; affinity is best-effort
            SetThreadAffinityMask(GetCurrentThread(), mask);
        }

        DWORD bytesTransferred = 0;
        ULONG_PTR completionKey = 0;
        OVERLAPPED* pOverlapped = nullptr;

        while (!m_shutdown.load(std::memory_order_acquire))
        {
            const BOOL ok = GetQueuedCompletionStatus(m_iocp, &bytesTransferred, &completionKey, &pOverlapped, INFINITE);
            (void)ok;

            // a NULL overlapped is our shutdown signal (or a benign empty post)
            if (pOverlapped == nullptr)
            {
                if (m_shutdown.load(std::memory_order_acquire))
                {
                    break;
                }
                continue;
            }

            // run callback; mimic original SEH handling to avoid TP swallowing SEH
            const EXCEPTION_POINTERS* exr = nullptr;
            __try
            {
                auto* request = reinterpret_cast<ctThreadIocpCallbackInfo*>(pOverlapped);
                request->callback(pOverlapped);
                delete request;
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
    }

    void ShutdownAndJoin() noexcept
    {
        bool expected = false;
        if (!m_shutdown.compare_exchange_strong(expected, true))
        {
            // already shutting down
            return;
        }

        // wake up all workers by posting null OVERLAPPEDs
        for (size_t i = 0; i < m_workers.size(); ++i)
        {
            PostQueuedCompletionStatus(m_iocp, 0, 0, nullptr);
        }

        for (auto& t : m_workers)
        {
            if (t.joinable())
            {
                t.join();
            }
        }

        if (m_iocp)
        {
            CloseHandle(m_iocp);
            m_iocp = nullptr;
        }
    }
};

} // namespace ctl
