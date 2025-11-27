/*
Upload client implementation - based on ctsMediaStreamClient
*/
// cpp headers
#include <memory>
// os headers
#include <Windows.h>
#include <WinSock2.h>
// ctl headers
#include <ctSockaddr.hpp>
// project headers
#include "ctsMediaUploadProtocol.hpp"
#include "ctsMediaUploadClient.h"
#include "ctsWinsockLayer.h"
#include "ctsIOTask.hpp"
#include "ctsIOPattern.h"
#include "ctsSocket.h"
#include "ctsConfig.h"
// wil headers always included last
#include <wil/stl.h>
#include <wil/resource.h>

namespace ctsTraffic
{
    struct IoImplStatus
    {
        int m_errorCode = 0;
        bool m_continueIo = false;
    };

    static IoImplStatus ctsMediaUploadClientIoImpl(
        const std::shared_ptr<ctsSocket>& sharedSocket,
        SOCKET socket,
        const std::shared_ptr<ctsIoPattern>& lockedPattern,
        const ctsTask& task) noexcept;

    static void ctsMediaUploadClientIoCompletionCallback(
        _In_ OVERLAPPED* pOverlapped,
        const std::weak_ptr<ctsSocket>& weakSocket,
        const ctsTask& task
    ) noexcept;

    static void ctsMediaUploadClientConnectionCompletionCallback(
        _In_ OVERLAPPED* pOverlapped,
        const std::weak_ptr<ctsSocket>& weakSocket,
        const ctl::ctSockaddr& targetAddress
    ) noexcept;

