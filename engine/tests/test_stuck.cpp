// Step 8: noticing that something did NOT happen.
#include <gtest/gtest.h>

#include "helpers.hpp"

using namespace pjt;
using namespace pjt::testing;

TEST(Stuck, PaymentThatStopsMovingRaisesAlertAtItsDeadline) {
    CollectSink sink;
    Tracker t(test_config(), sink);  // 4000 ms per stage
    t.process(ev("e1", "p", Stage::Received, 1000));
    t.process(ev("e2", "p", Stage::Validated, 1500));
    t.advance_to(5499);
    EXPECT_EQ(sink.count(AlertType::Stuck), 0);
    t.advance_to(5500);
    ASSERT_EQ(sink.count(AlertType::Stuck), 1);
    EXPECT_EQ(sink.alerts[0].alert_id, "p:stuck:Validated");
    EXPECT_EQ(sink.alerts[0].raised_at_ms, 5500);
    EXPECT_EQ(sink.alerts[0].stage, Stage::Validated);
}

TEST(Stuck, OtherPaymentsMoveTheClock) {
    // A stuck payment sends nothing, so other payments' events are what reveal it.
    CollectSink sink;
    Tracker t(test_config(), sink);
    t.process(ev("s1", "stuck", Stage::Received, 0));
    for (int i = 0; i < 10; ++i) {
        for (const auto& e : healthy("ok" + std::to_string(i), i * 1000, 100)) t.process(e);
    }
    EXPECT_EQ(sink.count(AlertType::Stuck), 1);
    EXPECT_TRUE(sink.has_alert("stuck:stuck:Received"));
}

TEST(Stuck, SlowButWithinLimitIsNotStuck) {
    CollectSink sink;
    Tracker t(test_config(), sink);
    for (const auto& e : healthy("slow", 0, 3900)) t.process(e);
    t.drain();
    EXPECT_EQ(sink.count(AlertType::Stuck), 0);
}

TEST(Stuck, OneAlertPerOpenProblemAndClearedWhenItMoves) {
    CollectSink sink;
    Tracker t(test_config(), sink);
    t.process(ev("e1", "p", Stage::Received, 0));
    t.advance_to(20000);
    t.advance_to(40000);  // still stuck: no second alert
    EXPECT_EQ(sink.count(AlertType::Stuck), 1);
    t.process(ev("e2", "p", Stage::Validated, 41000));
    ASSERT_EQ(sink.cleared.size(), 1u);
    EXPECT_EQ(sink.cleared[0], "p:stuck:Received");
    t.advance_to(50000);  // stuck again, at the next stage: a new problem
    EXPECT_EQ(sink.count(AlertType::Stuck), 2);
    EXPECT_TRUE(sink.has_alert("p:stuck:Validated"));
}

TEST(Stuck, FinishedPaymentsAreNeverStuck) {
    CollectSink sink;
    Tracker t(test_config(), sink);
    for (const auto& e : healthy("p", 0)) t.process(e);
    t.advance_to(1000000);
    EXPECT_EQ(sink.count(AlertType::Stuck), 0);
}

TEST(Stuck, EachStageHasItsOwnLimit) {
    CollectSink sink;
    auto c = test_config();
    c.stuck_after_ms = {1000, 1000, 10000, 1000, 0, 0};  // screening may take longer
    Tracker t(c, sink);
    t.process(ev("a1", "a", Stage::Received, 0));
    t.process(ev("a2", "a", Stage::Validated, 100));
    t.process(ev("a3", "a", Stage::Screened, 200));
    t.advance_to(5000);
    EXPECT_EQ(sink.count(AlertType::Stuck), 0);
    t.advance_to(10200);
    EXPECT_EQ(sink.count(AlertType::Stuck), 1);
}
