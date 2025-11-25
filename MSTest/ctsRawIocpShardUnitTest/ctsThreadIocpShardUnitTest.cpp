// MSTest-style unit test demonstrating ctThreadIocp_shard with a UDP socket

#include "CppUnitTest.h"

#include <winsock2.h>
#include <ws2tcpip.h>
#include <Windows.h>

#include "../../ctl/ctThreadIocp_shard.hpp"

using namespace Microsoft::VisualStudio::CppUnitTestFramework;

namespace ctsRawIocpShardUnitTest
{
    TEST_CLASS(ctsThreadIocpShardUnitTest)
    {
    public:
        TEST_METHOD(ReceiveUdpPacket)
        {
            WSADATA wsa{};
            Assert::AreEqual(0, WSAStartup(MAKEWORD(2, 2), &wsa));

            SOCKET recvSock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
            Assert::AreNotEqual(INVALID_SOCKET, recvSock);

            sockaddr_in addr{};
            addr.sin_family = AF_INET;
            addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
            addr.sin_port = 0; // ephemeral

            Assert::AreEqual(0, bind(recvSock, reinterpret_cast<sockaddr*>(&addr), static_cast<int>(sizeof addr)));

            // find bound port
            sockaddr_in bound{};
            int len = static_cast<int>(sizeof bound);
            Assert::AreEqual(0, getsockname(recvSock, reinterpret_cast<sockaddr*>(&bound), &len));

            const USHORT port = ntohs(bound.sin_port);

            // create a ctThreadIocp_shard for this socket
            ctl::ctThreadIocp_shard iocp_shard(recvSock, 1);

            // set up receive buffer and WSAOVERLAPPED
            CHAR buf[128] = {};
            WSABUF wsabuf{};
            wsabuf.buf = buf;
            wsabuf.len = static_cast<ULONG>(sizeof buf);

            sockaddr_storage from{};
            int fromlen = static_cast<int>(sizeof from);

            // event to signal callback completion
            const HANDLE doneEvent = CreateEvent(nullptr, TRUE, FALSE, nullptr);
            Assert::IsNotNull(doneEvent);

            WSAOVERLAPPED* ov = reinterpret_cast<WSAOVERLAPPED*>(iocp_shard.new_request([&](OVERLAPPED* _ov)
            {
                (void)_ov;
                Logger::WriteMessage(L"ctThreadIocp_shard callback invoked");
                SetEvent(doneEvent);
            }));

            DWORD flags = 0;
            DWORD bytesRecv = 0;
            int rc = WSARecvFrom(recvSock, &wsabuf, 1, &bytesRecv, &flags, reinterpret_cast<sockaddr*>(&from), &fromlen, ov, nullptr);
            if (rc == SOCKET_ERROR)
            {
                const int err = WSAGetLastError();
                if (err != WSA_IO_PENDING)
                {
                    iocp_shard.cancel_request(ov);
                    closesocket(recvSock);
                    WSACleanup();
                    CloseHandle(doneEvent);
                    Assert::Fail(L"WSARecvFrom failed to start overlapped receive");
                }
            }

            // send a packet to the bound port
            SOCKET sendSock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
            Assert::AreNotEqual(INVALID_SOCKET, sendSock);

            sockaddr_in dest{};
            dest.sin_family = AF_INET;
            dest.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
            dest.sin_port = htons(port);

            const char* msg = "hello";
            const int sent = sendto(sendSock, msg, static_cast<int>(strlen(msg)), 0, reinterpret_cast<sockaddr*>(&dest), static_cast<int>(sizeof dest));
            Assert::AreNotEqual(SOCKET_ERROR, sent);

            // wait for callback (2s timeout)
            const DWORD wait = WaitForSingleObject(doneEvent, 2000);
            Assert::AreEqual(WAIT_OBJECT_0, wait);

            // cleanup
            closesocket(sendSock);
            closesocket(recvSock);
            WSACleanup();
            CloseHandle(doneEvent);
        }

        TEST_METHOD(CancelRequestDoesNotInvokeCallback)
        {
            WSADATA wsa{};
            Assert::AreEqual(0, WSAStartup(MAKEWORD(2, 2), &wsa));

            SOCKET recvSock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
            Assert::AreNotEqual(INVALID_SOCKET, recvSock);

            sockaddr_in addr{};
            addr.sin_family = AF_INET;
            addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
            addr.sin_port = 0; // ephemeral

            Assert::AreEqual(0, bind(recvSock, reinterpret_cast<sockaddr*>(&addr), static_cast<int>(sizeof addr)));

            // create a ctThreadIocp_shard for this socket
            ctl::ctThreadIocp_shard iocp_shard(recvSock, 1);

            // set up receive buffer and WSAOVERLAPPED
            CHAR buf[128] = {};
            WSABUF wsabuf{};
            wsabuf.buf = buf;
            wsabuf.len = static_cast<ULONG>(sizeof buf);

            sockaddr_storage from{};
            int fromlen = static_cast<int>(sizeof from);

            // event to signal callback completion (should NOT be signaled)
            const HANDLE doneEvent = CreateEvent(nullptr, TRUE, FALSE, nullptr);
            Assert::IsNotNull(doneEvent);

            bool callbackInvoked = false;
            WSAOVERLAPPED* ov = reinterpret_cast<WSAOVERLAPPED*>(iocp_shard.new_request([&](OVERLAPPED* _ov)
            {
                (void)_ov;
                callbackInvoked = true;
                SetEvent(doneEvent);
            }));

            // Close the socket to force WSARecvFrom to fail immediately
            closesocket(recvSock);

            DWORD flags = 0;
            DWORD bytesRecv = 0;
            int rc = WSARecvFrom(recvSock, &wsabuf, 1, &bytesRecv, &flags, reinterpret_cast<sockaddr*>(&from), &fromlen, ov, nullptr);
            if (rc == SOCKET_ERROR)
            {
                const int err = WSAGetLastError();
                // Expected: error and not WSA_IO_PENDING
                if (err == WSA_IO_PENDING)
                {
                    // Unexpected; cancel the request
                    iocp_shard.cancel_request(ov);
                }
                else
                {
                    // Must call cancel_request for the failed immediate call
                    iocp_shard.cancel_request(ov);
                }
            }

            // wait briefly to make sure callback is not called
            const DWORD wait = WaitForSingleObject(doneEvent, 500);
            Assert::AreEqual(static_cast<DWORD>(WAIT_TIMEOUT), wait);
            Assert::IsFalse(callbackInvoked);

            WSACleanup();
            CloseHandle(doneEvent);
        }
    };
}
