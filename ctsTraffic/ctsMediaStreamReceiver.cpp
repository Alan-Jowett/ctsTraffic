/*
Implementation of ctsMediaStreamReceiver
*/

#include "ctsMediaStreamReceiver.h"

// cpp headers
#include <memory>
// os headers
#include <Windows.h>
#include <WinSock2.h>
// ctl headers
#include <ctSockaddr.hpp>
// project headers
#include "ctsIOPattern.h"
#include "ctsIOTask.hpp"
#include "ctsConfig.h"
#include "ctsWinsockLayer.h"
#include "ctsSocket.h"

#include <wil/stl.h>
#include <wil/resource.h>

namespace ctsTraffic
{
    ctsMediaStreamReceiver::ctsMediaStreamReceiver(const std::shared_ptr<ctsSocket>& socket, bool passiveReceive) noexcept
        : m_socket(socket), m_passiveReceive(passiveReceive)
    {
    }

    void ctsMediaStreamReceiver::Start() noexcept
    {
        // If in passive receive mode, do not start IO processing.
        if (m_passiveReceive)
        {
            return;
        }

        const auto sharedSocket = m_socket;
        if (!sharedSocket)
        {
            return;
        }

        const auto lockedSocket = sharedSocket->AcquireSocketLock();
        const auto lockedPattern = lockedSocket.GetPattern();
        if (!lockedPattern || lockedSocket.GetSocket() == INVALID_SOCKET)
        {
            return;
        }

        // always register our ctsIOPattern callback since it's necessary for this IO Pattern
        lockedPattern->RegisterCallback(
            [weakSocket = std::weak_ptr<ctsSocket>(sharedSocket)](const ctsTask& task) noexcept
            {
                const auto lambdaSharedSocket = weakSocket.lock();
                if (!lambdaSharedSocket)
                {
                    return;
                }

                const auto lambdaLockedSocket = lambdaSharedSocket->AcquireSocketLock();
                const auto lambdaLockedPattern = lambdaLockedSocket.GetPattern();
                if (!lambdaLockedPattern || lambdaLockedSocket.GetSocket() == INVALID_SOCKET)
                {
                    return;
                }

                if (lambdaSharedSocket->IncrementIo() > 1)
                {
                    const IoImplStatus status = IoImpl(
                        lambdaSharedSocket, lambdaLockedSocket.GetSocket(), lambdaLockedPattern, task);
                    if (lambdaSharedSocket->DecrementIo() == 0)
                    {
                        lambdaSharedSocket->CompleteState(status.m_errorCode);
                    }
                }
                else
                {
                    lambdaSharedSocket->DecrementIo();
                }
            });

        sharedSocket->IncrementIo();
        IoImplStatus status = IoImpl(
            sharedSocket,
            lockedSocket.GetSocket(),
            lockedPattern,
            lockedPattern->InitiateIo());
        while (status.m_continueIo)
        {
            status = IoImpl(
                sharedSocket,
                lockedSocket.GetSocket(),
                lockedPattern,
                lockedPattern->InitiateIo());
        }
        if (0 == sharedSocket->DecrementIo())
        {
            sharedSocket->CompleteState(status.m_errorCode);
        }
    }

