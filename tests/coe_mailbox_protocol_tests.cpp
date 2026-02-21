#include <cassert>
#include <cstdint>
#include <iostream>
#include <vector>

#include "openethercat/transport/coe_mailbox_protocol.hpp"

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

    std::cout << "coe_mailbox_protocol_tests passed\n";
    return 0;
}
