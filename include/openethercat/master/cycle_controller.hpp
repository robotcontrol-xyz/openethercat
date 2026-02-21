/**
 * @file cycle_controller.hpp
 * @brief openEtherCAT source file.
 */

#pragma once

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <optional>
#include <thread>

namespace oec {

class EthercatMaster;

/**
 * @brief Configuration for cyclic execution worker.
 */
struct CycleControllerOptions {
    std::chrono::microseconds period = std::chrono::microseconds(1000);
    bool stopOnError = true;
    std::size_t maxConsecutiveFailures = 3;
    bool enablePhaseCorrection = false;
    std::function<std::optional<std::int64_t>()> phaseCorrectionNsProvider;
};

/**
 * @brief Runtime report for one cycle.
 */
struct CycleReport {
    std::uint64_t cycleIndex = 0;
    bool success = false;
    std::uint16_t workingCounter = 0;
    std::chrono::microseconds runtime = std::chrono::microseconds(0);
};

/**
 * @brief Dedicated cyclic thread runner for EthercatMaster::runCycle().
 */
class CycleController {
public:
    using CycleReportCallback = std::function<void(const CycleReport& report)>;

    CycleController() = default;
    ~CycleController();

    bool start(EthercatMaster& master,
               CycleControllerOptions options,
               CycleReportCallback callback = {});
    void stop();

    bool isRunning() const noexcept;

private:
    std::atomic<bool> running_{false};
    std::thread worker_;
};

} // namespace oec
