/*

Copyright (c) Microsoft Corporation
All rights reserved.

Licensed under the Apache License, Version 2.0 (the ""License""); you may not use this file except in compliance with the License. You may obtain a copy of the License at http://www.apache.org/licenses/LICENSE-2.0

THIS CODE IS PROVIDED ON AN  *AS IS* BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, EITHER EXPRESS OR IMPLIED, INCLUDING WITHOUT LIMITATION ANY IMPLIED WARRANTIES OR CONDITIONS OF TITLE, FITNESS FOR A PARTICULAR PURPOSE, MERCHANTABLITY OR NON-INFRINGEMENT.

See the Apache Version 2.0 License for specific language governing permissions and limitations under the License.

*/

#pragma once

/**
 * @file ctSocketExtensions.hpp
 * @brief Thin wrappers for optional Winsock extension functions (AcceptEx,
 *        ConnectEx, RIO, TransmitFile, etc.) with runtime detection.
 */

// os headers
#include <Windows.h>
#include <WinSock2.h>
#include <MSWSock.h>
// wil headers
// ReSharper disable once CppUnusedIncludeDirective
#include <wil/stl.h>
#include <wil/resource.h>


namespace ctl { namespace Details
    {
        constexpr uint32_t c_functionPtrCount = 9;
        static LPFN_TRANSMITFILE g_transmitfile = nullptr; // WSAID_TRANSMITFILE
        static LPFN_ACCEPTEX g_acceptex = nullptr; // WSAID_ACCEPTEX
        static LPFN_GETACCEPTEXSOCKADDRS g_getacceptexsockaddrs = nullptr; // WSAID_GETACCEPTEXSOCKADDRS
        static LPFN_TRANSMITPACKETS g_transmitpackets = nullptr; // WSAID_TRANSMITPACKETS
        static LPFN_CONNECTEX g_connectex = nullptr; // WSAID_CONNECTEX
        static LPFN_DISCONNECTEX g_disconnectex = nullptr; // WSAID_DISCONNECTEX
        static LPFN_WSARECVMSG g_wsarecvmsg = nullptr; // WSAID_WSARECVMSG
        static LPFN_WSASENDMSG g_wsasendmsg = nullptr; // WSAID_WSASENDMSG
        static RIO_EXTENSION_FUNCTION_TABLE g_rioextensionfunctiontable; // WSAID_MULTIPLE_RIO

