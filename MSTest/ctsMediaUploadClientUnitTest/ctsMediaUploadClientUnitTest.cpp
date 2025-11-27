// Unit tests for ctsMediaUpload protocol helpers (client-side behaviors)
#include <sdkddkver.h>
#include "CppUnitTest.h"

#include "ctsMediaUploadProtocol.hpp"

using namespace Microsoft::VisualStudio::CppUnitTestFramework;
using namespace ctsTraffic;

namespace ctsUnitTest
{
    TEST_CLASS(ctsMediaUploadClientUnitTest)
    {
    public:
        TEST_METHOD(Extract_StartMessage)
        {
            const char start[] = "START";
            auto msg = ctsMediaUploadMessage::Extract(start, static_cast<uint32_t>(sizeof(start) - 1));
            Assert::IsTrue(msg.m_action == MediaUploadAction::START);
        }

        TEST_METHOD(Extract_InvalidThrows)
        {
            const char bad[] = "BAD";
            bool threw = false;
            try
            {
                (void)ctsMediaUploadMessage::Extract(bad, static_cast<uint32_t>(sizeof(bad) - 1));
            }
            catch (...) { threw = true; }

            Assert::IsTrue(threw);
        }
    };
}
