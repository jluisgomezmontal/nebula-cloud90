#pragma once

#include <cstdint>
#include <optional>
#include <string_view>

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
    Paralyze,        // nothing but pause reaches the game: buttons, stick, shakes
    Crouch,          // Z held (crouch; with a jump, a backflip or long jump)
    DrunkPointer,    // the Wii Remote pointer drifts and shakes
    // Screen (post-process pass, phase 4): bits of screen_effect_flags().
    FlipScreen,      // upside down
    MirrorScreen,    // left-right mirror
    Grayscale,       // black and white
    Pixelate,        // big pixels
    WobbleScreen,    // wavy "drunk" image
    InvertColors,    // negative
    TunnelVision,    // dark edges, small circle in the middle
    Earthquake,      // the image shakes
    GameBoy,         // four shades of green
    Fisheye,         // bulging middle
    Blur,            // out of focus
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

// Input thread, right after apply_input_effects: the pointer as the game's
// KPAD reads it with a mouse ([-1, 1], from the host, not the IR dots).
void apply_pointer_effects(float& x, float& y, std::uint64_t current_vi) noexcept;

// Remaining VIs of an effect (0 = off), for the settings page and tests.
[[nodiscard]] std::uint64_t remaining_vi(Effect effect,
                                         std::uint64_t current_vi) noexcept;

// The id Cloud90 sends ("invert_controls"…) → the effect, if known.
[[nodiscard]] std::optional<Effect> effect_from_name(std::string_view name) noexcept;

// Screen effects active now, as bits (1 << (effect - FlipScreen)); 0 in
// menus. Read by the renderer each frame (any thread).
[[nodiscard]] std::uint32_t screen_effect_flags() noexcept;

// What the game allows right now (input thread, each report). Mario has no
// spin in the prologue (the Luma gives it to him after).
void set_spin_available(bool available) noexcept;

// Whether an effect would show now: false for the spin before Mario has
// it. The link answers "no disponible ahora" instead of queueing it.
[[nodiscard]] bool effect_applicable(Effect effect) noexcept;

// Whether the last report was during play (not a menu), for the link to
// tell Cloud90 to hold effects.
[[nodiscard]] bool in_play() noexcept;

// Tests only: forget all state.
void reset_for_tests() noexcept;

}  // namespace galaxy::cloud90
