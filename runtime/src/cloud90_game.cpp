#include "galaxy/cloud90_game.h"

#include "galaxy/cloud90_effects.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <deque>
#include <mutex>

namespace galaxy::cloud90 {
namespace {

struct Queued {
    GameAction action;
    std::uint64_t at_ms;
    std::uint32_t duration_ms;
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
// Bit n: item status n can be given now (its models are loaded).
std::atomic<std::uint32_t> g_items_ready{0};
std::atomic<std::uint32_t> g_power_mode{0};
// Bit n: an EnemyKind n is waiting to come out.
std::atomic<std::uint32_t> g_enemies_ready{0};
std::atomic<bool> g_in_rush{false};
std::atomic<bool> g_can_back_to_start{false};
std::atomic<bool> g_can_rewind{false};
std::atomic<std::uint64_t> g_grace_until_ms{0};
// For the log only: -1 = no start / no rewind point.
std::atomic<float> g_start_distance{-1.0f};
std::atomic<float> g_rewind_distance{-1.0f};
std::atomic<bool> g_on_ground{false};
// Panic from another thread: the game thread ends what's running.
std::atomic<bool> g_clear_requested{false};

// The timed actions, Giant..TinyJump.
constexpr std::size_t kTimedFirst = static_cast<std::size_t>(GameAction::Giant);
constexpr std::size_t kTimedCount = static_cast<std::size_t>(GameAction::Count) - kTimedFirst;

// Floats of MarioConstTable a timed action changes: [first, first + 4 * count),
// scaled by `factor`, or set to the floats at `source` when it isn't 0.
struct TableChange {
    GameAction action;
    std::uint32_t first;
    std::uint32_t count;
    float factor;
    std::uint32_t source;
};
constexpr std::array<TableChange, 10> kTableChanges{{
    {GameAction::SuperJump, 0x000u, 9u, kSuperJumpFactor, 0u},     // mJumpHeight[9]
    {GameAction::MoonGravity, 0x024u, 12u, kMoonGravityFactor, 0u},  // mGravityJumping[12]
    {GameAction::SuperSpeed, 0x224u, 1u, kSuperSpeedFactor, 0u},   // mWalkSpeed
    {GameAction::Slow, 0x224u, 1u, kSlowFactor, 0u},
    {GameAction::TinyJump, 0x000u, 9u, kTinyJumpFactor, 0u},
    // The grip on ice the table holds for icy ground, on every ground.
    {GameAction::IceFloor, 0x228u, 1u, 1.0f, 0x294u},  // mInertiaStandardStop <- Ice
    {GameAction::IceFloor, 0x22Cu, 1u, 1.0f, 0x298u},  // mInertiaStandardMax <- Ice
    {GameAction::IceFloor, 0x250u, 1u, 1.0f, 0x29Cu},  // mInertiaStartSpin <- Ice
    {GameAction::IceFloor, 0x238u, 1u, 1.0f, 0x2A0u},  // mInertiaStop <- IceStop
    {GameAction::IceFloor, 0x2DCu, 1u, 1.0f, 0x2A0u},  // mInertiaBrake <- IceStop
}};
constexpr std::size_t kMaxTableFloats = 12u;

// Timed actions that change the same floats the other way: the one asked
// for last ends the other.
constexpr std::array<std::array<GameAction, 2>, 2> kOpposites{{
    {GameAction::SuperSpeed, GameAction::Slow},
    {GameAction::SuperJump, GameAction::TinyJump},
}};

struct SavedTable {
    bool saved = false;
    std::uint32_t table = 0;
    std::array<float, kMaxTableFloats> values{};
};

struct Sample {
    std::uint64_t at_ms;
    Vec3 position;
};

// Game thread only.
std::uint64_t g_shake_until_ms = 0;
std::uint64_t g_last_shake_ms = 0;
std::uint64_t g_last_action_ms = 0;
bool g_has_last_action = false;
std::array<std::uint64_t, kTimedCount> g_timed_until{};
bool g_scale_saved = false;
float g_saved_scale = 1.0f;
std::array<SavedTable, kTableChanges.size()> g_saved_tables{};
// Where Mario stood (on the ground), oldest first, and where he first stood
// in this scene or since he respawned.
std::deque<Sample> g_positions;
bool g_has_start = false;
Vec3 g_start{};
std::uint64_t g_last_sample_ms = 0;
bool g_was_dead = false;
std::uint32_t g_scene = 0;

std::size_t timed_index(GameAction action) noexcept {
    return static_cast<std::size_t>(action) - kTimedFirst;
}

bool fresh(std::uint64_t now_ms) noexcept {
    if (!g_seen_frame.load(std::memory_order_acquire)) return false;
    const std::uint64_t last = g_last_frame_ms.load(std::memory_order_acquire);
    return now_ms >= last ? now_ms - last <= kPlayerFreshMs : true;
}

float distance(const Vec3& a, const Vec3& b) noexcept {
    const float x = a[0] - b[0], y = a[1] - b[1], z = a[2] - b[2];
    return std::sqrt(x * x + y * y + z * z);
}

// Where a rewind goes: the last place noted kRewindMs ago or before, else
// the oldest one, if it's at least 2 s old.
const Sample* rewind_target(std::uint64_t now_ms) noexcept {
    const Sample* best = nullptr;
    for (const Sample& s : g_positions) {
        if (now_ms - s.at_ms < kRewindMs) break;
        best = &s;
    }
    if (!best && !g_positions.empty() && now_ms - g_positions.front().at_ms >= 2'000u) best = &g_positions.front();
    return best;
}

EnemyKind enemy_of(GameAction action) noexcept {
    return action == GameAction::SpawnBombomb ? EnemyKind::Bombomb : EnemyKind::Goomba;
}

std::int32_t item_of(GameAction action) noexcept {
    switch (action) {
    case GameAction::Rainbow: return kItemRainbowStar;
    case GameAction::PowerSpring: return kItemSpring;
    case GameAction::PowerBee: return kItemBee;
    case GameAction::PowerBoo: return kItemBoo;
    case GameAction::PowerIce: return kItemIce;
    case GameAction::PowerFire: return kItemFire;
    case GameAction::PowerFlying: return kItemFlying;
    default: return 0;
    }
}

// The harshest: held back for kTrollGraceMs after a respawn or a teleport.
bool needs_grace(GameAction action) noexcept {
    switch (action) {
    case GameAction::Damage:
    case GameAction::LifeDown:
    case GameAction::Death:
    case GameAction::BackToStart:
    case GameAction::Rewind:
    case GameAction::Launch: return true;
    default: return false;
    }
}

// Against values the caller read, so the link (atomics) and the game thread
// (live reads) apply the same rules.
bool useful(GameAction action, std::uint64_t now_ms, std::uint32_t health, std::uint32_t max_health,
            std::int32_t lives, std::int32_t star_bits, std::int32_t coins) noexcept {
    if (game_action_troll(action) && shield_active()) return false;
    if (needs_grace(action) && now_ms < g_grace_until_ms.load(std::memory_order_relaxed)) return false;
    const bool free = !g_in_rush.load(std::memory_order_relaxed);
    switch (action) {
    case GameAction::Heal: return health > 0 && health < max_health;
    case GameAction::Damage: return health > 1;
    case GameAction::LifeUp: return lives < 99;
    case GameAction::LifeDown: return lives > 1;
    case GameAction::StarBitsAdd: return star_bits < 9999;
    case GameAction::StarBitsRemove: return star_bits > 0;
    case GameAction::CoinsAdd: return coins < 999;
    case GameAction::CoinsRemove: return coins > 0;
    case GameAction::LifeMushroom: return max_health <= 3;
    case GameAction::CameraShake: return true;
    case GameAction::Death: return health > 0;
    case GameAction::Rainbow:
    case GameAction::PowerSpring:
    case GameAction::PowerBee:
    case GameAction::PowerBoo:
    case GameAction::PowerIce:
    case GameAction::PowerFire:
    case GameAction::PowerFlying:
        return (g_items_ready.load(std::memory_order_relaxed) >> item_of(action)) & 1u;
    case GameAction::BackToStart:
        return health > 0 && free && g_can_back_to_start.load(std::memory_order_relaxed);
    case GameAction::Rewind: return health > 0 && free && g_can_rewind.load(std::memory_order_relaxed);
    case GameAction::Launch: return health > 0 && free;
    case GameAction::RemovePower: return g_power_mode.load(std::memory_order_relaxed) != 0u;
    case GameAction::SpawnGoomba:
    case GameAction::SpawnBombomb:
        return health > 0 && free &&
               ((g_enemies_ready.load(std::memory_order_relaxed) >> static_cast<std::uint32_t>(enemy_of(action))) & 1u);
    case GameAction::Giant:
    case GameAction::Mini:
    case GameAction::SuperJump:
    case GameAction::MoonGravity:
    case GameAction::SuperSpeed:
    case GameAction::IceFloor:
    case GameAction::Slow:
    case GameAction::TinyJump: return true;
    case GameAction::Count: break;
    }
    return false;
}

void refresh_items_ready(PlayerAccess& player) noexcept {
    std::uint32_t ready = 0;
    for (const std::int32_t item :
         {kItemSpring, kItemBee, kItemBoo, kItemIce, kItemFire, kItemFlying, kItemRainbowStar})
        if (player.item_ready(item)) ready |= 1u << item;
    g_items_ready.store(ready, std::memory_order_relaxed);
    std::uint32_t enemies = 0;
    for (std::uint32_t kind = 0; kind < static_cast<std::uint32_t>(EnemyKind::Count); ++kind)
        if (player.enemy_ready(static_cast<EnemyKind>(kind))) enemies |= 1u << kind;
    g_enemies_ready.store(enemies, std::memory_order_relaxed);
}

// A new scene, a respawn or a teleport: the places noted before don't count
// (only a new scene or a respawn forget the start) and the harshest troll
// actions wait.
void start_over(std::uint64_t now_ms, bool forget_start) noexcept {
    g_positions.clear();
    g_last_sample_ms = 0;
    if (forget_start) g_has_start = false;
    g_grace_until_ms.store(now_ms + kTrollGraceMs, std::memory_order_relaxed);
}

// Notes where Mario stands and publishes what the teleports can do.
void track_position(PlayerAccess& player, std::uint64_t now_ms, std::uint32_t health) noexcept {
    const bool rush = player.in_rush();
    g_in_rush.store(rush, std::memory_order_relaxed);
    const Vec3 here = player.position();
    const bool ground = player.on_ground();
    g_on_ground.store(ground, std::memory_order_relaxed);
    if (health > 0 && !rush && (g_last_sample_ms == 0 || now_ms - g_last_sample_ms >= kPositionEveryMs) &&
        ground) {
        try {
            g_positions.push_back({now_ms, here});
        } catch (...) {
        }
        g_last_sample_ms = now_ms;
        if (!g_has_start) {
            g_start = here;
            g_has_start = true;
        }
    }
    while (!g_positions.empty() && now_ms - g_positions.front().at_ms > kRewindMs + 2'000u)
        g_positions.pop_front();
    const float to_start = g_has_start ? distance(here, g_start) : -1.0f;
    g_start_distance.store(to_start, std::memory_order_relaxed);
    g_can_back_to_start.store(to_start >= kMinTeleportDistance, std::memory_order_relaxed);
    const Sample* back = rewind_target(now_ms);
    const float to_back = back ? distance(here, back->position) : -1.0f;
    g_rewind_distance.store(to_back, std::memory_order_relaxed);
    g_can_rewind.store(to_back >= kMinTeleportDistance, std::memory_order_relaxed);
}

// Keeps the timed actions applied while they run and puts everything back
// once they end (or at once after a panic, when every `until` is 0).
void apply_timed(PlayerAccess& player, std::uint64_t now_ms) noexcept {
    const auto active = [now_ms](GameAction a) { return now_ms < g_timed_until[timed_index(a)]; };

    const bool giant = active(GameAction::Giant);
    const bool mini = active(GameAction::Mini);
    if (giant || mini) {
        if (!g_scale_saved) {
            g_saved_scale = player.scale();
            g_scale_saved = true;
        }
        // Both running: the one asked for last (it ends later) wins.
        const bool use_giant =
            giant && (!mini || g_timed_until[timed_index(GameAction::Giant)] >=
                                   g_timed_until[timed_index(GameAction::Mini)]);
        player.set_scale(use_giant ? kGiantScale : kMiniScale);
    } else if (g_scale_saved) {
        player.set_scale(g_saved_scale);
        g_scale_saved = false;
    }

    // Everything that ended is put back first, so an action that changes the
    // same floats saves the game's own values, not the ended one's.
    for (std::size_t i = 0; i < kTableChanges.size(); ++i) {
        const TableChange& change = kTableChanges[i];
        SavedTable& saved = g_saved_tables[i];
        if (active(change.action) || !saved.saved) continue;
        for (std::uint32_t f = 0; f < change.count; ++f)
            player.write_f32(saved.table + change.first + f * 4u, saved.values[f]);
        saved.saved = false;
    }
    for (std::size_t i = 0; i < kTableChanges.size(); ++i) {
        const TableChange& change = kTableChanges[i];
        SavedTable& saved = g_saved_tables[i];
        if (!active(change.action) || saved.saved) continue;
        const std::uint32_t table = player.const_table();
        if (table == 0u) continue;
        for (std::uint32_t f = 0; f < change.count; ++f) {
            const std::uint32_t address = table + change.first + f * 4u;
            saved.values[f] = player.read_f32(address);
            player.write_f32(address, change.source != 0u ? player.read_f32(table + change.source + f * 4u)
                                                          : saved.values[f] * change.factor);
        }
        saved.table = table;
        saved.saved = true;
    }
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
    if (name == "life_mushroom") return GameAction::LifeMushroom;
    if (name == "rainbow_star") return GameAction::Rainbow;
    if (name == "camera_shake") return GameAction::CameraShake;
    if (name == "death") return GameAction::Death;
    if (name == "power_spring") return GameAction::PowerSpring;
    if (name == "power_bee") return GameAction::PowerBee;
    if (name == "power_boo") return GameAction::PowerBoo;
    if (name == "power_ice") return GameAction::PowerIce;
    if (name == "power_fire") return GameAction::PowerFire;
    if (name == "power_flying") return GameAction::PowerFlying;
    if (name == "back_to_start") return GameAction::BackToStart;
    if (name == "rewind") return GameAction::Rewind;
    if (name == "launch") return GameAction::Launch;
    if (name == "remove_power") return GameAction::RemovePower;
    if (name == "spawn_goomba") return GameAction::SpawnGoomba;
    if (name == "spawn_bombomb") return GameAction::SpawnBombomb;
    if (name == "giant") return GameAction::Giant;
    if (name == "mini") return GameAction::Mini;
    if (name == "super_jump") return GameAction::SuperJump;
    if (name == "moon_gravity") return GameAction::MoonGravity;
    if (name == "super_speed") return GameAction::SuperSpeed;
    if (name == "ice_floor") return GameAction::IceFloor;
    if (name == "slow") return GameAction::Slow;
    if (name == "tiny_jump") return GameAction::TinyJump;
    return std::nullopt;
}

bool game_action_timed(GameAction action) noexcept {
    return action >= GameAction::Giant && action < GameAction::Count;
}

bool game_action_troll(GameAction action) noexcept {
    switch (action) {
    case GameAction::Damage:
    case GameAction::LifeDown:
    case GameAction::StarBitsRemove:
    case GameAction::CoinsRemove:
    case GameAction::CameraShake:
    case GameAction::Death:
    case GameAction::BackToStart:
    case GameAction::Rewind:
    case GameAction::Launch:
    case GameAction::RemovePower:
    case GameAction::SpawnGoomba:
    case GameAction::SpawnBombomb:
    case GameAction::Mini:
    case GameAction::IceFloor:
    case GameAction::Slow:
    case GameAction::TinyJump: return true;
    default: return false;
    }
}

bool game_action_applicable(GameAction action, std::uint64_t now_ms) noexcept {
    if (action >= GameAction::Count || !fresh(now_ms)) return false;
    return useful(action, now_ms, g_health.load(std::memory_order_relaxed),
                  g_max_health.load(std::memory_order_relaxed), g_lives.load(std::memory_order_relaxed),
                  g_star_bits.load(std::memory_order_relaxed), g_coins.load(std::memory_order_relaxed));
}

std::string game_debug_state(std::uint64_t now_ms) {
    const std::uint64_t grace = g_grace_until_ms.load(std::memory_order_relaxed);
    const auto whole = [](float v) { return std::to_string(static_cast<long>(std::lround(v))); };
    const float to_start = g_start_distance.load(std::memory_order_relaxed);
    const float to_back = g_rewind_distance.load(std::memory_order_relaxed);
    return std::string("fresh=") + (fresh(now_ms) ? "1" : "0") + " start=" + (to_start >= 0.0f ? "1" : "0") +
           " start_dist=" + whole(to_start) + " rewind_dist=" + whole(to_back) +
           " ground=" + (g_on_ground.load(std::memory_order_relaxed) ? "1" : "0") +
           " rush=" + (g_in_rush.load(std::memory_order_relaxed) ? "1" : "0") +
           " grace_ms=" + std::to_string(grace > now_ms ? grace - now_ms : 0u) +
           " power=" + std::to_string(g_power_mode.load(std::memory_order_relaxed)) +
           " enemies=" + std::to_string(g_enemies_ready.load(std::memory_order_relaxed)) +
           " hp=" + std::to_string(g_health.load(std::memory_order_relaxed)) +
           " shield=" + (shield_active() ? "1" : "0");
}

bool request_game_action(GameAction action, std::uint64_t now_ms, std::uint32_t duration_ms) noexcept {
    if (action >= GameAction::Count || paused()) return false;
    if (game_action_timed(action) && duration_ms == 0u) return false;
    try {
        const std::lock_guard<std::mutex> lock(g_mutex);
        if (g_queue.size() >= kGameQueueMax) return false;
        g_queue.push_back({action, now_ms, duration_ms});
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
    if (g_clear_requested.exchange(false, std::memory_order_relaxed)) {
        g_shake_until_ms = 0;
        g_timed_until.fill(0);
    }
    // Another stage or mission is a new scene; health 0 is a death, and him
    // having health again the respawn.
    const std::uint32_t scene = player.scene_id();
    if (g_seen_frame.load(std::memory_order_relaxed) && scene != g_scene) start_over(now_ms, true);
    g_scene = scene;
    if (health == 0) {
        g_positions.clear();
        g_has_start = false;
        g_was_dead = true;
    } else if (g_was_dead) {
        g_was_dead = false;
        start_over(now_ms, true);
    }
    refresh_items_ready(player);
    g_power_mode.store(player.power_mode(), std::memory_order_relaxed);
    track_position(player, now_ms, health);

    std::optional<Queued> next;
    if (!paused() && (!g_has_last_action || now_ms - g_last_action_ms >= kGameActionSpacingMs)) {
        try {
            const std::lock_guard<std::mutex> lock(g_mutex);
            while (!g_queue.empty() && now_ms - g_queue.front().at_ms > kGameActionTimeoutMs)
                g_queue.pop_front();
            if (!g_queue.empty()) {
                next = g_queue.front();
                g_queue.pop_front();
            }
        } catch (...) {
        }
    }
    // Checked again here: several queued actions may have used it up.
    if (next && useful(next->action, now_ms, health, max_health, lives, star_bits, coins)) {
        switch (next->action) {
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
        case GameAction::LifeMushroom: player.item_status(kItemLifeUp); break;
        case GameAction::CameraShake:
            g_shake_until_ms = now_ms + kCameraShakeMs;
            g_last_shake_ms = 0;
            break;
        case GameAction::Death: player.kill(); break;
        case GameAction::Rainbow:
        case GameAction::PowerSpring:
        case GameAction::PowerBee:
        case GameAction::PowerBoo:
        case GameAction::PowerIce:
        case GameAction::PowerFire:
        case GameAction::PowerFlying: player.item_status(item_of(next->action)); break;
        case GameAction::BackToStart:
            player.teleport(g_start);
            start_over(now_ms, false);
            break;
        case GameAction::Rewind:
            if (const Sample* back = rewind_target(now_ms)) {
                player.teleport(back->position);
                start_over(now_ms, false);
            }
            break;
        case GameAction::Launch: player.launch(kLaunchSpeed); break;
        case GameAction::RemovePower: player.item_status(kItemNormal); break;
        case GameAction::SpawnGoomba:
        case GameAction::SpawnBombomb: player.spawn_enemy(enemy_of(next->action)); break;
        case GameAction::Giant:
        case GameAction::Mini:
        case GameAction::SuperJump:
        case GameAction::MoonGravity:
        case GameAction::SuperSpeed:
        case GameAction::IceFloor:
        case GameAction::Slow:
        case GameAction::TinyJump: {
            for (const auto& pair : kOpposites) {
                if (pair[0] == next->action) g_timed_until[timed_index(pair[1])] = 0;
                if (pair[1] == next->action) g_timed_until[timed_index(pair[0])] = 0;
            }
            // A repeat extends from whichever ends later: now or the current run.
            auto& until = g_timed_until[timed_index(next->action)];
            until = std::max(until, now_ms) + std::min<std::uint64_t>(next->duration_ms, kMaxTimedMs);
            until = std::min(until, now_ms + kMaxTimedMs);
            break;
        }
        case GameAction::Count: break;
        }
        g_last_action_ms = now_ms;
        g_has_last_action = true;
        health = player.health();
        max_health = player.max_health();
        lives = player.lives();
        star_bits = player.star_bits();
        coins = player.coins();
    }
    apply_timed(player, now_ms);

    // A shake lasts a fraction of a second: repeated while the effect runs.
    if (now_ms < g_shake_until_ms && !paused() &&
        (g_last_shake_ms == 0 || now_ms - g_last_shake_ms >= kCameraShakeEveryMs)) {
        player.shake_camera();
        g_last_shake_ms = now_ms;
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
    g_clear_requested.store(true, std::memory_order_relaxed);
}

void reset_game_for_tests() noexcept {
    try {
        const std::lock_guard<std::mutex> lock(g_mutex);
        g_queue.clear();
    } catch (...) {
    }
    g_clear_requested.store(false);
    g_seen_frame.store(false);
    g_last_frame_ms.store(0);
    g_health.store(0);
    g_max_health.store(0);
    g_lives.store(0);
    g_star_bits.store(0);
    g_coins.store(0);
    g_items_ready.store(0);
    g_power_mode.store(0);
    g_enemies_ready.store(0);
    g_in_rush.store(false);
    g_can_back_to_start.store(false);
    g_can_rewind.store(false);
    g_grace_until_ms.store(0);
    g_start_distance.store(-1.0f);
    g_rewind_distance.store(-1.0f);
    g_on_ground.store(false);
    g_has_last_action = false;
    g_last_action_ms = 0;
    g_shake_until_ms = 0;
    g_last_shake_ms = 0;
    g_timed_until.fill(0);
    g_scale_saved = false;
    g_saved_scale = 1.0f;
    g_saved_tables = {};
    g_positions.clear();
    g_has_start = false;
    g_start = {};
    g_last_sample_ms = 0;
    g_was_dead = false;
    g_scene = 0;
}

}  // namespace galaxy::cloud90