        //
        // ctSocketExtensionInit
        //
        // InitOnce function only to be called locally to ensure WSAStartup is held
        // for the function pointers to remain accurate
        //
        /**
         * @brief One-time initializer that queries and caches optional Winsock
         *        extension function pointers. This assumes WSAStartup has been
         *        called on the current process.
         * @param initOnce [in] InitOnce token (unused).
         * @param parameter [in] User parameter (unused).
         * @param context [out] Out context (unused).
         * @returns TRUE on success, FALSE on failure.
         */
        static BOOL CALLBACK SocketExtensionInitFn(_In_ PINIT_ONCE, _In_ PVOID, _In_ PVOID*) noexcept
        {
            WSADATA wsadata;
            if (WSAStartup(WINSOCK_VERSION, &wsadata) != 0)
            {
                return FALSE;
            }
            auto wsaCleanupOnExit = wil::scope_exit([&]() noexcept { WSACleanup(); });

            // check to see if we need to create a temp socket
            const wil::unique_socket localSocket{socket(AF_INET6, SOCK_DGRAM, IPPROTO_UDP)};
            if (INVALID_SOCKET == localSocket.get())
            {
                return FALSE;
            }
            // control code and the size to fetch the extension function pointers
            for (auto fnLoop = 0ul; fnLoop < c_functionPtrCount; ++fnLoop)
            {
                VOID* functionPtr{nullptr};
                DWORD controlCode{SIO_GET_EXTENSION_FUNCTION_POINTER};
                DWORD bytes{sizeof(VOID*)};
                // must declare GUID explicitly at a global scope as some commonly used test libraries
                // - incorrectly pull it into their own namespace
                GUID guid{};

                switch (fnLoop)
                {
                    case 0:
                    {
                        functionPtr = &g_transmitfile;
                        constexpr GUID tmpGuid = WSAID_TRANSMITFILE;
                        memcpy(&guid, &tmpGuid, sizeof GUID);
                        break;
                    }
                    case 1:
                    {
                        functionPtr = &g_acceptex;
                        constexpr GUID tmpGuid = WSAID_ACCEPTEX;
                        memcpy(&guid, &tmpGuid, sizeof GUID);
                        break;
                    }
                    case 2:
                    {
                        functionPtr = &g_getacceptexsockaddrs;
                        constexpr GUID tmpGuid = WSAID_GETACCEPTEXSOCKADDRS;
                        memcpy(&guid, &tmpGuid, sizeof GUID);
                        break;
                    }
                    case 3:
                    {
                        functionPtr = &g_transmitpackets;
                        constexpr GUID tmpGuid = WSAID_TRANSMITPACKETS;
                        memcpy(&guid, &tmpGuid, sizeof GUID);
                        break;
                    }
                    case 4:
                    {
                        functionPtr = &g_connectex;
                        constexpr GUID tmpGuid = WSAID_CONNECTEX;
                        memcpy(&guid, &tmpGuid, sizeof GUID);
                        break;
                    }
                    case 5:
                    {
                        functionPtr = &g_disconnectex;
                        constexpr GUID tmpGuid = WSAID_DISCONNECTEX;
                        memcpy(&guid, &tmpGuid, sizeof GUID);
                        break;
                    }
                    case 6:
                    {
                        functionPtr = &g_wsarecvmsg;
                        constexpr GUID tmpGuid = WSAID_WSARECVMSG;
                        memcpy(&guid, &tmpGuid, sizeof GUID);
                        break;
                    }
                    case 7:
                    {
                        functionPtr = &g_wsasendmsg;
                        constexpr GUID tmpGuid = WSAID_WSASENDMSG;
                        memcpy(&guid, &tmpGuid, sizeof GUID);
                        break;
                    }
                    case 8:
                    {
                        functionPtr = &g_rioextensionfunctiontable;
                        constexpr GUID tmpGuid = WSAID_MULTIPLE_RIO;
                        memcpy(&guid, &tmpGuid, sizeof GUID);
                        controlCode = SIO_GET_MULTIPLE_EXTENSION_FUNCTION_POINTER;
                        bytes = {sizeof g_rioextensionfunctiontable};
                        ::ZeroMemory(&g_rioextensionfunctiontable, bytes);
                        g_rioextensionfunctiontable.cbSize = bytes;
                        break;
                    }
                    default:
                        FAIL_FAST_MSG("Unknown ctSocketExtension function number");
                }

                if (0 != WSAIoctl(
                        localSocket.get(),
                        controlCode,
                        &guid,
                        DWORD{sizeof guid},
                        functionPtr,
                        bytes,
                        &bytes,
                        nullptr,
                        nullptr))
                {
                    if (WSAGetLastError() == WSAEOPNOTSUPP && 8 == fnLoop)
                    {
                        // ignore not-supported errors for RIO APIs to support Win7
                    }
                    else
                    {
                        return FALSE;
                    }
                }
            }

            return TRUE;
        }

        /**
         * @brief Ensure the socket extension function pointers are initialized.
         *        Safe to call multiple times from different threads.
         */
        static void InitSocketExtensions() noexcept
        {
            // ReSharper disable once CppZeroConstantCanBeReplacedWithNullptr
            static INIT_ONCE socketExtensionInitOnce = INIT_ONCE_STATIC_INIT;
            FAIL_FAST_IF(!::InitOnceExecuteOnce(&socketExtensionInitOnce, SocketExtensionInitFn, nullptr, nullptr));
        }
    }

    //
    // Dynamic check if RIO is available on this operating system
    //
    /**
     * @brief Query whether RIO (Registered I/O) is available on this OS.
     * @returns true when RIO APIs are present and cached, false otherwise.
     */
    inline bool ctSocketIsRioAvailable() noexcept
    {
        Details::InitSocketExtensions();
        return nullptr != Details::g_rioextensionfunctiontable.RIOReceive;
    }