    ctsMediaStreamReceiver::IoImplStatus ctsMediaStreamReceiver::IoImpl(
        const std::shared_ptr<ctsSocket>& sharedSocket,
        SOCKET socket,
        const std::shared_ptr<ctsIoPattern>& lockedPattern,
        const ctsTask& task) noexcept
    {
        IoImplStatus returnStatus;

        switch (task.m_ioAction)
        {
        case ctsTaskAction::Send:
            [[fallthrough]];
        case ctsTaskAction::Recv:
            {
                sharedSocket->IncrementIo();
                auto callback = [weak_reference = std::weak_ptr(sharedSocket), task](OVERLAPPED* ov) noexcept
                {
                    IoCompletionCallback(ov, weak_reference, task);
                };

                PCSTR functionName{};
                wsIOResult result;
                if (ctsTaskAction::Send == task.m_ioAction)
                {
                    functionName = "WSASendTo";
                    result = ctsWSASendTo(sharedSocket, socket, task, std::move(callback));
                }
                else if (ctsTaskAction::Recv == task.m_ioAction)
                {
                    functionName = "WSARecvFrom";
                    result = ctsWSARecvFrom(sharedSocket, socket, task, std::move(callback));
                }
                else
                {
                    FAIL_FAST_MSG(
                        "ctsMediaStreamReceiver::IoImpl: received an unexpected IOStatus in the ctsIOTask (%p)", &task);
                }

                if (WSA_IO_PENDING == result.m_errorCode)
                {
                    returnStatus.m_errorCode = static_cast<int>(result.m_errorCode);
                    returnStatus.m_continueIo = true;
                }
                else
                {
                    if (result.m_errorCode != 0)
                    {
                        PRINT_DEBUG_INFO(L"\t\tIO Failed: %hs (%u) [ctsMediaStreamReceiver]\n",
                                         functionName,
                                         result.m_errorCode);
                    }

                    switch (const auto protocolStatus = lockedPattern->CompleteIo(
                        task, result.m_bytesTransferred, result.m_errorCode))
                    {
                    case ctsIoStatus::ContinueIo:
                        returnStatus.m_errorCode = NO_ERROR;
                        returnStatus.m_continueIo = true;
                        break;

                    case ctsIoStatus::CompletedIo:
                        sharedSocket->CloseSocket();
                        returnStatus.m_errorCode = NO_ERROR;
                        returnStatus.m_continueIo = false;
                        break;

                    case ctsIoStatus::FailedIo:
                        ctsConfig::PrintErrorIfFailed(functionName, result.m_errorCode);
                        sharedSocket->CloseSocket();
                        returnStatus.m_errorCode = static_cast<int>(lockedPattern->GetLastPatternError());
                        returnStatus.m_continueIo = false;
                        break;

                    default:
                        FAIL_FAST_MSG("ctsMediaStreamReceiver::IoImpl: unknown ctsSocket::IOStatus - %d\n", protocolStatus);
                    }

                    const auto ioCount = sharedSocket->DecrementIo();
                    FAIL_FAST_IF_MSG(
                        0 == ioCount,
                        "ctsMediaStreamReceiver : ctsSocket::io_count fell to zero while the Impl function was called (dt %p ctsTraffic::ctsSocket)",
                        sharedSocket.get());
                }

                break;
            }

        case ctsTaskAction::None:
            {
                returnStatus.m_errorCode = NO_ERROR;
                returnStatus.m_continueIo = false;
                break;
            }

        case ctsTaskAction::Abort:
            {
                lockedPattern->CompleteIo(task, 0, 0);
                sharedSocket->CloseSocket();

                returnStatus.m_errorCode = NO_ERROR;
                returnStatus.m_continueIo = false;
                break;
            }

        case ctsTaskAction::FatalAbort:
            {
                lockedPattern->CompleteIo(task, 0, 0);
                sharedSocket->CloseSocket();

                returnStatus.m_errorCode = static_cast<int>(lockedPattern->GetLastPatternError());
                returnStatus.m_continueIo = false;
                break;
            }

        case ctsTaskAction::GracefulShutdown:
        case ctsTaskAction::HardShutdown:
        default:
            // Intentionally fall through and ignore shutdown actions here: the
            // media-stream receiver is receive-only and does not need to perform
            // any shutdown handling. `returnStatus` is default-initialized
            // (errorCode=0, continueIo=false) so no further handling is required.
            break;
        }

        return returnStatus;
    }

