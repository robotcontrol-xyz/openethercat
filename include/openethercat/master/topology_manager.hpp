/**
 * @file topology_manager.hpp
 * @brief openEtherCAT source file.
 */

#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "openethercat/config/eni_esi_models.hpp"
#include "openethercat/core/slave_state.hpp"
#include "openethercat/transport/i_transport.hpp"

namespace oec {

/**
 * @brief Discovered slave tuple from topology scan.
 */
struct TopologySlaveInfo {
    std::uint16_t position = 0;
    std::uint32_t vendorId = 0;
    std::uint32_t productCode = 0;
    std::uint16_t escType = 0;
    std::uint16_t escRevision = 0;
    bool identityFromCoe = false;
    bool identityFromSii = false;
    bool alStateValid = false;
    SlaveState alState = SlaveState::Init;
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
 * @brief Per-slave delta entry between two topology snapshots.
 */
struct TopologySlaveDelta {
    std::uint16_t position = 0;
    bool wasOnline = false;
    bool isOnline = false;
    std::uint32_t previousVendorId = 0;
    std::uint32_t previousProductCode = 0;
    std::uint32_t vendorId = 0;
    std::uint32_t productCode = 0;
};

/**
 * @brief Deterministic topology change set emitted by refresh().
 */
struct TopologyChangeSet {
    std::uint64_t generation = 0;
    bool changed = false;
    bool redundancyChanged = false;
    bool previousRedundancyHealthy = true;
    bool redundancyHealthy = true;
    std::vector<TopologySlaveInfo> added;
    std::vector<TopologySlaveInfo> removed;
    std::vector<TopologySlaveDelta> updated;
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
    TopologyChangeSet changeSet() const;
    std::uint64_t generation() const;

private:
    ITransport& transport_;
    TopologySnapshot snapshot_{};
    TopologyChangeSet changeSet_{};
    std::uint64_t generation_ = 0;
};

} // namespace oec
