#include "galaxy/cloud90_link.h"

#include "galaxy/cloud90_effects.h"
#include "galaxy/cloud90_game.h"

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <Windows.h>
#include <dpapi.h>
#include <wincrypt.h>
#include <winhttp.h>

#include <algorithm>
#include <atomic>
#include <cctype>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#pragma comment(lib, "winhttp.lib")
#pragma comment(lib, "crypt32.lib")

namespace galaxy::cloud90 {
namespace {

constexpr const wchar_t* kDefaultServer = L"https://backcloud90devstreamkit.onrender.com";
constexpr const wchar_t* kPath = L"/api/game-link/ws";
constexpr auto kPingEvery = std::chrono::seconds(20);
// A connection that lasted this long counts as working: the retry waits start over.
constexpr auto kStableConnection = std::chrono::seconds(30);
// DPAPI entropy: the token file only opens for this Windows user and this
// purpose (tools\cloud90\set-token.ps1 uses the same).
constexpr char kEntropy[] = "Cloud90GameLink";

std::mutex g_status_mutex;
std::string g_status = "sin iniciar";  // guarded by g_status_mutex
std::string g_ui_status = "OFF";        // guarded by g_status_mutex
std::atomic<bool> g_started{false};
// Bumped when the token changes (F1 page): the link drops its connection
// and starts over with the new one.
std::atomic<unsigned> g_generation{0};

std::filesystem::path data_dir() {
    wchar_t buf[MAX_PATH]{};
    const DWORD n = GetEnvironmentVariableW(L"LOCALAPPDATA", buf, MAX_PATH);
    if (n == 0 || n >= MAX_PATH) return {};
    return std::filesystem::path(buf) / L"Nebula";
}

std::filesystem::path config_path() { return data_dir() / L"cloud90-link.ini"; }

// `line` for the log, `ui` for the F1 page (its font: capitals, no accents).
void set_status(const std::string& line, const std::string& ui) {
    {
        const std::lock_guard<std::mutex> lock(g_status_mutex);
        g_status = line;
        g_ui_status = ui;
    }
    std::fprintf(stderr, "[cloud90] %s\n", line.c_str());
    try {
        std::ofstream out(data_dir() / L"cloud90-link.log", std::ios::app);
        SYSTEMTIME t{};
        GetLocalTime(&t);
        char stamp[32];
        std::snprintf(stamp, sizeof stamp, "%02u:%02u:%02u ", t.wHour, t.wMinute, t.wSecond);
        out << stamp << line << '\n';
    } catch (...) {
    }
}

std::string trim(std::string s) {
    const auto issp = [](unsigned char c) { return c == ' ' || c == '\t' || c == '\r' || c == '\n'; };
    while (!s.empty() && issp(static_cast<unsigned char>(s.back()))) s.pop_back();
    std::size_t i = 0;
    while (i < s.size() && issp(static_cast<unsigned char>(s[i]))) ++i;
    return s.substr(i);
}

struct LinkConfig {
    std::wstring host;
    INTERNET_PORT port = INTERNET_DEFAULT_HTTPS_PORT;
    bool secure = true;
    std::string token;
};

std::wstring widen(const std::string& s) {
    if (s.empty()) return {};
    const int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0);
    std::wstring w(static_cast<std::size_t>(n), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), w.data(), n);
    return w;
}

// "https://host[:port][/...]" or "http://…" (local testing).
bool parse_server(const std::wstring& url, LinkConfig& cfg) {
    std::wstring rest;
    if (url.rfind(L"https://", 0) == 0) {
        cfg.secure = true;
        cfg.port = INTERNET_DEFAULT_HTTPS_PORT;
        rest = url.substr(8);
    } else if (url.rfind(L"http://", 0) == 0) {
        cfg.secure = false;
        cfg.port = INTERNET_DEFAULT_HTTP_PORT;
        rest = url.substr(7);
    } else {
        return false;
    }
    rest = rest.substr(0, rest.find(L'/'));
    const auto colon = rest.find(L':');
    if (colon != std::wstring::npos) {
        const int port = _wtoi(rest.substr(colon + 1).c_str());
        if (port <= 0 || port > 65535) return false;
        cfg.port = static_cast<INTERNET_PORT>(port);
        rest = rest.substr(0, colon);
    }
    cfg.host = rest;
    return !cfg.host.empty();
}

