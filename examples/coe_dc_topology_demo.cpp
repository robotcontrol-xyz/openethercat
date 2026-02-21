/**
 * @file coe_dc_topology_demo.cpp
 * @brief openEtherCAT source file.
 */

#include <chrono>
#include <iostream>
#include <memory>
#include <string>

#include "openethercat/master/ethercat_master.hpp"
#include "openethercat/transport/mock_transport.hpp"
#include "openethercat/transport/transport_factory.hpp"

int main(int argc, char** argv) {
    const std::string transportSpec = (argc > 1) ? argv[1] : "mock";

    oec::NetworkConfiguration cfg;
    cfg.processImageInputBytes = 1;
    cfg.processImageOutputBytes = 1;
    cfg.slaves = {
        {.name = "EL1008", .alias = 0, .position = 1, .vendorId = 0x2, .productCode = 0x03f03052},
        {.name = "EL2008", .alias = 0, .position = 2, .vendorId = 0x2, .productCode = 0x07d83052},
    };
    cfg.signals = {
        {.logicalName = "InputA", .direction = oec::SignalDirection::Input, .slaveName = "EL1008", .byteOffset = 0, .bitOffset = 0},
        {.logicalName = "OutputA", .direction = oec::SignalDirection::Output, .slaveName = "EL2008", .byteOffset = 0, .bitOffset = 0},
    };

    std::string transportError;
    oec::TransportFactoryConfig transportConfig;
    transportConfig.mockInputBytes = cfg.processImageInputBytes;
    transportConfig.mockOutputBytes = cfg.processImageOutputBytes;
    if (!oec::TransportFactory::parseTransportSpec(transportSpec, transportConfig, transportError)) {
        std::cerr << "Invalid transport spec: " << transportError << '\n';
        return 1;
    }

    auto transport = oec::TransportFactory::create(transportConfig, transportError);
    if (!transport) {
        std::cerr << "Transport creation failed: " << transportError << '\n';
        return 1;
    }

    oec::EthercatMaster master(*transport);
    if (!master.configure(cfg) || !master.start()) {
        std::cerr << "Master startup failed: " << master.lastError() << '\n';
        return 1;
    }

    const auto sdoWr = master.sdoDownload(2, {.index = 0x2000, .subIndex = 1}, {0x11, 0x22});
    const auto sdoRd = master.sdoUpload(2, {.index = 0x2000, .subIndex = 1});
    std::cout << "sdo_wr=" << (sdoWr.success ? 1 : 0) << " sdo_rd_len=" << sdoRd.data.size() << '\n';

    std::string pdoErr;
    const bool pdoOk = master.configureRxPdo(2, {{.index = 0x7000, .subIndex = 1, .bitLength = 1}}, pdoErr);
    std::cout << "pdo_cfg=" << (pdoOk ? 1 : 0) << " err=" << pdoErr << '\n';

    if (auto* mock = dynamic_cast<oec::MockTransport*>(transport.get())) {
        mock->setDiscoveredSlaves({
            {.position = 1, .vendorId = 0x2, .productCode = 0x03f03052, .online = true},
            {.position = 2, .vendorId = 0x2, .productCode = 0x07d83052, .online = true},
        });
    }
    std::string topoErr;
    if (master.refreshTopology(topoErr)) {
        const auto snap = master.topologySnapshot();
        std::cout << "topology_slaves=" << snap.slaves.size() << " redundancy_ok=" << (snap.redundancyHealthy ? 1 : 0)
                  << '\n';
    }

    for (int i = 0; i < 10; ++i) {
        const auto corr = master.updateDistributedClock(1'000'000LL * (i + 1), 1'000'000LL * (i + 1) - 500 + i * 10);
        if (corr) {
            std::cout << "dc_correction_ns=" << *corr << '\n';
        }
    }

    master.stop();
    return 0;
}
