/**
 * @file topology_reconcile_demo.cpp
 * @brief Phase-3 kickoff demo: deterministic topology reconciliation.
 */

#include <iostream>
#include <string>
#include <vector>

#include "openethercat/master/ethercat_master.hpp"
#include "openethercat/transport/mock_transport.hpp"

namespace {

void printChangeSet(const oec::TopologyChangeSet& changes) {
    std::cout << "generation=" << changes.generation
              << " changed=" << (changes.changed ? 1 : 0)
              << " redundancy=" << (changes.redundancyHealthy ? "healthy" : "degraded")
              << " redundancy_changed=" << (changes.redundancyChanged ? 1 : 0)
              << " added=" << changes.added.size()
              << " removed=" << changes.removed.size()
              << " updated=" << changes.updated.size()
              << '\n';

    for (const auto& s : changes.added) {
        std::cout << "  + pos=" << s.position
                  << " vid=0x" << std::hex << s.vendorId
                  << " pid=0x" << s.productCode << std::dec
                  << " online=" << (s.online ? 1 : 0)
                  << '\n';
    }
    for (const auto& s : changes.removed) {
        std::cout << "  - pos=" << s.position
                  << " vid=0x" << std::hex << s.vendorId
                  << " pid=0x" << s.productCode << std::dec
                  << " online=" << (s.online ? 1 : 0)
                  << '\n';
    }
    for (const auto& s : changes.updated) {
        std::cout << "  * pos=" << s.position
                  << " online " << (s.wasOnline ? 1 : 0) << "->" << (s.isOnline ? 1 : 0)
                  << " vid 0x" << std::hex << s.previousVendorId << "->0x" << s.vendorId
                  << " pid 0x" << s.previousProductCode << "->0x" << s.productCode
                  << std::dec
                  << '\n';
    }
}

} // namespace

int main() {
    oec::MockTransport transport(1U, 1U);
    oec::EthercatMaster master(transport);

    oec::NetworkConfiguration config;
    config.processImageInputBytes = 1;
    config.processImageOutputBytes = 1;
    config.slaves = {
        {.name = "EK1100", .alias = 0, .position = 0, .vendorId = 0x00000002, .productCode = 0x044c2c52},
        {.name = "EL1004", .alias = 0, .position = 1, .vendorId = 0x00000002, .productCode = 0x03ec3052},
        {.name = "EL2004", .alias = 0, .position = 2, .vendorId = 0x00000002, .productCode = 0x0f743052},
    };
    config.signals = {
        {.logicalName = "InputA", .direction = oec::SignalDirection::Input, .slaveName = "EL1004", .byteOffset = 0, .bitOffset = 0},
        {.logicalName = "OutputA", .direction = oec::SignalDirection::Output, .slaveName = "EL2004", .byteOffset = 0, .bitOffset = 0},
    };

    if (!master.configure(config)) {
        std::cerr << "Configure failed: " << master.lastError() << '\n';
        return 1;
    }
    auto stateOpts = oec::EthercatMaster::StateMachineOptions{};
    stateOpts.enable = false;
    master.setStateMachineOptions(stateOpts);
    if (!master.start()) {
        std::cerr << "Start failed: " << master.lastError() << '\n';
        return 1;
    }

    std::string error;
    transport.setDiscoveredSlaves({
        {.position = 0, .vendorId = 0x00000002, .productCode = 0x044c2c52, .online = true},
        {.position = 1, .vendorId = 0x00000002, .productCode = 0x03ec3052, .online = true},
        {.position = 2, .vendorId = 0x00000002, .productCode = 0x0f743052, .online = true},
    });
    transport.setRedundancyHealthy(true);
    if (!master.refreshTopology(error)) {
        std::cerr << "refreshTopology failed: " << error << '\n';
        master.stop();
        return 1;
    }
    printChangeSet(master.topologyChangeSet());

    transport.setDiscoveredSlaves({
        {.position = 0, .vendorId = 0x00000002, .productCode = 0x044c2c52, .online = true},
        {.position = 1, .vendorId = 0x00000002, .productCode = 0x03ec3052, .online = false},
        {.position = 3, .vendorId = 0x00000002, .productCode = 0x1a243052, .online = true},
    });
    transport.setRedundancyHealthy(false);
    if (!master.refreshTopology(error)) {
        std::cerr << "refreshTopology failed: " << error << '\n';
        master.stop();
        return 1;
    }
    printChangeSet(master.topologyChangeSet());

    const auto missing = master.missingSlaves();
    const auto hot = master.hotConnectedSlaves();
    std::cout << "missing=" << missing.size() << " hot_connected=" << hot.size() << '\n';

    master.stop();
    return 0;
}
