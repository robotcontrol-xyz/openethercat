/**
 * @file dc_soak_demo.cpp
 * @brief Long-run DC and cycle KPI collection demo.
 */

#include <algorithm>
#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdint>
#include <cstdlib>
#include <cmath>
#include <deque>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

#include "openethercat/config/config_loader.hpp"
#include "openethercat/master/ethercat_master.hpp"
#include "openethercat/transport/transport_factory.hpp"

using namespace std::chrono_literals;

namespace {

std::atomic_bool gStopRequested{false};

void handleSignal(int) {
    gStopRequested.store(true);
}

std::uint32_t parseUnsigned(const char* text, const char* label) {
    try {
        return static_cast<std::uint32_t>(std::stoul(text, nullptr, 0));
    } catch (...) {
        throw std::runtime_error(std::string("Invalid ") + label + ": " + text);
    }
}

std::uint64_t percentile(std::vector<std::uint64_t> values, int p) {
    if (values.empty()) {
        return 0U;
    }
    std::sort(values.begin(), values.end());
    const auto idx = static_cast<std::size_t>(
        std::max(0.0, std::ceil((static_cast<double>(p) / 100.0) * static_cast<double>(values.size())) - 1.0));
    return values[std::min(idx, values.size() - 1U)];
}

void usage(const char* argv0) {
    std::cout
        << "Usage: " << argv0 << " <transport-spec> [duration-s] [period-us] [eni-path] [esi-dir]\n"
        << "  transport-spec: mock | linux:<ifname> | linux:<if_primary>,<if_secondary>\n"
        << "Defaults:\n"
        << "  duration-s = 60\n"
        << "  period-us = 1000\n"
        << "  eni-path   = examples/config/beckhoff_demo.eni.xml\n"
        << "  esi-dir    = examples/config\n"
        << "JSON mode:\n"
        << "  OEC_SOAK_JSON=1 " << argv0 << " linux:enp2s0 600 1000\n";
}

} // namespace

