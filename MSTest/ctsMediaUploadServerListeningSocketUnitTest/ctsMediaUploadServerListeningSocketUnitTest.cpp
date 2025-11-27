// Unit tests for ctsMediaUpload protocol helpers used by server listening socket
#include <sdkddkver.h>
#include "CppUnitTest.h"

#include "ctsMediaUploadProtocol.hpp"
#include "ctsIOTask.hpp"

using namespace Microsoft::VisualStudio::CppUnitTestFramework;
using namespace ctsTraffic;

namespace ctsUnitTest
{
    TEST_CLASS(ctsMediaUploadServerListeningSocketUnitTest)
    {
    public:
        TEST_METHOD(Construct_StartTask)
        {
            auto task = ctsMediaUploadMessage::Construct(MediaUploadAction::START);
            Assert::IsTrue(task.m_ioAction == ctsTaskAction::Send);
            Assert::IsTrue(task.m_bufferLength == c_udpDatagramStartStringLength);
        }

        TEST_METHOD(MakeConnectionIdTask_Length)
        {
            ctsTask raw{};
            raw.m_bufferLength = ctsStatistics::ConnectionIdLength + c_udpDatagramProtocolHeaderFlagLength;
            raw.m_buffer = (char*)malloc(raw.m_bufferLength);
            // provide a fake connection id
            const char conn[ctsStatistics::ConnectionIdLength] = "00000000-0000-0000-0000-000000000000";
            auto out = ctsMediaUploadMessage::MakeConnectionIdTask(raw, conn);
            Assert::AreEqual(out.m_bufferLength, raw.m_bufferLength);
            free(raw.m_buffer);
        }
    };
}