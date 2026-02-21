#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace oec {

struct HilKpi {
    std::uint64_t cycles = 0;
    std::uint64_t cycleFailures = 0;
    std::uint64_t recoveryEvents = 0;
    double cycleFailureRate = 0.0;
    double p99CycleRuntimeUs = 0.0;
    std::uint64_t degradedCycles = 0;
};

struct HilConformanceRule {
    std::string id;
    std::string description;
    bool passed = false;
};

struct HilConformanceReport {
    HilKpi kpi;
    std::vector<HilConformanceRule> rules;
};

class HilCampaignEvaluator {
public:
    static HilConformanceReport evaluate(const HilKpi& kpi,
                                         double maxFailureRate,
                                         double maxP99RuntimeUs,
                                         std::uint64_t maxDegradedCycles);
};

} // namespace oec
