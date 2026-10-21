#include "galaxy/cloud90_game.h"

#include "galaxy/cloud90_effects.h"

#include <atomic>
#include <deque>
#include <mutex>

namespace galaxy::cloud90 {
namespace {

struct Queued {
    GameAction action;
    std::uint64_t at_ms;
};

std::mutex g_mutex;
std::deque<Queued> g_queue;  // guarded by g_mutex

// Written by the game thread each frame, read by the link.
std::atomic<std::uint64_t> g_last_frame_ms{0};
std::atomic<bool> g_seen_frame{false};
std::atomic<std::uint32_t> g_health{0};
std::atomic<std::uint32_t> g_max_health{0};
std::atomic<std::int32_t> g_lives{0};
std::atomic<std::int32_t> g_star_bits{0};
std::atomic<std::int32_t> g_coins{0};

// Game thread only.
std::uint64_t g_last_action_ms = 0;
bool g_has_last_action = false;

bool fresh(std::uint64_t now_ms) noexcept {
    if (!g_seen_frame.load(std::memory_order_acquire)) return false;
    const std::uint64_t last = g_last_frame_ms.load(std::memory_order_acquire);
    return now_ms >= last ? now_ms - last <= kPlayerFreshMs : true;
}

// Against values the caller read, so the link (atomics) and the game thread
// (live reads) apply the same rules.
bool useful(GameAction action, std::uint32_t health, std::uint32_t max_health, std::int32_t lives,
            std::int32_t star_bits, std::int32_t coins) noexcept {
    switch (action) {
    case GameAction::Heal: return health > 0 && health < max_health;
    case GameAction::Damage: return health > 1;
    case GameAction::LifeUp: return lives < 99;
    case GameAction::LifeDown: return lives > 1;
    case GameAction::StarBitsAdd: return star_bits < 9999;
    case GameAction::StarBitsRemove: return star_bits > 0;
    case GameAction::CoinsAdd: return coins < 999;
    case GameAction::CoinsRemove: return coins > 0;
    case GameAction::Count: break;
    }
    return false;
}

}  // namespace

std::optional<GameAction> game_action_from_name(std::string_view name) noexcept {
    if (name == "heal") return GameAction::Heal;
    if (name == "damage") return GameAction::Damage;
    if (name == "life_up") return GameAction::LifeUp;
    if (name == "life_down") return GameAction::LifeDown;
    if (name == "starbits_add") return GameAction::StarBitsAdd;
    if (name == "starbits_remove") return GameAction::StarBitsRemove;
    if (name == "coins_add") return GameAction::CoinsAdd;
    if (name == "coins_remove") return GameAction::CoinsRemove;
    return std::nullopt;
}

bool game_action_applicable(GameAction action, std::uint64_t now_ms) noexcept {
    if (action >= GameAction::Count || !fresh(now_ms)) return false;
    return useful(action, g_health.load(std::memory_order_relaxed),
                  g_max_health.load(std::memory_order_relaxed), g_lives.load(std::memory_order_relaxed),
                  g_star_bits.load(std::memory_order_relaxed), g_coins.load(std::memory_order_relaxed));
}

bool request_game_action(GameAction action, std::uint64_t now_ms) noexcept {
    if (action >= GameAction::Count || paused()) return false;
    try {
        const std::lock_guard<std::mutex> lock(g_mutex);
        if (g_queue.size() >= kGameQueueMax) return false;
        g_queue.push_back({action, now_ms});
        return true;
    } catch (...) {
        return false;
    }
}

void on_player_frame(PlayerAccess& player, std::uint64_t now_ms) noexcept {
    std::uint32_t health = player.health();
    std::uint32_t max_health = player.max_health();
    std::int32_t lives = player.lives();
    std::int32_t star_bits = player.star_bits();
    std::int32_t coins = player.coins();

    std::optional<GameAction> next;
    if (!paused() && (!g_has_last_action || now_ms - g_last_action_ms >= kGameActionSpacingMs)) {
        try {
            const std::lock_guard<std::mutex> lock(g_mutex);
            while (!g_queue.empty() && now_ms - g_queue.front().at_ms > kGameActionTimeoutMs)
                g_queue.pop_front();
            if (!g_queue.empty()) {
                next = g_queue.front().action;
                g_queue.pop_front();
            }
        } catch (...) {
        }
    }
    // Checked again here: several queued actions may have used it up.
    if (next && useful(*next, health, max_health, lives, star_bits, coins)) {
        switch (*next) {
        case GameAction::Heal: player.heal(1); break;
        case GameAction::Damage: player.damage(); break;
        case GameAction::LifeUp: player.add_lives(1); break;
        case GameAction::LifeDown: player.add_lives(-1); break;
        case GameAction::StarBitsAdd: player.add_star_bits(kStarBitsStep); break;
        case GameAction::StarBitsRemove:
            player.add_star_bits(-(star_bits < kStarBitsStep ? star_bits : kStarBitsStep));
            break;
        case GameAction::CoinsAdd: player.add_coins(kCoinsStep); break;
        case GameAction::CoinsRemove: player.add_coins(-(coins < kCoinsStep ? coins : kCoinsStep)); break;
        case GameAction::Count: break;
        }
        g_last_action_ms = now_ms;
        g_has_last_action = true;
        health = player.health();
        lives = player.lives();
        star_bits = player.star_bits();
        coins = player.coins();
    }

    g_health.store(health, std::memory_order_relaxed);
    g_max_health.store(max_health, std::memory_order_relaxed);
    g_lives.store(lives, std::memory_order_relaxed);
    g_star_bits.store(star_bits, std::memory_order_relaxed);
    g_coins.store(coins, std::memory_order_relaxed);
    g_last_frame_ms.store(now_ms, std::memory_order_release);
    g_seen_frame.store(true, std::memory_order_release);
}

void clear_game_actions() noexcept {
    try {
        const std::lock_guard<std::mutex> lock(g_mutex);
        g_queue.clear();
    } catch (...) {
    }
}

void reset_game_for_tests() noexcept {
    clear_game_actions();
    g_seen_frame.store(false);
    g_last_frame_ms.store(0);
    g_health.store(0);
    g_max_health.store(0);
    g_lives.store(0);
    g_star_bits.store(0);
    g_coins.store(0);
    g_has_last_action = false;
    g_last_action_ms = 0;
}

}  // namespace galaxy::cloud90
