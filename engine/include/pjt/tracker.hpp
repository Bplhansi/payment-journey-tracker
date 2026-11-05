// The tracking engine: follows every payment through its stages and raises alerts.
#pragma once

#include <array>
#include <cstdint>
#include <deque>
#include <queue>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "pjt/config.hpp"
#include "pjt/event.hpp"
#include "pjt/records.hpp"
#include "pjt/slowdown.hpp"
#include "pjt/stage.hpp"

namespace pjt {

struct PaymentState {
    bool started = false;  // false until the first event is applied
    Stage stage = Stage::Received;
    std::int64_t amount_minor = 0;
    std::int64_t first_amount_minor = 0;
    std::string currency;
    std::int64_t last_update_ms = 0;
    bool invalid = false;
    bool finished = false;
    std::int64_t finished_at_ms = 0;
    std::vector<HistoryEntry> history;
    std::vector<Event> pending;  // events that arrived early, waiting for a missing one
    std::int64_t pending_since_ms = 0;
    bool stuck_open = false;
    bool amount_alerted = false;
    bool duplicate_alerted = false;
    bool out_of_order_alerted = false;
    bool invalid_alerted = false;
    // Bumped whenever a timer is replaced, so the old timer is ignored when it fires.
    std::uint32_t stuck_gen = 0;
    std::uint32_t reorder_gen = 0;
};

struct TrackerStats {
    std::int64_t events = 0;
    std::int64_t applied = 0;
    std::int64_t duplicates = 0;
    std::int64_t held = 0;
    std::int64_t stale = 0;
    std::array<std::int64_t, 7> alerts{};  // indexed by AlertType
};

class Tracker {
public:
    Tracker(EngineConfig config, Sink& sink);

    // Handles one event. The engine's clock is "payment time": the newest timestamp seen.
    void process(const Event& e);

    // Moves the clock forward and fires any timers that are due (stuck payments,
    // early events whose missing event never came, and clean-up of finished payments).
    void advance_to(std::int64_t now_ms);

    // End of input: move the clock far enough that every timer fires.
    void drain();

    std::int64_t now() const { return now_ms_; }
    const std::unordered_map<std::string, PaymentState>& payments() const { return payments_; }
    const TrackerStats& stats() const { return stats_; }
    const SlowdownDetector& slowdown() const { return slowdown_; }

    // Crash recovery: the whole engine state as JSON, plus where in the input it got to.
    std::string snapshot_json(const std::string& position) const;
    // Replaces the current state with a snapshot. Returns the saved position.
    std::string restore_json(const std::string& json);

private:
    enum class TimerKind : std::uint8_t { Reorder, Stuck, Retention };
    struct Timer {
        std::int64_t at;
        std::string payment_id;
        TimerKind kind;
        std::uint32_t gen;
        bool operator>(const Timer& o) const {
            if (at != o.at) return at > o.at;
            if (payment_id != o.payment_id) return payment_id > o.payment_id;
            return kind > o.kind;
        }
    };

    bool seen_before(const std::string& event_id) const { return seen_.count(event_id) != 0; }
    void remember(const std::string& event_id);
    void hold(const std::string& id, PaymentState& p, const Event& e);
    void apply(const std::string& id, PaymentState& p, const Event& e);
    void drain_pending(const std::string& id, PaymentState& p);
    void on_timer(const Timer& t);
    void schedule_stuck(const std::string& id, PaymentState& p);
    void raise(AlertType type, const std::string& alert_id, const std::string& payment_id,
               const PaymentState* p, std::optional<Stage> stage, std::string detail);
    void rebuild_timers();

    EngineConfig config_;
    Sink& sink_;
    SlowdownDetector slowdown_;
    std::int64_t now_ms_ = 0;
    std::unordered_map<std::string, PaymentState> payments_;
    std::unordered_set<std::string> seen_;
    std::deque<std::string> seen_order_;
    std::priority_queue<Timer, std::vector<Timer>, std::greater<Timer>> timers_;
    TrackerStats stats_;
};

}  // namespace pjt
