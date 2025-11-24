/*

Copyright (c) Microsoft Corporation
All rights reserved.

Licensed under the Apache License, Version 2.0 (the ""License""); you may not use this file except in compliance with the License. You may obtain a copy of the License at http://www.apache.org/licenses/LICENSE-2.0

THIS CODE IS PROVIDED ON AN  *AS IS* BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, EITHER EXPRESS OR IMPLIED, INCLUDING WITHOUT LIMITATION ANY IMPLIED WARRANTIES OR CONDITIONS OF TITLE, FITNESS FOR A PARTICULAR PURPOSE, MERCHANTABLITY OR NON-INFRINGEMENT.

See the Apache Version 2.0 License for specific language governing permissions and limitations under the License.

*/

#include <sdkddkver.h>
#include <winsock2.h>
#include <ws2tcpip.h>
#include "CppUnitTest.h"

#include "../../ctl/ctRawIocpShard.hpp"

using namespace Microsoft::VisualStudio::CppUnitTestFramework;
using namespace std;

namespace ctsUnitTest
{
    TEST_CLASS(ctsRawIocpShardUnitTest)
    {
    public:
        TEST_CLASS_INITIALIZE(Setup)
        {
            WSADATA wsa;
            const int startup = WSAStartup(MAKEWORD(2,2), &wsa);
            Assert::AreEqual(0, startup);
        }

        TEST_CLASS_CLEANUP(Cleanup)
        {
            WSACleanup();
        }

        TEST_METHOD(Initialize_StartShutdown)
        {
            ctl::ctRawIocpShard shard(0);
            const bool ok = shard.Initialize(INVALID_SOCKET, 2);
            if (!ok)
            {
                const DWORD gle = GetLastError();
                const int wsa = WSAGetLastError();
                wchar_t wbuf[256];
                swprintf_s(wbuf, L"Initialize failed. GetLastError=%lu WSAGetLastError=%d", gle, wsa);
                Assert::Fail(wbuf);
            }
            Assert::IsTrue(ok);
            Assert::IsTrue(shard.StartWorkers(1));
            // allow a moment for worker thread to start
            Sleep(50);
            shard.Shutdown();
        }

        TEST_METHOD(ReceiveRepost_Smoke)
        {
            // Create a real UDP socket pair bound to localhost and use one to send to the other
            SOCKET s1 = WSASocketW(AF_INET, SOCK_DGRAM, IPPROTO_UDP, nullptr, 0, WSA_FLAG_OVERLAPPED);
            SOCKET s2 = WSASocketW(AF_INET, SOCK_DGRAM, IPPROTO_UDP, nullptr, 0, WSA_FLAG_OVERLAPPED);
            Assert::IsTrue(s1 != INVALID_SOCKET);
            Assert::IsTrue(s2 != INVALID_SOCKET);

            sockaddr_in addr1 = {};
            addr1.sin_family = AF_INET;
            addr1.sin_port = 0; // let system pick
            InetPtonA(AF_INET, "127.0.0.1", &addr1.sin_addr);

            sockaddr_in addr2 = {};
            addr2.sin_family = AF_INET;
            addr2.sin_port = 0;
            InetPtonA(AF_INET, "127.0.0.1", &addr2.sin_addr);

            Assert::AreEqual(0, bind(s1, reinterpret_cast<sockaddr*>(&addr1), sizeof(addr1)));
            Assert::AreEqual(0, bind(s2, reinterpret_cast<sockaddr*>(&addr2), sizeof(addr2)));

            // Query assigned port for s2
            sockaddr_in sa = {};
            int saLen = sizeof(sa);
            getsockname(s2, reinterpret_cast<sockaddr*>(&sa), &saLen);

            ctl::ctRawIocpShard shard(1);
            Assert::IsTrue(shard.Initialize(s2, 2));
            Assert::IsTrue(shard.StartWorkers(1));

            // send a small packet to the shard socket
            sockaddr_in to = sa;
            const char msg[] = "ping";
            const int sendRc = sendto(s1, msg, static_cast<int>(strlen(msg)), 0, reinterpret_cast<sockaddr*>(&to), sizeof(to));
            Assert::IsTrue(sendRc > 0);

            // give worker time to process and repost
            Sleep(200);

            shard.Shutdown();
            closesocket(s1);
            closesocket(s2);
        }
    };
}
