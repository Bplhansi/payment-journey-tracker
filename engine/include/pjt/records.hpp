// What the engine reports: event outcomes, alerts and payment state updates.
#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "pjt/event.hpp"
#include "pjt/stage.hpp"

namespace pjt {

enum class AlertType : std::uint8_t {
    Stuck,           // a payment stopped moving for longer than its stage's time limit
    InvalidJourney,  // a stage was skipped (the missing event never arrived)
    Failed,          // the payment moved to Failed
    AmountMismatch,  // the amount changed between stages
    Duplicate,       // the same event arrived more than once (counted once)
    OutOfOrder,      // a later event arrived first; it was held and applied in order
    Slowdown,        // a stage became much slower than its recent normal range
};

enum class Severity : std::uint8_t { Info, Warning, High };

// What happened to one incoming event.
enum class EventStatus : std::uint8_t {
    Applied,    // moved the payment on
    Duplicate,  // already seen: ignored
    Held,       // arrived early: waiting for an earlier event
    Stale,      // arrived after the payment had already moved past it: ignored
};

struct HistoryEntry {
    std::string event_id;
    Stage stage = Stage::Received;
    std::int64_t timestamp_ms = 0;
    std::int64_t amount_minor = 0;

    bool operator==(const HistoryEntry&) const = default;
};

struct Alert {
    std::string alert_id;  // the same problem always gets the same ID, so repeats can be merged
    AlertType type = AlertType::Stuck;
    std::string payment_id;     // empty for stage-wide alerts (Slowdown)
    std::optional<Stage> stage;
    std::int64_t raised_at_ms = 0;
    std::string detail;
    std::vector<HistoryEntry> history;  // the payment's applied events so far
};

struct StateUpdate {
    std::string payment_id;
    Stage stage = Stage::Received;
    std::int64_t amount_minor = 0;
    std::string currency;
    std::int64_t last_update_ms = 0;
    bool invalid = false;   // a stage was skipped
    bool finished = false;  // Settled or Failed
    int events_applied = 0;
};

Severity severity_of(AlertType t) noexcept;
std::string_view to_string(AlertType t) noexcept;
std::string_view to_string(Severity s) noexcept;
std::string_view to_string(EventStatus s) noexcept;

// Receives everything the engine reports. Override only what you need.
class Sink {
public:
    virtual ~Sink() = default;
    virtual void on_event(const Event& /*e*/, EventStatus /*status*/, std::int64_t /*now_ms*/) {}
    virtual void on_alert(const Alert& /*a*/) {}
    virtual void on_alert_cleared(const std::string& /*alert_id*/, std::int64_t /*now_ms*/) {}
    virtual void on_state(const StateUpdate& /*s*/) {}
};

}  // namespace pjt
