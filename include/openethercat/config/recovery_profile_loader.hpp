/**
 * @file recovery_profile_loader.hpp
 * @brief openEtherCAT source file.
 */

#pragma once

#include <cstdint>
#include <string>
#include <unordered_map>

#include "openethercat/master/slave_diagnostics.hpp"

namespace oec {

struct RecoveryProfile {
    std::unordered_map<std::uint16_t, RecoveryAction> actionByAlStatusCode;
};

class RecoveryProfileLoader {
public:
    static bool loadFromJsonFile(const std::string& filePath,
                                 RecoveryProfile& outProfile,
                                 std::string& outError);
};

} // namespace oec
