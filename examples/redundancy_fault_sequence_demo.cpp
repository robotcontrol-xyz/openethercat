/**
 * @file redundancy_fault_sequence_demo.cpp
 * @brief Scripted phase-3 redundancy fault sequence with KPI timeline output.
 */

#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <string>
#include <thread>
#include <vector>

#include "openethercat/master/ethercat_master.hpp"
#include "openethercat/transport/mock_transport.hpp"

using namespace std::chrono_literals;

namespace {

struct Step {
    const char* label;
    bool redundancyHealthy;
    int cycles;
};

void printStatus(const oec::EthercatMaster& master, std::size_t stepIndex, const char* label, bool jsonMode) {
    const auto s = master.redundancyStatus();
    const auto k = master.redundancyKpis();
    const auto t = master.redundancyTransitions();
    if (jsonMode) {
        std::cout << "{\"type\":\"step\",\"step\":" << stepIndex
                  << ",\"label\":\"" << label << "\""
                  << ",\"redundancy_healthy\":" << (s.redundancyHealthy ? 1 : 0)
                  << ",\"state\":" << static_cast<int>(s.state)
                  << ",\"transitions\":" << s.transitionCount
                  << ",\"timeline_events\":" << t.size()
                  << ",\"degrade_events\":" << k.degradeEvents
                  << ",\"recover_events\":" << k.recoverEvents
                  << ",\"impacted_cycles\":" << k.impactedCycles
                  << ",\"last_detection_ms\":" << k.lastDetectionLatencyMs
                  << ",\"last_policy_ms\":" << k.lastPolicyTriggerLatencyMs
                  << ",\"last_recovery_ms\":" << k.lastRecoveryLatencyMs
                  << "}\n";
    } else {
        std::cout << "step=" << stepIndex
                  << " label=" << label
                  << " healthy=" << (s.redundancyHealthy ? 1 : 0)
                  << " state=" << static_cast<int>(s.state)
                  << " transitions=" << s.transitionCount
                  << " timeline_events=" << t.size()
                  << " degrade_events=" << k.degradeEvents
                  << " recover_events=" << k.recoverEvents
                  << " impacted_cycles=" << k.impactedCycles
                  << " last_detection_ms=" << k.lastDetectionLatencyMs
                  << " last_policy_ms=" << k.lastPolicyTriggerLatencyMs
                  << " last_recovery_ms=" << k.lastRecoveryLatencyMs
                  << '\n';
    }
}

} // namespace

int main() {
    const bool jsonMode = (std::getenv("OEC_SOAK_JSON") != nullptr);

    oec::MockTransport transport(1, 1);
    oec::EthercatMaster master(transport);

    oec::NetworkConfiguration config;
    config.processImageInputBytes = 1;
    config.processImageOutputBytes = 1;
    config.slaves = {
        {.name = "EK1100", .alias = 0, .position = 0, .vendorId = 0x00000002, .productCode = 0x044c2c52},
    };
    config.signals = {
        {.logicalName = "InputA", .direction = oec::SignalDirection::Input, .slaveName = "EK1100", .byteOffset = 0, .bitOffset = 0},
    };

    if (!master.configure(config)) {
        std::cerr << "Configure failed: " << master.lastError() << '\n';
        return 1;
    }
    auto stateOptions = oec::EthercatMaster::StateMachineOptions{};
    stateOptions.enable = false;
    master.setStateMachineOptions(stateOptions);
    auto topoOptions = oec::EthercatMaster::TopologyRecoveryOptions{};
    topoOptions.enable = true;
    topoOptions.missingSlaveAction = oec::EthercatMaster::TopologyPolicyAction::Monitor;
    topoOptions.hotConnectAction = oec::EthercatMaster::TopologyPolicyAction::Monitor;
    topoOptions.redundancyGraceCycles = 2U;
    topoOptions.redundancyAction = oec::EthercatMaster::TopologyPolicyAction::Degrade;
    master.setTopologyRecoveryOptions(topoOptions);

    if (!master.start()) {
        std::cerr << "Start failed: " << master.lastError() << '\n';
        return 1;
    }

    transport.setDiscoveredSlaves({
        {.position = 0, .vendorId = 0x00000002, .productCode = 0x044c2c52, .online = true},
    });

    const std::vector<Step> steps = {
        {"healthy", true, 8},
        {"cable_break", false, 10},
        {"hold_fault", false, 8},
        {"restore", true, 10},
        {"flap_down", false, 4},
        {"flap_up", true, 4},
    };

    std::string error;
    for (std::size_t i = 0; i < steps.size(); ++i) {
        transport.setRedundancyHealthy(steps[i].redundancyHealthy);
        for (int c = 0; c < steps[i].cycles; ++c) {
            if (!master.refreshTopology(error)) {
                std::cerr << "refreshTopology failed: " << error << '\n';
                master.stop();
                return 2;
            }
            if (!master.runCycle()) {
                std::cerr << "runCycle failed: " << master.lastError() << '\n';
                master.stop();
                return 3;
            }
            std::this_thread::sleep_for(2ms);
        }
        printStatus(master, i, steps[i].label, jsonMode);
    }

    const auto status = master.redundancyStatus();
    const auto kpi = master.redundancyKpis();
    const auto transitions = master.redundancyTransitions();
    if (jsonMode) {
        std::cout << "{\"type\":\"summary\""
                  << ",\"state\":" << static_cast<int>(status.state)
                  << ",\"healthy\":" << (status.redundancyHealthy ? 1 : 0)
                  << ",\"transitions\":" << status.transitionCount
                  << ",\"timeline_events\":" << transitions.size()
                  << ",\"degrade_events\":" << kpi.degradeEvents
                  << ",\"recover_events\":" << kpi.recoverEvents
                  << ",\"impacted_cycles\":" << kpi.impactedCycles
                  << ",\"last_detection_ms\":" << kpi.lastDetectionLatencyMs
                  << ",\"last_policy_ms\":" << kpi.lastPolicyTriggerLatencyMs
                  << ",\"last_recovery_ms\":" << kpi.lastRecoveryLatencyMs
                  << "}\n";
    } else {
        std::cout << "summary"
                  << " state=" << static_cast<int>(status.state)
                  << " healthy=" << (status.redundancyHealthy ? 1 : 0)
                  << " transitions=" << status.transitionCount
                  << " timeline_events=" << transitions.size()
                  << " degrade_events=" << kpi.degradeEvents
                  << " recover_events=" << kpi.recoverEvents
                  << " impacted_cycles=" << kpi.impactedCycles
                  << " last_detection_ms=" << kpi.lastDetectionLatencyMs
                  << " last_policy_ms=" << kpi.lastPolicyTriggerLatencyMs
                  << " last_recovery_ms=" << kpi.lastRecoveryLatencyMs
                  << '\n';
    }

    if (!jsonMode) {
        for (const auto& e : transitions) {
            std::cout << "transition generation=" << e.topologyGeneration
                      << " cycle=" << e.cycleIndex
                      << " from=" << static_cast<int>(e.from)
                      << " to=" << static_cast<int>(e.to)
                      << " reason=" << e.reason
                      << '\n';
        }
    }

    master.stop();
    return 0;
}
