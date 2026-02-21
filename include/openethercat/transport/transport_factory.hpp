#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>

#include "openethercat/transport/i_transport.hpp"

namespace oec {

enum class TransportKind {
    Mock,
    LinuxRawSocket,
};

struct TransportFactoryConfig {
    TransportKind kind = TransportKind::Mock;
    std::size_t mockInputBytes = 0;
    std::size_t mockOutputBytes = 0;

    std::string primaryInterface;
    std::string secondaryInterface;
    int cycleTimeoutMs = 20;
    std::uint32_t logicalAddress = 0;
    std::uint16_t expectedWorkingCounter = 1;
    std::size_t maxFramesPerCycle = 128;
    bool enableRedundancy = false;
};

/**
 * @brief Create transport instances from a small runtime config.
 *
 * Transport spec format for parseTransportSpec:
 * - mock
 * - linux:<ifname>
 * - linux:<ifname_primary>,<ifname_secondary>
 */
class TransportFactory {
public:
    static bool parseTransportSpec(const std::string& spec,
                                   TransportFactoryConfig& outConfig,
                                   std::string& outError);

    static std::unique_ptr<ITransport> create(const TransportFactoryConfig& config,
                                              std::string& outError);
};

} // namespace oec