int main(int argc, char** argv) {
    std::signal(SIGINT, handleSignal);
    if (argc < 2) {
        usage(argv[0]);
        return 1;
    }

    try {
        const std::string transportSpec = argv[1];
        const std::uint32_t durationSeconds = (argc > 2) ? parseUnsigned(argv[2], "duration-s") : 60U;
        const std::uint32_t periodUs = (argc > 3) ? parseUnsigned(argv[3], "period-us") : 1000U;
        const std::string eniPath = (argc > 4) ? argv[4] : "examples/config/beckhoff_demo.eni.xml";
        const std::string esiDir = (argc > 5) ? argv[5] : "examples/config";
        const bool jsonMode = (std::getenv("OEC_SOAK_JSON") != nullptr) ||
                              (std::getenv("OEC_DC_SOAK_JSON") != nullptr);

        // Load network definition so soak run uses the same mapping as normal operation.
        oec::NetworkConfiguration config;
        std::string error;
        if (!oec::ConfigurationLoader::loadFromEniAndEsiDirectory(eniPath, esiDir, config, error)) {
            std::cerr << "Config load failed: " << error << '\n';
            return 1;
        }

        // Build selected transport and propagate process-image sizes for mock mode.
        oec::TransportFactoryConfig tc;
        tc.mockInputBytes = config.processImageInputBytes;
        tc.mockOutputBytes = config.processImageOutputBytes;
        if (!oec::TransportFactory::parseTransportSpec(transportSpec, tc, error)) {
            std::cerr << "Invalid transport spec: " << error << '\n';
            return 1;
        }
        auto transport = oec::TransportFactory::create(tc, error);
        if (!transport) {
            std::cerr << "Transport creation failed: " << error << '\n';
            return 1;
        }

        oec::EthercatMaster master(*transport);
        if (!master.configure(config)) {
            std::cerr << "Configure failed: " << master.lastError() << '\n';
            return 1;
        }
        if (!master.start()) {
            std::cerr << "Start failed: " << master.lastError() << '\n';
            return 1;
        }

        if (jsonMode) {
            std::cout << "{\"type\":\"start\",\"duration_s\":" << durationSeconds
                      << ",\"period_us\":" << periodUs
                      << ",\"transport\":\"" << transportSpec << "\"}\n";
        } else {
            std::cout << "Running DC soak demo for " << durationSeconds
                      << "s at " << periodUs << "us period. Press Ctrl-C to stop.\n";
        }

        // Fixed-rate loop state for latency/jitter KPI collection.
        const auto start = std::chrono::steady_clock::now();
        auto nextWake = start;
        auto prevWake = start;
        auto lastReport = start;

        std::uint64_t cycles = 0U;
        std::uint64_t failures = 0U;
        std::uint64_t lockedCycles = 0U;
        std::uint64_t lockTransitions = 0U;
        bool prevLocked = false;

        std::deque<std::uint64_t> runtimeUsWindow;
        std::deque<std::uint64_t> wakeJitterNsWindow;
        constexpr std::size_t kWindowLimit = 100000U;

        while (!gStopRequested.load()) {
            const auto now = std::chrono::steady_clock::now();
            if (now - start >= std::chrono::seconds(durationSeconds)) {
                break;
            }

            const auto wakeDelta = std::chrono::duration_cast<std::chrono::nanoseconds>(now - prevWake).count();
            const auto targetDelta = static_cast<std::int64_t>(periodUs) * 1000LL;
            const auto wakeErrNs = static_cast<std::uint64_t>(std::llabs(wakeDelta - targetDelta));
            prevWake = now;

            // Measure runCycle runtime as an application-visible determinism metric.
            const auto cycleBegin = std::chrono::steady_clock::now();
            if (!master.runCycle()) {
                ++failures;
            }
            const auto cycleEnd = std::chrono::steady_clock::now();
            ++cycles;

            const auto runUs = static_cast<std::uint64_t>(
                std::chrono::duration_cast<std::chrono::microseconds>(cycleEnd - cycleBegin).count());
            runtimeUsWindow.push_back(runUs);
            wakeJitterNsWindow.push_back(wakeErrNs);
            if (runtimeUsWindow.size() > kWindowLimit) {
                runtimeUsWindow.pop_front();
            }
            if (wakeJitterNsWindow.size() > kWindowLimit) {
                wakeJitterNsWindow.pop_front();
            }

            // Track DC lock duty and lock/unlock transitions over the soak window.
            const auto dc = master.distributedClockQuality();
            if (dc.locked) {
                ++lockedCycles;
            }
            if (cycles > 1U && dc.locked != prevLocked) {
                ++lockTransitions;
            }
            prevLocked = dc.locked;

            if (std::chrono::duration_cast<std::chrono::seconds>(now - lastReport).count() >= 1) {
                std::vector<std::uint64_t> runtimeValues(runtimeUsWindow.begin(), runtimeUsWindow.end());
                std::vector<std::uint64_t> jitterValues(wakeJitterNsWindow.begin(), wakeJitterNsWindow.end());
                const auto runP99 = percentile(runtimeValues, 99);
                const auto wakeP99 = percentile(jitterValues, 99);
                if (jsonMode) {
                    std::cout << "{\"type\":\"progress\",\"cycles\":" << cycles
                              << ",\"failures\":" << failures
                              << ",\"lock_duty\":" << (cycles == 0 ? 0.0 : static_cast<double>(lockedCycles) / static_cast<double>(cycles))
                              << ",\"runtime_p99_us\":" << runP99
                              << ",\"wake_jitter_p99_ns\":" << wakeP99
                              << ",\"dc_locked\":" << (dc.locked ? 1 : 0)
                              << ",\"dc_phase_err_ns\":" << dc.lastPhaseErrorNs
                              << ",\"dc_jitter_p99_ns\":" << dc.jitterP99Ns
                              << ",\"dc_policy_triggers\":" << dc.policyTriggers
                              << "}\n";
                } else {
                    std::cout << "cycles=" << cycles
                              << " failures=" << failures
                              << " lock_duty=" << (cycles == 0 ? 0.0 : static_cast<double>(lockedCycles) / static_cast<double>(cycles))
                              << " runtime_p99_us=" << runP99
                              << " wake_jitter_p99_ns=" << wakeP99
                              << " dc_locked=" << (dc.locked ? 1 : 0)
                              << " dc_phase_err_ns=" << dc.lastPhaseErrorNs
                              << " dc_jitter_p99_ns=" << dc.jitterP99Ns
                              << " dc_policy_triggers=" << dc.policyTriggers
                              << '\n';
                }
                lastReport = now;
            }

            // Sleep to absolute next deadline to avoid cumulative drift from relative sleeps.
            nextWake += std::chrono::microseconds(periodUs);
            std::this_thread::sleep_until(nextWake);
        }

        const auto dc = master.distributedClockQuality();
        const auto stats = master.distributedClockStats();
        std::vector<std::uint64_t> runtimeValues(runtimeUsWindow.begin(), runtimeUsWindow.end());
        std::vector<std::uint64_t> jitterValues(wakeJitterNsWindow.begin(), wakeJitterNsWindow.end());
        const auto runP50 = percentile(runtimeValues, 50);
        const auto runP95 = percentile(runtimeValues, 95);
        const auto runP99 = percentile(runtimeValues, 99);
        const auto runMax = runtimeValues.empty() ? 0U : *std::max_element(runtimeValues.begin(), runtimeValues.end());
        const auto wakeP95 = percentile(jitterValues, 95);
        const auto wakeP99 = percentile(jitterValues, 99);
        const auto wakeMax = jitterValues.empty() ? 0U : *std::max_element(jitterValues.begin(), jitterValues.end());

        if (jsonMode) {
            std::cout << "{\"type\":\"summary\",\"cycles\":" << cycles
                      << ",\"failures\":" << failures
                      << ",\"lock_duty\":" << (cycles == 0 ? 0.0 : static_cast<double>(lockedCycles) / static_cast<double>(cycles))
                      << ",\"lock_transitions\":" << lockTransitions
                      << ",\"runtime_p50_us\":" << runP50
                      << ",\"runtime_p95_us\":" << runP95
                      << ",\"runtime_p99_us\":" << runP99
                      << ",\"runtime_max_us\":" << runMax
                      << ",\"wake_jitter_p95_ns\":" << wakeP95
                      << ",\"wake_jitter_p99_ns\":" << wakeP99
                      << ",\"wake_jitter_max_ns\":" << wakeMax
                      << ",\"dc_samples\":" << dc.samples
                      << ",\"dc_locked\":" << (dc.locked ? 1 : 0)
                      << ",\"dc_lock_acq\":" << dc.lockAcquisitions
                      << ",\"dc_lock_loss\":" << dc.lockLosses
                      << ",\"dc_policy_triggers\":" << dc.policyTriggers
                      << ",\"dc_phase_err_ns\":" << dc.lastPhaseErrorNs
                      << ",\"dc_jitter_p95_ns\":" << dc.jitterP95Ns
                      << ",\"dc_jitter_p99_ns\":" << dc.jitterP99Ns
                      << ",\"dc_jitter_max_ns\":" << dc.jitterMaxNs
                      << ",\"dc_ctrl_jitter_rms_ns\":" << stats.jitterRmsNs
                      << "}\n";
        } else {
            std::cout << "summary"
                      << " cycles=" << cycles
                      << " failures=" << failures
                      << " lock_duty=" << (cycles == 0 ? 0.0 : static_cast<double>(lockedCycles) / static_cast<double>(cycles))
                      << " lock_transitions=" << lockTransitions
                      << " runtime_p50_us=" << runP50
                      << " runtime_p95_us=" << runP95
                      << " runtime_p99_us=" << runP99
                      << " runtime_max_us=" << runMax
                      << " wake_jitter_p95_ns=" << wakeP95
                      << " wake_jitter_p99_ns=" << wakeP99
                      << " wake_jitter_max_ns=" << wakeMax
                      << " dc_samples=" << dc.samples
                      << " dc_locked=" << (dc.locked ? 1 : 0)
                      << " dc_lock_acq=" << dc.lockAcquisitions
                      << " dc_lock_loss=" << dc.lockLosses
                      << " dc_policy_triggers=" << dc.policyTriggers
                      << " dc_phase_err_ns=" << dc.lastPhaseErrorNs
                      << " dc_jitter_p95_ns=" << dc.jitterP95Ns
                      << " dc_jitter_p99_ns=" << dc.jitterP99Ns
                      << " dc_jitter_max_ns=" << dc.jitterMaxNs
                      << " dc_ctrl_jitter_rms_ns=" << stats.jitterRmsNs
                      << '\n';
        }

        master.stop();
        return (failures == 0U) ? 0 : 2;
    } catch (const std::exception& ex) {
        std::cerr << ex.what() << '\n';
        return 1;
    }
}
