// Step 6: the tracking engine follows a payment through its stages.
#include <gtest/gtest.h>

#include "helpers.hpp"

using namespace pjt;
using namespace pjt::testing;

TEST(Tracker, HealthyPaymentReachesSettled) {
    CollectSink sink;
    Tracker t(test_config(), sink);
    for (const auto& e : healthy("pay-1", 1000)) t.process(e);

    const auto& s = sink.states.at("pay-1");
    EXPECT_EQ(s.stage, Stage::Settled);
    EXPECT_TRUE(s.finished);
    EXPECT_FALSE(s.invalid);
    EXPECT_EQ(s.events_applied, 5);
    EXPECT_TRUE(sink.alerts.empty());
    for (const auto& [id, status] : sink.events) EXPECT_EQ(status, EventStatus::Applied) << id;
}

TEST(Tracker, ClockFollowsNewestTimestamp) {
    CollectSink sink;
    Tracker t(test_config(), sink);
    t.process(ev("e1", "p", Stage::Received, 5000));
    t.process(ev("e2", "q", Stage::Received, 3000));  // an older timestamp doesn't move the clock back
    EXPECT_EQ(t.now(), 5000);
}

TEST(Tracker, FailedPaymentRaisesAlert) {
    CollectSink sink;
    Tracker t(test_config(), sink);
    t.process(ev("e1", "p", Stage::Received, 0));
    t.process(ev("e2", "p", Stage::Validated, 100));
    t.process(ev("e3", "p", Stage::Failed, 200));
    EXPECT_EQ(sink.states.at("p").stage, Stage::Failed);
    EXPECT_TRUE(sink.states.at("p").finished);
    ASSERT_EQ(sink.count(AlertType::Failed), 1);
    EXPECT_EQ(sink.alerts[0].alert_id, "p:failed");
    EXPECT_EQ(sink.alerts[0].history.size(), 3u);  // the alert carries the payment's journey
}

TEST(Tracker, AmountChangeRaisesOneAlert) {
    CollectSink sink;
    Tracker t(test_config(), sink);
    t.process(ev("e1", "p", Stage::Received, 0, 25000));
    t.process(ev("e2", "p", Stage::Validated, 100, 25000));
    t.process(ev("e3", "p", Stage::Screened, 200, 26000));
    t.process(ev("e4", "p", Stage::Sent, 300, 27000));  // still wrong, but the problem is already open
    EXPECT_EQ(sink.count(AlertType::AmountMismatch), 1);
    EXPECT_EQ(sink.states.at("p").amount_minor, 27000);
}

TEST(Tracker, NothingMovesOutOfAFinalStage) {
    CollectSink sink;
    Tracker t(test_config(), sink);
    for (const auto& e : healthy("p", 0)) t.process(e);
    t.process(ev("late", "p", Stage::Failed, 5000));
    EXPECT_EQ(sink.states.at("p").stage, Stage::Settled);
    EXPECT_EQ(sink.events.back().second, EventStatus::Stale);
}

TEST(Tracker, FinishedPaymentsLeaveMemoryAfterRetention) {
    CollectSink sink;
    auto c = test_config();
    c.retention_ms = 1000;
    Tracker t(c, sink);
    for (const auto& e : healthy("p", 0)) t.process(e);
    EXPECT_EQ(t.payments().size(), 1u);
    t.advance_to(10000);
    EXPECT_EQ(t.payments().size(), 0u);
}

TEST(Tracker, ManyPaymentsAtOnce) {
    CollectSink sink;
    Tracker t(test_config(), sink);
    std::vector<std::vector<Event>> all;
    for (int i = 0; i < 50; ++i) all.push_back(healthy("p" + std::to_string(i), i * 10));
    for (int step = 0; step < 5; ++step) {  // interleave: everyone's step 0, then step 1...
        for (auto& p : all) t.process(p[static_cast<std::size_t>(step)]);
    }
    for (int i = 0; i < 50; ++i) EXPECT_EQ(sink.states.at("p" + std::to_string(i)).stage, Stage::Settled);
    EXPECT_TRUE(sink.alerts.empty());
}
