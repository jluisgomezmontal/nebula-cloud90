#include "galaxy/cloud90_effects.h"
#include "galaxy/cloud90_link.h"
#include "galaxy/native_input.h"

#include <chrono>
#include <cstdlib>
#include <iostream>
#include <string>
#include <thread>

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <Windows.h>

namespace {

bool expect(bool condition, const char* message) {
    if (!condition) std::cerr << "FAILED: " << message << '\n';
    return condition;
}

bool test_reads_the_servers_effect_message() {
    using namespace galaxy::cloud90;
    const std::string msg =
        R"({"type":"effect","id":"3f1c-9a","effect":"invert_controls","ms":15000,"by":"Ana \"la\" grande"})";
    bool ok = expect(json_string_field(msg, "type") == "effect", "type");
    ok &= expect(json_string_field(msg, "effect") == "invert_controls", "effect");
    ok &= expect(json_number_field(msg, "ms", 0) == 15000, "ms");
    ok &= expect(json_string_field(msg, "by") == "Ana \"la\" grande", "escaped quotes");
    ok &= expect(json_string_field(msg, "nope").empty(), "missing string");
    ok &= expect(json_number_field(msg, "nope", -1) == -1, "missing number");
    ok &= expect(json_bool_field(R"({"type":"pause","paused":false})", "paused", true) == false, "bool");
    return ok;
}

bool test_effect_names_match_the_backend_catalog() {
    using namespace galaxy::cloud90;
    bool ok = true;
    for (const char* name : {"invert_controls", "auto_jump", "no_jump", "drunk_stick", "force_spin", "auto_run",
                             "flip_screen", "mirror_screen", "grayscale", "pixelate", "wobble_screen",
                             "invert_colors", "tunnel_vision"})
        ok &= expect(effect_from_name(name).has_value(), name);
    ok &= expect(!effect_from_name("explode").has_value(), "unknown effect");
    return ok;
}

bool test_token_is_stored_sealed() {
    using namespace galaxy::cloud90;
    bool ok = expect(unprotect_token(protect_token("c90gl_abcdefghijklmnopqrst")) == "c90gl_abcdefghijklmnopqrst",
                     "DPAPI round trip");
    ok &= expect(unprotect_token("not base64 at all").empty(), "garbage stays empty");
    // A private LOCALAPPDATA so the real token is never touched.
    wchar_t tmp[MAX_PATH]{};
    GetTempPathW(MAX_PATH, tmp);
    const std::wstring dir = std::wstring(tmp) + L"cloud90-token-test";
    CreateDirectoryW(dir.c_str(), nullptr);
    SetEnvironmentVariableW(L"LOCALAPPDATA", dir.c_str());
    ok &= expect(!save_token("hola"), "rejects a non-token");
    ok &= expect(save_token("  c90gl_abcdefghijklmnopWXYZ  "), "saves a token");
    ok &= expect(token_hint() == "C90GL_....WXYZ", "hint shows the last four");
    std::string file;
    if (FILE* in = _wfopen((dir + L"\\Nebula\\cloud90-link.ini").c_str(), L"rb")) {
        char buf[4096];
        file.assign(buf, fread(buf, 1, sizeof buf, in));
        fclose(in);
    }
    ok &= expect(file.find("token_protected=") == 0, "stored sealed");
    ok &= expect(file.find("c90gl_") == std::string::npos, "never in plain text");
    ok &= expect(clear_token() && token_hint() == "NOT SET", "removed");
    return ok;
}

// --live: against a server on localhost (tools/cloud90/fake-server.mjs),
// with LOCALAPPDATA pointing at a folder holding its cloud90-link.ini.
bool live_round_trip() {
    using namespace galaxy::cloud90;
    start_link();
    for (int i = 0; i < 100; ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        // The fake server sends invert_controls for 5 s right after hello.
        galaxy::input::WiimoteInputSnapshot snap{};
        apply_input_effects(snap, 1000u + static_cast<std::uint64_t>(i), true);
        if (remaining_vi(Effect::InvertControls, 1000u + static_cast<std::uint64_t>(i)) > 0) {
            std::cout << "live: effect received (" << link_status() << ")\n";
            std::this_thread::sleep_for(std::chrono::milliseconds(500));  // let the ack go out
            return true;
        }
    }
    std::cerr << "live: no effect; status: " << link_status() << '\n';
    return false;
}

}  // namespace

int main(int argc, char** argv) {
    if (argc > 1 && std::string(argv[1]) == "--live") return live_round_trip() ? 0 : 1;
    // --unseal <base64>: what set-token.ps1 sealed, as the runtime reads it.
    if (argc > 2 && std::string(argv[1]) == "--unseal") {
        std::cout << galaxy::cloud90::unprotect_token(argv[2]) << '\n';
        return 0;
    }
    bool ok = true;
    ok &= test_reads_the_servers_effect_message();
    ok &= test_effect_names_match_the_backend_catalog();
    ok &= test_token_is_stored_sealed();
    if (ok) std::cout << "cloud90 link: all tests passed\n";
    return ok ? 0 : 1;
}