    void ctsMediaUploadClient(const std::weak_ptr<ctsSocket>& weakSocket) noexcept
    {
        const auto sharedSocket(weakSocket.lock());
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

        lockedPattern->RegisterCallback(
            [weakSocket](const ctsTask& task) noexcept
            {
                const auto lambdaSharedSocket(weakSocket.lock());
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
                    const IoImplStatus status = ctsMediaUploadClientIoImpl(
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
        IoImplStatus status = ctsMediaUploadClientIoImpl(
            sharedSocket,
            lockedSocket.GetSocket(),
            lockedPattern,
            lockedPattern->InitiateIo());
        while (status.m_continueIo)
        {
            status = ctsMediaUploadClientIoImpl(
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

    void ctsMediaUploadClientConnect(const std::weak_ptr<ctsSocket>& weakSocket) noexcept
    {
        const auto sharedSocket(weakSocket.lock());
        if (!sharedSocket)
        {
            return;
        }

        const auto lockedSocket = sharedSocket->AcquireSocketLock();
        if (lockedSocket.GetSocket() == INVALID_SOCKET)
        {
            sharedSocket->CompleteState(WSAECONNABORTED);
            return;
        }

        const auto socket = lockedSocket.GetSocket();
        const ctl::ctSockaddr targetAddress(sharedSocket->GetRemoteSockaddr());
        const ctsTask startTask = ctsMediaUploadMessage::Construct(MediaUploadAction::START);

        const auto response = ctsWSASendTo(
            sharedSocket,
            lockedSocket.GetSocket(),
            startTask,
            [weakSocket, targetAddress](OVERLAPPED* ov) noexcept
            {
                ctsMediaUploadClientConnectionCompletionCallback(ov, weakSocket, targetAddress);
            });

        if (NO_ERROR == response.m_errorCode)
        {
            ctl::ctSockaddr localAddr;
            auto localAddrLen = localAddr.length();
            if (0 == getsockname(socket, localAddr.sockaddr(), &localAddrLen))
            {
                sharedSocket->SetLocalSockaddr(localAddr);
            }
            sharedSocket->SetRemoteSockaddr(targetAddress);

            ctsConfig::SetPostConnectOptions(socket, targetAddress);

            ctsConfig::PrintNewConnection(localAddr, targetAddress);

            PRINT_DEBUG_INFO(
                L"\t\tctsMediaUploadClient sent its START message to %ws\n",
                targetAddress.writeCompleteAddress().c_str());
        }

        if (response.m_errorCode != WSA_IO_PENDING)
        {
            sharedSocket->CompleteState(response.m_errorCode);
        }
    }

    IoImplStatus ctsMediaUploadClientIoImpl(
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
                    ctsMediaUploadClientIoCompletionCallback(ov, weak_reference, task);
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
                        "ctsMediaUploadClientIoImpl: received an unexpected IOStatus in the ctsIOTask (%p)", &task);
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
                        PRINT_DEBUG_INFO(L"\t\tIO Failed: %hs (%u) [ctsMediaUploadClient]\n",
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
                        FAIL_FAST_MSG("ctsMediaUploadClientIoImpl: unknown ctsSocket::IOStatus - %d\n", protocolStatus);
                    }

                    const auto ioCount = sharedSocket->DecrementIo();
                    FAIL_FAST_IF_MSG(
                        0 == ioCount,
                        "ctsMediaUploadClient : ctsSocket::io_count fell to zero while the Impl function was called (dt %p ctsTraffic::ctsSocket)",
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
            break;
        }

        return returnStatus;
    }

    void ctsMediaUploadClientIoCompletionCallback(
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
                L"MediaUpload Client: %ws failed with WSAEMSGSIZE: received [%u bytes]",
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
                    status = ctsMediaUploadClientIoImpl(
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
                    L"MediaUpload Client: IO failed (%ws) with error %d",
                    task.m_ioAction == ctsTaskAction::Recv ? L"WSARecvFrom" : L"WSASendTo", gle);
            }
            else
            {
                ctsConfig::PrintErrorInfo(
                    L"MediaUpload Client: IO succeeded (%ws) but the ctsIOProtocol failed the stream (%u)",
                    task.m_ioAction == ctsTaskAction::Recv ? L"WSARecvFrom" : L"WSASendTo",
                    lockedPattern->GetLastPatternError());
            }

            sharedSocket->CloseSocket();
            gle = static_cast<int>(lockedPattern->GetLastPatternError());
            break;

        default:
            FAIL_FAST_MSG(
                "ctsMediaUploadClientIoCompletionCallback: unknown ctsSocket::IOStatus - %d\n",
                protocolStatus);
        }

        if (sharedSocket->DecrementIo() == 0)
        {
            sharedSocket->CompleteState(gle);
        }
    }

    void ctsMediaUploadClientConnectionCompletionCallback(
        _In_ OVERLAPPED* pOverlapped,
        const std::weak_ptr<ctsSocket>& weakSocket,
        const ctl::ctSockaddr& targetAddress) noexcept
    {
        const auto sharedSocket(weakSocket.lock());
        if (!sharedSocket)
        {
            return;
        }

        int gle = NO_ERROR;
        const auto lockedSocket = sharedSocket->AcquireSocketLock();
        const auto socket = lockedSocket.GetSocket();
        if (socket == INVALID_SOCKET)
        {
            gle = WSAECONNABORTED;
        }
        else
        {
            DWORD transferred;
            DWORD flags;
            if (!WSAGetOverlappedResult(socket, pOverlapped, &transferred, FALSE, &flags))
            {
                gle = WSAGetLastError();
            }
        }

        ctsConfig::PrintErrorIfFailed("\tWSASendTo (START request)", gle);

        if (NO_ERROR == gle)
        {
            ctl::ctSockaddr localAddr;
            int localAddrLen = localAddr.length();
            if (0 == getsockname(socket, localAddr.sockaddr(), &localAddrLen))
            {
                sharedSocket->SetLocalSockaddr(localAddr);
            }
            sharedSocket->SetRemoteSockaddr(targetAddress);
            ctsConfig::SetPostConnectOptions(socket, targetAddress);
            ctsConfig::PrintNewConnection(localAddr, targetAddress);
        }

        sharedSocket->CompleteState(gle);
    }
} // namespace