    //
    // TransmitFile
    //
    /**
     * @brief Wrapper for TransmitFile extension function.
     * @param hSocket [in] Socket to transmit on.
     * @param hFile [in] File handle to read data from.
     * @param nNumberOfBytesToWrite [in] Bytes to write (0 -> until EOF).
     * @param nNumberOfBytesPerSend [in] Bytes per send call.
     * @param lpOverlapped [in,out,opt] Overlapped structure for async IO.
     * @param lpTransmitBuffers [in,opt] Optional header/trailer buffers.
     * @param dwReserved [in] Reserved parameter (pass 0).
     */
    inline BOOL ctTransmitFile(
        _In_ SOCKET hSocket,
        _In_ HANDLE hFile,
        _In_ DWORD nNumberOfBytesToWrite,
        _In_ DWORD nNumberOfBytesPerSend,
        _Inout_opt_ LPOVERLAPPED lpOverlapped,
        _In_opt_ LPTRANSMIT_FILE_BUFFERS lpTransmitBuffers,
        _In_ DWORD dwReserved) noexcept
    {
        Details::InitSocketExtensions();
        return Details::g_transmitfile(
            hSocket,
            hFile,
            nNumberOfBytesToWrite,
            nNumberOfBytesPerSend,
            lpOverlapped,
            lpTransmitBuffers,
            dwReserved);
    }

    //
    // TransmitPackets
    //
    /**
     * @brief Wrapper for TransmitPackets extension function.
     * @param hSocket [in] Socket to send packets on.
     * @param lpPacketArray [in,opt] Array of packet descriptors.
     * @param nElementCount [in] Number of elements in the array.
     * @param nSendSize [in] Aggregate send size.
     * @param lpOverlapped [in,out,opt] Overlapped for async IO.
     * @param dwFlags [in] Flags for transmit behaviour.
     */
    inline BOOL ctTransmitPackets(
        _In_ SOCKET hSocket,
        _In_opt_ LPTRANSMIT_PACKETS_ELEMENT lpPacketArray,
        _In_ DWORD nElementCount,
        _In_ DWORD nSendSize,
        _Inout_opt_ LPOVERLAPPED lpOverlapped,
        _In_ DWORD dwFlags) noexcept
    {
        Details::InitSocketExtensions();
        return Details::g_transmitpackets(
            hSocket,
            lpPacketArray,
            nElementCount,
            nSendSize,
            lpOverlapped,
            dwFlags
        );
    }

    //
    // AcceptEx
    //
    /**
     * @brief Wrapper for AcceptEx. Mirrors the native signature and semantics.
     * @param sListenSocket [in] Listening socket.
     * @param sAcceptSocket [in] Accept socket returned to the caller.
     * @param lpOutputBuffer [out] Buffer to receive address and initial data.
     * @param dwReceiveDataLength [in] Number of bytes to receive from remote.
     * @param dwLocalAddressLength [in] Size reserved for local sockaddr.
     * @param dwRemoteAddressLength [in] Size reserved for remote sockaddr.
     * @param lpdwBytesReceived [out] Receives number of bytes received.
     * @param lpOverlapped [in,out] Overlapped for async completion.
     */
    inline BOOL ctAcceptEx(
        _In_ SOCKET sListenSocket,
        _In_ SOCKET sAcceptSocket,
        _Out_writes_bytes_(dwReceiveDataLength + dwLocalAddressLength + dwRemoteAddressLength) PVOID lpOutputBuffer,
        _In_ DWORD dwReceiveDataLength,
        _In_ DWORD dwLocalAddressLength,
        _In_ DWORD dwRemoteAddressLength,
        _Out_ LPDWORD lpdwBytesReceived,
        _Inout_ LPOVERLAPPED lpOverlapped) noexcept
    {
        Details::InitSocketExtensions();
        return Details::g_acceptex(
            sListenSocket,
            sAcceptSocket,
            lpOutputBuffer,
            dwReceiveDataLength,
            dwLocalAddressLength,
            dwRemoteAddressLength,
            lpdwBytesReceived,
            lpOverlapped
        );
    }

