// Spots a stage that is getting slower over time, before the fixed time limit is hit.
#pragma once

#include <array>
#include <cstdint>

#include "pjt/config.hpp"
#include "pjt/stage.hpp"

namespace pjt {

// For each stage, keeps two running averages of how long payments spend there:
// a "recent" one that reacts quickly and a "normal" one that moves slowly.
// When recent > ratio x normal, the stage is flagged. The normal average is frozen while
// flagged, so a long slowdown can't quietly become the new normal.
class SlowdownDetector {
public:
    enum class Change { None, Started, Cleared };

    struct StageStats {
        double recent_ms = 0;
        double normal_ms = 0;
        std::int64_t samples = 0;
        bool warning = false;
        int episodes = 0;  // how many times this stage has been flagged
    };

    explicit SlowdownDetector(SlowdownConfig config = {}) : config_(config) {}

    // Records that a payment spent `duration_ms` in `stage`.
    Change record(Stage stage, std::int64_t duration_ms);

    const StageStats& stats(Stage s) const { return stats_[static_cast<std::size_t>(index_of(s))]; }
    StageStats& mutable_stats(Stage s) { return stats_[static_cast<std::size_t>(index_of(s))]; }

private:
    SlowdownConfig config_;
    std::array<StageStats, kStageCount> stats_{};
};

}  // namespace pjt