bool valid_token(const std::string& t) {
    if (t.rfind("c90gl_", 0) != 0 || t.size() < 20 || t.size() > 100) return false;
    return std::all_of(t.begin(), t.end(), [](char c) {
        return std::isalnum(static_cast<unsigned char>(c)) || c == '_' || c == '-';
    });
}

struct StoredConfig {
    std::string token;
    std::string server;
    bool was_plain = false;
};

StoredConfig read_stored() {
    StoredConfig out;
    std::ifstream in(config_path());
    std::string line;
    while (std::getline(in, line)) {
        line = trim(line);
        if (line.rfind("token_protected=", 0) == 0) {
            out.token = unprotect_token(trim(line.substr(16)));
        } else if (line.rfind("token=", 0) == 0 && out.token.empty()) {
            out.token = trim(line.substr(6));  // older set-token: plain text
            out.was_plain = true;
        } else if (line.rfind("server=", 0) == 0) {
            out.server = trim(line.substr(7));
        }
    }
    return out;
}

bool write_stored(const std::string& token, const std::string& server) {
    try {
        std::filesystem::create_directories(data_dir());
        const std::filesystem::path tmp = config_path().wstring() + L".tmp";
        {
            std::ofstream out(tmp, std::ios::trunc);
            if (!token.empty()) {
                const std::string sealed = protect_token(token);
                if (sealed.empty()) return false;
                out << "token_protected=" << sealed << '\n';
            }
            if (!server.empty()) out << "server=" << server << '\n';
            if (!out) return false;
        }
        std::filesystem::rename(tmp, config_path());
        return true;
    } catch (...) {
        return false;
    }
}

bool read_config(LinkConfig& cfg) {
    StoredConfig stored = read_stored();
    // A token left in plain text by an older set-token gets sealed now.
    if (stored.was_plain && valid_token(stored.token)) (void)write_stored(stored.token, stored.server);
    if (!valid_token(stored.token)) return false;
    cfg.token = stored.token;
    return parse_server(stored.server.empty() ? std::wstring(kDefaultServer) : widen(stored.server), cfg);
}

std::string escape(const std::string& s) {
    std::string out;
    for (const char c : s) {
        if (c == '"' || c == '\\') out += '\\';
        if (static_cast<unsigned char>(c) >= 0x20) out += c;
    }
    return out;
}

class Connection {
public:
    ~Connection() { close(); }

