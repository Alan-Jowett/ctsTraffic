/*
  Unit tests for ctl::ctCpuAffinity helpers
*/

#include <sdkddkver.h>
#include <winsock2.h>
#include <ws2tcpip.h>
#include "CppUnitTest.h"

#include "../../ctl/ctCpuAffinity.hpp"

using namespace Microsoft::VisualStudio::CppUnitTestFramework;
using namespace std;

namespace ctsUnitTest
{
    TEST_CLASS(ctsCpuAffinityUnitTest)
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

        TEST_METHOD(QueryCpuAffinitySupport_Basic)
        {
            const auto info = ctl::QueryCpuAffinitySupport();
            Assert::IsTrue(info.ProcessorGroupCount >= 1);
            Assert::IsTrue(info.LogicalProcessorCount >= 1);
        }

        TEST_METHOD(ParsePolicyName_Varieties)
        {
            auto p = ctl::ParsePolicyName(L"none");
            Assert::IsTrue(p.has_value());
            Assert::AreEqual(static_cast<int>(*p), static_cast<int>(ctl::CpuAffinityPolicy::None));

            p = ctl::ParsePolicyName(L"percpu");
            Assert::IsTrue(p.has_value());
            Assert::AreEqual(static_cast<int>(*p), static_cast<int>(ctl::CpuAffinityPolicy::PerCpu));

            p = ctl::ParsePolicyName(L"per_group");
            Assert::IsTrue(p.has_value());
            Assert::AreEqual(static_cast<int>(*p), static_cast<int>(ctl::CpuAffinityPolicy::PerGroup));

            p = ctl::ParsePolicyName(L"rss_aligned");
            Assert::IsTrue(p.has_value());
            Assert::AreEqual(static_cast<int>(*p), static_cast<int>(ctl::CpuAffinityPolicy::RssAligned));

            p = ctl::ParsePolicyName(L"manual");
            Assert::IsTrue(p.has_value());
            Assert::AreEqual(static_cast<int>(*p), static_cast<int>(ctl::CpuAffinityPolicy::Manual));
        }

        TEST_METHOD(ComputeShardAffinities_Sanity)
        {
            const uint32_t shardCount = 4;
            auto mapping = ctl::ComputeShardAffinities(shardCount, ctl::CpuAffinityPolicy::PerCpu);
            Assert::IsTrue(mapping.has_value());
            Assert::AreEqual(shardCount, static_cast<uint32_t>(mapping->size()));

            // None policy should return zero masks
            auto noneMap = ctl::ComputeShardAffinities(shardCount, ctl::CpuAffinityPolicy::None);
            Assert::IsTrue(noneMap.has_value());
            for (const auto& g : *noneMap)
            {
                Assert::AreEqual(static_cast<unsigned long long>(0), static_cast<unsigned long long>(g.Mask));
            }

            // Manual should indicate uncomputable mapping
            auto manualMap = ctl::ComputeShardAffinities(shardCount, ctl::CpuAffinityPolicy::Manual);
            Assert::IsFalse(manualMap.has_value());
        }
    };
}
