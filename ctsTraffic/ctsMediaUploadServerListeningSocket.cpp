// cpp headers
#include <exception>
#include <memory>
#include <utility>
// os headers
#include <Windows.h>
#include <WinSock2.h>
// ctl headers
#include <ctThreadIocp.hpp>
#include <ctSockaddr.hpp>
// project headers
#include "ctsMediaUploadServerListeningSocket.h"
#include "ctsMediaUploadServer.h"
#include "ctsMediaUploadProtocol.hpp"
#include "ctsConfig.h"
// wil headers always included last
#include <wil/stl.h>
#include <wil/resource.h>

namespace ctsTraffic
{
ctsMediaUploadServerListeningSocket::ctsMediaUploadServerListeningSocket(wil::unique_socket&& listeningSocket, ctl::ctSockaddr listeningAddr) :
    m_threadIocp(std::make_shared<ctl::ctThreadIocp>(listeningSocket.get(), ctsConfig::g_configSettings->pTpEnvironment)),
    m_listeningSocket(std::move(listeningSocket)),
    m_listeningAddr(std::move(listeningAddr))
{
    FAIL_FAST_IF_MSG(
        !!(ctsConfig::g_configSettings->Options & ctsConfig::OptionType::HandleInlineIocp),
        "ctsMediaUploadServer sockets must not have HANDLE_INLINE_IOCP set on its datagram sockets");
}

ctsMediaUploadServerListeningSocket::~ctsMediaUploadServerListeningSocket() noexcept
{
    {
        const auto lock = m_listeningSocketLock.lock();
        m_listeningSocket.reset();
    }
    m_threadIocp.reset();
}

SOCKET ctsMediaUploadServerListeningSocket::GetSocket() const noexcept
{
    const auto lock = m_listeningSocketLock.lock();
    return m_listeningSocket.get();
}

ctl::ctSockaddr ctsMediaUploadServerListeningSocket::GetListeningAddress() const noexcept
{
    return m_listeningAddr;
}

void ctsMediaUploadServerListeningSocket::InitiateRecv() noexcept
{
    int error = SOCKET_ERROR;
    uint32_t failureCounter = 0;
    while (error != NO_ERROR)
    {
        try
        {
            const auto lock = m_listeningSocketLock.lock();
            if (m_listeningSocket)
            {
                WSABUF wsaBuffer{};
                wsaBuffer.buf = m_recvBuffer.data();
                wsaBuffer.len = static_cast<ULONG>(m_recvBuffer.size());
                ::ZeroMemory(m_recvBuffer.data(), m_recvBuffer.size());

                m_recvFlags = 0;
                m_remoteAddr.reset(m_remoteAddr.family(), ctl::ctSockaddr::AddressType::Any);
                m_remoteAddrLen = m_remoteAddr.length();
                OVERLAPPED* pOverlapped = m_threadIocp->new_request(
                    [this](OVERLAPPED* pCallbackOverlapped) noexcept {
                        RecvCompletion(pCallbackOverlapped);
                    });

                error = WSARecvFrom(
                    m_listeningSocket.get(),
                    &wsaBuffer,
                    1,
                    nullptr,
                    &m_recvFlags,
                    m_remoteAddr.sockaddr(),
                    &m_remoteAddrLen,
                    pOverlapped,
                    nullptr);
                if (SOCKET_ERROR == error)
                {
                    error = WSAGetLastError();
                    if (WSA_IO_PENDING == error)
                    {
                        error = NO_ERROR;
                    }
                    else
                    {
                        m_threadIocp->cancel_request(pOverlapped);
                        if (WSAECONNRESET == error)
                        {
                        }
                        else
                        {
                            ctsConfig::PrintErrorInfo(
                                L"WSARecvFrom failed (SOCKET %Iu) with error (%d)",
                                m_listeningSocket.get(), error);
                        }
                    }
                }
                else
                {
                    error = NO_ERROR;
                }
            }
            else
            {
                break;
            }
        }
        catch (...)
        {
            error = static_cast<int>(ctsConfig::PrintThrownException());
        }

        if (error != NO_ERROR && error != WSAECONNRESET)
        {
            ctsConfig::g_configSettings->UdpStatusDetails.m_errorFrames.Increment();
            ++failureCounter;

            ctsConfig::PrintErrorInfo(
                L"MediaUpload Server : WSARecvFrom failed (%d) %u times in a row trying to get another recv posted",
                error, failureCounter);

            FAIL_FAST_IF_MSG(
                0 == failureCounter % 10,
                "ctsMediaUploadServer has failed to post another recv - it cannot accept any more client connections");

            Sleep(10);
        }
    }
}

void ctsMediaUploadServerListeningSocket::RecvCompletion(OVERLAPPED* pOverlapped) noexcept
{
    std::function<void()> pimplOperation(nullptr);

    try
    {
        {
            const auto lock = m_listeningSocketLock.lock();
            if (!m_listeningSocket)
            {
                return;
            }

            DWORD bytesReceived{};
            if (!WSAGetOverlappedResult(m_listeningSocket.get(), pOverlapped, &bytesReceived, FALSE, &m_recvFlags))
            {
                if (WSAECONNRESET == WSAGetLastError())
                {
                    if (!m_priorFailureWasConnectionReset)
                    {
                        ctsConfig::PrintErrorInfo(L"ctsMediaUploadServer - WSARecvFrom failed as a prior WSASendTo from this socket silently failed with port unreachable");
                    }
                    m_priorFailureWasConnectionReset = true;
                }
                else
                {
                    ctsConfig::PrintErrorInfo(
                        L"ctsMediaUploadServer - WSARecvFrom failed [%d]", WSAGetLastError());
                    ctsConfig::g_configSettings->UdpStatusDetails.m_errorFrames.Increment();
                    m_priorFailureWasConnectionReset = false;
                }
            }
            else
            {
                m_priorFailureWasConnectionReset = false;
                // try to parse as a control message first
                bool parsedAsControl = false;
                try
                {
                    const ctsMediaUploadMessage message(ctsMediaUploadMessage::Extract(m_recvBuffer.data(), bytesReceived));
                    switch (message.m_action)
                    {
                        case MediaUploadAction::START:
                            PRINT_DEBUG_INFO(
                                L"\t\tctsMediaUploadServer - processing START from %ws\n",
                                m_remoteAddr.writeCompleteAddress().c_str());
    #ifndef TESTING_IGNORE_START
                            pimplOperation = [this] {
                                ctsMediaUploadServerImpl::Start(m_listeningSocket.get(), m_listeningAddr, m_remoteAddr);
                            };
    #endif
                            parsedAsControl = true;
                            break;

                        default:
                            // unknown control; fall through to treat as data
                            parsedAsControl = false;
                            break;
                    }
                }
                catch (...) // not a control message: treat as data
                {
                    parsedAsControl = false;
                }

                if (!parsedAsControl)
                {
                    // dispatch datagram to server impl to either route to a connected socket or queue
                    ctsMediaUploadServerImpl::DispatchDatagram(m_listeningSocket.get(), m_remoteAddr, m_recvBuffer.data(), static_cast<uint32_t>(bytesReceived));
                }
            }
        }

        if (pimplOperation)
        {
            pimplOperation();
        }
    }
    catch (...)
    {
        ctsConfig::PrintThrownException();
    }

    InitiateRecv();
}
} // namespace
