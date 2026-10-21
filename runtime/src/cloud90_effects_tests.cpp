#include "galaxy/cloud90_effects.h"
#include "galaxy/native_input.h"

#include <iostream>

namespace {

using galaxy::cloud90::Effect;
using galaxy::input::WiimoteInputSnapshot;

bool expect(bool condition, const char* message) {
    if (!condition) std::cerr << "FAILED: " << message << '\n';
    return condition;
}

WiimoteInputSnapshot stick(std::uint8_t x, std::uint8_t y) {
    WiimoteInputSnapshot s{};
    s.nunchuk.stick_x = x;
    s.nunchuk.stick_y = y;
    return s;
}

bool test_invert_mirrors_the_stick_while_active() {
    galaxy::cloud90::reset_for_tests();
    galaxy::cloud90::enqueue(Effect::InvertControls, 1000);  // 60 VIs
    auto s = stick(0x7b + 50, 0x81 - 20);
    galaxy::cloud90::apply_input_effects(s, 100, true);
    bool ok = expect(s.nunchuk.stick_x == 0x7b - 50, "x mirrored");
    ok &= expect(s.nunchuk.stick_y == 0x81 + 20, "y mirrored");
    auto later = stick(0x7b + 50, 0x81);
    galaxy::cloud90::apply_input_effects(later, 161, true);
    ok &= expect(later.nunchuk.stick_x == 0x7b + 50, "over after its time");
    return ok;
}

bool test_menus_freeze_the_timer_and_the_report() {
    galaxy::cloud90::reset_for_tests();
    galaxy::cloud90::enqueue(Effect::InvertControls, 1000);
    auto s = stick(0x7b + 50, 0x81);
    galaxy::cloud90::apply_input_effects(s, 100, true);  // starts: ends at 160
    auto menu = stick(0x7b + 50, 0x81);
    galaxy::cloud90::apply_input_effects(menu, 300, false);  // 200 VIs in a menu
    bool ok = expect(menu.nunchuk.stick_x == 0x7b + 50, "menu input untouched");
    auto back = stick(0x7b + 50, 0x81);
    galaxy::cloud90::apply_input_effects(back, 330, true);
    ok &= expect(back.nunchuk.stick_x == 0x7b - 50, "still on after the menu");
    return ok;
}

bool test_no_jump_beats_auto_jump_and_panic_clears() {
    galaxy::cloud90::reset_for_tests();
    galaxy::cloud90::enqueue(Effect::AutoJump, 5000);
    galaxy::cloud90::enqueue(Effect::NoJump, 5000);
    WiimoteInputSnapshot s{};
    s.buttons.a = true;
    galaxy::cloud90::apply_input_effects(s, 0, true);
    bool ok = expect(!s.buttons.a, "no jump wins");
    galaxy::cloud90::panic();
    WiimoteInputSnapshot after{};
    after.buttons.a = true;
    galaxy::cloud90::apply_input_effects(after, 1, true);
    ok &= expect(after.buttons.a, "panic ends every effect");
    galaxy::cloud90::enqueue(Effect::NoJump, 5000);
    ok &= expect(galaxy::cloud90::remaining_vi(Effect::NoJump, 2) == 0, "paused: nothing queued");
    return ok;
}

bool test_durations_are_clamped() {
    galaxy::cloud90::reset_for_tests();
    galaxy::cloud90::enqueue(Effect::DrunkStick, 10'000'000);
    WiimoteInputSnapshot s{};
    galaxy::cloud90::apply_input_effects(s, 0, true);
    return expect(galaxy::cloud90::remaining_vi(Effect::DrunkStick, 0) == 3600, "max 60 s");
}

}  // namespace

int main() {
    bool ok = true;
    ok &= test_invert_mirrors_the_stick_while_active();
    ok &= test_menus_freeze_the_timer_and_the_report();
    ok &= test_no_jump_beats_auto_jump_and_panic_clears();
    ok &= test_durations_are_clamped();
    if (ok) std::cout << "cloud90 effects: all tests passed\n";
    return ok ? 0 : 1;
}
