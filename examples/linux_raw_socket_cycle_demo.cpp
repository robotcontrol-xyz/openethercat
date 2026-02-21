#include <chrono>
#include <iostream>
#include <string>
#include <thread>

#include "openethercat/config/config_loader.hpp"
#include "openethercat/master/cycle_controller.hpp"
#include "openethercat/master/ethercat_master.hpp"
#include "openethercat/transport/linux_raw_socket_transport.hpp"

using namespace std::chrono_literals;

int main(int argc, char** argv) {
    if (argc < 2) {
        std::cerr << "Usage: " << argv[0] << " <ifname> [eni_file] [esi_dir]\n";
        return 1;
    }

    const std::string ifname = argv[1];
    const std::string eniPath = (argc > 2) ? argv[2] : "examples/config/beckhoff_demo.eni.xml";
    const std::string esiDir = (argc > 3) ? argv[3] : "examples/config";

    oec::NetworkConfiguration config;
    std::string error;
    if (!oec::ConfigurationLoader::loadFromEniAndEsiDirectory(eniPath, esiDir, config, error)) {
        std::cerr << "Config load failed: " << error << '\n';
        return 1;
    }

    oec::LinuxRawSocketTransport transport(ifname);
    transport.setCycleTimeoutMs(20);
    transport.setExpectedWorkingCounter(1);
    transport.setLogicalAddress(0);

    oec::EthercatMaster master(transport);
    if (!master.configure(config) || !master.start()) {
        std::cerr << "Master startup failed: " << master.lastError() << '\n';
        return 1;
    }

    oec::CycleController controller;
    oec::CycleControllerOptions options;
    options.period = 1ms;
    options.maxConsecutiveFailures = 3;

    controller.start(master, options, [&](const oec::CycleReport& report) {
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
