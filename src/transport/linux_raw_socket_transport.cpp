/**
 * @file linux_raw_socket_transport.cpp
 * @brief openEtherCAT source file.
 */

#include "openethercat/transport/linux_raw_socket_transport.hpp"

#include <algorithm>
#include <cerrno>
#include <cstring>
#include <cstdlib>
#include <iostream>
#include <unordered_map>
#include <unordered_set>
#include <string>
#include <vector>
#include <chrono>
#include <thread>
#include <map>

#include <linux/if_packet.h>
#include <net/if.h>
#include <netinet/in.h>
#include <sys/ioctl.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <unistd.h>

#include "openethercat/transport/ethercat_frame.hpp"
#include "openethercat/master/coe_mailbox.hpp"
#include "openethercat/master/foe_eoe.hpp"
#include "openethercat/master/topology_manager.hpp"
#include "openethercat/config/eni_esi_models.hpp"
#include "openethercat/transport/coe_mailbox_protocol.hpp"

namespace oec {
namespace {

constexpr std::uint16_t kEtherTypeEthercat = 0x88A4;
constexpr std::uint8_t kCommandLrw = 0x0C;
constexpr std::uint8_t kCommandLrd = 0x0A;
constexpr std::uint8_t kCommandLwr = 0x0B;
constexpr std::uint8_t kCommandBrd = 0x07;
constexpr std::uint8_t kCommandBwr = 0x08;
constexpr std::uint8_t kCommandAprd = 0x01;
constexpr std::uint8_t kCommandApwr = 0x02;
constexpr std::uint16_t kRegisterAlControl = 0x0120;
constexpr std::uint16_t kRegisterAlStatus = 0x0130;
constexpr std::uint16_t kRegisterAlStatusCode = 0x0134;
constexpr std::uint16_t kRegisterDcSystemTime = 0x0910;
constexpr std::uint16_t kRegisterDcSystemTimeOffset = 0x0920;
constexpr std::uint16_t kRegisterEscType = 0x0008;
constexpr std::uint16_t kRegisterEscRevision = 0x000A;
constexpr std::uint16_t kRegisterSmBase = 0x0800;
constexpr std::uint16_t kRegisterSmStatusOffset = 0x0005;
constexpr std::uint16_t kRegisterFmmuBase = 0x0600;
constexpr std::uint16_t kRegisterEepControlStatus = 0x0502;
constexpr std::uint16_t kRegisterEepAddress = 0x0504;
constexpr std::uint16_t kRegisterEepData = 0x0508;
constexpr std::uint16_t kEepCommandRead = 0x0100;
constexpr std::uint16_t kEepBusy = 0x8000;
constexpr std::uint16_t kEepErrorMask = 0x7800;
constexpr std::uint16_t kSiiWordVendorId = 0x0008;
constexpr std::uint16_t kSiiWordProductCode = 0x000A;
constexpr std::uint16_t kAlStateMask = 0x000F;

bool openEthercatInterfaceSocket(const std::string& ifname,
                                 int& outSocketFd,
                                 int& outIfIndex,
                                 std::array<std::uint8_t, 6>& outSourceMac,
                                 std::string& outError) {
    outSocketFd = ::socket(AF_PACKET, SOCK_RAW, htons(kEtherTypeEthercat));
    if (outSocketFd < 0) {
        outError = "socket() failed: " + std::string(std::strerror(errno));
        return false;
    }

    ifreq ifr {};
    std::strncpy(ifr.ifr_name, ifname.c_str(), IFNAMSIZ - 1);
    if (::ioctl(outSocketFd, SIOCGIFINDEX, &ifr) < 0) {
        outError = "ioctl(SIOCGIFINDEX) failed: " + std::string(std::strerror(errno));
        ::close(outSocketFd);
        outSocketFd = -1;
        return false;
    }
    outIfIndex = ifr.ifr_ifindex;

    if (::ioctl(outSocketFd, SIOCGIFHWADDR, &ifr) < 0) {
        outError = "ioctl(SIOCGIFHWADDR) failed: " + std::string(std::strerror(errno));
        ::close(outSocketFd);
        outSocketFd = -1;
        return false;
    }
    for (std::size_t i = 0; i < outSourceMac.size(); ++i) {
        outSourceMac[i] = static_cast<std::uint8_t>(ifr.ifr_hwaddr.sa_data[i]);
    }

    sockaddr_ll sll {};
    sll.sll_family = AF_PACKET;
    sll.sll_protocol = htons(kEtherTypeEthercat);
    sll.sll_ifindex = outIfIndex;
    if (::bind(outSocketFd, reinterpret_cast<sockaddr*>(&sll), sizeof(sll)) < 0) {
        outError = "bind() failed: " + std::string(std::strerror(errno));
        ::close(outSocketFd);
        outSocketFd = -1;
        return false;
    }

    return true;
}

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
    // EtherCAT auto-increment addresses are signed: 0, -1, -2, ...
    return static_cast<std::uint16_t>(0U - position);
}

const char* commandName(std::uint8_t cmd) {
    switch (cmd) {
    case kCommandLrw:
        return "LRW";
    case kCommandLwr:
        return "LWR";
    case kCommandLrd:
        return "LRD";
    case kCommandAprd:
        return "APRD";
    case kCommandApwr:
        return "APWR";
    case kCommandBrd:
        return "BRD";
    case kCommandBwr:
        return "BWR";
    default:
        return "CMD";
    }
}

MailboxStatusMode parseMailboxStatusMode(const char* value) {
    if (value == nullptr) {
        return MailboxStatusMode::Hybrid;
    }
    const std::string text(value);
    if (text == "strict") {
        return MailboxStatusMode::Strict;
    }
    if (text == "poll") {
        return MailboxStatusMode::Poll;
    }
    return MailboxStatusMode::Hybrid;
}

void incrementMailboxErrorClassCounter(MailboxDiagnostics& diagnostics, MailboxErrorClass cls) {
    switch (cls) {
    case MailboxErrorClass::Timeout:
        ++diagnostics.errorTimeout;
        break;
    case MailboxErrorClass::Busy:
        ++diagnostics.errorBusy;
        break;
    case MailboxErrorClass::ParseReject:
        ++diagnostics.errorParseReject;
        break;
    case MailboxErrorClass::StaleCounter:
        ++diagnostics.errorStaleCounter;
        break;
    case MailboxErrorClass::Abort:
        ++diagnostics.errorAbort;
        break;
    case MailboxErrorClass::TransportIo:
        ++diagnostics.errorTransportIo;
        break;
    case MailboxErrorClass::Unknown:
        ++diagnostics.errorUnknown;
        break;
    case MailboxErrorClass::None:
        break;
    }
}

bool isIgnorableSdoParseError(const std::string& error) {
    return (error.find("Unexpected CoE service") != std::string::npos) ||
           (error.find("Unexpected SDO command") != std::string::npos) ||
           (error.find("address mismatch") != std::string::npos) ||
           (error.find("toggle mismatch") != std::string::npos);
}

bool isTransientMailboxTransportError(const std::string& error) {
    return (error.find("timeout") != std::string::npos) ||
           (error.find("response frame not found") != std::string::npos) ||
           (error.find("select() failed") != std::string::npos) ||
           (error.find("recv() failed") != std::string::npos);
}

void sleepMailboxBackoff(int attempt, int baseDelayMs, int maxDelayMs) {
    const auto shift = std::min(attempt, 10);
    const auto delay = std::min(maxDelayMs, baseDelayMs << shift);
    std::this_thread::sleep_for(std::chrono::milliseconds(std::max(1, delay)));
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

bool sendAndReceiveDatagram(
    int socketFd,
    int ifIndex,
    int timeoutMs,
    std::size_t maxFramesPerCycle,
    std::uint16_t expectedWorkingCounter,
    std::array<std::uint8_t, 6>& destinationMac,
    std::array<std::uint8_t, 6>& sourceMac,
    const EthercatDatagramRequest& request,
    std::uint16_t& outWkc,
    std::vector<std::uint8_t>& outPayload,
    std::string& outError) {
    const auto frame = EthercatFrameCodec::buildDatagramFrame(
        destinationMac.data(), sourceMac.data(), request);

    sockaddr_ll target {};
    target.sll_family = AF_PACKET;
    target.sll_protocol = htons(kEtherTypeEthercat);
    target.sll_ifindex = ifIndex;
    target.sll_halen = 6;
    std::copy(destinationMac.begin(), destinationMac.end(), target.sll_addr);

    const auto sent = ::sendto(socketFd, frame.data(), frame.size(), 0,
                               reinterpret_cast<const sockaddr*>(&target), sizeof(target));
    if (sent < 0 || static_cast<std::size_t>(sent) != frame.size()) {
        outError = "sendto() failed: " + std::string(std::strerror(errno));
        return false;
    }

    const auto start = std::chrono::steady_clock::now();
    std::size_t scannedFrames = 0U;
    std::vector<std::uint8_t> rxFrame(1518U, 0U);
    while (scannedFrames < maxFramesPerCycle) {
        const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - start);
        const auto remainingMs = timeoutMs - static_cast<int>(elapsed.count());
        if (remainingMs <= 0) {
            outError = "receive timeout";
            return false;
        }

        fd_set readSet;
        FD_ZERO(&readSet);
        FD_SET(socketFd, &readSet);

        timeval timeout {};
        timeout.tv_sec = remainingMs / 1000;
        timeout.tv_usec = (remainingMs % 1000) * 1000;

        const int selectResult = ::select(socketFd + 1, &readSet, nullptr, nullptr, &timeout);
        if (selectResult == 0) {
            outError = "receive timeout";
            return false;
        }
        if (selectResult < 0) {
            outError = "select() failed: " + std::string(std::strerror(errno));
            return false;
        }

        const auto received = ::recv(socketFd, rxFrame.data(), rxFrame.size(), 0);
        if (received < 0) {
            outError = "recv() failed: " + std::string(std::strerror(errno));
            return false;
        }
        rxFrame.resize(static_cast<std::size_t>(received));
        ++scannedFrames;

        const auto parsed = EthercatFrameCodec::parseDatagramFrame(
            rxFrame, request.command, request.datagramIndex, request.payload.size());
        if (!parsed) {
            continue;
        }
        if (parsed->workingCounter < expectedWorkingCounter) {
            outError = "working counter too low (got=" + std::to_string(parsed->workingCounter) +
                       ", expected>=" + std::to_string(expectedWorkingCounter) + ")";
            return false;
        }

        outWkc = parsed->workingCounter;
        outPayload = parsed->payload;
        return true;
    }

    outError = "response frame not found in cycle window";
    return false;
}

} // namespace

