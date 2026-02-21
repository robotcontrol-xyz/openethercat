/**
 * @file eni_esi_models.cpp
 * @brief openEtherCAT source file.
 */

#include "openethercat/config/eni_esi_models.hpp"

#include <regex>

namespace oec {
namespace {

std::optional<std::string> attr(const std::string& xml, const std::string& key) {
    const auto pattern = key + "\\s*=\\s*\"([^\"]+)\"";
    std::regex re(pattern);
    std::smatch match;
    if (!std::regex_search(xml, match, re) || match.size() < 2) {
        return std::nullopt;
    }
    return match[1].str();
}

std::uint32_t parseUnsigned(const std::string& value) {
    std::size_t consumed = 0;
    const auto parsed = std::stoul(value, &consumed, 0);
    if (consumed != value.size()) {
        throw std::invalid_argument("invalid numeric value");
    }
    return static_cast<std::uint32_t>(parsed);
}

} // namespace

std::optional<SlaveIdentity> EniEsiParser::parseSlaveIdentityFromXml(const std::string& xml) {
    try {
        SlaveIdentity slave;
        const auto name = attr(xml, "name");
        const auto alias = attr(xml, "alias");
        const auto position = attr(xml, "position");
        const auto vendor = attr(xml, "vendorId");
        const auto product = attr(xml, "productCode");

        if (!name || !alias || !position || !vendor || !product) {
            return std::nullopt;
        }

        slave.name = *name;
        slave.alias = static_cast<std::uint16_t>(parseUnsigned(*alias));
        slave.position = static_cast<std::uint16_t>(parseUnsigned(*position));
        slave.vendorId = parseUnsigned(*vendor);
        slave.productCode = parseUnsigned(*product);

        return slave;
    } catch (...) {
        return std::nullopt;
    }
}

std::optional<SignalBinding> EniEsiParser::parseSignalBindingFromXml(const std::string& xml) {
    try {
        SignalBinding binding;
        const auto logical = attr(xml, "logicalName");
        const auto direction = attr(xml, "direction");
        const auto slave = attr(xml, "slaveName");
        const auto byteOffset = attr(xml, "byteOffset");
        const auto bitOffset = attr(xml, "bitOffset");

        if (!logical || !direction || !slave || !byteOffset || !bitOffset) {
            return std::nullopt;
        }

        binding.logicalName = *logical;
        binding.direction = (*direction == "output") ? SignalDirection::Output : SignalDirection::Input;
        binding.slaveName = *slave;
        binding.byteOffset = static_cast<std::size_t>(parseUnsigned(*byteOffset));
        binding.bitOffset = static_cast<std::uint8_t>(parseUnsigned(*bitOffset));

        return binding;
    } catch (...) {
        return std::nullopt;
    }
}

} // namespace oec
