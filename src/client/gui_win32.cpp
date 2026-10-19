#include "gui_win32_internal.hpp"

// Purpose: Win32/ImGui application shell: window proc, main loop, D3D device, root renderer.

namespace apivpn::client {

// DirectX 11 globals
ID3D11Device*           g_pd3dDevice           = nullptr;
ID3D11DeviceContext*    g_pd3dDeviceContext    = nullptr;
IDXGISwapChain*         g_pSwapChain           = nullptr;
bool                    g_SwapChainOccluded    = false;
UINT                    g_ResizeWidth          = 0;
UINT                    g_ResizeHeight         = 0;
ID3D11RenderTargetView* g_mainRenderTargetView = nullptr;

UiPalette g_pal{};

ImTextureID createAppIconTexture() {
    HRSRC res = FindResourceA(nullptr, MAKEINTRESOURCEA(IDR_APIVPN_RGBA), RT_RCDATA);
    if (!res) return 0;
    const DWORD bytes = SizeofResource(nullptr, res);
    const void* bits = LockResource(LoadResource(nullptr, res));
    if (!bits || bytes < static_cast<DWORD>(kAppIconPx) * kAppIconPx * 4) return 0;
    if (!g_pd3dDevice) return 0;

    D3D11_TEXTURE2D_DESC desc{};
    desc.Width            = kAppIconPx;
    desc.Height           = kAppIconPx;
    desc.MipLevels        = 1;
    desc.ArraySize        = 1;
    desc.Format           = DXGI_FORMAT_R8G8B8A8_UNORM;
    desc.SampleDesc.Count = 1;
    desc.Usage            = D3D11_USAGE_IMMUTABLE;
    desc.BindFlags        = D3D11_BIND_SHADER_RESOURCE;

    D3D11_SUBRESOURCE_DATA sub{};
    sub.pSysMem     = bits;
    sub.SysMemPitch = kAppIconPx * 4;

    ID3D11Texture2D* tex = nullptr;
    if (FAILED(g_pd3dDevice->CreateTexture2D(&desc, &sub, &tex)) || !tex) return 0;

    ID3D11ShaderResourceView* srv = nullptr;
    HRESULT hr = g_pd3dDevice->CreateShaderResourceView(tex, nullptr, &srv);
    tex->Release();
    if (FAILED(hr) || !srv) return 0;

    return static_cast<ImTextureID>(reinterpret_cast<uintptr_t>(srv));
}

std::string trimStr(const std::string& s) {
    size_t start = s.find_first_not_of(" \t\r\n");
    if (start == std::string::npos) return "";
    size_t end = s.find_last_not_of(" \t\r\n");
    return s.substr(start, end - start + 1);
}

std::string formatBytes(uint64_t bytes) {
    char buf[64];
    if (bytes < 1024) {
        std::snprintf(buf, sizeof(buf), "%llu B", static_cast<unsigned long long>(bytes));
    } else if (bytes < 1024 * 1024) {
        std::snprintf(buf, sizeof(buf), "%.1f KB", static_cast<double>(bytes) / 1024.0);
    } else if (bytes < 1024ULL * 1024ULL * 1024ULL) {
        std::snprintf(buf, sizeof(buf), "%.2f MB", static_cast<double>(bytes) / (1024.0 * 1024.0));
    } else {
        std::snprintf(buf, sizeof(buf), "%.2f GB", static_cast<double>(bytes) / (1024.0 * 1024.0 * 1024.0));
    }
    return buf;
}

std::string formatRateKbps(float kbps) {
    char buf[64];
    if (kbps < 1024.0f) {
        std::snprintf(buf, sizeof(buf), "%.1f KB/s", kbps);
    } else {
        std::snprintf(buf, sizeof(buf), "%.2f MB/s", kbps / 1024.0f);
    }
    return buf;
}

std::string formatConnectedSince(int64_t epoch_sec, uint64_t connected_since_us, uint64_t now_us) {
    uint64_t elapsed_sec = 0;
    if (connected_since_us != 0 && now_us >= connected_since_us) {
        elapsed_sec = (now_us - connected_since_us) / 1'000'000ULL;
    }

    char dur_buf[64];
    if (elapsed_sec < 60) {
        std::snprintf(dur_buf, sizeof(dur_buf), "%llus", static_cast<unsigned long long>(elapsed_sec));
    } else if (elapsed_sec < 3600) {
        std::snprintf(dur_buf, sizeof(dur_buf), "%llum %llus",
                      static_cast<unsigned long long>(elapsed_sec / 60),
                      static_cast<unsigned long long>(elapsed_sec % 60));
    } else {
        std::snprintf(dur_buf, sizeof(dur_buf), "%lluh %llum %llus",
                      static_cast<unsigned long long>(elapsed_sec / 3600),
                      static_cast<unsigned long long>((elapsed_sec % 3600) / 60),
                      static_cast<unsigned long long>(elapsed_sec % 60));
    }

    if (epoch_sec > 0) {
        std::time_t tt = static_cast<std::time_t>(epoch_sec);
        std::tm local_tm{};
        if (localtime_s(&local_tm, &tt) == 0) {
            char time_buf[64];
            if (std::strftime(time_buf, sizeof(time_buf), "%H:%M:%S", &local_tm) > 0) {
                return std::string(time_buf) + " (" + dur_buf + ")";
            }
        }
    }
    return std::string(dur_buf) + " ago";
}

void showBanner(GuiState& st, const std::string& msg, bool is_error, double duration_sec) {
    st.status_banner_text = msg;
    st.status_banner_is_error = is_error;
    st.status_banner_until = ImGui::GetTime() + duration_sec;
}

void CreateRenderTarget() {
    ID3D11Texture2D* pBackBuffer = nullptr;
    if (SUCCEEDED(g_pSwapChain->GetBuffer(0, IID_PPV_ARGS(&pBackBuffer))) && pBackBuffer) {
        g_pd3dDevice->CreateRenderTargetView(pBackBuffer, nullptr, &g_mainRenderTargetView);
        pBackBuffer->Release();
    }
}

void CleanupRenderTarget() {
    if (g_mainRenderTargetView) {
        g_mainRenderTargetView->Release();
        g_mainRenderTargetView = nullptr;
    }
}

bool CreateDeviceD3D(HWND hWnd) {
    DXGI_SWAP_CHAIN_DESC sd{};
    sd.BufferCount = 2;
    sd.BufferDesc.Width = 0;
    sd.BufferDesc.Height = 0;
    sd.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    sd.BufferDesc.RefreshRate.Numerator = 60;
    sd.BufferDesc.RefreshRate.Denominator = 1;
    sd.Flags = DXGI_SWAP_CHAIN_FLAG_ALLOW_MODE_SWITCH;
    sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    sd.OutputWindow = hWnd;
    sd.SampleDesc.Count = 1;
    sd.SampleDesc.Quality = 0;
    sd.Windowed = TRUE;
    sd.SwapEffect = DXGI_SWAP_EFFECT_DISCARD;

    UINT createDeviceFlags = 0;
    D3D_FEATURE_LEVEL featureLevel;
    const D3D_FEATURE_LEVEL featureLevelArray[2] = {
        D3D_FEATURE_LEVEL_11_0,
        D3D_FEATURE_LEVEL_10_0,
    };

    HRESULT res = D3D11CreateDeviceAndSwapChain(
        nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, createDeviceFlags,
        featureLevelArray, 2, D3D11_SDK_VERSION, &sd,
        &g_pSwapChain, &g_pd3dDevice, &featureLevel, &g_pd3dDeviceContext);
    if (res == DXGI_ERROR_UNSUPPORTED) {
        res = D3D11CreateDeviceAndSwapChain(
            nullptr, D3D_DRIVER_TYPE_WARP, nullptr, createDeviceFlags,
            featureLevelArray, 2, D3D11_SDK_VERSION, &sd,
            &g_pSwapChain, &g_pd3dDevice, &featureLevel, &g_pd3dDeviceContext);
    }
    if (res != S_OK) return false;

    CreateRenderTarget();
    return true;
}

void CleanupDeviceD3D() {
    CleanupRenderTarget();
    if (g_pSwapChain) { g_pSwapChain->Release(); g_pSwapChain = nullptr; }
    if (g_pd3dDeviceContext) { g_pd3dDeviceContext->Release(); g_pd3dDeviceContext = nullptr; }
    if (g_pd3dDevice) { g_pd3dDevice->Release(); g_pd3dDevice = nullptr; }
}

bool drawSidebarNavButton(const GuiState& st, const char* id, const char* label, const char* subtitle,
                          bool selected, int badge_count, ImU32 badge_bg_col, ImU32 badge_txt_col) {
    ImGuiWindow* window = ImGui::GetCurrentWindow();
    if (window->SkipItems) return false;

    float line_h = ImGui::GetTextLineHeight();
    float pad_y = st.S(8.0f);
    float gap_y = st.S(3.0f);
    float width = ImGui::GetContentRegionAvail().x;
    float height = pad_y * 2.0f + line_h * 2.0f + gap_y;

    ImVec2 pos = ImGui::GetCursorScreenPos();
    ImRect bb(pos, ImVec2(pos.x + width, pos.y + height));

    ImGuiID imgui_id = window->GetID(id);
    ImGui::ItemSize(bb);
    if (!ImGui::ItemAdd(bb, imgui_id)) return false;

    bool hovered = false, held = false;
    bool pressed = ImGui::ButtonBehavior(bb, imgui_id, &hovered, &held);

    ImDrawList* dl = ImGui::GetWindowDrawList();
    if (selected) {
        dl->AddRectFilled(bb.Min, bb.Max, g_pal.sb_selected_bg, st.S(7.0f));
        dl->AddRectFilled(ImVec2(bb.Min.x, bb.Min.y + st.S(7.0f)),
                          ImVec2(bb.Min.x + st.S(4.0f), bb.Max.y - st.S(7.0f)),
                          g_pal.sb_selected_bar, st.S(2.0f));
    } else if (hovered) {
        dl->AddRectFilled(bb.Min, bb.Max, g_pal.sb_hover_bg, st.S(7.0f));
    }

    float text_x = bb.Min.x + st.S(13.0f);
    if (st.font_bold) ImGui::PushFont(st.font_bold);
    ImU32 title_col = selected ? g_pal.sb_title_sel : g_pal.sb_title;
    dl->AddText(ImVec2(text_x, bb.Min.y + pad_y), title_col, label);
    if (st.font_bold) ImGui::PopFont();

    if (subtitle && subtitle[0] != '\0') {
        dl->AddText(ImVec2(text_x, bb.Min.y + pad_y + line_h + gap_y), g_pal.sb_subtitle, subtitle);
    }

    if (badge_count > 0) {
        char btxt[32];
        std::snprintf(btxt, sizeof(btxt), "%d", badge_count);
        ImVec2 bsz = ImGui::CalcTextSize(btxt);
        float bw = std::max(bsz.x + st.S(12.0f), st.S(22.0f));
        float bh = bsz.y + st.S(4.0f);
        ImVec2 bmin(bb.Max.x - bw - st.S(8.0f), bb.Min.y + (height - bh) * 0.5f);
        ImVec2 bmax(bmin.x + bw, bmin.y + bh);
        dl->AddRectFilled(bmin, bmax, badge_bg_col, bh * 0.5f);
        dl->AddText(ImVec2(bmin.x + (bw - bsz.x) * 0.5f, bmin.y + st.S(2.0f)), badge_txt_col, btxt);
    }

    return pressed;
}

bool containsNoCase(const std::string& haystack, const std::string& needle) {
    if (needle.empty()) return true;
    auto it = std::search(haystack.begin(), haystack.end(), needle.begin(), needle.end(),
                          [](char a, char b) { return std::tolower(static_cast<unsigned char>(a)) ==
                                                      std::tolower(static_cast<unsigned char>(b)); });
    return it != haystack.end();
}

void rightAlignItem(float item_width, float extra_pad) {
    float target = ImGui::GetWindowWidth() - item_width - ImGui::GetStyle().WindowPadding.x - extra_pad;
    ImGui::SameLine();
    if (target > ImGui::GetCursorPosX()) {
        ImGui::SetCursorPosX(target);
    }
}

void renderApp(GuiState& st) {
    ClientStatusSnapshot snap = st.client->getSnapshot();
    refreshProcessStatusCache(st, snap);
    updateTrafficTelemetry(st, snap);
    updatePeerRxPulses(st, snap);
    updateProcessActivityPulses(st, snap);
    updatePeerRttHistory(st, snap);
    updateTrayTooltip(st.hWnd, st, snap);

    // "Follow Windows" theme: poll occasionally and re-apply on change (Feature 14)
    if (st.theme_mode == ThemeMode::System && ImGui::GetTime() - st.last_theme_check_sec > 2.0) {
        st.last_theme_check_sec = ImGui::GetTime();
        if (systemPrefersLightTheme() != st.resolved_light) {
            applyThemeAndWindow(st.hWnd, st);
        }
    }

    if (st.prev_connected && !snap.connected && snap.reconnecting) {
        showBanner(st, "Lost connection to VPN server - retrying every 3s...", true, 5.0);
    } else if (st.prev_reconnecting && snap.connected && snap.local_vip != 0) {
        showBanner(st, "Reconnected to " + platform::formatHostPort(snap.server_host, snap.server_port) +
                       " as " + ipv4ToString(snap.local_vip), false, 5.0);
    }
    st.prev_connected = snap.connected;
    st.prev_reconnecting = snap.reconnecting && !snap.connected;

    // Peer join/leave notifications (Feature 7)
    processPeerNotifications(st, snap);

    // Chat bookkeeping: timestamps, unread badge, and scroll policy (Features 3 & 10).
    // VpnClient keeps only the newest messages, so once its history is full the size
    // stops growing; new arrivals are then found by locating the previously newest
    // message (a repeat of the identical text from the same sender can be missed).
    {
        const auto& msgs = snap.chat_messages;
        const size_t now = msgs.size();
        const size_t prev = st.last_total_chat_count;
        auto isLastSeen = [&](const ChatHistoryEntry& m) {
            return m.sender_vip == st.last_chat_vip && m.message == st.last_chat_text &&
                   m.sender_name == st.last_chat_name;
        };

        size_t added = 0;
        if (now > prev) {
            added = now - prev;
        } else if (now > 0 && !isLastSeen(msgs.back())) {
            added = now; // history replaced unless the old newest message is found below
            for (size_t k = now - 1; k-- > 0;) {
                if (isLastSeen(msgs[k])) {
                    added = now - 1 - k;
                    break;
                }
            }
        }

        if (added > 0 || now != st.chat_times.size()) {
            // Keep chat_times aligned with msgs: drop timestamps of messages that fell
            // off the front, then stamp the new ones.
            const int64_t now_epoch = static_cast<int64_t>(std::time(nullptr));
            const size_t keep = (now >= added) ? now - added : 0;
            if (st.chat_times.size() > keep) {
                st.chat_times.erase(st.chat_times.begin(),
                                    st.chat_times.begin() + static_cast<std::ptrdiff_t>(st.chat_times.size() - keep));
            }
            while (st.chat_times.size() < now) st.chat_times.push_back(now_epoch);
        }

        st.last_total_chat_count = now;
        if (now > 0) {
            st.last_chat_vip = msgs.back().sender_vip;
            st.last_chat_name = msgs.back().sender_name;
            st.last_chat_text = msgs.back().message;
        }

        if (added > 0) {
            if (st.chat_at_bottom) st.scroll_chat_to_bottom = true;
            else st.new_chat_available = true;

            if (st.active_tab == NavTab::LobbyChat) {
                st.unread_chat_count = 0;
            } else {
                st.unread_chat_count = std::min(st.unread_chat_count + added, now);
            }
        }
        if (st.active_tab == NavTab::LobbyChat) {
            st.seen_chat_count = now;
        }
    }

    ImGuiIO& io = ImGui::GetIO();
    if (io.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_1, false)) st.active_tab = NavTab::NetworkPeers;
    if (io.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_2, false)) { st.active_tab = NavTab::LobbyChat; st.focus_chat_input = true; }
    if (io.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_3, false)) st.active_tab = NavTab::GameLauncher;
    if (io.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_4, false)) st.active_tab = NavTab::Settings;
    if (io.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_L, false)) {
        st.active_tab = NavTab::GameLauncher;
        st.focus_app_path = true;
    }

    const ImGuiViewport* viewport = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(viewport->WorkPos);
    ImGui::SetNextWindowSize(viewport->WorkSize);

    ImGuiWindowFlags root_flags =
        ImGuiWindowFlags_NoTitleBar |
        ImGuiWindowFlags_NoResize |
        ImGuiWindowFlags_NoMove |
        ImGuiWindowFlags_NoCollapse |
        ImGuiWindowFlags_NoBringToFrontOnFocus |
        ImGuiWindowFlags_NoNavFocus |
        ImGuiWindowFlags_NoScrollbar;

    ImGui::Begin("##APIVPNRoot", nullptr, root_flags);

    // ========================================================================
    // Persistent Compact Top Status Bar (AutoResizeY + Dynamic Right Alignment)
    // ========================================================================
    ImGui::BeginChild("##TopHeaderBar", ImVec2(0, 0),
                      ImGuiChildFlags_Borders | ImGuiChildFlags_AutoResizeY,
                      ImGuiWindowFlags_NoScrollbar);
    {
        ImGui::AlignTextToFramePadding();
        ImDrawList* dl = ImGui::GetWindowDrawList();
        ImVec2 cur = ImGui::GetCursorScreenPos();
        float frame_h = ImGui::GetFrameHeight();

        // Vertically centered status dot
        float dot_y = cur.y + frame_h * 0.5f;
        float dot_x = cur.x + st.S(8.0f);
        const bool busy = snap.reconnecting || (st.connect_in_progress && !snap.connected);
        float pulse = busy ? (0.5f + 0.5f * std::sin(static_cast<float>(ImGui::GetTime()) * 6.0f)) : 1.0f;
        ImU32 dot_col = snap.connected ? IM_COL32(16, 185, 129, 255) :
                        busy           ? IM_COL32(245, 158, 11, static_cast<int>(180.0f + 75.0f * pulse)) :
                                         IM_COL32(239, 68, 68, 255);
        ImU32 glow_col = snap.connected ? IM_COL32(16, 185, 129, 70) :
                         busy           ? IM_COL32(245, 158, 11, static_cast<int>(35.0f + 75.0f * pulse)) :
                                          IM_COL32(239, 68, 68, 50);
        dl->AddCircleFilled(ImVec2(dot_x, dot_y), st.S(8.0f), glow_col);
        dl->AddCircleFilled(ImVec2(dot_x, dot_y), st.S(4.5f), dot_col);

        ImGui::SetCursorPosX(ImGui::GetCursorPosX() + st.S(22.0f));
        if (st.font_bold) ImGui::PushFont(st.font_bold);
        if (snap.connected) {
            ImGui::TextColored(g_pal.success, "CONNECTED");
        } else if (snap.reconnecting) {
            uint64_t now_us = platform::monotonicMicros();
            double rem_sec = 0.0;
            if (snap.last_reconnect_us != 0 && snap.reconnect_interval_us > 0) {
                uint64_t elapsed_us = (now_us >= snap.last_reconnect_us) ? (now_us - snap.last_reconnect_us) : 0;
                if (elapsed_us < snap.reconnect_interval_us) {
                    rem_sec = static_cast<double>(snap.reconnect_interval_us - elapsed_us) / 1'000'000.0;
                }
            }
            char reconn_txt[96];
            if (snap.reconnect_attempts > 0) {
                std::snprintf(reconn_txt, sizeof(reconn_txt), "RECONNECTING (#%u - retry in %.1fs)",
                              snap.reconnect_attempts, rem_sec);
            } else {
                std::snprintf(reconn_txt, sizeof(reconn_txt), "RECONNECTING (retry in %.1fs)", rem_sec);
            }
            ImGui::TextColored(g_pal.warning, "%s", reconn_txt);
        } else if (st.connect_in_progress) {
            ImGui::TextColored(g_pal.warning, "CONNECTING...");
        } else {
            ImGui::TextColored(g_pal.danger, "DISCONNECTED");
        }
        if (st.font_bold) ImGui::PopFont();

        ImGui::SameLine(0, st.S(14.0f));

        // Clickable Virtual IP Button (uses standard Button height so it aligns with text & right button)
        std::string vip_str = snap.connected ? ipv4ToString(snap.local_vip) : "Offline";
        std::string vip_label = "Virtual IP: " + vip_str + (snap.connected ? "/24" : "") + "##TopCopyVip";
        if (st.font_mono) ImGui::PushFont(st.font_mono);
        ImGui::PushStyleColor(ImGuiCol_Button, snap.connected ? g_pal.vip_btn_bg : g_pal.vip_btn_bg_off);
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, snap.connected ? g_pal.vip_btn_hover : g_pal.vip_btn_hover_off);
        if (ImGui::Button(vip_label.c_str()) && snap.connected) {
            copyTextToClipboard(st.hWnd, vip_str);
            st.vip_copied_until = ImGui::GetTime() + 2.0;
            showBanner(st, "Copied Virtual IP (" + vip_str + ") to clipboard!", false, 2.5);
        }
        ImGui::PopStyleColor(2);
        if (st.font_mono) ImGui::PopFont();
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip(ImGui::GetTime() < st.vip_copied_until ? "Copied!" : "Click to copy Virtual IP");
        }

        if (snap.connected) {
            ImGui::SameLine(0, st.S(12.0f));
            ImGui::TextDisabled("Server: %s [%s]",
                                platform::formatHostPort(snap.server_host, snap.server_port).c_str(),
                                snap.server_endpoint.versionString());

            // At-a-glance routing summary (Feature 9)
            int direct_count = 0;
            int relay_count  = 0;
            for (const auto& p : snap.peers) {
                if (p.route_mode == RouteMode::DirectP2P) ++direct_count; else ++relay_count;
            }
            ImGui::SameLine(0, st.S(12.0f));
            if (snap.peers.empty()) {
                ImGui::TextDisabled("| No peers yet");
            } else {
                ImGui::TextDisabled("| %zu peer%s (%d direct, %d relayed)",
                                    snap.peers.size(), snap.peers.size() == 1 ? "" : "s",
                                    direct_count, relay_count);
            }
        }

        // Compute exact width of right-aligned items so the buttons are never clipped
        std::string up_str = "UP " + formatRateKbps(st.current_tx_kbps);
        std::string dn_str = "DN " + formatRateKbps(st.current_rx_kbps);
        const char* tray_btn_str = "Hide to Tray";
        const char* quit_btn_str = "Quit";

        float up_w = ImGui::CalcTextSize(up_str.c_str()).x;
        float dn_w = ImGui::CalcTextSize(dn_str.c_str()).x;
        float tray_btn_w = ImGui::CalcTextSize(tray_btn_str).x + ImGui::GetStyle().FramePadding.x * 2.0f;
        float quit_btn_w = ImGui::CalcTextSize(quit_btn_str).x + ImGui::GetStyle().FramePadding.x * 2.0f;
        float right_total_w = up_w + dn_w + tray_btn_w + quit_btn_w
                            + st.S(40.0f) + ImGui::GetStyle().WindowPadding.x;

        float target_x = ImGui::GetWindowWidth() - right_total_w;
        ImGui::SameLine();
        if (target_x > ImGui::GetCursorPosX()) {
            ImGui::SetCursorPosX(target_x);
        }

        ImGui::TextColored(g_pal.rate_tx, "%s", up_str.c_str());
        ImGui::SameLine(0, st.S(12.0f));
        ImGui::TextColored(g_pal.rate_rx, "%s", dn_str.c_str());
        ImGui::SameLine(0, st.S(12.0f));
        if (ImGui::Button(tray_btn_str)) {
            hideWindowToTray(st.hWnd, st);
        }
        ImGui::SameLine(0, st.S(12.0f));
        if (ImGui::Button(quit_btn_str)) {
            st.exit_requested = true;
            PostMessageW(st.hWnd, WM_CLOSE, 0, 0);
        }
    }
    ImGui::EndChild();

    // Toast notifications are rendered as a non-layout-shifting overlay at the end of this window.
    float toast_top_y = ImGui::GetCursorPosY() + st.S(6.0f);

    // Liveness comes from the timer-refreshed cache (refreshProcessStatusCache above),
    // never from a per-frame OpenProcess() per launched process.
    int active_procs_count = 0;
    for (const auto& pr : snap.launched_processes) {
        auto status_it = st.proc_status_cache.find(pr.pid);
        if (status_it != st.proc_status_cache.end() && status_it->second.alive) {
            ++active_procs_count;
        }
    }

    // ========================================================================
    // Main Body: Left Navigation Rail (DPI-Scaled, No Scrollbar) + Right View
    // ========================================================================
    ImGui::BeginChild("##LeftNavSidebar", ImVec2(st.S(232.0f), 0),
                      ImGuiChildFlags_Borders,
                      ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
    {
        // App icon, centred above the navigation rail.
        if (st.app_icon_tex != 0) {
            const float icon_sz = st.S(76.0f);
            const float indent = (ImGui::GetContentRegionAvail().x - icon_sz) * 0.5f;
            if (indent > 0.0f) ImGui::SetCursorPosX(ImGui::GetCursorPosX() + indent);
            ImGui::Image(st.app_icon_tex, ImVec2(icon_sz, icon_sz));
            ImGui::Spacing();
        }

        ImGui::TextDisabled("NAVIGATION");
        ImGui::Spacing();

        if (drawSidebarNavButton(st, "nav_net", "Network & Peers", "Server & Virtual LAN",
                                 st.active_tab == NavTab::NetworkPeers,
                                 static_cast<int>(snap.peers.size()),
                                 pillColors(PillTone::Blue).bg, pillColors(PillTone::Blue).fg)) {
            st.active_tab = NavTab::NetworkPeers;
        }

        if (drawSidebarNavButton(st, "nav_chat", "Lobby Chat", "Messages & Mentions",
                                 st.active_tab == NavTab::LobbyChat,
                                 static_cast<int>(st.unread_chat_count),
                                 pillColors(PillTone::Amber).bg, pillColors(PillTone::Amber).fg)) {
            st.active_tab = NavTab::LobbyChat;
            st.focus_chat_input = true;
        }

        if (drawSidebarNavButton(st, "nav_games", "Game Launcher", "Saved Apps & Hooks",
                                 st.active_tab == NavTab::GameLauncher,
                                 active_procs_count,
                                 pillColors(PillTone::Green).bg, pillColors(PillTone::Green).fg)) {
            st.active_tab = NavTab::GameLauncher;
        }

        if (drawSidebarNavButton(st, "nav_settings", "Settings & Info", "Relay, Tray & Gateways",
                                 st.active_tab == NavTab::Settings,
                                 0, 0, 0)) {
            st.active_tab = NavTab::Settings;
        }

        // Compute exact height of bottom Quick Actions footer so it never overflows or clips
        bool has_quick_game = !st.saved_games.empty();
        float footer_h = ImGui::GetTextLineHeightWithSpacing() * 2.0f
                       + ImGui::GetFrameHeightWithSpacing() * (has_quick_game ? 2.0f : 1.0f)
                       + st.S(12.0f);
        float space_left = ImGui::GetContentRegionAvail().y - footer_h;
        if (space_left > 0.0f) {
            ImGui::Dummy(ImVec2(0, space_left));
        }

        ImGui::Separator();
        ImGui::TextDisabled("QUICK ACTIONS");
        if (snap.connected || snap.reconnecting) {
            ImGui::PushStyleColor(ImGuiCol_Button, g_pal.btn_red);
            ImGui::PushStyleColor(ImGuiCol_ButtonHovered, g_pal.btn_red_hover);
            if (ImGui::Button(snap.reconnecting ? "Stop Reconnecting" : "Disconnect VPN", ImVec2(-1, 0))) {
                triggerConnectOrDisconnect(st);
            }
            ImGui::PopStyleColor(2);
        } else if (st.connect_in_progress) {
            ImGui::BeginDisabled();
            ImGui::Button("Connecting...##SidebarConnect", ImVec2(-1, 0));
            ImGui::EndDisabled();
        } else {
            ImGui::PushStyleColor(ImGuiCol_Button, g_pal.btn_blue);
            ImGui::PushStyleColor(ImGuiCol_ButtonHovered, g_pal.btn_blue_hover);
            if (ImGui::Button("Quick Connect", ImVec2(-1, 0))) {
                triggerConnectOrDisconnect(st);
            }
            ImGui::PopStyleColor(2);
        }

        if (has_quick_game) {
            std::string ql_label = "Launch: " + imguiLabelText(st.saved_games[0].name) + "###SidebarQuickLaunch";
            const bool can_launch = snap.connected && snap.local_vip != 0;
            ImGui::BeginDisabled(!can_launch);
            ImGui::PushStyleColor(ImGuiCol_Button, g_pal.btn_green);
            ImGui::PushStyleColor(ImGuiCol_ButtonHovered, g_pal.btn_green_hover);
            if (ImGui::Button(ql_label.c_str(), ImVec2(-1, 0))) {
                triggerLaunchApp(st, st.saved_games[0].exe_path, st.saved_games[0].args, st.saved_games[0].name);
            }
            ImGui::PopStyleColor(2);
            ImGui::EndDisabled();
            if (!can_launch && ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
                ImGui::SetTooltip("Connect to a VPN server first: games need an assigned virtual IP.");
            }
        }
        ImGui::TextDisabled("Shortcuts: Ctrl+1..4");
    }
    ImGui::EndChild();

    ImGui::SameLine();

    // ========================================================================
    // Right Focused Content Area
    // ========================================================================
    ImGui::BeginChild("##MainViewContent", ImVec2(0, 0), ImGuiChildFlags_None);
    {
        switch (st.active_tab) {
            case NavTab::NetworkPeers:
                renderNetworkPeersView(st, snap);
                break;
            case NavTab::LobbyChat:
                renderLobbyChatView(st, snap);
                break;
            case NavTab::GameLauncher:
                renderGameLauncherView(st, snap);
                break;
            case NavTab::Settings:
                renderSettingsView(st, snap);
                break;
        }
    }
    ImGui::EndChild();

    // ========================================================================
    // Non-layout-shifting toast overlay (Feature 2) - drawn last so it floats on top
    // ========================================================================
    if (ImGui::GetTime() < st.status_banner_until && !st.status_banner_text.empty()) {
        float pad = st.S(14.0f);
        float max_w = ImGui::GetWindowWidth() * 0.5f;
        float text_w = ImGui::CalcTextSize(st.status_banner_text.c_str()).x;
        float child_w = std::min(max_w, text_w + pad * 2.0f);
        child_w = std::max(child_w, st.S(220.0f));

        ImVec2 saved = ImGui::GetCursorPos();
        ImGui::SetCursorPos(ImVec2(ImGui::GetWindowWidth() - child_w - pad, toast_top_y));

        ImVec4 bg = st.status_banner_is_error ? g_pal.toast_bg_err : g_pal.toast_bg_ok;
        ImVec4 fg = st.status_banner_is_error ? g_pal.toast_fg_err : g_pal.toast_fg_ok;
        ImGui::PushStyleColor(ImGuiCol_ChildBg, bg);
        ImGui::BeginChild("##ToastOverlay", ImVec2(child_w, 0),
                          ImGuiChildFlags_Borders | ImGuiChildFlags_AutoResizeY,
                          ImGuiWindowFlags_NoScrollbar);
        ImGui::PushTextWrapPos(0.0f);
        ImGui::TextColored(fg, "%s", st.status_banner_text.c_str());
        ImGui::PopTextWrapPos();
        ImGui::EndChild();
        ImGui::PopStyleColor();

        ImGui::SetCursorPos(saved);
    }

    ImGui::End();
}

