/**
 * @file foe_eoe.hpp
 * @brief openEtherCAT source file.
 */

#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "openethercat/transport/i_transport.hpp"

namespace oec {

/**
 * @brief FoE file transfer request.
 */
struct FoERequest {
    std::string fileName;
    std::uint32_t password = 0;
    std::size_t maxChunkBytes = 1024;
};

/**
 * @brief FoE file transfer response.
 */
struct FoEResponse {
    bool success = false;
    std::vector<std::uint8_t> data;
    std::string error;
};

/**
 * @brief FoE/EoE service facade over transport primitives.
 */
class FoeEoeService {
public:
    explicit FoeEoeService(ITransport& transport);

    FoEResponse readFile(std::uint16_t slavePosition, const FoERequest& request) const;
    bool writeFile(std::uint16_t slavePosition,
                   const FoERequest& request,
                   const std::vector<std::uint8_t>& data,
                   std::string& outError) const;

    bool sendEthernetOverEthercat(std::uint16_t slavePosition,
                                  const std::vector<std::uint8_t>& frame,
                                  std::string& outError) const;
    bool receiveEthernetOverEthercat(std::uint16_t slavePosition,
                                     std::vector<std::uint8_t>& frame,
                                     std::string& outError) const;

private:
    ITransport& transport_;
};

} // namespace oec
