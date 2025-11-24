/*

Copyright (c) Microsoft Corporation
All rights reserved.

Licensed under the Apache License, Version 2.0 (the ""License""); you may not use this file except in compliance with the License. You may obtain a copy of the License at http://www.apache.org/licenses/LICENSE-2.0

THIS CODE IS PROVIDED ON AN  *AS IS* BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, EITHER EXPRESS OR IMPLIED, INCLUDING WITHOUT LIMITATION ANY IMPLIED WARRANTIES OR CONDITIONS OF TITLE, FITNESS FOR A PARTICULAR PURPOSE, MERCHANTABLITY OR NON-INFRINGEMENT.

See the Apache Version 2.0 License for specific language governing permissions and limitations under the License.

*/

#include "ctRawIocpShard.hpp"
#include <iostream>
#include <synchapi.h>

using namespace ctl;

ctRawIocpShard::ctRawIocpShard(uint32_t id) noexcept : m_id(id)
{
}

ctRawIocpShard::~ctRawIocpShard() noexcept
{
    Shutdown();
}

static void FreeRecvOp(ctRawIocpShard::RecvOp* op) noexcept
{
    if (op)
    {
        delete op;
    }
}

bool ctRawIocpShard::Initialize(SOCKET listenSocketHint, uint32_t outstandingReceives) noexcept
{
    // create IOCP for this shard
    m_iocp = CreateIoCompletionPort(INVALID_HANDLE_VALUE, NULL, 0, 0);
    if (m_iocp == NULL)
    {
        std::cerr << "CreateIoCompletionPort failed: " << GetLastError() << "\n";
        return false;
    }

    // If no socket provided, create a UDP socket for the shard.
    if (listenSocketHint == INVALID_SOCKET)
    {
        m_socket = WSASocketW(AF_INET, SOCK_DGRAM, IPPROTO_UDP, nullptr, 0, WSA_FLAG_OVERLAPPED);
        if (m_socket == INVALID_SOCKET)
        {
            std::cerr << "WSASocketW failed: " << WSAGetLastError() << "\n";
            CloseHandle(m_iocp);
            m_iocp = NULL;
            return false;
        }
        // bind to any address / ephemeral port so receives can be posted
        sockaddr_in bindAddr;
        ZeroMemory(&bindAddr, sizeof(bindAddr));
        bindAddr.sin_family = AF_INET;
        bindAddr.sin_addr.s_addr = INADDR_ANY;
        bindAddr.sin_port = 0; // ephemeral
        if (bind(m_socket, reinterpret_cast<sockaddr*>(&bindAddr), sizeof(bindAddr)) == SOCKET_ERROR)
        {
            const int gle = WSAGetLastError();
            std::cerr << "bind failed: " << gle << "\n";
            closesocket(m_socket);
            m_socket = INVALID_SOCKET;
            CloseHandle(m_iocp);
            m_iocp = NULL;
            return false;
        }
    }
    else
    {
        m_socket = listenSocketHint;
    }

    // associate socket with IOCP
    if (CreateIoCompletionPort(reinterpret_cast<HANDLE>(m_socket), m_iocp, static_cast<ULONG_PTR>(m_id), 0) == NULL)
    {
        std::cerr << "CreateIoCompletionPort (associate) failed: " << GetLastError() << "\n";
        CloseHandle(m_iocp);
        m_iocp = NULL;
        if (listenSocketHint == INVALID_SOCKET && m_socket != INVALID_SOCKET)
        {
            closesocket(m_socket);
            m_socket = INVALID_SOCKET;
        }
        return false;
    }

    // Pre-allocate and post outstanding receives if requested
    if (outstandingReceives > 0)
    {
        try
        {
            m_recvOps.reserve(outstandingReceives);
            for (uint32_t i = 0; i < outstandingReceives; ++i)
            {
                RecvOp* op = new RecvOp();
                ZeroMemory(&op->Overlapped, sizeof(op->Overlapped));
                op->FromLen = sizeof(op->From);
                op->Flags = 0;
                op->Buffer.buf = op->Data;
                op->Buffer.len = sizeof(op->Data);

                DWORD bytesReceived = 0;
                const int rc = WSARecvFrom(m_socket, &op->Buffer, 1, &bytesReceived, &op->Flags, reinterpret_cast<sockaddr*>(&op->From), &op->FromLen, &op->Overlapped, nullptr);
                if (rc == SOCKET_ERROR)
                {
                    const int gle = WSAGetLastError();
                    if (gle != WSA_IO_PENDING)
                    {
                        FreeRecvOp(op);
                        // on failure, clean up previous ops
                        for (auto p : m_recvOps) FreeRecvOp(p);
                        m_recvOps.clear();
                        return false;
                    }
                }
                m_recvOps.push_back(op);
            }
        }
        catch (...) {
            for (auto p : m_recvOps) FreeRecvOp(p);
            m_recvOps.clear();
            return false;
        }
    }

    return true;
}

