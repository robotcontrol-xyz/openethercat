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

namespace oec {

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

    // Optionally drive a full AL startup ladder so cyclic exchange starts from OP.
    if (stateMachineOptions_.enable) {
        if (!transitionNetworkTo(SlaveState::Init) ||
            !transitionNetworkTo(SlaveState::PreOp) ||
            !transitionNetworkTo(SlaveState::SafeOp) ||
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
    return dcController_.update({referenceTimeNs, localTimeNs});
}

DcSyncStats EthercatMaster::distributedClockStats() const {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    return dcController_.stats();
}

bool EthercatMaster::refreshTopology(std::string& outError) {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    return topologyManager_.refresh(outError);
}

TopologySnapshot EthercatMaster::topologySnapshot() const {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    return topologyManager_.snapshot();
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

void EthercatMaster::setError(std::string message) { error_ = std::move(message); }

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
