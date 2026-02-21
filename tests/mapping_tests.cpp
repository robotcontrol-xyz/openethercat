/**
 * @file mapping_tests.cpp
 * @brief openEtherCAT source file.
 */

#include <cassert>
#include <iostream>

#include "openethercat/config/eni_esi_models.hpp"
#include "openethercat/master/ethercat_master.hpp"
#include "openethercat/transport/mock_transport.hpp"

int main() {
    oec::NetworkConfiguration config;
    config.processImageInputBytes = 1;
    config.processImageOutputBytes = 1;
    config.signals = {
        {.logicalName = "InputA", .direction = oec::SignalDirection::Input, .slaveName = "EL1008", .byteOffset = 0, .bitOffset = 0},
        {.logicalName = "OutputA", .direction = oec::SignalDirection::Output, .slaveName = "EL2008", .byteOffset = 0, .bitOffset = 0},
    };

    oec::MockTransport transport(1, 1);
    oec::EthercatMaster master(transport);

    assert(master.configure(config));

    bool callbackState = false;
    int callbackCalls = 0;
    assert(master.onInputChange("InputA", [&](bool value) {
        callbackState = value;
        ++callbackCalls;
    }));

    assert(master.start());

    transport.setInputBit(0, 0, true);
    assert(master.runCycle());
    assert(callbackCalls == 1);
    assert(callbackState);

    // No change, callback should not fire again.
    assert(master.runCycle());
    assert(callbackCalls == 1);

    assert(master.setOutputByName("OutputA", true));
    assert(master.runCycle());
    assert(transport.getLastOutputBit(0, 0));

    master.stop();

    std::cout << "mapping_tests passed\n";
    return 0;
}
