#include <gtest/gtest.h>

#include <stdexcept>

#include "pjt/config.hpp"

TEST(Config, ReadsAllSettings) {
    const auto c = pjt::parse_config(R"(
# comment
[stuck_after_ms]
Received = 1000
Sent = 7000   ; trailing comment
[ordering]
reorder_wait_ms = 250
dedupe_capacity = 10
[memory]
retention_ms = 5
[snapshot]
every_events = 42
every_ms = 250
[slowdown]
enabled = false
ratio = 3.5
warmup = 7
)");
    EXPECT_EQ(c.stuck_after_ms[0], 1000);
    EXPECT_EQ(c.stuck_after_ms[3], 7000);
    EXPECT_EQ(c.reorder_wait_ms, 250);
    EXPECT_EQ(c.dedupe_capacity, 10u);
    EXPECT_EQ(c.retention_ms, 5);
    EXPECT_EQ(c.snapshot_every_events, 42);
    EXPECT_EQ(c.snapshot_every_ms, 250);
    EXPECT_FALSE(c.slowdown.enabled);
    EXPECT_DOUBLE_EQ(c.slowdown.ratio, 3.5);
    EXPECT_EQ(c.slowdown.warmup, 7);
}

TEST(Config, RejectsMistakes) {
    EXPECT_THROW(pjt::parse_config("[stuck_after_ms]\nSettled = 5\n"), std::runtime_error);
    EXPECT_THROW(pjt::parse_config("[stuck_after_ms]\nReceived = -5\n"), std::runtime_error);
    EXPECT_THROW(pjt::parse_config("[ordering]\nreorder_wait = 5\n"), std::runtime_error);
    EXPECT_THROW(pjt::parse_config("[ordering]\nreorder_wait_ms = soon\n"), std::runtime_error);
    EXPECT_THROW(pjt::parse_config("just text\n"), std::runtime_error);
}

TEST(Config, ProjectConfigFileLoads) {
    // Run from the repo root or engine/build; try both.
    for (const char* path : {"config/engine.ini", "../config/engine.ini", "../../config/engine.ini"}) {
        try {
            const auto c = pjt::load_config(path);
            EXPECT_GT(c.stuck_after_ms[0], 0);
            return;
        } catch (const std::runtime_error&) {
        }
    }
    GTEST_SKIP() << "config/engine.ini not found from this folder";
}
