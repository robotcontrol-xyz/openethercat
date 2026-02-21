/**
 * @file beckhoff_io_demo.cpp
 * @brief openEtherCAT source file.
 */

#include <chrono>
#include <csignal>
#include <iostream>
#include <memory>
#include <string>
#include <thread>
#include <atomic>

#include "openethercat/config/config_loader.hpp"
#include "openethercat/master/ethercat_master.hpp"
#include "openethercat/transport/mock_transport.hpp"
#include "openethercat/transport/transport_factory.hpp"

using namespace std::chrono_literals;

namespace {
std::atomic_bool gStopRequested{false};

void handleSignal(int) {
    gStopRequested.store(true);
}
} // namespace

int main(int argc, char** argv) {
    std::signal(SIGINT, handleSignal);

    const std::string transportSpec = (argc > 1) ? argv[1] : "mock";
    const std::string eniPath = (argc > 2) ? argv[2] : "examples/config/beckhoff_demo.eni.xml";
    const std::string esiDir = (argc > 3) ? argv[3] : "examples/config";

    oec::NetworkConfiguration config;
    std::string error;
    if (!oec::ConfigurationLoader::loadFromEniAndEsiDirectory(
            eniPath,
            esiDir,
            config,
            error)) {
        std::cerr << "Config load failed: " << error << '\n';
        return 1;
    }

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

    if (!master.configure(config)) {
        std::cerr << "Configure failed: " << master.lastError() << '\n';
        return 1;
    }
    if (!master.onInputChange("StartButton", [&](bool state) {
            std::cout << "Callback: StartButton=" << (state ? "ON" : "OFF") << '\n';
            if (!master.setOutputByName("LampGreen", state)) {
                std::cerr << "Set output failed: " << master.lastError() << '\n';
            }
        })) {
        std::cerr << "Callback registration failed: " << master.lastError() << '\n';
        return 1;
    }

    if (!master.start()) {
        std::cerr << "Start failed: " << master.lastError() << '\n';
        return 1;
    }

    auto* mock = dynamic_cast<oec::MockTransport*>(transport.get());
    if (mock) {
        std::cout << "Simulating EL1004 input toggles and controlling EL2004 output. Press Ctrl-C to stop.\n";
        int cycle = 0;
        while (!gStopRequested.load()) {
            const bool inputState = (cycle % 2) == 1;
            mock->setInputBit(0, 0, inputState);

            if (!master.runCycle()) {
                std::cerr << "Cycle failed: " << master.lastError() << '\n';
                master.stop();
                return 1;
            }

            std::cout << "Cycle " << cycle
                      << ", EL1004.StartButton=" << (inputState ? 1 : 0)
                      << ", EL2004.LampGreen(out byte0/bit0)="
                      << (mock->getLastOutputBit(0, 0) ? 1 : 0) << '\n';

            std::this_thread::sleep_for(150ms);
            ++cycle;
        }
    } else {
        std::cout << "Running physical cycle mode; toggling EL2004 output to trigger EL1004 callback. "
                     "Press Ctrl-C to stop.\n";
        bool driveOutput = false;
        int cycle = 0;
        while (!gStopRequested.load()) {
            if ((cycle % 100) == 0) {
                driveOutput = !driveOutput;
                if (!master.setOutputByName("LampGreen", driveOutput)) {
                    std::cerr << "Failed to toggle output: " << master.lastError() << '\n';
                    master.stop();
                    return 1;
                }
                std::cout << "Toggled LampGreen=" << (driveOutput ? "ON" : "OFF") << '\n';
            }
            if (!master.runCycle()) {
                std::cerr << "Cycle failed: " << master.lastError() << '\n';
                master.stop();
                return 1;
            }
            std::this_thread::sleep_for(5ms);
            ++cycle;
        }
    }

    master.stop();
    return 0;
}
