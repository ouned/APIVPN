#pragma once

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif

#include <winsock2.h>
#include <windows.h>
#include <commdlg.h>
#include <dwmapi.h>
#include <shellapi.h>
#include <tlhelp32.h>
#include <d3d11.h>
#include <dxgi.h>

#include "imgui.h"
#include "imgui_internal.h"
#include "imgui_impl_dx11.h"
#include "imgui_impl_win32.h"

#include "apivpn/client/vpn_client.hpp"
#include "apivpn/common/protocol.hpp"
#include "apivpn/platform/win32_utf8.hpp"

#include "gui_resource_ids.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cctype>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <cwchar>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <optional>
#include <sstream>
#include <string>
#include <system_error>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <vector>

extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam);

namespace apivpn::client {

constexpr UINT WM_TRAYICON  = WM_APP + 1;
constexpr UINT ID_TRAY_ICON = 1;

constexpr UINT IDM_TRAY_RESTORE     = 4001;
constexpr UINT IDM_TRAY_CONNECT     = 4002;
constexpr UINT IDM_TRAY_COPY_VIP    = 4003;
constexpr UINT IDM_TRAY_EXIT        = 4004;
constexpr UINT IDM_TRAY_LAUNCH_BASE = 4100; // 4100..4109 for quick-launch games

// Retries NIM_ADD while the shell's notification area is not ready yet (e.g. at logon).
constexpr UINT_PTR kTrayRetryTimerId   = 1;
constexpr UINT     kTrayRetryIntervalMs = 1000;
constexpr int      kTrayMaxAddAttempts  = 15;

enum class NavTab : int {
    NetworkPeers = 0,
    LobbyChat    = 1,
    GameLauncher = 2,
    Settings     = 3,
};

enum class ThemeMode : int {
    Dark   = 0,
    Light  = 1,
    System = 2,
};

struct SavedGameProfile {
    std::string name;
    std::string exe_path;
    std::string args;
};

struct RunningProcEntry {
    uint32_t    pid = 0;
    std::string name;
    std::string full_path;
};

struct PeerRxPulseState {
    uint64_t last_seen_recv_us = 0;
    double   pulse_start_sec   = -10.0;
};

// Rolling per-peer RTT samples for the hover sparkline (Feature 8)
struct PeerRttHistory {
    std::array<float, 60> samples{};
    size_t   offset          = 0;
    double   last_sample_sec = -10.0;
};

struct GuiState {
    VpnClient* client    = nullptr;
    HWND       hWnd      = nullptr;
    float      dpi_scale = 1.0f;

    // Shader resource view of the embedded app icon, for drawing it in the UI.
    ImTextureID app_icon_tex = 0;

    // Scale helper for DPI-aware pixel dimensions
    float S(float v) const { return std::round(v * dpi_scale); }

    NavTab active_tab = NavTab::NetworkPeers;

    // Input buffers (supports hostnames, IPv6 literals, and IPv4 literals)
    char server_ip_buf[256]   = "localhost";
    char server_port_buf[32]  = "34200";
    char nickname_buf[64]     = "Player1";
    char chat_input_buf[512]  = "";
    char app_path_buf[512]    = "";
    char app_args_buf[256]    = "";
    char profile_name_buf[64] = "";

    bool force_relay         = false;
    bool close_to_tray       = true;
    bool minimize_to_tray    = true;
    bool in_tray             = false;
    bool tray_balloon_shown  = false;
    bool exit_when_game_ends = false;
    // Auto-exit only fires once armed: by a game launched (or still running)
    // while exit_when_game_ends is on, never by a game that ended earlier.
    bool exit_armed          = false;
    bool exit_requested      = false;
    bool focus_chat_input    = false;
    bool focus_app_path      = false;

    // Persisted profiles & history (Feature 1)
    std::vector<std::string>      recent_servers;
    std::vector<SavedGameProfile> saved_games;

    // Running process picker cache (Feature 2)
    std::vector<RunningProcEntry> running_desktop_procs;

