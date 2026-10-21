#pragma once

#include <cstdint>

namespace galaxy::input {
struct WiimoteInputSnapshot;
}

// Cloud90 Game Link (PLAN-GAME-LINK.md): live-stream events change how the
// game plays. Effects are queued from any thread (network, window keys) and
// applied on the input thread to each virtual Wii Remote report, so the game
// itself is never touched from another thread. Timers count VIs (60 Hz) and
// stand still while a menu is open.
namespace galaxy::cloud90 {

enum class Effect : std::uint8_t {
    InvertControls,  // Nunchuk stick mirrored on both axes
    AutoJump,        // A pressed by itself every so often
    NoJump,          // A never reaches the game
    DrunkStick,      // the stick drifts in slow circles
    ForceSpin,       // Wii Remote shakes (spin)
    AutoRun,         // stick held forward
    Count,
};

// Longest any effect may last, whatever a request asks for.
inline constexpr std::uint32_t kMaxEffectMs = 60'000u;

// Any thread. Starts (or extends) `effect` for `duration_ms`, clamped to
// kMaxEffectMs. Ignored while paused.
void enqueue(Effect effect, std::uint32_t duration_ms) noexcept;

// Any thread. Panic key: ends every effect now and pauses new ones until
// set_paused(false).
void panic() noexcept;
void set_paused(bool paused) noexcept;
[[nodiscard]] bool paused() noexcept;

// Input thread, once per report. `in_play` is false in menus: timers freeze
// and the report is left alone.
void apply_input_effects(galaxy::input::WiimoteInputSnapshot& snapshot,
                         std::uint64_t current_vi,
                         bool in_play) noexcept;

// Remaining VIs of an effect (0 = off), for the settings page and tests.
[[nodiscard]] std::uint64_t remaining_vi(Effect effect,
                                         std::uint64_t current_vi) noexcept;

// Tests only: forget all state.
void reset_for_tests() noexcept;

}  // namespace galaxy::cloud90
