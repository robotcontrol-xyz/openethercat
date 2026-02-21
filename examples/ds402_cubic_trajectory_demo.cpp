/**
 * @file ds402_cubic_trajectory_demo.cpp
 * @brief openEtherCAT source file.
 */

#include <chrono>
#include <cmath>
#include <cstdint>
#include <iostream>
#include <vector>

#include "openethercat/master/ethercat_master.hpp"
#include "openethercat/transport/mock_transport.hpp"

namespace {

struct CubicSpline1D {
    double p0 = 0.0;
    double p1 = 0.0;
    double v0 = 0.0;
    double v1 = 0.0;
    double T = 1.0;

    double position(double t) const {
        const double clamped = std::max(0.0, std::min(t, T));
        const double a0 = p0;
        const double a1 = v0;
        const double a2 = (3.0 * (p1 - p0) - (2.0 * v0 + v1) * T) / (T * T);
        const double a3 = (2.0 * (p0 - p1) + (v0 + v1) * T) / (T * T * T);
        return a0 + a1 * clamped + a2 * clamped * clamped + a3 * clamped * clamped * clamped;
    }
};

std::vector<std::uint8_t> packDs402CspFrame(std::uint16_t controlWord,
                                            std::int32_t targetPosition,
                                            std::int8_t modeOfOperation) {
    std::vector<std::uint8_t> out(7, 0U);
    out[0] = static_cast<std::uint8_t>(controlWord & 0xFFU);
    out[1] = static_cast<std::uint8_t>((controlWord >> 8U) & 0xFFU);
    out[2] = static_cast<std::uint8_t>(targetPosition & 0xFF);
    out[3] = static_cast<std::uint8_t>((targetPosition >> 8) & 0xFF);
    out[4] = static_cast<std::uint8_t>((targetPosition >> 16) & 0xFF);
    out[5] = static_cast<std::uint8_t>((targetPosition >> 24) & 0xFF);
    out[6] = static_cast<std::uint8_t>(modeOfOperation);
    return out;
}

} // namespace

int main() {
    // Process image bytes [0..6] are mapped as:
    // 0..1: DS402 Controlword (0x6040)
    // 2..5: DS402 Target position (0x607A)
    // 6:    Mode of operation (0x6060)
    oec::NetworkConfiguration cfg;
    cfg.processImageInputBytes = 8;
    cfg.processImageOutputBytes = 8;
    cfg.slaves = {
        {.name = "EK1100", .alias = 0, .position = 0, .vendorId = 0x2, .productCode = 0x044c2c52},
        {.name = "DS402Drive", .alias = 0, .position = 1, .vendorId = 0x12345678, .productCode = 0x0000DC42},
    };
    cfg.signals = {
        {.logicalName = "DriveReadyBit", .direction = oec::SignalDirection::Input, .slaveName = "DS402Drive", .byteOffset = 0, .bitOffset = 0},
    };

    oec::MockTransport transport(cfg.processImageInputBytes, cfg.processImageOutputBytes);
    oec::EthercatMaster master(transport);

    if (!master.configure(cfg) || !master.start()) {
        std::cerr << "Startup failed: " << master.lastError() << '\n';
        return 1;
    }

    // DS402 startup sequence (simplified): shutdown -> switch on -> enable operation.
    const std::uint16_t cwShutdown = 0x0006;
    const std::uint16_t cwSwitchOn = 0x0007;
    const std::uint16_t cwEnableOp = 0x000F;
    const std::int8_t modeCsp = 8; // Cyclic Synchronous Position

    if (!master.writeOutputBytes(0, packDs402CspFrame(cwShutdown, 0, modeCsp)) || !master.runCycle()) {
        std::cerr << "Failed during shutdown step: " << master.lastError() << '\n';
        return 1;
    }
    if (!master.writeOutputBytes(0, packDs402CspFrame(cwSwitchOn, 0, modeCsp)) || !master.runCycle()) {
        std::cerr << "Failed during switch-on step: " << master.lastError() << '\n';
        return 1;
    }
    if (!master.writeOutputBytes(0, packDs402CspFrame(cwEnableOp, 0, modeCsp)) || !master.runCycle()) {
        std::cerr << "Failed during enable-op step: " << master.lastError() << '\n';
        return 1;
    }

    // Simple A->B cubic spline in position units (encoder counts for demo).
    const CubicSpline1D spline{
        .p0 = 0.0,
        .p1 = 120000.0,
        .v0 = 0.0,
        .v1 = 0.0,
        .T = 2.0,
    };

    constexpr double cycleSeconds = 0.001; // 1 ms cyclic task
    const int totalCycles = static_cast<int>(std::ceil(spline.T / cycleSeconds));

    std::cout << "Running DS402 CSP trajectory over " << totalCycles << " cycles\n";
    for (int i = 0; i <= totalCycles; ++i) {
        const double t = i * cycleSeconds;
        const auto setpoint = static_cast<std::int32_t>(std::llround(spline.position(t)));

        if (!master.writeOutputBytes(0, packDs402CspFrame(cwEnableOp, setpoint, modeCsp))) {
            std::cerr << "writeOutputBytes failed: " << master.lastError() << '\n';
            return 1;
        }
        if (!master.runCycle()) {
            std::cerr << "runCycle failed: " << master.lastError() << '\n';
            return 1;
        }

        if ((i % 250) == 0 || i == totalCycles) {
            std::cout << "cycle=" << i << " setpoint=" << setpoint << '\n';
        }
    }

    master.stop();
    std::cout << "Trajectory complete." << '\n';
    return 0;
}
