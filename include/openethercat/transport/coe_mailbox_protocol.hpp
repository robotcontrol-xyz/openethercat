/**
 * @file coe_mailbox_protocol.hpp
 * @brief openEtherCAT source file.
 */

#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "openethercat/master/coe_mailbox.hpp"

namespace oec {

/**
 * @brief EtherCAT ESC mailbox frame container.
 */
struct EscMailboxFrame {
    std::uint8_t channel = 0;
    std::uint8_t priority = 0;
    std::uint8_t type = 0;
    std::uint8_t counter = 0;
    std::vector<std::uint8_t> payload;
};

/**
 * @brief Parsed SDO initiate-upload response metadata.
 */
struct CoeSdoInitiateUploadResponse {
    bool success = false;
    bool expedited = false;
    bool sizeIndicated = false;
    std::uint32_t completeSize = 0;
    std::vector<std::uint8_t> data;
    std::uint32_t abortCode = 0;
    std::string error;
};

/**
 * @brief Parsed SDO upload-segment response.
 */
struct CoeSdoSegmentUploadResponse {
    bool success = false;
    bool lastSegment = false;
    std::uint8_t toggle = 0;
    std::vector<std::uint8_t> data;
    std::uint32_t abortCode = 0;
    std::string error;
};

/**
 * @brief Parsed SDO download acknowledgement frame.
 */
struct CoeSdoAckResponse {
    bool success = false;
    std::uint8_t toggle = 0;
    std::uint32_t abortCode = 0;
    std::string error;
};

/**
 * @brief CoE mailbox wire codec and segmented SDO helper.
 */
class CoeMailboxProtocol {
public:
    static constexpr std::uint8_t kMailboxTypeCoe = 0x03;

    static std::vector<std::uint8_t> encodeEscMailbox(const EscMailboxFrame& frame);
    static std::optional<EscMailboxFrame> decodeEscMailbox(const std::vector<std::uint8_t>& bytes);

    static std::vector<std::uint8_t> buildSdoInitiateUploadRequest(SdoAddress address);
    static CoeSdoInitiateUploadResponse parseSdoInitiateUploadResponse(const std::vector<std::uint8_t>& payload,
                                                                       SdoAddress expectedAddress);

    static std::vector<std::uint8_t> buildSdoUploadSegmentRequest(std::uint8_t toggle);
    static CoeSdoSegmentUploadResponse parseSdoUploadSegmentResponse(const std::vector<std::uint8_t>& payload);

    static std::vector<std::uint8_t> buildSdoInitiateDownloadRequest(SdoAddress address,
                                                                      std::uint32_t totalSize);
    static std::vector<std::uint8_t> buildSdoDownloadSegmentRequest(std::uint8_t toggle,
                                                                     bool lastSegment,
                                                                     const std::vector<std::uint8_t>& segmentData,
                                                                     std::size_t maxSegmentBytes);
    static CoeSdoAckResponse parseSdoDownloadResponse(const std::vector<std::uint8_t>& payload);

private:
    static std::uint16_t readLe16(const std::vector<std::uint8_t>& in, std::size_t offset);
    static std::uint32_t readLe32(const std::vector<std::uint8_t>& in, std::size_t offset);
    static void putLe16(std::vector<std::uint8_t>& out, std::uint16_t value);
    static void putLe32(std::vector<std::uint8_t>& out, std::uint32_t value);
};

} // namespace oec
