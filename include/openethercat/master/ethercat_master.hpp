/**
 * @file ethercat_master.hpp
 * @brief openEtherCAT source file.
 */

#pragma once

#include <mutex>
#include <string>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <unordered_map>
#include <vector>

#include "openethercat/config/config_validator.hpp"
#include "openethercat/config/eni_esi_models.hpp"
#include "openethercat/core/process_image.hpp"
#include "openethercat/master/coe_mailbox.hpp"
#include "openethercat/master/cycle_statistics.hpp"
#include "openethercat/master/distributed_clock.hpp"
#include "openethercat/master/foe_eoe.hpp"
#include "openethercat/master/hil_campaign.hpp"
#include "openethercat/master/slave_diagnostics.hpp"
#include "openethercat/master/topology_manager.hpp"
#include "openethercat/mapping/io_mapper.hpp"
#include "openethercat/transport/i_transport.hpp"

namespace oec {

class LinuxRawSocketTransport;

/**
 * @brief High-level orchestration class for EtherCAT runtime.
 *
 * EthercatMaster coordinates process-image exchange, mapping callbacks,
 * mailbox services, diagnostics/recovery, DC updates, and topology checks.
 */
class EthercatMaster {
public:
    /**
     * @brief Startup state-machine behavior for INIT->PRE-OP->SAFE-OP->OP transitions.
     */
    struct StateMachineOptions {
        bool enable = true;
        std::chrono::milliseconds transitionTimeout{500};
        std::size_t pollIntervalMs = 5;
    };
    /**
     * @brief Recovery policy knobs used when cyclic exchange fails.
     */
    struct RecoveryOptions {
        bool enable = true;
        std::size_t maxRetriesPerSlave = 3;
        std::size_t maxReconfigurePerSlave = 1;
        bool stopMasterOnFailover = false;
        std::size_t maxEventHistory = 1024;
    };
    /**
     * @brief Immutable record for one recovery action attempt.
     */
    struct RecoveryEvent {
        std::chrono::system_clock::time_point timestamp{};
        std::uint64_t cycleIndex = 0;
        std::uint16_t slavePosition = 0;
        std::uint16_t alStatusCode = 0;
        RecoveryAction action = RecoveryAction::None;
        bool success = false;
        std::string message;
    };

    explicit EthercatMaster(ITransport& transport);

    /**
     * @brief Configure master process image and logical bindings.
     */
    bool configure(const NetworkConfiguration& config);
    /**
     * @brief Open transport and transition network to OP state (if enabled).
     */
    bool start();
    /**
     * @brief Stop communication and close transport.
     */
    void stop();

    /**
     * @brief Run one cyclic process-data exchange.
     */
    bool runCycle();

    bool setOutputByName(const std::string& logicalName, bool value);
    bool getInputByName(const std::string& logicalName, bool& value) const;
    /**
     * @brief Write raw bytes to output process image at specified offset.
     */
    bool writeOutputBytes(std::size_t byteOffset, const std::vector<std::uint8_t>& data);
    /**
     * @brief Read raw bytes from input process image.
     */
    bool readInputBytes(std::size_t byteOffset, std::size_t length,
                        std::vector<std::uint8_t>& outData) const;

    bool onInputChange(const std::string& logicalName, IoMapper::InputCallback callback);
    /**
     * @brief Replace state-machine transition options.
     */
    void setStateMachineOptions(StateMachineOptions options);
    /**
     * @brief Replace recovery policy options.
     */
    void setRecoveryOptions(RecoveryOptions options);
    /**
     * @brief Force a recovery action for a specific AL status code.
     */
    void setRecoveryActionOverride(std::uint16_t alStatusCode, RecoveryAction action);
    /**
     * @brief Clear all status-code specific recovery overrides.
     */
    void clearRecoveryActionOverrides();

