#include "galaxy/cloud90_link.h"

#include "galaxy/cloud90_effects.h"

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <Windows.h>
#include <winhttp.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#pragma comment(lib, "winhttp.lib")

namespace galaxy::cloud90 {
namespace {

constexpr const wchar_t* kDefaultServer = L"https://backcloud90devstreamkit.onrender.com";
constexpr const wchar_t* kPath = L"/api/game-link/ws";
constexpr auto kPingEvery = std::chrono::seconds(20);

std::mutex g_status_mutex;
std::string g_status = "sin iniciar";  // guarded by g_status_mutex
std::atomic<bool> g_started{false};

std::filesystem::path data_dir() {
    wchar_t buf[MAX_PATH]{};
    const DWORD n = GetEnvironmentVariableW(L"LOCALAPPDATA", buf, MAX_PATH);
    if (n == 0 || n >= MAX_PATH) return {};
    return std::filesystem::path(buf) / L"Nebula";
}

void log_line(const std::string& line) {
    {
        const std::lock_guard<std::mutex> lock(g_status_mutex);
        g_status = line;
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

bool read_config(LinkConfig& cfg) {
    std::ifstream in(data_dir() / L"cloud90-link.ini");
    std::string server;
    std::string line;
    while (std::getline(in, line)) {
        line = trim(line);
        if (line.rfind("token=", 0) == 0) cfg.token = trim(line.substr(6));
        if (line.rfind("server=", 0) == 0) server = trim(line.substr(7));
    }
    if (cfg.token.empty() || cfg.token.size() > 100) return false;
    return parse_server(server.empty() ? std::wstring(kDefaultServer) : widen(server), cfg);
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

std::string state_message() {
    return std::string("{\"type\":\"state\",\"paused\":") + (paused() ? "true" : "false") +
           ",\"inPlay\":" + (in_play() ? "true" : "false") + "}";
}

void handle_message(Connection& conn, const std::string& msg) {
    const std::string type = json_string_field(msg, "type");
    if (type == "effect") {
        const std::string id = json_string_field(msg, "id");
        const auto effect = effect_from_name(json_string_field(msg, "effect"));
        const long long ms = json_number_field(msg, "ms", 0);
        std::string reason;
        if (!effect) reason = "efecto desconocido";
        else if (ms <= 0) reason = "duración inválida";
        else if (paused()) reason = "en pausa";
        if (reason.empty())
            enqueue(*effect, static_cast<std::uint32_t>(std::min<long long>(ms, kMaxEffectMs)));
        conn.send("{\"type\":\"ack\",\"id\":\"" + escape(id) + "\",\"ok\":" + (reason.empty() ? "true" : "false") +
                  (reason.empty() ? "" : ",\"reason\":\"" + reason + "\"") + "}");
    } else if (type == "pause") {
        const bool p = json_bool_field(msg, "paused", true);
        if (p) panic();
        else set_paused(false);
        conn.send(state_message());
    } else if (type == "welcome") {
        log_line("conectado a Cloud90");
    }
}

void link_thread(LinkConfig cfg) {
    auto backoff = std::chrono::seconds(2);
    for (;;) {
        log_line("conectando…");
        Connection conn;
        std::string error;
        if (conn.open(cfg, error)) {
            backoff = std::chrono::seconds(2);
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
            USHORT code = 0;
            std::string reason;
            while (conn.receive(msg, code, reason)) handle_message(conn, msg);
            alive.store(false);
            side.join();
            error = reason.empty() ? "conexión perdida" : reason;
            // Refused for good (bad token, no access, old version): wait long.
            if (code == 4401 || code == 4403 || code == 4426 || code == 4400) backoff = std::chrono::seconds(60);
        }
        log_line("desconectado: " + error + "; reintento en " + std::to_string(backoff.count()) + " s");
        std::this_thread::sleep_for(backoff);
        backoff = std::min(backoff * 2, std::chrono::seconds(60));
    }
}

}  // namespace

void start_link() noexcept {
    if (g_started.exchange(true)) return;
    try {
        std::filesystem::create_directories(data_dir());
        std::ofstream(data_dir() / L"cloud90-link.log", std::ios::trunc);
        LinkConfig cfg;
        if (!read_config(cfg)) {
            log_line("sin token: corre tools\\cloud90\\set-token.cmd para conectar con Cloud90");
            return;
        }
        std::thread(link_thread, cfg).detach();
    } catch (...) {
        log_line("no se pudo iniciar la conexión con Cloud90");
    }
}

std::string link_status() {
    const std::lock_guard<std::mutex> lock(g_status_mutex);
    return g_status;
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
