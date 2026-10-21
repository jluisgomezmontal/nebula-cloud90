#include "galaxy/cloud90_effects.h"

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

// Input thread only (plus remaining_vi, read on the same thread or in tests).
std::array<std::uint64_t, kEffectCount> g_until{};  // VI an effect ends at
std::uint64_t g_last_vi = 0;
bool g_has_last_vi = false;

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
    if (panic_now) g_until.fill(0);
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
    try {
        take_pending(vi);
    } catch (...) {
    }
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
    if (active(Effect::NoJump, vi)) {
        snapshot.buttons.a = false;
    } else if (active(Effect::AutoJump, vi)) {
        // Pressed 6 VIs out of every 45 (a jump about every 0.75 s).
        snapshot.buttons.a = snapshot.buttons.a || (vi % 45u) < 6u;
    }
    if (active(Effect::ForceSpin, vi)) {
        // A hard shake on X, flipping every other VI, reads as a spin.
        snapshot.accel.x = (vi & 1u) ? 0x3f0u : 0x010u;
    }
}

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
}

}  // namespace galaxy::cloud90
