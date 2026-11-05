// Turns everything the engine reports into JSON lines (one record per line).
#pragma once

#include <string>
#include <utility>
#include <vector>

#include "pjt/records.hpp"

namespace pjt {

// Record types: "event", "alert", "alert_cleared" and "state". Downstream (the Python
// alert service) stores them by ID, so receiving a record twice after a crash is harmless.
class JsonSink : public Sink {
public:
    void on_event(const Event& e, EventStatus status, std::int64_t now_ms) override;
    void on_alert(const Alert& a) override;
    void on_alert_cleared(const std::string& alert_id, std::int64_t now_ms) override;
    void on_state(const StateUpdate& s) override;

    // Records produced since the last call. The caller writes them somewhere.
    std::vector<std::string> take() { return std::exchange(lines_, {}); }
    std::size_t size() const { return lines_.size(); }

private:
    std::vector<std::string> lines_;
};

std::string alert_to_json(const Alert& a);

}  // namespace pjt
