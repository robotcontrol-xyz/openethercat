/**
 * @file dc_hardware_sync_demo.cpp
 * @brief openEtherCAT source file.
 */

#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>
#include <thread>

#include "openethercat/master/distributed_clock.hpp"
#include "openethercat/transport/linux_raw_socket_transport.hpp"
#include "openethercat/transport/transport_factory.hpp"

namespace {

std::uint32_t parseUnsigned(const char* text, const char* label) {
    try {
        return static_cast<std::uint32_t>(std::stoul(text, nullptr, 0));
    } catch (...) {
        throw std::runtime_error(std::string("Invalid ") + label + ": " + text);
    }
}

void usage(const char* argv0) {
    std::cout << "Usage: " << argv0 << " <transport-spec> [slave-pos] [samples] [period-ms]\n"
              << "  transport-spec: linux:<ifname> | linux:<ifname_primary>,<ifname_secondary>\n"
              << "Example:\n"
              << "  " << argv0 << " linux:enp2s0 1 500 10\n";
}

} // namespace

int main(int argc, char** argv) {
    if (argc < 2) {
        usage(argv[0]);
        return 1;
    }

    try {
        const std::string spec = argv[1];
        const std::uint16_t slavePosition =
            static_cast<std::uint16_t>((argc > 2) ? parseUnsigned(argv[2], "slave position") : 1U);
        const std::size_t samples =
            static_cast<std::size_t>((argc > 3) ? parseUnsigned(argv[3], "samples") : 500U);
        const int periodMs = static_cast<int>((argc > 4) ? parseUnsigned(argv[4], "period-ms") : 10U);

        oec::TransportFactoryConfig tc;
        std::string error;
        if (!oec::TransportFactory::parseTransportSpec(spec, tc, error)) {
            std::cerr << "Invalid transport spec: " << error << '\n';
            return 1;
        }
        auto transport = oec::TransportFactory::create(tc, error);
        if (!transport) {
            std::cerr << "Transport creation failed: " << error << '\n';
            return 1;
        }
        auto* linux = dynamic_cast<oec::LinuxRawSocketTransport*>(transport.get());
        if (!linux) {
            std::cerr << "This demo requires linux transport\n";
            return 1;
        }
        if (!linux->open()) {
            std::cerr << "Transport open failed: " << linux->lastError() << '\n';
            return 1;
        }

        oec::DistributedClockController dc({
            .filterAlpha = 0.2,
            .kp = 0.1,
            .ki = 0.01,
            .correctionClampNs = 20000
        });

        std::cout << "Running DC hardware sync prototype, slave=" << slavePosition
                  << " samples=" << samples << " period_ms=" << periodMs << '\n';

        for (std::size_t i = 0; i < samples; ++i) {
            std::int64_t slaveNs = 0;
            if (!linux->readDcSystemTime(slavePosition, slaveNs, error)) {
                std::cerr << "readDcSystemTime failed at sample " << i << ": " << error << '\n';
                linux->close();
                return 2;
            }

            const auto hostNow = std::chrono::steady_clock::now().time_since_epoch();
            const auto hostNs = std::chrono::duration_cast<std::chrono::nanoseconds>(hostNow).count();
            const auto corr = dc.update({.referenceTimeNs = slaveNs, .localTimeNs = hostNs});
            if (corr.has_value()) {
                if (!linux->writeDcSystemTimeOffset(slavePosition, *corr, error)) {
                    std::cerr << "writeDcSystemTimeOffset failed at sample " << i << ": " << error << '\n';
                    linux->close();
                    return 3;
                }
            }

            if ((i + 1U) % 20U == 0U || (i + 1U) == samples) {
                const auto s = dc.stats();
                std::cout << "sample=" << (i + 1U)
                          << " offset_ns=" << s.lastOffsetNs
                          << " filtered_ns=" << s.filteredOffsetNs
                          << " corr_ns=" << s.correctionNs
                          << " jitter_rms_ns=" << s.jitterRmsNs
                          << " max_abs_offset_ns=" << s.maxAbsOffsetNs
                          << '\n';
            }

            std::this_thread::sleep_for(std::chrono::milliseconds(periodMs));
        }

        linux->close();
        return 0;
    } catch (const std::exception& ex) {
        std::cerr << ex.what() << '\n';
        return 1;
    }
}
