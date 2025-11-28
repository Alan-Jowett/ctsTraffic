#include "ctsMediaStreamProtocol.hpp"
#include <cstring>

using namespace ctsTraffic;

ctsTask ctsMediaStreamMessage::MakeSynTask(const ctsTask& rawTask, const char* const desiredConnectionId) noexcept
{
    ctsTask returnTask{ rawTask };
    if (returnTask.m_bufferLength < c_udpDatagramDataHeaderLength + c_udpDatagramControlFixedBodyLength)
    {
        returnTask.m_buffer = const_cast<char*>(g_udpDatagramStartString);
        returnTask.m_bufferLength = c_udpDatagramStartStringLength;
        returnTask.m_bufferType = ctsTask::BufferType::Static;
        return returnTask;
    }

    memcpy_s(returnTask.m_buffer, c_udpDatagramProtocolHeaderFlagLength, &c_udpDatagramFlagSyn, c_udpDatagramProtocolHeaderFlagLength);
    const uint64_t zero64 = 0;
    memcpy_s(returnTask.m_buffer + c_udpDatagramProtocolHeaderFlagLength, c_udpDatagramSequenceNumberLength, &zero64, c_udpDatagramSequenceNumberLength);
    memcpy_s(returnTask.m_buffer + c_udpDatagramProtocolHeaderFlagLength + c_udpDatagramSequenceNumberLength, c_udpDatagramQpcLength + c_udpDatagramQpfLength, &zero64, c_udpDatagramQpcLength + c_udpDatagramQpfLength);

    const auto bodyOffset = c_udpDatagramDataHeaderLength;
    returnTask.m_buffer[bodyOffset + 0] = 1;
    returnTask.m_buffer[bodyOffset + 1] = 0;
    returnTask.m_buffer[bodyOffset + 2] = 0;
    returnTask.m_buffer[bodyOffset + 3] = 0;

    if (desiredConnectionId)
    {
        memcpy_s(returnTask.m_buffer + bodyOffset + 4, c_udpDatagramControlConnectionIdLength, desiredConnectionId, c_udpDatagramControlConnectionIdLength);
    }
    else
    {
        memset(returnTask.m_buffer + bodyOffset + 4, 0, c_udpDatagramControlConnectionIdLength);
    }

    returnTask.m_ioAction = ctsTaskAction::Send;
    returnTask.m_bufferType = ctsTask::BufferType::Dynamic;
    returnTask.m_trackIo = false;
    return returnTask;
}

ctsTask ctsMediaStreamMessage::MakeSynAckTask(const ctsTask& rawTask, bool accept, const char* const assignedConnectionId) noexcept
{
    ctsTask returnTask{ rawTask };
    if (returnTask.m_bufferLength < c_udpDatagramDataHeaderLength + c_udpDatagramControlFixedBodyLength)
    {
        returnTask.m_buffer = const_cast<char*>(g_udpDatagramStartString);
        returnTask.m_bufferLength = c_udpDatagramStartStringLength;
        returnTask.m_bufferType = ctsTask::BufferType::Static;
        return returnTask;
    }

    memcpy_s(returnTask.m_buffer, c_udpDatagramProtocolHeaderFlagLength, &c_udpDatagramFlagSynAck, c_udpDatagramProtocolHeaderFlagLength);
    const uint64_t zero64 = 0;
    memcpy_s(returnTask.m_buffer + c_udpDatagramProtocolHeaderFlagLength, c_udpDatagramSequenceNumberLength + c_udpDatagramQpcLength + c_udpDatagramQpfLength, &zero64, c_udpDatagramSequenceNumberLength + c_udpDatagramQpcLength + c_udpDatagramQpfLength);

    const auto bodyOffset = c_udpDatagramDataHeaderLength;
    returnTask.m_buffer[bodyOffset + 0] = 1; // version
    returnTask.m_buffer[bodyOffset + 1] = accept ? 0x01 : 0x00; // flags (bit0=Accept)
    returnTask.m_buffer[bodyOffset + 2] = 0;
    returnTask.m_buffer[bodyOffset + 3] = 0;
    if (assignedConnectionId)
    {
        memcpy_s(returnTask.m_buffer + bodyOffset + 4, c_udpDatagramControlConnectionIdLength, assignedConnectionId, c_udpDatagramControlConnectionIdLength);
    }
    else
    {
        memset(returnTask.m_buffer + bodyOffset + 4, 0, c_udpDatagramControlConnectionIdLength);
    }

    returnTask.m_ioAction = ctsTaskAction::Send;
    returnTask.m_bufferType = ctsTask::BufferType::Dynamic;
    returnTask.m_trackIo = false;
    return returnTask;
}

ctsTask ctsMediaStreamMessage::MakeAckTask(const ctsTask& rawTask, const char* const confirmedConnectionId) noexcept
{
    ctsTask returnTask{ rawTask };
    if (returnTask.m_bufferLength < c_udpDatagramDataHeaderLength + c_udpDatagramControlFixedBodyLength)
    {
        returnTask.m_buffer = const_cast<char*>(g_udpDatagramStartString);
        returnTask.m_bufferLength = c_udpDatagramStartStringLength;
        returnTask.m_bufferType = ctsTask::BufferType::Static;
        return returnTask;
    }

    memcpy_s(returnTask.m_buffer, c_udpDatagramProtocolHeaderFlagLength, &c_udpDatagramFlagAck, c_udpDatagramProtocolHeaderFlagLength);
    const uint64_t zero64 = 0;
    memcpy_s(returnTask.m_buffer + c_udpDatagramProtocolHeaderFlagLength, c_udpDatagramSequenceNumberLength + c_udpDatagramQpcLength + c_udpDatagramQpfLength, &zero64, c_udpDatagramSequenceNumberLength + c_udpDatagramQpcLength + c_udpDatagramQpfLength);

    const auto bodyOffset = c_udpDatagramDataHeaderLength;
    returnTask.m_buffer[bodyOffset + 0] = 1; // version
    returnTask.m_buffer[bodyOffset + 1] = 0; // flags
    returnTask.m_buffer[bodyOffset + 2] = 0;
    returnTask.m_buffer[bodyOffset + 3] = 0;
    if (confirmedConnectionId)
    {
        memcpy_s(returnTask.m_buffer + bodyOffset + 4, c_udpDatagramControlConnectionIdLength, confirmedConnectionId, c_udpDatagramControlConnectionIdLength);
    }
    else
    {
        memset(returnTask.m_buffer + bodyOffset + 4, 0, c_udpDatagramControlConnectionIdLength);
    }

    returnTask.m_ioAction = ctsTaskAction::Send;
    returnTask.m_bufferType = ctsTask::BufferType::Dynamic;
    returnTask.m_trackIo = false;
    return returnTask;
}

bool ctsMediaStreamMessage::ParseControlFrame(char* connectionIdOut, const ctsTask& task, uint32_t completedBytes) noexcept
{
    if (completedBytes < c_udpDatagramDataHeaderLength + c_udpDatagramControlFixedBodyLength)
    {
        return false;
    }

    const auto protocol = GetProtocolHeaderFromTask(task);
    if (protocol != c_udpDatagramFlagSyn && protocol != c_udpDatagramFlagSynAck && protocol != c_udpDatagramFlagAck)
    {
        return false;
    }

    const auto bodyOffset = task.m_bufferOffset + c_udpDatagramDataHeaderLength;
    if (connectionIdOut)
    {
        memcpy_s(connectionIdOut, ctsStatistics::ConnectionIdLength, task.m_buffer + bodyOffset + 4, ctsStatistics::ConnectionIdLength);
    }
    return true;
}
