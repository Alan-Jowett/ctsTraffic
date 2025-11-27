// Unit tests for ctsMediaUpload server connected socket helpers
#include <sdkddkver.h>
#include "CppUnitTest.h"

#include "ctsMediaUploadProtocol.hpp"

using namespace Microsoft::VisualStudio::CppUnitTestFramework;
using namespace ctsTraffic;

// Minimal stubs for ctsConfig functions referenced by the protocol helpers
namespace ctsConfigStub {
    void PrintErrorInfo(PCWSTR, ...) noexcept {}
    const ctsConfig::MediaStreamSettings& GetMediaStreamStub() noexcept
    {
        static ctsConfig::MediaStreamSettings s;
        if (s.DatagramMaxSize == 0) {
            s.DatagramMaxSize = 1500;
            s.BitsPerSecond = 8000000;
            s.FramesPerSecond = 30;
            s.BufferDepthSeconds = 1;
            s.StreamLengthSeconds = 1;
        }
        return s;
    }
}

// Linker-friendly redirectors placed in the ctsTraffic::ctsConfig namespace
namespace ctsTraffic { namespace ctsConfig {
    void PrintErrorInfo(PCWSTR text, ...) noexcept { ctsConfigStub::PrintErrorInfo(text); }
    const MediaStreamSettings& GetMediaStream() noexcept { return ctsConfigStub::GetMediaStreamStub(); }
} }

namespace ctsUnitTest
{
    TEST_CLASS(ctsMediaUploadServerConnectedSocketUnitTest)
    {
    public:
        TEST_METHOD(ValidateBufferLengthFromTask_DataAndId)
        {
            ctsTask task{};
            // test Data frame: protocol header + seq + qpc + qpf + at least 1 byte
            task.m_buffer = (char*)malloc(c_udpDatagramDataHeaderLength + 1);
            task.m_bufferLength = c_udpDatagramDataHeaderLength + 1;
            // set protocol header to DATA
            *reinterpret_cast<unsigned short*>(task.m_buffer) = c_udpDatagramProtocolHeaderFlagData;
            Assert::IsTrue(ctsMediaUploadMessage::ValidateBufferLengthFromTask(task, task.m_bufferLength));
            free(task.m_buffer);

            // test ID frame: header + connection id length
            ctsTask idTask{};
            idTask.m_bufferLength = c_udpDatagramProtocolHeaderFlagLength + ctsStatistics::ConnectionIdLength;
            idTask.m_buffer = (char*)malloc(idTask.m_bufferLength);
            *reinterpret_cast<unsigned short*>(idTask.m_buffer) = c_udpDatagramProtocolHeaderFlagId;
            Assert::IsTrue(ctsMediaUploadMessage::ValidateBufferLengthFromTask(idTask, idTask.m_bufferLength));
            free(idTask.m_buffer);
        }
    };
}