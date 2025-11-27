/*
  MediaUpload IoPattern implementations
*/

// cpp headers
#include <algorithm>
#include <vector>
// os headers
#include <Windows.h>
// ctl headers
#include <ctTimer.hpp>
// project headers
#include "ctsIOPattern.h"
#include "ctsStatistics.hpp"
#include "ctsConfig.h"
#include "ctsIOTask.hpp"
#include "ctsMediaStreamProtocol.hpp"
// wil headers always included last
#include <wil/stl.h>
#include <wil/resource.h>

using namespace ctl;
using std::vector;

namespace ctsTraffic
{
// MediaUpload Server: behaves like MediaStreamClient (it receives uploads from clients)
ctsIoPatternMediaUploadServer::ctsIoPatternMediaUploadServer() noexcept :
    ctsIoPatternStatistics(ctsConfig::g_configSettings->PrePostRecvs),
    m_frameSizeBytes(ctsConfig::GetMediaStream().FrameSizeBytes),
    m_frameRateFps(ctsConfig::GetMediaStream().FramesPerSecond)
{
    PRINT_DEBUG_INFO(L"\t\tctsIoPatternMediaUploadServer - frame rate in milliseconds per frame : %lld\n", static_cast<int64_t>(1000UL / m_frameRateFps));
}

ctsTask ctsIoPatternMediaUploadServer::GetNextTaskFromPattern() noexcept
{
    ctsTask returnTask;
    if (m_recvNeeded > 0)
    {
        --m_recvNeeded;
        returnTask = CreateTrackedTask(ctsTaskAction::Recv, m_frameSizeBytes);
    }
    return returnTask;
}

ctsIoPatternError ctsIoPatternMediaUploadServer::CompleteTaskBackToPattern(const ctsTask& task, uint32_t currentTransfer) noexcept
{
    if (task.m_bufferType != ctsTask::BufferType::UdpConnectionId)
    {
        const int64_t currentTransferBits = static_cast<int64_t>(currentTransfer) * 8LL;

        ctsConfig::g_configSettings->UdpStatusDetails.m_bitsReceived.Add(currentTransferBits);
        m_statistics.m_bitsReceived.Add(currentTransferBits);

        m_currentFrameCompleted += currentTransfer;
        if (m_currentFrameCompleted == m_frameSizeBytes)
        {
            ++m_currentFrame;
            m_currentFrameRequested = 0UL;
            m_currentFrameCompleted = 0UL;
        }
    }
    return ctsIoPatternError::NoError;
}

// MediaUpload Client: behaves like MediaStreamServer (it sends uploads to server)
ctsIoPatternMediaUploadClient::ctsIoPatternMediaUploadClient() :
    ctsIoPatternStatistics(1), // the pattern will use the recv writeable-buffer for sending a connection ID
    m_frameSizeBytes(ctsConfig::GetMediaStream().FrameSizeBytes),
    m_frameRateFps(ctsConfig::GetMediaStream().FramesPerSecond)
{
    PRINT_DEBUG_INFO(L"\t\tctsIoPatternMediaUploadClient - frame rate in milliseconds per frame : %lld\n", static_cast<int64_t>(1000UL / m_frameRateFps));
}

ctsIoPatternMediaUploadClient::~ctsIoPatternMediaUploadClient() noexcept
{
}

ctsTask ctsIoPatternMediaUploadClient::GetNextTaskFromPattern() noexcept
{
    ctsTask returnTask;
    switch (m_state)
    {
    case ServerState::NotStarted:
        // send a connection id by using a writeable buffer from the base (Recv) then turn into Send
        returnTask = ctsMediaStreamMessage::MakeConnectionIdTask(
            CreateUntrackedTask(ctsTaskAction::Recv, c_udpDatagramConnectionIdHeaderLength),
            GetConnectionIdentifier());
        m_state = ServerState::IdSent;
        break;

    case ServerState::IdSent:
        m_baseTimeMilliseconds = ctTimer::snap_qpc_as_msec();
        m_state = ServerState::IoStarted;
        [[fallthrough]];
    case ServerState::IoStarted:
        if (m_currentFrameRequested < m_frameSizeBytes)
        {
            returnTask = CreateTrackedTask(ctsTaskAction::Send, m_frameSizeBytes);
            // calculate the future time to initiate the IO
            returnTask.m_timeOffsetMilliseconds =
                m_baseTimeMilliseconds
                + (m_currentFrame * 1000LL / m_frameRateFps)
                - ctTimer::snap_qpc_as_msec();

            m_currentFrameRequested += returnTask.m_bufferLength;
        }
        break;
    }
    return returnTask;
}

ctsIoPatternError ctsIoPatternMediaUploadClient::CompleteTaskBackToPattern(const ctsTask& task, uint32_t currentTransfer) noexcept
{
    if (task.m_bufferType != ctsTask::BufferType::UdpConnectionId)
    {
        const int64_t currentTransferBits = static_cast<int64_t>(currentTransfer) * 8LL;

        ctsConfig::g_configSettings->UdpStatusDetails.m_bitsReceived.Add(currentTransferBits);
        m_statistics.m_bitsReceived.Add(currentTransferBits);

        m_currentFrameCompleted += currentTransfer;
        if (m_currentFrameCompleted == m_frameSizeBytes)
        {
            ++m_currentFrame;
            m_currentFrameRequested = 0UL;
            m_currentFrameCompleted = 0UL;
        }
    }
    return ctsIoPatternError::NoError;
}

} // namespace ctsTraffic
