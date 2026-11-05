#include "pjt/records.hpp"

namespace pjt {

Severity severity_of(AlertType t) noexcept {
    switch (t) {
        case AlertType::Duplicate:
        case AlertType::OutOfOrder: return Severity::Info;
        case AlertType::Slowdown: return Severity::Warning;
        default: return Severity::High;
    }
}

std::string_view to_string(AlertType t) noexcept {
    switch (t) {
        case AlertType::Stuck: return "stuck";
        case AlertType::InvalidJourney: return "invalid_journey";
        case AlertType::Failed: return "failed";
        case AlertType::AmountMismatch: return "amount_mismatch";
        case AlertType::Duplicate: return "duplicate";
        case AlertType::OutOfOrder: return "out_of_order";
        case AlertType::Slowdown: return "slowdown";
    }
    return "unknown";
}

std::string_view to_string(Severity s) noexcept {
    switch (s) {
        case Severity::Info: return "info";
        case Severity::Warning: return "warning";
        case Severity::High: return "high";
    }
    return "unknown";
}

std::string_view to_string(EventStatus s) noexcept {
    switch (s) {
        case EventStatus::Applied: return "applied";
        case EventStatus::Duplicate: return "duplicate";
        case EventStatus::Held: return "held";
        case EventStatus::Stale: return "stale";
    }
    return "unknown";
}

}  // namespace pjt
