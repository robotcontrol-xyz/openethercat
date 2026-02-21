/**
 * @file el6692_bridge_demo.cpp
 * @brief openEtherCAT source file.
 */

#include <cstdint>
#include <cstring>
#include <iostream>
#include <vector>

#include "openethercat/master/ethercat_master.hpp"
#include "openethercat/transport/mock_transport.hpp"

namespace {

constexpr std::size_t kProcessBytes = 32;
constexpr std::size_t kBridgeTxOffset = 8;
constexpr std::size_t kBridgeRxOffset = 16;
constexpr std::size_t kBridgePayloadBytes = 8;

std::vector<std::uint8_t> packBridgePayload(std::uint16_t seq, std::int32_t value, std::uint8_t flags) {
    std::vector<std::uint8_t> out(kBridgePayloadBytes, 0U);
    out[0] = static_cast<std::uint8_t>(seq & 0xFFU);
    out[1] = static_cast<std::uint8_t>((seq >> 8U) & 0xFFU);
    out[2] = static_cast<std::uint8_t>(value & 0xFF);
    out[3] = static_cast<std::uint8_t>((value >> 8) & 0xFF);
    out[4] = static_cast<std::uint8_t>((value >> 16) & 0xFF);
    out[5] = static_cast<std::uint8_t>((value >> 24) & 0xFF);
    out[6] = flags;
    out[7] = 0U;
    return out;
}

struct BridgeData {
    std::uint16_t seq = 0;
    std::int32_t value = 0;
    std::uint8_t flags = 0;
};

BridgeData unpackBridgePayload(const std::vector<std::uint8_t>& in) {
    BridgeData data;
    if (in.size() < kBridgePayloadBytes) {
        return data;
    }
    data.seq = static_cast<std::uint16_t>(static_cast<std::uint16_t>(in[0]) |
                                          (static_cast<std::uint16_t>(in[1]) << 8U));
    data.value = static_cast<std::int32_t>(
        static_cast<std::uint32_t>(in[2]) |
        (static_cast<std::uint32_t>(in[3]) << 8U) |
        (static_cast<std::uint32_t>(in[4]) << 16U) |
        (static_cast<std::uint32_t>(in[5]) << 24U));
    data.flags = in[6];
    return data;
}

class El6692BridgeSimulator {
public:
    void transfer(oec::MockTransport& sideA, oec::MockTransport& sideB) {
        const auto aTx = sideA.lastOutputs();
        const auto bTx = sideB.lastOutputs();

        std::vector<std::uint8_t> aToB(aTx.begin() + static_cast<std::ptrdiff_t>(kBridgeTxOffset),
                                       aTx.begin() + static_cast<std::ptrdiff_t>(kBridgeTxOffset + kBridgePayloadBytes));
        std::vector<std::uint8_t> bToA(bTx.begin() + static_cast<std::ptrdiff_t>(kBridgeTxOffset),
                                       bTx.begin() + static_cast<std::ptrdiff_t>(kBridgeTxOffset + kBridgePayloadBytes));

        sideA.setInputBytes(kBridgeRxOffset, bToA);
        sideB.setInputBytes(kBridgeRxOffset, aToB);
    }
};

} // namespace

int main() {
    // Two EtherCAT strands connected through EL6692 bridge terminals.
    oec::NetworkConfiguration strandA;
    strandA.processImageInputBytes = kProcessBytes;
    strandA.processImageOutputBytes = kProcessBytes;
    strandA.slaves = {
        {.name = "EK1100_A", .alias = 0, .position = 0, .vendorId = 0x2, .productCode = 0x044c2c52},
        {.name = "EL6692_A", .alias = 0, .position = 1, .vendorId = 0x2, .productCode = 0x1a243052},
    };
    strandA.signals = {
        {.logicalName = "BridgeAliveA", .direction = oec::SignalDirection::Input, .slaveName = "EL6692_A", .byteOffset = 0, .bitOffset = 0},
    };

    oec::NetworkConfiguration strandB = strandA;
    strandB.slaves[0].name = "EK1100_B";
    strandB.slaves[1].name = "EL6692_B";
    strandB.signals[0].logicalName = "BridgeAliveB";
    strandB.signals[0].slaveName = "EL6692_B";

    oec::MockTransport transportA(kProcessBytes, kProcessBytes);
    oec::MockTransport transportB(kProcessBytes, kProcessBytes);
    oec::EthercatMaster masterA(transportA);
    oec::EthercatMaster masterB(transportB);

    if (!masterA.configure(strandA) || !masterB.configure(strandB) ||
        !masterA.start() || !masterB.start()) {
        std::cerr << "Startup failed\n";
        return 1;
    }

    El6692BridgeSimulator bridge;

    // Prime bridge traffic.
    const auto initPayload = packBridgePayload(0, 0, 0x01);
    masterA.writeOutputBytes(kBridgeTxOffset, initPayload);
    masterB.writeOutputBytes(kBridgeTxOffset, initPayload);
    masterA.runCycle();
    masterB.runCycle();
    bridge.transfer(transportA, transportB);

    std::cout << "EL6692 bridge demo running\n";
    for (std::uint16_t cycle = 1; cycle <= 12; ++cycle) {
        const auto aPayload = packBridgePayload(cycle, static_cast<std::int32_t>(cycle * 100), 0xA1);
        const auto bPayload = packBridgePayload(cycle, static_cast<std::int32_t>(-static_cast<int>(cycle) * 50), 0xB2);

        masterA.writeOutputBytes(kBridgeTxOffset, aPayload);
        masterB.writeOutputBytes(kBridgeTxOffset, bPayload);

        if (!masterA.runCycle() || !masterB.runCycle()) {
            std::cerr << "Cycle failed\n";
            return 1;
        }

        bridge.transfer(transportA, transportB);

        // Next cycle reads bridged data into input image.
        masterA.runCycle();
        masterB.runCycle();

        std::vector<std::uint8_t> aRx;
        std::vector<std::uint8_t> bRx;
        masterA.readInputBytes(kBridgeRxOffset, kBridgePayloadBytes, aRx);
        masterB.readInputBytes(kBridgeRxOffset, kBridgePayloadBytes, bRx);

        const auto fromB = unpackBridgePayload(aRx);
        const auto fromA = unpackBridgePayload(bRx);

        std::cout << "cycle=" << cycle
                  << " A<-B{seq=" << fromB.seq << ", value=" << fromB.value
                  << ", flags=0x" << std::hex << static_cast<int>(fromB.flags) << std::dec << "}"
                  << " B<-A{seq=" << fromA.seq << ", value=" << fromA.value
                  << ", flags=0x" << std::hex << static_cast<int>(fromA.flags) << std::dec << "}"
                  << '\n';
    }

    masterA.stop();
    masterB.stop();
    return 0;
}
