// Shared helpers for the engine tests.
#pragma once

#include <algorithm>
#include <map>
#include <string>
#include <tuple>
#include <vector>

#include "pjt/records.hpp"
#include "pjt/tracker.hpp"

namespace pjt::testing {

// Keeps everything the engine reports, so tests can check it.
struct CollectSink : Sink {
    std::vector<std::pair<std::string, EventStatus>> events;
    std::vector<Alert> alerts;
    std::vector<std::string> cleared;
    std::map<std::string, StateUpdate> states;  // latest state per payment

    void on_event(const Event& e, EventStatus s, std::int64_t) override { events.emplace_back(e.event_id, s); }
    void on_alert(const Alert& a) override { alerts.push_back(a); }
    void on_alert_cleared(const std::string& id, std::int64_t) override { cleared.push_back(id); }
    void on_state(const StateUpdate& s) override { states[s.payment_id] = s; }

    int count(AlertType t) const {
        int n = 0;
        for (const auto& a : alerts) n += a.type == t;
        return n;
    }
    bool has_alert(const std::string& id) const {
        for (const auto& a : alerts) {
            if (a.alert_id == id) return true;
        }
        return false;
    }
};

inline Event ev(const std::string& event_id, const std::string& payment_id, Stage stage, std::int64_t ts,
                std::int64_t amount = 25000, const std::string& currency = "GBP") {
    return {event_id, payment_id, stage, ts, amount, currency};
}

// A healthy payment: one event per stage, `gap_ms` apart, starting at `start_ms`.
inline std::vector<Event> healthy(const std::string& payment_id, std::int64_t start_ms, std::int64_t gap_ms = 500) {
    std::vector<Event> out;
    const Stage path[] = {Stage::Received, Stage::Validated, Stage::Screened, Stage::Sent, Stage::Settled};
    int i = 0;
    for (Stage s : path) {
        out.push_back(ev(payment_id + "-e" + std::to_string(i), payment_id, s, start_ms + i * gap_ms));
        ++i;
    }
    return out;
}

// A test config with stage limits that are easy to reason about.
inline EngineConfig test_config() {
    EngineConfig c;
    c.stuck_after_ms = {4000, 4000, 4000, 4000, 0, 0};
    c.reorder_wait_ms = 1000;
    c.retention_ms = 10000;
    c.slowdown.enabled = false;
    return c;
}

// The comparable outcome of a run: final state of each payment and the set of alert IDs.
using Outcome = std::tuple<std::map<std::string, std::tuple<Stage, std::int64_t, bool, bool>>, std::vector<std::string>>;

inline Outcome outcome_of(const CollectSink& sink) {
    std::map<std::string, std::tuple<Stage, std::int64_t, bool, bool>> finals;
    for (const auto& [id, s] : sink.states) finals[id] = {s.stage, s.amount_minor, s.invalid, s.finished};
    std::vector<std::string> ids;
    for (const auto& a : sink.alerts) ids.push_back(a.alert_id);
    std::sort(ids.begin(), ids.end());
    ids.erase(std::unique(ids.begin(), ids.end()), ids.end());
    return {finals, ids};
}

}  // namespace pjt::testing
