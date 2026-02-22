/**
 * @file topology_to_eni_dump.cpp
 * @brief Discover live topology and generate a starter ENI XML file.
 */

#include <algorithm>
#include <cstdint>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <string>
#include <unordered_map>
#include <vector>

#include "openethercat/master/topology_manager.hpp"
#include "openethercat/transport/transport_factory.hpp"

namespace {

struct MappingRule {
    std::string deviceName;
    int inputChannels = 0;
    int outputChannels = 0;
};

const std::unordered_map<std::uint32_t, MappingRule> kKnownRules = {
    {0x044c2c52U, {"EK1100", 0, 0}},
    {0x03ec3052U, {"EL1004", 4, 0}},
    {0x03f03052U, {"EL1008", 8, 0}},
    {0x03f43052U, {"EL1012", 2, 0}},
    {0x03f63052U, {"EL1014", 4, 0}},
    {0x03fa3052U, {"EL1018", 8, 0}},
    {0x07103052U, {"EL1808", 8, 0}},
    {0x07113052U, {"EL1809", 16, 0}},
    {0x07d43052U, {"EL2004", 0, 4}},
    {0x07d83052U, {"EL2008", 0, 8}},
    {0x07e63052U, {"EL2022", 0, 2}},
    {0x07e83052U, {"EL2024", 0, 4}},
    {0x07ec3052U, {"EL2028", 0, 8}},
    {0x0af83052U, {"EL2808", 0, 8}},
    {0x1a243052U, {"EL6692", 0, 0}},
    {0x1a6f3052U, {"EL6751", 0, 0}},
};

struct SignalSpec {
    std::string logicalName;
    std::string direction;
    std::string slaveName;
    std::size_t byteOffset = 0;
    std::uint8_t bitOffset = 0;
};

std::string hex32(std::uint32_t value) {
    std::ostringstream oss;
    oss << "0x" << std::hex << std::nouppercase << std::setw(8) << std::setfill('0') << value;
    return oss.str();
}

std::string defaultSlaveName(std::uint16_t position, std::uint32_t productCode) {
    const auto it = kKnownRules.find(productCode);
    if (it != kKnownRules.end()) {
        return it->second.deviceName + "_P" + std::to_string(position);
    }
    return "Slave_P" + std::to_string(position) + "_" + hex32(productCode);
}

void printUsage(const char* argv0) {
    std::cerr << "Usage: " << argv0
              << " <transport-spec> [output-eni] [fallback-input-bytes] [fallback-output-bytes]\n"
              << "  transport-spec: linux:<ifname> | linux:<ifname_primary>,<ifname_secondary> | mock\n"
              << "Example:\n"
              << "  sudo " << argv0 << " linux:enp2s0 generated.eni.xml 1 1\n";
}

} // namespace

