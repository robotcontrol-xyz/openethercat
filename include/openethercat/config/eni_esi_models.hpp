#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace oec {

enum class SignalDirection { Input, Output };

/**
 * @brief Identity fields for a configured or discovered EtherCAT slave.
 */
struct SlaveIdentity {
    std::string name;
    std::uint16_t alias = 0;
    std::uint16_t position = 0;
    std::uint32_t vendorId = 0;
    std::uint32_t productCode = 0;
};

/**
 * @brief Mapping between a logical signal name and process-image bit location.
 */
struct SignalBinding {
    std::string logicalName;
    SignalDirection direction = SignalDirection::Input;
    std::string slaveName;
    std::size_t byteOffset = 0;
    std::uint8_t bitOffset = 0;
};

/**
 * @brief High-level network configuration model.
 */
struct NetworkConfiguration {
    std::vector<SlaveIdentity> slaves;
    std::vector<SignalBinding> signals;
    std::size_t processImageInputBytes = 0;
    std::size_t processImageOutputBytes = 0;
};

/**
 * @brief Lightweight ENI/ESI attribute parser helper.
 */
class EniEsiParser {
public:
    // Lightweight helpers that extract key attributes from ENI/ESI-like XML text.
    // Intended as a foundation to replace with full schema support.
    static std::optional<SlaveIdentity> parseSlaveIdentityFromXml(const std::string& xml);
    static std::optional<SignalBinding> parseSignalBindingFromXml(const std::string& xml);
};

} // namespace oec