    void ctsMediaStreamReceiver::IoCompletionCallback(
        _In_ OVERLAPPED* pOverlapped,
        const std::weak_ptr<ctsSocket>& weakSocket,
        const ctsTask& task) noexcept
    {
        const auto sharedSocket(weakSocket.lock());
        if (!sharedSocket)
        {
            return;
        }

        const auto lockedSocket = sharedSocket->AcquireSocketLock();
        const auto lockedPattern = lockedSocket.GetPattern();
        if (!lockedPattern)
        {
            sharedSocket->DecrementIo();
            sharedSocket->CompleteState(WSAECONNABORTED);
            return;
        }

        const auto socket = lockedSocket.GetSocket();

        int gle = NO_ERROR;
        DWORD transferred = 0;
        {
            if (socket != INVALID_SOCKET)
            {
                DWORD flags;
                if (!WSAGetOverlappedResult(socket, pOverlapped, &transferred, FALSE, &flags))
                {
                    gle = WSAGetLastError();
                }
            }
            else
            {
                gle = NO_ERROR;
            }
        }

        if (gle == WSAEMSGSIZE)
        {
            ctsConfig::PrintErrorInfo(
                L"MediaStream Client ConnectedSocket: %ws failed with WSAEMSGSIZE: received [%u bytes]",
                task.m_ioAction == ctsTaskAction::Recv ? L"WSARecvFrom" : L"WSASendTo", transferred);
            gle = NO_ERROR;
        }

        switch (const ctsIoStatus protocolStatus = lockedPattern->CompleteIo(task, transferred, gle))
        {
        case ctsIoStatus::ContinueIo:
            {
                IoImplStatus status;
                do
                {
                    status = IoImpl(
                        sharedSocket,
                        lockedSocket.GetSocket(),
                        lockedPattern,
                        lockedPattern->InitiateIo());
                }
                while (status.m_continueIo);

                gle = status.m_errorCode;
                break;
            }

        case ctsIoStatus::CompletedIo:
            sharedSocket->CloseSocket();
            gle = NO_ERROR;
            break;

        case ctsIoStatus::FailedIo:
            if (gle != 0)
            {
                ctsConfig::PrintErrorInfo(
                    L"MediaStream Client ConnectedSocket: IO failed (%ws) with error %d",
                    task.m_ioAction == ctsTaskAction::Recv ? L"WSARecvFrom" : L"WSASendTo", gle);
            }
            else
            {
                ctsConfig::PrintErrorInfo(
                    L"MediaStream Client ConnectedSocket: IO succeeded (%ws) but the ctsIOProtocol failed the stream (%u)",
                    task.m_ioAction == ctsTaskAction::Recv ? L"WSARecvFrom" : L"WSASendTo",
                    lockedPattern->GetLastPatternError());
            }

            sharedSocket->CloseSocket();
            gle = static_cast<int>(lockedPattern->GetLastPatternError());
            break;

        default:
            FAIL_FAST_MSG(
                "ctsMediaStreamReceiver::IoCompletionCallback: unknown ctsSocket::IOStatus - %d\n",
                protocolStatus);
        }

        if (sharedSocket->DecrementIo() == 0)
        {
            sharedSocket->CompleteState(gle);
        }
    }

    void ctsMediaStreamReceiver::OnDataReceived(const char* buffer, uint32_t bufferLength) noexcept
    {
        auto sharedSocket = m_socket;
        const auto lockedSocket = sharedSocket->AcquireSocketLock();
        const auto lockedPattern = lockedSocket.GetPattern();
        if (!lockedPattern || m_closed)
        {
            return;
        }

        ctsTask task = lockedPattern->InitiateIo();

        if (task.m_ioAction != ctsTaskAction::Recv)
        {
            return;
        }

        wsIOResult result;
        result.m_bytesTransferred = std::min(bufferLength, task.m_bufferLength);
        std::memcpy(task.m_buffer + task.m_bufferOffset, buffer, result.m_bytesTransferred);
        result.m_errorCode = NO_ERROR;

        switch (const auto protocolStatus = lockedPattern->CompleteIo(
            task, result.m_bytesTransferred, result.m_errorCode))
        {
        case ctsIoStatus::ContinueIo:
            break;

        case ctsIoStatus::CompletedIo:
            sharedSocket->CloseSocket();
            sharedSocket->CompleteState(NO_ERROR);
            m_closed = true;
            break;

        case ctsIoStatus::FailedIo:
            ctsConfig::PrintErrorIfFailed("OnDataReceived", result.m_errorCode);
            sharedSocket->CloseSocket();
            sharedSocket->CompleteState(lockedPattern->GetLastPatternError());
            m_closed = true;
            break;

        default:
            FAIL_FAST_MSG("ctsMediaStreamReceiver::IoImpl: unknown ctsSocket::IOStatus - %d\n", protocolStatus);
        }
    }


} // namespace ctsTraffic
