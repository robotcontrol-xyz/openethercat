/**
 * @file transport_module_boundary_tests.cpp
 * @brief Module-boundary regression tests for Linux transport refactor.
 */

#include <cassert>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

namespace {

std::string readFile(const std::string& path) {
    std::ifstream in(path);
    if (!in.is_open()) {
        return {};
    }
    std::ostringstream buffer;
    buffer << in.rdbuf();
    return buffer.str();
}

bool contains(const std::string& text, const std::string& needle) {
    return text.find(needle) != std::string::npos;
}

void assertContainsAll(const std::string& fileText, const std::vector<std::string>& needles) {
    for (const auto& needle : needles) {
        assert(contains(fileText, needle));
    }
}

void assertContainsNone(const std::string& fileText, const std::vector<std::string>& needles) {
    for (const auto& needle : needles) {
        assert(!contains(fileText, needle));
    }
}

} // namespace

int main() {
    const std::string sourceRoot = OEC_SOURCE_DIR;
    const auto corePath = sourceRoot + "/src/transport/linux_raw_socket_transport.cpp";
    const auto coreIoPath = sourceRoot + "/src/transport/linux_raw_socket_transport_core_io.cpp";
    const auto stateDcPath = sourceRoot + "/src/transport/linux_raw_socket_transport_state_dc.cpp";

    const auto coreText = readFile(corePath);
    const auto coreIoText = readFile(coreIoPath);
    const auto stateDcText = readFile(stateDcPath);

    assert(!coreText.empty());
    assert(!coreIoText.empty());
    assert(!stateDcText.empty());

    // Core I/O module owns Linux socket lifecycle and datagram exchange plumbing.
    assertContainsAll(coreIoText, {
        "bool LinuxRawSocketTransport::open(",
        "void LinuxRawSocketTransport::close(",
        "bool LinuxRawSocketTransport::sendDatagramRequest(",
        "bool sendAndReceiveDatagram(",
    });

    // State/DC module owns AL-state APIs and DC register operations.
    assertContainsAll(stateDcText, {
        "bool LinuxRawSocketTransport::requestNetworkState(",
        "bool LinuxRawSocketTransport::readNetworkState(",
        "bool LinuxRawSocketTransport::requestSlaveState(",
        "bool LinuxRawSocketTransport::readSlaveState(",
        "bool LinuxRawSocketTransport::readSlaveAlStatusCode(",
        "bool LinuxRawSocketTransport::readDcSystemTime(",
        "bool LinuxRawSocketTransport::writeDcSystemTimeOffset(",
    });

    // The main transport module should not regress by reclaiming these moved responsibilities.
    assertContainsNone(coreText, {
        "bool LinuxRawSocketTransport::open(",
        "void LinuxRawSocketTransport::close(",
        "bool LinuxRawSocketTransport::sendDatagramRequest(",
        "bool LinuxRawSocketTransport::requestNetworkState(",
        "bool LinuxRawSocketTransport::readNetworkState(",
        "bool LinuxRawSocketTransport::requestSlaveState(",
        "bool LinuxRawSocketTransport::readSlaveState(",
        "bool LinuxRawSocketTransport::readSlaveAlStatusCode(",
        "bool LinuxRawSocketTransport::readDcSystemTime(",
        "bool LinuxRawSocketTransport::writeDcSystemTimeOffset(",
    });

    return 0;
}
