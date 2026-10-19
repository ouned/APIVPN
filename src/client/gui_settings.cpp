#include "gui_win32_internal.hpp"

// Purpose: Settings persistence, saved-game profiles, and running-process enumeration.

namespace apivpn::client {

namespace {

// INI format 2 escapes '\' and '|' inside the fields of "game=name|exe|args".
// Files without an "ini_format=" line (format 1) are parsed the legacy way, where
// backslashes are literal (so existing "C:\Games\x.exe" and "\\server\share" paths
// keep loading unchanged).
constexpr int kIniFormatVersion = 2;

std::string escapeGameField(const std::string& s) {
    std::string out;
    out.reserve(s.size());
    for (char c : s) {
        if (c == '\\' || c == '|') out += '\\';
        out += c;
    }
    return out;
}

// Splits on unescaped '|' and decodes "\\" and "\|". Any other backslash is kept
// literally, so hand-edited paths with single backslashes still work.
std::vector<std::string> splitEscapedGameFields(const std::string& s) {
    std::vector<std::string> fields(1);
    for (size_t i = 0; i < s.size(); ++i) {
        const char c = s[i];
        if (c == '\\' && i + 1 < s.size() && (s[i + 1] == '\\' || s[i + 1] == '|')) {
            fields.back() += s[++i];
        } else if (c == '|') {
            fields.emplace_back();
        } else {
            fields.back() += c;
        }
    }
    return fields;
}

bool readFileBytes(const std::filesystem::path& path, std::string& out) {
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
    // Tolerate a UTF-8 BOM added by an editor.
    if (out.size() >= 3 &&
        static_cast<unsigned char>(out[0]) == 0xEF &&
        static_cast<unsigned char>(out[1]) == 0xBB &&
        static_cast<unsigned char>(out[2]) == 0xBF) {
        out.erase(0, 3);
    }
    return true;
}

// Writes "<target>.tmp" and renames it over the target, so a crash or a concurrent
// reader never observes a truncated INI.
bool writeFileAtomically(const std::filesystem::path& target, const std::string& content) {
    std::filesystem::path tmp = target;
    tmp += L".tmp";
    HANDLE h = CreateFileW(tmp.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    DWORD written = 0;
    bool ok = WriteFile(h, content.data(), static_cast<DWORD>(content.size()), &written, nullptr) != FALSE &&
              written == content.size();
    ok = ok && FlushFileBuffers(h) != FALSE;
    CloseHandle(h);
    if (ok) {
        ok = MoveFileExW(tmp.c_str(), target.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) != FALSE;
    }
    if (!ok) {
        DeleteFileW(tmp.c_str());
    }
    return ok;
}

} // namespace

std::filesystem::path getExeDir() {
    return pathFromUtf8(platform::moduleFileNameUtf8(nullptr)).parent_path();
}

std::filesystem::path getConfigFilePath() {
    return getExeDir() / "apivpn_client.ini";
}

void copyTextToClipboard(HWND hWnd, const std::string& text) {
    // CF_UNICODETEXT so non-ASCII paths and nicknames survive (Windows synthesizes CF_TEXT).
    const std::wstring wtext = platform::utf8ToWide(text);
    if (!OpenClipboard(hWnd)) return;
    EmptyClipboard();
    const size_t bytes = (wtext.size() + 1) * sizeof(wchar_t);
    HGLOBAL hGlob = GlobalAlloc(GMEM_MOVEABLE, bytes);
    if (hGlob) {
        void* ptr = GlobalLock(hGlob);
        if (ptr) {
            std::memcpy(ptr, wtext.c_str(), bytes);
            GlobalUnlock(hGlob);
            if (!SetClipboardData(CF_UNICODETEXT, hGlob)) {
                GlobalFree(hGlob);
            }
        } else {
            GlobalFree(hGlob);
        }
    }
    CloseClipboard();
}

// Several client instances (e.g. one per game shortcut) share this INI. Every save
// rewrites the whole file from this instance's in-memory state, so the last writer
// wins; the write itself is atomic (temp file + rename), never a torn file.
void saveSettings(const GuiState& st) {
    std::ostringstream out;
    out << "# APIVPN Client Configuration (UTF-8)\n";
    out << "ini_format=" << kIniFormatVersion << "\n";
    out << "server_ip=" << st.server_ip_buf << "\n";
    out << "server_port=" << st.server_port_buf << "\n";
    out << "nickname=" << st.nickname_buf << "\n";
    out << "force_relay=" << (st.force_relay ? 1 : 0) << "\n";
    out << "close_to_tray=" << (st.close_to_tray ? 1 : 0) << "\n";
    out << "minimize_to_tray=" << (st.minimize_to_tray ? 1 : 0) << "\n";
    out << "theme=" << static_cast<int>(st.theme_mode) << "\n";
    out << "notify_peer_events=" << (st.notify_peer_events ? 1 : 0) << "\n";
    out << "notify_sounds=" << (st.notify_sounds ? 1 : 0) << "\n";
    out << "active_tab=" << static_cast<int>(st.active_tab) << "\n";
    out << "win_x=" << st.win_x << "\n";
    out << "win_y=" << st.win_y << "\n";
    out << "win_w=" << st.win_w << "\n";
    out << "win_h=" << st.win_h << "\n";
    out << "win_maximized=" << (st.win_maximized ? 1 : 0) << "\n";
    out << "last_app_path=" << st.app_path_buf << "\n";
    out << "last_app_args=" << st.app_args_buf << "\n";

    for (const auto& srv : st.recent_servers) {
        if (!srv.empty()) {
            out << "recent_server=" << srv << "\n";
        }
    }

    for (const auto& g : st.saved_games) {
        if (!g.exe_path.empty()) {
            out << "game=" << escapeGameField(g.name) << "|" << escapeGameField(g.exe_path)
                << "|" << escapeGameField(g.args) << "\n";
        }
    }

    writeFileAtomically(getConfigFilePath(), out.str());
}

void addOrUpdateSavedGame(GuiState& st, const std::string& exe_path, const std::string& args, const std::string& custom_name) {
    if (exe_path.empty()) return;
    std::string name = trimStr(custom_name);
    if (name.empty()) {
        name = pathToUtf8(pathFromUtf8(exe_path).stem());
        if (name.empty()) name = exe_path;
    }

    for (auto& existing : st.saved_games) {
        if (_stricmp(existing.exe_path.c_str(), exe_path.c_str()) == 0 && existing.args == args) {
            if (!custom_name.empty()) existing.name = name;
            saveSettings(st);
            return;
        }
    }

    st.saved_games.insert(st.saved_games.begin(), SavedGameProfile{name, exe_path, args});
    if (st.saved_games.size() > 16) {
        st.saved_games.resize(16);
    }
    saveSettings(st);
}

void addRecentServer(GuiState& st, const std::string& host, uint16_t port) {
    std::string entry = platform::formatHostPort(host, port);
    st.recent_servers.erase(
        std::remove_if(st.recent_servers.begin(), st.recent_servers.end(),
                       [&](const std::string& s) { return _stricmp(s.c_str(), entry.c_str()) == 0; }),
        st.recent_servers.end());
    st.recent_servers.insert(st.recent_servers.begin(), entry);
    if (st.recent_servers.size() > 8) {
        st.recent_servers.resize(8);
    }
}

void loadSettings(GuiState& st) {
    std::string content;
    if (readFileBytes(getConfigFilePath(), content)) {
        // The format marker is written first, but scan for it up front so the
        // game= parser below never depends on line order.
        int ini_format = 1;
        {
            std::istringstream scan(content);
            std::string line;
            while (std::getline(scan, line)) {
                line = trimStr(line);
                if (line.rfind("ini_format=", 0) == 0) {
                    ini_format = std::atoi(line.c_str() + 11);
                    break;
                }
            }
        }

        std::istringstream in(content);
        std::string line;
        while (std::getline(in, line)) {
            line = trimStr(line);
            if (line.empty() || line[0] == '#') continue;
            size_t eq = line.find('=');
            if (eq == std::string::npos) continue;
            std::string key = trimStr(line.substr(0, eq));
            std::string val = trimStr(line.substr(eq + 1));

            if (key == "server_ip" && !val.empty()) {
                std::strncpy(st.server_ip_buf, val.c_str(), sizeof(st.server_ip_buf) - 1);
            } else if (key == "server_port" && !val.empty()) {
                std::strncpy(st.server_port_buf, val.c_str(), sizeof(st.server_port_buf) - 1);
            } else if (key == "nickname" && !val.empty()) {
                std::strncpy(st.nickname_buf, val.c_str(), sizeof(st.nickname_buf) - 1);
            } else if (key == "force_relay") {
                st.force_relay = (val == "1" || val == "true");
            } else if (key == "close_to_tray") {
                st.close_to_tray = (val == "1" || val == "true");
            } else if (key == "minimize_to_tray") {
                st.minimize_to_tray = (val == "1" || val == "true");
            } else if (key == "theme") {
                int m = std::atoi(val.c_str());
                st.theme_mode = (m == 1) ? ThemeMode::Light : (m == 2) ? ThemeMode::System : ThemeMode::Dark;
            } else if (key == "notify_peer_events") {
                st.notify_peer_events = (val == "1" || val == "true");
            } else if (key == "notify_sounds") {
                st.notify_sounds = (val == "1" || val == "true");
            } else if (key == "active_tab") {
                int t = std::atoi(val.c_str());
                if (t >= 0 && t <= 3) st.active_tab = static_cast<NavTab>(t);
            } else if (key == "win_x") {
                st.win_x = std::atoi(val.c_str());
            } else if (key == "win_y") {
                st.win_y = std::atoi(val.c_str());
            } else if (key == "win_w") {
                st.win_w = std::atoi(val.c_str());
            } else if (key == "win_h") {
                st.win_h = std::atoi(val.c_str());
            } else if (key == "win_maximized") {
                st.win_maximized = (val == "1" || val == "true");
            } else if (key == "last_app_path") {
                std::strncpy(st.app_path_buf, val.c_str(), sizeof(st.app_path_buf) - 1);
            } else if (key == "last_app_args") {
                std::strncpy(st.app_args_buf, val.c_str(), sizeof(st.app_args_buf) - 1);
            } else if (key == "recent_server" && !val.empty()) {
                st.recent_servers.push_back(val);
            } else if (key == "game" && !val.empty()) {
                SavedGameProfile prof;
                bool parsed = false;
                if (ini_format >= 2) {
                    std::vector<std::string> fields = splitEscapedGameFields(val);
                    if (fields.size() >= 3) {
                        prof.name = fields[0];
                        prof.exe_path = fields[1];
                        prof.args = fields[2];
                        // Unescaped extra separators (hand edits) belong to the args.
                        for (size_t fi = 3; fi < fields.size(); ++fi) {
                            prof.args += "|" + fields[fi];
                        }
                        parsed = true;
                    }
                } else {
                    size_t p1 = val.find('|');
                    size_t p2 = (p1 != std::string::npos) ? val.find('|', p1 + 1) : std::string::npos;
                    if (p1 != std::string::npos && p2 != std::string::npos) {
                        prof.name = val.substr(0, p1);
                        prof.exe_path = val.substr(p1 + 1, p2 - p1 - 1);
                        prof.args = val.substr(p2 + 1);
                        parsed = true;
                    }
                }
                if (parsed && !prof.exe_path.empty()) {
                    st.saved_games.push_back(prof);
                }
            }
        }
    }

    if (st.saved_games.empty()) {
        auto exe_dir = getExeDir();
        auto test64 = exe_dir / "test_game_app64.exe";
        auto test32 = exe_dir / "test_game_app32.exe";
        std::error_code ec;
        if (std::filesystem::exists(test64, ec)) {
            const std::string test64_utf8 = pathToUtf8(test64);
            st.saved_games.push_back({"Test Game (64-bit x64)", test64_utf8, ""});
            if (st.app_path_buf[0] == '\0') {
                std::strncpy(st.app_path_buf, test64_utf8.c_str(), sizeof(st.app_path_buf) - 1);
            }
        }
        if (std::filesystem::exists(test32, ec)) {
            st.saved_games.push_back({"Test Game (32-bit x86)", pathToUtf8(test32), ""});
        }
    }

    if (st.recent_servers.empty()) {
        st.recent_servers.push_back("localhost:34200");
        st.recent_servers.push_back("[::1]:34200");
        st.recent_servers.push_back("127.0.0.1:34200");
    }

    if (st.client) {
        auto snap = st.client->getSnapshot();
        if (snap.connected || snap.reconnecting) {
            std::strncpy(st.server_ip_buf, snap.server_host.c_str(), sizeof(st.server_ip_buf) - 1);
            std::snprintf(st.server_port_buf, sizeof(st.server_port_buf), "%u", snap.server_port);
            std::strncpy(st.nickname_buf, snap.nickname.c_str(), sizeof(st.nickname_buf) - 1);
            st.force_relay = snap.force_relay;
        } else {
            st.client->setForceRelay(st.force_relay);
        }
        st.prev_connected = snap.connected;
        st.prev_reconnecting = snap.reconnecting && !snap.connected;
        if (!snap.launched_processes.empty()) {
            // Launched before the GUI came up with the option already on: those
            // games count for "exit when game ends".
            if (st.exit_when_game_ends) {
                armExitWhenGameEnds(st, true);
            }
            const auto& first_proc = snap.launched_processes.front();
            if (!first_proc.exe_path.empty()) {
                std::strncpy(st.app_path_buf, first_proc.exe_path.c_str(), sizeof(st.app_path_buf) - 1);
                std::strncpy(st.app_args_buf, first_proc.args.c_str(), sizeof(st.app_args_buf) - 1);
            }
        }
    }
}

void refreshRunningProcesses(GuiState& st) {
    st.running_desktop_procs.clear();
    HANDLE hSnap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (hSnap == INVALID_HANDLE_VALUE) return;

    PROCESSENTRY32W pe{};
    pe.dwSize = sizeof(pe);
    DWORD self_pid = GetCurrentProcessId();

    if (Process32FirstW(hSnap, &pe)) {
        do {
            if (pe.th32ProcessID <= 4 || pe.th32ProcessID == self_pid) continue;
            HANDLE hProc = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pe.th32ProcessID);
            if (hProc) {
                wchar_t full_path[1024] = {};
                DWORD sz = static_cast<DWORD>(sizeof(full_path) / sizeof(full_path[0]));
                if (QueryFullProcessImageNameW(hProc, 0, full_path, &sz)) {
                    std::string path_str = platform::wideToUtf8(full_path, sz);
                    if (_strnicmp(path_str.c_str(), "C:\\Windows\\System32\\", 20) != 0 &&
                        _strnicmp(path_str.c_str(), "C:\\Windows\\SysWOW64\\", 20) != 0) {
                        st.running_desktop_procs.push_back({pe.th32ProcessID, platform::wideToUtf8(pe.szExeFile), path_str});
                    }
                }
                CloseHandle(hProc);
            }
        } while (Process32NextW(hSnap, &pe));
    }
    CloseHandle(hSnap);
}


} // namespace apivpn::client
