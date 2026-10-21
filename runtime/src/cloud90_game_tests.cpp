#include "galaxy/cloud90_effects.h"
#include "galaxy/cloud90_game.h"

#include <algorithm>
#include <iostream>

namespace {

using galaxy::cloud90::GameAction;

bool expect(bool condition, const char* message) {
    if (!condition) std::cerr << "FAILED: " << message << '\n';
    return condition;
}

class FakePlayer final : public galaxy::cloud90::PlayerAccess {
public:
    std::uint32_t hp = 3, max_hp = 3;
    std::int32_t left = 4, bits = 25, money = 5;
    int calls = 0;

    std::uint32_t health() override { return hp; }
    std::uint32_t max_health() override { return max_hp; }
    std::int32_t lives() override { return left; }
    std::int32_t star_bits() override { return bits; }
    std::int32_t coins() override { return money; }
    void heal(std::uint32_t amount) override { ++calls; hp = std::min(hp + amount, max_hp); }
    void damage() override { ++calls; if (hp) --hp; }
    void add_lives(std::int32_t delta) override { ++calls; left += delta; }
    void add_star_bits(std::int32_t delta) override { ++calls; bits += delta; }
    void add_coins(std::int32_t delta) override { ++calls; money += delta; }
    int rained = 0, item = -1, shakes = 0;
    void coin_rain(std::int32_t count) override { ++calls; rained += count; }
    void item_status(std::int32_t status) override {
        ++calls;
        item = status;
        if (status == galaxy::cloud90::kItemLifeUp) max_hp = 6;
    }
    void shake_camera() override { ++shakes; }
    bool rainbow = false;
    bool rainbow_ready() override { return rainbow; }
};

void reset() {
    galaxy::cloud90::reset_game_for_tests();
    galaxy::cloud90::set_paused(false);
}

bool test_names() {
    bool ok = expect(galaxy::cloud90::game_action_from_name("heal") == GameAction::Heal, "heal");
    ok &= expect(galaxy::cloud90::game_action_from_name("starbits_remove") == GameAction::StarBitsRemove,
                 "starbits_remove");
    ok &= expect(!galaxy::cloud90::game_action_from_name("invert_controls"), "effects are not actions");
    return ok;
}

bool test_not_applicable_until_mario_moves() {
    reset();
    FakePlayer p;
    bool ok = expect(!galaxy::cloud90::game_action_applicable(GameAction::Damage, 1000), "no frame yet");
    galaxy::cloud90::on_player_frame(p, 1000);
    ok &= expect(galaxy::cloud90::game_action_applicable(GameAction::Damage, 1200), "fresh frame");
    ok &= expect(!galaxy::cloud90::game_action_applicable(GameAction::Damage, 1000 + 600),
                 "stale after a cutscene or a menu");
    return ok;
}

bool test_runs_one_action_per_spacing() {
    reset();
    FakePlayer p;
    galaxy::cloud90::request_game_action(GameAction::StarBitsAdd, 0);
    galaxy::cloud90::request_game_action(GameAction::LifeUp, 0);
    galaxy::cloud90::on_player_frame(p, 100);
    bool ok = expect(p.bits == 35 && p.left == 4, "first action only");
    galaxy::cloud90::on_player_frame(p, 116);
    ok &= expect(p.left == 4, "waits for the spacing");
    galaxy::cloud90::on_player_frame(p, 100 + galaxy::cloud90::kGameActionSpacingMs);
    ok &= expect(p.left == 5, "then the next");
    return ok;
}

bool test_never_kills_nor_takes_the_last_life() {
    reset();
    FakePlayer p;
    p.hp = 1;
    p.left = 1;
    galaxy::cloud90::on_player_frame(p, 0);
    bool ok = expect(!galaxy::cloud90::game_action_applicable(GameAction::Damage, 10), "damage at 1 hp refused");
    ok &= expect(!galaxy::cloud90::game_action_applicable(GameAction::LifeDown, 10), "last life refused");
    // Queued while it was still possible: checked again when it runs.
    galaxy::cloud90::request_game_action(GameAction::Damage, 10);
    galaxy::cloud90::on_player_frame(p, 20);
    galaxy::cloud90::request_game_action(GameAction::LifeDown, 20);
    galaxy::cloud90::on_player_frame(p, 20 + galaxy::cloud90::kGameActionSpacingMs);
    ok &= expect(p.hp == 1 && p.left == 1 && p.calls == 0, "nothing applied");
    return ok;
}

bool test_star_bits_never_negative() {
    reset();
    FakePlayer p;
    p.bits = 4;
    galaxy::cloud90::request_game_action(GameAction::StarBitsRemove, 0);
    galaxy::cloud90::on_player_frame(p, 0);
    return expect(p.bits == 0, "takes only what there is");
}

bool test_coins() {
    reset();
    FakePlayer p;
    galaxy::cloud90::request_game_action(GameAction::CoinsAdd, 0);
    galaxy::cloud90::on_player_frame(p, 0);
    bool ok = expect(p.money == 15, "+10 coins");
    galaxy::cloud90::request_game_action(GameAction::CoinsRemove, 0);
    galaxy::cloud90::on_player_frame(p, galaxy::cloud90::kGameActionSpacingMs);
    galaxy::cloud90::request_game_action(GameAction::CoinsRemove, 0);
    galaxy::cloud90::on_player_frame(p, 2 * galaxy::cloud90::kGameActionSpacingMs);
    ok &= expect(p.money == 0, "never below 0");
    ok &= expect(!galaxy::cloud90::game_action_applicable(GameAction::CoinsRemove, 2 * galaxy::cloud90::kGameActionSpacingMs),
                 "nothing to take");
    ok &= expect(galaxy::cloud90::game_action_from_name("coins_remove") == GameAction::CoinsRemove, "name");
    return ok;
}

bool test_batch_two() {
    reset();
    FakePlayer p;
    galaxy::cloud90::request_game_action(GameAction::CoinRain, 0);
    galaxy::cloud90::on_player_frame(p, 0);
    bool ok = expect(p.rained == galaxy::cloud90::kCoinRainCount, "coin rain");
    galaxy::cloud90::request_game_action(GameAction::LifeMushroom, 0);
    galaxy::cloud90::on_player_frame(p, 300);
    ok &= expect(p.item == galaxy::cloud90::kItemLifeUp && p.max_hp == 6, "life mushroom");
    ok &= expect(!galaxy::cloud90::game_action_applicable(GameAction::LifeMushroom, 310), "already 6");
    galaxy::cloud90::request_game_action(GameAction::CameraShake, 310);
    for (std::uint64_t t = 600; t <= 600 + galaxy::cloud90::kCameraShakeMs + 1000; t += 100)
        galaxy::cloud90::on_player_frame(p, t);
    ok &= expect(p.shakes == 6, "a shake every 500 ms for 3 s");
    galaxy::cloud90::request_game_action(GameAction::CameraShake, 5000);
    galaxy::cloud90::on_player_frame(p, 5000);
    galaxy::cloud90::panic();
    galaxy::cloud90::set_paused(false);
    const int before = p.shakes;
    galaxy::cloud90::on_player_frame(p, 5600);
    ok &= expect(p.shakes == before, "panic ends a shake");
    ok &= expect(galaxy::cloud90::game_action_from_name("rainbow_star") == GameAction::Rainbow, "name");
    galaxy::cloud90::request_game_action(GameAction::Rainbow, 6000);
    galaxy::cloud90::on_player_frame(p, 6000);
    ok &= expect(p.item != galaxy::cloud90::kItemRainbowStar, "no rainbow without its models");
    ok &= expect(!galaxy::cloud90::game_action_applicable(GameAction::Rainbow, 6010), "refused");
    p.rainbow = true;
    galaxy::cloud90::on_player_frame(p, 6400);
    galaxy::cloud90::request_game_action(GameAction::Rainbow, 6400);
    galaxy::cloud90::on_player_frame(p, 6800);
    ok &= expect(p.item == galaxy::cloud90::kItemRainbowStar, "rainbow where the galaxy has it");
    return ok;
}

bool test_heal_only_when_hurt() {
    reset();
    FakePlayer p;
    galaxy::cloud90::on_player_frame(p, 0);
    bool ok = expect(!galaxy::cloud90::game_action_applicable(GameAction::Heal, 10), "full health");
    p.hp = 2;
    galaxy::cloud90::on_player_frame(p, 20);
    ok &= expect(galaxy::cloud90::game_action_applicable(GameAction::Heal, 30), "hurt");
    galaxy::cloud90::request_game_action(GameAction::Heal, 30);
    galaxy::cloud90::on_player_frame(p, 40);
    ok &= expect(p.hp == 3, "healed");
    return ok;
}

bool test_old_requests_expire() {
    reset();
    FakePlayer p;
    galaxy::cloud90::request_game_action(GameAction::LifeUp, 0);
    galaxy::cloud90::on_player_frame(p, galaxy::cloud90::kGameActionTimeoutMs + 1);
    return expect(p.left == 4, "dropped after waiting too long");
}

bool test_panic_clears_and_pause_refuses() {
    reset();
    FakePlayer p;
    galaxy::cloud90::request_game_action(GameAction::LifeUp, 0);
    galaxy::cloud90::panic();
    bool ok = expect(!galaxy::cloud90::request_game_action(GameAction::LifeUp, 0), "refused while paused");
    galaxy::cloud90::set_paused(false);
    galaxy::cloud90::on_player_frame(p, 10);
    ok &= expect(p.left == 4, "panic forgot the queue");
    return ok;
}

bool test_queue_is_bounded() {
    reset();
    for (std::size_t i = 0; i < galaxy::cloud90::kGameQueueMax; ++i)
        galaxy::cloud90::request_game_action(GameAction::StarBitsAdd, 0);
    return expect(!galaxy::cloud90::request_game_action(GameAction::StarBitsAdd, 0), "full queue refuses");
}

}  // namespace

int main() {
    bool ok = true;
    ok &= test_names();
    ok &= test_not_applicable_until_mario_moves();
    ok &= test_runs_one_action_per_spacing();
    ok &= test_never_kills_nor_takes_the_last_life();
    ok &= test_star_bits_never_negative();
    ok &= test_heal_only_when_hurt();
    ok &= test_coins();
    ok &= test_batch_two();
    ok &= test_old_requests_expire();
    ok &= test_panic_clears_and_pause_refuses();
    ok &= test_queue_is_bounded();
    if (ok) std::cout << "cloud90 game tests passed\n";
    return ok ? 0 : 1;
}
