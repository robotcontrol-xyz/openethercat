/**
 * @file el6692_structured_bridge_demo.cpp
 * @brief openEtherCAT source file.
 */

#include <array>
#include <algorithm>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <optional>
#include <string>
#include <vector>

#include "openethercat/master/ethercat_master.hpp"
#include "openethercat/transport/mock_transport.hpp"

namespace {

constexpr std::size_t kProcessBytes = 64;
constexpr std::size_t kBridgeTxOffset = 8;
constexpr std::size_t kBridgeRxOffset = 24;
constexpr std::size_t kFrameBytes = 22;
constexpr std::uint16_t kMagic = 0x6942;
constexpr std::uint8_t kVersion = 1;
constexpr std::uint8_t kFlagAck = 0x01;
constexpr std::size_t kPayloadBytes = 10;

std::uint16_t crc16Ccitt(const std::uint8_t* data, std::size_t len) {
    std::uint16_t crc = 0xFFFF;
    for (std::size_t i = 0; i < len; ++i) {
        crc ^= static_cast<std::uint16_t>(data[i]) << 8U;
        for (int bit = 0; bit < 8; ++bit) {
            if ((crc & 0x8000U) != 0U) {
                crc = static_cast<std::uint16_t>((crc << 1U) ^ 0x1021U);
            } else {
                crc = static_cast<std::uint16_t>(crc << 1U);
            }
        }
    }
    return crc;
}

struct BridgeFrame {
    std::uint16_t magic = kMagic;
    std::uint8_t version = kVersion;
    std::uint8_t flags = 0;
    std::uint16_t seq = 0;
    std::uint16_t ackSeq = 0;
    std::uint8_t cmd = 0;
    std::uint8_t payloadLen = 0;
    std::array<std::uint8_t, kPayloadBytes> payload{};
};

std::vector<std::uint8_t> serialize(const BridgeFrame& f) {
    std::vector<std::uint8_t> out(kFrameBytes, 0U);
    out[0] = static_cast<std::uint8_t>(f.magic & 0xFFU);
    out[1] = static_cast<std::uint8_t>((f.magic >> 8U) & 0xFFU);
    out[2] = f.version;
    out[3] = f.flags;
    out[4] = static_cast<std::uint8_t>(f.seq & 0xFFU);
    out[5] = static_cast<std::uint8_t>((f.seq >> 8U) & 0xFFU);
    out[6] = static_cast<std::uint8_t>(f.ackSeq & 0xFFU);
    out[7] = static_cast<std::uint8_t>((f.ackSeq >> 8U) & 0xFFU);
    out[8] = f.cmd;
    out[9] = f.payloadLen;
    std::memcpy(out.data() + 10, f.payload.data(), kPayloadBytes);
    const auto crc = crc16Ccitt(out.data(), kFrameBytes - 2);
    out[kFrameBytes - 2] = static_cast<std::uint8_t>(crc & 0xFFU);
    out[kFrameBytes - 1] = static_cast<std::uint8_t>((crc >> 8U) & 0xFFU);
    return out;
}

std::optional<BridgeFrame> parse(const std::vector<std::uint8_t>& in) {
    if (in.size() < kFrameBytes) {
        return std::nullopt;
    }
    const auto rxCrc = static_cast<std::uint16_t>(
        static_cast<std::uint16_t>(in[kFrameBytes - 2]) |
        (static_cast<std::uint16_t>(in[kFrameBytes - 1]) << 8U));
    const auto calc = crc16Ccitt(in.data(), kFrameBytes - 2);
    if (rxCrc != calc) {
        return std::nullopt;
    }

    BridgeFrame f;
    f.magic = static_cast<std::uint16_t>(static_cast<std::uint16_t>(in[0]) |
                                         (static_cast<std::uint16_t>(in[1]) << 8U));
    f.version = in[2];
    f.flags = in[3];
    f.seq = static_cast<std::uint16_t>(static_cast<std::uint16_t>(in[4]) |
                                       (static_cast<std::uint16_t>(in[5]) << 8U));
    f.ackSeq = static_cast<std::uint16_t>(static_cast<std::uint16_t>(in[6]) |
                                          (static_cast<std::uint16_t>(in[7]) << 8U));
    f.cmd = in[8];
    f.payloadLen = in[9];
    std::memcpy(f.payload.data(), in.data() + 10, kPayloadBytes);

    if (f.magic != kMagic || f.version != kVersion || f.payloadLen > kPayloadBytes) {
        return std::nullopt;
    }
    return f;
}

class BridgeEndpoint {
public:
    explicit BridgeEndpoint(std::string name) : name_(std::move(name)) {}

    void requestCommand(std::uint8_t cmd, const std::vector<std::uint8_t>& payload, int cycle,
                        int timeoutCycles = 3) {
        if (pending_.has_value()) {
            return;
        }
        Pending p;
        p.seq = nextSeq_++;
        p.cmd = cmd;
        p.payload = payload;
        p.deadlineCycle = cycle + timeoutCycles;
        p.timeoutCycles = timeoutCycles;
        p.retries = 0;
        pending_ = p;
    }

