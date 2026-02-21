/**
 * @file ethercat_frame.hpp
 * @brief openEtherCAT source file.
 */

#pragma once

#include <cstdint>
#include <optional>
#include <vector>

namespace oec {

struct EthercatLrwRequest {
    std::uint8_t datagramIndex = 0;
    std::uint32_t logicalAddress = 0;
    std::vector<std::uint8_t> payload;
};

struct EthercatLrwResponse {
    std::uint8_t datagramIndex = 0;
    std::vector<std::uint8_t> payload;
    std::uint16_t workingCounter = 0;
};

struct EthercatDatagramRequest {
    std::uint8_t command = 0;
    std::uint8_t datagramIndex = 0;
    std::uint16_t adp = 0;
    std::uint16_t ado = 0;
    std::vector<std::uint8_t> payload;
};

struct EthercatDatagramResponse {
    std::uint8_t command = 0;
    std::uint8_t datagramIndex = 0;
    std::vector<std::uint8_t> payload;
    std::uint16_t workingCounter = 0;
};

class EthercatFrameCodec {
public:
    static std::vector<std::uint8_t> buildDatagramFrame(
        const std::uint8_t destinationMac[6],
        const std::uint8_t sourceMac[6],
        const EthercatDatagramRequest& request);

    static std::optional<EthercatDatagramResponse> parseDatagramFrame(
        const std::vector<std::uint8_t>& ethernetFrame,
        std::uint8_t expectedCommand,
        std::uint8_t expectedDatagramIndex,
        std::size_t expectedPayloadBytes);

    static std::vector<std::uint8_t> buildLrwFrame(
        const std::uint8_t destinationMac[6],
        const std::uint8_t sourceMac[6],
        const EthercatLrwRequest& request);

    static std::optional<EthercatLrwResponse> parseLrwFrame(
        const std::vector<std::uint8_t>& ethernetFrame,
        std::uint8_t expectedDatagramIndex,
        std::size_t expectedPayloadBytes);
};

} // namespace oec
