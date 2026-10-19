#include "apivpn/client/vpn_client.hpp"
#include "apivpn/common/logger.hpp"
#include "apivpn/platform/net_socket.hpp"
#include "apivpn/platform/process_launcher.hpp"

#ifdef _WIN32
#include "apivpn/platform/win32_utf8.hpp"
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <shellapi.h>
#endif

#include <chrono>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

namespace apivpn::client {
int runWin32Gui(VpnClient& client, const GuiLaunchOptions& options);
}

namespace {

// How long --launch waits for the server to assign a virtual IP (VpnClient keeps
// retrying the connection in the background meanwhile).
constexpr auto kLaunchConnectTimeout = std::chrono::seconds(30);

// Exit codes
constexpr int kExitUsageError = 5;

// std::string paths are UTF-8; std::filesystem must be fed wide strings on Windows,
// since its narrow constructor / .string() use the ANSI code page.
std::filesystem::path pathFromUtf8(const std::string& utf8) {
#ifdef _WIN32
    return std::filesystem::path(apivpn::platform::utf8ToWide(utf8));
#else
    return std::filesystem::path(utf8);
#endif
}

std::string pathToUtf8(const std::filesystem::path& p) {
#ifdef _WIN32
    return apivpn::platform::wideToUtf8(p.wstring());
#else
    return p.string();
#endif
}

std::string trimConfigStr(const std::string& s) {
    size_t start = s.find_first_not_of(" \t\r\n");
    if (start == std::string::npos) return "";
    size_t end = s.find_last_not_of(" \t\r\n");
    return s.substr(start, end - start + 1);
}

bool readConfigFile(const std::filesystem::path& path, std::string& out) {
#ifdef _WIN32
    HANDLE h = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                           nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    out.clear();
    char buf[4096];
    DWORD got = 0;
    while (ReadFile(h, buf, static_cast<DWORD>(sizeof(buf)), &got, nullptr) && got > 0) {
        out.append(buf, got);
    }
    CloseHandle(h);
#else
    std::ifstream in(path, std::ios::binary);
    if (!in.is_open()) return false;
    std::ostringstream ss;
    ss << in.rdbuf();
    out = ss.str();
#endif
    if (out.size() >= 3 &&
        static_cast<unsigned char>(out[0]) == 0xEF &&
        static_cast<unsigned char>(out[1]) == 0xBB &&
        static_cast<unsigned char>(out[2]) == 0xBF) {
        out.erase(0, 3);
    }
    return true;
}

void loadConfigDefaults(
    std::string& server_ip,
    uint16_t& server_port,
    std::string& nickname,
    bool& force_relay) {

    std::filesystem::path cfg_path =
        pathFromUtf8(apivpn::platform::ProcessLauncher::getCurrentModuleDirectory()) / "apivpn_client.ini";
    std::string content;
    if (!readConfigFile(cfg_path, content)) return;

    std::istringstream in(content);
    std::string line;
    while (std::getline(in, line)) {
        line = trimConfigStr(line);
        if (line.empty() || line[0] == '#') continue;
        size_t eq = line.find('=');
        if (eq == std::string::npos) continue;
        std::string key = trimConfigStr(line.substr(0, eq));
        std::string val = trimConfigStr(line.substr(eq + 1));

        if (key == "server_ip" && !val.empty()) {
            server_ip = val;
        } else if (key == "server_port" && !val.empty()) {
            uint16_t p = static_cast<uint16_t>(std::strtoul(val.c_str(), nullptr, 10));
            if (p != 0) server_port = p;
        } else if (key == "nickname" && !val.empty()) {
            nickname = val;
        } else if (key == "force_relay") {
            force_relay = (val == "1" || val == "true");
        }
    }
}

// Appends one argument to a command line so that CommandLineToArgvW / the MSVC CRT
// parse it back to exactly `token`: tokens that are empty or contain whitespace or
// quotes are quoted, embedded quotes are backslash-escaped, and backslashes are
// doubled only where they precede a quote (embedded or the closing one).
void appendArgToken(std::string& args_str, const std::string& token) {
    if (!args_str.empty()) {
        args_str += ' ';
    }
    if (!token.empty() && token.find_first_of(" \t\n\v\"") == std::string::npos) {
        args_str += token;
        return;
    }
    args_str += '"';
    size_t backslashes = 0;
    for (char c : token) {
        if (c == '\\') {
            ++backslashes;
            continue;
        }
        if (c == '"') {
            args_str.append(backslashes * 2 + 1, '\\');
        } else {
            args_str.append(backslashes, '\\');
        }
        args_str += c;
        backslashes = 0;
    }
    args_str.append(backslashes * 2, '\\');
    args_str += '"';
}

std::string resolveLaunchExecutablePath(const std::string& raw_exe) {
    if (raw_exe.empty()) return raw_exe;
    std::error_code ec;
    std::filesystem::path p = pathFromUtf8(raw_exe);
    if (std::filesystem::exists(p, ec) && !std::filesystem::is_directory(p, ec)) {
        auto abs_p = std::filesystem::absolute(p, ec);
        if (!ec) return pathToUtf8(abs_p);
        return pathToUtf8(p);
    }
    std::filesystem::path bin_dir = pathFromUtf8(apivpn::platform::ProcessLauncher::getCurrentModuleDirectory());
    std::filesystem::path in_bin = bin_dir / p;
    if (std::filesystem::exists(in_bin, ec) && !std::filesystem::is_directory(in_bin, ec)) {
        auto abs_p = std::filesystem::absolute(in_bin, ec);
        if (!ec) return pathToUtf8(abs_p);
        return pathToUtf8(in_bin);
    }
    return raw_exe;
}

bool parsePort(const std::string& s, uint16_t& out) {
    if (s.empty() || s.size() > 5) return false;
    unsigned long v = 0;
    for (char c : s) {
        if (c < '0' || c > '9') return false;
        v = v * 10 + static_cast<unsigned long>(c - '0');
    }
    if (v < 1 || v > 65535) return false;
    out = static_cast<uint16_t>(v);
    return true;
}

struct ClientOptions {
    std::string server_ip   = "localhost";
    uint16_t    server_port = apivpn::kDefaultServerPort;
    std::string nickname    = "Player1";
    std::string launch_exe;
    std::string launch_args;
    bool auto_connect        = false;
    bool headless            = false;
    bool force_relay         = false;
    bool start_in_tray       = false;
    bool tray_explicit       = false;
    bool exit_when_game_ends = false;
    bool exit_explicit       = false;
};

constexpr const char* kUsageText =
    "Usage: apivpn_client [options] [game.exe [game args...]]\n"
    "  -s, --server <host>     VPN server host (connects on startup)\n"
    "  -p, --port <1-65535>    VPN server port (connects on startup)\n"
    "  -n, --nick <name>       Nickname\n"
    "  -l, --launch <exe>      Game to launch once connected\n"
    "  --args <text>           Extra game arguments (raw command-line text)\n"
    "  --connect, --headless, --force-relay, --tray, --no-tray,\n"
    "  --exit-with-game, --keep-open\n"
    "The first non-option argument is the game; everything after it (or after --)\n"
    "is passed to the game unchanged.";

// Parses client options. The first positional token (or the token after "--") is
// the game executable and everything after it is passed to the game verbatim; a
// positional token after --launch starts the game arguments the same way.
bool parseCommandLine(const std::vector<std::string>& args, ClientOptions& o, std::string& error) {
    auto is = [](const std::string& a, const char* long_name, const char* short_name) {
        return a == long_name || (short_name != nullptr && a == short_name);
    };

    // Appends args[from..] to the game arguments, quoting each token.
    auto takeRestAsGameArgs = [&](size_t from) {
        for (size_t j = from; j < args.size(); ++j) {
            appendArgToken(o.launch_args, args[j]);
        }
    };

    for (size_t i = 1; i < args.size(); ++i) {
        const std::string& arg = args[i];

        auto needValue = [&](std::string& out_value) {
            if (i + 1 >= args.size()) {
                error = "Option " + arg + " requires a value.";
                return false;
            }
            out_value = args[++i];
            return true;
        };

        if (arg == "--") {
            size_t next = i + 1;
            if (o.launch_exe.empty() && next < args.size()) {
                o.launch_exe = args[next];
                ++next;
            }
            takeRestAsGameArgs(next);
            return true;
        } else if (is(arg, "--server", "-s")) {
            if (!needValue(o.server_ip)) return false;
            if (o.server_ip.empty()) {
                error = "Option " + arg + " requires a non-empty value.";
                return false;
            }
            o.auto_connect = true;
        } else if (is(arg, "--port", "-p")) {
            std::string value;
            if (!needValue(value)) return false;
            if (!parsePort(value, o.server_port)) {
                error = "Invalid port '" + value + "': expected a number from 1 to 65535.";
                return false;
            }
            o.auto_connect = true;
        } else if (is(arg, "--nick", "-n")) {
            if (!needValue(o.nickname)) return false;
        } else if (is(arg, "--launch", "-l")) {
            if (!needValue(o.launch_exe)) return false;
        } else if (is(arg, "--launch-args", "--args")) {
            std::string value;
            if (!needValue(value)) return false;
            // Raw command-line fragment, appended as-is (already quoted by the caller).
            if (!value.empty()) {
                if (!o.launch_args.empty()) o.launch_args += ' ';
                o.launch_args += value;
            }
        } else if (is(arg, "--tray", "-m") || arg == "--minimized" || arg == "--minimize" ||
                   arg == "--minimize-to-tray") {
            o.start_in_tray = true;
            o.tray_explicit = true;
        } else if (arg == "--no-tray" || arg == "--show-gui") {
            o.start_in_tray = false;
            o.tray_explicit = true;
        } else if (arg == "--exit-with-game" || arg == "--exit-with-app" || arg == "--wait-for-game") {
            o.exit_when_game_ends = true;
            o.exit_explicit = true;
        } else if (arg == "--keep-open" || arg == "--no-exit-with-game") {
            o.exit_when_game_ends = false;
            o.exit_explicit = true;
        } else if (arg == "--connect") {
            o.auto_connect = true;
        } else if (arg == "--headless") {
            o.headless = true;
        } else if (arg == "--force-relay") {
            o.force_relay = true;
        } else if (o.launch_exe.empty() && !arg.empty() && arg[0] != '-') {
            // Positional game executable: no client options after this point.
            o.launch_exe = arg;
            takeRestAsGameArgs(i + 1);
            return true;
        } else if (!o.launch_exe.empty()) {
            // Game set via --launch: the first token that is not a client option
            // (e.g. "-windowed") starts the game's own arguments.
            takeRestAsGameArgs(i);
            return true;
        } else {
            error = "Unknown option '" + arg + "'.";
            return false;
        }
    }
    return true;
}

void reportCliError(const std::string& msg, bool headless) {
    apivpn::logError(msg);
#ifdef _WIN32
    if (!headless) {
        // GUI-subsystem binary: there is usually no console to print to.
        std::wstring text = apivpn::platform::utf8ToWide(msg + "\n\n" + kUsageText);
        MessageBoxW(nullptr, text.c_str(), L"APIVPN Client", MB_OK | MB_ICONERROR);
    }
#else
    (void)headless;
#endif
}

// Waits until the client is connected with an assigned virtual IP. Keeps waiting
// while VpnClient retries in the background; gives up early once it stops trying.
bool waitForVirtualIp(apivpn::client::VpnClient& client, std::chrono::seconds timeout) {
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (std::chrono::steady_clock::now() < deadline) {
        if (client.isConnected() && client.localVip() != 0) return true;
        if (!client.isConnected() && !client.isReconnecting()) return false;
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
    return client.isConnected() && client.localVip() != 0;
}

int runClient(const std::vector<std::string>& args) {
    ClientOptions o;
    loadConfigDefaults(o.server_ip, o.server_port, o.nickname, o.force_relay);

    std::string parse_error;
    if (!parseCommandLine(args, o, parse_error)) {
        // --headless may not have been reached; look for it among the client options.
        bool headless_hint = o.headless;
        for (size_t i = 1; i < args.size() && args[i] != "--"; ++i) {
            if (args[i] == "--headless") headless_hint = true;
        }
        reportCliError(parse_error, headless_hint);
        return kExitUsageError;
    }

#ifdef _WIN32
    if (!o.tray_explicit) {
        STARTUPINFOW si{};
        si.cb = sizeof(si);
        GetStartupInfoW(&si);
        if ((si.dwFlags & STARTF_USESHOWWINDOW) != 0 &&
            (si.wShowWindow == SW_SHOWMINIMIZED ||
             si.wShowWindow == SW_MINIMIZE ||
             si.wShowWindow == SW_SHOWMINNOACTIVE ||
             si.wShowWindow == SW_HIDE)) {
            o.start_in_tray = true;
        }
    }
#endif

    if (!o.launch_exe.empty()) {
        o.launch_exe = resolveLaunchExecutablePath(o.launch_exe);
        o.auto_connect = true;
        if (!o.tray_explicit) {
            o.start_in_tray = true;
        }
        if (!o.exit_explicit) {
            o.exit_when_game_ends = true;
        }
    }

    apivpn::Logger::instance().setTag("Client:" + o.nickname);
    if (!apivpn::platform::initNetwork()) {
        apivpn::logError("Failed to initialize network stack");
        return 1;
    }

    apivpn::client::VpnClient client;
    if (!client.initLocalGateways()) {
        apivpn::logError("Failed to initialize local hook gateways");
        apivpn::platform::cleanupNetwork();
        return 2;
    }

    client.setForceRelay(o.force_relay);

    int rc = 0;
    if (o.headless) {
        if (o.auto_connect) {
            if (!client.connectToServer(o.server_ip, o.server_port, o.nickname)) {
                apivpn::logError("Auto-connect to server failed");
                if (o.launch_exe.empty()) {
                    client.stop();
                    apivpn::platform::cleanupNetwork();
                    return 3;
                }
            }
        }

        if (!o.launch_exe.empty()) {
            // Games must start with the server-assigned virtual IP, so wait for it
            // (VpnClient keeps retrying the connection meanwhile).
            if (!waitForVirtualIp(client, kLaunchConnectTimeout)) {
                apivpn::logError("Cannot launch " + o.launch_exe + ": not connected to VPN server " +
                                 apivpn::platform::formatHostPort(o.server_ip, o.server_port) +
                                 " (no virtual IP assigned within " +
                                 std::to_string(kLaunchConnectTimeout.count()) + " s)");
                client.stop();
                apivpn::platform::cleanupNetwork();
                return 4;
            }
            auto info = client.launchApp(o.launch_exe, o.launch_args);
            if (!info.success) {
                apivpn::logError("Failed to launch application: " + info.error_msg);
                client.stop();
                apivpn::platform::cleanupNetwork();
                return 4;
            }
        }

        if (!o.launch_exe.empty() && o.exit_when_game_ends) {
            // hasActiveLaunchedApps() walks the whole system process list, so
            // poll it a few times per second rather than 20 times per second.
            while (client.hasActiveLaunchedApps()) {
                std::this_thread::sleep_for(std::chrono::milliseconds(250));
            }
        } else {
            while (client.isConnected() || client.isReconnecting()) {
                std::this_thread::sleep_for(std::chrono::milliseconds(100));
            }
        }
    } else {
        // The GUI connects on a background thread (the window stays responsive)
        // and launches the game once a virtual IP has been assigned.
        apivpn::client::GuiLaunchOptions gui_opts{};
        gui_opts.start_in_tray = o.start_in_tray;
        gui_opts.exit_when_game_ends = o.exit_when_game_ends;
        gui_opts.auto_connect = o.auto_connect;
        gui_opts.server_host = o.server_ip;
        gui_opts.server_port = o.server_port;
        gui_opts.nickname = o.nickname;
        gui_opts.force_relay = o.force_relay;
        gui_opts.launch_exe = o.launch_exe;
        gui_opts.launch_args = o.launch_args;
        rc = apivpn::client::runWin32Gui(client, gui_opts);
    }

    client.stop();
    apivpn::platform::cleanupNetwork();
    return rc;
}

#ifdef _WIN32
// The real (UTF-16) command line; __argv / main's argv are in the ANSI code page.
std::vector<std::string> commandLineArgsUtf8() {
    std::vector<std::string> out;
    int argc_w = 0;
    LPWSTR* argv_w = CommandLineToArgvW(GetCommandLineW(), &argc_w);
    if (!argv_w) return out;
    out.reserve(static_cast<size_t>(argc_w));
    for (int i = 0; i < argc_w; ++i) {
        out.push_back(apivpn::platform::wideToUtf8(argv_w[i]));
    }
    LocalFree(argv_w);
    return out;
}
#endif

} // namespace

#ifdef _WIN32
int main(int, char**) {
    return runClient(commandLineArgsUtf8());
}

int WINAPI WinMain(HINSTANCE, HINSTANCE, LPSTR, int) {
    return runClient(commandLineArgsUtf8());
}
#else
int main(int argc, char* argv[]) {
    return runClient(std::vector<std::string>(argv, argv + argc));
}
#endif