namespace {

// Broadcast by Explorer when the taskbar is (re)created, e.g. after an Explorer
// crash/restart; every tray icon has to be added again at that point.
UINT g_TaskbarCreatedMsg = 0;

// (Re)builds the font atlas for st.dpi_scale. Invalidates all previous ImFont pointers.
void loadFonts(GuiState& st) {
    ImGuiIO& io = ImGui::GetIO();
    io.Fonts->Clear();
    st.font_regular = nullptr;
    st.font_bold = nullptr;
    st.font_heading = nullptr;
    st.font_mono = nullptr;

    float base_font_size = std::round(15.5f * st.dpi_scale);
    if (std::filesystem::exists("C:\\Windows\\Fonts\\segoeui.ttf")) {
        st.font_regular = io.Fonts->AddFontFromFileTTF("C:\\Windows\\Fonts\\segoeui.ttf", base_font_size);
    }
    if (std::filesystem::exists("C:\\Windows\\Fonts\\segoeuib.ttf")) {
        st.font_bold = io.Fonts->AddFontFromFileTTF("C:\\Windows\\Fonts\\segoeuib.ttf", base_font_size);
        st.font_heading = io.Fonts->AddFontFromFileTTF("C:\\Windows\\Fonts\\segoeuib.ttf", std::round(19.5f * st.dpi_scale));
    }
    if (std::filesystem::exists("C:\\Windows\\Fonts\\consola.ttf")) {
        st.font_mono = io.Fonts->AddFontFromFileTTF("C:\\Windows\\Fonts\\consola.ttf", std::round(14.5f * st.dpi_scale));
    }
    if (!st.font_regular) {
        st.font_regular = io.Fonts->AddFontDefault();
    }
}

// Applies a WM_DPICHANGED scale between frames (never inside NewFrame/Render):
// fonts are re-rasterized at the new size and the style sizes rescaled.
void applyPendingDpiChange(GuiState& st) {
    if (st.pending_dpi_scale <= 0.0f) return;
    const float new_scale = st.pending_dpi_scale;
    st.pending_dpi_scale = 0.0f;
    if (std::fabs(new_scale - st.dpi_scale) < 0.01f) return;
    st.dpi_scale = new_scale;
    // Drops the old font texture; ImGui_ImplDX11_NewFrame() recreates it from the new atlas.
    ImGui_ImplDX11_InvalidateDeviceObjects();
    loadFonts(st);
    applyTheme(st); // resets style sizes, then scales them by dpi_scale
}

void releaseAppIconTexture(GuiState& st) {
    if (st.app_icon_tex != 0) {
        reinterpret_cast<ID3D11ShaderResourceView*>(st.app_icon_tex)->Release();
        st.app_icon_tex = 0;
    }
}

// Recovers from DXGI_ERROR_DEVICE_REMOVED / DEVICE_RESET (driver update, TDR, GPU
// switch): tears down the ImGui DX11 backend and every D3D object, then recreates
// them. Returns false if the new device could not be created yet (caller retries).
bool recreateDeviceAfterLoss(HWND hWnd, GuiState& st, bool& backend_ready) {
    if (backend_ready) {
        ImGui_ImplDX11_Shutdown();
        backend_ready = false;
    }
    releaseAppIconTexture(st);
    CleanupDeviceD3D();
    g_SwapChainOccluded = false;
    if (!CreateDeviceD3D(hWnd)) {
        CleanupDeviceD3D();
        return false;
    }
    ImGui_ImplDX11_Init(g_pd3dDevice, g_pd3dDeviceContext);
    backend_ready = true;
    st.app_icon_tex = createAppIconTexture();
    return true;
}

bool isDeviceLost(HRESULT hr) {
    return hr == DXGI_ERROR_DEVICE_REMOVED || hr == DXGI_ERROR_DEVICE_RESET;
}

// Idle wait used while nothing is rendered (tray, minimized, occluded): returns
// as soon as a message arrives instead of always sleeping the full interval.
void idleWait(DWORD ms) {
    MsgWaitForMultipleObjects(0, nullptr, FALSE, ms, QS_ALLINPUT);
}

} // namespace

