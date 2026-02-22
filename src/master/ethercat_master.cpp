/**
 * @file ethercat_master.cpp
 * @brief openEtherCAT source file.
 */

#include "openethercat/master/ethercat_master.hpp"

#include <chrono>
#include <thread>
#include <exception>
#include <sstream>
#include <algorithm>
#include <cstdlib>
#include <cmath>
#include <type_traits>
#include <vector>
#include <iostream>

#include "openethercat/transport/linux_raw_socket_transport.hpp"

namespace oec {
namespace {

bool parseBoolEnv(const char* name, bool defaultValue) {
    const char* value = std::getenv(name);
    if (value == nullptr) {
        return defaultValue;
    }
    const std::string text(value);
    if (text == "1" || text == "true" || text == "TRUE" || text == "on" || text == "ON") {
        return true;
    }
    if (text == "0" || text == "false" || text == "FALSE" || text == "off" || text == "OFF") {
        return false;
    }
    return defaultValue;
}

template <typename T>
T parseIntegralEnv(const char* name, T defaultValue) {
    const char* value = std::getenv(name);
    if (value == nullptr) {
        return defaultValue;
    }
    try {
        if constexpr (std::is_signed<T>::value) {
            return static_cast<T>(std::stoll(value, nullptr, 0));
        } else {
            return static_cast<T>(std::stoull(value, nullptr, 0));
        }
    } catch (...) {
        return defaultValue;
    }
}

double parseDoubleEnv(const char* name, double defaultValue) {
    const char* value = std::getenv(name);
    if (value == nullptr) {
        return defaultValue;
    }
    try {
        return std::stod(value);
    } catch (...) {
        return defaultValue;
    }
}

EthercatMaster::DcPolicyAction parseDcPolicyAction(const char* text,
                                                   EthercatMaster::DcPolicyAction fallback) {
    if (text == nullptr) {
        return fallback;
    }
    const std::string value(text);
    if (value == "warn" || value == "WARN") {
        return EthercatMaster::DcPolicyAction::Warn;
    }
    if (value == "degrade" || value == "DEGRADE") {
        return EthercatMaster::DcPolicyAction::Degrade;
    }
    if (value == "recover" || value == "RECOVER") {
        return EthercatMaster::DcPolicyAction::Recover;
    }
    return fallback;
}

EthercatMaster::TopologyPolicyAction parseTopologyPolicyAction(
    const char* text, EthercatMaster::TopologyPolicyAction fallback) {
    if (text == nullptr) {
        return fallback;
    }
    const std::string value(text);
    if (value == "monitor" || value == "MONITOR") {
        return EthercatMaster::TopologyPolicyAction::Monitor;
    }
    if (value == "retry" || value == "RETRY") {
        return EthercatMaster::TopologyPolicyAction::Retry;
    }
    if (value == "reconfigure" || value == "RECONFIGURE") {
        return EthercatMaster::TopologyPolicyAction::Reconfigure;
    }
    if (value == "degrade" || value == "DEGRADE") {
        return EthercatMaster::TopologyPolicyAction::Degrade;
    }
    if (value == "failstop" || value == "FAILSTOP" || value == "fail-stop" || value == "FAIL-STOP") {
        return EthercatMaster::TopologyPolicyAction::FailStop;
    }
    return fallback;
}

std::int64_t percentileFromSorted(const std::vector<std::int64_t>& sorted,
                                  double percentile) {
    if (sorted.empty()) {
        return 0;
    }
    const auto index = static_cast<std::size_t>(
        std::ceil((percentile / 100.0) * static_cast<double>(sorted.size())) - 1.0);
    return sorted[std::min(index, sorted.size() - 1U)];
}

std::int64_t clampDcStep(std::int64_t correctionNs,
                         std::int64_t previousCorrectionNs,
                         std::int64_t maxStepNs,
                         std::int64_t maxSlewNs) {
    auto clamped = correctionNs;
    if (maxStepNs > 0 && std::llabs(clamped) > maxStepNs) {
        clamped = (clamped < 0) ? -maxStepNs : maxStepNs;
    }
    if (maxSlewNs > 0) {
        const auto delta = clamped - previousCorrectionNs;
        if (std::llabs(delta) > maxSlewNs) {
            clamped = previousCorrectionNs + ((delta < 0) ? -maxSlewNs : maxSlewNs);
        }
    }
    return clamped;
}

} // namespace

EthercatMaster::EthercatMaster(ITransport& transport)
    : transport_(transport), mailbox_(transport), foeEoe_(transport), topologyManager_(transport) {}

bool EthercatMaster::configure(const NetworkConfiguration& config) {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    // Reset all runtime state so reconfiguration is deterministic and idempotent.
    mapper_ = IoMapper{};
    config_ = config;
    processImage_ = ProcessImage(config.processImageInputBytes, config.processImageOutputBytes);
    statistics_ = CycleStatistics{};
    lastDiagnostics_.clear();
    retryCounts_.clear();
    reconfigureCounts_.clear();
    recoveryEvents_.clear();
    degraded_ = false;
    dcController_.reset();
    lastAppliedDcCorrectionNs_.reset();
    dcLinuxTransport_ = nullptr;
    dcSyncQuality_ = DcSyncQualitySnapshot{};
    dcPhaseErrorAbsHistoryNs_.clear();
    dcPolicyLatched_ = false;
    missingConditionCycles_ = 0;
    hotConnectConditionCycles_ = 0;
    redundancyConditionCycles_ = 0;
    missingPolicyLatched_ = false;
    hotConnectPolicyLatched_ = false;
    redundancyPolicyLatched_ = false;
    redundancyStatus_ = RedundancyStatusSnapshot{};
    redundancyKpis_ = RedundancyKpiSnapshot{};
    redundancyFaultActive_ = false;
    redundancyTransitions_.clear();
    dcTraceCounter_ = 0;

    // Validate before binding signals to avoid partially configured runtime state.
    const auto issues = ConfigurationValidator::validate(config);
    if (ConfigurationValidator::hasErrors(issues)) {
        std::ostringstream os;
        os << "Configuration invalid:";
        for (const auto& issue : issues) {
            if (issue.severity == ValidationSeverity::Error) {
                os << " " << issue.message << ";";
            }
        }
        setError(os.str());
        configured_ = false;
        return false;
    }

    // Pre-bind logical names so cycle-time lookups avoid repeated map construction.
    for (const auto& signal : config.signals) {
        if (!mapper_.bind(signal)) {
            setError("Duplicate logical signal name: " + signal.logicalName);
            configured_ = false;
            return false;
        }
    }

    configured_ = true;
    return true;
}

bool EthercatMaster::start() {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    if (!configured_) {
        setError("Master not configured");
        return false;
    }
    if (!transport_.open()) {
        setError("Transport open failed: " + transport_.lastError());
        return false;
    }
    configureDcClosedLoopFromEnvironment();
    configureTopologyRecoveryFromEnvironment();
    redundancyStatus_ = RedundancyStatusSnapshot{};
    redundancyStatus_.state = RedundancyState::PrimaryOnly;
    redundancyKpis_ = RedundancyKpiSnapshot{};
    redundancyFaultActive_ = false;
    redundancyTransitions_.clear();

    // Optionally drive a full AL startup ladder so cyclic exchange starts from OP.
    if (stateMachineOptions_.enable) {
        if (!transitionNetworkTo(SlaveState::Init) ||
            !transitionNetworkTo(SlaveState::PreOp)) {
            transport_.close();
            return false;
        }

        std::string processMapError;
        if (!transport_.configureProcessImage(config_, processMapError)) {
            setError("Failed to configure process image mapping: " + processMapError);
            transport_.close();
            return false;
        }

        if (!transitionNetworkTo(SlaveState::SafeOp) ||
            !transitionNetworkTo(SlaveState::Op)) {
            transport_.close();
            return false;
        }
    }

    degraded_ = false;
    started_ = true;
    return true;
}

void EthercatMaster::stop() {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    if (started_) {
        transport_.close();
        started_ = false;
    }
    dcLinuxTransport_ = nullptr;
}

bool EthercatMaster::runCycle() {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    if (!started_) {
        setError("Master not started");
        ++statistics_.cyclesFailed;
        ++statistics_.cyclesTotal;
        return false;
    }

    try {
        const auto begin = std::chrono::steady_clock::now();
        // Start from a copy of current input image; transport fills it in-place.
        auto rx = processImage_.inputBytes();
        if (!transport_.exchange(processImage_.outputBytes(), rx)) {
            setError("Transport exchange failed: " + transport_.lastError());
            if (recoveryOptions_.enable) {
                // Recovery is best-effort and can append contextual error details.
                const bool recovered = recoverNetwork();
                if (!recovered) {
                    setError(error_ + " | recovery failed");
                }
            }
            ++statistics_.cyclesFailed;
            ++statistics_.cyclesTotal;
            return false;
        }
        processImage_.inputBytes() = rx;
        statistics_.lastWorkingCounter = transport_.lastWorkingCounter();
        if (redundancyStatus_.state == RedundancyState::RedundancyDegraded ||
            redundancyStatus_.state == RedundancyState::Recovering) {
            ++redundancyKpis_.impactedCycles;
        }
        if (!runDcClosedLoopUpdate()) {
            ++statistics_.cyclesFailed;
            ++statistics_.cyclesTotal;
            return false;
        }
        // Dispatch callbacks only after a consistent full-image update.
        mapper_.dispatchInputChanges(processImage_);
        const auto end = std::chrono::steady_clock::now();
        statistics_.lastCycleRuntime =
            std::chrono::duration_cast<std::chrono::microseconds>(end - begin);
        ++statistics_.cyclesTotal;
        return true;
    } catch (const std::exception& ex) {
        setError(std::string("Cycle failed: ") + ex.what());
        ++statistics_.cyclesFailed;
        ++statistics_.cyclesTotal;
        return false;
    }
}

bool EthercatMaster::setOutputByName(const std::string& logicalName, bool value) {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    try {
        if (!mapper_.setOutput(processImage_, logicalName, value)) {
            setError("Unknown output signal or wrong direction: " + logicalName);
            return false;
        }
        return true;
    } catch (const std::exception& ex) {
        setError(std::string("Set output failed: ") + ex.what());
        return false;
    }
}

bool EthercatMaster::getInputByName(const std::string& logicalName, bool& value) const {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    try {
        return mapper_.getInput(processImage_, logicalName, value);
    } catch (...) {
        return false;
    }
}

bool EthercatMaster::writeOutputBytes(std::size_t byteOffset, const std::vector<std::uint8_t>& data) {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    const auto& output = processImage_.outputBytes();
    if (byteOffset > output.size()) {
        setError("writeOutputBytes byteOffset out of range");
        return false;
    }
    if (data.size() > (output.size() - byteOffset)) {
        setError("writeOutputBytes size out of range");
        return false;
    }

    auto updated = output;
    std::copy(data.begin(), data.end(), updated.begin() + static_cast<std::ptrdiff_t>(byteOffset));
    processImage_.outputBytes() = updated;
    return true;
}

bool EthercatMaster::readInputBytes(std::size_t byteOffset, std::size_t length,
                                    std::vector<std::uint8_t>& outData) const {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    const auto& input = processImage_.inputBytes();
    if (byteOffset > input.size()) {
        return false;
    }
    if (length > (input.size() - byteOffset)) {
        return false;
    }
    outData.assign(input.begin() + static_cast<std::ptrdiff_t>(byteOffset),
                   input.begin() + static_cast<std::ptrdiff_t>(byteOffset + length));
    return true;
}

bool EthercatMaster::onInputChange(const std::string& logicalName, IoMapper::InputCallback callback) {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    if (!mapper_.registerInputCallback(logicalName, std::move(callback))) {
        setError("Unknown input signal or wrong direction: " + logicalName);
        return false;
    }
    return true;
}

void EthercatMaster::setStateMachineOptions(StateMachineOptions options) {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    stateMachineOptions_ = options;
}

void EthercatMaster::setRecoveryOptions(RecoveryOptions options) {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    recoveryOptions_ = options;
    if (recoveryOptions_.maxEventHistory == 0U) {
        recoveryOptions_.maxEventHistory = 1U;
    }
}

void EthercatMaster::setTopologyRecoveryOptions(TopologyRecoveryOptions options) {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    topologyRecoveryOptions_ = options;
    if (topologyRecoveryOptions_.missingGraceCycles == 0U) {
        topologyRecoveryOptions_.missingGraceCycles = 1U;
    }
    if (topologyRecoveryOptions_.hotConnectGraceCycles == 0U) {
        topologyRecoveryOptions_.hotConnectGraceCycles = 1U;
    }
    if (topologyRecoveryOptions_.redundancyGraceCycles == 0U) {
        topologyRecoveryOptions_.redundancyGraceCycles = 1U;
    }
}

void EthercatMaster::setRecoveryActionOverride(std::uint16_t alStatusCode, RecoveryAction action) {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    recoveryActionOverrides_[alStatusCode] = action;
}

void EthercatMaster::clearRecoveryActionOverrides() {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    recoveryActionOverrides_.clear();
}

std::vector<SlaveDiagnostic> EthercatMaster::collectSlaveDiagnostics() {
    std::lock_guard<std::recursive_mutex> lock(mutex_);

    std::vector<SlaveDiagnostic> diagnostics;
    diagnostics.reserve(config_.slaves.size());
    for (const auto& slave : config_.slaves) {
        SlaveDiagnostic diagnostic;
        diagnostic.identity = slave;

        SlaveState state = SlaveState::Init;
        std::uint16_t alStatusCode = 0U;
        // Read both state and AL status code to classify recoverability.
        const bool hasState = transport_.readSlaveState(slave.position, state);
        const bool hasAlStatus = transport_.readSlaveAlStatusCode(slave.position, alStatusCode);
        diagnostic.available = hasState && hasAlStatus;
        if (diagnostic.available) {
            diagnostic.state = state;
            diagnostic.alStatusCode = alStatusCode;
            diagnostic.alStatus = AlStatusDecoder::decode(alStatusCode);
            // Override table allows deterministic policy behavior per AL status code.
            const auto overrideIt = recoveryActionOverrides_.find(alStatusCode);
            if (overrideIt != recoveryActionOverrides_.end()) {
                diagnostic.suggestedAction = overrideIt->second;
            } else {
                diagnostic.suggestedAction = RecoveryPolicy::decide(
                    diagnostic,
                    retryCounts_[slave.position],
                    reconfigureCounts_[slave.position],
                    recoveryOptions_.maxRetriesPerSlave,
                    recoveryOptions_.maxReconfigurePerSlave);
            }
        } else {
            diagnostic.alStatus = {0U, "Unavailable", transport_.lastError(), false};
            diagnostic.suggestedAction = RecoveryAction::Failover;
        }

        diagnostics.push_back(diagnostic);
    }

    lastDiagnostics_ = diagnostics;
    return diagnostics;
}

bool EthercatMaster::recoverNetwork() {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    const auto diagnostics = collectSlaveDiagnostics();
    if (diagnostics.empty()) {
        setError("No slaves available for recovery");
        return false;
    }

    // Continue across all diagnostics so one failing slave does not block others.
    bool recoveredAny = false;
    for (const auto& diagnostic : diagnostics) {
        if (diagnostic.suggestedAction == RecoveryAction::None) {
            continue;
        }
        if (recoverSlave(diagnostic)) {
            recoveredAny = true;
        }
    }
    return recoveredAny;
}

std::uint16_t EthercatMaster::lastWorkingCounter() const noexcept {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    return statistics_.lastWorkingCounter;
}

std::vector<EthercatMaster::RecoveryEvent> EthercatMaster::recoveryEvents() const {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    return recoveryEvents_;
}

void EthercatMaster::clearRecoveryEvents() {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    recoveryEvents_.clear();
}

bool EthercatMaster::isDegraded() const {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    return degraded_;
}

SdoResponse EthercatMaster::sdoUpload(std::uint16_t slavePosition, SdoAddress address) {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    return mailbox_.upload(slavePosition, address);
}

SdoResponse EthercatMaster::sdoDownload(std::uint16_t slavePosition, SdoAddress address,
                                        const std::vector<std::uint8_t>& data) {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    return mailbox_.download(slavePosition, address, data);
}

bool EthercatMaster::configureRxPdo(std::uint16_t slavePosition,
                                    const std::vector<PdoMappingEntry>& entries,
                                    std::string& outError) {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    return mailbox_.configureRxPdo(slavePosition, entries, outError);
}

bool EthercatMaster::configureTxPdo(std::uint16_t slavePosition,
                                    const std::vector<PdoMappingEntry>& entries,
                                    std::string& outError) {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    return mailbox_.configureTxPdo(slavePosition, entries, outError);
}

std::vector<EmergencyMessage> EthercatMaster::drainEmergencies(std::size_t maxMessages) {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    return mailbox_.drainEmergencyQueue(maxMessages);
}

FoEResponse EthercatMaster::foeReadFile(std::uint16_t slavePosition, const FoERequest& request) {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    return foeEoe_.readFile(slavePosition, request);
}

bool EthercatMaster::foeWriteFile(std::uint16_t slavePosition, const FoERequest& request,
                                  const std::vector<std::uint8_t>& data, std::string& outError) {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    return foeEoe_.writeFile(slavePosition, request, data, outError);
}

bool EthercatMaster::eoeSendFrame(std::uint16_t slavePosition, const std::vector<std::uint8_t>& frame,
                                  std::string& outError) {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    return foeEoe_.sendEthernetOverEthercat(slavePosition, frame, outError);
}

bool EthercatMaster::eoeReceiveFrame(std::uint16_t slavePosition, std::vector<std::uint8_t>& frame,
                                     std::string& outError) {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    return foeEoe_.receiveEthernetOverEthercat(slavePosition, frame, outError);
}

std::optional<std::int64_t> EthercatMaster::updateDistributedClock(std::int64_t referenceTimeNs,
                                                                   std::int64_t localTimeNs) {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    const auto correction = dcController_.update({referenceTimeNs, localTimeNs});
    updateDcSyncQualityLocked(referenceTimeNs - localTimeNs);
    return correction;
}

DcSyncStats EthercatMaster::distributedClockStats() const {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    return dcController_.stats();
}

EthercatMaster::DcSyncQualitySnapshot EthercatMaster::distributedClockQuality() const {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    return dcSyncQuality_;
}

std::optional<std::int64_t> EthercatMaster::lastAppliedDcCorrectionNs() const {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    return lastAppliedDcCorrectionNs_;
}

bool EthercatMaster::refreshTopology(std::string& outError) {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    if (!topologyManager_.refresh(outError)) {
        return false;
    }
    const auto changes = topologyManager_.changeSet();
    redundancyStatus_.redundancyHealthy = changes.redundancyHealthy;
    if (!topologyRecoveryOptions_.enable) {
        transitionRedundancyState(changes.redundancyHealthy ? RedundancyState::RedundantHealthy
                                                            : RedundancyState::RedundancyDegraded,
                                  changes.redundancyHealthy ? "redundancy healthy (policy disabled)"
                                                            : "redundancy degraded (policy disabled)",
                                  changes.generation);
    }
    if (topologyRecoveryOptions_.enable) {
        const auto missing = topologyManager_.detectMissing(config_.slaves);
        const auto hotConnected = topologyManager_.detectHotConnected(config_.slaves);
        applyTopologyPolicyIfNeeded(missing, hotConnected, changes.redundancyHealthy, changes.generation);
    }
    return true;
}

TopologySnapshot EthercatMaster::topologySnapshot() const {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    return topologyManager_.snapshot();
}

TopologyChangeSet EthercatMaster::topologyChangeSet() const {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    return topologyManager_.changeSet();
}

std::uint64_t EthercatMaster::topologyGeneration() const {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    return topologyManager_.generation();
}

std::vector<SlaveIdentity> EthercatMaster::hotConnectedSlaves() const {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    return topologyManager_.detectHotConnected(config_.slaves);
}

std::vector<SlaveIdentity> EthercatMaster::missingSlaves() const {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    return topologyManager_.detectMissing(config_.slaves);
}

HilConformanceReport EthercatMaster::evaluateHilConformance(double maxFailureRate,
                                                            double maxP99RuntimeUs,
                                                            std::uint64_t maxDegradedCycles,
                                                            double observedP99RuntimeUs) const {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    HilKpi kpi;
    kpi.cycles = statistics_.cyclesTotal;
    kpi.cycleFailures = statistics_.cyclesFailed;
    kpi.recoveryEvents = recoveryEvents_.size();
    kpi.cycleFailureRate = (kpi.cycles == 0U) ? 0.0
                                              : static_cast<double>(kpi.cycleFailures) /
                                                    static_cast<double>(kpi.cycles);
    kpi.p99CycleRuntimeUs = observedP99RuntimeUs;
    kpi.degradedCycles = degraded_ ? kpi.cycles : 0U;
    return HilCampaignEvaluator::evaluate(kpi, maxFailureRate, maxP99RuntimeUs, maxDegradedCycles);
}

CycleStatistics EthercatMaster::statistics() const {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    return statistics_;
}

std::string EthercatMaster::lastError() const {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    return error_;
}

EthercatMaster::RedundancyStatusSnapshot EthercatMaster::redundancyStatus() const {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    return redundancyStatus_;
}

EthercatMaster::RedundancyKpiSnapshot EthercatMaster::redundancyKpis() const {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    return redundancyKpis_;
}

std::vector<EthercatMaster::RedundancyTransitionEvent> EthercatMaster::redundancyTransitions() const {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    return redundancyTransitions_;
}

void EthercatMaster::clearRedundancyTransitions() {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    redundancyTransitions_.clear();
}

void EthercatMaster::setError(std::string message) { error_ = std::move(message); }

void EthercatMaster::configureDcClosedLoopFromEnvironment() {
    dcClosedLoopOptions_.enabled = parseBoolEnv("OEC_DC_CLOSED_LOOP", dcClosedLoopOptions_.enabled);
    dcClosedLoopOptions_.referenceSlavePosition =
        parseIntegralEnv<std::uint16_t>("OEC_DC_REFERENCE_SLAVE", dcClosedLoopOptions_.referenceSlavePosition);
    dcClosedLoopOptions_.targetPhaseNs =
        parseIntegralEnv<std::int64_t>("OEC_DC_TARGET_PHASE_NS", dcClosedLoopOptions_.targetPhaseNs);
    dcClosedLoopOptions_.maxCorrectionStepNs =
        parseIntegralEnv<std::int64_t>("OEC_DC_MAX_CORR_STEP_NS", dcClosedLoopOptions_.maxCorrectionStepNs);
    dcClosedLoopOptions_.maxSlewPerCycleNs =
        parseIntegralEnv<std::int64_t>("OEC_DC_MAX_SLEW_NS", dcClosedLoopOptions_.maxSlewPerCycleNs);
    dcSyncQualityOptions_.enabled = parseBoolEnv("OEC_DC_SYNC_MONITOR", dcSyncQualityOptions_.enabled);
    dcSyncQualityOptions_.maxPhaseErrorNs =
        parseIntegralEnv<std::int64_t>("OEC_DC_SYNC_MAX_PHASE_ERROR_NS", dcSyncQualityOptions_.maxPhaseErrorNs);
    dcSyncQualityOptions_.lockAcquireInWindowCycles =
        parseIntegralEnv<std::size_t>("OEC_DC_SYNC_LOCK_ACQUIRE_CYCLES",
                                      dcSyncQualityOptions_.lockAcquireInWindowCycles);
    dcSyncQualityOptions_.maxConsecutiveOutOfWindowCycles =
        parseIntegralEnv<std::size_t>("OEC_DC_SYNC_MAX_OOW_CYCLES",
                                      dcSyncQualityOptions_.maxConsecutiveOutOfWindowCycles);
    dcSyncQualityOptions_.historyWindowCycles =
        parseIntegralEnv<std::size_t>("OEC_DC_SYNC_HISTORY_WINDOW",
                                      dcSyncQualityOptions_.historyWindowCycles);
    dcSyncQualityOptions_.policyAction = parseDcPolicyAction(
        std::getenv("OEC_DC_SYNC_ACTION"),
        dcSyncQualityOptions_.policyAction);
    traceDc_ = parseBoolEnv("OEC_TRACE_DC", false);
    dcTraceCounter_ = 0;

    dcSyncQuality_ = DcSyncQualitySnapshot{};
    dcSyncQuality_.enabled = dcSyncQualityOptions_.enabled;
    dcPhaseErrorAbsHistoryNs_.clear();
    dcPolicyLatched_ = false;

    DistributedClockController::Options dcOptions{};
    dcOptions.filterAlpha = parseDoubleEnv("OEC_DC_FILTER_ALPHA", dcOptions.filterAlpha);
    dcOptions.kp = parseDoubleEnv("OEC_DC_KP", dcOptions.kp);
    dcOptions.ki = parseDoubleEnv("OEC_DC_KI", dcOptions.ki);
    dcOptions.correctionClampNs =
        parseIntegralEnv<std::int64_t>("OEC_DC_CORRECTION_CLAMP_NS", dcOptions.correctionClampNs);
    dcController_ = DistributedClockController(dcOptions);
    dcController_.reset();

    lastAppliedDcCorrectionNs_.reset();
    dcLinuxTransport_ = dynamic_cast<LinuxRawSocketTransport*>(&transport_);
}

void EthercatMaster::configureTopologyRecoveryFromEnvironment() {
    topologyRecoveryOptions_.enable =
        parseBoolEnv("OEC_TOPOLOGY_POLICY_ENABLE", topologyRecoveryOptions_.enable);
    topologyRecoveryOptions_.missingGraceCycles =
        parseIntegralEnv<std::size_t>("OEC_TOPOLOGY_MISSING_GRACE", topologyRecoveryOptions_.missingGraceCycles);
    topologyRecoveryOptions_.hotConnectGraceCycles =
        parseIntegralEnv<std::size_t>("OEC_TOPOLOGY_HOTCONNECT_GRACE", topologyRecoveryOptions_.hotConnectGraceCycles);
    topologyRecoveryOptions_.redundancyGraceCycles =
        parseIntegralEnv<std::size_t>("OEC_TOPOLOGY_REDUNDANCY_GRACE", topologyRecoveryOptions_.redundancyGraceCycles);
    topologyRecoveryOptions_.missingSlaveAction = parseTopologyPolicyAction(
        std::getenv("OEC_TOPOLOGY_MISSING_ACTION"),
        topologyRecoveryOptions_.missingSlaveAction);
    topologyRecoveryOptions_.hotConnectAction = parseTopologyPolicyAction(
        std::getenv("OEC_TOPOLOGY_HOTCONNECT_ACTION"),
        topologyRecoveryOptions_.hotConnectAction);
    topologyRecoveryOptions_.redundancyAction = parseTopologyPolicyAction(
        std::getenv("OEC_TOPOLOGY_REDUNDANCY_ACTION"),
        topologyRecoveryOptions_.redundancyAction);

    if (topologyRecoveryOptions_.missingGraceCycles == 0U) {
        topologyRecoveryOptions_.missingGraceCycles = 1U;
    }
    if (topologyRecoveryOptions_.hotConnectGraceCycles == 0U) {
        topologyRecoveryOptions_.hotConnectGraceCycles = 1U;
    }
    if (topologyRecoveryOptions_.redundancyGraceCycles == 0U) {
        topologyRecoveryOptions_.redundancyGraceCycles = 1U;
    }

    missingConditionCycles_ = 0;
    hotConnectConditionCycles_ = 0;
    redundancyConditionCycles_ = 0;
    missingPolicyLatched_ = false;
    hotConnectPolicyLatched_ = false;
    redundancyPolicyLatched_ = false;
    redundancyStatus_ = RedundancyStatusSnapshot{};
    redundancyStatus_.state = RedundancyState::PrimaryOnly;
    redundancyKpis_ = RedundancyKpiSnapshot{};
    redundancyFaultActive_ = false;
    redundancyTransitions_.clear();
    maxRedundancyTransitionHistory_ = parseIntegralEnv<std::size_t>(
        "OEC_TOPOLOGY_REDUNDANCY_HISTORY", maxRedundancyTransitionHistory_);
    if (maxRedundancyTransitionHistory_ == 0U) {
        maxRedundancyTransitionHistory_ = 1U;
    }
}

RecoveryAction EthercatMaster::mapTopologyActionToRecoveryAction(TopologyPolicyAction action) const {
    switch (action) {
    case TopologyPolicyAction::Monitor:
        return RecoveryAction::None;
    case TopologyPolicyAction::Retry:
        return RecoveryAction::RetryTransition;
    case TopologyPolicyAction::Reconfigure:
        return RecoveryAction::Reconfigure;
    case TopologyPolicyAction::Degrade:
    case TopologyPolicyAction::FailStop:
        return RecoveryAction::Failover;
    }
    return RecoveryAction::None;
}

void EthercatMaster::applyTopologyPolicyIfNeeded(const std::vector<SlaveIdentity>& missing,
                                                 const std::vector<SlaveIdentity>& hotConnected,
                                                 bool redundancyHealthy,
                                                 std::uint64_t topologyGeneration) {
    redundancyStatus_.redundancyHealthy = redundancyHealthy;
    const bool hasMissing = !missing.empty();
    const bool hasHotConnected = !hotConnected.empty();
    const bool redundancyDown = !redundancyHealthy;

    missingConditionCycles_ = hasMissing ? (missingConditionCycles_ + 1U) : 0U;
    hotConnectConditionCycles_ = hasHotConnected ? (hotConnectConditionCycles_ + 1U) : 0U;
    redundancyConditionCycles_ = redundancyDown ? (redundancyConditionCycles_ + 1U) : 0U;

    if (!hasMissing) {
        missingPolicyLatched_ = false;
    }
    if (!hasHotConnected) {
        hotConnectPolicyLatched_ = false;
    }
    if (!redundancyDown) {
        redundancyPolicyLatched_ = false;
    }

    if (redundancyDown && !redundancyFaultActive_) {
        redundancyFaultActive_ = true;
        redundancyFaultStart_ = std::chrono::steady_clock::now();
        ++redundancyKpis_.degradeEvents;
        transitionRedundancyState(RedundancyState::RedundancyDegraded,
                                  "redundancy down detected",
                                  topologyGeneration);
        redundancyKpis_.lastDetectionLatencyMs = 0;
    } else if (!redundancyDown && redundancyFaultActive_) {
        redundancyFaultActive_ = false;
        redundancyRecoveryStart_ = std::chrono::steady_clock::now();
        ++redundancyKpis_.recoverEvents;
        transitionRedundancyState(RedundancyState::Recovering,
                                  "redundancy link restored",
                                  topologyGeneration);
    }

    auto emitEvent = [&](std::uint16_t slavePosition,
                         TopologyPolicyAction policyAction,
                         bool success,
                         const std::string& reason) {
        RecoveryEvent event;
        event.timestamp = std::chrono::system_clock::now();
        event.cycleIndex = statistics_.cyclesTotal;
        event.slavePosition = slavePosition;
        event.alStatusCode = 0U;
        event.action = mapTopologyActionToRecoveryAction(policyAction);
        event.success = success;
        event.message = "topology_generation=" + std::to_string(topologyGeneration) + " " + reason;
        appendRecoveryEvent(event);
    };

    if (hasMissing &&
        !missingPolicyLatched_ &&
        missingConditionCycles_ >= topologyRecoveryOptions_.missingGraceCycles) {
        switch (topologyRecoveryOptions_.missingSlaveAction) {
        case TopologyPolicyAction::Monitor:
            emitEvent(missing.front().position, topologyRecoveryOptions_.missingSlaveAction, true,
                      "missing-slave monitor");
            break;
        case TopologyPolicyAction::Retry: {
            const bool ok = recoverNetwork();
            emitEvent(missing.front().position, topologyRecoveryOptions_.missingSlaveAction, ok,
                      ok ? "missing-slave retry recovery succeeded" : "missing-slave retry recovery failed");
            break;
        }
        case TopologyPolicyAction::Reconfigure: {
            const bool ok = recoverNetwork();
            emitEvent(missing.front().position, topologyRecoveryOptions_.missingSlaveAction, ok,
                      ok ? "missing-slave reconfigure recovery succeeded"
                         : "missing-slave reconfigure recovery failed");
            break;
        }
        case TopologyPolicyAction::Degrade:
            degraded_ = true;
            emitEvent(missing.front().position, topologyRecoveryOptions_.missingSlaveAction, true,
                      "missing-slave degraded");
            break;
        case TopologyPolicyAction::FailStop:
            degraded_ = true;
            started_ = false;
            transport_.close();
            emitEvent(missing.front().position, topologyRecoveryOptions_.missingSlaveAction, true,
                      "missing-slave fail-stop");
            break;
        }
        missingPolicyLatched_ = true;
    }

    if (hasHotConnected &&
        !hotConnectPolicyLatched_ &&
        hotConnectConditionCycles_ >= topologyRecoveryOptions_.hotConnectGraceCycles) {
        switch (topologyRecoveryOptions_.hotConnectAction) {
        case TopologyPolicyAction::Monitor:
            emitEvent(hotConnected.front().position, topologyRecoveryOptions_.hotConnectAction, true,
                      "hot-connect monitor");
            break;
        case TopologyPolicyAction::Retry: {
            const bool ok = recoverNetwork();
            emitEvent(hotConnected.front().position, topologyRecoveryOptions_.hotConnectAction, ok,
                      ok ? "hot-connect retry recovery succeeded" : "hot-connect retry recovery failed");
            break;
        }
        case TopologyPolicyAction::Reconfigure: {
            const bool ok = recoverNetwork();
            emitEvent(hotConnected.front().position, topologyRecoveryOptions_.hotConnectAction, ok,
                      ok ? "hot-connect reconfigure recovery succeeded"
                         : "hot-connect reconfigure recovery failed");
            break;
        }
        case TopologyPolicyAction::Degrade:
            degraded_ = true;
            emitEvent(hotConnected.front().position, topologyRecoveryOptions_.hotConnectAction, true,
                      "hot-connect degraded");
            break;
        case TopologyPolicyAction::FailStop:
            degraded_ = true;
            started_ = false;
            transport_.close();
            emitEvent(hotConnected.front().position, topologyRecoveryOptions_.hotConnectAction, true,
                      "hot-connect fail-stop");
            break;
        }
        hotConnectPolicyLatched_ = true;
    }

    if (redundancyDown &&
        !redundancyPolicyLatched_ &&
        redundancyConditionCycles_ >= topologyRecoveryOptions_.redundancyGraceCycles) {
        if (redundancyKpis_.lastDetectionLatencyMs < 0) {
            const auto now = std::chrono::steady_clock::now();
            redundancyKpis_.lastDetectionLatencyMs = std::chrono::duration_cast<std::chrono::milliseconds>(
                now - redundancyFaultStart_).count();
        }
        redundancyKpis_.lastPolicyTriggerLatencyMs = redundancyKpis_.lastDetectionLatencyMs;
        switch (topologyRecoveryOptions_.redundancyAction) {
        case TopologyPolicyAction::Monitor:
            emitEvent(0U, topologyRecoveryOptions_.redundancyAction, true, "redundancy-down monitor");
            break;
        case TopologyPolicyAction::Retry: {
            const bool ok = recoverNetwork();
            emitEvent(0U, topologyRecoveryOptions_.redundancyAction, ok,
                      ok ? "redundancy-down retry recovery succeeded" : "redundancy-down retry recovery failed");
            break;
        }
        case TopologyPolicyAction::Reconfigure: {
            const bool ok = recoverNetwork();
            emitEvent(0U, topologyRecoveryOptions_.redundancyAction, ok,
                      ok ? "redundancy-down reconfigure recovery succeeded"
                         : "redundancy-down reconfigure recovery failed");
            break;
        }
        case TopologyPolicyAction::Degrade:
            degraded_ = true;
            emitEvent(0U, topologyRecoveryOptions_.redundancyAction, true, "redundancy-down degraded");
            break;
        case TopologyPolicyAction::FailStop:
            degraded_ = true;
            started_ = false;
            transport_.close();
            emitEvent(0U, topologyRecoveryOptions_.redundancyAction, true, "redundancy-down fail-stop");
            break;
        }
        redundancyPolicyLatched_ = true;
    }

    if (!redundancyDown && redundancyStatus_.state == RedundancyState::Recovering) {
        const auto now = std::chrono::steady_clock::now();
        redundancyKpis_.lastRecoveryLatencyMs =
            std::chrono::duration_cast<std::chrono::milliseconds>(now - redundancyRecoveryStart_).count();
        transitionRedundancyState(RedundancyState::RedundantHealthy,
                                  "redundancy healthy",
                                  topologyGeneration);
    }
}

void EthercatMaster::transitionRedundancyState(RedundancyState newState,
                                               const std::string& reason,
                                               std::uint64_t topologyGeneration) {
    if (redundancyStatus_.state == newState) {
        redundancyStatus_.lastReason = reason;
        return;
    }
    RedundancyTransitionEvent event;
    event.timestamp = std::chrono::system_clock::now();
    event.cycleIndex = statistics_.cyclesTotal;
    event.topologyGeneration = topologyGeneration;
    event.from = redundancyStatus_.state;
    event.to = newState;
    event.reason = reason;
    redundancyTransitions_.push_back(std::move(event));
    if (redundancyTransitions_.size() > maxRedundancyTransitionHistory_) {
        const auto overflow = redundancyTransitions_.size() - maxRedundancyTransitionHistory_;
        redundancyTransitions_.erase(redundancyTransitions_.begin(),
                                     redundancyTransitions_.begin() + static_cast<std::ptrdiff_t>(overflow));
    }
    redundancyStatus_.state = newState;
    redundancyStatus_.lastReason = reason;
    ++redundancyStatus_.transitionCount;
}

bool EthercatMaster::runDcClosedLoopUpdate() {
    if (!dcClosedLoopOptions_.enabled) {
        return true;
    }
    if (dcLinuxTransport_ == nullptr) {
        setError("DC closed-loop requires LinuxRawSocketTransport");
        return false;
    }

    std::int64_t slaveTimeNs = 0;
    std::string dcError;
    if (!dcLinuxTransport_->readDcSystemTime(dcClosedLoopOptions_.referenceSlavePosition, slaveTimeNs, dcError)) {
        setError("DC read failed: " + dcError);
        return false;
    }

    const auto hostNow = std::chrono::steady_clock::now().time_since_epoch();
    const auto hostTimeNs = std::chrono::duration_cast<std::chrono::nanoseconds>(hostNow).count() +
                            dcClosedLoopOptions_.targetPhaseNs;

    DcSyncSample sample;
    sample.referenceTimeNs = slaveTimeNs;
    sample.localTimeNs = hostTimeNs;
    const bool wasLocked = dcSyncQuality_.locked;
    const auto correction = dcController_.update(sample);
    updateDcSyncQualityLocked(sample.referenceTimeNs - sample.localTimeNs);
    if (!correction.has_value()) {
        return true;
    }

    const auto previous = lastAppliedDcCorrectionNs_.value_or(0);
    const auto safeCorrection = clampDcStep(*correction,
                                            previous,
                                            dcClosedLoopOptions_.maxCorrectionStepNs,
                                            dcClosedLoopOptions_.maxSlewPerCycleNs);
    if (!dcLinuxTransport_->writeDcSystemTimeOffset(dcClosedLoopOptions_.referenceSlavePosition,
                                                    safeCorrection, dcError)) {
        setError("DC write failed: " + dcError);
        return false;
    }

    lastAppliedDcCorrectionNs_ = safeCorrection;
    if (traceDc_) {
        const bool isLocked = dcSyncQuality_.locked;
        const char* transition = "none";
        if (!wasLocked && isLocked) {
            transition = "acquired";
        } else if (wasLocked && !isLocked) {
            transition = "lost";
        }
        std::cout << "[oec-dc] cycle=" << dcTraceCounter_
                  << " ref_slave=" << dcClosedLoopOptions_.referenceSlavePosition
                  << " ref_ns=" << sample.referenceTimeNs
                  << " host_ns=" << sample.localTimeNs
                  << " phase_err_ns=" << (sample.referenceTimeNs - sample.localTimeNs)
                  << " raw_corr_ns=" << *correction
                  << " applied_corr_ns=" << safeCorrection
                  << " lock=" << (isLocked ? "1" : "0")
                  << " lock_transition=" << transition
                  << " jitter_p95_ns=" << dcSyncQuality_.jitterP95Ns
                  << " jitter_p99_ns=" << dcSyncQuality_.jitterP99Ns
                  << '\n';
    }
    ++dcTraceCounter_;
    return true;
}

void EthercatMaster::updateDcSyncQualityLocked(std::int64_t phaseErrorNs) {
    if (!dcSyncQualityOptions_.enabled) {
        return;
    }
    dcSyncQuality_.enabled = true;
    dcSyncQuality_.lastPhaseErrorNs = phaseErrorNs;
    ++dcSyncQuality_.samples;

    const auto absError = static_cast<std::int64_t>(std::llabs(phaseErrorNs));
    const bool inWindow = absError <= dcSyncQualityOptions_.maxPhaseErrorNs;
    if (inWindow) {
        ++dcSyncQuality_.consecutiveInWindowCycles;
        dcSyncQuality_.consecutiveOutOfWindowCycles = 0U;
        if (!dcSyncQuality_.locked &&
            dcSyncQuality_.consecutiveInWindowCycles >= dcSyncQualityOptions_.lockAcquireInWindowCycles) {
            dcSyncQuality_.locked = true;
            ++dcSyncQuality_.lockAcquisitions;
            dcPolicyLatched_ = false;
        }
    } else {
        dcSyncQuality_.consecutiveInWindowCycles = 0U;
        ++dcSyncQuality_.consecutiveOutOfWindowCycles;
        if (dcSyncQuality_.locked) {
            dcSyncQuality_.locked = false;
            ++dcSyncQuality_.lockLosses;
        }
        if (dcSyncQuality_.consecutiveOutOfWindowCycles >=
            dcSyncQualityOptions_.maxConsecutiveOutOfWindowCycles) {
            applyDcPolicyLocked();
        }
    }

    dcPhaseErrorAbsHistoryNs_.push_back(absError);
    while (dcPhaseErrorAbsHistoryNs_.size() > dcSyncQualityOptions_.historyWindowCycles) {
        dcPhaseErrorAbsHistoryNs_.pop_front();
    }
    std::vector<std::int64_t> sorted(dcPhaseErrorAbsHistoryNs_.begin(), dcPhaseErrorAbsHistoryNs_.end());
    std::sort(sorted.begin(), sorted.end());
    dcSyncQuality_.jitterP50Ns = percentileFromSorted(sorted, 50.0);
    dcSyncQuality_.jitterP95Ns = percentileFromSorted(sorted, 95.0);
    dcSyncQuality_.jitterP99Ns = percentileFromSorted(sorted, 99.0);
    dcSyncQuality_.jitterMaxNs = sorted.empty() ? 0 : sorted.back();
}

void EthercatMaster::applyDcPolicyLocked() {
    if (dcPolicyLatched_) {
        return;
    }
    ++dcSyncQuality_.policyTriggers;
    switch (dcSyncQualityOptions_.policyAction) {
    case DcPolicyAction::Warn:
        setError("DC sync out-of-window threshold exceeded");
        break;
    case DcPolicyAction::Degrade:
        degraded_ = true;
        setError("DC sync degraded: out-of-window threshold exceeded");
        break;
    case DcPolicyAction::Recover:
        setError("DC sync recovery requested due to out-of-window threshold");
        if (started_) {
            (void)recoverNetwork();
        }
        break;
    }
    dcPolicyLatched_ = true;
}

bool EthercatMaster::transitionNetworkTo(SlaveState target) {
    if (!transport_.requestNetworkState(target)) {
        setError("Failed to request state " + std::string(toString(target)) + ": " + transport_.lastError());
        return false;
    }

    const auto deadline = std::chrono::steady_clock::now() + stateMachineOptions_.transitionTimeout;
    while (std::chrono::steady_clock::now() < deadline) {
        SlaveState state = SlaveState::Init;
        if (!transport_.readNetworkState(state)) {
            setError("Failed to read network state: " + transport_.lastError());
            return false;
        }
        if (state == target) {
            return true;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(stateMachineOptions_.pollIntervalMs));
    }

    setError("Timeout waiting for state " + std::string(toString(target)));
    return false;
}

bool EthercatMaster::transitionSlaveTo(std::uint16_t position, SlaveState target) {
    if (!transport_.requestSlaveState(position, target)) {
        setError("Failed to request slave " + std::to_string(position) + " state " +
                 std::string(toString(target)) + ": " + transport_.lastError());
        return false;
    }

    const auto deadline = std::chrono::steady_clock::now() + stateMachineOptions_.transitionTimeout;
    while (std::chrono::steady_clock::now() < deadline) {
        SlaveState state = SlaveState::Init;
        if (!transport_.readSlaveState(position, state)) {
            setError("Failed to read slave state for position " + std::to_string(position) + ": " +
                     transport_.lastError());
            return false;
        }
        if (state == target) {
            return true;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(stateMachineOptions_.pollIntervalMs));
    }

    setError("Timeout waiting for slave " + std::to_string(position) + " state " +
             std::string(toString(target)));
    return false;
}

bool EthercatMaster::recoverSlave(const SlaveDiagnostic& diagnostic) {
    const auto position = diagnostic.identity.position;
    RecoveryEvent event;
    event.timestamp = std::chrono::system_clock::now();
    event.cycleIndex = statistics_.cyclesTotal;
    event.slavePosition = position;
    event.alStatusCode = diagnostic.alStatusCode;
    event.action = diagnostic.suggestedAction;

    switch (diagnostic.suggestedAction) {
    case RecoveryAction::None:
        event.success = true;
        event.message = "No recovery needed";
        appendRecoveryEvent(event);
        return true;
    case RecoveryAction::RetryTransition:
        ++retryCounts_[position];
        event.success = transitionSlaveTo(position, SlaveState::Op);
        event.message = event.success ? "Retry transition to OP succeeded" : error_;
        appendRecoveryEvent(event);
        return event.success;
    case RecoveryAction::Reconfigure:
        ++reconfigureCounts_[position];
        if (!transport_.reconfigureSlave(position)) {
            setError("Reconfigure failed for slave " + std::to_string(position) + ": " +
                     transport_.lastError());
            event.success = false;
            event.message = error_;
            appendRecoveryEvent(event);
            return false;
        }
        event.success = transitionSlaveTo(position, SlaveState::Op);
        event.message = event.success ? "Reconfigure + transition to OP succeeded" : error_;
        appendRecoveryEvent(event);
        return event.success;
    case RecoveryAction::Failover:
        if (!transport_.failoverSlave(position)) {
            setError("Failover failed for slave " + std::to_string(position) + ": " +
                     transport_.lastError());
            event.success = false;
            event.message = error_;
            appendRecoveryEvent(event);
            return false;
        }
        degraded_ = true;
        if (recoveryOptions_.stopMasterOnFailover) {
            started_ = false;
            transport_.close();
            setError("Failover triggered master stop for slave " + std::to_string(position));
            event.success = true;
            event.message = error_;
        } else {
            event.success = true;
            event.message = "Slave moved to failover/degraded mode";
        }
        appendRecoveryEvent(event);
        return true;
    }
    return false;
}

void EthercatMaster::appendRecoveryEvent(const RecoveryEvent& event) {
    recoveryEvents_.push_back(event);
    if (recoveryEvents_.size() > recoveryOptions_.maxEventHistory) {
        const auto overflow = recoveryEvents_.size() - recoveryOptions_.maxEventHistory;
        recoveryEvents_.erase(recoveryEvents_.begin(),
                              recoveryEvents_.begin() + static_cast<std::ptrdiff_t>(overflow));
    }
}

} // namespace oec
