/**
 * @file mailbox_soak_demo.cpp
 * @brief openEtherCAT source file.
 */

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <exception>
#include <iomanip>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

#include "openethercat/master/coe_mailbox.hpp"
#include "openethercat/transport/linux_raw_socket_transport.hpp"
#include "openethercat/transport/transport_factory.hpp"

namespace {

std::uint32_t parseUnsigned(const char* text, const char* label) {
    try {
        return static_cast<std::uint32_t>(std::stoul(text, nullptr, 0));
    } catch (const std::exception&) {
        throw std::runtime_error(std::string("Invalid ") + label + ": " + text);
    }
}

double percentile(std::vector<double> values, double p) {
    if (values.empty()) {
        return 0.0;
    }
    std::sort(values.begin(), values.end());
    const auto idx = static_cast<std::size_t>(
        std::min<double>(values.size() - 1, (p / 100.0) * static_cast<double>(values.size() - 1)));
    return values[idx];
}

void printUsage(const char* argv0) {
    std::cout << "Usage: " << argv0 << " <transport-spec> [slave-pos] [index] [subindex] [cycles]\n"
              << "  transport-spec: linux:<ifname> | linux:<ifname_primary>,<ifname_secondary> | mock\n"
              << "  slave-pos:      default 1\n"
              << "  index:          default 0x1018\n"
              << "  subindex:       default 0x01\n"
              << "  cycles:         default 1000\n"
              << "Example:\n"
              << "  " << argv0 << " linux:enp2s0 1 0x1018 0x01 5000\n";
}

const char* toModeName(oec::MailboxStatusMode mode) {
    switch (mode) {
    case oec::MailboxStatusMode::Strict:
        return "strict";
    case oec::MailboxStatusMode::Hybrid:
        return "hybrid";
    case oec::MailboxStatusMode::Poll:
        return "poll";
    }
    return "hybrid";
}

const char* toErrorClassName(oec::MailboxErrorClass cls) {
    switch (cls) {
    case oec::MailboxErrorClass::None:
        return "none";
    case oec::MailboxErrorClass::Timeout:
        return "timeout";
    case oec::MailboxErrorClass::Busy:
        return "busy";
    case oec::MailboxErrorClass::ParseReject:
        return "parse_reject";
    case oec::MailboxErrorClass::StaleCounter:
        return "stale_counter";
    case oec::MailboxErrorClass::Abort:
        return "abort";
    case oec::MailboxErrorClass::TransportIo:
        return "transport_io";
    case oec::MailboxErrorClass::Unknown:
        return "unknown";
    }
    return "unknown";
}

} // namespace

