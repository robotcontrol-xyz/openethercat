/**
 * @file linux_raw_socket_transport_process_image.cpp
 * @brief Linux-specific process-image mapping/FMMU programming.
 */

#include "openethercat/transport/linux_raw_socket_transport.hpp"
#include "openethercat/config/eni_esi_models.hpp"

#include <algorithm>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <map>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace oec {
namespace {

constexpr std::uint8_t kCommandAprd = 0x01;
constexpr std::uint8_t kCommandApwr = 0x02;
constexpr std::uint16_t kRegisterSmBase = 0x0800;
constexpr std::uint16_t kRegisterFmmuBase = 0x0600;

std::uint16_t toAutoIncrementAddress(std::uint16_t position) {
    // EtherCAT auto-increment addresses are signed: 0, -1, -2, ...
    return static_cast<std::uint16_t>(0U - position);
}

} // namespace

bool LinuxRawSocketTransport::configureProcessImage(const NetworkConfiguration& config, std::string& outError) {
    outError.clear();
    if (socketFd_ < 0) {
        outError = "transport not open";
        return false;
    }
    const bool traceMap = (std::getenv("OEC_TRACE_MAP") != nullptr);
    outputWindows_.clear();

    const auto readSm = [&](std::uint16_t position, std::uint8_t smIndex,
                            std::uint16_t& outStart, std::uint16_t& outLen) -> bool {
        const auto currentIndex = datagramIndex_++;
        EthercatDatagramRequest req;
        req.command = kCommandAprd;
        req.datagramIndex = currentIndex;
        req.adp = toAutoIncrementAddress(position);
        req.ado = static_cast<std::uint16_t>(kRegisterSmBase + (smIndex * 8U));
        req.payload.assign(8U, 0U);

        std::uint16_t wkc = 0;
        std::vector<std::uint8_t> payload;
        if (!sendDatagramRequest(req, wkc, payload, outError)) {
            return false;
        }
        if (payload.size() < 4U) {
            outError = "SM read payload too short";
            return false;
        }
        outStart = static_cast<std::uint16_t>(payload[0]) |
                   (static_cast<std::uint16_t>(payload[1]) << 8U);
        outLen = static_cast<std::uint16_t>(payload[2]) |
                 (static_cast<std::uint16_t>(payload[3]) << 8U);
        return true;
    };

    const auto writeFmmu = [&](std::uint16_t position, std::uint8_t fmmuIndex,
                               std::uint32_t logicalStart, std::uint16_t length,
                               std::uint16_t physicalStart, bool writeDirection) -> bool {
        std::vector<std::uint8_t> payload(16U, 0U);
        payload[0] = static_cast<std::uint8_t>(logicalStart & 0xFFU);
        payload[1] = static_cast<std::uint8_t>((logicalStart >> 8U) & 0xFFU);
        payload[2] = static_cast<std::uint8_t>((logicalStart >> 16U) & 0xFFU);
        payload[3] = static_cast<std::uint8_t>((logicalStart >> 24U) & 0xFFU);
        payload[4] = static_cast<std::uint8_t>(length & 0xFFU);
        payload[5] = static_cast<std::uint8_t>((length >> 8U) & 0xFFU);
        payload[6] = 0U;   // logical start bit
        payload[7] = 7U;   // logical end bit
        payload[8] = static_cast<std::uint8_t>(physicalStart & 0xFFU);
        payload[9] = static_cast<std::uint8_t>((physicalStart >> 8U) & 0xFFU);
        payload[10] = 0U;  // physical start bit
        payload[11] = writeDirection ? 0x02U : 0x01U; // write or read enable
        payload[12] = 0x01U; // enable

        const auto currentIndex = datagramIndex_++;
        EthercatDatagramRequest req;
        req.command = kCommandApwr;
        req.datagramIndex = currentIndex;
        req.adp = toAutoIncrementAddress(position);
        req.ado = static_cast<std::uint16_t>(kRegisterFmmuBase + (fmmuIndex * 16U));
        req.payload = std::move(payload);

        std::uint16_t wkc = 0;
        std::vector<std::uint8_t> ack;
        return sendDatagramRequest(req, wkc, ack, outError);
    };
    const auto writeSm = [&](std::uint16_t position, std::uint8_t smIndex,
                             std::uint16_t start, std::uint16_t len,
                             std::uint8_t control, std::uint8_t activate) -> bool {
        std::vector<std::uint8_t> payload(8U, 0U);
        payload[0] = static_cast<std::uint8_t>(start & 0xFFU);
        payload[1] = static_cast<std::uint8_t>((start >> 8U) & 0xFFU);
        payload[2] = static_cast<std::uint8_t>(len & 0xFFU);
        payload[3] = static_cast<std::uint8_t>((len >> 8U) & 0xFFU);
        payload[4] = control;
        payload[6] = activate;

        const auto currentIndex = datagramIndex_++;
        EthercatDatagramRequest req;
        req.command = kCommandApwr;
        req.datagramIndex = currentIndex;
        req.adp = toAutoIncrementAddress(position);
        req.ado = static_cast<std::uint16_t>(kRegisterSmBase + (smIndex * 8U));
        req.payload = std::move(payload);

        std::uint16_t wkc = 0;
        std::vector<std::uint8_t> ack;
        return sendDatagramRequest(req, wkc, ack, outError);
    };

    std::unordered_map<std::string, std::uint16_t> slaveByName;
    slaveByName.reserve(config.slaves.size());
    for (const auto& s : config.slaves) {
        slaveByName[s.name] = s.position;
    }
    std::unordered_map<std::uint16_t, std::vector<SignalBinding>> outputSignalsBySlave;
    std::unordered_map<std::uint16_t, std::vector<SignalBinding>> inputSignalsBySlave;

    std::unordered_set<std::uint16_t> outputSlaves;
    std::unordered_set<std::uint16_t> inputSlaves;
    for (const auto& signal : config.signals) {
        const auto it = slaveByName.find(signal.slaveName);
        if (it == slaveByName.end()) {
            continue;
        }
        if (signal.direction == SignalDirection::Output) {
            outputSlaves.insert(it->second);
            outputSignalsBySlave[it->second].push_back(signal);
        } else {
            inputSlaves.insert(it->second);
            inputSignalsBySlave[it->second].push_back(signal);
        }
    }

    auto buildDefaultEntries = [](const std::vector<SignalBinding>& signals,
                                  bool outputDirection) -> std::vector<PdoMappingEntry> {
        // For simple EL1xxx/EL2xxx terminals, channel bits are typically mapped at
        // 0x6000:1..N (inputs) and 0x7000:1..N (outputs).
        std::map<std::uint8_t, PdoMappingEntry> ordered;
        for (const auto& sig : signals) {
            PdoMappingEntry e;
            e.index = outputDirection ? 0x7000U : 0x6000U;
            e.subIndex = static_cast<std::uint8_t>(sig.bitOffset + 1U);
            e.bitLength = 1U;
            ordered[e.subIndex] = e;
        }
        std::vector<PdoMappingEntry> out;
        out.reserve(ordered.size());
        for (const auto& kv : ordered) {
            out.push_back(kv.second);
        }
        return out;
    };
    auto estimatedByteLength = [](const std::vector<SignalBinding>& signals) -> std::uint16_t {
        std::size_t maxByte = 0U;
        bool any = false;
        for (const auto& sig : signals) {
            any = true;
            maxByte = std::max(maxByte, sig.byteOffset);
        }
        return static_cast<std::uint16_t>(any ? (maxByte + 1U) : 0U);
    };

    std::uint32_t outputLogical = logicalAddress_;
    std::uint32_t inputLogical = logicalAddress_ + static_cast<std::uint32_t>(config.processImageOutputBytes);
    std::uint8_t fmmuIndex = 0U;
    std::size_t mappedOutputSlaves = 0U;
    std::size_t mappedInputSlaves = 0U;

    for (const auto position : outputSlaves) {
        std::uint16_t smStart = 0U;
        std::uint16_t smLen = 0U;
        if (!readSm(position, 2U, smStart, smLen)) {
            return false;
        }
        if (traceMap) {
            std::cerr << "[oec-map] slave=" << position
                      << " SM2(start=0x" << std::hex << smStart
                      << ", len=" << std::dec << smLen << ")\n";
        }
        if (smLen == 0U) {
            const auto sigIt = outputSignalsBySlave.find(position);
            if (sigIt != outputSignalsBySlave.end() && !sigIt->second.empty()) {
                std::string pdoError;
                const auto entries = buildDefaultEntries(sigIt->second, true);
                if (configurePdo(position, 0x1600U, entries, pdoError)) {
                    if (!readSm(position, 2U, smStart, smLen)) {
                        return false;
                    }
                    if (traceMap) {
                        std::cerr << "[oec-map] slave=" << position
                                  << " SM2 re-read after default RxPDO config (start=0x" << std::hex << smStart
                                  << ", len=" << std::dec << smLen << ")\n";
                    }
                } else if (traceMap) {
                    std::cerr << "[oec-map] slave=" << position
                              << " default RxPDO config failed: " << pdoError << '\n';
                }
                if (smLen == 0U) {
                    // Mailbox-less fallback (SOEM-style simple IO): write minimal SM2 defaults.
                    const auto estLen = std::max<std::uint16_t>(1U, estimatedByteLength(sigIt->second));
                    if (writeSm(position, 2U, 0x1100U, estLen, 0x24U, 0x01U)) {
                        if (!readSm(position, 2U, smStart, smLen)) {
                            return false;
                        }
                        if (traceMap) {
                            std::cerr << "[oec-map] slave=" << position
                                      << " SM2 re-read after direct SM fallback (start=0x" << std::hex << smStart
                                      << ", len=" << std::dec << smLen << ")\n";
                        }
                    } else if (traceMap) {
                        std::cerr << "[oec-map] slave=" << position
                                  << " direct SM2 fallback failed: " << outError << '\n';
                    }
                }
            }
        }
        if (smLen == 0U) {
            continue;
        }
        if (!writeFmmu(position, fmmuIndex++, outputLogical, smLen, smStart, true)) {
            return false;
        }
        if (traceMap) {
            std::cerr << "[oec-map] slave=" << position
                      << " FMMU(write, logical=0x" << std::hex << outputLogical
                      << ", len=" << std::dec << smLen
                      << ", physical=0x" << std::hex << smStart << ")\n";
        }
        outputWindows_.push_back(ProcessDataWindow{
            position, smStart, smLen, outputLogical
        });
        outputLogical += smLen;
        ++mappedOutputSlaves;
    }

    for (const auto position : inputSlaves) {
        std::uint16_t smStart = 0U;
        std::uint16_t smLen = 0U;
        if (!readSm(position, 3U, smStart, smLen)) {
            return false;
        }
        if (traceMap) {
            std::cerr << "[oec-map] slave=" << position
                      << " SM3(start=0x" << std::hex << smStart
                      << ", len=" << std::dec << smLen << ")\n";
        }
        if (smLen == 0U) {
            const auto sigIt = inputSignalsBySlave.find(position);
            if (sigIt != inputSignalsBySlave.end() && !sigIt->second.empty()) {
                std::string pdoError;
                const auto entries = buildDefaultEntries(sigIt->second, false);
                if (configurePdo(position, 0x1A00U, entries, pdoError)) {
                    if (!readSm(position, 3U, smStart, smLen)) {
                        return false;
                    }
                    if (traceMap) {
                        std::cerr << "[oec-map] slave=" << position
                                  << " SM3 re-read after default TxPDO config (start=0x" << std::hex << smStart
                                  << ", len=" << std::dec << smLen << ")\n";
                    }
                } else if (traceMap) {
                    std::cerr << "[oec-map] slave=" << position
                              << " default TxPDO config failed: " << pdoError << '\n';
                }
                if (smLen == 0U) {
                    // Mailbox-less fallback (SOEM-style simple IO): write minimal SM3 defaults.
                    const auto estLen = std::max<std::uint16_t>(1U, estimatedByteLength(sigIt->second));
                    if (writeSm(position, 3U, 0x1100U, estLen, 0x20U, 0x01U)) {
                        if (!readSm(position, 3U, smStart, smLen)) {
                            return false;
                        }
                        if (traceMap) {
                            std::cerr << "[oec-map] slave=" << position
                                      << " SM3 re-read after direct SM fallback (start=0x" << std::hex << smStart
                                      << ", len=" << std::dec << smLen << ")\n";
                        }
                    } else if (traceMap) {
                        std::cerr << "[oec-map] slave=" << position
                                  << " direct SM3 fallback failed: " << outError << '\n';
                    }
                }
            }
        }
        if (smLen == 0U) {
            continue;
        }
        if (!writeFmmu(position, fmmuIndex++, inputLogical, smLen, smStart, false)) {
            return false;
        }
        if (traceMap) {
            std::cerr << "[oec-map] slave=" << position
                      << " FMMU(read, logical=0x" << std::hex << inputLogical
                      << ", len=" << std::dec << smLen
                      << ", physical=0x" << std::hex << smStart << ")\n";
        }
        inputLogical += smLen;
        ++mappedInputSlaves;
    }

    if (!outputSlaves.empty() && mappedOutputSlaves == 0U) {
        outError = "No output slaves produced valid SM2 mapping (all SM2 lengths were zero)";
        return false;
    }
    if (!inputSlaves.empty() && mappedInputSlaves == 0U) {
        outError = "No input slaves produced valid SM3 mapping (all SM3 lengths were zero)";
        return false;
    }
    if (traceMap) {
        std::cerr << "[oec-map] mapped outputs=" << mappedOutputSlaves
                  << " mapped inputs=" << mappedInputSlaves << '\n';
    }
    return true;
}

} // namespace oec