    bool open(const LinkConfig& cfg, std::string& error) {
        session_ = WinHttpOpen(L"Nebula-Cloud90/1", WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY,
                               WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
        if (!session_) return fail(error, "WinHttpOpen");
        WinHttpSetTimeouts(session_, 10'000, 10'000, 10'000, 0);  // receive: no timeout
        connect_ = WinHttpConnect(session_, cfg.host.c_str(), cfg.port, 0);
        if (!connect_) return fail(error, "WinHttpConnect");
        request_ = WinHttpOpenRequest(connect_, L"GET", kPath, nullptr, WINHTTP_NO_REFERER,
                                      WINHTTP_DEFAULT_ACCEPT_TYPES, cfg.secure ? WINHTTP_FLAG_SECURE : 0);
        if (!request_) return fail(error, "WinHttpOpenRequest");
        if (!WinHttpSetOption(request_, WINHTTP_OPTION_UPGRADE_TO_WEB_SOCKET, nullptr, 0))
            return fail(error, "upgrade option");
        const std::wstring header = L"Authorization: Bearer " + widen(cfg.token);
        if (!WinHttpSendRequest(request_, header.c_str(), static_cast<DWORD>(header.size()),
                                WINHTTP_NO_REQUEST_DATA, 0, 0, 0))
            return fail(error, "no se pudo conectar al servidor");
        if (!WinHttpReceiveResponse(request_, nullptr)) return fail(error, "sin respuesta del servidor");
        DWORD status = 0;
        DWORD size = sizeof status;
        WinHttpQueryHeaders(request_, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                            WINHTTP_HEADER_NAME_BY_INDEX, &status, &size, WINHTTP_NO_HEADER_INDEX);
        if (status != 101) {
            error = status == 503 ? "Game Link está apagado en el servidor"
                                  : "el servidor respondió " + std::to_string(status);
            return false;
        }
        socket_ = WinHttpWebSocketCompleteUpgrade(request_, 0);
        if (!socket_) return fail(error, "WebSocket");
        WinHttpCloseHandle(request_);
        request_ = nullptr;
        return true;
    }

    bool send(const std::string& text) {
        const std::lock_guard<std::mutex> lock(send_mutex_);
        if (!socket_) return false;
        return WinHttpWebSocketSend(socket_, WINHTTP_WEB_SOCKET_UTF8_MESSAGE_BUFFER_TYPE,
                                    const_cast<char*>(text.data()), static_cast<DWORD>(text.size())) == NO_ERROR;
    }

    // One whole text message, or false when the connection ended (`code`
    // and `reason` then say why, if the server closed it).
    bool receive(std::string& out, USHORT& code, std::string& reason) {
        out.clear();
        std::vector<char> buf(4096);
        for (;;) {
            DWORD read = 0;
            WINHTTP_WEB_SOCKET_BUFFER_TYPE type{};
            if (WinHttpWebSocketReceive(socket_, buf.data(), static_cast<DWORD>(buf.size()), &read, &type) != NO_ERROR)
                return false;
            if (type == WINHTTP_WEB_SOCKET_CLOSE_BUFFER_TYPE) {
                char r[128]{};
                DWORD rlen = 0;
                WinHttpWebSocketQueryCloseStatus(socket_, &code, r, sizeof r, &rlen);
                reason.assign(r, rlen);
                return false;
            }
            out.append(buf.data(), read);
            if (out.size() > 16384) return false;
            if (type == WINHTTP_WEB_SOCKET_UTF8_MESSAGE_BUFFER_TYPE) return true;
        }
    }

    // From the side thread when a send fails (the network is gone): closing
    // the socket unblocks the pending receive.
    void abort() {
        const std::lock_guard<std::mutex> lock(send_mutex_);
        if (socket_) {
            WinHttpCloseHandle(socket_);
            socket_ = nullptr;
        }
    }

    void close() {
        {
            const std::lock_guard<std::mutex> lock(send_mutex_);
            if (socket_) {
                WinHttpWebSocketClose(socket_, WINHTTP_WEB_SOCKET_SUCCESS_CLOSE_STATUS, nullptr, 0);
                WinHttpCloseHandle(socket_);
                socket_ = nullptr;
            }
        }
        if (request_) WinHttpCloseHandle(request_), request_ = nullptr;
        if (connect_) WinHttpCloseHandle(connect_), connect_ = nullptr;
        if (session_) WinHttpCloseHandle(session_), session_ = nullptr;
    }

private:
    static bool fail(std::string& error, const char* what) {
        error = std::string(what) + " (error " + std::to_string(GetLastError()) + ")";
        return false;
    }

    HINTERNET session_ = nullptr;
    HINTERNET connect_ = nullptr;
    HINTERNET request_ = nullptr;
    HINTERNET socket_ = nullptr;
    std::mutex send_mutex_;
};

// The connection the link thread is using, so a token change can drop it.
std::mutex g_current_mutex;
Connection* g_current = nullptr;  // guarded by g_current_mutex

void drop_current_connection() {
    const std::lock_guard<std::mutex> lock(g_current_mutex);
    if (g_current) g_current->abort();
}

std::string state_message() {
    return std::string("{\"type\":\"state\",\"paused\":") + (paused() ? "true" : "false") +
           ",\"inPlay\":" + (in_play() ? "true" : "false") + "}";
}

void handle_message(Connection& conn, const std::string& msg) {
    const std::string type = json_string_field(msg, "type");
    if (type == "effect") {
        const std::string id = json_string_field(msg, "id");
        const std::string name = json_string_field(msg, "effect");
        const auto effect = effect_from_name(name);
        // Phase 5: instant changes to the game (health, lives, star bits); no duration.
        const auto action = effect ? std::nullopt : game_action_from_name(name);
        const long long ms = json_number_field(msg, "ms", 0);
        const auto now_ms = static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now().time_since_epoch()).count());
        std::string reason;
        if (!effect && !action) reason = "efecto desconocido";
        else if (effect && ms <= 0) reason = "duración inválida";
        else if (paused()) reason = "en pausa";
        else if (effect ? !effect_applicable(*effect) : !game_action_applicable(*action, now_ms))
            reason = "no disponible ahora";
        else if (action && !request_game_action(*action, now_ms)) reason = "no disponible ahora";
        if (reason.empty() && effect)
            enqueue(*effect, static_cast<std::uint32_t>(std::min<long long>(ms, kMaxEffectMs)));
        conn.send("{\"type\":\"ack\",\"id\":\"" + escape(id) + "\",\"ok\":" + (reason.empty() ? "true" : "false") +
                  (reason.empty() ? "" : ",\"reason\":\"" + reason + "\"") + "}");
    } else if (type == "pause") {
        const bool p = json_bool_field(msg, "paused", true);
        if (p) panic();
        else set_paused(false);
        conn.send(state_message());
    } else if (type == "welcome") {
        set_status("conectado a Cloud90", "CONNECTED");
    }
}

