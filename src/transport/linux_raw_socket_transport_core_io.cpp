/**
 * @file linux_raw_socket_transport_core_io.cpp
 * @brief Linux socket lifecycle and core datagram I/O wrappers.
 */

#include "openethercat/transport/linux_raw_socket_transport.hpp"

#include <algorithm>
#include <cerrno>
#include <cstring>
#include <cstdlib>
#include <string>
#include <vector>
#include <chrono>

#include <linux/if_packet.h>
#include <net/if.h>
#include <netinet/in.h>
#include <sys/ioctl.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <unistd.h>

#include "openethercat/transport/ethercat_frame.hpp"

namespace oec {
namespace {

constexpr std::uint16_t kEtherTypeEthercat = 0x88A4;

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

bool sendAndReceiveDatagram(int socketFd,
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
    lastOutputWorkingCounter_ = 0;
    lastInputWorkingCounter_ = 0;
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
    lastOutputWorkingCounter_ = 0;
    lastInputWorkingCounter_ = 0;
    lastFrameUsedSecondary_ = false;
    outputWindows_.clear();
    while (!emergencies_.empty()) {
        emergencies_.pop();
    }
    lastMailboxErrorClass_ = MailboxErrorClass::None;
    dcDiagnostics_ = DcDiagnostics{};
}

bool LinuxRawSocketTransport::sendDatagramRequest(const EthercatDatagramRequest& request,
                                                  std::uint16_t& outWkc,
                                                  std::vector<std::uint8_t>& outPayload,
                                                  std::string& outError) {
    return sendAndReceiveDatagram(socketFd_, ifIndex_, timeoutMs_, maxFramesPerCycle_,
                                  expectedWorkingCounter_, destinationMac_, sourceMac_,
                                  request, outWkc, outPayload, outError);
}

} // namespace oec
