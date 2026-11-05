// Step 7: duplicates and out-of-order events. Repeats change nothing (idempotency),
// and early events wait briefly for the missing one.
#include <gtest/gtest.h>

#include <algorithm>
#include <random>

#include "helpers.hpp"

using namespace pjt;
using namespace pjt::testing;

TEST(Ordering, DuplicateIsCountedOnce) {
    CollectSink sink;
    Tracker t(test_config(), sink);
    auto events = healthy("p", 0);
    t.process(events[0]);
    t.process(events[1]);
    t.process(events[1]);  // the same event again
    t.process(events[1]);
    for (std::size_t i = 2; i < events.size(); ++i) t.process(events[i]);

    EXPECT_EQ(sink.states.at("p").events_applied, 5);
    EXPECT_EQ(t.stats().duplicates, 2);
    EXPECT_EQ(sink.count(AlertType::Duplicate), 1);  // one notice per payment, not per copy
    EXPECT_EQ(sink.states.at("p").stage, Stage::Settled);
}

TEST(Ordering, LateEventIsHeldThenAppliedInOrder) {
    CollectSink sink;
    Tracker t(test_config(), sink);
    auto e = healthy("p", 0, 100);
    t.process(e[0]);
    t.process(e[2]);  // Screened arrives before Validated
    EXPECT_EQ(sink.events.back().second, EventStatus::Held);
    EXPECT_EQ(t.payments().at("p").stage, Stage::Received);
    t.process(e[1]);  // Validated arrives: both now apply, in order
    EXPECT_EQ(t.payments().at("p").stage, Stage::Screened);
    t.process(e[3]);
    t.process(e[4]);
    EXPECT_EQ(sink.states.at("p").stage, Stage::Settled);
    EXPECT_FALSE(sink.states.at("p").invalid);
    EXPECT_EQ(sink.count(AlertType::OutOfOrder), 1);
    EXPECT_EQ(sink.count(AlertType::InvalidJourney), 0);
    // The journey is stored in the right order.
    const auto& h = t.payments().at("p").history;
    for (std::size_t i = 0; i < h.size(); ++i) EXPECT_EQ(h[i].stage, static_cast<Stage>(i));
}

TEST(Ordering, FirstEventMissingIsHeldToo) {
    CollectSink sink;
    Tracker t(test_config(), sink);
    auto e = healthy("p", 0, 100);
    t.process(e[1]);  // Validated before Received
    t.process(e[0]);
    for (std::size_t i = 2; i < e.size(); ++i) t.process(e[i]);
    EXPECT_EQ(sink.states.at("p").stage, Stage::Settled);
    EXPECT_EQ(sink.count(AlertType::InvalidJourney), 0);
}

TEST(Ordering, MissingEventThatNeverComesIsAnInvalidJourney) {
    CollectSink sink;
    Tracker t(test_config(), sink);  // reorder wait: 1000 ms
    auto e = healthy("p", 0, 100);
    t.process(e[0]);
    t.process(e[2]);  // Validated is skipped completely
    t.process(e[3]);
    t.process(e[4]);
    EXPECT_EQ(sink.count(AlertType::InvalidJourney), 0);  // still waiting
    t.advance_to(1500);
    ASSERT_EQ(sink.count(AlertType::InvalidJourney), 1);
    EXPECT_EQ(sink.alerts.back().stage, Stage::Validated);  // names the missing stage
    EXPECT_EQ(sink.alerts.back().raised_at_ms, 1200);       // held at 200 (the clock then), + 1000 wait
    // It then carries on with the events it has.
    EXPECT_EQ(sink.states.at("p").stage, Stage::Settled);
    EXPECT_TRUE(sink.states.at("p").invalid);
}

TEST(Ordering, OverlongWaitStillCountsAsSkipped) {
    CollectSink sink;
    Tracker t(test_config(), sink);
    auto e = healthy("p", 0, 100);
    t.process(e[0]);
    t.process(e[2]);
    t.advance_to(5000);
    t.process(e[1]);  // far too late: the payment has already moved past it
    EXPECT_EQ(sink.events.back().second, EventStatus::Stale);
    EXPECT_EQ(sink.count(AlertType::InvalidJourney), 1);
}

// The core promise: whatever order the events arrive in, and however many copies,
// the final states and the alerts are the same.
TEST(Ordering, ShuffledAndDuplicatedStreamsGiveTheSameResult) {
    std::vector<Event> stream;
    for (int i = 0; i < 300; ++i) {
        auto p = healthy("pay-" + std::to_string(i), i * 40, 150);
        if (i % 7 == 0) p.back().stage = Stage::Failed;              // some fail at the end
        if (i % 11 == 0) p[3].amount_minor += 100;                    // some change amount
        if (i % 13 == 0) p.erase(p.begin() + 2);                      // some skip Screened
        stream.insert(stream.end(), p.begin(), p.end());
    }
    std::stable_sort(stream.begin(), stream.end(),
                     [](const Event& a, const Event& b) { return a.timestamp_ms < b.timestamp_ms; });

    CollectSink clean_sink;
    {
        Tracker t(test_config(), clean_sink);
        for (const auto& e : stream) t.process(e);
        t.drain();
    }
    const auto clean = outcome_of(clean_sink);

    std::mt19937 rng(42);
    for (int trial = 0; trial < 20; ++trial) {
        // Shuffle inside small windows (like a real network), then add duplicates.
        auto messy = stream;
        for (std::size_t start = 0; start < messy.size(); start += 12) {
            const auto end = std::min(messy.size(), start + 12);
            std::shuffle(messy.begin() + static_cast<std::ptrdiff_t>(start),
                         messy.begin() + static_cast<std::ptrdiff_t>(end), rng);
        }
        std::vector<Event> with_dupes;
        std::uniform_int_distribution<int> pct(0, 99);
        for (const auto& e : messy) {
            with_dupes.push_back(e);
            if (pct(rng) < 10) with_dupes.push_back(e);
        }

        CollectSink sink;
        Tracker t(test_config(), sink);
        for (const auto& e : with_dupes) t.process(e);
        t.drain();
        const auto got = outcome_of(sink);
        EXPECT_EQ(std::get<0>(got), std::get<0>(clean)) << "trial " << trial;
        // Compare the real problems; duplicate/out-of-order notices depend on the arrival order.
        auto real = [](const std::vector<std::string>& ids) {
            std::vector<std::string> out;
            for (const auto& id : ids) {
                if (id.find(":duplicate") == std::string::npos && id.find(":out_of_order") == std::string::npos) out.push_back(id);
            }
            return out;
        };
        EXPECT_EQ(real(std::get<1>(got)), real(std::get<1>(clean))) << "trial " << trial;
    }
}

TEST(Ordering, DedupeMemoryIsBounded) {
    CollectSink sink;
    auto c = test_config();
    c.dedupe_capacity = 3;
    Tracker t(c, sink);
    t.process(ev("a", "p1", Stage::Received, 0));
    t.process(ev("b", "p2", Stage::Received, 0));
    t.process(ev("c", "p3", Stage::Received, 0));
    t.process(ev("d", "p4", Stage::Received, 0));  // "a" is forgotten now
    t.process(ev("d", "p4", Stage::Received, 0));
    EXPECT_EQ(t.stats().duplicates, 1);
}
