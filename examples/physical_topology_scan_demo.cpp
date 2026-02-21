/**
 * @file physical_topology_scan_demo.cpp
 * @brief openEtherCAT source file.
 */

#include <iomanip>
#include <iostream>
#include <string>

#include "openethercat/master/topology_manager.hpp"
#include "openethercat/transport/transport_factory.hpp"

namespace {

void printUsage(const char* argv0) {
    std::cerr << "Usage: " << argv0 << " <transport-spec>\n"
              << "  transport-spec: linux:<ifname> | linux:<ifname_primary>,<ifname_secondary> | mock\n"
              << "Examples:\n"
              << "  " << argv0 << " linux:eth0\n"
              << "  " << argv0 << " linux:eth0,eth1\n";
}

} // namespace

int main(int argc, char** argv) {
    if (argc < 2) {
        printUsage(argv[0]);
        return 1;
    }

    std::string error;
    oec::TransportFactoryConfig transportConfig;
    if (!oec::TransportFactory::parseTransportSpec(argv[1], transportConfig, error)) {
        std::cerr << "Invalid transport spec: " << error << '\n';
        return 1;
    }

    auto transport = oec::TransportFactory::create(transportConfig, error);
    if (!transport) {
        std::cerr << "Transport creation failed: " << error << '\n';
        return 1;
    }

    if (!transport->open()) {
        std::cerr << "Transport open failed: " << transport->lastError() << '\n';
        return 1;
    }

    oec::TopologyManager topology(*transport);
    if (!topology.refresh(error)) {
        std::cerr << "Topology scan failed: " << error << '\n';
        transport->close();
        return 1;
    }

    const auto snapshot = topology.snapshot();
    std::cout << "Discovered " << snapshot.slaves.size() << " slave(s)"
              << ", redundancy_healthy=" << (snapshot.redundancyHealthy ? "true" : "false") << '\n';

    std::cout << "Position  Online  VendorId    ProductCode  EscType  EscRev  IdentitySource\n";
    for (const auto& slave : snapshot.slaves) {
        std::cout << std::setw(8) << std::dec << slave.position << "  "
                  << std::setw(6) << (slave.online ? "yes" : "no") << "  "
                  << "0x" << std::hex << std::setw(8) << std::setfill('0') << slave.vendorId << std::setfill(' ')
                  << "  "
                  << "0x" << std::hex << std::setw(8) << std::setfill('0') << slave.productCode
                  << std::setfill(' ') << "  "
                  << "0x" << std::hex << std::setw(4) << std::setfill('0') << slave.escType
                  << std::setfill(' ') << "  "
                  << "0x" << std::hex << std::setw(4) << std::setfill('0') << slave.escRevision
                  << std::setfill(' ') << std::dec << "  "
                  << (slave.identityFromCoe ? "CoE-0x1018" : "n/a")
                  << '\n';
    }

    transport->close();
    return 0;
}
