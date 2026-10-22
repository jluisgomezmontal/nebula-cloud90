#include "galaxy/cloud90_effects.h"

#include "galaxy/cloud90_game.h"
#include "galaxy/native_input.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <mutex>
#include <vector>

namespace galaxy::cloud90 {
namespace {

constexpr std::size_t kEffectCount = static_cast<std::size_t>(Effect::Count);
// The Nunchuk stick's rest position (NunchukState defaults).
constexpr int kStickCenterX = 0x7b;
constexpr int kStickCenterY = 0x81;
constexpr int kStickRadius = 100;

struct Pending {
    Effect effect;
    std::uint32_t duration_ms;
};

std::mutex g_mutex;
std::vector<Pending> g_pending;   // guarded by g_mutex
bool g_panic_requested = false;   // guarded by g_mutex
std::atomic<bool> g_paused{false};
std::atomic<bool> g_in_play{true};
std::atomic<std::uint32_t> g_screen_flags{0};
std::atomic<bool> g_spin_available{true};
std::atomic<bool> g_shield_active{false};

// Input thread only (plus remaining_vi, read on the same thread or in tests).
std::array<std::uint64_t, kEffectCount> g_until{};  // VI an effect ends at
std::uint64_t g_last_vi = 0;
bool g_has_last_vi = false;
// Spin: a new shake burst every this many reports (~0.4 s at 100 Hz).
constexpr std::uint32_t kSpinEveryReports = 40;
std::uint32_t g_spin_reports = 0;
bool g_spin_previous_press = false;
std::uint8_t g_spin_frames_left = 0;

std::uint64_t ms_to_vi(std::uint32_t ms) noexcept {
    return (static_cast<std::uint64_t>(std::min(ms, kMaxEffectMs)) * 60u + 999u) / 1000u;
}

std::uint8_t clamp_axis(int value) noexcept {
    return static_cast<std::uint8_t>(std::clamp(value, 0, 255));
}

bool active(Effect effect, std::uint64_t vi) noexcept {
    return g_until[static_cast<std::size_t>(effect)] > vi;
}

void take_pending(std::uint64_t vi) {
    std::vector<Pending> pending;
    bool panic_now = false;
    {
        const std::lock_guard<std::mutex> lock(g_mutex);
        pending.swap(g_pending);
        panic_now = g_panic_requested;
        g_panic_requested = false;
    }
    if (panic_now) {
        // The shield only protects: panic ends everything else.
        const std::uint64_t shield = g_until[static_cast<std::size_t>(Effect::Shield)];
        g_until.fill(0);
        g_until[static_cast<std::size_t>(Effect::Shield)] = shield;
    }
    for (const Pending& p : pending) {
        auto& until = g_until[static_cast<std::size_t>(p.effect)];
        // A repeat extends from whichever ends later: now or the current run.
        until = std::max(until, vi) + ms_to_vi(p.duration_ms);
        until = std::min(until, vi + ms_to_vi(kMaxEffectMs));
    }
}

}  // namespace

void enqueue(Effect effect, std::uint32_t duration_ms) noexcept {
    if (effect >= Effect::Count || duration_ms == 0 || g_paused.load(std::memory_order_relaxed)) return;
    try {
        const std::lock_guard<std::mutex> lock(g_mutex);
        if (g_pending.size() < 64) g_pending.push_back({effect, duration_ms});
    } catch (...) {
        // Out of memory or a broken mutex: drop the effect, never the game.
    }
}

void panic() noexcept {
    g_paused.store(true, std::memory_order_relaxed);
    try {
        const std::lock_guard<std::mutex> lock(g_mutex);
        g_pending.clear();
        g_panic_requested = true;
    } catch (...) {
    }
    clear_game_actions();
}

void set_paused(bool paused) noexcept { g_paused.store(paused, std::memory_order_relaxed); }

bool paused() noexcept { return g_paused.load(std::memory_order_relaxed); }

void apply_input_effects(galaxy::input::WiimoteInputSnapshot& snapshot,
                         std::uint64_t vi,
                         bool in_play) noexcept {
    // Menus freeze every timer: the time spent there is added back, so no
    // effect runs out behind a pause screen. Done before taking new ones, so
    // an effect queued during a menu isn't stretched too.
    if (g_has_last_vi && vi > g_last_vi && !in_play) {
        const std::uint64_t frozen = vi - g_last_vi;
        for (auto& until : g_until) {
            if (until > g_last_vi) until += frozen;
        }
    }
    g_last_vi = vi;
    g_has_last_vi = true;
    g_in_play.store(in_play, std::memory_order_relaxed);
    try {
        take_pending(vi);
    } catch (...) {
    }
    // Screen effects: published for the renderer; none over menus.
    std::uint32_t flags = 0;
    if (in_play) {
        for (auto e = static_cast<std::size_t>(Effect::FlipScreen); e < kEffectCount; ++e)
            if (g_until[e] > vi) flags |= 1u << (e - static_cast<std::size_t>(Effect::FlipScreen));
    }
    g_screen_flags.store(flags, std::memory_order_relaxed);
    g_shield_active.store(active(Effect::Shield, vi), std::memory_order_relaxed);
    if (!in_play) return;

    auto& stick = snapshot.nunchuk;
    if (active(Effect::AutoRun, vi)) {
        stick.stick_x = static_cast<std::uint8_t>(kStickCenterX);
        stick.stick_y = clamp_axis(kStickCenterY + kStickRadius);
    }
    if (active(Effect::DrunkStick, vi)) {
        const double t = static_cast<double>(vi) / 60.0;
        stick.stick_x = clamp_axis(stick.stick_x + static_cast<int>(std::lround(std::sin(t * 2.1) * 45.0)));
        stick.stick_y = clamp_axis(stick.stick_y + static_cast<int>(std::lround(std::cos(t * 1.7) * 45.0)));
    }
    if (active(Effect::InvertControls, vi)) {
        stick.stick_x = clamp_axis(2 * kStickCenterX - stick.stick_x);
        stick.stick_y = clamp_axis(2 * kStickCenterY - stick.stick_y);
    }
    if (active(Effect::Crouch, vi)) snapshot.nunchuk.z = true;
    if (active(Effect::NoJump, vi)) {
        snapshot.buttons.a = false;
    } else if (active(Effect::AutoJump, vi)) {
        // Pressed 6 VIs out of every 45 (a jump about every 0.75 s).
        snapshot.buttons.a = snapshot.buttons.a || (vi % 45u) < 6u;
    }
    // Spin: the same 12-report shake burst Nebula sends when the spin key is
    // pressed, re-pressed every kSpinEveryReports so each burst reads as a
    // new shake (a steady shake doesn't). A burst in progress finishes even
    // when the effect ends.
    const bool spinning =
        active(Effect::ForceSpin, vi) && g_spin_available.load(std::memory_order_relaxed);
    const bool press = spinning && (g_spin_reports++ % kSpinEveryReports) < 2u;
    if (!spinning) g_spin_reports = 0;
    (void)galaxy::input::advance_wiimote_shake_accel(
        snapshot, press, g_spin_previous_press, g_spin_frames_left);
    // Last, over everything above: only pause and the HOME menu get through.
    if (active(Effect::Paralyze, vi)) {
        const galaxy::input::WiimoteButtons keep = snapshot.buttons;
        snapshot.buttons = {};
        snapshot.buttons.plus = keep.plus;
        snapshot.buttons.minus = keep.minus;
        snapshot.buttons.home = keep.home;
        snapshot.accel = {galaxy::input::kWiimoteRestAccelX, galaxy::input::kWiimoteRestAccelY,
                          galaxy::input::kWiimoteRestAccelZ};
        snapshot.nunchuk.stick_x = static_cast<std::uint8_t>(kStickCenterX);
        snapshot.nunchuk.stick_y = static_cast<std::uint8_t>(kStickCenterY);
        snapshot.nunchuk.c = false;
        snapshot.nunchuk.z = false;
        snapshot.nunchuk.accel = {};
    }
}

std::optional<Effect> effect_from_name(std::string_view name) noexcept {
    if (name == "invert_controls") return Effect::InvertControls;
    if (name == "auto_jump") return Effect::AutoJump;
    if (name == "no_jump") return Effect::NoJump;
    if (name == "drunk_stick") return Effect::DrunkStick;
    if (name == "force_spin") return Effect::ForceSpin;
    if (name == "auto_run") return Effect::AutoRun;
    if (name == "paralyze") return Effect::Paralyze;
    if (name == "crouch") return Effect::Crouch;
    if (name == "flip_screen") return Effect::FlipScreen;
    if (name == "mirror_screen") return Effect::MirrorScreen;
    if (name == "grayscale") return Effect::Grayscale;
    if (name == "pixelate") return Effect::Pixelate;
    if (name == "wobble_screen") return Effect::WobbleScreen;
    if (name == "invert_colors") return Effect::InvertColors;
    if (name == "tunnel_vision") return Effect::TunnelVision;
    if (name == "earthquake") return Effect::Earthquake;
    if (name == "gameboy") return Effect::GameBoy;
    if (name == "fisheye") return Effect::Fisheye;
    if (name == "blur") return Effect::Blur;
    if (name == "blackout") return Effect::Blackout;
    if (name == "shield") return Effect::Shield;
    return std::nullopt;
}

bool effect_troll(Effect effect) noexcept { return effect < Effect::Count && effect != Effect::Shield; }

bool shield_active() noexcept { return g_shield_active.load(std::memory_order_relaxed); }

bool in_play() noexcept { return g_in_play.load(std::memory_order_relaxed); }

void set_spin_available(bool available) noexcept {
    g_spin_available.store(available, std::memory_order_relaxed);
}

bool effect_applicable(Effect effect) noexcept {
    if (effect == Effect::ForceSpin) return g_spin_available.load(std::memory_order_relaxed);
    return effect < Effect::Count;
}

std::uint32_t screen_effect_flags() noexcept { return g_screen_flags.load(std::memory_order_relaxed); }

std::uint64_t remaining_vi(Effect effect, std::uint64_t vi) noexcept {
    if (effect >= Effect::Count) return 0;
    const std::uint64_t until = g_until[static_cast<std::size_t>(effect)];
    return until > vi ? until - vi : 0;
}

void reset_for_tests() noexcept {
    try {
        const std::lock_guard<std::mutex> lock(g_mutex);
        g_pending.clear();
        g_panic_requested = false;
    } catch (...) {
    }
    g_paused.store(false);
    g_until.fill(0);
    g_last_vi = 0;
    g_has_last_vi = false;
    g_spin_reports = 0;
    g_spin_previous_press = false;
    g_spin_frames_left = 0;
    g_screen_flags.store(0);
    g_spin_available.store(true);
    g_shield_active.store(false);
}

}  // namespace galaxy::cloud90
