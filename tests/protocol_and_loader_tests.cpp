#include <cassert>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <vector>

#include "openethercat/config/config_loader.hpp"
#include "openethercat/transport/ethercat_frame.hpp"

namespace {

void testEthercatCodec() {
    const std::uint8_t dst[6] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};
    const std::uint8_t src[6] = {0x10, 0x11, 0x12, 0x13, 0x14, 0x15};

    oec::EthercatLrwRequest request;
    request.datagramIndex = 0x42;
    request.logicalAddress = 0;
    request.payload = {0xAA, 0x55};

    auto frame = oec::EthercatFrameCodec::buildLrwFrame(dst, src, request);

    // Simulate slave-updated payload and WKC in response frame.
    frame[26] = 0x01;
    frame[27] = 0x00;
    frame[28] = 0x02;
    frame[29] = 0x00;

    auto parsed = oec::EthercatFrameCodec::parseLrwFrame(frame, 0x42, 2);
    assert(parsed.has_value());
    assert(parsed->workingCounter == 2);
    assert(parsed->payload.size() == 2);
    assert(parsed->payload[0] == 0x01);
    assert(parsed->payload[1] == 0x00);

    auto wrongIndex = oec::EthercatFrameCodec::parseLrwFrame(frame, 0x41, 2);
    assert(!wrongIndex.has_value());

    oec::EthercatDatagramRequest datagram;
    datagram.command = 0x08;
    datagram.datagramIndex = 0x11;
    datagram.adp = 0x0000;
    datagram.ado = 0x0120;
    datagram.payload = {0x08, 0x00};
    auto dframe = oec::EthercatFrameCodec::buildDatagramFrame(dst, src, datagram);
    dframe[28] = 0x01;
    dframe[29] = 0x00;

    auto parsedDatagram = oec::EthercatFrameCodec::parseDatagramFrame(dframe, 0x08, 0x11, 2);
    assert(parsedDatagram.has_value());
    assert(parsedDatagram->workingCounter == 1U);
    assert(parsedDatagram->payload[0] == 0x08);
}

void testConfigLoader() {
    namespace fs = std::filesystem;
    const auto base = fs::temp_directory_path() / "oec_loader_test";
    fs::create_directories(base);

    const auto eniPath = base / "demo.eni.xml";
    const auto esiPath = base / "devices.xml";

    {
        std::ofstream eni(eniPath);
        eni << "<Network>"
            << "<ProcessImage inputBytes=\"1\" outputBytes=\"1\"/>"
            << "<Slave name=\"EL1008\" alias=\"0\" position=\"1\"/>"
            << "<Slave name=\"EL2008\" alias=\"0\" position=\"2\"/>"
            << "<Signal logicalName=\"StartButton\" direction=\"input\" slaveName=\"EL1008\" byteOffset=\"0\" bitOffset=\"0\"/>"
            << "<Signal logicalName=\"LampGreen\" direction=\"output\" slaveName=\"EL2008\" byteOffset=\"0\" bitOffset=\"0\"/>"
            << "</Network>";
    }

    {
        std::ofstream esi(esiPath);
        esi << "<Catalog>"
            << "<Device name=\"EL1008\" vendorId=\"0x00000002\" productCode=\"0x03f03052\"/>"
            << "<Device name=\"EL2008\" vendorId=\"0x00000002\" productCode=\"0x07d83052\"/>"
            << "</Catalog>";
    }

    oec::NetworkConfiguration config;
    std::string error;
    const bool ok = oec::ConfigurationLoader::loadFromEniAndEsiDirectory(
        eniPath.string(), base.string(), config, error);

    assert(ok);
    assert(error.empty());
    assert(config.processImageInputBytes == 1);
    assert(config.processImageOutputBytes == 1);
    assert(config.slaves.size() == 2);
    assert(config.slaves[0].vendorId == 0x00000002);
    assert(config.slaves[1].productCode == 0x07d83052);
    assert(config.signals.size() == 2);

    fs::remove_all(base);
}

} // namespace

int main() {
    testEthercatCodec();
    testConfigLoader();
    std::cout << "protocol_and_loader_tests passed\n";
    return 0;
}
