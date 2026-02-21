#include "openethercat/config/config_loader.hpp"

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <regex>
#include <sstream>
#include <stdexcept>
#include <unordered_map>

#include "openethercat/config/config_validator.hpp"

namespace oec {
namespace {

std::optional<std::string> attr(const std::string& xml, const std::string& key) {
    const auto pattern = key + "\\s*=\\s*\"([^\"]+)\"";
    std::regex re(pattern, std::regex_constants::icase);
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

bool readFile(const std::string& path, std::string& out, std::string& outError) {
    std::ifstream file(path);
    if (!file) {
        outError = "Cannot open file: " + path;
        return false;
    }
    std::ostringstream buffer;
    buffer << file.rdbuf();
    out = buffer.str();
    return true;
}

std::vector<std::string> extractTags(const std::string& xml, const std::string& tagName) {
    const auto pattern = "<\\s*" + tagName + "\\b[^>]*>";
    std::regex re(pattern, std::regex_constants::icase);
    std::vector<std::string> tags;

    for (std::sregex_iterator it(xml.begin(), xml.end(), re), end; it != end; ++it) {
        tags.push_back(it->str());
    }
    return tags;
}

bool parseProcessImage(const std::string& xml, NetworkConfiguration& config) {
    const auto tags = extractTags(xml, "ProcessImage");
    if (tags.empty()) {
        return false;
    }
    const auto input = attr(tags.front(), "inputBytes");
    const auto output = attr(tags.front(), "outputBytes");
    if (!input || !output) {
        return false;
    }

    config.processImageInputBytes = static_cast<std::size_t>(parseUnsigned(*input));
    config.processImageOutputBytes = static_cast<std::size_t>(parseUnsigned(*output));
    return true;
}

std::optional<SlaveIdentity> parseSlaveTag(const std::string& tag) {
    SlaveIdentity slave;
    const auto name = attr(tag, "name");
    if (!name) {
        return std::nullopt;
    }

    slave.name = *name;
    if (const auto alias = attr(tag, "alias")) {
        slave.alias = static_cast<std::uint16_t>(parseUnsigned(*alias));
    }
    if (const auto position = attr(tag, "position")) {
        slave.position = static_cast<std::uint16_t>(parseUnsigned(*position));
    }
    if (const auto vendor = attr(tag, "vendorId")) {
        slave.vendorId = parseUnsigned(*vendor);
    }
    if (const auto product = attr(tag, "productCode")) {
        slave.productCode = parseUnsigned(*product);
    }

    return slave;
}

std::optional<SignalBinding> parseSignalTag(const std::string& tag) {
    SignalBinding signal;
    const auto logical = attr(tag, "logicalName");
    const auto direction = attr(tag, "direction");
    const auto slave = attr(tag, "slaveName");
    const auto byteOffset = attr(tag, "byteOffset");
    const auto bitOffset = attr(tag, "bitOffset");

    if (!logical || !direction || !slave || !byteOffset || !bitOffset) {
        return std::nullopt;
    }

    signal.logicalName = *logical;
    signal.direction = (*direction == "output" || *direction == "Output")
                           ? SignalDirection::Output
                           : SignalDirection::Input;
    signal.slaveName = *slave;
    signal.byteOffset = static_cast<std::size_t>(parseUnsigned(*byteOffset));
    signal.bitOffset = static_cast<std::uint8_t>(parseUnsigned(*bitOffset));
    return signal;
}

bool parseEniXml(const std::string& xml, NetworkConfiguration& config, std::string& outError) {
    try {
        if (!parseProcessImage(xml, config)) {
            outError = "Missing or invalid <ProcessImage inputBytes=\"...\" outputBytes=\"...\"/>";
            return false;
        }

        for (const auto& tag : extractTags(xml, "Slave")) {
            const auto slave = parseSlaveTag(tag);
            if (slave) {
                config.slaves.push_back(*slave);
            }
        }

        for (const auto& tag : extractTags(xml, "Signal")) {
            const auto signal = parseSignalTag(tag);
            if (signal) {
                config.signals.push_back(*signal);
            }
        }

        if (config.signals.empty()) {
            outError = "No <Signal ...> entries found in ENI file";
            return false;
        }
        return true;
    } catch (const std::exception& ex) {
        outError = std::string("ENI parse error: ") + ex.what();
        return false;
    }
}

std::unordered_map<std::string, SlaveIdentity> loadEsiCatalog(const std::string& esiDirectory,
                                                              std::string& outError) {
    std::unordered_map<std::string, SlaveIdentity> catalog;
    namespace fs = std::filesystem;

    std::error_code ec;
    if (!fs::exists(esiDirectory, ec)) {
        outError = "ESI directory does not exist: " + esiDirectory;
        return catalog;
    }

    for (const auto& entry : fs::directory_iterator(esiDirectory, ec)) {
        if (ec) {
            outError = "Failed to enumerate ESI directory: " + ec.message();
            return {};
        }
        if (!entry.is_regular_file()) {
            continue;
        }
        if (entry.path().extension() != ".xml") {
            continue;
        }

        std::string xml;
        if (!readFile(entry.path().string(), xml, outError)) {
            return {};
        }

        const auto mergeIntoCatalog = [&](const SlaveIdentity& slave) {
            auto& existing = catalog[slave.name];
            if (existing.name.empty()) {
                existing = slave;
                return;
            }
            if (existing.vendorId == 0U && slave.vendorId != 0U) {
                existing.vendorId = slave.vendorId;
            }
            if (existing.productCode == 0U && slave.productCode != 0U) {
                existing.productCode = slave.productCode;
            }
            if (existing.alias == 0U && slave.alias != 0U) {
                existing.alias = slave.alias;
            }
            if (existing.position == 0U && slave.position != 0U) {
                existing.position = slave.position;
            }
        };

        for (const auto& tag : extractTags(xml, "Device")) {
            const auto slave = parseSlaveTag(tag);
            if (slave) {
                mergeIntoCatalog(*slave);
            }
        }
        for (const auto& tag : extractTags(xml, "Slave")) {
            const auto slave = parseSlaveTag(tag);
            if (slave) {
                mergeIntoCatalog(*slave);
            }
        }
    }

    return catalog;
}

void mergeEsiInfo(NetworkConfiguration& config,
                  const std::unordered_map<std::string, SlaveIdentity>& catalog) {
    for (auto& slave : config.slaves) {
        const auto it = catalog.find(slave.name);
        if (it == catalog.end()) {
            continue;
        }
        if (slave.vendorId == 0U) {
            slave.vendorId = it->second.vendorId;
        }
        if (slave.productCode == 0U) {
            slave.productCode = it->second.productCode;
        }
    }
}

} // namespace

bool ConfigurationLoader::loadFromEniFile(const std::string& eniPath,
                                          NetworkConfiguration& outConfig,
                                          std::string& outError) {
    outConfig = NetworkConfiguration{};
    outError.clear();

    std::string xml;
    if (!readFile(eniPath, xml, outError)) {
        return false;
    }

    if (!parseEniXml(xml, outConfig, outError)) {
        return false;
    }

    const auto issues = ConfigurationValidator::validate(outConfig);
    if (ConfigurationValidator::hasErrors(issues)) {
        outError = "Configuration validation failed:";
        for (const auto& issue : issues) {
            if (issue.severity == ValidationSeverity::Error) {
                outError += " " + issue.message + ";";
            }
        }
        return false;
    }
    return true;
}

bool ConfigurationLoader::loadFromEniAndEsiDirectory(const std::string& eniPath,
                                                     const std::string& esiDirectory,
                                                     NetworkConfiguration& outConfig,
                                                     std::string& outError) {
    if (!loadFromEniFile(eniPath, outConfig, outError)) {
        return false;
    }

    auto catalog = loadEsiCatalog(esiDirectory, outError);
    if (!outError.empty()) {
        return false;
    }

    mergeEsiInfo(outConfig, catalog);

    const auto issues = ConfigurationValidator::validate(outConfig);
    if (ConfigurationValidator::hasErrors(issues)) {
        outError = "Configuration validation failed:";
        for (const auto& issue : issues) {
            if (issue.severity == ValidationSeverity::Error) {
                outError += " " + issue.message + ";";
            }
        }
        return false;
    }

    return true;
}

} // namespace oec
