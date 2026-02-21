#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "openethercat/config/eni_esi_models.hpp"
#include "openethercat/transport/i_transport.hpp"

namespace oec {

/**
 * @brief Discovered slave tuple from topology scan.
 */
struct TopologySlaveInfo {
    std::uint16_t position = 0;
    std::uint32_t vendorId = 0;
    std::uint32_t productCode = 0;
    bool online = false;
};

/**
 * @brief Snapshot of discovered topology and redundancy health.
 */
struct TopologySnapshot {
    std::vector<TopologySlaveInfo> slaves;
    bool redundancyHealthy = true;
};

/**
 * @brief Topology and hot-connect manager over transport discovery hooks.
 */
class TopologyManager {
public:
    explicit TopologyManager(ITransport& transport);

    bool refresh(std::string& outError);
    std::vector<SlaveIdentity> detectHotConnected(const std::vector<SlaveIdentity>& expected) const;
    std::vector<SlaveIdentity> detectMissing(const std::vector<SlaveIdentity>& expected) const;

    TopologySnapshot snapshot() const;

private:
    ITransport& transport_;
    TopologySnapshot snapshot_{};
};

} // namespace oec
