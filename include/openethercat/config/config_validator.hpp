/**
 * @file config_validator.hpp
 * @brief openEtherCAT source file.
 */

#pragma once

#include <string>
#include <vector>

#include "openethercat/config/eni_esi_models.hpp"

namespace oec {

/**
 * @brief Severity level for configuration validation findings.
 */
enum class ValidationSeverity { Warning, Error };

/**
 * @brief One configuration validation finding.
 */
struct ValidationIssue {
    ValidationSeverity severity = ValidationSeverity::Error;
    std::string message;
};

/**
 * @brief Validates a `NetworkConfiguration` before runtime use.
 *
 * Checks include signal integrity (direction/name), bounds against process-image
 * sizes, and basic configuration consistency expected by mapping/runtime layers.
 */
class ConfigurationValidator {
public:
    /**
     * @brief Perform validation and return all findings.
     */
    static std::vector<ValidationIssue> validate(const NetworkConfiguration& config);
    /**
     * @brief Convenience predicate to detect if any issue is fatal.
     */
    static bool hasErrors(const std::vector<ValidationIssue>& issues);
};

} // namespace oec
