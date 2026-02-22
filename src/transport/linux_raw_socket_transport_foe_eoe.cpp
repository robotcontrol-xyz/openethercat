/**
 * @file linux_raw_socket_transport_foe_eoe.cpp
 * @brief Linux-specific FoE/EoE implementations for LinuxRawSocketTransport.
 */

#include "openethercat/transport/linux_raw_socket_transport.hpp"
#include "openethercat/master/foe_eoe.hpp"

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <string>
#include <thread>
#include <vector>

namespace oec {
namespace {

constexpr std::uint8_t kMailboxTypeEoe = 0x02U;
constexpr std::uint8_t kMailboxTypeFoe = 0x04U;
constexpr std::uint16_t kFoeOpReadReq = 0x0001U;
constexpr std::uint16_t kFoeOpWriteReq = 0x0002U;
constexpr std::uint16_t kFoeOpData = 0x0003U;
constexpr std::uint16_t kFoeOpAck = 0x0004U;
constexpr std::uint16_t kFoeOpErr = 0x0005U;
constexpr std::uint16_t kFoeOpBusy = 0x0006U;

std::uint16_t readLe16Raw(const std::vector<std::uint8_t>& in, std::size_t offset) {
    return static_cast<std::uint16_t>(static_cast<std::uint16_t>(in[offset]) |
                                      (static_cast<std::uint16_t>(in[offset + 1]) << 8U));
}

std::uint32_t readLe32Raw(const std::vector<std::uint8_t>& in, std::size_t offset) {
    return static_cast<std::uint32_t>(static_cast<std::uint32_t>(in[offset]) |
                                      (static_cast<std::uint32_t>(in[offset + 1]) << 8U) |
                                      (static_cast<std::uint32_t>(in[offset + 2]) << 16U) |
                                      (static_cast<std::uint32_t>(in[offset + 3]) << 24U));
}

void appendLe16Raw(std::vector<std::uint8_t>& out, std::uint16_t value) {
    out.push_back(static_cast<std::uint8_t>(value & 0xFFU));
    out.push_back(static_cast<std::uint8_t>((value >> 8U) & 0xFFU));
}

void appendLe32Raw(std::vector<std::uint8_t>& out, std::uint32_t value) {
    out.push_back(static_cast<std::uint8_t>(value & 0xFFU));
    out.push_back(static_cast<std::uint8_t>((value >> 8U) & 0xFFU));
    out.push_back(static_cast<std::uint8_t>((value >> 16U) & 0xFFU));
    out.push_back(static_cast<std::uint8_t>((value >> 24U) & 0xFFU));
}

} // namespace

bool LinuxRawSocketTransport::foeRead(std::uint16_t slavePosition, const FoERequest& request,
                                      FoEResponse& outResponse, std::string& outError) {
    outResponse = FoEResponse{};
    outError.clear();
    ++mailboxDiagnostics_.foeReadStarted;
    auto fail = [&](std::string message) -> bool {
        outError = std::move(message);
        outResponse.success = false;
        outResponse.error = outError;
        ++mailboxDiagnostics_.foeReadFailed;
        return false;
    };
    if (socketFd_ < 0) {
        return fail("transport not open");
    }

    const auto adp = static_cast<std::uint16_t>(0U - slavePosition);
    std::uint16_t writeOffset = 0U;
    std::uint16_t writeSize = 0U;
    std::uint16_t readOffset = 0U;
    std::uint16_t readSize = 0U;
    resolveMailboxWindow(adp, writeOffset, writeSize, readOffset, readSize, outError);

    std::vector<std::uint8_t> rrq;
    rrq.reserve(8U + request.fileName.size() + 1U);
    appendLe16Raw(rrq, kFoeOpReadReq);
    appendLe32Raw(rrq, request.password);
    rrq.insert(rrq.end(), request.fileName.begin(), request.fileName.end());
    rrq.push_back('\0');

    std::uint8_t expectedCounter = 0U;
    if (!mailboxWriteFrame(adp, writeOffset, writeSize, kMailboxTypeFoe, rrq, expectedCounter, outError)) {
        if (outError == "Mailbox payload exceeds write window") {
            outError = "FoE request exceeds mailbox write window";
        }
        return fail(outError);
    }

    const std::size_t maxDataPerPacket =
        (readSize > 12U) ? (readSize - 12U) : std::max<std::size_t>(16U, request.maxChunkBytes);
    std::uint32_t expectedPacket = 1U;
    while (true) {
        EscMailboxFrame frame;
        if (!mailboxReadFrameExpected(adp, slavePosition, readOffset, readSize, expectedCounter, kMailboxTypeFoe,
                                      true, frame, true, "Timed out waiting for FoE mailbox response", outError)) {
            return fail(outError);
        }
        if (frame.payload.size() < 2U) {
            return fail("FoE response payload too short");
        }
        const auto op = readLe16Raw(frame.payload, 0U);
        if (op == kFoeOpErr) {
            const auto errCode = (frame.payload.size() >= 6U) ? readLe32Raw(frame.payload, 2U) : 0U;
            return fail("FoE error response code=0x" + std::to_string(errCode));
        }
        if (op == kFoeOpBusy) {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
            continue;
        }
        if (op != kFoeOpData || frame.payload.size() < 6U) {
            return fail("Unexpected FoE response opcode");
        }

        const auto packetNo = readLe32Raw(frame.payload, 2U);
        if (packetNo != expectedPacket) {
            return fail("FoE packet sequence mismatch");
        }
        const std::vector<std::uint8_t> chunk(frame.payload.begin() + 6, frame.payload.end());
        outResponse.data.insert(outResponse.data.end(), chunk.begin(), chunk.end());

        std::vector<std::uint8_t> ack;
        ack.reserve(6U);
        appendLe16Raw(ack, kFoeOpAck);
        appendLe32Raw(ack, packetNo);
        if (!mailboxWriteFrame(adp, writeOffset, writeSize, kMailboxTypeFoe, ack, expectedCounter, outError)) {
            if (outError == "Mailbox payload exceeds write window") {
                outError = "FoE request exceeds mailbox write window";
            }
            return fail(outError);
        }
        ++expectedPacket;

        if (chunk.size() < maxDataPerPacket) {
            outResponse.success = true;
            outResponse.error.clear();
            return true;
        }
    }
}

bool LinuxRawSocketTransport::foeWrite(std::uint16_t slavePosition, const FoERequest& request,
                                       const std::vector<std::uint8_t>& data, std::string& outError) {
    outError.clear();
    ++mailboxDiagnostics_.foeWriteStarted;
    auto fail = [&](std::string message) -> bool {
        outError = std::move(message);
        ++mailboxDiagnostics_.foeWriteFailed;
        return false;
    };
    if (socketFd_ < 0) {
        return fail("transport not open");
    }

    const auto adp = static_cast<std::uint16_t>(0U - slavePosition);
    std::uint16_t writeOffset = 0U;
    std::uint16_t writeSize = 0U;
    std::uint16_t readOffset = 0U;
    std::uint16_t readSize = 0U;
    resolveMailboxWindow(adp, writeOffset, writeSize, readOffset, readSize, outError);

    std::vector<std::uint8_t> wrq;
    wrq.reserve(8U + request.fileName.size() + 1U);
    appendLe16Raw(wrq, kFoeOpWriteReq);
    appendLe32Raw(wrq, request.password);
    wrq.insert(wrq.end(), request.fileName.begin(), request.fileName.end());
    wrq.push_back('\0');

    std::uint8_t expectedCounter = 0U;
    if (!mailboxWriteFrame(adp, writeOffset, writeSize, kMailboxTypeFoe, wrq, expectedCounter, outError)) {
        if (outError == "Mailbox payload exceeds write window") {
            outError = "FoE request exceeds mailbox write window";
        }
        return fail(outError);
    }

    EscMailboxFrame frame;
    if (!mailboxReadFrameExpected(adp, slavePosition, readOffset, readSize, expectedCounter, kMailboxTypeFoe,
                                  true, frame, false, "Timed out waiting for FoE mailbox response", outError)) {
        return fail(outError);
    }
    if (frame.payload.size() < 2U) {
        return fail("FoE response payload too short");
    }
    auto op = readLe16Raw(frame.payload, 0U);
    if (op == kFoeOpErr) {
        return fail("FoE write request rejected");
    }
    if (op != kFoeOpAck) {
        return fail("Expected FoE ACK after WRQ");
    }

    const std::size_t maxDataBytes = (writeSize > 12U)
        ? std::min<std::size_t>(request.maxChunkBytes, writeSize - 12U)
        : std::min<std::size_t>(request.maxChunkBytes, 256U);
    std::size_t cursor = 0U;
    std::uint32_t packetNo = 1U;
    while (cursor < data.size() || (data.empty() && packetNo == 1U)) {
        const auto remaining = (cursor < data.size()) ? (data.size() - cursor) : 0U;
        const auto chunkBytes = std::min<std::size_t>(maxDataBytes, remaining);
        std::vector<std::uint8_t> payload;
        payload.reserve(6U + chunkBytes);
        appendLe16Raw(payload, kFoeOpData);
        appendLe32Raw(payload, packetNo);
        if (chunkBytes > 0U) {
            payload.insert(payload.end(), data.begin() + static_cast<std::ptrdiff_t>(cursor),
                           data.begin() + static_cast<std::ptrdiff_t>(cursor + chunkBytes));
        }

        if (!mailboxWriteFrame(adp, writeOffset, writeSize, kMailboxTypeFoe, payload, expectedCounter, outError)) {
            if (outError == "Mailbox payload exceeds write window") {
                outError = "FoE request exceeds mailbox write window";
            }
            return fail(outError);
        }

        frame = EscMailboxFrame{};
        if (!mailboxReadFrameExpected(adp, slavePosition, readOffset, readSize, expectedCounter, kMailboxTypeFoe,
                                      true, frame, false, "Timed out waiting for FoE mailbox response", outError)) {
            return fail(outError);
        }
        if (frame.payload.size() < 2U) {
            return fail("FoE ACK payload too short");
        }
        op = readLe16Raw(frame.payload, 0U);
        if (op == kFoeOpErr) {
            return fail("FoE data packet rejected");
        }
        if (op != kFoeOpAck || frame.payload.size() < 6U) {
            return fail("Expected FoE ACK for data packet");
        }
        const auto ackPacket = readLe32Raw(frame.payload, 2U);
        if (ackPacket != packetNo) {
            return fail("FoE ACK packet mismatch");
        }

        cursor += chunkBytes;
        ++packetNo;
        if (chunkBytes < maxDataBytes) {
            break;
        }
    }
    return true;
}

bool LinuxRawSocketTransport::eoeSend(std::uint16_t slavePosition, const std::vector<std::uint8_t>& frame,
                                      std::string& outError) {
    outError.clear();
    ++mailboxDiagnostics_.eoeSendStarted;
    auto fail = [&](std::string message) -> bool {
        outError = std::move(message);
        ++mailboxDiagnostics_.eoeSendFailed;
        return false;
    };
    if (socketFd_ < 0) {
        return fail("transport not open");
    }
    const auto adp = static_cast<std::uint16_t>(0U - slavePosition);
    std::uint16_t writeOffset = 0U;
    std::uint16_t writeSize = 0U;
    std::uint16_t readOffset = 0U;
    std::uint16_t readSize = 0U;
    resolveMailboxWindow(adp, writeOffset, writeSize, readOffset, readSize, outError);
    (void)readOffset;
    (void)readSize;
    std::uint8_t counter = 0U;
    if (!mailboxWriteFrame(adp, writeOffset, writeSize, kMailboxTypeEoe, frame, counter, outError)) {
        if (outError == "Mailbox payload exceeds write window") {
            outError = "EoE frame exceeds mailbox write window";
        }
        return fail(outError);
    }
    (void)counter;
    return true;
}

bool LinuxRawSocketTransport::eoeReceive(std::uint16_t slavePosition, std::vector<std::uint8_t>& frame,
                                         std::string& outError) {
    frame.clear();
    outError.clear();
    ++mailboxDiagnostics_.eoeReceiveStarted;
    auto fail = [&](std::string message) -> bool {
        outError = std::move(message);
        ++mailboxDiagnostics_.eoeReceiveFailed;
        return false;
    };
    if (socketFd_ < 0) {
        return fail("transport not open");
    }
    const auto adp = static_cast<std::uint16_t>(0U - slavePosition);
    std::uint16_t writeOffset = 0U;
    std::uint16_t writeSize = 0U;
    std::uint16_t readOffset = 0U;
    std::uint16_t readSize = 0U;
    resolveMailboxWindow(adp, writeOffset, writeSize, readOffset, readSize, outError);
    (void)writeOffset;
    (void)writeSize;
    EscMailboxFrame mailboxFrame;
    if (!mailboxReadFrameExpected(adp, slavePosition, readOffset, readSize, 0U, kMailboxTypeEoe, false, mailboxFrame,
                                  false, "Timed out waiting for EoE mailbox frame", outError)) {
        return fail(outError);
    }
    frame = mailboxFrame.payload;
    return true;
}

} // namespace oec
