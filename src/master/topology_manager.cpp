/**
 * @file topology_manager.cpp
 * @brief openEtherCAT source file.
 */

#include "openethercat/master/topology_manager.hpp"

#include <algorithm>
#include <unordered_map>
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
    TopologyChangeSet changes;
    changes.generation = generation_ + 1U;
    changes.previousRedundancyHealthy = snapshot_.redundancyHealthy;
    changes.redundancyHealthy = newSnapshot.redundancyHealthy;
    changes.redundancyChanged = (changes.previousRedundancyHealthy != changes.redundancyHealthy);

    std::unordered_map<std::uint16_t, TopologySlaveInfo> previousByPosition;
    previousByPosition.reserve(snapshot_.slaves.size());
    for (const auto& slave : snapshot_.slaves) {
        previousByPosition[slave.position] = slave;
    }

    std::unordered_map<std::uint16_t, TopologySlaveInfo> currentByPosition;
    currentByPosition.reserve(newSnapshot.slaves.size());
    for (const auto& slave : newSnapshot.slaves) {
        currentByPosition[slave.position] = slave;
    }

    for (const auto& kv : currentByPosition) {
        const auto pos = kv.first;
        const auto& now = kv.second;
        const auto previousIt = previousByPosition.find(pos);
        if (previousIt == previousByPosition.end()) {
            changes.added.push_back(now);
            continue;
        }
        const auto& prev = previousIt->second;
        if (prev.online != now.online ||
            prev.vendorId != now.vendorId ||
            prev.productCode != now.productCode ||
            prev.alStateValid != now.alStateValid ||
            (prev.alStateValid && now.alStateValid && prev.alState != now.alState)) {
            TopologySlaveDelta delta;
            delta.position = pos;
            delta.wasOnline = prev.online;
            delta.isOnline = now.online;
            delta.previousVendorId = prev.vendorId;
            delta.previousProductCode = prev.productCode;
            delta.vendorId = now.vendorId;
            delta.productCode = now.productCode;
            changes.updated.push_back(delta);
        }
    }

    for (const auto& kv : previousByPosition) {
        if (currentByPosition.find(kv.first) == currentByPosition.end()) {
            changes.removed.push_back(kv.second);
        }
    }

    changes.changed = changes.redundancyChanged ||
                      !changes.added.empty() ||
                      !changes.removed.empty() ||
                      !changes.updated.empty();

    std::sort(changes.added.begin(), changes.added.end(),
              [](const TopologySlaveInfo& a, const TopologySlaveInfo& b) { return a.position < b.position; });
    std::sort(changes.removed.begin(), changes.removed.end(),
              [](const TopologySlaveInfo& a, const TopologySlaveInfo& b) { return a.position < b.position; });
    std::sort(changes.updated.begin(), changes.updated.end(),
              [](const TopologySlaveDelta& a, const TopologySlaveDelta& b) { return a.position < b.position; });

    snapshot_ = std::move(newSnapshot);
    changeSet_ = std::move(changes);
    generation_ = changeSet_.generation;
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
TopologyChangeSet TopologyManager::changeSet() const { return changeSet_; }
std::uint64_t TopologyManager::generation() const { return generation_; }

} // namespace oec
