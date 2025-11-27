// Unit tests for server-side media upload helpers
#include <sdkddkver.h>
#include "CppUnitTest.h"

#include "ctsMediaUploadProtocol.hpp"

using namespace Microsoft::VisualStudio::CppUnitTestFramework;
using namespace ctsTraffic;

namespace ctsUnitTest
{
    TEST_CLASS(ctsMediaUploadServerUnitTest)
    {
    public:
        TEST_METHOD(MakeConnectionIdTask_ContainsHeader)
        {
            ctsTask raw{};
            raw.m_bufferLength = ctsStatistics::ConnectionIdLength + c_udpDatagramProtocolHeaderFlagLength;
            raw.m_buffer = (char*)malloc(raw.m_bufferLength);

            const char conn[ctsStatistics::ConnectionIdLength] = "11111111-1111-1111-1111-111111111111";
            auto task = ctsMediaUploadMessage::MakeConnectionIdTask(raw, conn);
            // the protocol header should be set to the Id flag in the returned task buffer
            unsigned short hdr = *reinterpret_cast<unsigned short*>(task.m_buffer);
            Assert::AreEqual(static_cast<unsigned short>(c_udpDatagramProtocolHeaderFlagId), hdr);
            free(raw.m_buffer);
        }
    };
}