    BridgeFrame outbound(int cycle) {
        BridgeFrame frame;

        if (pending_.has_value()) {
            auto& p = *pending_;
            if (cycle > p.deadlineCycle) {
                if (p.retries < 2) {
                    ++p.retries;
                    p.deadlineCycle = cycle + p.timeoutCycles;
                    std::cout << name_ << " retry seq=" << p.seq << " retry=" << static_cast<int>(p.retries)
                              << '\n';
                } else {
                    std::cout << name_ << " timeout seq=" << p.seq << '\n';
                    pending_.reset();
                }
            }
        }

        if (ackToSend_ != 0U) {
            frame.flags |= kFlagAck;
            frame.ackSeq = ackToSend_;
            ackToSend_ = 0U;
            return frame;
        }

        if (pending_.has_value()) {
            const auto& p = *pending_;
            frame.seq = p.seq;
            frame.cmd = p.cmd;
            frame.payloadLen = static_cast<std::uint8_t>(std::min<std::size_t>(p.payload.size(), kPayloadBytes));
            std::copy(p.payload.begin(), p.payload.begin() + static_cast<std::ptrdiff_t>(frame.payloadLen),
                      frame.payload.begin());
        }
        return frame;
    }

    void onReceived(const BridgeFrame& f, int cycle) {
        (void)cycle;
        if ((f.flags & kFlagAck) != 0U && pending_.has_value() && f.ackSeq == pending_->seq) {
            std::cout << name_ << " acked seq=" << f.ackSeq << '\n';
            pending_.reset();
        }

        if (f.seq != 0U) {
            // Ack all valid command frames, including duplicates after retries.
            ackToSend_ = f.seq;
        }

        if (f.seq != 0U && f.seq != lastRxSeq_) {
            lastRxSeq_ = f.seq;
            std::cout << name_ << " rx cmd=" << static_cast<int>(f.cmd)
                      << " seq=" << f.seq
                      << " payloadLen=" << static_cast<int>(f.payloadLen) << '\n';
        }
    }

private:
    struct Pending {
        std::uint16_t seq = 0;
        std::uint8_t cmd = 0;
        std::vector<std::uint8_t> payload;
        int deadlineCycle = 0;
        int timeoutCycles = 3;
        std::uint8_t retries = 0;
    };

    std::string name_;
    std::uint16_t nextSeq_ = 1;
    std::uint16_t ackToSend_ = 0;
    std::uint16_t lastRxSeq_ = 0;
    std::optional<Pending> pending_;
};

class BridgeWire {
public:
    void transfer(int cycle, oec::MockTransport& a, oec::MockTransport& b) {
        const auto aTx = a.lastOutputs();
        const auto bTx = b.lastOutputs();

        std::vector<std::uint8_t> aToB(aTx.begin() + static_cast<std::ptrdiff_t>(kBridgeTxOffset),
                                       aTx.begin() + static_cast<std::ptrdiff_t>(kBridgeTxOffset + kFrameBytes));
        std::vector<std::uint8_t> bToA(bTx.begin() + static_cast<std::ptrdiff_t>(kBridgeTxOffset),
                                       bTx.begin() + static_cast<std::ptrdiff_t>(kBridgeTxOffset + kFrameBytes));

        // Simulate transient bridge loss on B->A for a window to force retry/timeout handling.
        if (cycle >= 2 && cycle <= 6) {
            bToA.assign(kFrameBytes, 0U);
        }

        a.setInputBytes(kBridgeRxOffset, bToA);
        b.setInputBytes(kBridgeRxOffset, aToB);
    }
};

} // namespace

int main() {
    oec::NetworkConfiguration cfgA;
    cfgA.processImageInputBytes = kProcessBytes;
    cfgA.processImageOutputBytes = kProcessBytes;
    cfgA.slaves = {
        {.name = "EK1100_A", .alias = 0, .position = 0, .vendorId = 0x2, .productCode = 0x044c2c52},
        {.name = "EL6692_A", .alias = 0, .position = 1, .vendorId = 0x2, .productCode = 0x1a243052},
    };
    cfgA.signals = {
        {.logicalName = "BridgeAliveA", .direction = oec::SignalDirection::Input, .slaveName = "EL6692_A", .byteOffset = 0, .bitOffset = 0},
    };

    oec::NetworkConfiguration cfgB = cfgA;
    cfgB.slaves[0].name = "EK1100_B";
    cfgB.slaves[1].name = "EL6692_B";
    cfgB.signals[0].logicalName = "BridgeAliveB";
    cfgB.signals[0].slaveName = "EL6692_B";

    oec::MockTransport ta(kProcessBytes, kProcessBytes);
    oec::MockTransport tb(kProcessBytes, kProcessBytes);
    oec::EthercatMaster ma(ta);
    oec::EthercatMaster mb(tb);

    if (!ma.configure(cfgA) || !mb.configure(cfgB) || !ma.start() || !mb.start()) {
        std::cerr << "startup failed\n";
        return 1;
    }

    BridgeEndpoint epA("A");
    BridgeEndpoint epB("B");
    BridgeWire wire;

    std::cout << "EL6692 structured bridge demo running\n";
    for (int cycle = 1; cycle <= 16; ++cycle) {
        if (cycle == 2) {
            epA.requestCommand(0x31, {0x10, 0x20, 0x30, 0x40}, cycle);
        }

        const auto outA = serialize(epA.outbound(cycle));
        const auto outB = serialize(epB.outbound(cycle));

        ma.writeOutputBytes(kBridgeTxOffset, outA);
        mb.writeOutputBytes(kBridgeTxOffset, outB);

        ma.runCycle();
        mb.runCycle();

        wire.transfer(cycle, ta, tb);

        ma.runCycle();
        mb.runCycle();

        std::vector<std::uint8_t> inA;
        std::vector<std::uint8_t> inB;
        ma.readInputBytes(kBridgeRxOffset, kFrameBytes, inA);
        mb.readInputBytes(kBridgeRxOffset, kFrameBytes, inB);

        if (const auto fA = parse(inA)) {
            epA.onReceived(*fA, cycle);
        }
        if (const auto fB = parse(inB)) {
            epB.onReceived(*fB, cycle);
        }
    }

    ma.stop();
    mb.stop();
    return 0;
}