// Waits up to `d`, or less if the token changes.
void wait_or_token_change(std::chrono::seconds d, unsigned generation) {
    const auto until = std::chrono::steady_clock::now() + d;
    while (std::chrono::steady_clock::now() < until && g_generation.load() == generation)
        std::this_thread::sleep_for(std::chrono::milliseconds(250));
}

void link_thread() {
    auto backoff = std::chrono::seconds(2);
    unsigned last_generation = g_generation.load();
    for (;;) {
        const unsigned generation = g_generation.load();
        if (generation != last_generation) {
            backoff = std::chrono::seconds(2);
            last_generation = generation;
        }
        LinkConfig cfg;
        if (!read_config(cfg)) {
            set_status("sin token: pégalo en F1 > CLOUD90 o con tools\\cloud90\\set-token.cmd", "NO TOKEN");
            wait_or_token_change(std::chrono::seconds(3600), generation);
            continue;
        }
        set_status("conectando…", "CONNECTING");
        Connection conn;
        std::string error;
        USHORT code = 0;
        if (conn.open(cfg, error)) {
            const auto opened_at = std::chrono::steady_clock::now();
            {
                const std::lock_guard<std::mutex> lock(g_current_mutex);
                g_current = &conn;
            }
            conn.send(std::string("{\"type\":\"hello\",\"game\":\"smg\",\"version\":\"") + kLinkVersion + "\"}");
            conn.send(state_message());
            // Pings and state changes from a side thread; receive blocks here.
            std::atomic<bool> alive{true};
            std::thread side([&] {
                std::string last = state_message();
                auto next_ping = std::chrono::steady_clock::now() + kPingEvery;
                while (alive.load()) {
                    std::this_thread::sleep_for(std::chrono::milliseconds(250));
                    const std::string now_state = state_message();
                    if (now_state != last && conn.send(now_state)) last = now_state;
                    if (std::chrono::steady_clock::now() >= next_ping) {
                        if (!conn.send("{\"type\":\"ping\"}")) {
                            conn.abort();
                            break;
                        }
                        next_ping = std::chrono::steady_clock::now() + kPingEvery;
                    }
                }
            });
            std::string msg;
            std::string reason;
            while (conn.receive(msg, code, reason)) handle_message(conn, msg);
            alive.store(false);
            side.join();
            {
                const std::lock_guard<std::mutex> lock(g_current_mutex);
                g_current = nullptr;
            }
            error = reason.empty() ? "conexión perdida" : reason;
            // Only a connection that held starts the waits over: one dropped
            // at once (a server restarting, a deploy) keeps backing off, or
            // the retries would hit the server's per-minute limit (429).
            if (std::chrono::steady_clock::now() - opened_at >= kStableConnection)
                backoff = std::chrono::seconds(2);
            // Refused for good (bad token, no access, old version): wait long.
            if (code == 4401 || code == 4403 || code == 4426 || code == 4400) backoff = std::chrono::seconds(60);
        }
        if (g_generation.load() != generation) continue;  // new token: right away
        const std::string ui = code == 4401 ? "TOKEN REJECTED"
            : code == 4403                  ? "NO ACCESS"
            : code == 4426                  ? "UPDATE THE GAME"
                                            : "OFFLINE - RETRYING";
        set_status("desconectado: " + error + "; reintento en " + std::to_string(backoff.count()) + " s", ui);
        wait_or_token_change(backoff, generation);
        backoff = std::min(backoff * 2, std::chrono::seconds(60));
    }
}

}  // namespace

