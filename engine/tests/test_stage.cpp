#include <gtest/gtest.h>

#include "pjt/stage.hpp"

using pjt::Stage;

namespace {

const Stage kAll[] = {Stage::Received, Stage::Validated, Stage::Screened, Stage::Sent, Stage::Settled, Stage::Failed};

// The full table of allowed moves. Every pair not listed here must be blocked.
bool expected(Stage from, Stage to) {
    switch (from) {
        case Stage::Received: return to == Stage::Validated || to == Stage::Failed;
        case Stage::Validated: return to == Stage::Screened || to == Stage::Failed;
        case Stage::Screened: return to == Stage::Sent || to == Stage::Failed;
        case Stage::Sent: return to == Stage::Settled || to == Stage::Failed;
        case Stage::Settled:
        case Stage::Failed: return false;
    }
    return false;
}

}  // namespace

TEST(StateMachine, EveryAllowedAndBlockedMove) {
    int allowed = 0;
    for (Stage from : kAll) {
        for (Stage to : kAll) {
            EXPECT_EQ(pjt::can_move(from, to), expected(from, to))
                << pjt::to_string(from) << " -> " << pjt::to_string(to);
            allowed += pjt::can_move(from, to);
        }
    }
    EXPECT_EQ(allowed, 8);  // 4 steps forward + 4 ways to fail
}

TEST(StateMachine, FinalStages) {
    EXPECT_TRUE(pjt::is_final(Stage::Settled));
    EXPECT_TRUE(pjt::is_final(Stage::Failed));
    EXPECT_FALSE(pjt::is_final(Stage::Sent));
}

TEST(StateMachine, NamesRoundTrip) {
    for (Stage s : kAll) EXPECT_EQ(pjt::parse_stage(pjt::to_string(s)), s);
    EXPECT_FALSE(pjt::parse_stage("received").has_value());  // names are case-sensitive
    EXPECT_FALSE(pjt::parse_stage("").has_value());
}
