/**
 * @file hil_campaign.cpp
 * @brief openEtherCAT source file.
 */

#include "openethercat/master/hil_campaign.hpp"

namespace oec {

HilConformanceReport HilCampaignEvaluator::evaluate(const HilKpi& kpi,
                                                    double maxFailureRate,
                                                    double maxP99RuntimeUs,
                                                    std::uint64_t maxDegradedCycles) {
    HilConformanceReport report;
    report.kpi = kpi;

    HilConformanceRule failRate;
    failRate.id = "KPI-FAIL-RATE";
    failRate.description = "Cycle failure rate within threshold";
    failRate.passed = kpi.cycleFailureRate <= maxFailureRate;
    report.rules.push_back(failRate);

    HilConformanceRule p99;
    p99.id = "KPI-P99-RT";
    p99.description = "P99 cycle runtime below threshold";
    p99.passed = kpi.p99CycleRuntimeUs <= maxP99RuntimeUs;
    report.rules.push_back(p99);

    HilConformanceRule degraded;
    degraded.id = "KPI-DEGRADED";
    degraded.description = "Degraded cycles under threshold";
    degraded.passed = kpi.degradedCycles <= maxDegradedCycles;
    report.rules.push_back(degraded);

    return report;
}

} // namespace oec
