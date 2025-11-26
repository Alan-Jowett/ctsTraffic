/*
Copyright (c) Microsoft Corporation
All rights reserved.

Licensed under the Apache License, Version 2.0 (the "License"); you may not use this file except in compliance with the License. You may obtain a copy of the License at http://www.apache.org/licenses/LICENSE-2.0

THIS CODE IS PROVIDED ON AN  *AS IS* BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, EITHER EXPRESS OR IMPLIED, INCLUDING WITHOUT LIMITATION ANY IMPLIED WARRANTIES OR CONDITIONS OF TITLE, FITNESS FOR A PARTICULAR PURPOSE, MERCHANTABLITY OR NON-INFRINGEMENT.

See the Apache Version 2.0 License for specific language governing permissions and limitations under the License.
*/

// Implementation of the server-side send extraction

#include "ctsMediaStreamSend.h"
#include "ctsMediaStreamProtocol.hpp"
#include "ctsConfig.h"
#include <Windows.h>
#include <WinSock2.h>

namespace ctsTraffic
{
    wsIOResult ConnectedSocketSendImpl(_In_ ctsMediaStreamServerConnectedSocket* connectedSocket) noexcept
    {
        const SOCKET socket = connectedSocket->GetSendingSocket();
        if (INVALID_SOCKET == socket)
        {
            return wsIOResult(WSA_OPERATION_ABORTED);
        }

        const ctl::ctSockaddr& remoteAddr(connectedSocket->GetRemoteAddress());
        const ctsTask nextTask = connectedSocket->GetNextTask();

        wsIOResult returnResults;
        if (ctsTask::BufferType::UdpConnectionId == nextTask.m_bufferType)
        {
            // making a synchronous call
            WSABUF wsaBuffer;
            wsaBuffer.buf = nextTask.m_buffer;
            wsaBuffer.len = nextTask.m_bufferLength;

            const auto sendResult = WSASendTo(
                socket,
                &wsaBuffer,
                1,
                &returnResults.m_bytesTransferred,
                0,
                remoteAddr.sockaddr(),
                remoteAddr.length(),
                nullptr,
                nullptr);

            if (SOCKET_ERROR == sendResult)
            {
                const auto error = WSAGetLastError();
                ctsConfig::PrintErrorInfo(
                    L"WSASendTo(%Iu, %ws) for the Connection-ID failed [%d]",
                    socket,
                    remoteAddr.writeCompleteAddress().c_str(),
                    error);
                return wsIOResult(error);
            }
        }
        else
        {
            const auto sequenceNumber = connectedSocket->IncrementSequence();
            ctsMediaStreamSendRequests sendingRequests(
                nextTask.m_bufferLength, // total bytes to send
                sequenceNumber,
                nextTask.m_buffer);
            for (auto& sendRequest : sendingRequests)
            {
                // making a synchronous call
                DWORD bytesSent{};
                const auto sendResult = WSASendTo(
                    socket,
                    sendRequest.data(),
                    static_cast<DWORD>(sendRequest.size()),
                    &bytesSent,
                    0,
                    remoteAddr.sockaddr(),
                    remoteAddr.length(),
                    nullptr,
                    nullptr);
                if (SOCKET_ERROR == sendResult)
                {
                    const auto error = WSAGetLastError();
                    if (WSAEMSGSIZE == error)
                    {
                        uint32_t bytesRequested = 0;
                        // iterate across each WSABUF* in the array
                        for (const auto& wsaBuffer : sendRequest)
                        {
                            bytesRequested += wsaBuffer.len;
                        }
                        ctsConfig::PrintErrorInfo(
                            L"WSASendTo(%Iu, seq %lld, %ws) failed with WSAEMSGSIZE : attempted to send datagram of size %u bytes",
                            socket,
                            sequenceNumber,
                            remoteAddr.writeCompleteAddress().c_str(),
                            bytesRequested);
                    }
                    else
                    {
                        ctsConfig::PrintErrorInfo(
                            L"WSASendTo(%Iu, seq %lld, %ws) failed [%d]",
                            socket,
                            sequenceNumber,
                            remoteAddr.writeCompleteAddress().c_str(),
                            error);
                    }
                    return wsIOResult(error);
                }

                // successfully completed synchronously
                returnResults.m_bytesTransferred += bytesSent;
                PRINT_DEBUG_INFO(
                    L"\t\tctsMediaStreamServer sending seq number %lld (%u sent-bytes, %u frame-bytes)\n",
                    sequenceNumber, bytesSent, returnResults.m_bytesTransferred);
            }
        }

        return returnResults;
    }
}
