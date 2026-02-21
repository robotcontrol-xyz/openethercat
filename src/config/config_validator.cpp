/**
 * @file config_validator.cpp
 * @brief openEtherCAT source file.
 */

#include "openethercat/config/config_validator.hpp"

#include <sstream>
#include <unordered_map>

namespace oec {

std::vector<ValidationIssue> ConfigurationValidator::validate(const NetworkConfiguration& config) {
    std::vector<ValidationIssue> issues;

    if (config.processImageInputBytes == 0U && config.processImageOutputBytes == 0U) {
        issues.push_back({ValidationSeverity::Error,
                          "Process image cannot have both inputBytes and outputBytes equal to zero"});
    }

    std::unordered_map<std::string, std::size_t> signalNames;
    for (const auto& signal : config.signals) {
        if (signal.logicalName.empty()) {
            issues.push_back({ValidationSeverity::Error, "Signal logicalName cannot be empty"});
            continue;
        }

        const auto [_, inserted] = signalNames.emplace(signal.logicalName, 1U);
        if (!inserted) {
            issues.push_back({ValidationSeverity::Error,
                              "Duplicate logical signal name: " + signal.logicalName});
        }

        if (signal.slaveName.empty()) {
            issues.push_back({ValidationSeverity::Error,
                              "Signal '" + signal.logicalName + "' missing slaveName"});
        }

        if (signal.bitOffset >= 8U) {
            issues.push_back({ValidationSeverity::Error,
                              "Signal '" + signal.logicalName + "' has bitOffset >= 8"});
        }

        const auto imageBytes = (signal.direction == SignalDirection::Input)
                                    ? config.processImageInputBytes
                                    : config.processImageOutputBytes;
        if (signal.byteOffset >= imageBytes) {
            std::ostringstream os;
            os << "Signal '" << signal.logicalName << "' byteOffset " << signal.byteOffset
               << " outside process image size " << imageBytes;
            issues.push_back({ValidationSeverity::Error, os.str()});
        }
    }

    if (config.signals.empty()) {
        issues.push_back({ValidationSeverity::Error,
                          "Configuration must contain at least one logical signal"});
    }

    return issues;
}

bool ConfigurationValidator::hasErrors(const std::vector<ValidationIssue>& issues) {
    for (const auto& issue : issues) {
        if (issue.severity == ValidationSeverity::Error) {
            return true;
        }
    }
    return false;
}

} // namespace oec
