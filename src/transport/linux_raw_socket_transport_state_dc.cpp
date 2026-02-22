/**
 * @file linux_raw_socket_transport_state_dc.cpp
 * @brief AL-state and DC register operations for LinuxRawSocketTransport.
 */

#include "openethercat/transport/linux_raw_socket_transport.hpp"

namespace oec {
namespace {

constexpr std::uint8_t kCommandBrd = 0x07;
constexpr std::uint8_t kCommandBwr = 0x08;
constexpr std::uint8_t kCommandAprd = 0x01;
constexpr std::uint8_t kCommandApwr = 0x02;
constexpr std::uint16_t kRegisterAlControl = 0x0120;
constexpr std::uint16_t kRegisterAlStatus = 0x0130;
constexpr std::uint16_t kRegisterAlStatusCode = 0x0134;
constexpr std::uint16_t kRegisterDcSystemTime = 0x0910;
constexpr std::uint16_t kRegisterDcSystemTimeOffset = 0x0920;
constexpr std::uint16_t kAlStateMask = 0x000F;

bool decodeAlState(std::uint16_t rawState, SlaveState& out) {
    switch (rawState & kAlStateMask) {
    case 0x01:
        out = SlaveState::Init;
        return true;
    case 0x02:
        out = SlaveState::PreOp;
        return true;
    case 0x03:
        out = SlaveState::Bootstrap;
        return true;
    case 0x04:
        out = SlaveState::SafeOp;
        return true;
    case 0x08:
        out = SlaveState::Op;
        return true;
    default:
        return false;
    }
}

std::uint16_t toAutoIncrementAddress(std::uint16_t position) {
    return static_cast<std::uint16_t>(0U - position);
}

std::int64_t readLe64Signed(const std::vector<std::uint8_t>& data, std::size_t offset) {
    std::uint64_t v = 0U;
    for (std::size_t i = 0; i < 8U; ++i) {
        v |= (static_cast<std::uint64_t>(data[offset + i]) << (8U * i));
    }
    return static_cast<std::int64_t>(v);
}

void writeLe64Signed(std::vector<std::uint8_t>& out, std::int64_t value) {
    const auto u = static_cast<std::uint64_t>(value);
    for (std::size_t i = 0; i < 8U; ++i) {
        out.push_back(static_cast<std::uint8_t>((u >> (8U * i)) & 0xFFU));
    }
}

} // namespace

bool LinuxRawSocketTransport::requestNetworkState(SlaveState state) {
    if (socketFd_ < 0) {
        error_ = "transport not open";
        return false;
    }

    EthercatDatagramRequest request;
    request.command = kCommandBwr;
    request.datagramIndex = datagramIndex_++;
    request.adp = 0x0000;
    request.ado = kRegisterAlControl;
    request.payload = {static_cast<std::uint8_t>(state), 0x00U};

    std::uint16_t wkc = 0;
    std::vector<std::uint8_t> payload;
    if (!sendDatagramRequest(request, wkc, payload, error_)) {
        return false;
    }

    lastWorkingCounter_ = wkc;
    return true;
}

bool LinuxRawSocketTransport::readNetworkState(SlaveState& outState) {
    if (socketFd_ < 0) {
        error_ = "transport not open";
        return false;
    }

    EthercatDatagramRequest request;
    request.command = kCommandBrd;
    request.datagramIndex = datagramIndex_++;
    request.adp = 0x0000;
    request.ado = kRegisterAlStatus;
    request.payload = {0x00U, 0x00U};

    std::uint16_t wkc = 0;
    std::vector<std::uint8_t> payload;
    if (!sendDatagramRequest(request, wkc, payload, error_)) {
        return false;
    }
    if (payload.size() < 2U) {
        error_ = "state read payload too short";
        return false;
    }

    const auto raw = static_cast<std::uint16_t>(
        static_cast<std::uint16_t>(payload[0]) |
        (static_cast<std::uint16_t>(payload[1]) << 8U));
    SlaveState decoded = SlaveState::Init;
    if (!decodeAlState(raw, decoded)) {
        error_ = "unknown AL state value";
        return false;
    }

    lastWorkingCounter_ = wkc;
    outState = decoded;
    return true;
}

bool LinuxRawSocketTransport::requestSlaveState(std::uint16_t position, SlaveState state) {
    if (socketFd_ < 0) {
        error_ = "transport not open";
        return false;
    }

    EthercatDatagramRequest request;
    request.command = kCommandApwr;
    request.datagramIndex = datagramIndex_++;
    request.adp = toAutoIncrementAddress(position);
    request.ado = kRegisterAlControl;
    request.payload = {static_cast<std::uint8_t>(state), 0x00U};

    std::uint16_t wkc = 0;
    std::vector<std::uint8_t> payload;
    if (!sendDatagramRequest(request, wkc, payload, error_)) {
        return false;
    }

    lastWorkingCounter_ = wkc;
    return true;
}

bool LinuxRawSocketTransport::readSlaveState(std::uint16_t position, SlaveState& outState) {
    if (socketFd_ < 0) {
        error_ = "transport not open";
        return false;
    }

    EthercatDatagramRequest request;
    request.command = kCommandAprd;
    request.datagramIndex = datagramIndex_++;
    request.adp = toAutoIncrementAddress(position);
    request.ado = kRegisterAlStatus;
    request.payload = {0x00U, 0x00U};

    std::uint16_t wkc = 0;
    std::vector<std::uint8_t> payload;
    if (!sendDatagramRequest(request, wkc, payload, error_)) {
        return false;
    }
    if (payload.size() < 2U) {
        error_ = "state read payload too short";
        return false;
    }

    const auto raw = static_cast<std::uint16_t>(
        static_cast<std::uint16_t>(payload[0]) |
        (static_cast<std::uint16_t>(payload[1]) << 8U));
    SlaveState decoded = SlaveState::Init;
    if (!decodeAlState(raw, decoded)) {
        error_ = "unknown AL state value";
        return false;
    }

    lastWorkingCounter_ = wkc;
    outState = decoded;
    return true;
}

bool LinuxRawSocketTransport::readSlaveAlStatusCode(std::uint16_t position, std::uint16_t& outCode) {
    if (socketFd_ < 0) {
        error_ = "transport not open";
        return false;
    }

    EthercatDatagramRequest request;
    request.command = kCommandAprd;
    request.datagramIndex = datagramIndex_++;
    request.adp = toAutoIncrementAddress(position);
    request.ado = kRegisterAlStatusCode;
    request.payload = {0x00U, 0x00U};

    std::uint16_t wkc = 0;
    std::vector<std::uint8_t> payload;
    if (!sendDatagramRequest(request, wkc, payload, error_)) {
        return false;
    }
    if (payload.size() < 2U) {
        error_ = "AL status code payload too short";
        return false;
    }

    outCode = static_cast<std::uint16_t>(
        static_cast<std::uint16_t>(payload[0]) |
        (static_cast<std::uint16_t>(payload[1]) << 8U));
    lastWorkingCounter_ = wkc;
    return true;
}

bool LinuxRawSocketTransport::readDcSystemTime(std::uint16_t slavePosition,
                                               std::int64_t& outSlaveTimeNs,
                                               std::string& outError) {
    ++dcDiagnostics_.readAttempts;
    outError.clear();
    if (socketFd_ < 0) {
        outError = "transport not open";
        ++dcDiagnostics_.readFailure;
        return false;
    }

    EthercatDatagramRequest request;
    request.command = kCommandAprd;
    request.datagramIndex = datagramIndex_++;
    request.adp = toAutoIncrementAddress(slavePosition);
    request.ado = kRegisterDcSystemTime;
    request.payload.assign(8U, 0U);

    std::uint16_t wkc = 0;
    std::vector<std::uint8_t> payload;
    if (!sendDatagramRequest(request, wkc, payload, outError)) {
        ++dcDiagnostics_.readFailure;
        return false;
    }
    if (payload.size() < 8U) {
        outError = "DC system time payload too short";
        ++dcDiagnostics_.readInvalidPayload;
        ++dcDiagnostics_.readFailure;
        return false;
    }

    outSlaveTimeNs = readLe64Signed(payload, 0U);
    lastWorkingCounter_ = wkc;
    ++dcDiagnostics_.readSuccess;
    return true;
}

bool LinuxRawSocketTransport::writeDcSystemTimeOffset(std::uint16_t slavePosition,
                                                      std::int64_t offsetNs,
                                                      std::string& outError) {
    ++dcDiagnostics_.writeAttempts;
    outError.clear();
    if (socketFd_ < 0) {
        outError = "transport not open";
        ++dcDiagnostics_.writeFailure;
        return false;
    }

    EthercatDatagramRequest request;
    request.command = kCommandApwr;
    request.datagramIndex = datagramIndex_++;
    request.adp = toAutoIncrementAddress(slavePosition);
    request.ado = kRegisterDcSystemTimeOffset;
    request.payload.clear();
    request.payload.reserve(8U);
    writeLe64Signed(request.payload, offsetNs);

    std::uint16_t wkc = 0;
    std::vector<std::uint8_t> payload;
    if (!sendDatagramRequest(request, wkc, payload, outError)) {
        ++dcDiagnostics_.writeFailure;
        return false;
    }

    lastWorkingCounter_ = wkc;
    ++dcDiagnostics_.writeSuccess;
    return true;
}

} // namespace oec
