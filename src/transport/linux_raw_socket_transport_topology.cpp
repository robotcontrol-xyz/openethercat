/**
 * @file linux_raw_socket_transport_topology.cpp
 * @brief Linux-specific topology discovery/redundancy for LinuxRawSocketTransport.
 */

#include "openethercat/transport/linux_raw_socket_transport.hpp"
#include "openethercat/master/topology_manager.hpp"

#include <chrono>
#include <cstdint>
#include <string>
#include <thread>
#include <vector>

namespace oec {
namespace {

constexpr std::uint8_t kCommandAprd = 0x01;
constexpr std::uint8_t kCommandApwr = 0x02;
constexpr std::uint16_t kRegisterAlStatus = 0x0130;
constexpr std::uint16_t kRegisterEscType = 0x0008;
constexpr std::uint16_t kRegisterEscRevision = 0x000A;
constexpr std::uint16_t kRegisterEepControlStatus = 0x0502;
constexpr std::uint16_t kRegisterEepAddress = 0x0504;
constexpr std::uint16_t kRegisterEepData = 0x0508;
constexpr std::uint16_t kEepCommandRead = 0x0100;
constexpr std::uint16_t kEepBusy = 0x8000;
constexpr std::uint16_t kEepErrorMask = 0x7800;
constexpr std::uint16_t kSiiWordVendorId = 0x0008;
constexpr std::uint16_t kSiiWordProductCode = 0x000A;
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

} // namespace

bool LinuxRawSocketTransport::discoverTopology(TopologySnapshot& outSnapshot, std::string& outError) {
    outSnapshot = TopologySnapshot{};
    outError.clear();
    if (socketFd_ < 0) {
        outError = "transport not open";
        return false;
    }

    auto autoIncAddress = [](std::uint16_t position) -> std::uint16_t {
        // APRD/APWR use signed auto-increment addressing: 0, -1, -2, ...
        return static_cast<std::uint16_t>(0U - position);
    };
    auto readAt = [&](std::uint16_t adp, std::uint16_t ado, std::size_t size,
                      std::vector<std::uint8_t>& out) -> bool {
        const auto currentIndex = datagramIndex_++;
        EthercatDatagramRequest request;
        request.command = kCommandAprd;
        request.datagramIndex = currentIndex;
        request.adp = adp;
        request.ado = ado;
        request.payload.assign(size, 0U);

        std::uint16_t wkc = 0;
        std::vector<std::uint8_t> payload;
        if (!sendDatagramRequest(request, wkc, payload, error_)) {
            return false;
        }
        if (payload.size() < size) {
            return false;
        }
        out = std::move(payload);
        return true;
    };
    auto writeAt = [&](std::uint16_t adp, std::uint16_t ado, const std::vector<std::uint8_t>& value) -> bool {
        const auto currentIndex = datagramIndex_++;
        EthercatDatagramRequest request;
        request.command = kCommandApwr;
        request.datagramIndex = currentIndex;
        request.adp = adp;
        request.ado = ado;
        request.payload = value;

        std::uint16_t wkc = 0;
        std::vector<std::uint8_t> payload;
        return sendDatagramRequest(request, wkc, payload, error_);
    };
    auto readSiiWord32 = [&](std::uint16_t adp, std::uint16_t wordAddress, std::uint32_t& outValue) -> bool {
        const std::vector<std::uint8_t> addrPayload = {
            static_cast<std::uint8_t>(wordAddress & 0xFFU),
            static_cast<std::uint8_t>((wordAddress >> 8U) & 0xFFU),
            0x00U,
            0x00U,
        };
        if (!writeAt(adp, kRegisterEepAddress, addrPayload)) {
            return false;
        }
        const std::vector<std::uint8_t> cmdPayload = {
            static_cast<std::uint8_t>(kEepCommandRead & 0xFFU),
            static_cast<std::uint8_t>((kEepCommandRead >> 8U) & 0xFFU),
        };
        if (!writeAt(adp, kRegisterEepControlStatus, cmdPayload)) {
            return false;
        }

        const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(100);
        while (std::chrono::steady_clock::now() < deadline) {
            std::vector<std::uint8_t> statusPayload;
            if (!readAt(adp, kRegisterEepControlStatus, 2, statusPayload)) {
                return false;
            }
            const std::uint16_t status = static_cast<std::uint16_t>(statusPayload[0]) |
                                         (static_cast<std::uint16_t>(statusPayload[1]) << 8U);
            if ((status & kEepBusy) != 0U) {
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
                continue;
            }
            if ((status & kEepErrorMask) != 0U) {
                return false;
            }

            std::vector<std::uint8_t> dataPayload;
            if (!readAt(adp, kRegisterEepData, 4, dataPayload)) {
                return false;
            }
            outValue = static_cast<std::uint32_t>(dataPayload[0]) |
                       (static_cast<std::uint32_t>(dataPayload[1]) << 8U) |
                       (static_cast<std::uint32_t>(dataPayload[2]) << 16U) |
                       (static_cast<std::uint32_t>(dataPayload[3]) << 24U);
            return true;
        }
        return false;
    };

    for (std::uint16_t position = 0; position < 256; ++position) {
        const std::uint16_t adp = autoIncAddress(position);
        TopologySlaveInfo info;
        info.position = position;
        info.online = false;

        std::vector<std::uint8_t> alPayload;
        if (!readAt(adp, kRegisterAlStatus, 2, alPayload)) {
            if (position == 0) {
                continue;
            }
            // EtherCAT chains are contiguous in auto-increment addressing.
            break;
        }
        info.online = true;
        const auto alRaw = static_cast<std::uint16_t>(alPayload[0]) |
                           (static_cast<std::uint16_t>(alPayload[1]) << 8U);
        SlaveState decodedState = SlaveState::Init;
        info.alStateValid = decodeAlState(alRaw, decodedState);
        if (info.alStateValid) {
            info.alState = decodedState;
        }

        std::vector<std::uint8_t> escTypePayload;
        if (readAt(adp, kRegisterEscType, 2, escTypePayload) && escTypePayload.size() >= 2) {
            info.escType = static_cast<std::uint16_t>(escTypePayload[0]) |
                           (static_cast<std::uint16_t>(escTypePayload[1]) << 8U);
        }
        std::vector<std::uint8_t> escRevisionPayload;
        if (readAt(adp, kRegisterEscRevision, 2, escRevisionPayload) && escRevisionPayload.size() >= 2) {
            info.escRevision = static_cast<std::uint16_t>(escRevisionPayload[0]) |
                               (static_cast<std::uint16_t>(escRevisionPayload[1]) << 8U);
        }

        // Prefer standardized CoE identity object (0x1018) for real vendor/product identity.
        std::uint32_t abort = 0U;
        std::string sdoError;
        std::vector<std::uint8_t> objectData;
        SdoAddress vendorAddr;
        vendorAddr.index = 0x1018U;
        vendorAddr.subIndex = 0x01U;
        SdoAddress productAddr;
        productAddr.index = 0x1018U;
        productAddr.subIndex = 0x02U;
        const bool hasVendor = sdoUpload(position, vendorAddr, objectData, abort, sdoError) && objectData.size() >= 4;
        if (hasVendor) {
            info.vendorId = static_cast<std::uint32_t>(objectData[0]) |
                            (static_cast<std::uint32_t>(objectData[1]) << 8U) |
                            (static_cast<std::uint32_t>(objectData[2]) << 16U) |
                            (static_cast<std::uint32_t>(objectData[3]) << 24U);
        }
        objectData.clear();
        abort = 0U;
        sdoError.clear();
        const bool hasProduct = sdoUpload(position, productAddr, objectData, abort, sdoError) && objectData.size() >= 4;
        if (hasProduct) {
            info.productCode = static_cast<std::uint32_t>(objectData[0]) |
                               (static_cast<std::uint32_t>(objectData[1]) << 8U) |
                               (static_cast<std::uint32_t>(objectData[2]) << 16U) |
                               (static_cast<std::uint32_t>(objectData[3]) << 24U);
        }
        info.identityFromCoe = hasVendor && hasProduct;
        if (!info.identityFromCoe) {
            std::uint32_t siiVendor = 0U;
            std::uint32_t siiProduct = 0U;
            const bool siiVendorOk = readSiiWord32(adp, kSiiWordVendorId, siiVendor);
            const bool siiProductOk = readSiiWord32(adp, kSiiWordProductCode, siiProduct);
            if (siiVendorOk && siiProductOk) {
                info.vendorId = siiVendor;
                info.productCode = siiProduct;
                info.identityFromSii = true;
            }
        }

        outSnapshot.slaves.push_back(info);
    }
    outSnapshot.redundancyHealthy = (secondarySocketFd_ >= 0) || !redundancyEnabled_;
    return true;
}

bool LinuxRawSocketTransport::isRedundancyLinkHealthy(std::string& outError) {
    outError.clear();
    if (!redundancyEnabled_) {
        return true;
    }
    if (secondaryIfname_.empty()) {
        outError = "redundancy enabled but secondary interface not configured";
        return false;
    }
    return secondarySocketFd_ >= 0;
}

} // namespace oec