LinuxRawSocketTransport::LinuxRawSocketTransport(std::string ifname) : ifname_(std::move(ifname)) {}
LinuxRawSocketTransport::LinuxRawSocketTransport(std::string primaryIfname, std::string secondaryIfname)
    : ifname_(std::move(primaryIfname)), secondaryIfname_(std::move(secondaryIfname)),
      redundancyEnabled_(true) {}

LinuxRawSocketTransport::~LinuxRawSocketTransport() { close(); }

void LinuxRawSocketTransport::setCycleTimeoutMs(int timeoutMs) {
    timeoutMs_ = (timeoutMs <= 0) ? 1 : timeoutMs;
}

void LinuxRawSocketTransport::setLogicalAddress(std::uint32_t logicalAddress) {
    logicalAddress_ = logicalAddress;
}

void LinuxRawSocketTransport::setExpectedWorkingCounter(std::uint16_t expectedWorkingCounter) {
    expectedWorkingCounter_ = expectedWorkingCounter;
}

void LinuxRawSocketTransport::setMaxFramesPerCycle(std::size_t maxFramesPerCycle) {
    maxFramesPerCycle_ = (maxFramesPerCycle == 0U) ? 1U : maxFramesPerCycle;
}

void LinuxRawSocketTransport::enableRedundancy(bool enabled) { redundancyEnabled_ = enabled; }

void LinuxRawSocketTransport::setMailboxConfiguration(std::uint16_t writeOffset, std::uint16_t writeSize,
                                                      std::uint16_t readOffset, std::uint16_t readSize) {
    mailboxWriteOffset_ = writeOffset;
    mailboxWriteSize_ = writeSize;
    mailboxReadOffset_ = readOffset;
    mailboxReadSize_ = readSize;
}

bool LinuxRawSocketTransport::open() {
    close();
    mailboxStatusMode_ = parseMailboxStatusMode(std::getenv("OEC_MAILBOX_STATUS_MODE"));
    if (const char* env = std::getenv("OEC_MAILBOX_EMERGENCY_QUEUE_LIMIT")) {
        try {
            emergencyQueueLimit_ = std::max<std::size_t>(1U, static_cast<std::size_t>(std::stoul(env, nullptr, 0)));
        } catch (...) {
            // Keep default on parse failure.
        }
    }
    lastWorkingCounter_ = 0;
    lastMailboxErrorClass_ = MailboxErrorClass::None;
    dcDiagnostics_ = DcDiagnostics{};
    if (!openEthercatInterfaceSocket(ifname_, socketFd_, ifIndex_, sourceMac_, error_)) {
        close();
        return false;
    }

    if (redundancyEnabled_ && !secondaryIfname_.empty()) {
        std::array<std::uint8_t, 6> secondaryMac {};
        if (!openEthercatInterfaceSocket(secondaryIfname_, secondarySocketFd_, secondaryIfIndex_,
                                         secondaryMac, error_)) {
            close();
            return false;
        }
    }

    error_.clear();
    return true;
}

void LinuxRawSocketTransport::close() {
    if (socketFd_ >= 0) {
        ::close(socketFd_);
        socketFd_ = -1;
    }
    ifIndex_ = 0;
    if (secondarySocketFd_ >= 0) {
        ::close(secondarySocketFd_);
        secondarySocketFd_ = -1;
    }
    secondaryIfIndex_ = 0;
    lastWorkingCounter_ = 0;
    lastFrameUsedSecondary_ = false;
    outputWindows_.clear();
    while (!emergencies_.empty()) {
        emergencies_.pop();
    }
    lastMailboxErrorClass_ = MailboxErrorClass::None;
    dcDiagnostics_ = DcDiagnostics{};
}