    //
    // GetAcceptExSockaddrs
    //
    /**
     * @brief Wrapper for GetAcceptExSockaddrs which parses addresses from
     *        an AcceptEx output buffer.
     * @param lpOutputBuffer [in] Buffer returned by AcceptEx.
     * @param dwReceiveDataLength [in] See AcceptEx.
     * @param dwLocalAddressLength [in] See AcceptEx.
     * @param dwRemoteAddressLength [in] See AcceptEx.
     * @param localSockaddr [out] Receives pointer to local sockaddr in buffer.
     * @param LocalSockaddrLength [out] Receives length of local sockaddr.
     * @param remoteSockaddr [out] Receives pointer to remote sockaddr in buffer.
     * @param RemoteSockaddrLength [out] Receives length of remote sockaddr.
     */
    inline VOID ctGetAcceptExSockaddrs(
        _In_reads_bytes_(dwReceiveDataLength + dwLocalAddressLength + dwRemoteAddressLength) PVOID lpOutputBuffer,
        _In_ DWORD dwReceiveDataLength,
        _In_ DWORD dwLocalAddressLength,
        _In_ DWORD dwRemoteAddressLength,
        _Outptr_result_bytebuffer_(*LocalSockaddrLength) sockaddr** localSockaddr,
        _Out_ LPINT LocalSockaddrLength,
        _Outptr_result_bytebuffer_(*RemoteSockaddrLength) sockaddr** remoteSockaddr,
        _Out_ LPINT RemoteSockaddrLength) noexcept
    {
        Details::InitSocketExtensions();
        return Details::g_getacceptexsockaddrs(
            lpOutputBuffer,
            dwReceiveDataLength,
            dwLocalAddressLength,
            dwRemoteAddressLength,
            localSockaddr,
            LocalSockaddrLength,
            remoteSockaddr,
            RemoteSockaddrLength
        );
    }

    //
    // ConnectEx
    //
    /**
     * @brief Wrapper for ConnectEx extension function.
     * @param s [in] Socket on which to establish a connection.
     * @param name [in] Remote sockaddr to connect to.
     * @param namelen [in] Length of the sockaddr provided.
     * @param lpSendBuffer [in,opt] Optional data buffer to send as part of connect.
     * @param dwSendDataLength [in] Size of the send buffer.
     * @param lpdwBytesSent [out,opt] Receives number of bytes sent when buffer supplied.
     * @param lpOverlapped [in,out] Overlapped for async completion.
     */
    inline BOOL ctConnectEx(
        _In_ SOCKET s,
        _In_reads_bytes_(namelen) const sockaddr* name,
        _In_ int namelen,
        _In_reads_bytes_opt_(dwSendDataLength) PVOID lpSendBuffer,
        _In_ DWORD dwSendDataLength,
        _When_(lpSendBuffer, _Out_) LPDWORD lpdwBytesSent, // optional if lpSendBuffer is null
        _Inout_ LPOVERLAPPED lpOverlapped) noexcept
    {
        Details::InitSocketExtensions();
        return Details::g_connectex(
            s,
            name,
            namelen,
            lpSendBuffer,
            dwSendDataLength,
            lpdwBytesSent,
            lpOverlapped
        );
    }

    //
    // DisconnectEx
    //
    /**
     * @brief Wrapper for DisconnectEx.
     * @param s [in] Socket to disconnect.
     * @param lpOverlapped [in,out,opt] Optional overlapped for async completion.
     * @param dwFlags [in] Flags controlling graceful/abortive behaviour.
     * @param dwReserved [in] Reserved parameter (pass 0).
     */
    inline BOOL ctDisconnectEx(
        _In_ SOCKET s,
        _Inout_opt_ LPOVERLAPPED lpOverlapped,
        _In_ DWORD dwFlags,
        _In_ DWORD dwReserved) noexcept
    {
        Details::InitSocketExtensions();
        return Details::g_disconnectex(
            s,
            lpOverlapped,
            dwFlags,
            dwReserved
        );
    }

    //
    // WSARecvMsg
    //
    /**
     * @brief Wrapper for WSARecvMsg. Receives a message and ancillary data.
     * @param s [in] Socket to receive from.
     * @param lpMsg [in,out] WSAMSG describing buffers and control data.
     * @param lpdwNumberOfBytesRecvd [out,opt] Receives number of bytes read.
     * @param lpOverlapped [in,out,opt] Overlapped for async completion.
     * @param lpCompletionRoutine [in,opt] Optional completion routine.
     */
    inline INT ctWSARecvMsg(
        _In_ SOCKET s,
        _Inout_ LPWSAMSG lpMsg,
        _Out_opt_ LPDWORD lpdwNumberOfBytesRecvd,
        _Inout_opt_ LPWSAOVERLAPPED lpOverlapped,
        _In_opt_ LPWSAOVERLAPPED_COMPLETION_ROUTINE lpCompletionRoutine) noexcept
    {
        Details::InitSocketExtensions();
        return Details::g_wsarecvmsg(
            s,
            lpMsg,
            lpdwNumberOfBytesRecvd,
            lpOverlapped,
            lpCompletionRoutine
        );
    }

