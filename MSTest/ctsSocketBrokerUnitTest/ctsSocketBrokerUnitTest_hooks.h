// Test hook declarations for ctsSocketBroker unit tests
#pragma once
#include <vector>
#include <winsock2.h>

namespace ctsTestHook {
    struct AffinityRecord {
        unsigned int ShardId;
        WORD Group;
        KAFFINITY Mask;
    };

    extern std::vector<AffinityRecord> AppliedAffinityRecords;
}
