#include <chrono>
#include <iostream>
#include <thread>

#include "openethercat/config/config_loader.hpp"
#include "openethercat/master/ethercat_master.hpp"
#include "openethercat/transport/mock_transport.hpp"

using namespace std::chrono_literals;

int main() {
    oec::NetworkConfiguration config;
    std::string error;
    if (!oec::ConfigurationLoader::loadFromEniAndEsiDirectory(
            "examples/config/beckhoff_demo.eni.xml",
            "examples/config",
            config,
            error)) {
        std::cerr << "Config load failed: " << error << '\n';
        return 1;
    }

    oec::MockTransport transport(config.processImageInputBytes, config.processImageOutputBytes);
    oec::EthercatMaster master(transport);

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

    std::cout << "Simulating EL1008 input toggles and controlling EL2008 output...\n";

    for (int cycle = 0; cycle < 6; ++cycle) {
        const bool inputState = (cycle % 2) == 1;
        transport.setInputBit(0, 0, inputState);

        if (!master.runCycle()) {
            std::cerr << "Cycle failed: " << master.lastError() << '\n';
            master.stop();
            return 1;
        }

        std::cout << "Cycle " << cycle
                  << ", EL1008.StartButton=" << (inputState ? 1 : 0)
                  << ", EL2008.LampGreen(out byte0/bit0)="
                  << (transport.getLastOutputBit(0, 0) ? 1 : 0) << '\n';

        std::this_thread::sleep_for(150ms);
    }

    master.stop();
    return 0;
}
