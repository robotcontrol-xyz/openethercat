#include "openethercat/master/distributed_clock.hpp"

#include <algorithm>
#include <cmath>

namespace oec {

DistributedClockController::DistributedClockController() = default;

DistributedClockController::DistributedClockController(Options options) : options_(options) {}

std::optional<std::int64_t> DistributedClockController::update(const DcSyncSample& sample) {
    const auto offset = sample.referenceTimeNs - sample.localTimeNs;
    stats_.lastOffsetNs = offset;

    const double filtered =
        (stats_.samples == 0U)
            ? static_cast<double>(offset)
            : ((1.0 - options_.filterAlpha) * static_cast<double>(stats_.filteredOffsetNs) +
               options_.filterAlpha * static_cast<double>(offset));
    stats_.filteredOffsetNs = static_cast<std::int64_t>(filtered);

    integral_ += filtered;
    double correction = options_.kp * filtered + options_.ki * integral_;
    correction = std::clamp(correction,
                            -static_cast<double>(options_.correctionClampNs),
                            static_cast<double>(options_.correctionClampNs));

    stats_.correctionNs = static_cast<std::int64_t>(correction);
    const auto absOffset = static_cast<std::int64_t>(std::llabs(offset));
    stats_.maxAbsOffsetNs = std::max(stats_.maxAbsOffsetNs, absOffset);

    sumSquares_ += static_cast<double>(offset) * static_cast<double>(offset);
    ++stats_.samples;
    stats_.jitterRmsNs = std::sqrt(sumSquares_ / static_cast<double>(stats_.samples));

    return stats_.correctionNs;
}

DcSyncStats DistributedClockController::stats() const noexcept { return stats_; }

void DistributedClockController::reset() {
    stats_ = DcSyncStats{};
    integral_ = 0.0;
    sumSquares_ = 0.0;
}

} // namespace oec