void start_link() noexcept {
    if (g_started.exchange(true)) return;
    try {
        std::filesystem::create_directories(data_dir());
        std::ofstream(data_dir() / L"cloud90-link.log", std::ios::trunc);
        // Always on: with no token it waits for one (F1 > CLOUD90).
        std::thread(link_thread).detach();
    } catch (...) {
        set_status("no se pudo iniciar la conexión con Cloud90", "ERROR");
    }
}

std::string link_status() {
    const std::lock_guard<std::mutex> lock(g_status_mutex);
    return g_status;
}

std::string link_ui_status() {
    const std::lock_guard<std::mutex> lock(g_status_mutex);
    return g_ui_status;
}

std::string token_hint() {
    const std::string t = read_stored().token;
    if (!valid_token(t)) return "NOT SET";
    std::string tail = t.substr(t.size() - 4);
    for (char& c : tail) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    return "C90GL_...." + tail;
}

bool save_token(const std::string& raw) {
    const std::string token = trim(raw);
    if (!valid_token(token)) return false;
    if (!write_stored(token, read_stored().server)) return false;
    ++g_generation;
    drop_current_connection();
    return true;
}

bool clear_token() {
    if (!write_stored("", read_stored().server)) return false;
    ++g_generation;
    drop_current_connection();
    return true;
}

PasteResult paste_token_from_clipboard() {
    std::string text;
    if (OpenClipboard(nullptr)) {
        if (HANDLE data = GetClipboardData(CF_UNICODETEXT)) {
            if (const auto* w = static_cast<const wchar_t*>(GlobalLock(data))) {
                const int n = WideCharToMultiByte(CP_UTF8, 0, w, -1, nullptr, 0, nullptr, nullptr);
                if (n > 1 && n < 4096) {
                    text.resize(static_cast<std::size_t>(n));
                    WideCharToMultiByte(CP_UTF8, 0, w, -1, text.data(), n, nullptr, nullptr);
                    text.resize(static_cast<std::size_t>(n - 1));
                }
                GlobalUnlock(data);
            }
        }
        CloseClipboard();
    }
    if (!valid_token(trim(text))) return PasteResult::NotAToken;
    return save_token(text) ? PasteResult::Saved : PasteResult::SaveFailed;
}

