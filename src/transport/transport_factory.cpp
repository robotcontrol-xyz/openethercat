#include "openethercat/transport/transport_factory.hpp"

#include <algorithm>
#include <cctype>

#include "openethercat/transport/linux_raw_socket_transport.hpp"
#include "openethercat/transport/mock_transport.hpp"

namespace oec {
namespace {

std::string trimCopy(std::string value) {
    value.erase(value.begin(),
                std::find_if(value.begin(), value.end(), [](unsigned char c) { return !std::isspace(c); }));
    value.erase(std::find_if(value.rbegin(), value.rend(), [](unsigned char c) { return !std::isspace(c); }).base(),
                value.end());
    return value;
}

} // namespace

bool TransportFactory::parseTransportSpec(const std::string& spec,
                                          TransportFactoryConfig& outConfig,
                                          std::string& outError) {
    outError.clear();
    const auto trimmed = trimCopy(spec);
    if (trimmed.empty()) {
        outError = "transport spec is empty";
        return false;
    }

    if (trimmed == "mock") {
        outConfig.kind = TransportKind::Mock;
        outConfig.primaryInterface.clear();
        outConfig.secondaryInterface.clear();
        outConfig.enableRedundancy = false;
        return true;
    }

    constexpr const char* kLinuxPrefix = "linux:";
    if (trimmed.rfind(kLinuxPrefix, 0) == 0) {
        outConfig.kind = TransportKind::LinuxRawSocket;
        const std::string rest = trimCopy(trimmed.substr(6));
        if (rest.empty()) {
            outError = "linux transport requires interface name, e.g. linux:eth0";
            return false;
        }

        const auto comma = rest.find(',');
        if (comma == std::string::npos) {
            outConfig.primaryInterface = trimCopy(rest);
            outConfig.secondaryInterface.clear();
            outConfig.enableRedundancy = false;
            if (outConfig.primaryInterface.empty()) {
                outError = "linux transport requires interface name, e.g. linux:eth0";
                return false;
            }
            return true;
        }

        outConfig.primaryInterface = trimCopy(rest.substr(0, comma));
        outConfig.secondaryInterface = trimCopy(rest.substr(comma + 1));
        outConfig.enableRedundancy = !outConfig.secondaryInterface.empty();
        if (outConfig.primaryInterface.empty() || outConfig.secondaryInterface.empty()) {
            outError = "invalid linux transport spec, expected linux:<primary>,<secondary>";
            return false;
        }
        return true;
    }

    outError = "unsupported transport spec '" + spec + "', expected 'mock' or 'linux:<ifname>[,<ifname2>]'";
    return false;
}

std::unique_ptr<ITransport> TransportFactory::create(const TransportFactoryConfig& config,
                                                     std::string& outError) {
    outError.clear();

    if (config.kind == TransportKind::Mock) {
        return std::make_unique<MockTransport>(config.mockInputBytes, config.mockOutputBytes);
    }

    if (config.kind != TransportKind::LinuxRawSocket) {
        outError = "unsupported transport kind";
        return nullptr;
    }

    if (config.primaryInterface.empty()) {
        outError = "linux transport requires primaryInterface";
        return nullptr;
    }

    std::unique_ptr<LinuxRawSocketTransport> transport;
    if (config.secondaryInterface.empty()) {
        transport = std::make_unique<LinuxRawSocketTransport>(config.primaryInterface);
    } else {
        transport = std::make_unique<LinuxRawSocketTransport>(config.primaryInterface, config.secondaryInterface);
    }

    transport->setCycleTimeoutMs(config.cycleTimeoutMs);
    transport->setLogicalAddress(config.logicalAddress);
    transport->setExpectedWorkingCounter(config.expectedWorkingCounter);
    transport->setMaxFramesPerCycle(config.maxFramesPerCycle);
    transport->enableRedundancy(config.enableRedundancy);
    return transport;
}

} // namespace oec
