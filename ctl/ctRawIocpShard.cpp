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

bool ctRawIocpShard::Initialize(SOCKET listenSocketHint) noexcept
{
    // create IOCP for this shard
    m_iocp = CreateIoCompletionPort(INVALID_HANDLE_VALUE, NULL, 0, 0);
    if (m_iocp == NULL)
    {
        std::cerr << "CreateIoCompletionPort failed: " << GetLastError() << "\n";
        return false;
    }

    // placeholder: create or adopt socket
    m_socket = listenSocketHint;

    // if we have a valid socket, associate it with this IOCP
    if (m_socket != INVALID_SOCKET)
    {
        if (CreateIoCompletionPort(reinterpret_cast<HANDLE>(m_socket), m_iocp, static_cast<ULONG_PTR>(m_id), 0) == NULL)
        {
            std::cerr << "CreateIoCompletionPort (associate) failed: " << GetLastError() << "\n";
            CloseHandle(m_iocp);
            m_iocp = NULL;
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

    m_running = true;
    try
    {
        for (uint32_t i = 0; i < workerCount; ++i)
        {
            m_workers.emplace_back([this]() { this->WorkerThreadMain(); });
        }
    }
    catch (...) {
        m_running = false;
        m_workers.clear();
        return false;
    }

    return true;
}

void ctRawIocpShard::Shutdown() noexcept
{
    m_running = false;

    if (m_iocp != NULL)
    {
        // wake up any waiting threads: post one completion per worker so all blocked
        // GetQueuedCompletionStatus calls return and each thread can exit gracefully
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
    // Minimal loop - wait for completions and no-op
    while (m_running)
    {
        DWORD bytesTransferred = 0;
        ULONG_PTR completionKey = 0;
        LPOVERLAPPED overlapped = nullptr;

        BOOL ok = GetQueuedCompletionStatus(m_iocp, &bytesTransferred, &completionKey, &overlapped, INFINITE);
        if (!m_running) break;
        if (!ok)
        {
            auto err = GetLastError();
            // log and continue; no special-case for WAIT_TIMEOUT since INFINITE is used
            std::cerr << "GetQueuedCompletionStatus failed: " << err << "\n";
            continue;
        }

        // Detect the Shutdown() sentinel posted via PostQueuedCompletionStatus
        if (bytesTransferred == 0 && completionKey == 0 && overlapped == nullptr)
        {
            break; // graceful exit
        }

        // placeholder: process completion
    }
}
