/**
 * @file config_loader.hpp
 * @brief openEtherCAT source file.
 */

#pragma once

#include <string>

#include "openethercat/config/eni_esi_models.hpp"

namespace oec {

/**
 * @brief Loads network configuration from ENI and optional ESI sources.
 *
 * The loader provides a lightweight XML extraction path tailored for examples
 * and integration tests. It builds `NetworkConfiguration` with slave identities,
 * signal mappings, and process-image sizing used by `EthercatMaster`.
 */
class ConfigurationLoader {
public:
    /**
     * @brief Load a configuration from an ENI-like XML file.
     *
     * @param eniPath Path to ENI XML file.
     * @param outConfig Parsed configuration model on success.
     * @param outError Human-readable parse/IO error on failure.
     * @return true if parsing succeeded.
     */
    static bool loadFromEniFile(const std::string& eniPath,
                                NetworkConfiguration& outConfig,
                                std::string& outError);

    /**
     * @brief Load ENI and enrich slave identity fields using ESI files in a directory.
     *
     * This method first parses ENI, then scans ESI XML files for vendor/product
     * metadata and merges missing values into configured slaves by name.
     *
     * @param eniPath Path to ENI XML file.
     * @param esiDirectory Directory containing ESI XML files.
     * @param outConfig Parsed and merged configuration model on success.
     * @param outError Human-readable parse/IO error on failure.
     * @return true if parsing and merge succeeded.
     */
    static bool loadFromEniAndEsiDirectory(const std::string& eniPath,
                                           const std::string& esiDirectory,
                                           NetworkConfiguration& outConfig,
                                           std::string& outError);
};

} // namespace oec
