#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "openethercat/config/eni_esi_models.hpp"
#include "openethercat/core/slave_state.hpp"

namespace oec {

enum class RecoveryAction {
    None,
    RetryTransition,
    Reconfigure,
    Failover,
};

struct AlStatusInterpretation {
    std::uint16_t code = 0;
    std::string name;
    std::string description;
    bool recoverable = true;
};

struct SlaveDiagnostic {
    SlaveIdentity identity;
    bool available = false;
    SlaveState state = SlaveState::Init;
    std::uint16_t alStatusCode = 0;
    AlStatusInterpretation alStatus;
    RecoveryAction suggestedAction = RecoveryAction::None;
};

class AlStatusDecoder {
public:
    static AlStatusInterpretation decode(std::uint16_t code);
};

class RecoveryPolicy {
public:
    static RecoveryAction decide(const SlaveDiagnostic& diagnostic,
                                 std::size_t retryCount,
                                 std::size_t reconfigureCount,
                                 std::size_t maxRetries,
                                 std::size_t maxReconfigure);
};

std::optional<RecoveryAction> parseRecoveryAction(const std::string& actionText);
const char* toString(RecoveryAction action);

} // namespace oec
