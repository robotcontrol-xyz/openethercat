/**
 * @file coe_mailbox.hpp
 * @brief openEtherCAT source file.
 */

#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "openethercat/transport/i_transport.hpp"

namespace oec {

/**
 * @brief CoE object dictionary address (index/subindex).
 */
struct SdoAddress {
    std::uint16_t index = 0;
    std::uint8_t subIndex = 0;
};

/**
 * @brief SDO abort information.
 */
struct SdoAbort {
    std::uint32_t code = 0;
    std::string message;
};

/**
 * @brief SDO service response container.
 */
struct SdoResponse {
    bool success = false;
    std::vector<std::uint8_t> data;
    std::optional<SdoAbort> abort;
};

/**
 * @brief PDO mapping entry descriptor.
 */
struct PdoMappingEntry {
    std::uint16_t index = 0;
    std::uint8_t subIndex = 0;
    std::uint8_t bitLength = 0;
};

/**
 * @brief CoE emergency message structure.
 */
struct EmergencyMessage {
    std::uint16_t errorCode = 0;
    std::uint8_t errorRegister = 0;
    std::array<std::uint8_t, 5> manufacturerData{};
    std::uint16_t slavePosition = 0;
};

/**
 * @brief CoE mailbox service facade over transport primitives.
 */
class CoeMailboxService {
public:
    /**
     * @brief Construct CoE service facade bound to transport mailbox hooks.
     */
    explicit CoeMailboxService(ITransport& transport);

    /**
     * @brief Perform SDO upload (read) from a slave object entry.
     */
    SdoResponse upload(std::uint16_t slavePosition, SdoAddress address) const;
    /**
     * @brief Perform SDO download (write) to a slave object entry.
     */
    SdoResponse download(std::uint16_t slavePosition,
                         SdoAddress address,
                         const std::vector<std::uint8_t>& data) const;
    /**
     * @brief Configure standard RxPDO mapping for a slave.
     */
    bool configureRxPdo(std::uint16_t slavePosition,
                        const std::vector<PdoMappingEntry>& entries,
                        std::string& outError) const;
    /**
     * @brief Configure standard TxPDO mapping for a slave.
     */
    bool configureTxPdo(std::uint16_t slavePosition,
                        const std::vector<PdoMappingEntry>& entries,
                        std::string& outError) const;

    /**
     * @brief Drain up to `maxMessages` emergency messages from transport queue.
     */
    std::vector<EmergencyMessage> drainEmergencyQueue(std::size_t maxMessages) const;

private:
    static std::string describeAbort(std::uint32_t code);

    ITransport& transport_;
};

} // namespace oec
