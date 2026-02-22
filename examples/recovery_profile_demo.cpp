/**
 * @file recovery_profile_demo.cpp
 * @brief openEtherCAT source file.
 */

#include <iostream>

#include "openethercat/config/config_loader.hpp"
#include "openethercat/config/recovery_profile_loader.hpp"
#include "openethercat/master/ethercat_master.hpp"
#include "openethercat/master/slave_diagnostics.hpp"
#include "openethercat/transport/mock_transport.hpp"

int main() {
    // Base ENI/ESI config defines network and process image expected by the profile.
    oec::NetworkConfiguration config;
    std::string error;
    if (!oec::ConfigurationLoader::loadFromEniAndEsiDirectory(
            "examples/config/beckhoff_demo.eni.xml", "examples/config", config, error)) {
        std::cerr << "Config load failed: " << error << '\n';
        return 1;
    }

    // Profile maps AL status codes to explicit policy overrides.
    oec::RecoveryProfile profile;
    if (!oec::RecoveryProfileLoader::loadFromJsonFile("examples/config/recovery_profile.json", profile, error)) {
        std::cerr << "Recovery profile load failed: " << error << '\n';
        return 1;
    }

    oec::MockTransport transport(config.processImageInputBytes, config.processImageOutputBytes);
    oec::EthercatMaster master(transport);
    if (!master.configure(config) || !master.start()) {
        std::cerr << "Master startup failed: " << master.lastError() << '\n';
        return 1;
    }

    // Apply profile overrides before injecting the test fault.
    for (const auto& entry : profile.actionByAlStatusCode) {
        master.setRecoveryActionOverride(entry.first, entry.second);
    }

    transport.setSlaveAlStatusCode(2, 0x0014U); // mapped to Failover from profile
    transport.injectExchangeFailures(1);

    (void)master.runCycle();

    // Recovery event history is the primary artifact for policy verification.
    const auto events = master.recoveryEvents();
    for (const auto& e : events) {
        std::cout << "cycle=" << e.cycleIndex
                  << " slave=" << e.slavePosition
                  << " al=0x" << std::hex << e.alStatusCode << std::dec
                  << " action=" << oec::toString(e.action)
                  << " success=" << (e.success ? 1 : 0)
                  << " msg=" << e.message << '\n';
    }

    std::cout << "degraded=" << (master.isDegraded() ? 1 : 0) << '\n';

    master.stop();
    return 0;
}
