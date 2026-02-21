/**
 * @file slave_state.hpp
 * @brief openEtherCAT source file.
 */

#pragma once

#include <cstdint>
#include <string>

namespace oec {

enum class SlaveState : std::uint8_t {
    Init = 0x01,
    PreOp = 0x02,
    SafeOp = 0x04,
    Op = 0x08,
};

inline const char* toString(SlaveState state) {
    switch (state) {
    case SlaveState::Init:
        return "INIT";
    case SlaveState::PreOp:
        return "PRE-OP";
    case SlaveState::SafeOp:
        return "SAFE-OP";
    case SlaveState::Op:
        return "OP";
    }
    return "UNKNOWN";
}

} // namespace oec
