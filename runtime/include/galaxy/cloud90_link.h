#pragma once

#include <string>

// Cloud90 Game Link (PLAN-GAME-LINK.md): the game's connection to Cloud90.
// A background thread keeps a WebSocket open to the Cloud90 API with the
// streamer's Game Link token and turns each "effect" message into
// galaxy::cloud90::enqueue(); it never touches the game itself.
//
// Settings: %LOCALAPPDATA%\Nebula\cloud90-link.ini — `token_protected=…`
// (the token sealed with DPAPI for this Windows user; an older plain
// `token=…` is sealed on first read) and optionally `server=https://…`
// (default: the production API). The token is pasted in F1 > CLOUD90 or
// with tools\cloud90\set-token.cmd; with none the thread waits for one.
namespace galaxy::cloud90 {

// The build the server must accept (GAMES.smg.versions on the backend).
inline constexpr const char* kLinkVersion = "nebula-1.0.1+cloud90.3";

// Starts the link thread. Call once.
void start_link() noexcept;

// "connected", "connecting…", "sin token", the server's reason for closing…
[[nodiscard]] std::string link_status();

// The same for the F1 page: CONNECTED, CONNECTING, NO TOKEN, TOKEN REJECTED,
// NO ACCESS, UPDATE THE GAME, OFFLINE - RETRYING.
[[nodiscard]] std::string link_ui_status();

// "C90GL_....AB12", or "NOT SET".
[[nodiscard]] std::string token_hint();

// F1 > CLOUD90: store a token (sealed) or forget it; the link reconnects.
[[nodiscard]] bool save_token(const std::string& token);
[[nodiscard]] bool clear_token();

enum class PasteResult { Saved, NotAToken, SaveFailed };
// Reads a token from the clipboard and stores it.
[[nodiscard]] PasteResult paste_token_from_clipboard();

// DPAPI (current user) + base64, as set-token.ps1 writes it. Empty on failure.
[[nodiscard]] std::string protect_token(const std::string& token);
[[nodiscard]] std::string unprotect_token(const std::string& base64);

// Parsing helpers for the server's flat JSON messages (exposed for tests).
[[nodiscard]] std::string json_string_field(const std::string& json, const char* key);
[[nodiscard]] long long json_number_field(const std::string& json, const char* key, long long fallback);
[[nodiscard]] bool json_bool_field(const std::string& json, const char* key, bool fallback);

}  // namespace galaxy::cloud90
