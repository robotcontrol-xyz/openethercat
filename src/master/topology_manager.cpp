/**
 * @file topology_manager.cpp
 * @brief openEtherCAT source file.
 */

#include "openethercat/master/topology_manager.hpp"

#include <unordered_set>

namespace oec {

TopologyManager::TopologyManager(ITransport& transport) : transport_(transport) {}

bool TopologyManager::refresh(std::string& outError) {
    TopologySnapshot newSnapshot;
    if (!transport_.discoverTopology(newSnapshot, outError)) {
        return false;
    }
    newSnapshot.redundancyHealthy = transport_.isRedundancyLinkHealthy(outError);
    if (!newSnapshot.redundancyHealthy && !outError.empty()) {
        return false;
    }
    snapshot_ = std::move(newSnapshot);
    return true;
}

std::vector<SlaveIdentity> TopologyManager::detectHotConnected(
    const std::vector<SlaveIdentity>& expected) const {
    std::unordered_set<std::uint16_t> expectedPositions;
    expectedPositions.reserve(expected.size());
    for (const auto& slave : expected) {
        expectedPositions.insert(slave.position);
    }

    std::vector<SlaveIdentity> hotConnected;
    for (const auto& info : snapshot_.slaves) {
        if (!info.online) {
            continue;
        }
        if (expectedPositions.find(info.position) == expectedPositions.end()) {
            SlaveIdentity slave;
            slave.name = "HotConnected@" + std::to_string(info.position);
            slave.alias = 0;
            slave.position = info.position;
            slave.vendorId = info.vendorId;
            slave.productCode = info.productCode;
            hotConnected.push_back(slave);
        }
    }
    return hotConnected;
}

std::vector<SlaveIdentity> TopologyManager::detectMissing(const std::vector<SlaveIdentity>& expected) const {
    std::unordered_set<std::uint16_t> online;
    for (const auto& info : snapshot_.slaves) {
        if (info.online) {
            online.insert(info.position);
        }
    }

    std::vector<SlaveIdentity> missing;
    for (const auto& slave : expected) {
        if (online.find(slave.position) == online.end()) {
            missing.push_back(slave);
        }
    }
    return missing;
}

TopologySnapshot TopologyManager::snapshot() const { return snapshot_; }

} // namespace oec
