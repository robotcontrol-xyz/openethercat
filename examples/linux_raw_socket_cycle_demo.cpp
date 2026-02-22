/**
 * @file linux_raw_socket_cycle_demo.cpp
 * @brief openEtherCAT source file.
 */

#include <chrono>
#include <iostream>
#include <memory>
#include <string>
#include <thread>

#include "openethercat/config/config_loader.hpp"
#include "openethercat/master/cycle_controller.hpp"
#include "openethercat/master/ethercat_master.hpp"
#include "openethercat/transport/transport_factory.hpp"

using namespace std::chrono_literals;

int main(int argc, char** argv) {
    if (argc < 2) {
        std::cerr << "Usage: " << argv[0] << " <transport-spec|ifname> [eni_file] [esi_dir]\n"
                  << "  transport-spec: mock | linux:<ifname> | linux:<ifname_primary>,<ifname_secondary>\n";
        return 1;
    }

    // Accept either full transport specs or plain interface names for convenience.
    const std::string transportArg = argv[1];
    const std::string transportSpec = (transportArg.rfind("linux:", 0) == 0 || transportArg == "mock")
                                          ? transportArg
                                          : ("linux:" + transportArg);
    const std::string eniPath = (argc > 2) ? argv[2] : "examples/config/beckhoff_demo.eni.xml";
    const std::string esiDir = (argc > 3) ? argv[3] : "examples/config";

    // Load ENI/ESI to obtain process image sizes and logical signal bindings.
    oec::NetworkConfiguration config;
    std::string error;
    if (!oec::ConfigurationLoader::loadFromEniAndEsiDirectory(eniPath, esiDir, config, error)) {
        std::cerr << "Config load failed: " << error << '\n';
        return 1;
    }

    // Build transport through the shared factory used by all examples.
    oec::TransportFactoryConfig transportConfig;
    transportConfig.mockInputBytes = config.processImageInputBytes;
    transportConfig.mockOutputBytes = config.processImageOutputBytes;
    if (!oec::TransportFactory::parseTransportSpec(transportSpec, transportConfig, error)) {
        std::cerr << "Invalid transport spec: " << error << '\n';
        return 1;
    }
    auto transport = oec::TransportFactory::create(transportConfig, error);
    if (!transport) {
        std::cerr << "Transport creation failed: " << error << '\n';
        return 1;
    }

    oec::EthercatMaster master(*transport);
    if (!master.configure(config) || !master.start()) {
        std::cerr << "Master startup failed: " << master.lastError() << '\n';
        return 1;
    }

    // Run periodic cyclic exchange on a dedicated controller loop.
    oec::CycleController controller;
    oec::CycleControllerOptions options;
    options.period = 1ms;
    options.maxConsecutiveFailures = 3;

    controller.start(master, options, [&](const oec::CycleReport& report) {
        // Keep diagnostics lightweight to avoid perturbing cycle timing.
        if (!report.success) {
            std::cerr << "Cycle " << report.cycleIndex << " failed: " << master.lastError() << '\n';
            return;
        }
        if ((report.cycleIndex % 1000U) == 0U) {
            std::cout << "cycle=" << report.cycleIndex << " wkc=" << report.workingCounter
                      << " runtime_us=" << report.runtime.count() << '\n';
        }
    });

    std::this_thread::sleep_for(10s);
    controller.stop();
    master.stop();

    const auto stats = master.statistics();
    std::cout << "total_cycles=" << stats.cyclesTotal
              << " failed_cycles=" << stats.cyclesFailed
              << " last_wkc=" << stats.lastWorkingCounter << '\n';
    return 0;
}
