/**
 * @file hil_conformance_demo.cpp
 * @brief openEtherCAT source file.
 */

#include <algorithm>
#include <chrono>
#include <iostream>
#include <vector>

#include "openethercat/master/cycle_controller.hpp"
#include "openethercat/master/ethercat_master.hpp"
#include "openethercat/transport/mock_transport.hpp"

int main() {
    oec::NetworkConfiguration cfg;
    cfg.processImageInputBytes = 1;
    cfg.processImageOutputBytes = 1;
    cfg.slaves = {
        {.name = "EL1008", .alias = 0, .position = 1, .vendorId = 0x2, .productCode = 0x03f03052},
        {.name = "EL2008", .alias = 0, .position = 2, .vendorId = 0x2, .productCode = 0x07d83052},
    };
    cfg.signals = {
        {.logicalName = "InputA", .direction = oec::SignalDirection::Input, .slaveName = "EL1008", .byteOffset = 0, .bitOffset = 0},
        {.logicalName = "OutputA", .direction = oec::SignalDirection::Output, .slaveName = "EL2008", .byteOffset = 0, .bitOffset = 0},
    };

    oec::MockTransport transport(1, 1);
    oec::EthercatMaster master(transport);
    if (!master.configure(cfg) || !master.start()) {
        std::cerr << "startup failed: " << master.lastError() << '\n';
        return 1;
    }

    std::vector<double> runtimes;
    runtimes.reserve(4000);

    for (int i = 0; i < 4000; ++i) {
        if (i > 0 && i % 500 == 0) {
            transport.setSlaveAlStatusCode(2, 0x0017U);
            transport.requestSlaveState(2, oec::SlaveState::SafeOp);
            transport.injectExchangeFailures(1);
        }

        const auto t0 = std::chrono::steady_clock::now();
        (void)master.runCycle();
        const auto t1 = std::chrono::steady_clock::now();
        const auto us = std::chrono::duration_cast<std::chrono::microseconds>(t1 - t0).count();
        runtimes.push_back(static_cast<double>(us));
    }

    std::sort(runtimes.begin(), runtimes.end());
    const auto p99Index = static_cast<std::size_t>(0.99 * static_cast<double>(runtimes.size() - 1));
    const auto p99 = runtimes[p99Index];

    const auto report = master.evaluateHilConformance(0.01, 500.0, 2000, p99);
    std::cout << "kpi.cycles=" << report.kpi.cycles
              << " failures=" << report.kpi.cycleFailures
              << " fail_rate=" << report.kpi.cycleFailureRate
              << " p99_us=" << report.kpi.p99CycleRuntimeUs
              << " degraded_cycles=" << report.kpi.degradedCycles << '\n';

    for (const auto& rule : report.rules) {
        std::cout << rule.id << ": " << (rule.passed ? "PASS" : "FAIL")
                  << " - " << rule.description << '\n';
    }

    master.stop();
    return 0;
}
