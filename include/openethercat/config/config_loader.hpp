#pragma once

#include <string>

#include "openethercat/config/eni_esi_models.hpp"

namespace oec {

class ConfigurationLoader {
public:
    static bool loadFromEniFile(const std::string& eniPath,
                                NetworkConfiguration& outConfig,
                                std::string& outError);

    static bool loadFromEniAndEsiDirectory(const std::string& eniPath,
                                           const std::string& esiDirectory,
                                           NetworkConfiguration& outConfig,
                                           std::string& outError);
};

} // namespace oec
