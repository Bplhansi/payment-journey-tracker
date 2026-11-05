#include "pjt/event.hpp"

#include <cctype>
#include <nlohmann/json.hpp>

namespace pjt {

namespace {

bool fail(std::string* error, std::string message) {
    if (error) *error = std::move(message);
    return false;
}

bool valid_currency(const std::string& c) {
    if (c.size() != 3) return false;
    for (char ch : c) {
        if (!std::isupper(static_cast<unsigned char>(ch))) return false;
    }
    return true;
}

bool read_string(const nlohmann::json& j, const char* key, std::string& out, std::string* error) {
    auto it = j.find(key);
    if (it == j.end() || !it->is_string()) return fail(error, std::string(key) + " must be a string");
    out = it->get<std::string>();
    if (out.empty()) return fail(error, std::string(key) + " must not be empty");
    return true;
}

bool read_int(const nlohmann::json& j, const char* key, std::int64_t& out, std::string* error) {
    auto it = j.find(key);
    if (it == j.end() || !it->is_number_integer()) return fail(error, std::string(key) + " must be a whole number");
    out = it->get<std::int64_t>();
    return true;
}

}  // namespace

std::optional<Event> parse_event(std::string_view json, std::string* error) {
    const auto j = nlohmann::json::parse(json, nullptr, /*allow_exceptions=*/false);
    if (j.is_discarded() || !j.is_object()) {
        fail(error, "not a JSON object");
        return std::nullopt;
    }
    Event e;
    std::string stage_name;
    if (!read_string(j, "event_id", e.event_id, error) || !read_string(j, "payment_id", e.payment_id, error) ||
        !read_string(j, "stage", stage_name, error) || !read_int(j, "timestamp_ms", e.timestamp_ms, error) ||
        !read_int(j, "amount_minor", e.amount_minor, error) || !read_string(j, "currency", e.currency, error)) {
        return std::nullopt;
    }
    const auto stage = parse_stage(stage_name);
    if (!stage) {
        fail(error, "unknown stage: " + stage_name);
        return std::nullopt;
    }
    e.stage = *stage;
    if (e.timestamp_ms < 0) {
        fail(error, "timestamp_ms must not be negative");
        return std::nullopt;
    }
    if (e.amount_minor < 0) {
        fail(error, "amount_minor must not be negative");
        return std::nullopt;
    }
    if (!valid_currency(e.currency)) {
        fail(error, "currency must be three capital letters");
        return std::nullopt;
    }
    return e;
}

std::string to_json(const Event& e) {
    nlohmann::json j{{"event_id", e.event_id},         {"payment_id", e.payment_id},
                     {"stage", std::string(to_string(e.stage))}, {"timestamp_ms", e.timestamp_ms},
                     {"amount_minor", e.amount_minor}, {"currency", e.currency}};
    return j.dump();
}

}  // namespace pjt
