#include <array>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <optional>
#include <vector>

#include "openethercat/master/ethercat_master.hpp"
#include "openethercat/transport/mock_transport.hpp"

namespace {

constexpr std::size_t kProcessBytes = 64;
constexpr std::size_t kCanTxOffset = 0;
constexpr std::size_t kCanRxOffset = 16;
constexpr std::size_t kCanCtrlOffset = 32;
constexpr std::size_t kCanStatusOffset = 33;

struct CanFrame {
    std::uint32_t cobId = 0;
    std::uint8_t dlc = 0;
    std::array<std::uint8_t, 8> data{};
    bool extended = false;
    bool rtr = false;
};

std::vector<std::uint8_t> packCanFrame(const CanFrame& f) {
    std::vector<std::uint8_t> out(13, 0U);
    out[0] = static_cast<std::uint8_t>(f.cobId & 0xFFU);
    out[1] = static_cast<std::uint8_t>((f.cobId >> 8U) & 0xFFU);
    out[2] = static_cast<std::uint8_t>((f.cobId >> 16U) & 0xFFU);
    out[3] = static_cast<std::uint8_t>((f.cobId >> 24U) & 0x1FU);
    out[4] = static_cast<std::uint8_t>((f.dlc & 0x0FU) |
                                       (f.extended ? 0x40U : 0U) |
                                       (f.rtr ? 0x80U : 0U));
    std::memcpy(out.data() + 5, f.data.data(), 8);
    return out;
}

std::optional<CanFrame> unpackCanFrame(const std::vector<std::uint8_t>& in) {
    if (in.size() < 13) {
        return std::nullopt;
    }
    CanFrame f;
    f.cobId = static_cast<std::uint32_t>(in[0]) |
              (static_cast<std::uint32_t>(in[1]) << 8U) |
              (static_cast<std::uint32_t>(in[2]) << 16U) |
              ((static_cast<std::uint32_t>(in[3]) & 0x1FU) << 24U);
    f.dlc = static_cast<std::uint8_t>(in[4] & 0x0FU);
    f.extended = (in[4] & 0x40U) != 0U;
    f.rtr = (in[4] & 0x80U) != 0U;
    std::memcpy(f.data.data(), in.data() + 5, 8);
    return f;
}

class El6751CanBridgeSimulator {
public:
    void transfer(oec::MockTransport& terminalSide, oec::MockTransport& canBusSide) {
        const auto txOut = terminalSide.lastOutputs();
        std::vector<std::uint8_t> txFrame(txOut.begin() + static_cast<std::ptrdiff_t>(kCanTxOffset),
                                          txOut.begin() + static_cast<std::ptrdiff_t>(kCanTxOffset + 13));

        // Simulate EL6751 forwarding TX frame to CAN bus and placing response into RX area.
        canBusSide.setInputBytes(kCanRxOffset, txFrame);

        const auto busOut = canBusSide.lastOutputs();
        std::vector<std::uint8_t> rxFrame(busOut.begin() + static_cast<std::ptrdiff_t>(kCanTxOffset),
                                          busOut.begin() + static_cast<std::ptrdiff_t>(kCanTxOffset + 13));

        terminalSide.setInputBytes(kCanRxOffset, rxFrame);

        // Set status bit0 = CAN ready, bit1 = TX done.
        terminalSide.setInputByte(kCanStatusOffset, 0x03);
    }
};

} // namespace

int main() {
    // Simplified process-image layout for EL6751-like operation.
    // TX frame @ [0..12], RX frame @ [16..28], control @ [32], status @ [33].
    oec::NetworkConfiguration cfgTerminal;
    cfgTerminal.processImageInputBytes = kProcessBytes;
    cfgTerminal.processImageOutputBytes = kProcessBytes;
    cfgTerminal.slaves = {
        {.name = "EK1100", .alias = 0, .position = 0, .vendorId = 0x2, .productCode = 0x044c2c52},
        {.name = "EL6751", .alias = 0, .position = 1, .vendorId = 0x2, .productCode = 0x1a6f3052},
    };
    cfgTerminal.signals = {
        {.logicalName = "CanReady", .direction = oec::SignalDirection::Input, .slaveName = "EL6751", .byteOffset = kCanStatusOffset, .bitOffset = 0},
        {.logicalName = "CanTxDone", .direction = oec::SignalDirection::Input, .slaveName = "EL6751", .byteOffset = kCanStatusOffset, .bitOffset = 1},
    };

    // Peer side simulates external CAN bus participant in the demo.
    oec::NetworkConfiguration cfgBus = cfgTerminal;
    cfgBus.slaves[1].name = "CAN_BUS_SIM";

    oec::MockTransport tTerminal(kProcessBytes, kProcessBytes);
    oec::MockTransport tBus(kProcessBytes, kProcessBytes);
    oec::EthercatMaster mTerminal(tTerminal);
    oec::EthercatMaster mBus(tBus);

    if (!mTerminal.configure(cfgTerminal) || !mBus.configure(cfgBus) || !mTerminal.start() || !mBus.start()) {
        std::cerr << "startup failed\n";
        return 1;
    }

    El6751CanBridgeSimulator sim;

    std::cout << "EL6751 CAN bridge demo running\n";
    for (int cycle = 1; cycle <= 8; ++cycle) {
        CanFrame outgoing;
        outgoing.cobId = 0x180U + static_cast<std::uint32_t>(cycle);
        outgoing.dlc = 8;
        outgoing.data = {
            static_cast<std::uint8_t>(cycle),
            static_cast<std::uint8_t>(cycle + 1),
            static_cast<std::uint8_t>(cycle + 2),
            static_cast<std::uint8_t>(cycle + 3),
            static_cast<std::uint8_t>(cycle + 4),
            static_cast<std::uint8_t>(cycle + 5),
            static_cast<std::uint8_t>(cycle + 6),
            static_cast<std::uint8_t>(cycle + 7),
        };

        const auto txBytes = packCanFrame(outgoing);
        mTerminal.writeOutputBytes(kCanTxOffset, txBytes);
        mBus.writeOutputBytes(kCanTxOffset, txBytes);

        mTerminal.runCycle();
        mBus.runCycle();

        sim.transfer(tTerminal, tBus);

        mTerminal.runCycle();

        std::vector<std::uint8_t> rxBytes;
        mTerminal.readInputBytes(kCanRxOffset, 13, rxBytes);
        const auto rx = unpackCanFrame(rxBytes);

        bool canReady = false;
        bool txDone = false;
        mTerminal.getInputByName("CanReady", canReady);
        mTerminal.getInputByName("CanTxDone", txDone);

        if (rx.has_value()) {
            std::cout << "cycle=" << cycle
                      << " rx_cobid=0x" << std::hex << rx->cobId << std::dec
                      << " dlc=" << static_cast<int>(rx->dlc)
                      << " data0=" << static_cast<int>(rx->data[0])
                      << " ready=" << (canReady ? 1 : 0)
                      << " txDone=" << (txDone ? 1 : 0)
                      << '\n';
        }
    }

    mTerminal.stop();
    mBus.stop();
    return 0;
}
