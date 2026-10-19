#include "gui_win32_internal.hpp"

// Purpose: System tray icon, window placement, and tray/connect/launch actions.

namespace apivpn::client {

namespace {

NOTIFYICONDATAW makeTrayIconData(HWND hWnd) {
    NOTIFYICONDATAW nid{};
    nid.cbSize = sizeof(nid);
    nid.hWnd = hWnd;
    nid.uID = ID_TRAY_ICON;
    return nid;
}

// Joins a finished connect worker and reports its outcome. UI thread only.
void pollConnectJob(GuiState& st) {
    if (!st.connect_in_progress || !st.connect_finished.load()) return;
    if (st.connect_thread.joinable()) {
        st.connect_thread.join();
    }
    st.connect_in_progress = false;
    st.connect_finished.store(false);
    const bool ok = st.connect_ok.load();

    if (st.connect_cancel_requested) {
        // connectToServer() cannot be interrupted, so a cancel only takes effect
        // once it has returned: drop whatever it established (or its retry loop).
        st.connect_cancel_requested = false;
        st.client->disconnect();
        st.prev_connected = false;
        st.prev_reconnecting = false;
        showBanner(st, "Connection attempt cancelled.", false);
        return;
    }

    const std::string host_port = platform::formatHostPort(st.connect_host, st.connect_port);
    if (ok) {
        addRecentServer(st, st.connect_host, st.connect_port);
        saveSettings(st);
        auto snap = st.client->getSnapshot();
        st.prev_connected = snap.connected;
        st.prev_reconnecting = false;
        showBanner(st, "Connected to " + host_port +
                       " via " + std::string(snap.server_endpoint.versionString()) +
                       " (" + snap.server_endpoint.toString() + ") as " +
                       ipv4ToString(snap.local_vip), false);
    } else {
        auto snap = st.client->getSnapshot();
        st.prev_connected = false;
        st.prev_reconnecting = snap.reconnecting;
        if (snap.reconnecting) {
            showBanner(st, "Connection failed: Could not reach VPN server at " + host_port +
                           " - retrying every 3s...", true);
        } else {
            showBanner(st, "Connection failed: Invalid VPN server address " + host_port + ".", true);
        }
    }
}

// Launches the startup --launch executable once the client has a virtual IP.
void processPendingLaunch(GuiState& st) {
    if (!st.pending_launch_active || !st.client) return;

    if (st.client->isConnected() && st.client->localVip() != 0) {
        st.pending_launch_active = false;
        const std::string exe = st.pending_launch_exe;
        const std::string args = st.pending_launch_args;
        // Command-line launches are not added to the saved-game library.
        if (!triggerLaunchApp(st, exe, args, "", false) && st.in_tray) {
            restoreWindowFromTray(st.hWnd, st);
        }
        return;
    }

    const std::string exe_name = fileNameUtf8(st.pending_launch_exe);
    if (st.connect_in_progress || st.client->isReconnecting()) {
        // Still trying: keep waiting, but after 30 s make sure the user can see why
        // the game has not started yet.
        constexpr double kPendingLaunchWarnSec = 30.0;
        if (!st.pending_launch_warned && monoSec() - st.pending_launch_since_sec >= kPendingLaunchWarnSec) {
            st.pending_launch_warned = true;
            if (st.in_tray) {
                restoreWindowFromTray(st.hWnd, st);
            }
            showBanner(st, "Still waiting for the VPN server connection before launching " + exe_name +
                           " - it starts automatically once connected.", true, 10.0);
        }
        return;
    }

    // Neither connected nor (re)connecting, e.g. the user disconnected: the launch
    // can never happen, so drop it and surface the window with the reason.
    st.pending_launch_active = false;
    if (st.in_tray) {
        restoreWindowFromTray(st.hWnd, st);
    }
    showBanner(st, "Launch of " + exe_name + " cancelled: not connected to a VPN server.", true, 10.0);
}

} // namespace

bool initTrayIcon(HWND hWnd, GuiState& st) {
    NOTIFYICONDATAW nid = makeTrayIconData(hWnd);
    nid.uFlags = NIF_ICON | NIF_MESSAGE | NIF_TIP;
    nid.uCallbackMessage = WM_TRAYICON;
    nid.hIcon = loadAppIcon(GetSystemMetrics(SM_CXSMICON));
    copyUtf8ToWideBuf(nid.szTip, sizeof(nid.szTip) / sizeof(nid.szTip[0]), "APIVPN Client - Disconnected");
    st.tray_icon_added = Shell_NotifyIconW(NIM_ADD, &nid) != FALSE;
    if (!st.tray_icon_added) {
        // After an Explorer restart the old icon may still be registered.
        Shell_NotifyIconW(NIM_DELETE, &nid);
        st.tray_icon_added = Shell_NotifyIconW(NIM_ADD, &nid) != FALSE;
    }
    // Force the next updateTrayTooltip() to push the current status text.
    st.last_tray_tip.clear();
    return st.tray_icon_added;
}

void handleTrayRetryTimer(HWND hWnd, GuiState& st) {
    if (st.tray_icon_added || initTrayIcon(hWnd, st)) {
        KillTimer(hWnd, kTrayRetryTimerId);
        return;
    }
    if (++st.tray_add_attempts >= kTrayMaxAddAttempts) {
        KillTimer(hWnd, kTrayRetryTimerId);
        // Never leave the user without any UI: no tray icon means the window must stay visible.
        if (st.in_tray || !IsWindowVisible(hWnd)) {
            restoreWindowFromTray(hWnd, st);
        }
    }
}

void updateTrayTooltip(HWND hWnd, GuiState& st, const ClientStatusSnapshot& snap) {
    if (!st.tray_icon_added) return;
    std::string tip;
    if (snap.connected) {
        tip = "APIVPN - Connected (" + ipv4ToString(snap.local_vip) + ", " +
              snap.server_endpoint.versionString() + ") | Peers: " + std::to_string(snap.peers.size());
    } else if (snap.reconnecting) {
        tip = "APIVPN - Reconnecting to " + platform::formatHostPort(snap.server_host, snap.server_port) +
              " (Attempt #" + std::to_string(snap.reconnect_attempts) + ")";
    } else if (st.connect_in_progress) {
        tip = "APIVPN - Connecting to " + platform::formatHostPort(st.connect_host, st.connect_port) + "...";
    } else {
        tip = "APIVPN - Disconnected";
    }
    if (tip == st.last_tray_tip) return;
    st.last_tray_tip = tip;

    NOTIFYICONDATAW nid = makeTrayIconData(hWnd);
    nid.uFlags = NIF_TIP;
    copyUtf8ToWideBuf(nid.szTip, sizeof(nid.szTip) / sizeof(nid.szTip[0]), tip);
    Shell_NotifyIconW(NIM_MODIFY, &nid);
}

void hideWindowToTray(HWND hWnd, GuiState& st) {
    if (!st.tray_icon_added && !initTrayIcon(hWnd, st)) {
        // Hiding without a tray icon would leave no way back to the window:
        // fall back to a normal taskbar minimize.
        if (!IsIconic(hWnd)) {
            ShowWindow(hWnd, SW_MINIMIZE);
        }
        return;
    }
    captureWindowPlacement(hWnd, st);
    ShowWindow(hWnd, SW_HIDE);
    st.in_tray = true;
    if (!st.tray_balloon_shown) {
        st.tray_balloon_shown = true;
        NOTIFYICONDATAW nid = makeTrayIconData(hWnd);
        nid.uFlags = NIF_INFO;
        copyUtf8ToWideBuf(nid.szInfoTitle, sizeof(nid.szInfoTitle) / sizeof(nid.szInfoTitle[0]),
                          "APIVPN Running in Background");
        copyUtf8ToWideBuf(nid.szInfo, sizeof(nid.szInfo) / sizeof(nid.szInfo[0]),
                          "Double-click or right-click the tray icon to restore or quick-launch games.");
        nid.dwInfoFlags = NIIF_INFO;
        Shell_NotifyIconW(NIM_MODIFY, &nid);
    }
}

void restoreWindowFromTray(HWND hWnd, GuiState& st) {
    st.in_tray = false;
    ShowWindow(hWnd, SW_SHOW);
    ShowWindow(hWnd, SW_RESTORE);
    SetForegroundWindow(hWnd);
}

void removeTrayIcon(HWND hWnd) {
    NOTIFYICONDATAW nid = makeTrayIconData(hWnd);
    Shell_NotifyIconW(NIM_DELETE, &nid);
}

void captureWindowPlacement(HWND hWnd, GuiState& st) {
    if (!hWnd || !IsWindow(hWnd)) return;
    WINDOWPLACEMENT wp{};
    wp.length = sizeof(wp);
    if (GetWindowPlacement(hWnd, &wp)) {
        st.win_maximized = (wp.showCmd == SW_SHOWMAXIMIZED);
        const RECT& r = wp.rcNormalPosition;
        if (r.right > r.left && r.bottom > r.top) {
            st.win_x = r.left;
            st.win_y = r.top;
            st.win_w = r.right - r.left;
            st.win_h = r.bottom - r.top;
        }
    }
}

void showTrayContextMenu(HWND hWnd, GuiState& st) {
    if (!st.client) return;
    auto snap = st.client->getSnapshot();
    const bool can_launch = snap.connected && snap.local_vip != 0;

    HMENU hMenu = CreatePopupMenu();
    AppendMenuW(hMenu, MF_STRING, IDM_TRAY_RESTORE, L"Open APIVPN Window");
    SetMenuDefaultItem(hMenu, IDM_TRAY_RESTORE, FALSE);
    AppendMenuW(hMenu, MF_SEPARATOR, 0, nullptr);

    if (st.connect_in_progress) {
        std::string conn_item = "Connecting to " + platform::formatHostPort(st.connect_host, st.connect_port) +
                                "... (Click to Cancel)";
        AppendMenuW(hMenu, MF_STRING, IDM_TRAY_CONNECT, platform::utf8ToWide(conn_item).c_str());
    } else if (snap.connected) {
        std::string status_item = "Status: Connected (" + ipv4ToString(snap.local_vip) + ") - Click to Copy IP";
        AppendMenuW(hMenu, MF_STRING, IDM_TRAY_COPY_VIP, platform::utf8ToWide(status_item).c_str());
        AppendMenuW(hMenu, MF_STRING, IDM_TRAY_CONNECT, L"Disconnect from Server");
    } else if (snap.reconnecting) {
        AppendMenuW(hMenu, MF_STRING | MF_GRAYED, 0, L"Status: Reconnecting...");
        AppendMenuW(hMenu, MF_STRING, IDM_TRAY_CONNECT, L"Stop Reconnecting");
    } else {
        AppendMenuW(hMenu, MF_STRING | MF_GRAYED, 0, L"Status: Disconnected");
        std::string conn_item = std::string("Connect to ") + st.server_ip_buf + ":" + st.server_port_buf;
        AppendMenuW(hMenu, MF_STRING, IDM_TRAY_CONNECT, platform::utf8ToWide(conn_item).c_str());
    }

    if (!st.saved_games.empty()) {
        AppendMenuW(hMenu, MF_SEPARATOR, 0, nullptr);
        HMENU hSub = CreatePopupMenu();
        size_t max_games = std::min<size_t>(st.saved_games.size(), 10);
        for (size_t i = 0; i < max_games; ++i) {
            std::string label = st.saved_games[i].name;
            if (!st.saved_games[i].args.empty()) {
                label += " (" + st.saved_games[i].args + ")";
            }
            // '&' would otherwise be eaten as a menu mnemonic prefix.
            std::wstring wlabel;
            for (wchar_t wc : platform::utf8ToWide(label)) {
                if (wc == L'&') wlabel += L'&';
                wlabel += wc;
            }
            UINT item_flags = MF_STRING;
            if (!can_launch) item_flags |= MF_GRAYED;
            AppendMenuW(hSub, item_flags, IDM_TRAY_LAUNCH_BASE + i, wlabel.c_str());
        }
        AppendMenuW(hMenu, MF_POPUP, reinterpret_cast<UINT_PTR>(hSub),
                    can_launch ? L"Quick Launch in VPN" : L"Quick Launch in VPN (connect first)");
    }

    AppendMenuW(hMenu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(hMenu, MF_STRING, IDM_TRAY_EXIT, L"Exit APIVPN");

    POINT pt{};
    GetCursorPos(&pt);
    SetForegroundWindow(hWnd);
    TrackPopupMenu(hMenu, TPM_RIGHTBUTTON, pt.x, pt.y, 0, hWnd, nullptr);
    PostMessageW(hWnd, WM_NULL, 0, 0);
    DestroyMenu(hMenu);
}

void startConnectJob(GuiState& st, const std::string& host, uint16_t port, const std::string& nick) {
    if (!st.client || st.connect_in_progress) return;
    if (st.connect_thread.joinable()) {
        st.connect_thread.join();
    }
    st.connect_finished.store(false);
    st.connect_ok.store(false);
    st.connect_cancel_requested = false;
    st.connect_host = host;
    st.connect_port = port;

    VpnClient* client = st.client;
    std::atomic<bool>* finished_flag = &st.connect_finished;
    std::atomic<bool>* ok_flag = &st.connect_ok;
    try {
        st.connect_thread = std::thread([client, host, port, nick, finished_flag, ok_flag]() {
            const bool result = client->connectToServer(host, port, nick);
            ok_flag->store(result);
            finished_flag->store(true);
        });
    } catch (const std::system_error&) {
        showBanner(st, "Connection failed: could not start the connect worker thread.", true);
        return;
    }
    st.connect_in_progress = true;
    st.prev_connected = false;
    st.prev_reconnecting = false;
    showBanner(st, "Connecting to " + platform::formatHostPort(host, port) + "...", false, 3.0);
}

void triggerConnectOrDisconnect(GuiState& st) {
    if (!st.client) return;
    if (st.connect_in_progress) {
        // Acts as "cancel": applied when the worker returns (see pollConnectJob()).
        if (!st.connect_cancel_requested) {
            st.connect_cancel_requested = true;
            showBanner(st, "Cancelling connection attempt...", false, 3.0);
        }
        return;
    }
    if (st.client->isConnected() || st.client->isReconnecting()) {
        st.client->disconnect();
        st.prev_connected = false;
        st.prev_reconnecting = false;
        showBanner(st, "Disconnected from VPN server.", false);
    } else {
        std::string raw_host = trimStr(st.server_ip_buf);
        std::string port_str = trimStr(st.server_port_buf);
        std::string nick = trimStr(st.nickname_buf);
        if (nick.empty()) {
            nick = "Player1";
            std::strncpy(st.nickname_buf, nick.c_str(), sizeof(st.nickname_buf) - 1);
        }
        uint16_t port = static_cast<uint16_t>(std::strtoul(port_str.c_str(), nullptr, 10));
        if (port == 0) port = kDefaultServerPort;

        std::string clean_host = raw_host;
        platform::splitHostPort(raw_host, clean_host, port);
        if (!clean_host.empty()) {
            std::strncpy(st.server_ip_buf, clean_host.c_str(), sizeof(st.server_ip_buf) - 1);
            st.server_ip_buf[sizeof(st.server_ip_buf) - 1] = '\0';
        }
        std::snprintf(st.server_port_buf, sizeof(st.server_port_buf), "%u", port);

        startConnectJob(st, clean_host, port, nick);
    }
}

void queueLaunchOnConnect(GuiState& st, const std::string& exe_path, const std::string& args) {
    if (exe_path.empty()) return;
    st.pending_launch_exe = exe_path;
    st.pending_launch_args = args;
    st.pending_launch_active = true;
    st.pending_launch_warned = false;
    st.pending_launch_since_sec = monoSec();
}

void tickBackgroundTasks(GuiState& st) {
    pollConnectJob(st);
    processPendingLaunch(st);
}

void shutdownBackgroundTasks(GuiState& st) {
    // connectToServer() is not interruptible; this waits at most for its own
    // timeouts. VpnClient::stop() (called by main afterwards) tears the link down.
    if (st.connect_thread.joinable()) {
        st.connect_thread.join();
    }
    st.connect_in_progress = false;
    st.pending_launch_active = false;
}

void armExitWhenGameEnds(GuiState& st, bool app_running) {
    if (!st.exit_when_game_ends || !app_running) {
        st.exit_armed = false;
        return;
    }
    st.exit_armed = true;
    // Re-probe immediately instead of trusting a liveness result cached while disarmed.
    st.active_apps_cached = true;
    st.active_apps_refresh_sec = -1.0;
}

bool triggerLaunchApp(GuiState& st, const std::string& exe_path, const std::string& args,
                      const std::string& custom_name, bool remember) {
    if (!st.client) return false;
    std::string clean_exe = trimStr(exe_path);
    std::string clean_args = trimStr(args);
    if (clean_exe.empty()) {
        showBanner(st, "Please select or drop an executable (.exe) to launch.", true);
        return false;
    }
    if (!st.client->isConnected() || st.client->localVip() == 0) {
        showBanner(st, "Connect to a VPN server first - games are launched with the virtual IP it assigns.", true);
        return false;
    }

    auto res = st.client->launchApp(clean_exe, clean_args);
    if (res.success) {
        if (st.exit_when_game_ends) {
            armExitWhenGameEnds(st, true);
        }
        std::strncpy(st.app_path_buf, clean_exe.c_str(), sizeof(st.app_path_buf) - 1);
        std::strncpy(st.app_args_buf, clean_args.c_str(), sizeof(st.app_args_buf) - 1);
        if (remember) {
            addOrUpdateSavedGame(st, clean_exe, clean_args, custom_name);
        }
        showBanner(st, "Launched PID " + std::to_string(res.pid) + " inside Virtual LAN!", false);
        return true;
    }
    showBanner(st, "Launch failed: " + res.error_msg, true);
    return false;
}


} // namespace apivpn::client
