/**
 * @file foe_eoe_smoke_demo.cpp
 * @brief Basic FoE/EoE API smoke tool for mock and Linux transports.
 */

#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <memory>
#include <string>
#include <vector>

#include "openethercat/master/ethercat_master.hpp"
#include "openethercat/transport/transport_factory.hpp"

namespace {

void usage(const char* argv0) {
    std::cout << "Usage: " << argv0 << " <transport-spec> [slave-pos]\n"
              << "  transport-spec: mock | linux:<ifname> | linux:<if_primary>,<if_secondary>\n"
              << "Example:\n"
              << "  " << argv0 << " mock 1\n";
}

} // namespace

int main(int argc, char** argv) {
    if (argc < 2) {
        usage(argv[0]);
        return 1;
    }

    const std::string transportSpec = argv[1];
    const std::uint16_t slavePosition = (argc > 2) ? static_cast<std::uint16_t>(std::stoul(argv[2])) : 1U;

    std::string error;
    oec::TransportFactoryConfig tc;
    if (!oec::TransportFactory::parseTransportSpec(transportSpec, tc, error)) {
        std::cerr << "Invalid transport spec: " << error << '\n';
        return 1;
    }
    auto transport = oec::TransportFactory::create(tc, error);
    if (!transport) {
        std::cerr << "Transport creation failed: " << error << '\n';
        return 1;
    }
    if (!transport->open()) {
        std::cerr << "Transport open failed: " << transport->lastError() << '\n';
        return 1;
    }

    oec::FoERequest request;
    request.fileName = "oec_smoke.bin";
    request.password = 0U;
    request.maxChunkBytes = 256U;

    // FoE write/read smoke.
    std::vector<std::uint8_t> payload{0x4f, 0x45, 0x43, 0x21, 0x00, 0x01, 0x02, 0x03};
    if (!transport->foeWrite(slavePosition, request, payload, error)) {
        std::cout << "foe_write=0 error=\"" << error << "\"\n";
    } else {
        std::cout << "foe_write=1 bytes=" << payload.size() << '\n';
    }

    oec::FoEResponse readResponse;
    if (!transport->foeRead(slavePosition, request, readResponse, error)) {
        const auto& readError = readResponse.error.empty() ? error : readResponse.error;
        std::cout << "foe_read=0 error=\"" << readError << "\"\n";
    } else {
        std::cout << "foe_read=1 bytes=" << readResponse.data.size() << '\n';
    }

    // EoE send/receive smoke.
    std::vector<std::uint8_t> eoeTx{
        0xff, 0xff, 0xff, 0xff, 0xff, 0xff, // dst
        0x02, 0x00, 0x00, 0x00, 0x00, 0x01, // src
        0x08, 0x00,                         // ethertype (IPv4)
        0xde, 0xad, 0xbe, 0xef
    };
    if (!transport->eoeSend(slavePosition, eoeTx, error)) {
        std::cout << "eoe_send=0 error=\"" << error << "\"\n";
    } else {
        std::cout << "eoe_send=1 bytes=" << eoeTx.size() << '\n';
    }

    std::vector<std::uint8_t> eoeRx;
    if (!transport->eoeReceive(slavePosition, eoeRx, error)) {
        std::cout << "eoe_recv=0 error=\"" << error << "\"\n";
    } else {
        std::cout << "eoe_recv=1 bytes=" << eoeRx.size() << '\n';
    }

    transport->close();
    return 0;
}

