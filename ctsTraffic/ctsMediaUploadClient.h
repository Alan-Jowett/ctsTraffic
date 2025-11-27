/*
Header for upload client
*/
#pragma once

// cpp headers
#include <memory>
// local headers
#include "ctsSocket.h"

namespace ctsTraffic
{
// The function that is registered to run Winsock IO using IO Completion Ports with the specified ctsSocket
void ctsMediaUploadClient(const std::weak_ptr<ctsSocket>& weakSocket) noexcept;

// The function that is registered to 'connect' to the target server by sending a START command using IO Completion Ports
void ctsMediaUploadClientConnect(const std::weak_ptr<ctsSocket>& weakSocket) noexcept;
} // namespace
