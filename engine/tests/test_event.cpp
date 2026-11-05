#include <gtest/gtest.h>

#include "pjt/event.hpp"

TEST(EventFormat, ReadsTheDocumentedExample) {
    const auto e = pjt::parse_event(
        R"({"event_id": "evt-000123", "payment_id": "pay-000045", "stage": "Validated",
            "timestamp_ms": 1793700000000, "amount_minor": 25000, "currency": "GBP"})");
    ASSERT_TRUE(e.has_value());
    EXPECT_EQ(e->event_id, "evt-000123");
    EXPECT_EQ(e->payment_id, "pay-000045");
    EXPECT_EQ(e->stage, pjt::Stage::Validated);
    EXPECT_EQ(e->timestamp_ms, 1793700000000);
    EXPECT_EQ(e->amount_minor, 25000);
    EXPECT_EQ(e->currency, "GBP");
}

TEST(EventFormat, WriteThenReadGivesTheSameEvent) {
    const pjt::Event e{"evt-1", "pay-1", pjt::Stage::Settled, 1793700000500, 999999999999, "EUR"};
    EXPECT_EQ(pjt::parse_event(pjt::to_json(e)), e);
}

TEST(EventFormat, RejectsBadInput) {
    const char* bad[] = {
        "not json",
        "[]",
        R"({"payment_id": "p", "stage": "Sent", "timestamp_ms": 1, "amount_minor": 1, "currency": "GBP"})",
        R"({"event_id": "e", "payment_id": "p", "stage": "Lost", "timestamp_ms": 1, "amount_minor": 1, "currency": "GBP"})",
        R"({"event_id": "e", "payment_id": "p", "stage": "Sent", "timestamp_ms": 1, "amount_minor": 12.5, "currency": "GBP"})",
        R"({"event_id": "e", "payment_id": "p", "stage": "Sent", "timestamp_ms": 1, "amount_minor": -5, "currency": "GBP"})",
        R"({"event_id": "e", "payment_id": "p", "stage": "Sent", "timestamp_ms": 1, "amount_minor": 5, "currency": "gbp"})",
        R"({"event_id": "", "payment_id": "p", "stage": "Sent", "timestamp_ms": 1, "amount_minor": 5, "currency": "GBP"})",
        R"({"event_id": "e", "payment_id": "p", "stage": "Sent", "timestamp_ms": "1", "amount_minor": 5, "currency": "GBP"})",
    };
    for (const char* text : bad) {
        std::string error;
        EXPECT_FALSE(pjt::parse_event(text, &error).has_value()) << text;
        EXPECT_FALSE(error.empty()) << text;
    }
}

// The same line Python's sim/events.py writes (see tests/test_events.py on the Python side).
TEST(EventFormat, ReadsWhatPythonWrites) {
    const auto e = pjt::parse_event(
        R"({"event_id":"evt-000001","payment_id":"pay-000001","stage":"Received","timestamp_ms":1793700000000,"amount_minor":125050,"currency":"USD"})");
    ASSERT_TRUE(e.has_value());
    EXPECT_EQ(e->amount_minor, 125050);
    EXPECT_EQ(e->stage, pjt::Stage::Received);
}