LRESULT CALLBACK MainWndProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    if (ImGui_ImplWin32_WndProcHandler(hWnd, msg, wParam, lParam)) {
        return true;
    }

    auto* st = reinterpret_cast<GuiState*>(GetWindowLongPtrW(hWnd, GWLP_USERDATA));

    if (g_TaskbarCreatedMsg != 0 && msg == g_TaskbarCreatedMsg) {
        if (st) {
            st->tray_icon_added = false;
            if (!initTrayIcon(hWnd, *st)) {
                st->tray_add_attempts = 0;
                SetTimer(hWnd, kTrayRetryTimerId, kTrayRetryIntervalMs, nullptr);
            }
        }
        return 0;
    }

    switch (msg) {
        case WM_CREATE: {
            auto* cs = reinterpret_cast<CREATESTRUCTW*>(lParam);
            st = reinterpret_cast<GuiState*>(cs->lpCreateParams);
            SetWindowLongPtrW(hWnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(st));
            if (st) {
                st->hWnd = hWnd;
            }
            DragAcceptFiles(hWnd, TRUE);
            return 0;
        }

        case WM_SIZE:
            if (wParam == SIZE_MINIMIZED) {
                if (st && st->minimize_to_tray) {
                    hideWindowToTray(hWnd, *st);
                }
                return 0;
            }
            g_ResizeWidth = static_cast<UINT>(LOWORD(lParam));
            g_ResizeHeight = static_cast<UINT>(HIWORD(lParam));
            return 0;

        case WM_GETMINMAXINFO: {
            auto* mmi = reinterpret_cast<MINMAXINFO*>(lParam);
            float scale = st ? st->dpi_scale : 1.0f;
            mmi->ptMinTrackSize.x = static_cast<LONG>(1040 * scale);
            mmi->ptMinTrackSize.y = static_cast<LONG>(640 * scale);
            return 0;
        }

        case WM_DPICHANGED: {
            // The font/style rebuild is deferred to the main loop (between frames).
            if (st) {
                const float new_scale = static_cast<float>(HIWORD(wParam)) / 96.0f;
                if (new_scale > 0.0f) {
                    st->pending_dpi_scale = new_scale;
                }
            }
            const RECT* suggested = reinterpret_cast<const RECT*>(lParam);
            if (suggested) {
                SetWindowPos(hWnd, nullptr, suggested->left, suggested->top,
                             suggested->right - suggested->left, suggested->bottom - suggested->top,
                             SWP_NOZORDER | SWP_NOACTIVATE);
            }
            return 0;
        }

        case WM_TIMER:
            if (st && wParam == kTrayRetryTimerId) {
                handleTrayRetryTimer(hWnd, *st);
                return 0;
            }
            break;

        case WM_DROPFILES: {
            HDROP hDrop = reinterpret_cast<HDROP>(wParam);
            const UINT len = DragQueryFileW(hDrop, 0, nullptr, 0);
            if (len > 0 && st) {
                std::wstring wpath(static_cast<size_t>(len) + 1, L'\0');
                const UINT got = DragQueryFileW(hDrop, 0, wpath.data(), len + 1);
                wpath.resize(got);
                const std::string dropped_path = platform::wideToUtf8(wpath);
                if (dropped_path.empty()) {
                    // Nothing usable was dropped.
                } else if (dropped_path.size() >= sizeof(st->app_path_buf)) {
                    showBanner(*st, "Dropped path is too long (" + std::to_string(dropped_path.size()) +
                                    " bytes as UTF-8).", true, 4.5);
                } else {
                    std::strncpy(st->app_path_buf, dropped_path.c_str(), sizeof(st->app_path_buf) - 1);
                    st->app_path_buf[sizeof(st->app_path_buf) - 1] = '\0';
                    addOrUpdateSavedGame(*st, dropped_path, st->app_args_buf);
                    st->active_tab = NavTab::GameLauncher;
                    showBanner(*st, "Added executable: " + fileNameUtf8(dropped_path) + " - Ready to Start in VPN!", false, 4.5);
                }
            }
            DragFinish(hDrop);
            return 0;
        }

        case WM_TRAYICON: {
            if (!st) return 0;
            if (lParam == WM_LBUTTONDBLCLK || lParam == WM_LBUTTONUP) {
                restoreWindowFromTray(hWnd, *st);
            } else if (lParam == WM_RBUTTONUP || lParam == WM_CONTEXTMENU) {
                showTrayContextMenu(hWnd, *st);
            }
            return 0;
        }

        case WM_COMMAND: {
            if (!st) return 0;
            UINT cmd = LOWORD(wParam);
            if (cmd == IDM_TRAY_RESTORE) {
                restoreWindowFromTray(hWnd, *st);
            } else if (cmd == IDM_TRAY_CONNECT) {
                triggerConnectOrDisconnect(*st);
            } else if (cmd == IDM_TRAY_COPY_VIP) {
                auto snap = st->client->getSnapshot();
                if (snap.connected) {
                    copyTextToClipboard(hWnd, ipv4ToString(snap.local_vip));
                }
            } else if (cmd == IDM_TRAY_EXIT) {
                st->exit_requested = true;
                DestroyWindow(hWnd);
            } else if (cmd >= IDM_TRAY_LAUNCH_BASE && cmd < IDM_TRAY_LAUNCH_BASE + 10) {
                size_t idx = cmd - IDM_TRAY_LAUNCH_BASE;
                if (idx < st->saved_games.size()) {
                    const SavedGameProfile game = st->saved_games[idx];
                    if (!triggerLaunchApp(*st, game.exe_path, game.args, game.name) && st->in_tray) {
                        // The error banner is only visible with the window shown.
                        restoreWindowFromTray(hWnd, *st);
                    }
                }
            }
            return 0;
        }

        case WM_CLOSE:
            if (st && st->close_to_tray && !st->exit_requested) {
                // Minimizes instead when no tray icon could be registered.
                hideWindowToTray(hWnd, *st);
                return 0;
            }
            DestroyWindow(hWnd);
            return 0;

        case WM_DESTROY:
            KillTimer(hWnd, kTrayRetryTimerId);
            if (st) {
                captureWindowPlacement(hWnd, *st);
                // Always persist, including auto-exit after the game ended.
                saveSettings(*st);
            }
            removeTrayIcon(hWnd);
            PostQuitMessage(0);
            return 0;
    }

    return DefWindowProcW(hWnd, msg, wParam, lParam);
}

