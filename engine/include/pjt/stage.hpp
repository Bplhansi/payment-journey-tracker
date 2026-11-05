// Payment stages and the rules for moving between them (the state machine).
#pragma once

#include <cstdint>
#include <optional>
#include <string_view>

namespace pjt {

// The normal journey is Received -> Validated -> Screened -> Sent -> Settled.
// A payment can also move to Failed from any stage before Settled.
// Settled and Failed are final: nothing moves out of them.
enum class Stage : std::uint8_t { Received, Validated, Screened, Sent, Settled, Failed };

inline constexpr int kStageCount = 6;

// True when a payment may move from `from` straight to `to` in one event.
bool can_move(Stage from, Stage to) noexcept;

// True for Settled and Failed.
bool is_final(Stage s) noexcept;

// Position on the normal path: Received = 0 ... Settled = 4. Failed has no position (returns -1).
int path_position(Stage s) noexcept;

// The stage after `s` on the normal path. Only call for Received..Sent.
Stage next_on_path(Stage s) noexcept;

std::string_view to_string(Stage s) noexcept;
std::optional<Stage> parse_stage(std::string_view name) noexcept;

inline int index_of(Stage s) noexcept { return static_cast<int>(s); }

}  // namespace pjt
