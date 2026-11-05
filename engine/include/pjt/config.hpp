// Engine settings, read from a small INI file (config/engine.ini).
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>

#include "pjt/stage.hpp"

namespace pjt {

struct SlowdownConfig {
    bool enabled = true;
    double ratio = 1.5;       // warn when the recent average is this many times the normal one
    int warmup = 200;         // samples needed before a stage can warn
    double fast_alpha = 0.05;   // how quickly the "recent" average follows new values
    double slow_alpha = 0.001;  // how quickly the "normal" average follows new values
};

struct EngineConfig {
    // How long a payment may sit in each stage before it counts as stuck. 0 = no limit.
    std::array<std::int64_t, kStageCount> stuck_after_ms{4000, 4000, 5000, 6000, 0, 0};
    std::int64_t reorder_wait_ms = 1500;   // how long an early event waits for the missing one
    std::int64_t retention_ms = 30000;     // how long finished payments stay in memory
    std::size_t dedupe_capacity = 200000;  // how many recent event IDs to remember
    std::int64_t snapshot_every_events = 5000;  // pjt_replay: save after this many events
    std::int64_t snapshot_every_ms = 1000;      // pjt_stream: save at most this often (wall clock)
    SlowdownConfig slowdown;
};

// Throws std::runtime_error when the file can't be read or a value is invalid.
EngineConfig load_config(const std::string& path);
EngineConfig parse_config(const std::string& text);

}  // namespace pjt