bool ctRawIocpShard::StartWorkers(uint32_t workerCount) noexcept
{
    if (m_iocp == NULL)
    {
        return false;
    }

    m_running.store(true, std::memory_order_release);
    try
    {
        for (uint32_t i = 0; i < workerCount; ++i)
        {
            m_workers.emplace_back([this]() { this->WorkerThreadMain(); });
        }
    }
    catch (...) {
        m_running.store(false, std::memory_order_release);
        for (auto& t : m_workers)
        {
            if (t.joinable())
            {
                t.join();
            }
        }
        m_workers.clear();
        return false;
    }

    return true;
}

void ctRawIocpShard::Shutdown() noexcept
{
    m_running.store(false, std::memory_order_release);

    if (m_iocp != NULL)
    {
        const auto wakeCount = m_workers.size();
        for (size_t i = 0; i < wakeCount; ++i)
        {
            PostQueuedCompletionStatus(m_iocp, 0, 0, NULL);
        }
    }

    for (auto& t : m_workers)
    {
        if (t.joinable())
        {
            t.join();
        }
    }
    m_workers.clear();

    // free outstanding recv ops
    for (auto p : m_recvOps) FreeRecvOp(p);
    m_recvOps.clear();

    if (m_iocp != NULL)
    {
        CloseHandle(m_iocp);
        m_iocp = NULL;
    }

    if (m_socket != INVALID_SOCKET)
    {
        closesocket(m_socket);
        m_socket = INVALID_SOCKET;
    }
}

void ctRawIocpShard::WorkerThreadMain() noexcept
{
    // Loop waiting for completions and re-posting receives
    while (m_running.load(std::memory_order_acquire))
    {
        DWORD bytesTransferred = 0;
        ULONG_PTR completionKey = 0;
        LPOVERLAPPED overlapped = nullptr;

        BOOL ok = GetQueuedCompletionStatus(m_iocp, &bytesTransferred, &completionKey, &overlapped, INFINITE);
        if (!m_running.load(std::memory_order_acquire)) break;
        if (!ok && overlapped == nullptr)
        {
            auto err = GetLastError();
            std::cerr << "GetQueuedCompletionStatus failed: " << err << "\n";
            continue;
        }

        // Detect the Shutdown() sentinel
        if (bytesTransferred == 0 && completionKey == 0 && overlapped == nullptr)
        {
            break; // graceful exit
        }

        // We expect overlapped to be one of our RecvOp Overlappeds
        // compute RecvOp* from OVERLAPPED* using offsetof
        RecvOp* op = reinterpret_cast<RecvOp*>(reinterpret_cast<char*>(overlapped) - offsetof(RecvOp, Overlapped));

        // Process packet (no-op for now)

        // Re-post the receive
        ZeroMemory(&op->Overlapped, sizeof(op->Overlapped));
        op->FromLen = sizeof(op->From);
        op->Flags = 0;
        op->Buffer.buf = op->Data;
        op->Buffer.len = sizeof(op->Data);

        DWORD bytes = 0;
        const int rc = WSARecvFrom(m_socket, &op->Buffer, 1, &bytes, &op->Flags, reinterpret_cast<sockaddr*>(&op->From), &op->FromLen, &op->Overlapped, nullptr);
        if (rc == SOCKET_ERROR)
        {
            const int gle = WSAGetLastError();
            if (gle != WSA_IO_PENDING)
            {
                std::cerr << "WSARecvFrom re-post failed: " << gle << "\n";
                // drop this op and continue
                FreeRecvOp(op);
            }
        }
    }
}