bool LinuxRawSocketTransport::exchange(const std::vector<std::uint8_t>& txProcessData,
                                       std::vector<std::uint8_t>& rxProcessData) {
    if (socketFd_ < 0) {
        error_ = "transport not open";
        return false;
    }
    if (rxProcessData.size() != txProcessData.size()) {
        error_ = "TX/RX process image size mismatch";
        return false;
    }

    const auto logicalLo = static_cast<std::uint16_t>(logicalAddress_ & 0xFFFFU);
    const auto logicalHi = static_cast<std::uint16_t>((logicalAddress_ >> 16U) & 0xFFFFU);

    auto sendPrimaryOrSecondary = [&](const EthercatDatagramRequest& req,
                                      std::uint16_t& outWkc,
                                      std::vector<std::uint8_t>& outPayload) -> bool {
        if (sendAndReceiveDatagram(socketFd_, ifIndex_, timeoutMs_, maxFramesPerCycle_,
                                   expectedWorkingCounter_, destinationMac_, sourceMac_,
                                   req, outWkc, outPayload, error_)) {
            lastFrameUsedSecondary_ = false;
            return true;
        }
        if (redundancyEnabled_ && secondarySocketFd_ >= 0) {
            if (sendAndReceiveDatagram(secondarySocketFd_, secondaryIfIndex_, timeoutMs_, maxFramesPerCycle_,
                                       expectedWorkingCounter_, destinationMac_, sourceMac_,
                                       req, outWkc, outPayload, error_)) {
                lastFrameUsedSecondary_ = true;
                return true;
            }
        }
        return false;
    };

    const bool traceWkc = (std::getenv("OEC_TRACE_WKC") != nullptr);
    const std::uint32_t inputLogicalAddress = logicalAddress_ + static_cast<std::uint32_t>(txProcessData.size());
    const auto inputLogicalLo = static_cast<std::uint16_t>(inputLogicalAddress & 0xFFFFU);
    const auto inputLogicalHi = static_cast<std::uint16_t>((inputLogicalAddress >> 16U) & 0xFFFFU);

    std::uint16_t lwrWkc = 0;
    std::uint16_t lrdWkc = 0;
    std::vector<std::uint8_t> lwrAck;
    std::vector<std::uint8_t> lrdPayload;

    EthercatDatagramRequest lwr;
    lwr.command = kCommandLwr;
    lwr.datagramIndex = datagramIndex_++;
    lwr.adp = logicalLo;
    lwr.ado = logicalHi;
    lwr.payload = txProcessData;

    if (!sendPrimaryOrSecondary(lwr, lwrWkc, lwrAck)) {
        if (traceWkc) {
            std::cerr << "[oec] " << commandName(lwr.command) << " failed: " << error_ << '\n';
        }
        return false;
    }
    if (traceWkc) {
        std::cerr << "[oec] " << commandName(lwr.command) << " wkc=" << lwrWkc << '\n';
    }

    EthercatDatagramRequest lrd;
    lrd.command = kCommandLrd;
    lrd.datagramIndex = datagramIndex_++;
    lrd.adp = inputLogicalLo;
    lrd.ado = inputLogicalHi;
    lrd.payload.assign(rxProcessData.size(), 0U);

    if (!sendPrimaryOrSecondary(lrd, lrdWkc, lrdPayload)) {
        if (traceWkc) {
            std::cerr << "[oec] " << commandName(lrd.command) << " failed: " << error_ << '\n';
        }
        return false;
    }
    if (traceWkc) {
        std::cerr << "[oec] " << commandName(lrd.command) << " wkc=" << lrdWkc << '\n';
    }

    // Optional field-debug path: confirm written outputs by reading mapped SM2 process RAM.
    const bool traceOutputVerify = (std::getenv("OEC_TRACE_OUTPUT_VERIFY") != nullptr);
    if (traceOutputVerify && !outputWindows_.empty()) {
        for (const auto& window : outputWindows_) {
            const std::uint32_t relLogical = window.logicalStart - logicalAddress_;
            if (relLogical >= txProcessData.size()) {
                continue;
            }
            const std::size_t available = txProcessData.size() - static_cast<std::size_t>(relLogical);
            const std::size_t readLen = std::min<std::size_t>(window.length, available);
            if (readLen == 0U) {
                continue;
            }

            EthercatDatagramRequest verify;
            verify.command = kCommandAprd;
            verify.datagramIndex = datagramIndex_++;
            verify.adp = toAutoIncrementAddress(window.slavePosition);
            verify.ado = window.physicalStart;
            verify.payload.assign(readLen, 0U);

            std::uint16_t verifyWkc = 0;
            std::vector<std::uint8_t> physicalBytes;
            std::string verifyError;
            if (!sendAndReceiveDatagram(socketFd_, ifIndex_, timeoutMs_, maxFramesPerCycle_,
                                        expectedWorkingCounter_, destinationMac_, sourceMac_,
                                        verify, verifyWkc, physicalBytes, verifyError)) {
                std::cerr << "[oec-verify] slave=" << window.slavePosition
                          << " APRD@0x" << std::hex << window.physicalStart << std::dec
                          << " failed: " << verifyError << '\n';
                continue;
            }

            bool mismatch = false;
            for (std::size_t i = 0; i < readLen; ++i) {
                const auto expected = txProcessData[static_cast<std::size_t>(relLogical) + i];
                if (physicalBytes[i] != expected) {
                    mismatch = true;
                    break;
                }
            }

            if (mismatch) {
                std::cerr << "[oec-verify] slave=" << window.slavePosition
                          << " wkc=" << verifyWkc
                          << " logical=0x" << std::hex << window.logicalStart
                          << " physical=0x" << window.physicalStart
                          << " len=" << std::dec << readLen
                          << " mismatch expected:";
                for (std::size_t i = 0; i < readLen; ++i) {
                    const auto byte = txProcessData[static_cast<std::size_t>(relLogical) + i];
                    std::cerr << " " << std::hex << static_cast<int>(byte);
                }
                std::cerr << " actual:";
                for (std::size_t i = 0; i < readLen; ++i) {
                    std::cerr << " " << std::hex << static_cast<int>(physicalBytes[i]);
                }
                std::cerr << std::dec << '\n';
            } else {
                std::cerr << "[oec-verify] slave=" << window.slavePosition
                          << " wkc=" << verifyWkc
                          << " logical=0x" << std::hex << window.logicalStart
                          << " physical=0x" << window.physicalStart
                          << " len=" << std::dec << readLen
                          << " output image matches\n";
            }
        }
    }

    lastWorkingCounter_ = lrdWkc;
    rxProcessData = std::move(lrdPayload);
    error_.clear();
    return true;
}

