#pragma once

#include <cstddef>
#include <cstdint>
#include <unordered_map>
#include <queue>
#include <string>
#include <vector>

#include "openethercat/master/foe_eoe.hpp"
#include "openethercat/master/coe_mailbox.hpp"
#include "openethercat/master/topology_manager.hpp"
#include "openethercat/transport/i_transport.hpp"

namespace oec {

class MockTransport final : public ITransport {
public:
    MockTransport(std::size_t inputBytes, std::size_t outputBytes);

    bool open() override;
    void close() override;
    bool exchange(const std::vector<std::uint8_t>& txProcessData,
                  std::vector<std::uint8_t>& rxProcessData) override;

    std::string lastError() const override;
    std::uint16_t lastWorkingCounter() const override;
    bool requestNetworkState(SlaveState state) override;
    bool readNetworkState(SlaveState& outState) override;
    bool requestSlaveState(std::uint16_t position, SlaveState state) override;
    bool readSlaveState(std::uint16_t position, SlaveState& outState) override;
    bool readSlaveAlStatusCode(std::uint16_t position, std::uint16_t& outCode) override;
    bool reconfigureSlave(std::uint16_t position) override;
    bool failoverSlave(std::uint16_t position) override;
    bool sdoUpload(std::uint16_t slavePosition, const SdoAddress& address,
                   std::vector<std::uint8_t>& outData, std::uint32_t& outAbortCode,
                   std::string& outError) override;
    bool sdoDownload(std::uint16_t slavePosition, const SdoAddress& address,
                     const std::vector<std::uint8_t>& data, std::uint32_t& outAbortCode,
                     std::string& outError) override;
    bool configurePdo(std::uint16_t slavePosition, std::uint16_t assignIndex,
                      const std::vector<PdoMappingEntry>& entries,
                      std::string& outError) override;
    bool pollEmergency(EmergencyMessage& outEmergency) override;
    bool discoverTopology(TopologySnapshot& outSnapshot, std::string& outError) override;
    bool isRedundancyLinkHealthy(std::string& outError) override;
    bool foeRead(std::uint16_t slavePosition, const FoERequest& request,
                 FoEResponse& outResponse, std::string& outError) override;
    bool foeWrite(std::uint16_t slavePosition, const FoERequest& request,
                  const std::vector<std::uint8_t>& data, std::string& outError) override;
    bool eoeSend(std::uint16_t slavePosition, const std::vector<std::uint8_t>& frame,
                 std::string& outError) override;
    bool eoeReceive(std::uint16_t slavePosition, std::vector<std::uint8_t>& frame,
                    std::string& outError) override;

    void setInputBit(std::size_t byteOffset, std::uint8_t bitOffset, bool value);
    void setInputByte(std::size_t byteOffset, std::uint8_t value);
    void setInputBytes(std::size_t byteOffset, const std::vector<std::uint8_t>& data);
    bool getLastOutputBit(std::size_t byteOffset, std::uint8_t bitOffset) const;
    std::vector<std::uint8_t> lastOutputs() const;
    void setSlaveAlStatusCode(std::uint16_t position, std::uint16_t alStatusCode);
    void injectExchangeFailures(std::size_t count);
    void enqueueEmergency(const EmergencyMessage& emergency);
    void setRedundancyHealthy(bool healthy);
    void setDiscoveredSlaves(const std::vector<TopologySlaveInfo>& slaves);

private:
    static void setBit(std::vector<std::uint8_t>& bytes, std::size_t byteOffset,
                       std::uint8_t bitOffset, bool value);
    static bool getBit(const std::vector<std::uint8_t>& bytes, std::size_t byteOffset,
                       std::uint8_t bitOffset);

    std::vector<std::uint8_t> inputs_;
    std::vector<std::uint8_t> lastOutputs_;
    std::uint16_t lastWorkingCounter_ = 0;
    SlaveState state_ = SlaveState::Init;
    std::unordered_map<std::uint16_t, SlaveState> perSlaveState_;
    std::unordered_map<std::uint16_t, std::uint16_t> perSlaveAlStatusCode_;
    std::unordered_map<std::uint64_t, std::vector<std::uint8_t>> sdoObjects_;
    std::unordered_map<std::uint64_t, std::vector<std::uint8_t>> foeFiles_;
    std::unordered_map<std::uint16_t, std::vector<PdoMappingEntry>> pdoAssignments_;
    std::queue<EmergencyMessage> emergencies_;
    std::queue<std::pair<std::uint16_t, std::vector<std::uint8_t>>> eoeFrames_;
    std::vector<TopologySlaveInfo> discoveredSlaves_;
    bool redundancyHealthy_ = true;
    std::size_t remainingExchangeFailures_ = 0;
    bool opened_ = false;
    std::string error_;
};

} // namespace oec
