#include "pjt/stage.hpp"

#include <array>

namespace pjt {

namespace {
constexpr std::array<std::string_view, kStageCount> kNames{
    "Received", "Validated", "Screened", "Sent", "Settled", "Failed"};
}

bool is_final(Stage s) noexcept { return s == Stage::Settled || s == Stage::Failed; }

int path_position(Stage s) noexcept { return s == Stage::Failed ? -1 : index_of(s); }

Stage next_on_path(Stage s) noexcept { return static_cast<Stage>(index_of(s) + 1); }

bool can_move(Stage from, Stage to) noexcept {
    if (is_final(from)) return false;           // nothing leaves a final stage
    if (to == Stage::Failed) return true;       // any open stage can fail
    return path_position(to) == path_position(from) + 1;  // otherwise exactly one step forward
}

std::string_view to_string(Stage s) noexcept { return kNames[static_cast<std::size_t>(index_of(s))]; }

std::optional<Stage> parse_stage(std::string_view name) noexcept {
    for (std::size_t i = 0; i < kNames.size(); ++i) {
        if (kNames[i] == name) return static_cast<Stage>(i);
    }
    return std::nullopt;
}

}  // namespace pjt
