/*
Copyright (c) Microsoft Corporation
All rights reserved.

Licensed under the Apache License, Version 2.0 (the "License"); you may not use this file except in compliance with the License. You may obtain a copy of the License at http://www.apache.org/licenses/LICENSE-2.0

THIS CODE IS PROVIDED ON AN  *AS IS* BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, EITHER EXPRESS OR IMPLIED, INCLUDING WITHOUT LIMITATION ANY IMPLIED WARRANTIES OR CONDITIONS OF TITLE, FITNESS FOR A PARTICULAR PURPOSE, MERCHANTABLITY OR NON-INFRINGEMENT.

See the Apache Version 2.0 License for specific language governing permissions and limitations under the License.
*/

#pragma once

// Minimal header exposing the server-side send implementation.

#include <WinSock2.h>
#include "ctsMediaStreamServerConnectedSocket.h"
#include "ctsWinsockLayer.h"

namespace ctsTraffic
{
    // Implements the send-side behavior for a connected media-stream socket.
    // This function is a drop-in implementation that mirrors the existing
    // `ConnectedSocketIo` logic and is intended to be used by refactor work
    // to move send responsibilities into a separate translation unit.
    wsIOResult ConnectedSocketSendImpl(_In_ ctsMediaStreamServerConnectedSocket* connectedSocket) noexcept;
}
