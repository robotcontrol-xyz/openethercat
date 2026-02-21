/**
 * @file eni_esi_models.hpp
 * @brief openEtherCAT source file.
 */

#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace oec {

/**
 * @brief Logical direction of a mapped process-image signal.
 */
enum class SignalDirection { Input, Output };

/**
 * @brief Identity fields for a configured or discovered EtherCAT slave.
 */
struct SlaveIdentity {
    /// Human-readable slave name (usually from ENI/ESI).
    std::string name;
    /// EtherCAT alias address.
    std::uint16_t alias = 0;
    /// Auto-increment position used for direct slave access.
    std::uint16_t position = 0;
    /// Vendor ID from ESI/object dictionary identity.
    std::uint32_t vendorId = 0;
    /// Product code from ESI/object dictionary identity.
    std::uint32_t productCode = 0;
};

/**
 * @brief Mapping between a logical signal name and process-image bit location.
 */
struct SignalBinding {
    /// Application-level signal name, e.g. "StartButton".
    std::string logicalName;
    /// Data direction relative to master.
    SignalDirection direction = SignalDirection::Input;
    /// Referenced slave by name.
    std::string slaveName;
    /// Process-image byte offset.
    std::size_t byteOffset = 0;
    /// Bit offset inside the process-image byte.
    std::uint8_t bitOffset = 0;
};

/**
 * @brief High-level network configuration model.
 */
struct NetworkConfiguration {
    /// Declared slave chain.
    std::vector<SlaveIdentity> slaves;
    /// Logical I/O mapping table.
    std::vector<SignalBinding> signals;
    /// Input process-image size in bytes.
    std::size_t processImageInputBytes = 0;
    /// Output process-image size in bytes.
    std::size_t processImageOutputBytes = 0;
};

/**
 * @brief Lightweight ENI/ESI attribute parser helper.
 */
class EniEsiParser {
public:
    /**
     * @brief Parse a single slave identity from an ENI/ESI-like XML fragment.
     */
    static std::optional<SlaveIdentity> parseSlaveIdentityFromXml(const std::string& xml);
    /**
     * @brief Parse a single signal binding from an ENI/ESI-like XML fragment.
     */
    static std::optional<SignalBinding> parseSignalBindingFromXml(const std::string& xml);
};

} // namespace oec
