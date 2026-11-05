#include "pjt/slowdown.hpp"

#include <algorithm>

namespace pjt {

SlowdownDetector::Change SlowdownDetector::record(Stage stage, std::int64_t duration_ms) {
    if (!config_.enabled) return Change::None;
    auto& s = mutable_stats(stage);
    const double x = static_cast<double>(duration_ms < 0 ? 0 : duration_ms);
    // Early on, a plain average (1/n) is a better estimate than a slow-moving one,
    // so each average uses whichever weight is larger.
    const double n = static_cast<double>(s.samples + 1);
    s.recent_ms += std::max(config_.fast_alpha, 1.0 / n) * (x - s.recent_ms);
    if (!s.warning) s.normal_ms += std::max(config_.slow_alpha, 1.0 / n) * (x - s.normal_ms);
    ++s.samples;
    if (s.samples < config_.warmup || s.normal_ms <= 0) return Change::None;

    const double clear_at = 1.0 + (config_.ratio - 1.0) / 2.0;  // halfway back to normal
    if (!s.warning && s.recent_ms > config_.ratio * s.normal_ms) {
        s.warning = true;
        ++s.episodes;
        return Change::Started;
    }
    if (s.warning && s.recent_ms < clear_at * s.normal_ms) {
        s.warning = false;
        return Change::Cleared;
    }
    return Change::None;
}

}  // namespace pjt