int runWin32Gui(VpnClient& client, const GuiLaunchOptions& options) {
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    ImGui_ImplWin32_EnableDpiAwareness();

    GuiState state{};
    state.client = &client;
    state.exit_when_game_ends = options.exit_when_game_ends;
    state.dpi_scale = ImGui_ImplWin32_GetDpiScaleForMonitor(MonitorFromPoint(POINT{0, 0}, MONITOR_DEFAULTTOPRIMARY));
    if (state.dpi_scale <= 0.0f) state.dpi_scale = 1.0f;
    loadSettings(state);

    // Command-line connection settings override the INI for this session.
    if (options.auto_connect) {
        if (!options.server_host.empty()) {
            std::strncpy(state.server_ip_buf, options.server_host.c_str(), sizeof(state.server_ip_buf) - 1);
            state.server_ip_buf[sizeof(state.server_ip_buf) - 1] = '\0';
        }
        if (options.server_port != 0) {
            std::snprintf(state.server_port_buf, sizeof(state.server_port_buf), "%u",
                          static_cast<unsigned>(options.server_port));
        }
        if (!options.nickname.empty()) {
            std::strncpy(state.nickname_buf, options.nickname.c_str(), sizeof(state.nickname_buf) - 1);
            state.nickname_buf[sizeof(state.nickname_buf) - 1] = '\0';
        }
        state.force_relay = options.force_relay;
        client.setForceRelay(state.force_relay);
    }

    g_TaskbarCreatedMsg = RegisterWindowMessageW(L"TaskbarCreated");

    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.style = CS_CLASSDC;
    wc.lpfnWndProc = MainWndProc;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
    wc.hIcon = loadAppIcon(GetSystemMetrics(SM_CXICON));
    wc.hIconSm = loadAppIcon(GetSystemMetrics(SM_CXSMICON));
    wc.lpszClassName = L"APIVPNImGuiWndClass";
    RegisterClassExW(&wc);

    int init_w = static_cast<int>(1280 * state.dpi_scale);
    int init_h = static_cast<int>(800 * state.dpi_scale);
    int init_x = CW_USEDEFAULT;
    int init_y = CW_USEDEFAULT;

    // Restore persisted window placement when it still lands on a visible monitor.
    if (state.win_w > 200 && state.win_h > 200) {
        RECT saved_rect{state.win_x, state.win_y, state.win_x + state.win_w, state.win_y + state.win_h};
        HMONITOR mon = MonitorFromRect(&saved_rect, MONITOR_DEFAULTTONULL);
        if (mon != nullptr) {
            init_x = state.win_x;
            init_y = state.win_y;
            init_w = state.win_w;
            init_h = state.win_h;
            state.placement_restored = true;
        }
    }

    // Clamp initial window size to primary monitor work area if needed
    RECT work_rect{};
    if (SystemParametersInfoW(SPI_GETWORKAREA, 0, &work_rect, 0)) {
        int work_w = work_rect.right - work_rect.left;
        int work_h = work_rect.bottom - work_rect.top;
        if (work_w > 0 && init_w > work_w) init_w = work_w;
        if (work_h > 0 && init_h > work_h) init_h = work_h;
    }

    HWND hWnd = CreateWindowExW(
        WS_EX_ACCEPTFILES,
        wc.lpszClassName,
        L"APIVPN Client - Virtual LAN",
        WS_OVERLAPPEDWINDOW,
        init_x, init_y, init_w, init_h,
        nullptr, nullptr, wc.hInstance, &state);

    if (!hWnd) {
        UnregisterClassW(wc.lpszClassName, wc.hInstance);
        return 1;
    }

    // Let TaskbarCreated through UIPI when this client runs elevated.
    if (g_TaskbarCreatedMsg != 0) {
        ChangeWindowMessageFilterEx(hWnd, g_TaskbarCreatedMsg, MSGFLT_ALLOW, nullptr);
    }

    if (state.placement_restored && state.win_maximized) {
        ShowWindow(hWnd, SW_MAXIMIZE);
    }

    BOOL use_dark_mode = resolvedLightTheme(state) ? FALSE : TRUE;
    DwmSetWindowAttribute(hWnd, 20 /* DWMWA_USE_IMMERSIVE_DARK_MODE */, &use_dark_mode, sizeof(use_dark_mode));
    int corner_pref = 2 /* DWMWCP_ROUND */;
    DwmSetWindowAttribute(hWnd, 33 /* DWMWA_WINDOW_CORNER_PREFERENCE */, &corner_pref, sizeof(corner_pref));

    if (!CreateDeviceD3D(hWnd)) {
        CleanupDeviceD3D();
        UnregisterClassW(wc.lpszClassName, wc.hInstance);
        return 1;
    }

    // The shell may not be ready yet (e.g. autostart at logon): retry on a timer.
    // handleTrayRetryTimer() shows the window if the icon never appears.
    if (!initTrayIcon(hWnd, state)) {
        state.tray_add_attempts = 0;
        SetTimer(hWnd, kTrayRetryTimerId, kTrayRetryIntervalMs, nullptr);
    }
    updateTrayTooltip(hWnd, state, client.getSnapshot());

    if (options.start_in_tray) {
        state.in_tray = true;
        state.tray_balloon_shown = true;
        ShowWindow(hWnd, SW_HIDE);
    } else {
        ShowWindow(hWnd, SW_SHOWDEFAULT);
        UpdateWindow(hWnd);
    }

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
    io.IniFilename = nullptr;

    applyTheme(state);
    loadFonts(state);

    ImGui_ImplWin32_Init(hWnd);
    ImGui_ImplDX11_Init(g_pd3dDevice, g_pd3dDeviceContext);
    bool dx_backend_ready = true;
    state.app_icon_tex = createAppIconTexture();

    // Startup actions from the command line run in the background so the window
    // (or tray icon) is responsive immediately; the launch waits for a virtual IP.
    if (options.auto_connect && !client.isConnected() && !client.isReconnecting()) {
        triggerConnectOrDisconnect(state); // normalizes host/port/nick, then startConnectJob()
    }
    if (!options.launch_exe.empty()) {
        queueLaunchOnConnect(state, options.launch_exe, options.launch_args);
    }

    bool done = false;
    bool device_lost = false;
    while (!done) {
        MSG msg;
        while (PeekMessageW(&msg, nullptr, 0U, 0U, PM_REMOVE)) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
            if (msg.message == WM_QUIT) {
                done = true;
            }
        }
        if (done) break;

        tickBackgroundTasks(state);

        if (state.exit_when_game_ends && state.exit_armed && state.client) {
            // The liveness probe enumerates the whole system process list, so run
            // it on a timer instead of once per rendered frame. monoSec() keeps
            // ticking while hidden in the tray (ImGui::GetTime() does not).
            constexpr double kActiveAppsCheckSec = 0.5;
            double now_sec = monoSec();
            if (state.active_apps_refresh_sec < 0.0 ||
                (now_sec - state.active_apps_refresh_sec) >= kActiveAppsCheckSec) {
                state.active_apps_refresh_sec = now_sec;
                state.active_apps_cached = state.client->hasActiveLaunchedApps();
            }
            if (!state.active_apps_cached) {
                state.exit_requested = true;
                DestroyWindow(hWnd);
                continue;
            }
        }

        if (device_lost) {
            if (!recreateDeviceAfterLoss(hWnd, state, dx_backend_ready)) {
                idleWait(500);
                continue;
            }
            device_lost = false;
        }

        if (state.in_tray || IsIconic(hWnd) ||
            (g_SwapChainOccluded && g_pSwapChain->Present(0, DXGI_PRESENT_TEST) == DXGI_STATUS_OCCLUDED)) {
            if (state.in_tray && state.client) {
                updateTrayTooltip(hWnd, state, state.client->getSnapshot());
            }
            idleWait(30);
            continue;
        }
        g_SwapChainOccluded = false;

        applyPendingDpiChange(state);

        if (g_ResizeWidth != 0 && g_ResizeHeight != 0) {
            CleanupRenderTarget();
            HRESULT rhr = g_pSwapChain->ResizeBuffers(0, g_ResizeWidth, g_ResizeHeight, DXGI_FORMAT_UNKNOWN, 0);
            g_ResizeWidth = g_ResizeHeight = 0;
            if (SUCCEEDED(rhr)) {
                CreateRenderTarget();
            } else if (isDeviceLost(rhr)) {
                device_lost = true;
                continue;
            }
        }
        if (!g_mainRenderTargetView) {
            // A failed resize keeps the old buffers; try to target those instead.
            CreateRenderTarget();
            if (!g_mainRenderTargetView) {
                idleWait(30);
                continue;
            }
        }

        ImGui_ImplDX11_NewFrame();
        ImGui_ImplWin32_NewFrame();
        ImGui::NewFrame();

        renderApp(state);

        ImGui::Render();
        const ImVec4 clear_color = state.resolved_light
            ? ImVec4(0.94f, 0.95f, 0.97f, 1.00f)
            : ImVec4(0.06f, 0.08f, 0.11f, 1.00f);
        const float clear_color_with_alpha[4] = {
            clear_color.x * clear_color.w,
            clear_color.y * clear_color.w,
            clear_color.z * clear_color.w,
            clear_color.w
        };
        g_pd3dDeviceContext->OMSetRenderTargets(1, &g_mainRenderTargetView, nullptr);
        g_pd3dDeviceContext->ClearRenderTargetView(g_mainRenderTargetView, clear_color_with_alpha);
        ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());

        HRESULT hr = g_pSwapChain->Present(1, 0);
        if (isDeviceLost(hr)) {
            device_lost = true;
        }
        g_SwapChainOccluded = (hr == DXGI_STATUS_OCCLUDED);
    }

    // Join a still-running connect attempt before the GuiState (and its thread) go away.
    shutdownBackgroundTasks(state);

    if (dx_backend_ready) {
        ImGui_ImplDX11_Shutdown();
    }
    ImGui_ImplWin32_Shutdown();
    ImGui::DestroyContext();

    // Release the icon SRV before the device goes away, so it is not left
    // holding the last reference.
    releaseAppIconTexture(state);

    CleanupDeviceD3D();
    if (IsWindow(hWnd)) {
        DestroyWindow(hWnd);
    }
    UnregisterClassW(wc.lpszClassName, wc.hInstance);

    return 0;
}


} // namespace apivpn::client