std::string protect_token(const std::string& token) {
    DATA_BLOB in{static_cast<DWORD>(token.size()), reinterpret_cast<BYTE*>(const_cast<char*>(token.data()))};
    DATA_BLOB entropy{static_cast<DWORD>(sizeof kEntropy - 1),
                      reinterpret_cast<BYTE*>(const_cast<char*>(kEntropy))};
    DATA_BLOB out{};
    if (!CryptProtectData(&in, L"Cloud90 Game Link", &entropy, nullptr, nullptr, CRYPTPROTECT_UI_FORBIDDEN, &out))
        return {};
    DWORD chars = 0;
    std::string b64;
    if (CryptBinaryToStringA(out.pbData, out.cbData, CRYPT_STRING_BASE64 | CRYPT_STRING_NOCRLF, nullptr, &chars)) {
        b64.resize(chars);
        if (CryptBinaryToStringA(out.pbData, out.cbData, CRYPT_STRING_BASE64 | CRYPT_STRING_NOCRLF, b64.data(),
                                 &chars))
            b64.resize(chars);
        else
            b64.clear();
    }
    LocalFree(out.pbData);
    return b64;
}

std::string unprotect_token(const std::string& base64) {
    DWORD size = 0;
    if (base64.empty() || base64.size() > 4096 ||
        !CryptStringToBinaryA(base64.c_str(), 0, CRYPT_STRING_BASE64, nullptr, &size, nullptr, nullptr))
        return {};
    std::vector<BYTE> sealed(size);
    if (!CryptStringToBinaryA(base64.c_str(), 0, CRYPT_STRING_BASE64, sealed.data(), &size, nullptr, nullptr))
        return {};
    DATA_BLOB in{size, sealed.data()};
    DATA_BLOB entropy{static_cast<DWORD>(sizeof kEntropy - 1),
                      reinterpret_cast<BYTE*>(const_cast<char*>(kEntropy))};
    DATA_BLOB out{};
    if (!CryptUnprotectData(&in, nullptr, &entropy, nullptr, nullptr, CRYPTPROTECT_UI_FORBIDDEN, &out)) return {};
    std::string token(reinterpret_cast<char*>(out.pbData), out.cbData);
    SecureZeroMemory(out.pbData, out.cbData);
    LocalFree(out.pbData);
    return token;
}


// The server sends compact JSON.stringify output with flat fields.
std::string json_string_field(const std::string& json, const char* key) {
    const std::string needle = std::string("\"") + key + "\":\"";
    const auto at = json.find(needle);
    if (at == std::string::npos) return {};
    std::string out;
    for (std::size_t i = at + needle.size(); i < json.size(); ++i) {
        const char c = json[i];
        if (c == '\\' && i + 1 < json.size()) {
            out += json[++i];
            continue;
        }
        if (c == '"') return out;
        out += c;
    }
    return {};
}

long long json_number_field(const std::string& json, const char* key, long long fallback) {
    const std::string needle = std::string("\"") + key + "\":";
    const auto at = json.find(needle);
    if (at == std::string::npos) return fallback;
    std::size_t i = at + needle.size();
    bool neg = false;
    if (i < json.size() && json[i] == '-') neg = true, ++i;
    long long v = 0;
    std::size_t digits = 0;
    for (; i < json.size() && json[i] >= '0' && json[i] <= '9' && digits < 15; ++i, ++digits)
        v = v * 10 + (json[i] - '0');
    return digits ? (neg ? -v : v) : fallback;
}

bool json_bool_field(const std::string& json, const char* key, bool fallback) {
    const std::string needle = std::string("\"") + key + "\":";
    const auto at = json.find(needle);
    if (at == std::string::npos) return fallback;
    if (json.compare(at + needle.size(), 4, "true") == 0) return true;
    if (json.compare(at + needle.size(), 5, "false") == 0) return false;
    return fallback;
}

}  // namespace galaxy::cloud90
