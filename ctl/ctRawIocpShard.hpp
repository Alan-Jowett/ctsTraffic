/* Minimal scaffold for ctRawIocpShard
   Provides a RAII class to manage a socket, IOCP, and worker threads for a shard.
*/
#pragma once

#include <Windows.h>
#include <winsock2.h>
#include <cstdint>
#include <vector>
#include <memory>
#include <thread>
#include <atomic>

namespace ctl
{
    class ctRawIocpShard final
    {
    public:
        ctRawIocpShard(uint32_t id) noexcept;
        ~ctRawIocpShard() noexcept;

        // non-copyable
        ctRawIocpShard(const ctRawIocpShard&) = delete;
        ctRawIocpShard& operator=(const ctRawIocpShard&) = delete;

        bool Initialize(SOCKET listenSocketHint = INVALID_SOCKET) noexcept;
        bool StartWorkers(uint32_t workerCount = 1) noexcept;
        void Shutdown() noexcept;

        uint32_t Id() const noexcept { return m_id; }

    private:
        void WorkerThreadMain() noexcept;

        uint32_t m_id;
        HANDLE m_iocp = nullptr;
        SOCKET m_socket = INVALID_SOCKET;
        std::vector<std::thread> m_workers;
        std::atomic<bool> m_running{ false };
    };
}
