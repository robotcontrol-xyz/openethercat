/**
 * @file coe_mailbox_protocol.cpp
 * @brief openEtherCAT source file.
 */

#include "openethercat/transport/coe_mailbox_protocol.hpp"

#include <algorithm>

namespace oec {
namespace {

constexpr std::uint16_t kCoeServiceSdoReq = 0x0002;
constexpr std::uint16_t kCoeServiceSdoRes = 0x0003;
constexpr std::uint16_t kCoeServiceEmergency = 0x0001;
constexpr std::uint8_t kSdoCmdUploadInitiateReq = 0x40;
constexpr std::uint8_t kSdoCmdUploadInitiateRes = 0x40;
constexpr std::uint8_t kSdoCmdUploadSegmentReqBase = 0x60;
constexpr std::uint8_t kSdoCmdUploadSegmentResBase = 0x00;
constexpr std::uint8_t kSdoCmdDownloadInitiateReq = 0x21; // size indicated, segmented
constexpr std::uint8_t kSdoCmdDownloadInitiateRes = 0x60;
constexpr std::uint8_t kSdoCmdDownloadSegmentReqBase = 0x00;
constexpr std::uint8_t kSdoCmdDownloadSegmentResBase = 0x20;
constexpr std::uint8_t kSdoCmdAbort = 0x80;

} // namespace

std::vector<std::uint8_t> CoeMailboxProtocol::encodeEscMailbox(const EscMailboxFrame& frame) {
    std::vector<std::uint8_t> out;
    out.reserve(6U + frame.payload.size());

    putLe16(out, static_cast<std::uint16_t>(frame.payload.size()));
    putLe16(out, 0U); // address field is commonly 0 for master/slave mailbox exchange
    out.push_back(static_cast<std::uint8_t>(((frame.channel & 0x0FU) << 4U) | (frame.priority & 0x03U)));
    out.push_back(static_cast<std::uint8_t>(((frame.type & 0x0FU) << 4U) | (frame.counter & 0x07U)));
    out.insert(out.end(), frame.payload.begin(), frame.payload.end());
    return out;
}

std::optional<EscMailboxFrame> CoeMailboxProtocol::decodeEscMailbox(const std::vector<std::uint8_t>& bytes) {
    if (bytes.size() < 6U) {
        return std::nullopt;
    }

    const auto payloadBytes = static_cast<std::size_t>(readLe16(bytes, 0));
    if (payloadBytes == 0U || payloadBytes + 6U > bytes.size()) {
        return std::nullopt;
    }

    EscMailboxFrame frame;
    frame.channel = static_cast<std::uint8_t>((bytes[4] >> 4U) & 0x0FU);
    frame.priority = static_cast<std::uint8_t>(bytes[4] & 0x03U);
    frame.type = static_cast<std::uint8_t>((bytes[5] >> 4U) & 0x0FU);
    frame.counter = static_cast<std::uint8_t>(bytes[5] & 0x07U);
    frame.payload.assign(bytes.begin() + 6, bytes.begin() + static_cast<std::ptrdiff_t>(6U + payloadBytes));
    return frame;
}

bool CoeMailboxProtocol::parseEmergency(const std::vector<std::uint8_t>& payload,
                                        std::uint16_t slavePosition,
                                        EmergencyMessage& outEmergency) {
    if (payload.size() < 10U) {
        return false;
    }
    const auto service = readLe16(payload, 0);
    if (service != kCoeServiceEmergency) {
        return false;
    }
    outEmergency.errorCode = readLe16(payload, 2);
    outEmergency.errorRegister = payload[4];
    for (std::size_t i = 0; i < outEmergency.manufacturerData.size(); ++i) {
        outEmergency.manufacturerData[i] = payload[5U + i];
    }
    outEmergency.slavePosition = slavePosition;
    return true;
}

std::vector<std::uint8_t> CoeMailboxProtocol::buildSdoInitiateUploadRequest(SdoAddress address) {
    std::vector<std::uint8_t> out;
    out.reserve(10U);
    putLe16(out, kCoeServiceSdoReq);
    out.push_back(kSdoCmdUploadInitiateReq);
    putLe16(out, address.index);
    out.push_back(address.subIndex);
    putLe32(out, 0U);
    return out;
}

CoeSdoInitiateUploadResponse CoeMailboxProtocol::parseSdoInitiateUploadResponse(
    const std::vector<std::uint8_t>& payload,
    SdoAddress expectedAddress) {
    CoeSdoInitiateUploadResponse response;
    if (payload.size() < 10U) {
        response.error = "SDO upload initiate response too short";
        return response;
    }

    const auto service = readLe16(payload, 0);
    if (service != kCoeServiceSdoRes) {
        response.error = "Unexpected CoE service in upload initiate response";
        return response;
    }

    const auto cmd = payload[2];
    const auto index = readLe16(payload, 3);
    const auto subIndex = payload[5];
    if (index != expectedAddress.index || subIndex != expectedAddress.subIndex) {
        response.error = "SDO response address mismatch";
        return response;
    }

    if (cmd == kSdoCmdAbort) {
        response.abortCode = readLe32(payload, 6);
        response.error = "SDO abort";
        return response;
    }

    if ((cmd & 0xE0U) != kSdoCmdUploadInitiateRes) {
        response.error = "Unexpected SDO command for upload initiate response";
        return response;
    }

    response.expedited = (cmd & 0x02U) != 0U;
    response.sizeIndicated = (cmd & 0x01U) != 0U;

    if (response.expedited) {
        const auto unusedBytes = static_cast<std::size_t>((cmd >> 2U) & 0x03U);
        const auto used = 4U - std::min<std::size_t>(unusedBytes, 3U);
        response.data.assign(payload.begin() + 6, payload.begin() + static_cast<std::ptrdiff_t>(6U + used));
        response.completeSize = static_cast<std::uint32_t>(response.data.size());
    } else if (response.sizeIndicated) {
        response.completeSize = readLe32(payload, 6);
    }

    response.success = true;
    return response;
}

std::vector<std::uint8_t> CoeMailboxProtocol::buildSdoUploadSegmentRequest(std::uint8_t toggle) {
    std::vector<std::uint8_t> out;
    out.reserve(3U);
    putLe16(out, kCoeServiceSdoReq);
    out.push_back(static_cast<std::uint8_t>(kSdoCmdUploadSegmentReqBase | ((toggle & 0x01U) << 4U)));
    return out;
}

CoeSdoSegmentUploadResponse CoeMailboxProtocol::parseSdoUploadSegmentResponse(
    const std::vector<std::uint8_t>& payload) {
    CoeSdoSegmentUploadResponse response;
    if (payload.size() < 3U) {
        response.error = "SDO upload segment response too short";
        return response;
    }

    const auto service = readLe16(payload, 0);
    if (service != kCoeServiceSdoRes) {
        response.error = "Unexpected CoE service in upload segment response";
        return response;
    }

    const auto cmd = payload[2];
    if (cmd == kSdoCmdAbort) {
        if (payload.size() >= 7U) {
            response.abortCode = readLe32(payload, 3);
        }
        response.error = "SDO abort";
        return response;
    }

    if ((cmd & 0xE0U) != kSdoCmdUploadSegmentResBase) {
        response.error = "Unexpected SDO command for upload segment response";
        return response;
    }

    response.toggle = static_cast<std::uint8_t>((cmd >> 4U) & 0x01U);
    response.lastSegment = (cmd & 0x01U) != 0U;
    const auto unusedBytes = static_cast<std::size_t>((cmd >> 1U) & 0x07U);
    const auto dataBytes = payload.size() - 3U;
    if (unusedBytes > dataBytes) {
        response.error = "Invalid unused-byte count in segment response";
        return response;
    }
    response.data.assign(payload.begin() + 3, payload.end() - static_cast<std::ptrdiff_t>(unusedBytes));

    response.success = true;
    return response;
}

std::vector<std::uint8_t> CoeMailboxProtocol::buildSdoInitiateDownloadRequest(SdoAddress address,
                                                                               std::uint32_t totalSize) {
    std::vector<std::uint8_t> out;
    out.reserve(10U);
    putLe16(out, kCoeServiceSdoReq);
    out.push_back(kSdoCmdDownloadInitiateReq);
    putLe16(out, address.index);
    out.push_back(address.subIndex);
    putLe32(out, totalSize);
    return out;
}

CoeSdoAckResponse CoeMailboxProtocol::parseSdoInitiateDownloadResponse(
    const std::vector<std::uint8_t>& payload,
    SdoAddress expectedAddress) {
    CoeSdoAckResponse response;
    if (payload.size() < 6U) {
        response.error = "SDO initiate download response too short";
        return response;
    }

    const auto service = readLe16(payload, 0);
    if (service != kCoeServiceSdoRes) {
        response.error = "Unexpected CoE service in SDO initiate download response";
        return response;
    }

    const auto cmd = payload[2];
    if (cmd == kSdoCmdAbort) {
        if (payload.size() >= 7U) {
            response.abortCode = readLe32(payload, 3);
        }
        response.error = "SDO abort";
        return response;
    }

    if (cmd != kSdoCmdDownloadInitiateRes) {
        response.error = "Unexpected SDO command for initiate download response";
        return response;
    }

    const auto index = readLe16(payload, 3);
    const auto subIndex = payload[5];
    if (index != expectedAddress.index || subIndex != expectedAddress.subIndex) {
        response.error = "SDO response address mismatch";
        return response;
    }

    response.success = true;
    response.toggle = 0U;
    return response;
}

std::vector<std::uint8_t> CoeMailboxProtocol::buildSdoDownloadSegmentRequest(std::uint8_t toggle,
                                                                              bool lastSegment,
                                                                              const std::vector<std::uint8_t>& segmentData,
                                                                              std::size_t maxSegmentBytes) {
    const auto clampedBytes = std::min(segmentData.size(), maxSegmentBytes);
    const auto unused = static_cast<std::uint8_t>(maxSegmentBytes - clampedBytes);

    std::vector<std::uint8_t> out;
    out.reserve(3U + clampedBytes);
    putLe16(out, kCoeServiceSdoReq);
    out.push_back(static_cast<std::uint8_t>(kSdoCmdDownloadSegmentReqBase |
                                            ((toggle & 0x01U) << 4U) |
                                            ((unused & 0x07U) << 1U) |
                                            (lastSegment ? 0x01U : 0U)));
    out.insert(out.end(), segmentData.begin(), segmentData.begin() + static_cast<std::ptrdiff_t>(clampedBytes));
    return out;
}

CoeSdoAckResponse CoeMailboxProtocol::parseSdoDownloadSegmentResponse(
    const std::vector<std::uint8_t>& payload,
    std::uint8_t expectedToggle) {
    CoeSdoAckResponse response;
    if (payload.size() < 3U) {
        response.error = "SDO download segment response too short";
        return response;
    }

    const auto service = readLe16(payload, 0);
    if (service != kCoeServiceSdoRes) {
        response.error = "Unexpected CoE service in SDO download segment response";
        return response;
    }

    const auto cmd = payload[2];
    if (cmd == kSdoCmdAbort) {
        if (payload.size() >= 7U) {
            response.abortCode = readLe32(payload, 3);
        }
        response.error = "SDO abort";
        return response;
    }

    if ((cmd & 0xE0U) != kSdoCmdDownloadSegmentResBase) {
        response.error = "Unexpected SDO command for download segment response";
        return response;
    }

    response.toggle = static_cast<std::uint8_t>((cmd >> 4U) & 0x01U);
    if (response.toggle != (expectedToggle & 0x01U)) {
        response.error = "SDO download segment toggle mismatch";
        return response;
    }
    response.success = true;
    return response;
}

std::uint16_t CoeMailboxProtocol::readLe16(const std::vector<std::uint8_t>& in, std::size_t offset) {
    return static_cast<std::uint16_t>(static_cast<std::uint16_t>(in[offset]) |
                                      (static_cast<std::uint16_t>(in[offset + 1]) << 8U));
}

std::uint32_t CoeMailboxProtocol::readLe32(const std::vector<std::uint8_t>& in, std::size_t offset) {
    return static_cast<std::uint32_t>(static_cast<std::uint32_t>(in[offset]) |
                                      (static_cast<std::uint32_t>(in[offset + 1]) << 8U) |
                                      (static_cast<std::uint32_t>(in[offset + 2]) << 16U) |
                                      (static_cast<std::uint32_t>(in[offset + 3]) << 24U));
}

void CoeMailboxProtocol::putLe16(std::vector<std::uint8_t>& out, std::uint16_t value) {
    out.push_back(static_cast<std::uint8_t>(value & 0xFFU));
    out.push_back(static_cast<std::uint8_t>((value >> 8U) & 0xFFU));
}

void CoeMailboxProtocol::putLe32(std::vector<std::uint8_t>& out, std::uint32_t value) {
    out.push_back(static_cast<std::uint8_t>(value & 0xFFU));
    out.push_back(static_cast<std::uint8_t>((value >> 8U) & 0xFFU));
    out.push_back(static_cast<std::uint8_t>((value >> 16U) & 0xFFU));
    out.push_back(static_cast<std::uint8_t>((value >> 24U) & 0xFFU));
}

} // namespace oec
