/**
 * @file mock_hil_soak.cpp
 * @brief openEtherCAT source file.
 */

#include <chrono>
#include <iostream>

#include "openethercat/master/ethercat_master.hpp"
#include "openethercat/transport/mock_transport.hpp"

int main() {
    oec::NetworkConfiguration cfg;
    cfg.processImageInputBytes = 1;
    cfg.processImageOutputBytes = 1;
    cfg.slaves = {
        {.name = "EL1004", .alias = 0, .position = 1, .vendorId = 0x2, .productCode = 0x03ec3052},
        {.name = "EL2004", .alias = 0, .position = 2, .vendorId = 0x2, .productCode = 0x07d43052},
    };
    cfg.signals = {
        {.logicalName = "InputA", .direction = oec::SignalDirection::Input, .slaveName = "EL1004", .byteOffset = 0, .bitOffset = 0},
        {.logicalName = "OutputA", .direction = oec::SignalDirection::Output, .slaveName = "EL2004", .byteOffset = 0, .bitOffset = 0},
    };

    oec::MockTransport transport(1, 1);
    oec::EthercatMaster master(transport);
    oec::EthercatMaster::RecoveryOptions recovery;
    recovery.maxRetriesPerSlave = 2;
    recovery.maxReconfigurePerSlave = 2;
    recovery.maxEventHistory = 4096;
    master.setRecoveryOptions(recovery);

    if (!master.configure(cfg) || !master.start()) {
        std::cerr << "startup failed: " << master.lastError() << '\n';
        return 1;
    }

    constexpr int cycles = 5000;
    int failures = 0;
    for (int c = 0; c < cycles; ++c) {
        if (c > 0 && c % 500 == 0) {
            transport.setSlaveAlStatusCode(2, 0x0017U);
            transport.requestSlaveState(2, oec::SlaveState::SafeOp);
            transport.injectExchangeFailures(1);
        }

        if (!master.runCycle()) {
            ++failures;
        }

        if (c % 1000 == 0) {
            auto dc = master.updateDistributedClock(10'000'000LL + c * 1000LL, 10'000'300LL + c * 1000LL);
            (void)dc;
        }
    }

    const auto events = master.recoveryEvents();
    const auto stats = master.statistics();
    std::cout << "cycles=" << stats.cyclesTotal << " failures=" << failures
              << " recovery_events=" << events.size() << " degraded=" << (master.isDegraded() ? 1 : 0)
              << '\n';

    master.stop();
    return 0;
}
