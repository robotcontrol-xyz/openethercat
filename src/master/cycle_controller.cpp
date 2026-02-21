#include "openethercat/master/cycle_controller.hpp"

#include <chrono>

#include "openethercat/master/ethercat_master.hpp"

namespace oec {

CycleController::~CycleController() { stop(); }

bool CycleController::start(EthercatMaster& master,
                            CycleControllerOptions options,
                            CycleReportCallback callback) {
    if (running_.exchange(true)) {
        return false;
    }

    worker_ = std::thread([this, &master, options, callback = std::move(callback)]() mutable {
        std::uint64_t cycleIndex = 0;
        std::size_t consecutiveFailures = 0;
        auto nextWake = std::chrono::steady_clock::now();

        while (running_.load()) {
            const auto start = std::chrono::steady_clock::now();
            const bool ok = master.runCycle();
            const auto end = std::chrono::steady_clock::now();

            if (!ok) {
                ++consecutiveFailures;
            } else {
                consecutiveFailures = 0;
            }

            CycleReport report;
            report.cycleIndex = cycleIndex++;
            report.success = ok;
            report.workingCounter = master.lastWorkingCounter();
            report.runtime = std::chrono::duration_cast<std::chrono::microseconds>(end - start);
            if (callback) {
                callback(report);
            }

            if (!ok && options.stopOnError && consecutiveFailures >= options.maxConsecutiveFailures) {
                running_.store(false);
                break;
            }

            nextWake += options.period;
            if (options.enablePhaseCorrection && options.phaseCorrectionNsProvider) {
                const auto correction = options.phaseCorrectionNsProvider();
                if (correction.has_value()) {
                    nextWake += std::chrono::nanoseconds(*correction);
                }
            }
            std::this_thread::sleep_until(nextWake);
        }
    });

    return true;
}

void CycleController::stop() {
    running_.store(false);
    if (worker_.joinable()) {
        worker_.join();
    }
}

bool CycleController::isRunning() const noexcept { return running_.load(); }

} // namespace oec
