/**
 * @file recovery_profile_loader.cpp
 * @brief openEtherCAT source file.
 */

#include "openethercat/config/recovery_profile_loader.hpp"

#include <fstream>
#include <regex>
#include <sstream>
#include <stdexcept>

namespace oec {
namespace {

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

std::uint16_t parseCode(const std::string& text) {
    std::size_t consumed = 0;
    const auto value = std::stoul(text, &consumed, 0);
    if (consumed != text.size() || value > 0xFFFFUL) {
        throw std::invalid_argument("invalid AL status code");
    }
    return static_cast<std::uint16_t>(value);
}

} // namespace

bool RecoveryProfileLoader::loadFromJsonFile(const std::string& filePath,
                                             RecoveryProfile& outProfile,
                                             std::string& outError) {
    outProfile = RecoveryProfile{};
    outError.clear();

    std::string json;
    if (!readFile(filePath, json, outError)) {
        return false;
    }

    try {
        // Minimal JSON extraction by regex for object entries:
        // { "alStatusCode": "0x0017", "action": "Reconfigure" }
        std::regex entryRe(
            "\\{[^\\{\\}]*\"alStatusCode\"\\s*:\\s*\"([^\"]+)\"[^\\{\\}]*\"action\"\\s*:\\s*\"([^\"]+)\"[^\\{\\}]*\\}",
            std::regex_constants::icase);

        bool foundAny = false;
        for (std::sregex_iterator it(json.begin(), json.end(), entryRe), end; it != end; ++it) {
            const auto codeText = (*it)[1].str();
            const auto actionText = (*it)[2].str();
            const auto action = parseRecoveryAction(actionText);
            if (!action) {
                outError = "Unknown recovery action: " + actionText;
                return false;
            }
            outProfile.actionByAlStatusCode[parseCode(codeText)] = *action;
            foundAny = true;
        }

        if (!foundAny) {
            outError = "No recovery profile entries found";
            return false;
        }
        return true;
    } catch (const std::exception& ex) {
        outError = std::string("Recovery profile parse error: ") + ex.what();
        return false;
    }
}

} // namespace oec