    //
    // WSASendMsg
    //
    /**
     * @brief Wrapper for WSASendMsg.
     * @param s [in] Socket to send from.
     * @param lpMsg [in] WSAMSG describing buffers and control data.
     * @param dwFlags [in] Flags for send behaviour.
     * @param lpNumberOfBytesSent [out,opt] Receives number of bytes sent.
     * @param lpOverlapped [in,out,opt] Overlapped for async completion.
     * @param lpCompletionRoutine [in,opt] Optional completion routine.
     */
    inline INT ctWSASendMsg(
        _In_ SOCKET s,
        _In_ LPWSAMSG lpMsg,
        _In_ DWORD dwFlags,
        _Out_opt_ LPDWORD lpNumberOfBytesSent,
        _Inout_opt_ LPWSAOVERLAPPED lpOverlapped,
        _In_opt_ LPWSAOVERLAPPED_COMPLETION_ROUTINE lpCompletionRoutine) noexcept
    {
        Details::InitSocketExtensions();
        return Details::g_wsasendmsg(
            s,
            lpMsg,
            dwFlags,
            lpNumberOfBytesSent,
            lpOverlapped,
            lpCompletionRoutine
        );
    }

    //
    // RioReceive 
    //
    inline BOOL ctRIOReceive(
        _In_ RIO_RQ socketQueue,
        _In_reads_(dataBufferCount) PRIO_BUF pData,
        _In_ ULONG dataBufferCount,
        _In_ DWORD dwFlags,
        _In_ PVOID requestContext) noexcept
    {
        Details::InitSocketExtensions();
        return Details::g_rioextensionfunctiontable.RIOReceive(
            socketQueue,
            pData,
            dataBufferCount,
            dwFlags,
            requestContext
        );
    }

    //
    // RioReceiveEx 
    //
    inline int ctRIOReceiveEx(
        _In_ RIO_RQ socketQueue,
        _In_reads_(dataBufferCount) PRIO_BUF pData,
        _In_ ULONG dataBufferCount,
        _In_opt_ PRIO_BUF pLocalAddress,
        _In_opt_ PRIO_BUF pRemoteAddress,
        _In_opt_ PRIO_BUF pControlContext,
        _In_opt_ PRIO_BUF pdwFlags,
        _In_ DWORD dwFlags,
        _In_ PVOID requestContext) noexcept
    {
        Details::InitSocketExtensions();
        return Details::g_rioextensionfunctiontable.RIOReceiveEx(
            socketQueue,
            pData,
            dataBufferCount,
            pLocalAddress,
            pRemoteAddress,
            pControlContext,
            pdwFlags,
            dwFlags,
            requestContext
        );
    }

    //
    // RioSend
    //
    inline BOOL ctRIOSend(
        _In_ RIO_RQ socketQueue,
        _In_reads_(dataBufferCount) PRIO_BUF pData,
        _In_ ULONG dataBufferCount,
        _In_ DWORD dwFlags,
        _In_ PVOID requestContext) noexcept
    {
        Details::InitSocketExtensions();
        return Details::g_rioextensionfunctiontable.RIOSend(
            socketQueue,
            pData,
            dataBufferCount,
            dwFlags,
            requestContext
        );
    }

    //
    // RioSendEx
    //
    inline BOOL ctRIOSendEx(
        _In_ RIO_RQ socketQueue,
        _In_reads_(dataBufferCount) PRIO_BUF pData,
        _In_ ULONG dataBufferCount,
        _In_opt_ PRIO_BUF pLocalAddress,
        _In_opt_ PRIO_BUF pRemoteAddress,
        _In_opt_ PRIO_BUF pControlContext,
        _In_opt_ PRIO_BUF pdwFlags,
        _In_ DWORD dwFlags,
        _In_ PVOID requestContext) noexcept
    {
        Details::InitSocketExtensions();
        return Details::g_rioextensionfunctiontable.RIOSendEx(
            socketQueue,
            pData,
            dataBufferCount,
            pLocalAddress,
            pRemoteAddress,
            pControlContext,
            pdwFlags,
            dwFlags,
            requestContext
        );
    }

