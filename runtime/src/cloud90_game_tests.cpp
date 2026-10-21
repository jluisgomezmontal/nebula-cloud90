#include "galaxy/cloud90_effects.h"
#include "galaxy/cloud90_game.h"

#include <algorithm>
#include <array>
#include <cmath>
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
    int item = -1, shakes = 0;
    void item_status(std::int32_t status) override {
        ++calls;
        item = status;
        if (status == galaxy::cloud90::kItemLifeUp) max_hp = 6;
    }
    void shake_camera() override { ++shakes; }
    std::uint32_t ready_items = 0;  // bit n: item n's models are loaded
    bool item_ready(std::int32_t status) override { return (ready_items >> status) & 1u; }
    bool dead = false;
    void kill() override { ++calls; dead = true; hp = 0; }
    float mario_scale = 1.0f;
    float scale() override { return mario_scale; }
    void set_scale(float s) override { mario_scale = s; }
    // A table of 0x240 bytes at guest address 0x80001000: float i is 1 + i.
    std::array<float, 0x240 / 4> table{};
    FakePlayer() { for (std::size_t i = 0; i < table.size(); ++i) table[i] = 1.0f + static_cast<float>(i); }
    std::uint32_t const_table() override { return 0x80001000u; }
    float read_f32(std::uint32_t a) override { return table[(a - 0x80001000u) / 4u]; }
    void write_f32(std::uint32_t a, float v) override { table[(a - 0x80001000u) / 4u] = v; }
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
    bool ok = true;
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
    p.ready_items = 1u << galaxy::cloud90::kItemRainbowStar;
    galaxy::cloud90::on_player_frame(p, 6400);
    galaxy::cloud90::request_game_action(GameAction::Rainbow, 6400);
    galaxy::cloud90::on_player_frame(p, 6800);
    ok &= expect(p.item == galaxy::cloud90::kItemRainbowStar, "rainbow where the galaxy has it");
    return ok;
}

bool near(float a, float b) { return std::fabs(a - b) < 1e-4f; }

bool test_timed_actions_are_put_back() {
    reset();
    FakePlayer p;
    bool ok = expect(!galaxy::cloud90::request_game_action(GameAction::Giant, 0), "timed needs a duration");
    galaxy::cloud90::request_game_action(GameAction::Giant, 0, 1000);
    galaxy::cloud90::on_player_frame(p, 0);
    ok &= expect(near(p.mario_scale, galaxy::cloud90::kGiantScale), "giant");
    galaxy::cloud90::request_game_action(GameAction::SuperJump, 0, 1000);
    galaxy::cloud90::on_player_frame(p, 300);
    ok &= expect(near(p.table[0], 1.0f * galaxy::cloud90::kSuperJumpFactor) &&
                     near(p.table[8], 9.0f * galaxy::cloud90::kSuperJumpFactor) && near(p.table[9], 10.0f),
                 "jump heights only");
    galaxy::cloud90::on_player_frame(p, 600);
    ok &= expect(near(p.table[0], 1.0f * galaxy::cloud90::kSuperJumpFactor), "scaled once, not every frame");
    galaxy::cloud90::on_player_frame(p, 1001);
    ok &= expect(near(p.mario_scale, 1.0f), "giant over: size back");
    galaxy::cloud90::on_player_frame(p, 1301);
    ok &= expect(near(p.table[0], 1.0f) && near(p.table[8], 9.0f), "jump over: table back");
    galaxy::cloud90::request_game_action(GameAction::SuperSpeed, 2000, 5000);
    galaxy::cloud90::on_player_frame(p, 2000);
    ok &= expect(near(p.table[0x224 / 4], (1.0f + 0x224 / 4) * galaxy::cloud90::kSuperSpeedFactor), "speed");
    galaxy::cloud90::panic();
    galaxy::cloud90::set_paused(false);
    galaxy::cloud90::on_player_frame(p, 2100);
    ok &= expect(near(p.table[0x224 / 4], 1.0f + 0x224 / 4), "panic puts it back at once");
    return ok;
}

bool test_death_and_power_ups() {
    reset();
    FakePlayer p;
    galaxy::cloud90::on_player_frame(p, 0);
    bool ok = expect(!galaxy::cloud90::game_action_applicable(GameAction::PowerBee, 10), "no bee here");
    p.ready_items = 1u << galaxy::cloud90::kItemBee;
    galaxy::cloud90::on_player_frame(p, 20);
    ok &= expect(galaxy::cloud90::game_action_applicable(GameAction::PowerBee, 30), "bee galaxy");
    galaxy::cloud90::request_game_action(GameAction::PowerBee, 30);
    galaxy::cloud90::on_player_frame(p, 40);
    ok &= expect(p.item == galaxy::cloud90::kItemBee, "bee given");
    galaxy::cloud90::request_game_action(GameAction::Death, 40);
    galaxy::cloud90::on_player_frame(p, 400);
    ok &= expect(p.dead, "death");
    ok &= expect(!galaxy::cloud90::game_action_applicable(GameAction::Death, 410), "not twice");
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
    ok &= test_timed_actions_are_put_back();
    ok &= test_death_and_power_ups();
    ok &= test_old_requests_expire();
    ok &= test_panic_clears_and_pause_refuses();
    ok &= test_queue_is_bounded();
    if (ok) std::cout << "cloud90 game tests passed\n";
    return ok ? 0 : 1;
}
