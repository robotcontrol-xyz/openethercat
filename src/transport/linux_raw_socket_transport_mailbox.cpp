/**
 * @file linux_raw_socket_transport_mailbox.cpp
 * @brief Linux-specific mailbox helper implementations for LinuxRawSocketTransport.
 */

#include "openethercat/transport/linux_raw_socket_transport.hpp"

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <string>
#include <thread>
#include <vector>

namespace oec {
namespace {

constexpr std::uint8_t kCommandAprd = 0x01;
constexpr std::uint8_t kCommandApwr = 0x02;
constexpr std::uint16_t kRegisterSmBase = 0x0800;
constexpr std::uint16_t kRegisterSmStatusOffset = 0x0005;

} // namespace

LinuxRawSocketTransport::MailboxRetryConfig LinuxRawSocketTransport::mailboxRetryConfigFromEnv() const {
    MailboxRetryConfig cfg;
    if (const char* env = std::getenv("OEC_MAILBOX_RETRIES")) {
        try {
            cfg.retries = std::max(0, std::stoi(env));
        } catch (...) {
            // Keep defaults if parsing fails.
        }
    }
    if (const char* env = std::getenv("OEC_MAILBOX_BACKOFF_BASE_MS")) {
        try {
            cfg.backoffBaseMs = std::max(1, std::stoi(env));
        } catch (...) {
        }
    }
    if (const char* env = std::getenv("OEC_MAILBOX_BACKOFF_MAX_MS")) {
        try {
            cfg.backoffMaxMs = std::max(cfg.backoffBaseMs, std::stoi(env));
        } catch (...) {
        }
    }
    return cfg;
}

bool LinuxRawSocketTransport::readSmWindowWithRetry(std::uint16_t adp,
                                                    std::uint8_t smIndex,
                                                    bool forceTimeoutTest,
                                                    int mailboxRetries,
                                                    int mailboxBackoffBaseMs,
                                                    int mailboxBackoffMaxMs,
                                                    MailboxErrorClass& outErrorClass,
                                                    std::uint16_t& outStart,
                                                    std::uint16_t& outLen,
                                                    std::string& outError) {
    const auto currentIndex = datagramIndex_++;
    EthercatDatagramRequest req;
    req.command = kCommandAprd;
    req.datagramIndex = currentIndex;
    req.adp = adp;
    req.ado = static_cast<std::uint16_t>(kRegisterSmBase + (smIndex * 8U));
    req.payload.assign(8U, 0U);

    std::uint16_t wkc = 0;
    std::vector<std::uint8_t> payload;
    if (!mailboxDatagramWithRetry(req, wkc, payload, forceTimeoutTest, mailboxRetries, mailboxBackoffBaseMs,
                                  mailboxBackoffMaxMs, outErrorClass, outError)) {
        return false;
    }
    if (payload.size() < 4U) {
        outError = "SM payload too short for mailbox";
        outErrorClass = MailboxErrorClass::ParseReject;
        return false;
    }
    outStart = static_cast<std::uint16_t>(payload[0]) |
               (static_cast<std::uint16_t>(payload[1]) << 8U);
    outLen = static_cast<std::uint16_t>(payload[2]) |
             (static_cast<std::uint16_t>(payload[3]) << 8U);
    return true;
}

bool LinuxRawSocketTransport::readSmStatusWithRetry(std::uint16_t adp,
                                                    std::uint8_t smIndex,
                                                    bool forceTimeoutTest,
                                                    int mailboxRetries,
                                                    int mailboxBackoffBaseMs,
                                                    int mailboxBackoffMaxMs,
                                                    MailboxErrorClass& outErrorClass,
                                                    std::uint8_t& outStatus,
                                                    std::string& outError) {
    const auto currentIndex = datagramIndex_++;
    EthercatDatagramRequest req;
    req.command = kCommandAprd;
    req.datagramIndex = currentIndex;
    req.adp = adp;
    req.ado = static_cast<std::uint16_t>(kRegisterSmBase + (smIndex * 8U) + kRegisterSmStatusOffset);
    req.payload.assign(1U, 0U);

    std::uint16_t wkc = 0;
    std::vector<std::uint8_t> payload;
    if (!mailboxDatagramWithRetry(req, wkc, payload, forceTimeoutTest, mailboxRetries, mailboxBackoffBaseMs,
                                  mailboxBackoffMaxMs, outErrorClass, outError)) {
        return false;
    }
    if (payload.empty()) {
        outError = "SM status payload too short";
        outErrorClass = MailboxErrorClass::ParseReject;
        return false;
    }
    outStatus = payload[0];
    return true;
}

bool LinuxRawSocketTransport::mailboxWriteCoePayload(std::uint16_t adp,
                                                     std::uint16_t writeOffset,
                                                     std::uint16_t writeSize,
                                                     MailboxStatusMode statusMode,
                                                     bool forceTimeoutTest,
                                                     int mailboxRetries,
                                                     int mailboxBackoffBaseMs,
                                                     int mailboxBackoffMaxMs,
                                                     const std::vector<std::uint8_t>& coePayload,
                                                     std::uint8_t& outCounter,
                                                     MailboxErrorClass& outErrorClass,
                                                     std::string& outError) {
    if (statusMode != MailboxStatusMode::Poll) {
        bool writeReady = false;
        for (int probe = 0; probe < 3; ++probe) {
            std::uint8_t status = 0U;
            MailboxErrorClass localClass = MailboxErrorClass::None;
            if (!readSmStatusWithRetry(adp, 0U, forceTimeoutTest, mailboxRetries, mailboxBackoffBaseMs,
                                       mailboxBackoffMaxMs, localClass, status, outError)) {
                if (statusMode == MailboxStatusMode::Strict) {
                    outError = "SM0 status read failed in strict mode";
                    outErrorClass = MailboxErrorClass::Busy;
                    return false;
                }
                break;
            }
            if ((status & 0x08U) == 0U) {
                writeReady = true;
                break;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        if (statusMode == MailboxStatusMode::Strict && !writeReady) {
            outError = "SM0 mailbox remained busy in strict mode";
            outErrorClass = MailboxErrorClass::Busy;
            return false;
        }
    }

    EscMailboxFrame frame;
    frame.channel = 0;
    frame.priority = 0;
    frame.type = CoeMailboxProtocol::kMailboxTypeCoe;
    frame.counter = static_cast<std::uint8_t>(mailboxCounter_++ & 0x07U);
    outCounter = frame.counter;
    frame.payload = coePayload;
    auto bytes = CoeMailboxProtocol::encodeEscMailbox(frame);
    if (bytes.size() > writeSize) {
        outError = "CoE mailbox payload too large for configured write mailbox";
        outErrorClass = MailboxErrorClass::ParseReject;
        return false;
    }
    bytes.resize(writeSize, 0U);

    const auto currentIndex = datagramIndex_++;
    EthercatDatagramRequest req;
    req.command = kCommandApwr;
    req.datagramIndex = currentIndex;
    req.adp = adp;
    req.ado = writeOffset;
    req.payload = bytes;

    std::uint16_t wkc = 0;
    std::vector<std::uint8_t> payload;
    if (!mailboxDatagramWithRetry(req, wkc, payload, forceTimeoutTest, mailboxRetries, mailboxBackoffBaseMs,
                                  mailboxBackoffMaxMs, outErrorClass, outError)) {
        return false;
    }
    ++mailboxDiagnostics_.mailboxWrites;
    lastWorkingCounter_ = wkc;
    return true;
}

bool LinuxRawSocketTransport::mailboxReadMatchingCoe(std::uint16_t adp,
                                                     std::uint16_t slavePosition,
                                                     std::uint16_t readOffset,
                                                     std::uint16_t readSize,
                                                     MailboxStatusMode statusMode,
                                                     bool forceTimeoutTest,
                                                     int mailboxRetries,
                                                     int mailboxBackoffBaseMs,
                                                     int mailboxBackoffMaxMs,
                                                     std::uint8_t expectedCounter,
                                                     const std::function<bool(const EscMailboxFrame&)>& accept,
                                                     EscMailboxFrame& outFrame,
                                                     MailboxErrorClass& outErrorClass,
                                                     std::string& outError) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeoutMs_);
    int idlePolls = 0;
    while (std::chrono::steady_clock::now() < deadline) {
        if (statusMode != MailboxStatusMode::Poll) {
            // Status-driven read gate with strict/hybrid policies.
            std::uint8_t status = 0U;
            MailboxErrorClass statusClass = MailboxErrorClass::None;
            const bool haveStatus = readSmStatusWithRetry(adp, 1U, forceTimeoutTest, mailboxRetries,
                                                          mailboxBackoffBaseMs, mailboxBackoffMaxMs,
                                                          statusClass, status, outError);
            const bool mailboxHasData = haveStatus && ((status & 0x08U) != 0U);
            if (statusMode == MailboxStatusMode::Strict) {
                if (!haveStatus) {
                    outError = "SM1 status read failed in strict mode";
                    outErrorClass = MailboxErrorClass::Busy;
                    return false;
                }
                if (!mailboxHasData) {
                    std::this_thread::sleep_for(std::chrono::milliseconds(1));
                    continue;
                }
            } else if (!mailboxHasData && ((idlePolls++ % 3) != 0)) {
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
                continue;
            }
        }

        const auto currentIndex = datagramIndex_++;
        EthercatDatagramRequest req;
        req.command = kCommandAprd;
        req.datagramIndex = currentIndex;
        req.adp = adp;
        req.ado = readOffset;
        req.payload.assign(readSize, 0U);

        std::uint16_t wkc = 0;
        std::vector<std::uint8_t> payload;
        MailboxErrorClass ioClass = MailboxErrorClass::None;
        if (!mailboxDatagramWithRetry(req, wkc, payload, forceTimeoutTest, mailboxRetries,
                                      mailboxBackoffBaseMs, mailboxBackoffMaxMs, ioClass, outError)) {
            outErrorClass = ioClass;
            return false;
        }
        ++mailboxDiagnostics_.mailboxReads;
        lastWorkingCounter_ = wkc;

        auto decoded = CoeMailboxProtocol::decodeEscMailbox(payload);
        if (!decoded) {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
            continue;
        }
        if (decoded->type != CoeMailboxProtocol::kMailboxTypeCoe) {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
            continue;
        }
        EmergencyMessage emergency {};
        if (CoeMailboxProtocol::parseEmergency(decoded->payload, slavePosition, emergency)) {
            if (emergencies_.size() >= emergencyQueueLimit_) {
                emergencies_.pop();
                ++mailboxDiagnostics_.emergencyDropped;
            }
            emergencies_.push(emergency);
            ++mailboxDiagnostics_.emergencyQueued;
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
            continue;
        }
        if ((decoded->counter & 0x07U) != (expectedCounter & 0x07U)) {
            ++mailboxDiagnostics_.staleCounterDrops;
            outErrorClass = MailboxErrorClass::StaleCounter;
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
            continue;
        }
        if (!accept(*decoded)) {
            ++mailboxDiagnostics_.parseRejects;
            outErrorClass = MailboxErrorClass::ParseReject;
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
            continue;
        }
        ++mailboxDiagnostics_.matchedResponses;
        outFrame = *decoded;
        return true;
    }
    outError = "Timed out waiting for CoE mailbox response";
    ++mailboxDiagnostics_.mailboxTimeouts;
    if (outErrorClass == MailboxErrorClass::None) {
        outErrorClass = MailboxErrorClass::Timeout;
    }
    return false;
}

} // namespace oec