int main(int argc, char** argv) {
    if (argc < 2) {
        printUsage(argv[0]);
        return 1;
    }

    try {
        const std::string transportSpec = argv[1];
        const std::uint16_t slavePosition =
            static_cast<std::uint16_t>((argc > 2) ? parseUnsigned(argv[2], "slave position") : 1U);
        const std::uint16_t index =
            static_cast<std::uint16_t>((argc > 3) ? parseUnsigned(argv[3], "index") : 0x1018U);
        const std::uint8_t subIndex =
            static_cast<std::uint8_t>((argc > 4) ? parseUnsigned(argv[4], "subindex") : 0x01U);
        const std::size_t cycles =
            static_cast<std::size_t>((argc > 5) ? parseUnsigned(argv[5], "cycles") : 1000U);
        const bool jsonMode = (std::getenv("OEC_SOAK_JSON") != nullptr);

        // Create transport through factory so the same benchmark works on mock/Linux.
        oec::TransportFactoryConfig config;
        std::string error;
        if (!oec::TransportFactory::parseTransportSpec(transportSpec, config, error)) {
            std::cerr << "Invalid transport spec: " << error << '\n';
            return 1;
        }
        auto transport = oec::TransportFactory::create(config, error);
        if (!transport) {
            std::cerr << "Transport creation failed: " << error << '\n';
            return 1;
        }
        if (!transport->open()) {
            std::cerr << "Transport open failed: " << transport->lastError() << '\n';
            return 1;
        }

        // Linux transport exposes mailbox diagnostics/status tuning not present on all transports.
        auto* linux = dynamic_cast<oec::LinuxRawSocketTransport*>(transport.get());
        if (linux) {
            linux->resetMailboxDiagnostics();
            if (jsonMode) {
                std::cout << "{\"type\":\"start\",\"mailbox_status_mode\":\"" << toModeName(linux->mailboxStatusMode())
                          << "\",\"mailbox_emergency_queue_limit\":" << linux->emergencyQueueLimit() << "}\n";
            } else {
                std::cout << "mailbox_status_mode=" << toModeName(linux->mailboxStatusMode()) << '\n';
                std::cout << "mailbox_emergency_queue_limit=" << linux->emergencyQueueLimit() << '\n';
            }
        }

        // Probe a single object repeatedly to characterize mailbox latency and failure classes.
        const oec::SdoAddress address{.index = index, .subIndex = subIndex};
        std::vector<double> latenciesUs;
        latenciesUs.reserve(cycles);
        std::size_t success = 0U;
        std::size_t failed = 0U;

        for (std::size_t i = 0; i < cycles; ++i) {
            // Each iteration performs one full CoE SDO upload transaction.
            std::vector<std::uint8_t> data;
            std::uint32_t abortCode = 0U;
            std::string sdoError;
            const auto start = std::chrono::steady_clock::now();
            const bool ok = transport->sdoUpload(slavePosition, address, data, abortCode, sdoError);
            const auto end = std::chrono::steady_clock::now();
            const auto us = std::chrono::duration_cast<std::chrono::microseconds>(end - start).count();
            latenciesUs.push_back(static_cast<double>(us));

            if (ok) {
                ++success;
            } else {
                ++failed;
                if (failed <= 5U) {
                    if (jsonMode) {
                        std::cerr << "{\"type\":\"failure\",\"cycle\":" << i
                                  << ",\"abort\":\"0x" << std::hex << abortCode << std::dec
                                  << "\",\"class\":\"" << (linux ? toErrorClassName(linux->lastMailboxErrorClass()) : "n/a")
                                  << "\",\"error\":\"" << sdoError << "\"}\n";
                    } else {
                        std::cerr << "SDO failure cycle " << i
                                  << " abort=0x" << std::hex << abortCode << std::dec
                                  << " class=" << (linux ? toErrorClassName(linux->lastMailboxErrorClass()) : "n/a")
                                  << " error=" << sdoError << '\n';
                    }
                }
            }

            // Emit rolling percentiles to observe drift/instability during long runs.
            if ((i + 1U) % 200U == 0U || (i + 1U) == cycles) {
                const double p50 = percentile(latenciesUs, 50.0);
                const double p95 = percentile(latenciesUs, 95.0);
                const double p99 = percentile(latenciesUs, 99.0);
                if (jsonMode) {
                    std::cout << "{\"type\":\"progress\",\"done\":" << (i + 1U)
                              << ",\"total\":" << cycles
                              << ",\"success\":" << success
                              << ",\"failed\":" << failed
                              << ",\"p50_us\":" << std::fixed << std::setprecision(1) << p50
                              << ",\"p95_us\":" << p95
                              << ",\"p99_us\":" << p99
                              << "}\n";
                } else {
                    std::cout << "progress=" << (i + 1U) << "/" << cycles
                              << " success=" << success
                              << " failed=" << failed
                              << " p50_us=" << std::fixed << std::setprecision(1) << p50
                              << " p95_us=" << p95
                              << " p99_us=" << p99
                              << '\n';
                }
            }
        }

        if (linux) {
            const auto d = linux->mailboxDiagnostics();
            if (jsonMode) {
                std::cout << "{\"type\":\"mailbox_diag\""
                          << ",\"schema_version\":" << d.schemaVersion
                          << ",\"tx_started\":" << d.transactionsStarted
                          << ",\"tx_failed\":" << d.transactionsFailed
                          << ",\"writes\":" << d.mailboxWrites
                          << ",\"reads\":" << d.mailboxReads
                          << ",\"retries\":" << d.datagramRetries
                          << ",\"timeouts\":" << d.mailboxTimeouts
                          << ",\"stale_drop\":" << d.staleCounterDrops
                          << ",\"parse_reject\":" << d.parseRejects
                          << ",\"emergencies\":" << d.emergencyQueued
                          << ",\"emergencies_dropped\":" << d.emergencyDropped
                          << ",\"matched\":" << d.matchedResponses
                          << ",\"err_timeout\":" << d.errorTimeout
                          << ",\"err_busy\":" << d.errorBusy
                          << ",\"err_parse\":" << d.errorParseReject
                          << ",\"err_stale\":" << d.errorStaleCounter
                          << ",\"err_abort\":" << d.errorAbort
                          << ",\"err_io\":" << d.errorTransportIo
                          << ",\"err_unknown\":" << d.errorUnknown
                          << "}\n";
            } else {
                std::cout << "mailbox_diag"
                          << " schema_version=" << d.schemaVersion
                          << " tx_started=" << d.transactionsStarted
                          << " tx_failed=" << d.transactionsFailed
                          << " writes=" << d.mailboxWrites
                          << " reads=" << d.mailboxReads
                          << " retries=" << d.datagramRetries
                          << " timeouts=" << d.mailboxTimeouts
                          << " stale_drop=" << d.staleCounterDrops
                          << " parse_reject=" << d.parseRejects
                          << " emergencies=" << d.emergencyQueued
                          << " emergencies_dropped=" << d.emergencyDropped
                          << " matched=" << d.matchedResponses
                          << " err_timeout=" << d.errorTimeout
                          << " err_busy=" << d.errorBusy
                          << " err_parse=" << d.errorParseReject
                          << " err_stale=" << d.errorStaleCounter
                          << " err_abort=" << d.errorAbort
                          << " err_io=" << d.errorTransportIo
                          << " err_unknown=" << d.errorUnknown
                          << '\n';
            }
        }

        transport->close();
        return (failed == 0U) ? 0 : 2;
    } catch (const std::exception& ex) {
        std::cerr << ex.what() << '\n';
        return 1;
    }
}