    //
    // RioCloseCompletionQueue
    //
    inline void ctRIOCloseCompletionQueue(
        _In_ RIO_CQ cq) noexcept
    {
        Details::InitSocketExtensions();
        return Details::g_rioextensionfunctiontable.RIOCloseCompletionQueue(
            cq
        );
    }

    //
    // RioCreateCompletionQueue
    //
    inline RIO_CQ ctRIOCreateCompletionQueue(
        _In_ DWORD queueSize,
        _In_opt_ PRIO_NOTIFICATION_COMPLETION pNotificationCompletion) noexcept
    {
        Details::InitSocketExtensions();
        return Details::g_rioextensionfunctiontable.RIOCreateCompletionQueue(
            queueSize,
            pNotificationCompletion
        );
    }

    //
    // RioCreateRequestQueue
    //
    inline RIO_RQ ctRIOCreateRequestQueue(
        _In_ SOCKET socket,
        _In_ ULONG maxOutstandingReceive,
        _In_ ULONG maxReceiveDataBuffers,
        _In_ ULONG maxOutstandingSend,
        _In_ ULONG maxSendDataBuffers,
        _In_ RIO_CQ receiveCq,
        _In_ RIO_CQ sendCq,
        _In_ PVOID socketContext) noexcept
    {
        Details::InitSocketExtensions();
        return Details::g_rioextensionfunctiontable.RIOCreateRequestQueue(
            socket,
            maxOutstandingReceive,
            maxReceiveDataBuffers,
            maxOutstandingSend,
            maxSendDataBuffers,
            receiveCq,
            sendCq,
            socketContext
        );
    }

    //
    // RioDequeueCompletion
    //
    inline ULONG ctRIODequeueCompletion(
        _In_ RIO_CQ cq,
        _Out_writes_to_(arraySize, return) PRIORESULT array,
        _In_ ULONG arraySize) noexcept
    {
        Details::InitSocketExtensions();
        return Details::g_rioextensionfunctiontable.RIODequeueCompletion(
            cq,
            array,
            arraySize
        );
    }

    //
    // RioDeregisterBuffer
    //
    inline void ctRIODeregisterBuffer(
        _In_ RIO_BUFFERID bufferId) noexcept
    {
        Details::InitSocketExtensions();
        return Details::g_rioextensionfunctiontable.RIODeregisterBuffer(
            bufferId
        );
    }

    //
    // RioNotify
    //
    inline int ctRIONotify(
        _In_ RIO_CQ cq) noexcept
    {
        Details::InitSocketExtensions();
        return Details::g_rioextensionfunctiontable.RIONotify(
            cq
        );
    }

    //
    // RioRegisterBuffer 
    //
    inline RIO_BUFFERID ctRIORegisterBuffer(
        _In_ PCHAR dataBuffer,
        _In_ DWORD dataLength) noexcept
    {
        Details::InitSocketExtensions();
        return Details::g_rioextensionfunctiontable.RIORegisterBuffer(
            dataBuffer,
            dataLength
        );
    }

    //
    // RioResizeCompletionQueue
    //
    inline BOOL ctRIOResizeCompletionQueue(
        _In_ RIO_CQ cq,
        _In_ DWORD queueSize) noexcept
    {
        Details::InitSocketExtensions();
        return Details::g_rioextensionfunctiontable.RIOResizeCompletionQueue(
            cq,
            queueSize
        );
    }

    //
    // RioResizeRequestQueue
    //
    inline BOOL ctRIOResizeRequestQueue(
        _In_ RIO_RQ rq,
        _In_ DWORD maxOutstandingReceive,
        _In_ DWORD maxOutstandingSend) noexcept
    {
        Details::InitSocketExtensions();
        return Details::g_rioextensionfunctiontable.RIOResizeRequestQueue(
            rq,
            maxOutstandingReceive,
            maxOutstandingSend
        );
    }
} // namespace ctl