    /**
     * @brief Read diagnostic snapshot for all configured slaves.
     */
    std::vector<SlaveDiagnostic> collectSlaveDiagnostics();
    /**
     * @brief Attempt recovery actions for currently diagnosed failures.
     */
    bool recoverNetwork();
    std::vector<RecoveryEvent> recoveryEvents() const;
    void clearRecoveryEvents();
    bool isDegraded() const;
    /**
     * @brief CoE SDO upload convenience wrapper.
     */
    SdoResponse sdoUpload(std::uint16_t slavePosition, SdoAddress address);
    /**
     * @brief CoE SDO download convenience wrapper.
     */
    SdoResponse sdoDownload(std::uint16_t slavePosition, SdoAddress address,
                            const std::vector<std::uint8_t>& data);
    bool configureRxPdo(std::uint16_t slavePosition, const std::vector<PdoMappingEntry>& entries,
                        std::string& outError);
    bool configureTxPdo(std::uint16_t slavePosition, const std::vector<PdoMappingEntry>& entries,
                        std::string& outError);
    std::vector<EmergencyMessage> drainEmergencies(std::size_t maxMessages);
    /**
     * @brief FoE read convenience wrapper.
     */
    FoEResponse foeReadFile(std::uint16_t slavePosition, const FoERequest& request);
    bool foeWriteFile(std::uint16_t slavePosition, const FoERequest& request,
                      const std::vector<std::uint8_t>& data, std::string& outError);
    bool eoeSendFrame(std::uint16_t slavePosition, const std::vector<std::uint8_t>& frame,
                      std::string& outError);
    bool eoeReceiveFrame(std::uint16_t slavePosition, std::vector<std::uint8_t>& frame,
                         std::string& outError);

    std::optional<std::int64_t> updateDistributedClock(std::int64_t referenceTimeNs,
                                                       std::int64_t localTimeNs);
    DcSyncStats distributedClockStats() const;
    /**
     * @brief Last applied DC correction from closed-loop mode (ns).
     */
    std::optional<std::int64_t> lastAppliedDcCorrectionNs() const;

    /**
     * @brief Refresh live topology snapshot from transport discovery.
     */
    bool refreshTopology(std::string& outError);
    TopologySnapshot topologySnapshot() const;
    std::vector<SlaveIdentity> hotConnectedSlaves() const;
    std::vector<SlaveIdentity> missingSlaves() const;
    HilConformanceReport evaluateHilConformance(double maxFailureRate,
                                                double maxP99RuntimeUs,
                                                std::uint64_t maxDegradedCycles,
                                                double observedP99RuntimeUs) const;

    std::uint16_t lastWorkingCounter() const noexcept;
    CycleStatistics statistics() const;

    std::string lastError() const;

private:
    struct DcClosedLoopOptions {
        bool enabled = false;
        std::uint16_t referenceSlavePosition = 1U;
        std::int64_t targetPhaseNs = 0;
        std::int64_t maxCorrectionStepNs = 20000;
        std::int64_t maxSlewPerCycleNs = 5000;
    };

    void setError(std::string message);
    void configureDcClosedLoopFromEnvironment();
    bool runDcClosedLoopUpdate();
    bool transitionNetworkTo(SlaveState target);
    bool transitionSlaveTo(std::uint16_t position, SlaveState target);
    bool recoverSlave(const SlaveDiagnostic& diagnostic);
    void appendRecoveryEvent(const RecoveryEvent& event);

    ITransport& transport_;
    CoeMailboxService mailbox_;
    FoeEoeService foeEoe_;
    DistributedClockController dcController_{};
    DcClosedLoopOptions dcClosedLoopOptions_{};
    std::optional<std::int64_t> lastAppliedDcCorrectionNs_;
    LinuxRawSocketTransport* dcLinuxTransport_ = nullptr;
    TopologyManager topologyManager_;
    IoMapper mapper_;
    NetworkConfiguration config_{};
    ProcessImage processImage_{0, 0};
    bool configured_ = false;
    bool started_ = false;
    mutable std::recursive_mutex mutex_;
    CycleStatistics statistics_{};
    StateMachineOptions stateMachineOptions_{};
    RecoveryOptions recoveryOptions_{};
    std::unordered_map<std::uint16_t, RecoveryAction> recoveryActionOverrides_;
    std::vector<SlaveDiagnostic> lastDiagnostics_;
    std::vector<RecoveryEvent> recoveryEvents_;
    std::unordered_map<std::uint16_t, std::size_t> retryCounts_;
    std::unordered_map<std::uint16_t, std::size_t> reconfigureCounts_;
    bool degraded_ = false;
    std::string error_;
};

} // namespace oec
