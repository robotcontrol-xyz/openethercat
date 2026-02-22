/**
 * @file dc_hardware_sync_demo.cpp
 * @brief openEtherCAT source file.
 */

#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <cmath>
#include <iomanip>
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

std::int64_t clampStep(std::int64_t rawCorrection,
                       std::int64_t previousApplied,
                       std::int64_t maxAbsoluteStep,
                       std::int64_t maxSlewPerCycle,
                       bool& outStepClamped,
                       bool& outSlewClamped) {
    outStepClamped = false;
    outSlewClamped = false;
    auto corrected = rawCorrection;
    if (maxAbsoluteStep > 0 && std::llabs(corrected) > maxAbsoluteStep) {
        corrected = (corrected < 0) ? -maxAbsoluteStep : maxAbsoluteStep;
        outStepClamped = true;
    }
    if (maxSlewPerCycle > 0) {
        const auto delta = corrected - previousApplied;
        if (std::llabs(delta) > maxSlewPerCycle) {
            corrected = previousApplied + ((delta < 0) ? -maxSlewPerCycle : maxSlewPerCycle);
            outSlewClamped = true;
        }
    }
    return corrected;
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
        const bool jsonMode = (std::getenv("OEC_DC_SOAK_JSON") != nullptr);
        const std::int64_t maxCorrectionStepNs =
            static_cast<std::int64_t>((std::getenv("OEC_DC_MAX_CORR_STEP_NS") != nullptr)
                                          ? parseUnsigned(std::getenv("OEC_DC_MAX_CORR_STEP_NS"), "OEC_DC_MAX_CORR_STEP_NS")
                                          : 20000U);
        const std::int64_t maxSlewPerCycleNs =
            static_cast<std::int64_t>((std::getenv("OEC_DC_MAX_SLEW_NS") != nullptr)
                                          ? parseUnsigned(std::getenv("OEC_DC_MAX_SLEW_NS"), "OEC_DC_MAX_SLEW_NS")
                                          : 5000U);

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
        linux->resetDcDiagnostics();

        oec::DistributedClockController dc({
            .filterAlpha = 0.2,
            .kp = 0.1,
            .ki = 0.01,
            .correctionClampNs = 20000
        });

        if (jsonMode) {
            std::cout << "{\"type\":\"start\",\"slave\":" << slavePosition
                      << ",\"samples\":" << samples
                      << ",\"period_ms\":" << periodMs
                      << ",\"max_corr_step_ns\":" << maxCorrectionStepNs
                      << ",\"max_slew_ns\":" << maxSlewPerCycleNs
                      << "}\n";
        } else {
            std::cout << "Running DC hardware sync prototype, slave=" << slavePosition
                      << " samples=" << samples << " period_ms=" << periodMs
                      << " max_corr_step_ns=" << maxCorrectionStepNs
                      << " max_slew_ns=" << maxSlewPerCycleNs
                      << '\n';
        }

        std::uint64_t controllerClampHits = 0U;
        std::uint64_t stepClampHits = 0U;
        std::uint64_t slewClampHits = 0U;
        std::int64_t lastAppliedCorrectionNs = 0;
        constexpr std::int64_t kControllerClampNs = 20000;

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
                if (std::llabs(*corr) >= kControllerClampNs) {
                    ++controllerClampHits;
                }
                bool stepClamped = false;
                bool slewClamped = false;
                const auto safeCorr = clampStep(*corr, lastAppliedCorrectionNs,
                                                maxCorrectionStepNs, maxSlewPerCycleNs,
                                                stepClamped, slewClamped);
                if (stepClamped) {
                    ++stepClampHits;
                }
                if (slewClamped) {
                    ++slewClampHits;
                }
                if (!linux->writeDcSystemTimeOffset(slavePosition, safeCorr, error)) {
                    std::cerr << "writeDcSystemTimeOffset failed at sample " << i << ": " << error << '\n';
                    linux->close();
                    return 3;
                }
                lastAppliedCorrectionNs = safeCorr;
            }

            if ((i + 1U) % 20U == 0U || (i + 1U) == samples) {
                const auto s = dc.stats();
                if (jsonMode) {
                    std::cout << "{\"type\":\"progress\",\"sample\":" << (i + 1U)
                              << ",\"offset_ns\":" << s.lastOffsetNs
                              << ",\"filtered_ns\":" << s.filteredOffsetNs
                              << ",\"corr_ns\":" << s.correctionNs
                              << ",\"applied_corr_ns\":" << lastAppliedCorrectionNs
                              << ",\"jitter_rms_ns\":" << std::fixed << std::setprecision(2) << s.jitterRmsNs
                              << ",\"max_abs_offset_ns\":" << s.maxAbsOffsetNs
                              << ",\"controller_clamp_hits\":" << controllerClampHits
                              << ",\"step_clamp_hits\":" << stepClampHits
                              << ",\"slew_clamp_hits\":" << slewClampHits
                              << "}\n";
                } else {
                    std::cout << "sample=" << (i + 1U)
                              << " offset_ns=" << s.lastOffsetNs
                              << " filtered_ns=" << s.filteredOffsetNs
                              << " corr_ns=" << s.correctionNs
                              << " applied_corr_ns=" << lastAppliedCorrectionNs
                              << " jitter_rms_ns=" << s.jitterRmsNs
                              << " max_abs_offset_ns=" << s.maxAbsOffsetNs
                              << " controller_clamp_hits=" << controllerClampHits
                              << " step_clamp_hits=" << stepClampHits
                              << " slew_clamp_hits=" << slewClampHits
                              << '\n';
                }
            }

            std::this_thread::sleep_for(std::chrono::milliseconds(periodMs));
        }

        const auto d = linux->dcDiagnostics();
        if (jsonMode) {
            std::cout << "{\"type\":\"dc_diag\",\"schema_version\":" << d.schemaVersion
                      << ",\"read_attempts\":" << d.readAttempts
                      << ",\"read_success\":" << d.readSuccess
                      << ",\"read_failure\":" << d.readFailure
                      << ",\"read_invalid_payload\":" << d.readInvalidPayload
                      << ",\"write_attempts\":" << d.writeAttempts
                      << ",\"write_success\":" << d.writeSuccess
                      << ",\"write_failure\":" << d.writeFailure
                      << ",\"controller_clamp_hits\":" << controllerClampHits
                      << ",\"step_clamp_hits\":" << stepClampHits
                      << ",\"slew_clamp_hits\":" << slewClampHits
                      << "}\n";
        } else {
            std::cout << "dc_diag"
                      << " schema_version=" << d.schemaVersion
                      << " read_attempts=" << d.readAttempts
                      << " read_success=" << d.readSuccess
                      << " read_failure=" << d.readFailure
                      << " read_invalid_payload=" << d.readInvalidPayload
                      << " write_attempts=" << d.writeAttempts
                      << " write_success=" << d.writeSuccess
                      << " write_failure=" << d.writeFailure
                      << " controller_clamp_hits=" << controllerClampHits
                      << " step_clamp_hits=" << stepClampHits
                      << " slew_clamp_hits=" << slewClampHits
                      << '\n';
        }

        linux->close();
        return 0;
    } catch (const std::exception& ex) {
        std::cerr << ex.what() << '\n';
        return 1;
    }
}
