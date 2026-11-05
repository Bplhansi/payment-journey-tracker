// One payment event, and reading/writing it as a line of JSON.
#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

#include "pjt/stage.hpp"

namespace pjt {

// Money is kept in whole minor units (pence, cents) so no rounding can happen.
struct Event {
    std::string event_id;    // unique per event, used to spot duplicates
    std::string payment_id;
    Stage stage = Stage::Received;
    std::int64_t timestamp_ms = 0;  // when the stage happened (Unix time in milliseconds)
    std::int64_t amount_minor = 0;
    std::string currency;    // three capital letters, e.g. GBP

    bool operator==(const Event&) const = default;
};

// Parses one JSON object. Returns nothing (and fills `error`, if given) when a field is
// missing, has the wrong type, or holds a bad value.
std::optional<Event> parse_event(std::string_view json, std::string* error = nullptr);

std::string to_json(const Event& e);

}  // namespace pjt
