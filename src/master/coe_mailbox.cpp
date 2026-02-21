/**
 * @file coe_mailbox.cpp
 * @brief openEtherCAT source file.
 */

#include "openethercat/master/coe_mailbox.hpp"

#include <sstream>

namespace oec {

CoeMailboxService::CoeMailboxService(ITransport& transport) : transport_(transport) {}

SdoResponse CoeMailboxService::upload(std::uint16_t slavePosition, SdoAddress address) const {
    SdoResponse response;
    std::vector<std::uint8_t> data;
    std::uint32_t abortCode = 0;
    std::string error;
    // Transport returns either protocol-level abort code or a transport error string.
    const bool ok = transport_.sdoUpload(slavePosition, address, data, abortCode, error);
    response.success = ok;
    response.data = std::move(data);
    if (!ok) {
        if (abortCode != 0U) {
            response.abort = SdoAbort{abortCode, describeAbort(abortCode)};
        } else {
            response.abort = SdoAbort{0U, error.empty() ? "SDO upload failed" : error};
        }
    }
    return response;
}

SdoResponse CoeMailboxService::download(std::uint16_t slavePosition,
                                        SdoAddress address,
                                        const std::vector<std::uint8_t>& data) const {
    SdoResponse response;
    std::uint32_t abortCode = 0;
    std::string error;
    // Keep abort decoding in one place so callers always get consistent failure semantics.
    const bool ok = transport_.sdoDownload(slavePosition, address, data, abortCode, error);
    response.success = ok;
    if (!ok) {
        if (abortCode != 0U) {
            response.abort = SdoAbort{abortCode, describeAbort(abortCode)};
        } else {
            response.abort = SdoAbort{0U, error.empty() ? "SDO download failed" : error};
        }
    }
    return response;
}

bool CoeMailboxService::configureRxPdo(std::uint16_t slavePosition,
                                       const std::vector<PdoMappingEntry>& entries,
                                       std::string& outError) const {
    // 0x1600 is the first standard RxPDO mapping object.
    return transport_.configurePdo(slavePosition, 0x1600U, entries, outError);
}

bool CoeMailboxService::configureTxPdo(std::uint16_t slavePosition,
                                       const std::vector<PdoMappingEntry>& entries,
                                       std::string& outError) const {
    // 0x1A00 is the first standard TxPDO mapping object.
    return transport_.configurePdo(slavePosition, 0x1A00U, entries, outError);
}

std::vector<EmergencyMessage> CoeMailboxService::drainEmergencyQueue(std::size_t maxMessages) const {
    std::vector<EmergencyMessage> messages;
    messages.reserve(maxMessages);
    for (std::size_t i = 0; i < maxMessages; ++i) {
        EmergencyMessage emergency;
        // Poll until queue is empty or caller-imposed limit is reached.
        if (!transport_.pollEmergency(emergency)) {
            break;
        }
        messages.push_back(emergency);
    }
    return messages;
}

std::string CoeMailboxService::describeAbort(std::uint32_t code) {
    switch (code) {
    case 0x05030000U:
        return "Toggle bit not alternated";
    case 0x05040001U:
        return "SDO protocol timed out";
    case 0x06010000U:
        return "Unsupported access to object";
    case 0x06010001U:
        return "Attempt to read a write-only object";
    case 0x06010002U:
        return "Attempt to write a read-only object";
    case 0x06020000U:
        return "Object does not exist";
    case 0x06090011U:
        return "Sub-index does not exist";
    case 0x06090030U:
        return "Value range exceeded";
    case 0x08000000U:
        return "General error";
    default:
        break;
    }

    std::ostringstream os;
    os << "SDO abort 0x" << std::hex << code;
    return os.str();
}

} // namespace oec
