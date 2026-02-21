#include "openethercat/transport/ethercat_frame.hpp"

namespace oec {
namespace {

constexpr std::uint16_t kEtherTypeEthercat = 0x88A4;
constexpr std::uint8_t kCommandLrw = 0x0C;
constexpr std::size_t kEthernetHeaderBytes = 14;
constexpr std::size_t kEthercatHeaderBytes = 2;
constexpr std::size_t kDatagramHeaderBytes = 10;
constexpr std::size_t kFrameMinBytes = kEthernetHeaderBytes + kEthercatHeaderBytes + kDatagramHeaderBytes + 2;

void put16be(std::vector<std::uint8_t>& out, std::uint16_t value) {
    out.push_back(static_cast<std::uint8_t>((value >> 8U) & 0xFFU));
    out.push_back(static_cast<std::uint8_t>(value & 0xFFU));
}

void put16le(std::vector<std::uint8_t>& out, std::uint16_t value) {
    out.push_back(static_cast<std::uint8_t>(value & 0xFFU));
    out.push_back(static_cast<std::uint8_t>((value >> 8U) & 0xFFU));
}

std::uint16_t get16be(const std::vector<std::uint8_t>& in, std::size_t offset) {
    return static_cast<std::uint16_t>((static_cast<std::uint16_t>(in[offset]) << 8U) |
                                      static_cast<std::uint16_t>(in[offset + 1]));
}

std::uint16_t get16le(const std::vector<std::uint8_t>& in, std::size_t offset) {
    return static_cast<std::uint16_t>((static_cast<std::uint16_t>(in[offset + 1]) << 8U) |
                                      static_cast<std::uint16_t>(in[offset]));
}

} // namespace

std::vector<std::uint8_t> EthercatFrameCodec::buildLrwFrame(
    const std::uint8_t destinationMac[6],
    const std::uint8_t sourceMac[6],
    const EthercatLrwRequest& request) {
    EthercatDatagramRequest datagram;
    datagram.command = kCommandLrw;
    datagram.datagramIndex = request.datagramIndex;
    datagram.adp = static_cast<std::uint16_t>(request.logicalAddress & 0xFFFFU);
    datagram.ado = static_cast<std::uint16_t>((request.logicalAddress >> 16U) & 0xFFFFU);
    datagram.payload = request.payload;
    return buildDatagramFrame(destinationMac, sourceMac, datagram);
}

std::optional<EthercatLrwResponse> EthercatFrameCodec::parseLrwFrame(
    const std::vector<std::uint8_t>& ethernetFrame,
    std::uint8_t expectedDatagramIndex,
    std::size_t expectedPayloadBytes) {
    const auto datagram = parseDatagramFrame(ethernetFrame, kCommandLrw, expectedDatagramIndex,
                                             expectedPayloadBytes);
    if (!datagram) {
        return std::nullopt;
    }

    EthercatLrwResponse response;
    response.datagramIndex = datagram->datagramIndex;
    response.payload = datagram->payload;
    response.workingCounter = datagram->workingCounter;
    return response;
}

std::vector<std::uint8_t> EthercatFrameCodec::buildDatagramFrame(
    const std::uint8_t destinationMac[6],
    const std::uint8_t sourceMac[6],
    const EthercatDatagramRequest& request) {
    std::vector<std::uint8_t> frame;
    frame.reserve(kFrameMinBytes + request.payload.size());

    frame.insert(frame.end(), destinationMac, destinationMac + 6);
    frame.insert(frame.end(), sourceMac, sourceMac + 6);
    put16be(frame, kEtherTypeEthercat);

    const auto datagramBytes = static_cast<std::uint16_t>(kDatagramHeaderBytes + request.payload.size() + 2);
    const auto ethercatLengthField = static_cast<std::uint16_t>(datagramBytes | 0x1000U);
    put16le(frame, ethercatLengthField);

    frame.push_back(request.command);
    frame.push_back(request.datagramIndex);
    put16le(frame, request.adp);
    put16le(frame, request.ado);

    const auto lenField = static_cast<std::uint16_t>(request.payload.size() & 0x07FFU);
    put16le(frame, lenField);
    put16le(frame, 0U); // IRQ

    frame.insert(frame.end(), request.payload.begin(), request.payload.end());
    put16le(frame, 0U); // Placeholder WKC in request.
    return frame;
}

std::optional<EthercatDatagramResponse> EthercatFrameCodec::parseDatagramFrame(
    const std::vector<std::uint8_t>& ethernetFrame,
    std::uint8_t expectedCommand,
    std::uint8_t expectedDatagramIndex,
    std::size_t expectedPayloadBytes) {
    if (ethernetFrame.size() < kFrameMinBytes) {
        return std::nullopt;
    }

    const auto etherType = get16be(ethernetFrame, 12);
    if (etherType != kEtherTypeEthercat) {
        return std::nullopt;
    }

    const auto ethercatHeader = get16le(ethernetFrame, 14);
    const auto ethercatLength = static_cast<std::size_t>(ethercatHeader & 0x07FFU);
    if (ethercatLength + kEthernetHeaderBytes > ethernetFrame.size()) {
        return std::nullopt;
    }

    const auto command = ethernetFrame[16];
    const auto index = ethernetFrame[17];
    if (command != expectedCommand || index != expectedDatagramIndex) {
        return std::nullopt;
    }

    const auto lenField = get16le(ethernetFrame, 22);
    const auto payloadSize = static_cast<std::size_t>(lenField & 0x07FFU);
    if (payloadSize != expectedPayloadBytes) {
        return std::nullopt;
    }

    const auto payloadOffset = static_cast<std::size_t>(24 + 2); // start + irq bytes
    const auto wkcOffset = payloadOffset + payloadSize;
    if (wkcOffset + 2 > ethernetFrame.size()) {
        return std::nullopt;
    }

    EthercatDatagramResponse response;
    response.command = command;
    response.datagramIndex = index;
    response.payload.assign(ethernetFrame.begin() + static_cast<std::ptrdiff_t>(payloadOffset),
                            ethernetFrame.begin() + static_cast<std::ptrdiff_t>(wkcOffset));
    response.workingCounter = get16le(ethernetFrame, wkcOffset);
    return response;
}

} // namespace oec
