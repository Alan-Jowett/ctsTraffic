/*

Copyright (c) Microsoft Corporation
All rights reserved.

Licensed under the Apache License, Version 2.0 (the ""License""); you may not use this file except in compliance with the License. You may obtain a copy of the License at http://www.apache.org/licenses/LICENSE-2.0

THIS CODE IS PROVIDED ON AN  *AS IS* BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, EITHER EXPRESS OR IMPLIED, INCLUDING WITHOUT LIMITATION ANY IMPLIED WARRANTIES OR CONDITIONS OF TITLE, FITNESS FOR A PARTICULAR PURPOSE, MERCHANTABLITY OR NON-INFRINGEMENT.

See the Apache Version 2.0 License for specific language governing permissions and limitations under the License.

*/

#pragma once

// cpp headers
#include <array>
#include <memory>
// os headers
#include <Windows.h>
// ctl headers
#include <ctSockaddr.hpp>
// project headers
#include "ctsConfig.h"

namespace ctsTraffic
{
class ctsMediaStreamServerListeningSocketBase
{
protected:
    static constexpr size_t c_recvBufferSize = 1024;

    mutable wil::critical_section m_listeningSocketLock{ctsConfig::ctsConfigSettings::c_CriticalSectionSpinlock};
    const ctl::ctSockaddr m_listeningAddr;

    explicit ctsMediaStreamServerListeningSocketBase(ctl::ctSockaddr listeningAddr) noexcept;
    virtual ~ctsMediaStreamServerListeningSocketBase() noexcept;

    // Called by derived classes when a receive completes successfully.
    // - `sock` is the listening socket where the packet was received.
    // - `buffer` points to the payload buffer and is valid for `bytesReceived` bytes.
    // - `remoteAddr` is the sender's address.
    // - `recvFlags` are the flags returned by WSAGetOverlappedResult / WSARecvFrom.
    void ProcessReceivedPacket(SOCKET sock, const char* buffer, size_t bytesReceived, const ctl::ctSockaddr& remoteAddr, DWORD recvFlags) noexcept;

public:
    // must be provided by derived classes
    virtual SOCKET GetSocket() const noexcept = 0;
    ctl::ctSockaddr GetListeningAddress() const noexcept;
    virtual void InitiateRecv() noexcept = 0;
    virtual void Shutdown() noexcept;

    // non-copyable
    ctsMediaStreamServerListeningSocketBase(const ctsMediaStreamServerListeningSocketBase&) = delete;
    ctsMediaStreamServerListeningSocketBase& operator=(const ctsMediaStreamServerListeningSocketBase&) = delete;
};
} // namespace ctsTraffic
