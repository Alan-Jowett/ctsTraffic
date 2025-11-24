/*

Copyright (c) Microsoft Corporation
All rights reserved.

Licensed under the Apache License, Version 2.0 (the ""License""); you may not use this file except in compliance with the License. You may obtain a copy of the License at http://www.apache.org/licenses/LICENSE-2.0

THIS CODE IS PROVIDED ON AN  *AS IS* BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, EITHER EXPRESS OR IMPLIED, INCLUDING WITHOUT LIMITATION ANY IMPLIED WARRANTIES OR CONDITIONS OF TITLE, FITNESS FOR A PARTICULAR PURPOSE, MERCHANTABLITY OR NON-INFRINGEMENT.

See the Apache Version 2.0 License for specific language governing permissions and limitations under the License.

*/

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
#include <ws2tcpip.h>

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

        bool Initialize(SOCKET listenSocketHint = INVALID_SOCKET, uint32_t outstandingReceives = 0) noexcept;
        bool StartWorkers(uint32_t workerCount = 1) noexcept;
        void Shutdown() noexcept;

        uint32_t Id() const noexcept { return m_id; }

        // Outstanding receive operation owned by this shard. Public so
        // helper functions in the implementation can reference the type.
        struct RecvOp
        {
            OVERLAPPED Overlapped;
            WSABUF Buffer;
            sockaddr_storage From;
            int FromLen;
            DWORD Flags;
            // payload storage
            char Data[65536];
        };

    private:
        void WorkerThreadMain() noexcept;

        uint32_t m_id;
        HANDLE m_iocp = nullptr;
        SOCKET m_socket = INVALID_SOCKET;
        std::vector<std::thread> m_workers;
        std::atomic<bool> m_running{ false };
        // Outstanding receive operations owned by this shard
        std::vector<RecvOp*> m_recvOps;
    };
}
