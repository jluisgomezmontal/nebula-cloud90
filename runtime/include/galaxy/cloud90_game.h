#pragma once

#include <cstdint>
#include <optional>
#include <string_view>

// Cloud90 Game Link phase 5 (PLAN-GAME-LINK.md): live-stream events change
// the game's own state (Mario's health, lives, star bits). Unlike the input
// and screen effects these are instant and touch guest memory, so they only
// run on the game's thread, from the Cloud90Player site hook: once per frame
// while Mario can move (never in a menu, a cutscene or a scene change). They
// never kill Mario nor take his last life.
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
    CoinRain,        // kCoinRainCount real coins pop out around Mario
    LifeMushroom,    // the Life Up mushroom: 6 health instead of 3
    Rainbow,         // the Rainbow Star: invincible for the game's own time
    CameraShake,     // the game's strong camera shake, for kCameraShakeMs
    Count,
};

inline constexpr std::int32_t kStarBitsStep = 10;
inline constexpr std::int32_t kCoinsStep = 10;
inline constexpr std::int32_t kCoinRainCount = 10;
inline constexpr std::uint64_t kCameraShakeMs = 3000u;
inline constexpr std::uint64_t kCameraShakeEveryMs = 500u;
// A queued action that can't run this long (Mario not controllable) is dropped.
inline constexpr std::uint64_t kGameActionTimeoutMs = 10'000u;
// Mario's update counts as "now" this long after it last ran.
inline constexpr std::uint64_t kPlayerFreshMs = 500u;
// At most one action this often, so each one is seen and heard.
inline constexpr std::uint64_t kGameActionSpacingMs = 300u;
inline constexpr std::size_t kGameQueueMax = 16u;

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
    virtual void coin_rain(std::int32_t count) = 0;
    // MR::changePlayerItemStatus: what the game's items do (7 Rainbow Star,
    // 10 Life Up mushroom).
    virtual void item_status(std::int32_t status) = 0;
    virtual void shake_camera() = 0;
};

inline constexpr std::int32_t kItemRainbowStar = 7;
inline constexpr std::int32_t kItemLifeUp = 10;

// The id Cloud90 sends ("heal"…) → the action, if known.
[[nodiscard]] std::optional<GameAction> game_action_from_name(std::string_view name) noexcept;

// Any thread. Whether the action would do something now: Mario can move and
// it isn't pointless (full health, last life, no star bits…).
[[nodiscard]] bool game_action_applicable(GameAction action, std::uint64_t now_ms) noexcept;

// Any thread. Queues the action for the game thread; false while paused or
// with the queue full.
bool request_game_action(GameAction action, std::uint64_t now_ms) noexcept;

// Game thread, once per frame from the site hook: refreshes what
// game_action_applicable sees and runs at most one queued action.
void on_player_frame(PlayerAccess& player, std::uint64_t now_ms) noexcept;

// Panic / pause: forget queued actions.
void clear_game_actions() noexcept;

// Tests only.
void reset_game_for_tests() noexcept;

}  // namespace galaxy::cloud90
