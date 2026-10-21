#pragma once

#include <cstdint>
#include <optional>
#include <string_view>

// Cloud90 Game Link phase 5 (PLAN-GAME-LINK.md): live-stream events change
// the game's own state (Mario's health, lives, star bits, size, physics,
// power-ups). They touch guest memory, so they only run on the game's
// thread, from the Cloud90Player site hook: once per frame while Mario can
// move (never in a menu, a cutscene or a scene change). Only Death kills
// Mario; nothing else takes his last health point or life.
namespace galaxy::cloud90 {

enum class GameAction : std::uint8_t {
    Heal,            // +1 health
    Damage,          // -1 health, never below 1
    LifeUp,          // +1 life (1-up)
    LifeDown,        // -1 life, never below 1
    StarBitsAdd,     // +kStarBitsStep
    StarBitsRemove,  // -kStarBitsStep, never below 0
    CoinsAdd,        // +kCoinsStep (heals and gives 1-ups as picked-up coins do)
    CoinsRemove,     // -kCoinsStep, never below 0
    LifeMushroom,    // the Life Up mushroom: 6 health instead of 3
    Rainbow,         // the Rainbow Star: invincible for the game's own time
    CameraShake,     // the game's strong camera shake, for kCameraShakeMs
    Death,           // Mario dies as from a hit (a life is lost)
    PowerSpring,     // the game's power-ups (until Mario is hit, as in the game)
    PowerBee,
    PowerBoo,
    PowerIce,
    PowerFire,
    PowerFlying,
    // For the duration Cloud90 sends, then put back.
    Giant,           // Mario at kGiantScale
    Mini,            // Mario at kMiniScale
    SuperJump,       // jump heights x kSuperJumpFactor
    MoonGravity,     // gravity while jumping x kMoonGravityFactor
    SuperSpeed,      // walking/running speed x kSuperSpeedFactor
    Count,
};

inline constexpr std::int32_t kStarBitsStep = 10;
inline constexpr std::int32_t kCoinsStep = 10;
inline constexpr std::uint64_t kCameraShakeMs = 3000u;
inline constexpr std::uint64_t kCameraShakeEveryMs = 500u;
inline constexpr float kGiantScale = 2.0f;
inline constexpr float kMiniScale = 0.45f;
inline constexpr float kSuperJumpFactor = 1.6f;
inline constexpr float kMoonGravityFactor = 0.35f;
inline constexpr float kSuperSpeedFactor = 1.8f;
// Longest a timed action may last, whatever a request asks for.
inline constexpr std::uint64_t kMaxTimedMs = 60'000u;
// A queued action that can't run this long (Mario not controllable) is dropped.
inline constexpr std::uint64_t kGameActionTimeoutMs = 10'000u;
// Mario's update counts as "now" this long after it last ran.
inline constexpr std::uint64_t kPlayerFreshMs = 500u;
// At most one action this often, so each one is seen and heard.
inline constexpr std::uint64_t kGameActionSpacingMs = 300u;
inline constexpr std::size_t kGameQueueMax = 16u;

// MR::changePlayerItemStatus: what the game's items do.
inline constexpr std::int32_t kItemSpring = 1;
inline constexpr std::int32_t kItemBee = 2;
inline constexpr std::int32_t kItemBoo = 3;
inline constexpr std::int32_t kItemIce = 4;
inline constexpr std::int32_t kItemFire = 5;
inline constexpr std::int32_t kItemFlying = 6;
inline constexpr std::int32_t kItemRainbowStar = 7;
inline constexpr std::int32_t kItemLifeUp = 10;

// What the game thread reads and changes. The site hook implements it with
// guest calls; tests use a fake.
class PlayerAccess {
public:
    virtual ~PlayerAccess() = default;
    virtual std::uint32_t health() = 0;
    virtual std::uint32_t max_health() = 0;
    virtual std::int32_t lives() = 0;
    virtual std::int32_t star_bits() = 0;
    virtual std::int32_t coins() = 0;
    virtual void heal(std::uint32_t amount) = 0;
    virtual void damage() = 0;
    virtual void add_lives(std::int32_t delta) = 0;
    virtual void add_star_bits(std::int32_t delta) = 0;
    virtual void add_coins(std::int32_t delta) = 0;
    virtual void item_status(std::int32_t status) = 0;
    // Whether the item's models are loaded: power-ups only exist in the
    // galaxies that have them, and the game crashes without them.
    virtual bool item_ready(std::int32_t status) = 0;
    virtual void shake_camera() = 0;
    virtual void kill() = 0;
    // Mario's size (LiveActor::mScale, the same on every axis).
    virtual float scale() = 0;
    virtual void set_scale(float scale) = 0;
    // Mario's current constants table (MarioConstTable), or 0, and its floats.
    virtual std::uint32_t const_table() = 0;
    virtual float read_f32(std::uint32_t address) = 0;
    virtual void write_f32(std::uint32_t address, float value) = 0;
};

// The id Cloud90 sends ("heal"…) → the action, if known.
[[nodiscard]] std::optional<GameAction> game_action_from_name(std::string_view name) noexcept;

// Whether the action lasts the duration Cloud90 sends (Giant…SuperSpeed).
[[nodiscard]] bool game_action_timed(GameAction action) noexcept;

// Any thread. Whether the action would do something now: Mario can move and
// it isn't pointless (full health, last life, no star bits…) or impossible
// (a power-up this galaxy doesn't have).
[[nodiscard]] bool game_action_applicable(GameAction action, std::uint64_t now_ms) noexcept;

// Any thread. Queues the action for the game thread (`duration_ms` for the
// timed ones, clamped to kMaxTimedMs); false while paused or with the queue
// full.
bool request_game_action(GameAction action, std::uint64_t now_ms, std::uint32_t duration_ms = 0u) noexcept;

// Game thread, once per frame from the site hook: refreshes what
// game_action_applicable sees, runs at most one queued action and keeps the
// timed ones applied (or puts things back when they end).
void on_player_frame(PlayerAccess& player, std::uint64_t now_ms) noexcept;

// Panic / pause: forget queued actions and end the timed ones.
void clear_game_actions() noexcept;

// Tests only.
void reset_game_for_tests() noexcept;

}  // namespace galaxy::cloud90
