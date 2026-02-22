/**
 * @file coe_mailbox_protocol_tests.cpp
 * @brief openEtherCAT source file.
 */

#include <cassert>
#include <cstdlib>
#include <cstdint>
#include <iostream>
#include <queue>
#include <vector>

#include "openethercat/transport/coe_mailbox_protocol.hpp"
#include "openethercat/transport/linux_raw_socket_transport.hpp"

int main() {
    // ESC mailbox encode/decode round-trip.
    {
        oec::EscMailboxFrame frame;
        frame.channel = 1;
        frame.priority = 0;
        frame.type = oec::CoeMailboxProtocol::kMailboxTypeCoe;
        frame.counter = 3;
        frame.payload = {0xAA, 0xBB, 0xCC};

        const auto bytes = oec::CoeMailboxProtocol::encodeEscMailbox(frame);
        auto decoded = oec::CoeMailboxProtocol::decodeEscMailbox(bytes);
        assert(decoded.has_value());
        assert(decoded->type == oec::CoeMailboxProtocol::kMailboxTypeCoe);
        assert(decoded->counter == 3);
        assert(decoded->payload.size() == 3);
    }

    // Initiate upload (expedited) parsing.
    {
        // CoE service (0x0003), cmd 0x47 (expedited + size indicated + 3 bytes valid),
        // index 0x2000, sub 0x01, data [0x11 0x22 0x33 0x00]
        std::vector<std::uint8_t> payload = {0x03, 0x00, 0x47, 0x00, 0x20, 0x01, 0x11, 0x22, 0x33, 0x00};
        const auto parsed = oec::CoeMailboxProtocol::parseSdoInitiateUploadResponse(
            payload, {.index = 0x2000, .subIndex = 0x01});
        assert(parsed.success);
        assert(parsed.expedited);
        assert(parsed.data.size() == 3);
        assert(parsed.data[0] == 0x11);
    }

    // Upload segment parsing and toggle/last bits.
    {
        // cmd: toggle=1, unused=2, last=1 -> 0b0001 0101 = 0x15
        std::vector<std::uint8_t> payload = {0x03, 0x00, 0x15, 0xDE, 0xAD, 0xBE, 0xEF};
        auto seg = oec::CoeMailboxProtocol::parseSdoUploadSegmentResponse(payload);
        assert(seg.success);
        assert(seg.toggle == 1);
        assert(seg.lastSegment);
        assert(seg.data.size() == 2);
        assert(seg.data[0] == 0xDE);
    }

    // Download segment build sizing.
    {
        const std::vector<std::uint8_t> chunk = {1, 2, 3};
        const auto req = oec::CoeMailboxProtocol::buildSdoDownloadSegmentRequest(0, true, chunk, 7);
        assert(req.size() == 6);
        assert((req[2] & 0x01U) == 0x01U); // last segment bit
    }

    // Initiate download ack parsing with strict address matching.
    {
        // service=0x0003, cmd=0x60, index=0x2000, sub=0x01
        std::vector<std::uint8_t> payload = {0x03, 0x00, 0x60, 0x00, 0x20, 0x01};
        auto ack = oec::CoeMailboxProtocol::parseSdoInitiateDownloadResponse(
            payload, {.index = 0x2000, .subIndex = 0x01});
        assert(ack.success);

        auto mismatch = oec::CoeMailboxProtocol::parseSdoInitiateDownloadResponse(
            payload, {.index = 0x2001, .subIndex = 0x01});
        assert(!mismatch.success);
        assert(mismatch.error == "SDO response address mismatch");
    }

    // Download segment ack parsing with strict toggle.
    {
        // service=0x0003, cmd=0x30 => segment ack with toggle=1.
        std::vector<std::uint8_t> payload = {0x03, 0x00, 0x30};
        auto ack = oec::CoeMailboxProtocol::parseSdoDownloadSegmentResponse(payload, 1);
        assert(ack.success);
        assert(ack.toggle == 1);

        auto badToggle = oec::CoeMailboxProtocol::parseSdoDownloadSegmentResponse(payload, 0);
        assert(!badToggle.success);
        assert(badToggle.error == "SDO download segment toggle mismatch");
    }

    // CoE emergency decoding.
    {
        // service=0x0001, errorCode=0x8130, reg=0x10, mfg=[1..5]
        std::vector<std::uint8_t> payload = {0x01, 0x00, 0x30, 0x81, 0x10, 1, 2, 3, 4, 5};
        oec::EmergencyMessage em{};
        const bool ok = oec::CoeMailboxProtocol::parseEmergency(payload, 7, em);
        assert(ok);
        assert(em.errorCode == 0x8130);
        assert(em.errorRegister == 0x10);
        assert(em.manufacturerData[0] == 1);
        assert(em.slavePosition == 7);
    }

    // Mixed mailbox stream selection primitives:
    // emergency frame + stale SDO response + matching SDO response.
    {
        const oec::SdoAddress addr{.index = 0x2000, .subIndex = 0x01};
        constexpr std::uint8_t expectedCounter = 3U;
        std::queue<oec::EmergencyMessage> emergencies;
        bool matched = false;
        std::vector<std::uint8_t> matchedData;

        // 1) emergency frame, counter unrelated
        oec::EscMailboxFrame emFrame;
        emFrame.channel = 0;
        emFrame.priority = 0;
        emFrame.type = oec::CoeMailboxProtocol::kMailboxTypeCoe;
        emFrame.counter = 1;
        emFrame.payload = {0x01, 0x00, 0x30, 0x81, 0x10, 1, 2, 3, 4, 5};

        // 2) stale SDO response: correct payload, wrong counter
        oec::EscMailboxFrame staleSdo;
        staleSdo.channel = 0;
        staleSdo.priority = 0;
        staleSdo.type = oec::CoeMailboxProtocol::kMailboxTypeCoe;
        staleSdo.counter = 2;
        staleSdo.payload = {0x03, 0x00, 0x47, 0x00, 0x20, 0x01, 0x11, 0x22, 0x33, 0x00};

        // 3) matching SDO response: correct counter + address
        oec::EscMailboxFrame goodSdo = staleSdo;
        goodSdo.counter = expectedCounter;
        goodSdo.payload[6] = 0x44;
        goodSdo.payload[7] = 0x55;
        goodSdo.payload[8] = 0x66;

        const std::vector<oec::EscMailboxFrame> frames{emFrame, staleSdo, goodSdo};
        for (const auto& frame : frames) {
            oec::EmergencyMessage em{};
            if (oec::CoeMailboxProtocol::parseEmergency(frame.payload, 2, em)) {
                emergencies.push(em);
                continue;
            }
            if ((frame.counter & 0x07U) != expectedCounter) {
                continue;
            }
            const auto parsed = oec::CoeMailboxProtocol::parseSdoInitiateUploadResponse(frame.payload, addr);
            if (parsed.success) {
                matched = true;
                matchedData = parsed.data;
                break;
            }
        }

        assert(!emergencies.empty());
        assert(emergencies.front().errorCode == 0x8130);
        assert(matched);
        assert(matchedData.size() == 3);
        assert(matchedData[0] == 0x44);
    }

    // Mailbox status mode API should be configurable without opening transport.
    {
        oec::LinuxRawSocketTransport transport("eth0");
        assert(transport.mailboxStatusMode() == oec::MailboxStatusMode::Hybrid);
        transport.setMailboxStatusMode(oec::MailboxStatusMode::Poll);
        assert(transport.mailboxStatusMode() == oec::MailboxStatusMode::Poll);
        transport.setMailboxStatusMode(oec::MailboxStatusMode::Strict);
        assert(transport.mailboxStatusMode() == oec::MailboxStatusMode::Strict);
    }

    // Mailbox error classification API.
    {
        assert(oec::LinuxRawSocketTransport::classifyMailboxError("Timed out waiting for CoE mailbox response") ==
               oec::MailboxErrorClass::Timeout);
        assert(oec::LinuxRawSocketTransport::classifyMailboxError("response frame not found in cycle window") ==
               oec::MailboxErrorClass::Timeout);
        assert(oec::LinuxRawSocketTransport::classifyMailboxError("SM0 mailbox remained busy in strict mode") ==
               oec::MailboxErrorClass::Busy);
        assert(oec::LinuxRawSocketTransport::classifyMailboxError("Unexpected SDO command for upload") ==
               oec::MailboxErrorClass::ParseReject);
        assert(oec::LinuxRawSocketTransport::classifyMailboxError("counter mismatch while waiting for response") ==
               oec::MailboxErrorClass::StaleCounter);
        assert(oec::LinuxRawSocketTransport::classifyMailboxError("SDO abort") ==
               oec::MailboxErrorClass::Abort);
        assert(oec::LinuxRawSocketTransport::classifyMailboxError("transport not open") ==
               oec::MailboxErrorClass::TransportIo);
    }

    // Deterministic retry exhaustion + timeout classification path for mailbox transaction.
    {
        ::setenv("OEC_MAILBOX_TEST_FORCE_TIMEOUT", "1", 1);
        ::setenv("OEC_MAILBOX_RETRIES", "3", 1);
        oec::LinuxRawSocketTransport transport("eth0");
        transport.resetMailboxDiagnostics();

        std::vector<std::uint8_t> data;
        std::uint32_t abortCode = 0U;
        std::string error;
        const bool ok = transport.sdoUpload(1, {.index = 0x1018, .subIndex = 0x01}, data, abortCode, error);
        assert(!ok);
        assert(transport.lastMailboxErrorClass() == oec::MailboxErrorClass::Timeout);
        const auto d = transport.mailboxDiagnostics();
        assert(d.schemaVersion == 1U);
        assert(d.transactionsStarted == 1U);
        assert(d.transactionsFailed == 1U);
        assert(d.errorTimeout >= 1U);
        assert(d.datagramRetries >= 3U);
        assert(d.mailboxTimeouts >= 1U);

        ::unsetenv("OEC_MAILBOX_TEST_FORCE_TIMEOUT");
        ::unsetenv("OEC_MAILBOX_RETRIES");
    }

    std::cout << "coe_mailbox_protocol_tests passed\n";
    return 0;
}
