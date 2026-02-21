#pragma once

#include <cstdint>
#include <optional>

namespace oec {

/**
 * @brief One distributed-clock timing sample.
 */
struct DcSyncSample {
    std::int64_t referenceTimeNs = 0;
    std::int64_t localTimeNs = 0;
};

/**
 * @brief Aggregated distributed-clock control statistics.
 */
struct DcSyncStats {
    std::int64_t lastOffsetNs = 0;
    std::int64_t filteredOffsetNs = 0;
    std::int64_t correctionNs = 0;
    std::int64_t maxAbsOffsetNs = 0;
    double jitterRmsNs = 0.0;
    std::uint64_t samples = 0;
};

/**
 * @brief PI-based distributed-clock correction controller.
 */
class DistributedClockController {
public:
    struct Options {
        double filterAlpha = 0.2;
        double kp = 0.1;
        double ki = 0.01;
        std::int64_t correctionClampNs = 50000;
    };

    DistributedClockController();
    explicit DistributedClockController(Options options);

    std::optional<std::int64_t> update(const DcSyncSample& sample);
    DcSyncStats stats() const noexcept;
    void reset();

private:
    Options options_;
    DcSyncStats stats_{};
    double integral_ = 0.0;
    double sumSquares_ = 0.0;
};

} // namespace oec