    // Toast / Banner feedback
    std::string status_banner_text;
    bool        status_banner_is_error = false;
    double      status_banner_until    = 0.0;
    double      vip_copied_until       = 0.0;
    bool        prev_connected         = false;
    bool        prev_reconnecting      = false;

    // Chat scroll & unread tracking
    size_t last_total_chat_count = 0;
    size_t seen_chat_count       = 0;
    size_t unread_chat_count     = 0;
    // Newest message seen last frame; detects arrivals once the capped history stops growing.
    uint32_t    last_chat_vip = 0;
    std::string last_chat_name;
    std::string last_chat_text;
    bool   scroll_chat_to_bottom = false;

    // Live traffic rate & telemetry sparklines (Feature 5)
    uint64_t last_total_tx_bytes = 0;
    uint64_t last_total_rx_bytes = 0;
    std::chrono::steady_clock::time_point last_rate_sample_time = std::chrono::steady_clock::now();
    float current_tx_kbps = 0.0f;
    float current_rx_kbps = 0.0f;
    std::array<float, 60> tx_rate_history{};
    std::array<float, 60> rx_rate_history{};
    size_t rate_history_offset = 0;
    bool   rate_initialized    = false;

    // Per-peer RX packet pulse animation state
    std::unordered_map<uint32_t, PeerRxPulseState> peer_rx_pulses;

    // Per-process VPN activity pulse & lifecycle state
    std::unordered_map<uint32_t, PeerRxPulseState> proc_activity_pulses;
    std::unordered_map<uint32_t, uint32_t>         cached_proc_exit_codes;
    std::unordered_map<uint32_t, uint64_t>         proc_exited_at_us;

    // Cached process liveness. OpenProcess()/GetExitCodeProcess() are cheap per
    // call but must not run per process on every rendered frame (60 fps), so the
    // status is sampled on a timer and read from this cache while rendering.
    struct CachedProcStatus {
        bool     alive     = false;
        uint32_t exit_code = 0;
    };
    std::unordered_map<uint32_t, CachedProcStatus> proc_status_cache;
    double proc_status_refresh_sec = -1.0;

    // Throttled "is any launched game still running?" result. The check walks the
    // whole system process list (CreateToolhelp32Snapshot), which is far too
    // expensive to run once per frame in --exit-when-game-ends mode.
    bool   active_apps_cached      = true;
    double active_apps_refresh_sec = -1.0;

    // Fonts
    ImFont* font_regular = nullptr;
    ImFont* font_bold    = nullptr;
    ImFont* font_heading = nullptr;
    ImFont* font_mono    = nullptr;

    // Last tray tooltip state
    std::string last_tray_tip;

    // ---- Theme (Feature 14) ----
    ThemeMode theme_mode = ThemeMode::Dark;
    bool      resolved_light = false;
    double    last_theme_check_sec = -10.0;

    // ---- Persisted window placement (Feature 4) ----
    int  win_x          = -1;
    int  win_y          = -1;
    int  win_w          = 0;
    int  win_h          = 0;
    bool win_maximized  = false;
    bool placement_restored = false;

    // ---- Chat scroll / new-message handling (Feature 3) ----
    bool chat_at_bottom     = true;
    bool new_chat_available = false;

    // ---- Chat timestamps & mention state (Feature 10) ----
    std::vector<int64_t> chat_times;      // wall-clock epoch seconds, aligned with chat_messages
    std::string mention_query;            // active "@partial" being typed ("" = none)
    bool        mention_popup_open = false;

    // ---- Peer join/leave notifications (Feature 7) ----
    std::unordered_set<uint32_t>            known_peer_vips;
    std::unordered_map<uint32_t, std::string> known_peer_names;
    bool peer_set_initialized = false;
    bool notify_peer_events   = true;
    bool notify_sounds        = false;

    // ---- Per-peer RTT sparkline history (Feature 8) ----
    std::unordered_map<uint32_t, PeerRttHistory> peer_rtt_history;

