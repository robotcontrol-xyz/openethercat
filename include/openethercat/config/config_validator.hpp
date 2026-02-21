#pragma once

#include <string>
#include <vector>

#include "openethercat/config/eni_esi_models.hpp"

namespace oec {

enum class ValidationSeverity { Warning, Error };

struct ValidationIssue {
    ValidationSeverity severity = ValidationSeverity::Error;
    std::string message;
};

class ConfigurationValidator {
public:
    static std::vector<ValidationIssue> validate(const NetworkConfiguration& config);
    static bool hasErrors(const std::vector<ValidationIssue>& issues);
};

} // namespace oec
