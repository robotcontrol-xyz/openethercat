/**
 * @file mock_transport.cpp
 * @brief openEtherCAT source file.
 */

#include "openethercat/transport/mock_transport.hpp"

#include <algorithm>
#include <cstdint>
#include <functional>
#include <stdexcept>

namespace oec {

MockTransport::MockTransport(std::size_t inputBytes, std::size_t outputBytes)
    : inputs_(inputBytes, 0U), lastOutputs_(outputBytes, 0U) {}

bool MockTransport::open() {
    opened_ = true;
    lastWorkingCounter_ = 0;
    state_ = SlaveState::Init;
    perSlaveState_.clear();
    perSlaveAlStatusCode_.clear();
    pdoAssignments_.clear();
    while (!emergencies_.empty()) {
        emergencies_.pop();
    }
    while (!eoeFrames_.empty()) {
        eoeFrames_.pop();
    }
    remainingExchangeFailures_ = 0;
    redundancyHealthy_ = true;
    discoveredSlaves_.clear();
    error_.clear();
    return true;
}

void MockTransport::close() { opened_ = false; }

bool MockTransport::exchange(const std::vector<std::uint8_t>& txProcessData,
                             std::vector<std::uint8_t>& rxProcessData) {
    if (!opened_) {
        error_ = "not opened";
        return false;
    }
    if (txProcessData.size() != lastOutputs_.size()) {
        error_ = "unexpected TX process image size";
        return false;
    }
    if (remainingExchangeFailures_ > 0U) {
        --remainingExchangeFailures_;
        error_ = "injected exchange failure";
        return false;
    }

    lastOutputs_ = txProcessData;
    rxProcessData = inputs_;
    lastWorkingCounter_ = 1U;
    return true;
}

std::string MockTransport::lastError() const { return error_; }
std::uint16_t MockTransport::lastWorkingCounter() const { return lastWorkingCounter_; }

bool MockTransport::requestNetworkState(SlaveState state) {
    if (!opened_) {
        error_ = "not opened";
        return false;
    }
    state_ = state;
    return true;
}

bool MockTransport::readNetworkState(SlaveState& outState) {
    if (!opened_) {
        error_ = "not opened";
        return false;
    }
    outState = state_;
    return true;
}

bool MockTransport::requestSlaveState(std::uint16_t position, SlaveState state) {
    if (!opened_) {
        error_ = "not opened";
        return false;
    }
    perSlaveState_[position] = state;
    if (state == SlaveState::Op) {
        perSlaveAlStatusCode_[position] = 0U;
    }
    return true;
}

bool MockTransport::readSlaveState(std::uint16_t position, SlaveState& outState) {
    if (!opened_) {
        error_ = "not opened";
        return false;
    }
    const auto it = perSlaveState_.find(position);
    if (it == perSlaveState_.end()) {
        outState = state_;
    } else {
        outState = it->second;
    }
    return true;
}

bool MockTransport::readSlaveAlStatusCode(std::uint16_t position, std::uint16_t& outCode) {
    if (!opened_) {
        error_ = "not opened";
        return false;
    }
    const auto it = perSlaveAlStatusCode_.find(position);
    outCode = (it == perSlaveAlStatusCode_.end()) ? 0U : it->second;
    return true;
}

bool MockTransport::reconfigureSlave(std::uint16_t position) {
    if (!opened_) {
        error_ = "not opened";
        return false;
    }
    perSlaveState_[position] = SlaveState::SafeOp;
    perSlaveAlStatusCode_[position] = 0U;
    return true;
}

bool MockTransport::failoverSlave(std::uint16_t position) {
    if (!opened_) {
        error_ = "not opened";
        return false;
    }
    perSlaveState_[position] = SlaveState::SafeOp;
    perSlaveAlStatusCode_[position] = 0x0014U;
    return true;
}

void MockTransport::setInputBit(std::size_t byteOffset, std::uint8_t bitOffset, bool value) {
    setBit(inputs_, byteOffset, bitOffset, value);
}

void MockTransport::setInputByte(std::size_t byteOffset, std::uint8_t value) {
    if (byteOffset >= inputs_.size()) {
        throw std::out_of_range("byte access out of range");
    }
    inputs_[byteOffset] = value;
}

void MockTransport::setInputBytes(std::size_t byteOffset, const std::vector<std::uint8_t>& data) {
    if (byteOffset > inputs_.size()) {
        throw std::out_of_range("byte offset out of range");
    }
    if (data.size() > (inputs_.size() - byteOffset)) {
        throw std::out_of_range("byte range out of range");
    }
    std::copy(data.begin(), data.end(), inputs_.begin() + static_cast<std::ptrdiff_t>(byteOffset));
}

bool MockTransport::getLastOutputBit(std::size_t byteOffset, std::uint8_t bitOffset) const {
    return getBit(lastOutputs_, byteOffset, bitOffset);
}

std::vector<std::uint8_t> MockTransport::lastOutputs() const { return lastOutputs_; }

void MockTransport::setSlaveAlStatusCode(std::uint16_t position, std::uint16_t alStatusCode) {
    perSlaveAlStatusCode_[position] = alStatusCode;
}

void MockTransport::injectExchangeFailures(std::size_t count) { remainingExchangeFailures_ = count; }

bool MockTransport::sdoUpload(std::uint16_t slavePosition, const SdoAddress& address,
                              std::vector<std::uint8_t>& outData, std::uint32_t& outAbortCode,
                              std::string& outError) {
    if (!opened_) {
        outError = "not opened";
        return false;
    }
    outAbortCode = 0U;
    const auto key = (static_cast<std::uint64_t>(slavePosition) << 32U) |
                     (static_cast<std::uint64_t>(address.index) << 8U) |
                     static_cast<std::uint64_t>(address.subIndex);
    const auto it = sdoObjects_.find(key);
    if (it == sdoObjects_.end()) {
        outAbortCode = 0x06020000U;
        return false;
    }
    outData = it->second;
    return true;
}

bool MockTransport::sdoDownload(std::uint16_t slavePosition, const SdoAddress& address,
                                const std::vector<std::uint8_t>& data, std::uint32_t& outAbortCode,
                                std::string& outError) {
    if (!opened_) {
        outError = "not opened";
        return false;
    }
    outAbortCode = 0U;
    const auto key = (static_cast<std::uint64_t>(slavePosition) << 32U) |
                     (static_cast<std::uint64_t>(address.index) << 8U) |
                     static_cast<std::uint64_t>(address.subIndex);
    sdoObjects_[key] = data;
    return true;
}

bool MockTransport::configurePdo(std::uint16_t slavePosition, std::uint16_t assignIndex,
                                 const std::vector<PdoMappingEntry>& entries, std::string& outError) {
    if (!opened_) {
        outError = "not opened";
        return false;
    }
    (void)assignIndex;
    pdoAssignments_[slavePosition] = entries;
    return true;
}

bool MockTransport::pollEmergency(EmergencyMessage& outEmergency) {
    if (!opened_) {
        return false;
    }
    if (emergencies_.empty()) {
        return false;
    }
    outEmergency = emergencies_.front();
    emergencies_.pop();
    return true;
}

bool MockTransport::discoverTopology(TopologySnapshot& outSnapshot, std::string& outError) {
    if (!opened_) {
        outError = "not opened";
        return false;
    }
    outSnapshot.slaves = discoveredSlaves_;
    outSnapshot.redundancyHealthy = redundancyHealthy_;
    return true;
}

bool MockTransport::isRedundancyLinkHealthy(std::string& outError) {
    if (!opened_) {
        outError = "not opened";
        return false;
    }
    return redundancyHealthy_;
}

bool MockTransport::foeRead(std::uint16_t slavePosition, const FoERequest& request,
                            FoEResponse& outResponse, std::string& outError) {
    if (!opened_) {
        outError = "not opened";
        return false;
    }
    const auto key = (static_cast<std::uint64_t>(slavePosition) << 32U) ^
                     static_cast<std::uint64_t>(std::hash<std::string>{}(request.fileName));
    const auto it = foeFiles_.find(key);
    if (it == foeFiles_.end()) {
        outResponse.success = false;
        outResponse.error = "FoE file not found";
        return false;
    }
    outResponse.success = true;
    outResponse.data = it->second;
    outResponse.error.clear();
    return true;
}

bool MockTransport::foeWrite(std::uint16_t slavePosition, const FoERequest& request,
                             const std::vector<std::uint8_t>& data, std::string& outError) {
    if (!opened_) {
        outError = "not opened";
        return false;
    }
    const auto key = (static_cast<std::uint64_t>(slavePosition) << 32U) ^
                     static_cast<std::uint64_t>(std::hash<std::string>{}(request.fileName));
    foeFiles_[key] = data;
    return true;
}

bool MockTransport::eoeSend(std::uint16_t slavePosition, const std::vector<std::uint8_t>& frame,
                            std::string& outError) {
    if (!opened_) {
        outError = "not opened";
        return false;
    }
    eoeFrames_.push({slavePosition, frame});
    return true;
}

bool MockTransport::eoeReceive(std::uint16_t slavePosition, std::vector<std::uint8_t>& frame,
                               std::string& outError) {
    if (!opened_) {
        outError = "not opened";
        return false;
    }
    if (eoeFrames_.empty()) {
        return false;
    }
    const auto next = eoeFrames_.front();
    if (next.first != slavePosition) {
        return false;
    }
    frame = next.second;
    eoeFrames_.pop();
    return true;
}

void MockTransport::enqueueEmergency(const EmergencyMessage& emergency) {
    emergencies_.push(emergency);
}

void MockTransport::setRedundancyHealthy(bool healthy) { redundancyHealthy_ = healthy; }

void MockTransport::setDiscoveredSlaves(const std::vector<TopologySlaveInfo>& slaves) {
    discoveredSlaves_ = slaves;
}

void MockTransport::setBit(std::vector<std::uint8_t>& bytes, std::size_t byteOffset,
                           std::uint8_t bitOffset, bool value) {
    if (bitOffset >= 8U || byteOffset >= bytes.size()) {
        throw std::out_of_range("bit access out of range");
    }
    const auto mask = static_cast<std::uint8_t>(1U << bitOffset);
    if (value) {
        bytes[byteOffset] |= mask;
    } else {
        bytes[byteOffset] &= static_cast<std::uint8_t>(~mask);
    }
}

bool MockTransport::getBit(const std::vector<std::uint8_t>& bytes, std::size_t byteOffset,
                           std::uint8_t bitOffset) {
    if (bitOffset >= 8U || byteOffset >= bytes.size()) {
        throw std::out_of_range("bit access out of range");
    }
    return ((bytes[byteOffset] >> bitOffset) & 0x1U) != 0U;
}

} // namespace oec