    // ---- Table search filters (Feature 11) ----
    char peer_filter_buf[64]  = "";
    char game_filter_buf[64]  = "";
    char proc_filter_buf[64]  = "";

    // ---- Deferred destructive-action confirmations (Feature 5) ----
    bool     open_confirm_kill        = false;
    uint32_t confirm_kill_pid         = 0;
    std::string confirm_kill_name;
    bool     open_confirm_remove_game = false;
    int      confirm_remove_game_idx  = -1;
    std::string confirm_remove_game_name;
    bool     open_confirm_clear_exited = false;

    // ---- Background server connect ----
    // VpnClient::connectToServer() blocks for DNS + up to 3 s per endpoint + the
    // welcome wait, so it runs on connect_thread (one at a time). The worker only
    // writes the two atomics; every other field here is UI-thread only, and the
    // UI polls connect_finished each loop iteration (see tickBackgroundTasks()).
    std::thread       connect_thread;
    std::atomic<bool> connect_finished{false};
    std::atomic<bool> connect_ok{false};
    bool              connect_in_progress      = false;
    bool              connect_cancel_requested = false;
    std::string       connect_host;
    uint16_t          connect_port = 0;

    // ---- Launch queued until the client has a virtual IP (startup --launch) ----
    bool        pending_launch_active    = false;
    bool        pending_launch_warned    = false;
    double      pending_launch_since_sec = 0.0;
    std::string pending_launch_exe;
    std::string pending_launch_args;

    // ---- Tray icon registration (re-added on TaskbarCreated, retried on a timer) ----
    bool tray_icon_added   = false;
    int  tray_add_attempts = 0;

