#pragma once

#include <chrono>
#include <cstdint>

namespace oec {

struct CycleStatistics {
    std::uint64_t cyclesTotal = 0;
    std::uint64_t cyclesFailed = 0;
    std::uint16_t lastWorkingCounter = 0;
    std::chrono::microseconds lastCycleRuntime = std::chrono::microseconds(0);
};

} // namespace oec
