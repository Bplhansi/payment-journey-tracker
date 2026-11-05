#include "pjt/json_sink.hpp"

#include <nlohmann/json.hpp>

namespace pjt {

using nlohmann::json;

void JsonSink::on_event(const Event& e, EventStatus status, std::int64_t now_ms) {
    lines_.push_back(json{{"type", "event"},
                          {"event_id", e.event_id},
                          {"payment_id", e.payment_id},
                          {"stage", to_string(e.stage)},
                          {"timestamp_ms", e.timestamp_ms},
                          {"amount_minor", e.amount_minor},
                          {"currency", e.currency},
                          {"status", to_string(status)},
                          {"seen_at_ms", now_ms}}
                         .dump());
}

std::string alert_to_json(const Alert& a) {
    json history = json::array();
    for (const auto& h : a.history) {
        history.push_back({{"event_id", h.event_id},
                           {"stage", to_string(h.stage)},
                           {"timestamp_ms", h.timestamp_ms},
                           {"amount_minor", h.amount_minor}});
    }
    return json{{"type", "alert"},
                {"alert_id", a.alert_id},
                {"alert_type", to_string(a.type)},
                {"severity", to_string(severity_of(a.type))},
                {"payment_id", a.payment_id.empty() ? json(nullptr) : json(a.payment_id)},
                {"stage", a.stage ? json(to_string(*a.stage)) : json(nullptr)},
                {"raised_at_ms", a.raised_at_ms},
                {"detail", a.detail},
                {"history", std::move(history)}}
        .dump();
}

void JsonSink::on_alert(const Alert& a) { lines_.push_back(alert_to_json(a)); }

void JsonSink::on_alert_cleared(const std::string& alert_id, std::int64_t now_ms) {
    lines_.push_back(json{{"type", "alert_cleared"}, {"alert_id", alert_id}, {"cleared_at_ms", now_ms}}.dump());
}

void JsonSink::on_state(const StateUpdate& s) {
    lines_.push_back(json{{"type", "state"},
                          {"payment_id", s.payment_id},
                          {"stage", to_string(s.stage)},
                          {"amount_minor", s.amount_minor},
                          {"currency", s.currency},
                          {"last_update_ms", s.last_update_ms},
                          {"invalid", s.invalid},
                          {"finished", s.finished},
                          {"events_applied", s.events_applied}}
                         .dump());
}

}  // namespace pjt
