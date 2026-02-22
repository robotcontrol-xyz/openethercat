/**
 * @file periodic_controller_demo.cpp
 * @brief openEtherCAT source file.
 */

#include <chrono>
#include <iostream>
#include <thread>

#include "openethercat/config/config_loader.hpp"
#include "openethercat/master/cycle_controller.hpp"
#include "openethercat/master/ethercat_master.hpp"
#include "openethercat/transport/mock_transport.hpp"

using namespace std::chrono_literals;

int main() {
    // Reuse Beckhoff ENI/ESI so demo mirrors real signal naming and offsets.
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

    // Application-level coupling: mirror StartButton input to LampGreen output.
    master.onInputChange("StartButton", [&](bool state) {
        master.setOutputByName("LampGreen", state);
    });

    // Configure a 1 ms periodic scheduler with fail-fast behavior on repeated faults.
    oec::CycleController controller;
    oec::CycleControllerOptions options;
    options.period = 1ms;
    options.stopOnError = true;
    options.maxConsecutiveFailures = 5;

    controller.start(master, options, [](const oec::CycleReport& report) {
        // Throttle reporting to avoid console noise during high-rate cycles.
        if ((report.cycleIndex % 100U) == 0U) {
            std::cout << "cycle=" << report.cycleIndex
                      << " ok=" << (report.success ? 1 : 0)
                      << " wkc=" << report.workingCounter
                      << " runtime_us=" << report.runtime.count() << '\n';
        }
    });

    // Emulate a square-wave input to exercise callback and output write paths.
    for (int i = 0; i < 1000; ++i) {
        transport.setInputBit(0, 0, (i % 200) < 100);
        std::this_thread::sleep_for(1ms);
    }

    controller.stop();
    master.stop();

    const auto stats = master.statistics();
    std::cout << "total_cycles=" << stats.cyclesTotal
              << " failed_cycles=" << stats.cyclesFailed
              << " last_wkc=" << stats.lastWorkingCounter << '\n';
    return 0;
}