int main(int argc, char** argv) {
    if (argc < 2) {
        printUsage(argv[0]);
        return 1;
    }

    const std::string transportSpec = argv[1];
    const std::string outputPath = (argc > 2) ? argv[2] : "generated_discovery.eni.xml";
    const std::size_t fallbackInputBytes = (argc > 3) ? static_cast<std::size_t>(std::stoul(argv[3])) : 1U;
    const std::size_t fallbackOutputBytes = (argc > 4) ? static_cast<std::size_t>(std::stoul(argv[4])) : 1U;

    std::string error;
    oec::TransportFactoryConfig transportConfig;
    if (!oec::TransportFactory::parseTransportSpec(transportSpec, transportConfig, error)) {
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
    transport->close();

    if (snapshot.slaves.empty()) {
        std::cerr << "No slaves discovered; ENI not generated.\n";
        return 1;
    }

    std::vector<oec::TopologySlaveInfo> discovered = snapshot.slaves;
    std::sort(discovered.begin(), discovered.end(),
              [](const auto& a, const auto& b) { return a.position < b.position; });

    std::vector<SignalSpec> signals;
    std::size_t inputBitCursor = 0U;
    std::size_t outputBitCursor = 0U;

    std::ostringstream xml;
    xml << "<EniConfiguration>\n";

    for (const auto& slave : discovered) {
        const auto slaveName = defaultSlaveName(slave.position, slave.productCode);
        const auto it = kKnownRules.find(slave.productCode);
        if (it == kKnownRules.end()) {
            continue;
        }
        const auto& rule = it->second;
        for (int ch = 0; ch < rule.inputChannels; ++ch) {
            SignalSpec s;
            s.logicalName = slaveName + "_In" + std::to_string(ch + 1);
            s.direction = "input";
            s.slaveName = slaveName;
            s.byteOffset = inputBitCursor / 8U;
            s.bitOffset = static_cast<std::uint8_t>(inputBitCursor % 8U);
            signals.push_back(s);
            ++inputBitCursor;
        }
        for (int ch = 0; ch < rule.outputChannels; ++ch) {
            SignalSpec s;
            s.logicalName = slaveName + "_Out" + std::to_string(ch + 1);
            s.direction = "output";
            s.slaveName = slaveName;
            s.byteOffset = outputBitCursor / 8U;
            s.bitOffset = static_cast<std::uint8_t>(outputBitCursor % 8U);
            signals.push_back(s);
            ++outputBitCursor;
        }
    }

    std::size_t inputBytes = std::max<std::size_t>(fallbackInputBytes, (inputBitCursor + 7U) / 8U);
    std::size_t outputBytes = std::max<std::size_t>(fallbackOutputBytes, (outputBitCursor + 7U) / 8U);

    // Keep generated ENI loader-friendly even if no known PDO-capable terminals are recognized.
    if (signals.empty()) {
        SignalSpec placeholder;
        placeholder.logicalName = "PlaceholderInput";
        placeholder.direction = "input";
        placeholder.slaveName = defaultSlaveName(discovered.front().position, discovered.front().productCode);
        placeholder.byteOffset = 0U;
        placeholder.bitOffset = 0U;
        signals.push_back(placeholder);
        inputBytes = std::max<std::size_t>(inputBytes, 1U);
        std::cerr << "Warning: no known signal rules matched discovered devices; wrote placeholder signal.\n";
    }

    xml << "  <ProcessImage inputBytes=\"" << inputBytes
        << "\" outputBytes=\"" << outputBytes << "\"/>\n";

    for (const auto& slave : discovered) {
        xml << "  <Slave"
            << " name=\"" << defaultSlaveName(slave.position, slave.productCode) << "\""
            << " alias=\"0\""
            << " position=\"" << slave.position << "\""
            << " vendorId=\"" << hex32(slave.vendorId) << "\""
            << " productCode=\"" << hex32(slave.productCode) << "\"/>\n";
    }

    for (const auto& s : signals) {
        xml << "  <Signal"
            << " logicalName=\"" << s.logicalName << "\""
            << " direction=\"" << s.direction << "\""
            << " slaveName=\"" << s.slaveName << "\""
            << " byteOffset=\"" << s.byteOffset << "\""
            << " bitOffset=\"" << static_cast<int>(s.bitOffset) << "\"/>\n";
    }

    xml << "</EniConfiguration>\n";

    std::ofstream out(outputPath);
    if (!out) {
        std::cerr << "Failed to open output file: " << outputPath << '\n';
        return 1;
    }
    out << xml.str();
    out.close();

    std::cout << "Discovered " << discovered.size() << " slave(s), redundancy_healthy="
              << (snapshot.redundancyHealthy ? "true" : "false") << '\n';
    std::cout << "Generated ENI: " << outputPath << '\n';
    std::cout << "ProcessImage inputBytes=" << inputBytes
              << " outputBytes=" << outputBytes
              << " signals=" << signals.size() << '\n';
    return 0;
}

