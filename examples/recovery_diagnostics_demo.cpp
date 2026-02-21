/**
 * @file recovery_diagnostics_demo.cpp
 * @brief openEtherCAT source file.
 */

#include <iostream>

#include "openethercat/config/config_loader.hpp"
#include "openethercat/master/ethercat_master.hpp"
#include "openethercat/master/slave_diagnostics.hpp"
#include "openethercat/transport/mock_transport.hpp"

int main() {
    oec::NetworkConfiguration config;
    std::string error;
    if (!oec::ConfigurationLoader::loadFromEniAndEsiDirectory(
            "examples/config/beckhoff_demo.eni.xml", "examples/config", config, error)) {
        std::cerr << "Config load failed: " << error << '\n';
        return 1;
    }

    oec::MockTransport transport(config.processImageInputBytes, config.processImageOutputBytes);
    oec::EthercatMaster master(transport);
    if (!master.configure(config) || !master.start()) {
        std::cerr << "Master startup failed: " << master.lastError() << '\n';
        return 1;
    }

    // Inject a slave fault to demonstrate diagnostics and policy-driven recovery.
    transport.setSlaveAlStatusCode(2, 0x0017U);
    transport.requestSlaveState(2, oec::SlaveState::SafeOp);
    transport.injectExchangeFailures(1);

    if (!master.runCycle()) {
        std::cout << "Cycle failed as expected: " << master.lastError() << '\n';
    }

    for (const auto& d : master.collectSlaveDiagnostics()) {
        std::cout << "slave=" << d.identity.name
                  << " position=" << d.identity.position
                  << " state=" << oec::toString(d.state)
                  << " al_status=0x" << std::hex << d.alStatusCode << std::dec
                  << " al_name=" << d.alStatus.name
                  << " action=" << oec::toString(d.suggestedAction) << '\n';
    }

    if (master.runCycle()) {
        std::cout << "Recovery path succeeded; cycle resumed.\n";
    }

    master.stop();
    return 0;
}