    // ---- Per-monitor DPI change, applied between frames ----
    float pending_dpi_scale = 0.0f;
};

enum class PillTone { Green, TealGreen, Blue, DeepBlue, Slate, Muted, Amber, Orange, Red };
struct PillColors { ImU32 bg; ImU32 fg; };

struct UiPalette {
    ImVec4 accent, success, warning, danger, neutral, muted;
    ImVec4 rate_tx, rate_rx, plot_tx, plot_rx;
    ImVec4 chat_vip, chat_self, chat_other, mention_text;
    ImVec4 toast_bg_ok, toast_bg_err, toast_fg_ok, toast_fg_err;
    ImVec4 vip_btn_bg, vip_btn_hover, vip_btn_bg_off, vip_btn_hover_off;
    ImVec4 btn_blue, btn_blue_hover, btn_blue_active;
    ImVec4 btn_green, btn_green_hover, btn_green_active;
    ImVec4 btn_red, btn_red_hover, btn_red_active;
    ImU32  sb_selected_bg, sb_selected_bar, sb_hover_bg, sb_title_sel, sb_title, sb_subtitle;
    ImU32  mention_bg;
    ImU32  pill_green_bg, pill_green_fg;
    ImU32  pill_teal_bg, pill_teal_fg;
    ImU32  pill_blue_bg, pill_blue_fg;
    ImU32  pill_deepblue_bg, pill_deepblue_fg;
    ImU32  pill_slate_bg, pill_slate_fg;
    ImU32  pill_muted_bg, pill_muted_fg;
    ImU32  pill_amber_bg, pill_amber_fg;
    ImU32  pill_orange_bg, pill_orange_fg;
    ImU32  pill_red_bg, pill_red_fg;
};

// ---- Shared DirectX 11 & palette globals (defined in gui_win32.cpp) ----
extern ID3D11Device*           g_pd3dDevice;
extern ID3D11DeviceContext*    g_pd3dDeviceContext;
extern IDXGISwapChain*         g_pSwapChain;
extern bool                    g_SwapChainOccluded;
extern UINT                    g_ResizeWidth;
extern UINT                    g_ResizeHeight;
extern ID3D11RenderTargetView* g_mainRenderTargetView;
extern UiPalette               g_pal;

// ---- Small inline color helpers ----
inline ImU32 col32(const ImVec4& c) { return ImGui::ColorConvertFloat4ToU32(c); }
inline ImVec4 v4(float r, float g, float b, float a = 1.0f) { return ImVec4(r, g, b, a); }
inline ImU32 c32(int r, int g, int b, int a = 255) { return IM_COL32(r, g, b, a); }

// ---- UTF-8 path helpers ----
// All std::string paths are UTF-8 (see win32_utf8.hpp). A narrow std::filesystem::path
// constructor / .string() uses the ANSI code page on Windows, so always go through these.
inline std::filesystem::path pathFromUtf8(const std::string& utf8) {
    return std::filesystem::path(platform::utf8ToWide(utf8));
}
inline std::string pathToUtf8(const std::filesystem::path& p) {
    return platform::wideToUtf8(p.wstring());
}
inline std::string fileNameUtf8(const std::string& utf8_path) {
    return pathToUtf8(pathFromUtf8(utf8_path).filename());
}

// Makes user/peer text safe to embed in an ImGui label: "##" (and a trailing '#'
// that would merge with a following "##id") is broken up so the text can never
// hide part of itself or change the widget ID.
inline std::string imguiLabelText(const std::string& text) {
    std::string out = text;
    for (size_t pos = out.find("##"); pos != std::string::npos; pos = out.find("##", pos)) {
        out.replace(pos, 2, "# #");
    }
    if (!out.empty() && out.back() == '#') out += ' ';
    return out;
}

// Copies UTF-8 text into a fixed-size wide buffer (tray tooltips, balloons), truncating
// without splitting a surrogate pair. Always NUL-terminates.
inline void copyUtf8ToWideBuf(wchar_t* dst, size_t cap, const std::string& utf8) {
    if (!dst || cap == 0) return;
    std::wstring w = platform::utf8ToWide(utf8);
    size_t n = std::min(w.size(), cap - 1);
    if (n > 0 && n < w.size() && IS_HIGH_SURROGATE(w[n - 1])) --n;
    if (n > 0) std::wmemcpy(dst, w.data(), n);
    dst[n] = L'\0';
}

// Monotonic seconds. ImGui::GetTime() only advances inside NewFrame(), which is not
// called while the window is hidden in the tray, so background timers use this.
inline double monoSec() {
    return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();
}

// ---- Application icon (IDI_APIVPN / IDR_APIVPN_RGBA, from assets/apivpn.rc) ----
constexpr int kAppIconPx = 256;   // side length of the embedded RGBA blob

// LR_SHARED hands back a cached handle owned by the system, so callers must not
// DestroyIcon() the result.  `size` should be SM_CXICON or SM_CXSMICON.
inline HICON loadAppIcon(int size) {
    return static_cast<HICON>(LoadImageA(GetModuleHandleA(nullptr),
                                         MAKEINTRESOURCEA(IDI_APIVPN),
                                         IMAGE_ICON, size, size, LR_SHARED));
}

// Uploads IDR_APIVPN_RGBA into a D3D11 texture for ImGui::Image().  Returns 0
// if the resource is missing or the device failed.  Requires g_pd3dDevice, and
// the caller owns the returned SRV.
ImTextureID createAppIconTexture();

// ---- Cross-translation-unit GUI function declarations ----
std::string trimStr(const std::string& s);
std::string formatBytes(uint64_t bytes);
std::string formatRateKbps(float kbps);
std::string formatConnectedSince(int64_t epoch_sec, uint64_t connected_since_us, uint64_t now_us);
void renderPeerHoverTooltip(const GuiState& st, const ClientStatusSnapshot& snap, const PeerViewInfo& p, const std::string& peer_vip_str);
std::filesystem::path getExeDir();
std::filesystem::path getConfigFilePath();
void showBanner(GuiState& st, const std::string& msg, bool is_error = false, double duration_sec = 4.0);
void copyTextToClipboard(HWND hWnd, const std::string& text);
void saveSettings(const GuiState& st);
void addOrUpdateSavedGame(GuiState& st, const std::string& exe_path, const std::string& args, const std::string& custom_name = "");
void addRecentServer(GuiState& st, const std::string& host, uint16_t port);
void loadSettings(GuiState& st);
void refreshRunningProcesses(GuiState& st);
bool initTrayIcon(HWND hWnd, GuiState& st);
void handleTrayRetryTimer(HWND hWnd, GuiState& st);
void updateTrayTooltip(HWND hWnd, GuiState& st, const ClientStatusSnapshot& snap);
void hideWindowToTray(HWND hWnd, GuiState& st);
void restoreWindowFromTray(HWND hWnd, GuiState& st);
void removeTrayIcon(HWND hWnd);
void captureWindowPlacement(HWND hWnd, GuiState& st);
void showTrayContextMenu(HWND hWnd, GuiState& st);
void triggerConnectOrDisconnect(GuiState& st);
void startConnectJob(GuiState& st, const std::string& host, uint16_t port, const std::string& nick);
void queueLaunchOnConnect(GuiState& st, const std::string& exe_path, const std::string& args);
void tickBackgroundTasks(GuiState& st);
void shutdownBackgroundTasks(GuiState& st);
void armExitWhenGameEnds(GuiState& st, bool app_running);
// Fails (with an error banner) unless the client is connected and has a virtual IP.
// remember=false skips adding the executable to the saved-game library.
bool triggerLaunchApp(GuiState& st, const std::string& exe_path, const std::string& args,
                      const std::string& custom_name = "", bool remember = true);
void CreateRenderTarget();
void CleanupRenderTarget();
bool CreateDeviceD3D(HWND hWnd);
void CleanupDeviceD3D();
void applyThemeSizes();
PillColors pillColors(PillTone tone);
void applyThemeExtraColors(bool light, ImVec4* colors);
void applyThemeColors(bool light);
bool systemPrefersLightTheme();
bool resolvedLightTheme(const GuiState& st);
void applyTheme(GuiState& st);
void applyThemeAndWindow(HWND hWnd, GuiState& st);
void drawPillBadge(const GuiState& st, const char* text, ImU32 bg_col, ImU32 text_col);
void updatePeerRxPulses(GuiState& st, const ClientStatusSnapshot& snap);
void renderPeerRxActivityDotAt(const GuiState& st, ImVec2 top_left, const PeerViewInfo& peer);
bool focusProcessWindow(uint32_t pid);
void openExecutableFolder(const std::string& exe_path);
void refreshProcessStatusCache(GuiState& st, const ClientStatusSnapshot& snap);
void updateProcessActivityPulses(GuiState& st, const ClientStatusSnapshot& snap);
void renderProcessActivityDotAt(const GuiState& st, ImVec2 top_left, uint32_t pid, bool alive);
void renderProcessHoverTooltip( const GuiState& st, const ClientStatusSnapshot& snap, const platform::LaunchedProcessInfo& pr, bool alive, uint32_t exit_code, uint64_t end_or_now_us, const ProcessTrafficViewInfo* tel);
bool drawSidebarNavButton(const GuiState& st, const char* id, const char* label, const char* subtitle, bool selected, int badge_count, ImU32 badge_bg_col, ImU32 badge_txt_col);
void updateTrafficTelemetry(GuiState& st, const ClientStatusSnapshot& snap);
bool containsNoCase(const std::string& haystack, const std::string& needle);
void rightAlignItem(float item_width, float extra_pad = 0.0f);
void updatePeerRttHistory(GuiState& st, const ClientStatusSnapshot& snap);
void renderNetworkPeersView(GuiState& st, const ClientStatusSnapshot& snap);
void renderLobbyChatView(GuiState& st, const ClientStatusSnapshot& snap);
void renderGameLauncherView(GuiState& st, const ClientStatusSnapshot& snap);
void renderSettingsView(GuiState& st, const ClientStatusSnapshot& snap);
void processPeerNotifications(GuiState& st, const ClientStatusSnapshot& snap);
void renderApp(GuiState& st);
LRESULT CALLBACK MainWndProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam);
int runWin32Gui(VpnClient& client, const GuiLaunchOptions& options);

} // namespace apivpn::client
