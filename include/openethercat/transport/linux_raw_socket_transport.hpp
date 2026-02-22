/**
 * @file linux_raw_socket_transport.hpp
 * @brief openEtherCAT source file.
 */

#pragma once

#include <array>
#include <cstdint>
#include <queue>
#include <string>
#include <vector>

#include "openethercat/master/coe_mailbox.hpp"
#include "openethercat/transport/i_transport.hpp"

namespace oec {

/**
 * @brief Mailbox-path diagnostics counters for LinuxRawSocketTransport.
 */
struct MailboxDiagnostics {
    std::uint64_t transactionsStarted = 0;
    std::uint64_t transactionsFailed = 0;
    std::uint64_t mailboxWrites = 0;
    std::uint64_t mailboxReads = 0;
    std::uint64_t datagramRetries = 0;
    std::uint64_t mailboxTimeouts = 0;
    std::uint64_t staleCounterDrops = 0;
    std::uint64_t parseRejects = 0;
    std::uint64_t emergencyQueued = 0;
    std::uint64_t matchedResponses = 0;
};

class LinuxRawSocketTransport final : public ITransport {
public:
    explicit LinuxRawSocketTransport(std::string ifname);
    LinuxRawSocketTransport(std::string primaryIfname, std::string secondaryIfname);
    ~LinuxRawSocketTransport() override;

    void setCycleTimeoutMs(int timeoutMs);
    void setLogicalAddress(std::uint32_t logicalAddress);
    void setExpectedWorkingCounter(std::uint16_t expectedWorkingCounter);
    void setMaxFramesPerCycle(std::size_t maxFramesPerCycle);
    void enableRedundancy(bool enabled);
    void setMailboxConfiguration(std::uint16_t writeOffset, std::uint16_t writeSize,
                                 std::uint16_t readOffset, std::uint16_t readSize);

    bool open() override;
    void close() override;
    bool exchange(const std::vector<std::uint8_t>& txProcessData,
                  std::vector<std::uint8_t>& rxProcessData) override;
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
    bool configureProcessImage(const NetworkConfiguration& config, std::string& outError) override;
    bool foeRead(std::uint16_t slavePosition, const FoERequest& request,
                 FoEResponse& outResponse, std::string& outError) override;
    bool foeWrite(std::uint16_t slavePosition, const FoERequest& request,
                  const std::vector<std::uint8_t>& data, std::string& outError) override;
    bool eoeSend(std::uint16_t slavePosition, const std::vector<std::uint8_t>& frame,
                 std::string& outError) override;
    bool eoeReceive(std::uint16_t slavePosition, std::vector<std::uint8_t>& frame,
                    std::string& outError) override;

    std::string lastError() const override;
    std::uint16_t lastWorkingCounter() const override;
    MailboxDiagnostics mailboxDiagnostics() const;
    void resetMailboxDiagnostics();

private:
    struct ProcessDataWindow {
        std::uint16_t slavePosition = 0U;
        std::uint16_t physicalStart = 0U;
        std::uint16_t length = 0U;
        std::uint32_t logicalStart = 0U;
    };

    std::string ifname_;
    std::string secondaryIfname_;
    int socketFd_ = -1;
    int secondarySocketFd_ = -1;
    int ifIndex_ = 0;
    int secondaryIfIndex_ = 0;
    std::array<std::uint8_t, 6> sourceMac_{};
    std::array<std::uint8_t, 6> destinationMac_{0xFFU, 0xFFU, 0xFFU, 0xFFU, 0xFFU, 0xFFU};
    std::uint8_t datagramIndex_ = 0;
    std::uint32_t logicalAddress_ = 0;
    std::uint16_t expectedWorkingCounter_ = 1;
    std::uint16_t lastWorkingCounter_ = 0;
    std::size_t maxFramesPerCycle_ = 128;
    bool redundancyEnabled_ = false;
    bool lastFrameUsedSecondary_ = false;
    std::uint16_t mailboxWriteOffset_ = 0x1000;
    std::uint16_t mailboxWriteSize_ = 0x0080;
    std::uint16_t mailboxReadOffset_ = 0x1080;
    std::uint16_t mailboxReadSize_ = 0x0080;
    std::uint8_t mailboxCounter_ = 0;
    int timeoutMs_ = 10;
    std::string error_;
    std::vector<ProcessDataWindow> outputWindows_;
    std::queue<EmergencyMessage> emergencies_;
    MailboxDiagnostics mailboxDiagnostics_{};
};

} // namespace oec
