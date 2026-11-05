// Step 12: spotting a gradual slowdown before the fixed time limit catches it.
#include <gtest/gtest.h>

#include <algorithm>
#include <random>

#include "helpers.hpp"
#include "pjt/slowdown.hpp"

using namespace pjt;
using namespace pjt::testing;

TEST(Slowdown, SteadyTimesNeverWarn) {
    SlowdownDetector d({true, 1.5, 50, 0.05, 0.001});
    std::mt19937 rng(1);
    std::uniform_int_distribution<int> ms(100, 1200);
    for (int i = 0; i < 5000; ++i) EXPECT_EQ(d.record(Stage::Screened, ms(rng)), SlowdownDetector::Change::None);
}

TEST(Slowdown, GradualRiseWarnsThenClears) {
    SlowdownDetector d({true, 1.5, 50, 0.05, 0.001});
    std::mt19937 rng(2);
    std::uniform_int_distribution<int> ms(100, 1200);
    for (int i = 0; i < 1000; ++i) d.record(Stage::Screened, ms(rng));
    int warned_at = -1;
    for (int i = 0; i < 1000 && warned_at < 0; ++i) {  // add 4 ms more every sample
        if (d.record(Stage::Screened, ms(rng) + 4 * i) == SlowdownDetector::Change::Started) warned_at = i;
    }
    ASSERT_GE(warned_at, 0);
    EXPECT_LT(650 + 4 * warned_at, 4000) << "should warn while a typical payment is still under a 4 s limit";
    EXPECT_TRUE(d.stats(Stage::Screened).warning);
    bool cleared = false;
    for (int i = 0; i < 500 && !cleared; ++i) cleared = d.record(Stage::Screened, ms(rng)) == SlowdownDetector::Change::Cleared;
    EXPECT_TRUE(cleared);
}

TEST(Slowdown, NeedsWarmupFirst) {
    SlowdownDetector d({true, 2.0, 100, 0.05, 0.002});
    for (int i = 0; i < 99; ++i) EXPECT_EQ(d.record(Stage::Sent, i < 50 ? 100 : 100000), SlowdownDetector::Change::None);
}

TEST(Slowdown, EngineRaisesStageAlertBeforeAnyStuckAlert) {
    auto c = test_config();
    c.slowdown = {true, 1.5, 100, 0.05, 0.001};
    CollectSink sink;
    Tracker t(c, sink);
    // 1,500 payments; from payment 500 on, Screened slowly gets slower (+6 ms per payment).
    std::vector<Event> stream;
    for (int i = 0; i < 1500; ++i) {
        const std::string p = "p" + std::to_string(i);
        const std::int64_t start = i * 200LL;
        const std::int64_t screened_ms = 500 + (i > 500 ? 6LL * (i - 500) : 0);
        stream.push_back(ev(p + "a", p, Stage::Received, start));
        stream.push_back(ev(p + "b", p, Stage::Validated, start + 300));
        stream.push_back(ev(p + "c", p, Stage::Screened, start + 600));
        stream.push_back(ev(p + "d", p, Stage::Sent, start + 600 + screened_ms));
        stream.push_back(ev(p + "e", p, Stage::Settled, start + 900 + screened_ms));
    }
    // Events arrive in time order, as they would from a real queue.
    std::stable_sort(stream.begin(), stream.end(),
                     [](const Event& a, const Event& b) { return a.timestamp_ms < b.timestamp_ms; });
    for (const auto& e : stream) t.process(e);
    t.drain();
    std::int64_t first_slowdown = -1, first_stuck = -1;
    for (const auto& a : sink.alerts) {
        if (a.type == AlertType::Slowdown && first_slowdown < 0) first_slowdown = a.raised_at_ms;
        if (a.type == AlertType::Stuck && first_stuck < 0) first_stuck = a.raised_at_ms;
    }
    ASSERT_GE(first_slowdown, 0);
    ASSERT_GE(first_stuck, 0);  // eventually it gets slow enough to hit the 4 s limit
    EXPECT_LT(first_slowdown, first_stuck);
    EXPECT_TRUE(sink.has_alert("stage:Screened:slowdown:1"));
}
