#pragma once

#include <string>

// Cloud90 Game Link (PLAN-GAME-LINK.md): the game's connection to Cloud90.
// A background thread keeps a WebSocket open to the Cloud90 API with the
// streamer's Game Link token and turns each "effect" message into
// galaxy::cloud90::enqueue(); it never touches the game itself.
//
// Settings: %LOCALAPPDATA%\Nebula\cloud90-link.ini (written by
// tools\cloud90\set-token.cmd), lines `token=…` and optionally
// `server=https://…` (default: the production API). No token: no thread.
namespace galaxy::cloud90 {

// The build the server must accept (GAMES.smg.versions on the backend).
inline constexpr const char* kLinkVersion = "nebula-1.0.1+cloud90.2";

// Starts the link thread if a token is configured. Call once.
void start_link() noexcept;

// "connected", "connecting…", "sin token", the server's reason for closing…
[[nodiscard]] std::string link_status();

// Parsing helpers for the server's flat JSON messages (exposed for tests).
[[nodiscard]] std::string json_string_field(const std::string& json, const char* key);
[[nodiscard]] long long json_number_field(const std::string& json, const char* key, long long fallback);
[[nodiscard]] bool json_bool_field(const std::string& json, const char* key, bool fallback);

}  // namespace galaxy::cloud90
