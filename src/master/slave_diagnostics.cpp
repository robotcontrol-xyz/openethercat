#include "openethercat/master/slave_diagnostics.hpp"

#include <algorithm>
#include <cctype>

namespace oec {

AlStatusInterpretation AlStatusDecoder::decode(std::uint16_t code) {
    switch (code) {
    case 0x0000:
        return {code, "NoError", "No AL status error", true};
    case 0x0011:
        return {code, "InvalidRequestedStateChange", "Requested state transition is not allowed", true};
    case 0x0012:
        return {code, "UnknownRequestedState", "Requested state is not recognized", false};
    case 0x0013:
        return {code, "BootstrapNotSupported", "Bootstrap transition unsupported", false};
    case 0x0014:
        return {code, "NoValidFirmware", "No valid firmware/application present", false};
    case 0x0015:
        return {code, "InvalidMailboxConfiguration", "Mailbox configuration invalid", true};
    case 0x0016:
        return {code, "InvalidMailboxConfiguration", "Mailbox setup conflict detected", true};
    case 0x0017:
        return {code, "InvalidSyncManagerConfiguration", "Sync manager assignment invalid", true};
    case 0x0018:
        return {code, "NoValidInputConfiguration", "Input process image configuration invalid", true};
    case 0x0019:
        return {code, "NoValidOutputConfiguration", "Output process image configuration invalid", true};
    case 0x001A:
        return {code, "SyncError", "Synchronization error in distributed clock / sync path", true};
    case 0x001B:
        return {code, "SyncManagerWatchdog", "Sync manager watchdog event", true};
    default:
        return {code, "Unknown", "Unrecognized AL status code", false};
    }
}

RecoveryAction RecoveryPolicy::decide(const SlaveDiagnostic& diagnostic,
                                      std::size_t retryCount,
                                      std::size_t reconfigureCount,
                                      std::size_t maxRetries,
                                      std::size_t maxReconfigure) {
    if (!diagnostic.available) {
        return RecoveryAction::Failover;
    }

    if (diagnostic.state == SlaveState::Op && diagnostic.alStatusCode == 0U) {
        return RecoveryAction::None;
    }

    if (diagnostic.alStatus.recoverable && retryCount < maxRetries) {
        return RecoveryAction::RetryTransition;
    }

    if (reconfigureCount < maxReconfigure) {
        return RecoveryAction::Reconfigure;
    }

    return RecoveryAction::Failover;
}

const char* toString(RecoveryAction action) {
    switch (action) {
    case RecoveryAction::None:
        return "None";
    case RecoveryAction::RetryTransition:
        return "RetryTransition";
    case RecoveryAction::Reconfigure:
        return "Reconfigure";
    case RecoveryAction::Failover:
        return "Failover";
    }
    return "Unknown";
}

std::optional<RecoveryAction> parseRecoveryAction(const std::string& actionText) {
    std::string normalized = actionText;
    std::transform(normalized.begin(), normalized.end(), normalized.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });

    if (normalized == "none") {
        return RecoveryAction::None;
    }
    if (normalized == "retrytransition" || normalized == "retry_transition" || normalized == "retry") {
        return RecoveryAction::RetryTransition;
    }
    if (normalized == "reconfigure") {
        return RecoveryAction::Reconfigure;
    }
    if (normalized == "failover") {
        return RecoveryAction::Failover;
    }
    return std::nullopt;
}

} // namespace oec