bool LinuxRawSocketTransport::requestNetworkState(SlaveState state) {
    if (socketFd_ < 0) {
        error_ = "transport not open";
        return false;
    }

    const auto currentIndex = datagramIndex_++;
    EthercatDatagramRequest request;
    request.command = kCommandBwr;
    request.datagramIndex = currentIndex;
    request.adp = 0x0000;
    request.ado = kRegisterAlControl;
    request.payload = {static_cast<std::uint8_t>(state), 0x00U};

    std::uint16_t wkc = 0;
    std::vector<std::uint8_t> payload;
    if (!sendAndReceiveDatagram(socketFd_, ifIndex_, timeoutMs_, maxFramesPerCycle_,
                                expectedWorkingCounter_, destinationMac_, sourceMac_,
                                request, wkc, payload, error_)) {
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

    const auto currentIndex = datagramIndex_++;
    EthercatDatagramRequest request;
    request.command = kCommandBrd;
    request.datagramIndex = currentIndex;
    request.adp = 0x0000;
    request.ado = kRegisterAlStatus;
    request.payload = {0x00U, 0x00U};

    std::uint16_t wkc = 0;
    std::vector<std::uint8_t> payload;
    if (!sendAndReceiveDatagram(socketFd_, ifIndex_, timeoutMs_, maxFramesPerCycle_,
                                expectedWorkingCounter_, destinationMac_, sourceMac_,
                                request, wkc, payload, error_)) {
        return false;
    }
    if (payload.size() < 2) {
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

    const auto currentIndex = datagramIndex_++;
    EthercatDatagramRequest request;
    request.command = kCommandApwr;
    request.datagramIndex = currentIndex;
    request.adp = toAutoIncrementAddress(position);
    request.ado = kRegisterAlControl;
    request.payload = {static_cast<std::uint8_t>(state), 0x00U};

    std::uint16_t wkc = 0;
    std::vector<std::uint8_t> payload;
    if (!sendAndReceiveDatagram(socketFd_, ifIndex_, timeoutMs_, maxFramesPerCycle_,
                                expectedWorkingCounter_, destinationMac_, sourceMac_,
                                request, wkc, payload, error_)) {
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

    const auto currentIndex = datagramIndex_++;
    EthercatDatagramRequest request;
    request.command = kCommandAprd;
    request.datagramIndex = currentIndex;
    request.adp = toAutoIncrementAddress(position);
    request.ado = kRegisterAlStatus;
    request.payload = {0x00U, 0x00U};

    std::uint16_t wkc = 0;
    std::vector<std::uint8_t> payload;
    if (!sendAndReceiveDatagram(socketFd_, ifIndex_, timeoutMs_, maxFramesPerCycle_,
                                expectedWorkingCounter_, destinationMac_, sourceMac_,
                                request, wkc, payload, error_)) {
        return false;
    }
    if (payload.size() < 2) {
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

    const auto currentIndex = datagramIndex_++;
    EthercatDatagramRequest request;
    request.command = kCommandAprd;
    request.datagramIndex = currentIndex;
    request.adp = toAutoIncrementAddress(position);
    request.ado = kRegisterAlStatusCode;
    request.payload = {0x00U, 0x00U};

    std::uint16_t wkc = 0;
    std::vector<std::uint8_t> payload;
    if (!sendAndReceiveDatagram(socketFd_, ifIndex_, timeoutMs_, maxFramesPerCycle_,
                                expectedWorkingCounter_, destinationMac_, sourceMac_,
                                request, wkc, payload, error_)) {
        return false;
    }
    if (payload.size() < 2) {
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

    const auto currentIndex = datagramIndex_++;
    EthercatDatagramRequest request;
    request.command = kCommandAprd;
    request.datagramIndex = currentIndex;
    request.adp = toAutoIncrementAddress(slavePosition);
    request.ado = kRegisterDcSystemTime;
    request.payload.assign(8U, 0U);

    std::uint16_t wkc = 0;
    std::vector<std::uint8_t> payload;
    if (!sendAndReceiveDatagram(socketFd_, ifIndex_, timeoutMs_, maxFramesPerCycle_,
                                expectedWorkingCounter_, destinationMac_, sourceMac_,
                                request, wkc, payload, outError)) {
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

    const auto currentIndex = datagramIndex_++;
    EthercatDatagramRequest request;
    request.command = kCommandApwr;
    request.datagramIndex = currentIndex;
    request.adp = toAutoIncrementAddress(slavePosition);
    request.ado = kRegisterDcSystemTimeOffset;
    request.payload.clear();
    request.payload.reserve(8U);
    writeLe64Signed(request.payload, offsetNs);

    std::uint16_t wkc = 0;
    std::vector<std::uint8_t> payload;
    if (!sendAndReceiveDatagram(socketFd_, ifIndex_, timeoutMs_, maxFramesPerCycle_,
                                expectedWorkingCounter_, destinationMac_, sourceMac_,
                                request, wkc, payload, outError)) {
        ++dcDiagnostics_.writeFailure;
        return false;
    }

    lastWorkingCounter_ = wkc;
    ++dcDiagnostics_.writeSuccess;
    return true;
}

bool LinuxRawSocketTransport::reconfigureSlave(std::uint16_t position) {
    return requestSlaveState(position, SlaveState::Init) &&
           requestSlaveState(position, SlaveState::PreOp) &&
           requestSlaveState(position, SlaveState::SafeOp);
}

bool LinuxRawSocketTransport::failoverSlave(std::uint16_t position) {
    return requestSlaveState(position, SlaveState::SafeOp);
}

bool LinuxRawSocketTransport::sdoUpload(std::uint16_t slavePosition, const SdoAddress& address,
                                        std::vector<std::uint8_t>& outData, std::uint32_t& outAbortCode,
                                        std::string& outError) {
    ++mailboxDiagnostics_.transactionsStarted;
    outData.clear();
    outAbortCode = 0U;
    outError.clear();
    MailboxErrorClass txErrorClass = MailboxErrorClass::None;
    auto setTxErrorClass = [&](MailboxErrorClass cls) {
        if (txErrorClass == MailboxErrorClass::None) {
            txErrorClass = cls;
        }
    };
    auto fail = [&]() -> bool {
        if (txErrorClass == MailboxErrorClass::None) {
            txErrorClass = classifyMailboxError(outError);
            if (txErrorClass == MailboxErrorClass::None) {
                txErrorClass = MailboxErrorClass::Unknown;
            }
        }
        lastMailboxErrorClass_ = txErrorClass;
        incrementMailboxErrorClassCounter(mailboxDiagnostics_, txErrorClass);
        ++mailboxDiagnostics_.transactionsFailed;
        return false;
    };

    const bool forceTimeoutTest = (std::getenv("OEC_MAILBOX_TEST_FORCE_TIMEOUT") != nullptr);
    if (socketFd_ < 0 && !forceTimeoutTest) {
        outError = "transport not open";
        setTxErrorClass(MailboxErrorClass::TransportIo);
        return fail();
    }
    const auto statusMode = mailboxStatusMode_;
    auto enqueueEmergency = [&](const EmergencyMessage& emergency) {
        if (emergencies_.size() >= emergencyQueueLimit_) {
            emergencies_.pop();
            ++mailboxDiagnostics_.emergencyDropped;
        }
        emergencies_.push(emergency);
        ++mailboxDiagnostics_.emergencyQueued;
    };
    const auto adp = toAutoIncrementAddress(slavePosition);
    std::uint16_t writeOffset = mailboxWriteOffset_;
    std::uint16_t writeSize = mailboxWriteSize_;
    std::uint16_t readOffset = mailboxReadOffset_;
    std::uint16_t readSize = mailboxReadSize_;
    int mailboxRetries = 2;
    int mailboxBackoffBaseMs = 1;
    int mailboxBackoffMaxMs = 20;
    if (const char* env = std::getenv("OEC_MAILBOX_RETRIES")) {
        try {
            mailboxRetries = std::max(0, std::stoi(env));
        } catch (...) {
            // Keep defaults if parsing fails.
        }
    }
    if (const char* env = std::getenv("OEC_MAILBOX_BACKOFF_BASE_MS")) {
        try {
            mailboxBackoffBaseMs = std::max(1, std::stoi(env));
        } catch (...) {
        }
    }
    if (const char* env = std::getenv("OEC_MAILBOX_BACKOFF_MAX_MS")) {
        try {
            mailboxBackoffMaxMs = std::max(mailboxBackoffBaseMs, std::stoi(env));
        } catch (...) {
        }
    }

    auto datagramWithRetry = [&](const EthercatDatagramRequest& req,
                                 std::uint16_t& outWkc,
                                 std::vector<std::uint8_t>& outPayload) -> bool {
        (void)req;
        (void)outWkc;
        (void)outPayload;
        std::string firstError;
        if (forceTimeoutTest) {
            for (int attempt = 0; attempt <= mailboxRetries; ++attempt) {
                const std::string localError = "Timed out waiting for CoE mailbox response";
                if (firstError.empty()) {
                    firstError = localError;
                }
                if (attempt == mailboxRetries) {
                    outError = localError;
                    ++mailboxDiagnostics_.mailboxTimeouts;
                    setTxErrorClass(MailboxErrorClass::Timeout);
                    return false;
                }
                ++mailboxDiagnostics_.datagramRetries;
                sleepMailboxBackoff(attempt, mailboxBackoffBaseMs, mailboxBackoffMaxMs);
            }
        }
        for (int attempt = 0; attempt <= mailboxRetries; ++attempt) {
            std::string localError;
            if (sendAndReceiveDatagram(socketFd_, ifIndex_, timeoutMs_, maxFramesPerCycle_,
                                       expectedWorkingCounter_, destinationMac_, sourceMac_,
                                       req, outWkc, outPayload, localError)) {
                return true;
            }
            if (firstError.empty()) {
                firstError = localError;
            }
            if (!isTransientMailboxTransportError(localError) || attempt == mailboxRetries) {
                outError = localError;
                if (isTransientMailboxTransportError(localError)) {
                    ++mailboxDiagnostics_.mailboxTimeouts;
                }
                setTxErrorClass(classifyMailboxError(localError));
                return false;
            }
            ++mailboxDiagnostics_.datagramRetries;
            sleepMailboxBackoff(attempt, mailboxBackoffBaseMs, mailboxBackoffMaxMs);
        }
        outError = firstError.empty() ? "Mailbox datagram failed" : firstError;
        if (isTransientMailboxTransportError(outError)) {
            ++mailboxDiagnostics_.mailboxTimeouts;
            setTxErrorClass(MailboxErrorClass::Timeout);
        } else {
            setTxErrorClass(classifyMailboxError(outError));
        }
        return false;
    };

    auto readSmWindow = [&](std::uint8_t smIndex, std::uint16_t& outStart, std::uint16_t& outLen) -> bool {
        const auto currentIndex = datagramIndex_++;
        EthercatDatagramRequest req;
        req.command = kCommandAprd;
        req.datagramIndex = currentIndex;
        req.adp = adp;
        req.ado = static_cast<std::uint16_t>(kRegisterSmBase + (smIndex * 8U));
        req.payload.assign(8U, 0U);

        std::uint16_t wkc = 0;
        std::vector<std::uint8_t> payload;
        if (!datagramWithRetry(req, wkc, payload)) {
            return false;
        }
        if (payload.size() < 4U) {
            outError = "SM payload too short for mailbox";
            return false;
        }
        outStart = static_cast<std::uint16_t>(payload[0]) |
                   (static_cast<std::uint16_t>(payload[1]) << 8U);
        outLen = static_cast<std::uint16_t>(payload[2]) |
                 (static_cast<std::uint16_t>(payload[3]) << 8U);
        return true;
    };

    auto readSmStatus = [&](std::uint8_t smIndex, std::uint8_t& outStatus) -> bool {
        const auto currentIndex = datagramIndex_++;
        EthercatDatagramRequest req;
        req.command = kCommandAprd;
        req.datagramIndex = currentIndex;
        req.adp = adp;
        req.ado = static_cast<std::uint16_t>(kRegisterSmBase + (smIndex * 8U) + kRegisterSmStatusOffset);
        req.payload.assign(1U, 0U);

        std::uint16_t wkc = 0;
        std::vector<std::uint8_t> payload;
        if (!datagramWithRetry(req, wkc, payload)) {
            return false;
        }
        if (payload.empty()) {
            return false;
        }
        outStatus = payload[0];
        return true;
    };

    // Resolve mailbox windows from SM0/SM1 if available.
    std::uint16_t sm0Start = 0U, sm0Len = 0U, sm1Start = 0U, sm1Len = 0U;
    if (readSmWindow(0U, sm0Start, sm0Len) && readSmWindow(1U, sm1Start, sm1Len) &&
        sm0Len > 0U && sm1Len > 0U) {
        writeOffset = sm0Start;
        writeSize = sm0Len;
        readOffset = sm1Start;
        readSize = sm1Len;
    }

    auto mailboxWrite = [&](const std::vector<std::uint8_t>& coePayload,
                            std::uint8_t& outCounter) -> bool {
        // Status-driven gate: configurable strategy to accommodate ESC variant differences.
        if (statusMode != MailboxStatusMode::Poll) {
            bool writeReady = false;
            for (int probe = 0; probe < 3; ++probe) {
                std::uint8_t status = 0U;
                if (!readSmStatus(0U, status)) {
                    if (statusMode == MailboxStatusMode::Strict) {
                        outError = "SM0 status read failed in strict mode";
                        setTxErrorClass(MailboxErrorClass::Busy);
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
                setTxErrorClass(MailboxErrorClass::Busy);
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
        if (!datagramWithRetry(req, wkc, payload)) {
            return false;
        }
        ++mailboxDiagnostics_.mailboxWrites;
        lastWorkingCounter_ = wkc;
        return true;
    };

    auto mailboxReadMatching = [&](std::uint8_t expectedCounter, auto&& accept, EscMailboxFrame& outFrame) -> bool {
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeoutMs_);
        int idlePolls = 0;
        while (std::chrono::steady_clock::now() < deadline) {
            if (statusMode != MailboxStatusMode::Poll) {
                // Status-driven read gate with strict/hybrid policies.
                std::uint8_t status = 0U;
                const bool haveStatus = readSmStatus(1U, status);
                const bool mailboxHasData = haveStatus && ((status & 0x08U) != 0U);
                if (statusMode == MailboxStatusMode::Strict) {
                    if (!haveStatus) {
                        outError = "SM1 status read failed in strict mode";
                        setTxErrorClass(MailboxErrorClass::Busy);
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
            if (!datagramWithRetry(req, wkc, payload)) {
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
                enqueueEmergency(emergency);
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
                continue;
            }
            if ((decoded->counter & 0x07U) != (expectedCounter & 0x07U)) {
                ++mailboxDiagnostics_.staleCounterDrops;
                setTxErrorClass(MailboxErrorClass::StaleCounter);
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
                continue;
            }
            if (!accept(*decoded)) {
                ++mailboxDiagnostics_.parseRejects;
                setTxErrorClass(MailboxErrorClass::ParseReject);
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
                continue;
            }
            ++mailboxDiagnostics_.matchedResponses;
            outFrame = *decoded;
            return true;
        }
        outError = "Timed out waiting for CoE mailbox response";
        ++mailboxDiagnostics_.mailboxTimeouts;
        if (txErrorClass == MailboxErrorClass::None) {
            setTxErrorClass(MailboxErrorClass::Timeout);
        }
        return false;
    };

    std::uint8_t expectedCounter = 0U;
    if (!mailboxWrite(CoeMailboxProtocol::buildSdoInitiateUploadRequest(address), expectedCounter)) {
        return fail();
    }

    EscMailboxFrame responseFrame;
    CoeSdoInitiateUploadResponse init;
    bool fatalParseError = false;
    std::string fatalParseReason;
    if (!mailboxReadMatching(expectedCounter, [&](const EscMailboxFrame& frame) -> bool {
            auto parsed = CoeMailboxProtocol::parseSdoInitiateUploadResponse(frame.payload, address);
            if (parsed.success || parsed.error == "SDO abort") {
                init = std::move(parsed);
                return true;
            }
            if (!isIgnorableSdoParseError(parsed.error)) {
                fatalParseError = true;
                fatalParseReason = parsed.error;
                return true;
            }
            return false;
        }, responseFrame)) {
        return fail();
    }
    if (fatalParseError) {
        outError = fatalParseReason;
        setTxErrorClass(MailboxErrorClass::ParseReject);
        return fail();
    }
    if (!init.success) {
        outAbortCode = init.abortCode;
        outError = init.error;
        if (init.abortCode != 0U || init.error == "SDO abort") {
            setTxErrorClass(MailboxErrorClass::Abort);
        }
        return fail();
    }

    if (init.expedited) {
        outData = init.data;
        return true;
    }

    std::uint8_t toggle = 0;
    while (true) {
        expectedCounter = 0U;
        if (!mailboxWrite(CoeMailboxProtocol::buildSdoUploadSegmentRequest(toggle), expectedCounter)) {
            return fail();
        }
        CoeSdoSegmentUploadResponse seg;
        fatalParseError = false;
        fatalParseReason.clear();
        if (!mailboxReadMatching(expectedCounter, [&](const EscMailboxFrame& frame) -> bool {
                auto parsed = CoeMailboxProtocol::parseSdoUploadSegmentResponse(frame.payload);
                if (parsed.success && parsed.toggle == toggle) {
                    seg = std::move(parsed);
                    return true;
                }
                if (parsed.error == "SDO abort") {
                    seg = std::move(parsed);
                    return true;
                }
                if (!parsed.success && !isIgnorableSdoParseError(parsed.error)) {
                    fatalParseError = true;
                    fatalParseReason = parsed.error;
                    return true;
                }
                return false;
            }, responseFrame)) {
            return fail();
        }
        if (fatalParseError) {
            outError = fatalParseReason;
            setTxErrorClass(MailboxErrorClass::ParseReject);
            return fail();
        }
        if (!seg.success) {
            outAbortCode = seg.abortCode;
            outError = seg.error;
            if (seg.abortCode != 0U || seg.error == "SDO abort") {
                setTxErrorClass(MailboxErrorClass::Abort);
            }
            return fail();
        }
        outData.insert(outData.end(), seg.data.begin(), seg.data.end());
        toggle ^= 0x01U;
        if (seg.lastSegment) {
            break;
        }
    }

    lastMailboxErrorClass_ = MailboxErrorClass::None;
    return true;
}

bool LinuxRawSocketTransport::sdoDownload(std::uint16_t slavePosition, const SdoAddress& address,
                                          const std::vector<std::uint8_t>& data, std::uint32_t& outAbortCode,
                                          std::string& outError) {
    ++mailboxDiagnostics_.transactionsStarted;
    outAbortCode = 0U;
    outError.clear();
    MailboxErrorClass txErrorClass = MailboxErrorClass::None;
    auto setTxErrorClass = [&](MailboxErrorClass cls) {
        if (txErrorClass == MailboxErrorClass::None) {
            txErrorClass = cls;
        }
    };
    auto fail = [&]() -> bool {
        if (txErrorClass == MailboxErrorClass::None) {
            txErrorClass = classifyMailboxError(outError);
            if (txErrorClass == MailboxErrorClass::None) {
                txErrorClass = MailboxErrorClass::Unknown;
            }
        }
        lastMailboxErrorClass_ = txErrorClass;
        incrementMailboxErrorClassCounter(mailboxDiagnostics_, txErrorClass);
        ++mailboxDiagnostics_.transactionsFailed;
        return false;
    };

    const bool forceTimeoutTest = (std::getenv("OEC_MAILBOX_TEST_FORCE_TIMEOUT") != nullptr);
    if (socketFd_ < 0 && !forceTimeoutTest) {
        outError = "transport not open";
        setTxErrorClass(MailboxErrorClass::TransportIo);
        return fail();
    }
    const auto statusMode = mailboxStatusMode_;
    auto enqueueEmergency = [&](const EmergencyMessage& emergency) {
        if (emergencies_.size() >= emergencyQueueLimit_) {
            emergencies_.pop();
            ++mailboxDiagnostics_.emergencyDropped;
        }
        emergencies_.push(emergency);
        ++mailboxDiagnostics_.emergencyQueued;
    };
    const auto adp = toAutoIncrementAddress(slavePosition);
    std::uint16_t writeOffset = mailboxWriteOffset_;
    std::uint16_t writeSize = mailboxWriteSize_;
    std::uint16_t readOffset = mailboxReadOffset_;
    std::uint16_t readSize = mailboxReadSize_;
    int mailboxRetries = 2;
    int mailboxBackoffBaseMs = 1;
    int mailboxBackoffMaxMs = 20;
    if (const char* env = std::getenv("OEC_MAILBOX_RETRIES")) {
        try {
            mailboxRetries = std::max(0, std::stoi(env));
        } catch (...) {
            // Keep defaults if parsing fails.
        }
    }
    if (const char* env = std::getenv("OEC_MAILBOX_BACKOFF_BASE_MS")) {
        try {
            mailboxBackoffBaseMs = std::max(1, std::stoi(env));
        } catch (...) {
        }
    }
    if (const char* env = std::getenv("OEC_MAILBOX_BACKOFF_MAX_MS")) {
        try {
            mailboxBackoffMaxMs = std::max(mailboxBackoffBaseMs, std::stoi(env));
        } catch (...) {
        }
    }

    auto datagramWithRetry = [&](const EthercatDatagramRequest& req,
                                 std::uint16_t& outWkc,
                                 std::vector<std::uint8_t>& outPayload) -> bool {
        (void)req;
        (void)outWkc;
        (void)outPayload;
        std::string firstError;
        if (forceTimeoutTest) {
            for (int attempt = 0; attempt <= mailboxRetries; ++attempt) {
                const std::string localError = "Timed out waiting for CoE mailbox response";
                if (firstError.empty()) {
                    firstError = localError;
                }
                if (attempt == mailboxRetries) {
                    outError = localError;
                    ++mailboxDiagnostics_.mailboxTimeouts;
                    setTxErrorClass(MailboxErrorClass::Timeout);
                    return false;
                }
                ++mailboxDiagnostics_.datagramRetries;
                sleepMailboxBackoff(attempt, mailboxBackoffBaseMs, mailboxBackoffMaxMs);
            }
        }
        for (int attempt = 0; attempt <= mailboxRetries; ++attempt) {
            std::string localError;
            if (sendAndReceiveDatagram(socketFd_, ifIndex_, timeoutMs_, maxFramesPerCycle_,
                                       expectedWorkingCounter_, destinationMac_, sourceMac_,
                                       req, outWkc, outPayload, localError)) {
                return true;
            }
            if (firstError.empty()) {
                firstError = localError;
            }
            if (!isTransientMailboxTransportError(localError) || attempt == mailboxRetries) {
                outError = localError;
                if (isTransientMailboxTransportError(localError)) {
                    ++mailboxDiagnostics_.mailboxTimeouts;
                }
                setTxErrorClass(classifyMailboxError(localError));
                return false;
            }
            ++mailboxDiagnostics_.datagramRetries;
            sleepMailboxBackoff(attempt, mailboxBackoffBaseMs, mailboxBackoffMaxMs);
        }
        outError = firstError.empty() ? "Mailbox datagram failed" : firstError;
        if (isTransientMailboxTransportError(outError)) {
            ++mailboxDiagnostics_.mailboxTimeouts;
            setTxErrorClass(MailboxErrorClass::Timeout);
        } else {
            setTxErrorClass(classifyMailboxError(outError));
        }
        return false;
    };

    auto readSmWindow = [&](std::uint8_t smIndex, std::uint16_t& outStart, std::uint16_t& outLen) -> bool {
        const auto currentIndex = datagramIndex_++;
        EthercatDatagramRequest req;
        req.command = kCommandAprd;
        req.datagramIndex = currentIndex;
        req.adp = adp;
        req.ado = static_cast<std::uint16_t>(kRegisterSmBase + (smIndex * 8U));
        req.payload.assign(8U, 0U);

        std::uint16_t wkc = 0;
        std::vector<std::uint8_t> payload;
        if (!datagramWithRetry(req, wkc, payload)) {
            return false;
        }
        if (payload.size() < 4U) {
            outError = "SM payload too short for mailbox";
            return false;
        }
        outStart = static_cast<std::uint16_t>(payload[0]) |
                   (static_cast<std::uint16_t>(payload[1]) << 8U);
        outLen = static_cast<std::uint16_t>(payload[2]) |
                 (static_cast<std::uint16_t>(payload[3]) << 8U);
        return true;
    };

    auto readSmStatus = [&](std::uint8_t smIndex, std::uint8_t& outStatus) -> bool {
        const auto currentIndex = datagramIndex_++;
        EthercatDatagramRequest req;
        req.command = kCommandAprd;
        req.datagramIndex = currentIndex;
        req.adp = adp;
        req.ado = static_cast<std::uint16_t>(kRegisterSmBase + (smIndex * 8U) + kRegisterSmStatusOffset);
        req.payload.assign(1U, 0U);

        std::uint16_t wkc = 0;
        std::vector<std::uint8_t> payload;
        if (!datagramWithRetry(req, wkc, payload)) {
            return false;
        }
        if (payload.empty()) {
            return false;
        }
        outStatus = payload[0];
        return true;
    };

    // Resolve mailbox windows from SM0/SM1 if available.
    std::uint16_t sm0Start = 0U, sm0Len = 0U, sm1Start = 0U, sm1Len = 0U;
    if (readSmWindow(0U, sm0Start, sm0Len) && readSmWindow(1U, sm1Start, sm1Len) &&
        sm0Len > 0U && sm1Len > 0U) {
        writeOffset = sm0Start;
        writeSize = sm0Len;
        readOffset = sm1Start;
        readSize = sm1Len;
    }

    auto mailboxWrite = [&](const std::vector<std::uint8_t>& coePayload,
                            std::uint8_t& outCounter) -> bool {
        // Status-driven gate: configurable strategy to accommodate ESC variant differences.
        if (statusMode != MailboxStatusMode::Poll) {
            bool writeReady = false;
            for (int probe = 0; probe < 3; ++probe) {
                std::uint8_t status = 0U;
                if (!readSmStatus(0U, status)) {
                    if (statusMode == MailboxStatusMode::Strict) {
                        outError = "SM0 status read failed in strict mode";
                        setTxErrorClass(MailboxErrorClass::Busy);
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
                setTxErrorClass(MailboxErrorClass::Busy);
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
        if (!datagramWithRetry(req, wkc, payload)) {
            return false;
        }
        ++mailboxDiagnostics_.mailboxWrites;
        lastWorkingCounter_ = wkc;
        return true;
    };

    auto mailboxReadMatching = [&](std::uint8_t expectedCounter, auto&& accept, EscMailboxFrame& outFrame) -> bool {
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeoutMs_);
        int idlePolls = 0;
        while (std::chrono::steady_clock::now() < deadline) {
            if (statusMode != MailboxStatusMode::Poll) {
                // Status-driven read gate with strict/hybrid policies.
                std::uint8_t status = 0U;
                const bool haveStatus = readSmStatus(1U, status);
                const bool mailboxHasData = haveStatus && ((status & 0x08U) != 0U);
                if (statusMode == MailboxStatusMode::Strict) {
                    if (!haveStatus) {
                        outError = "SM1 status read failed in strict mode";
                        setTxErrorClass(MailboxErrorClass::Busy);
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
            if (!datagramWithRetry(req, wkc, payload)) {
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
                enqueueEmergency(emergency);
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
                continue;
            }
            if ((decoded->counter & 0x07U) != (expectedCounter & 0x07U)) {
                ++mailboxDiagnostics_.staleCounterDrops;
                setTxErrorClass(MailboxErrorClass::StaleCounter);
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
                continue;
            }
            if (!accept(*decoded)) {
                ++mailboxDiagnostics_.parseRejects;
                setTxErrorClass(MailboxErrorClass::ParseReject);
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
                continue;
            }
            ++mailboxDiagnostics_.matchedResponses;
            outFrame = *decoded;
            return true;
        }
        outError = "Timed out waiting for CoE mailbox response";
        ++mailboxDiagnostics_.mailboxTimeouts;
        if (txErrorClass == MailboxErrorClass::None) {
            setTxErrorClass(MailboxErrorClass::Timeout);
        }
        return false;
    };

    std::uint8_t expectedCounter = 0U;
    if (!mailboxWrite(CoeMailboxProtocol::buildSdoInitiateDownloadRequest(
            address, static_cast<std::uint32_t>(data.size())), expectedCounter)) {
        return fail();
    }

    EscMailboxFrame responseFrame;
    CoeSdoAckResponse ack;
    bool fatalParseError = false;
    std::string fatalParseReason;
    if (!mailboxReadMatching(expectedCounter, [&](const EscMailboxFrame& frame) -> bool {
            auto parsed = CoeMailboxProtocol::parseSdoInitiateDownloadResponse(frame.payload, address);
            if (parsed.success || parsed.error == "SDO abort") {
                ack = std::move(parsed);
                return true;
            }
            if (!isIgnorableSdoParseError(parsed.error)) {
                fatalParseError = true;
                fatalParseReason = parsed.error;
                return true;
            }
            return false;
        }, responseFrame)) {
        return fail();
    }
    if (fatalParseError) {
        outError = fatalParseReason;
        setTxErrorClass(MailboxErrorClass::ParseReject);
        return fail();
    }
    if (!ack.success) {
        outAbortCode = ack.abortCode;
        outError = ack.error;
        if (ack.abortCode != 0U || ack.error == "SDO abort") {
            setTxErrorClass(MailboxErrorClass::Abort);
        }
        return fail();
    }

    constexpr std::size_t kSegmentPayloadMax = 7;
    std::size_t offset = 0;
    std::uint8_t toggle = 0;
    while (offset < data.size()) {
        const auto remaining = data.size() - offset;
        const auto chunk = std::min<std::size_t>(remaining, kSegmentPayloadMax);
        std::vector<std::uint8_t> segment(data.begin() + static_cast<std::ptrdiff_t>(offset),
                                          data.begin() + static_cast<std::ptrdiff_t>(offset + chunk));
        const bool lastSegment = (offset + chunk) >= data.size();
        expectedCounter = 0U;
        if (!mailboxWrite(CoeMailboxProtocol::buildSdoDownloadSegmentRequest(
                toggle, lastSegment, segment, kSegmentPayloadMax), expectedCounter)) {
            return fail();
        }
        fatalParseError = false;
        fatalParseReason.clear();
        if (!mailboxReadMatching(expectedCounter, [&](const EscMailboxFrame& frame) -> bool {
                auto parsed = CoeMailboxProtocol::parseSdoDownloadSegmentResponse(frame.payload, toggle);
                if (parsed.success || parsed.error == "SDO abort") {
                    ack = std::move(parsed);
                    return true;
                }
                if (!isIgnorableSdoParseError(parsed.error)) {
                    fatalParseError = true;
                    fatalParseReason = parsed.error;
                    return true;
                }
                return false;
            }, responseFrame)) {
            return fail();
        }
        if (fatalParseError) {
            outError = fatalParseReason;
            setTxErrorClass(MailboxErrorClass::ParseReject);
            return fail();
        }
        if (!ack.success) {
            outAbortCode = ack.abortCode;
            outError = ack.error;
            if (ack.abortCode != 0U || ack.error == "SDO abort") {
                setTxErrorClass(MailboxErrorClass::Abort);
            }
            return fail();
        }
        offset += chunk;
        toggle ^= 0x01U;
    }

    lastMailboxErrorClass_ = MailboxErrorClass::None;
    return true;
}

bool LinuxRawSocketTransport::configurePdo(std::uint16_t slavePosition, std::uint16_t assignIndex,
                                           const std::vector<PdoMappingEntry>& entries,
                                           std::string& outError) {
    outError.clear();

    auto writeSdoU8 = [&](std::uint16_t index, std::uint8_t subIndex, std::uint8_t value) -> bool {
        std::uint32_t abortCode = 0;
        std::string sdoError;
        const std::vector<std::uint8_t> data = {value};
        SdoAddress address;
        address.index = index;
        address.subIndex = subIndex;
        if (!sdoDownload(slavePosition, address, data, abortCode, sdoError)) {
            outError = "SDO write 0x" + std::to_string(index) + ":" + std::to_string(subIndex) + " failed: " + sdoError;
            return false;
        }
        return true;
    };

    auto writeSdoU16 = [&](std::uint16_t index, std::uint8_t subIndex, std::uint16_t value) -> bool {
        std::uint32_t abortCode = 0;
        std::string sdoError;
        const std::vector<std::uint8_t> data = {
            static_cast<std::uint8_t>(value & 0xFFU),
            static_cast<std::uint8_t>((value >> 8U) & 0xFFU),
        };
        SdoAddress address;
        address.index = index;
        address.subIndex = subIndex;
        if (!sdoDownload(slavePosition, address, data, abortCode, sdoError)) {
            outError = "SDO write 0x" + std::to_string(index) + ":" + std::to_string(subIndex) + " failed: " + sdoError;
            return false;
        }
        return true;
    };

    auto writeSdoU32 = [&](std::uint16_t index, std::uint8_t subIndex, std::uint32_t value) -> bool {
        std::uint32_t abortCode = 0;
        std::string sdoError;
        const std::vector<std::uint8_t> data = {
            static_cast<std::uint8_t>(value & 0xFFU),
            static_cast<std::uint8_t>((value >> 8U) & 0xFFU),
            static_cast<std::uint8_t>((value >> 16U) & 0xFFU),
            static_cast<std::uint8_t>((value >> 24U) & 0xFFU),
        };
        SdoAddress address;
        address.index = index;
        address.subIndex = subIndex;
        if (!sdoDownload(slavePosition, address, data, abortCode, sdoError)) {
            outError = "SDO write 0x" + std::to_string(index) + ":" + std::to_string(subIndex) + " failed: " + sdoError;
            return false;
        }
        return true;
    };

    // Disable mapping object before editing entries.
    if (!writeSdoU8(assignIndex, 0, 0)) {
        return false;
    }

    std::uint8_t sub = 1;
    for (const auto& e : entries) {
        const auto mapEntry = static_cast<std::uint32_t>(e.index) |
                              (static_cast<std::uint32_t>(e.subIndex) << 16U) |
                              (static_cast<std::uint32_t>(e.bitLength) << 24U);
        if (!writeSdoU32(assignIndex, sub, mapEntry)) {
            return false;
        }
        ++sub;
    }

    if (!writeSdoU8(assignIndex, 0, static_cast<std::uint8_t>(entries.size()))) {
        return false;
    }

    // Sync manager assignment: 0x1C12 for RxPDO (0x1600..0x17FF), 0x1C13 for TxPDO (0x1A00..0x1BFF).
    const bool isRx = (assignIndex >= 0x1600U && assignIndex < 0x1800U);
    const std::uint16_t smAssign = isRx ? 0x1C12U : 0x1C13U;
    if (!writeSdoU8(smAssign, 0, 0)) {
        return false;
    }
    if (!writeSdoU16(smAssign, 1, assignIndex)) {
        return false;
    }
    if (!writeSdoU8(smAssign, 0, 1)) {
        return false;
    }

    return true;
}

bool LinuxRawSocketTransport::pollEmergency(EmergencyMessage& outEmergency) {
    if (emergencies_.empty()) {
        return false;
    }
    outEmergency = emergencies_.front();
    emergencies_.pop();
    return true;
}

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
        if (!sendAndReceiveDatagram(socketFd_, ifIndex_, timeoutMs_, maxFramesPerCycle_,
                                    expectedWorkingCounter_, destinationMac_, sourceMac_,
                                    request, wkc, payload, error_)) {
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
        return sendAndReceiveDatagram(socketFd_, ifIndex_, timeoutMs_, maxFramesPerCycle_,
                                      expectedWorkingCounter_, destinationMac_, sourceMac_,
                                      request, wkc, payload, error_);
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
        if (!sendAndReceiveDatagram(socketFd_, ifIndex_, timeoutMs_, maxFramesPerCycle_,
                                    expectedWorkingCounter_, destinationMac_, sourceMac_,
                                    req, wkc, payload, outError)) {
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
        return sendAndReceiveDatagram(socketFd_, ifIndex_, timeoutMs_, maxFramesPerCycle_,
                                      expectedWorkingCounter_, destinationMac_, sourceMac_,
                                      req, wkc, ack, outError);
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
        return sendAndReceiveDatagram(socketFd_, ifIndex_, timeoutMs_, maxFramesPerCycle_,
                                      expectedWorkingCounter_, destinationMac_, sourceMac_,
                                      req, wkc, ack, outError);
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

bool LinuxRawSocketTransport::foeRead(std::uint16_t slavePosition, const FoERequest& request,
                                      FoEResponse& outResponse, std::string& outError) {
    (void)slavePosition;
    (void)request;
    outResponse.success = false;
    outResponse.error = "FoE over mailbox for LinuxRawSocketTransport is pending full ESC mailbox integration";
    outError = outResponse.error;
    return false;
}

bool LinuxRawSocketTransport::foeWrite(std::uint16_t slavePosition, const FoERequest& request,
                                       const std::vector<std::uint8_t>& data, std::string& outError) {
    (void)slavePosition;
    (void)request;
    (void)data;
    outError = "FoE over mailbox for LinuxRawSocketTransport is pending full ESC mailbox integration";
    return false;
}

bool LinuxRawSocketTransport::eoeSend(std::uint16_t slavePosition, const std::vector<std::uint8_t>& frame,
                                      std::string& outError) {
    (void)slavePosition;
    (void)frame;
    outError = "EoE over mailbox for LinuxRawSocketTransport is pending full ESC mailbox integration";
    return false;
}

bool LinuxRawSocketTransport::eoeReceive(std::uint16_t slavePosition, std::vector<std::uint8_t>& frame,
                                         std::string& outError) {
    (void)slavePosition;
    (void)frame;
    outError = "EoE over mailbox for LinuxRawSocketTransport is pending full ESC mailbox integration";
    return false;
}

std::string LinuxRawSocketTransport::lastError() const { return error_; }
std::uint16_t LinuxRawSocketTransport::lastWorkingCounter() const { return lastWorkingCounter_; }
MailboxDiagnostics LinuxRawSocketTransport::mailboxDiagnostics() const { return mailboxDiagnostics_; }
void LinuxRawSocketTransport::resetMailboxDiagnostics() {
    mailboxDiagnostics_ = MailboxDiagnostics{};
    lastMailboxErrorClass_ = MailboxErrorClass::None;
}
void LinuxRawSocketTransport::setMailboxStatusMode(MailboxStatusMode mode) { mailboxStatusMode_ = mode; }
MailboxStatusMode LinuxRawSocketTransport::mailboxStatusMode() const { return mailboxStatusMode_; }
void LinuxRawSocketTransport::setEmergencyQueueLimit(std::size_t limit) {
    emergencyQueueLimit_ = std::max<std::size_t>(1U, limit);
    while (emergencies_.size() > emergencyQueueLimit_) {
        emergencies_.pop();
        ++mailboxDiagnostics_.emergencyDropped;
    }
}
std::size_t LinuxRawSocketTransport::emergencyQueueLimit() const { return emergencyQueueLimit_; }
MailboxErrorClass LinuxRawSocketTransport::lastMailboxErrorClass() const { return lastMailboxErrorClass_; }
MailboxErrorClass LinuxRawSocketTransport::classifyMailboxError(const std::string& errorText) {
    if (errorText.empty()) {
        return MailboxErrorClass::None;
    }
    if (errorText.find("SDO abort") != std::string::npos) {
        return MailboxErrorClass::Abort;
    }
    if (errorText.find("timeout") != std::string::npos ||
        errorText.find("Timed out") != std::string::npos ||
        errorText.find("response frame not found") != std::string::npos) {
        return MailboxErrorClass::Timeout;
    }
    if (errorText.find("busy") != std::string::npos ||
        errorText.find("status read failed in strict mode") != std::string::npos) {
        return MailboxErrorClass::Busy;
    }
    if (errorText.find("toggle mismatch") != std::string::npos ||
        errorText.find("address mismatch") != std::string::npos ||
        errorText.find("Unexpected CoE service") != std::string::npos ||
        errorText.find("Unexpected SDO command") != std::string::npos ||
        errorText.find("parse") != std::string::npos) {
        return MailboxErrorClass::ParseReject;
    }
    if (errorText.find("stale") != std::string::npos ||
        errorText.find("counter mismatch") != std::string::npos) {
        return MailboxErrorClass::StaleCounter;
    }
    if (errorText.find("socket") != std::string::npos ||
        errorText.find("sendto") != std::string::npos ||
        errorText.find("recv") != std::string::npos ||
        errorText.find("select()") != std::string::npos ||
        errorText.find("transport not open") != std::string::npos ||
        errorText.find("not open") != std::string::npos) {
        return MailboxErrorClass::TransportIo;
    }
    return MailboxErrorClass::Unknown;
}
DcDiagnostics LinuxRawSocketTransport::dcDiagnostics() const { return dcDiagnostics_; }
void LinuxRawSocketTransport::resetDcDiagnostics() { dcDiagnostics_ = DcDiagnostics{}; }

} // namespace oec
