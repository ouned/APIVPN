#include "gui_win32_internal.hpp"

// Purpose: Sidebar views: network/peers, lobby chat, game launcher, and settings panels.

namespace apivpn::client {

void renderPeerHoverTooltip(const GuiState& st, const ClientStatusSnapshot& snap, const PeerViewInfo& p, const std::string& peer_vip_str) {
    uint64_t now_us = platform::monotonicMicros();
    ImGui::BeginTooltip();

    if (st.font_bold) ImGui::PushFont(st.font_bold);
    ImGui::Text("%s (%s)", p.nickname.c_str(), peer_vip_str.c_str());
    if (st.font_bold) ImGui::PopFont();
    ImGui::Separator();

    // Connected Since
    ImGui::TextDisabled("Connected Since:");
    ImGui::SameLine();
    ImGui::TextUnformatted(formatConnectedSince(p.connected_epoch_sec, p.connected_since_us, now_us).c_str());

    // Last Activity (idle indicator — spot a stalled peer at a glance)
    ImGui::TextDisabled("Last Packet:");
    ImGui::SameLine();
    if (p.last_recv_us != 0 && now_us >= p.last_recv_us) {
        double idle_sec = static_cast<double>(now_us - p.last_recv_us) / 1'000'000.0;
        char idle_buf[64];
        if (idle_sec < 1.0) {
            std::snprintf(idle_buf, sizeof(idle_buf), "just now");
        } else if (idle_sec < 60.0) {
            std::snprintf(idle_buf, sizeof(idle_buf), "%.1f s ago", idle_sec);
        } else if (idle_sec < 3600.0) {
            std::snprintf(idle_buf, sizeof(idle_buf), "%.0f m %.0f s ago",
                          idle_sec / 60.0, std::fmod(idle_sec, 60.0));
        } else {
            std::snprintf(idle_buf, sizeof(idle_buf), "%.0f h %.0f m ago",
                          idle_sec / 3600.0, std::fmod(idle_sec, 3600.0) / 60.0);
        }
        // Healthy peers still exchange a keep-alive every kTunnelKeepAliveIntervalUs,
        // so anything older than that (plus slack) is either idle or stalled.
        constexpr double kFreshSec = static_cast<double>(kTunnelKeepAliveIntervalUs) / 1'000'000.0 + 1.0;
        constexpr double kStaleSec = static_cast<double>(kTunnelKeepAliveTimeoutUs) / 1'000'000.0;
        ImVec4 idle_col = (idle_sec < kFreshSec) ? g_pal.success :
                          (idle_sec < kStaleSec) ? g_pal.warning :
                                                    g_pal.danger;
        ImGui::TextColored(idle_col, "%s", idle_buf);
    } else {
        ImGui::TextDisabled("No traffic yet");
    }

    // Active Endpoint
    ImGui::TextDisabled("Active Endpoint:");
    ImGui::SameLine();
    if (p.route_mode == RouteMode::DirectP2P && p.active_endpoint.port_host != 0) {
        if (st.font_mono) ImGui::PushFont(st.font_mono);
        ImGui::TextColored(g_pal.success, "%s (%s)",
                           p.active_endpoint.toString().c_str(),
                           p.active_endpoint.versionString());
        if (st.font_mono) ImGui::PopFont();
    } else {
        if (snap.server_endpoint.port_host != 0) {
            ImGui::TextColored(g_pal.warning, "Relayed via Server (%s)",
                               snap.server_endpoint.toString().c_str());
        } else {
            ImGui::TextColored(g_pal.warning, "Relayed via Server");
        }
    }

    // Best Direct / Fallback Endpoints (if known)
    bool has_best_v6 = (p.best_v6_endpoint.port_host != 0);
    bool has_best_v4 = (p.best_v4_endpoint.port_host != 0);
    if (has_best_v6 || has_best_v4) {
        ImGui::Spacing();
        ImGui::TextDisabled("Verified P2P Routes (Active / Fallback):");
        if (has_best_v6) {
            bool is_active = (p.route_mode == RouteMode::DirectP2P && p.active_endpoint == p.best_v6_endpoint);
            if (st.font_mono) ImGui::PushFont(st.font_mono);
            ImGui::TextColored(is_active ? g_pal.success : g_pal.muted,
                               "  [IPv6] %s (%u ms)%s",
                               p.best_v6_endpoint.toString().c_str(),
                               p.rtt_v6_ms,
                               is_active ? " [Active]" : " [Fallback]");
            if (st.font_mono) ImGui::PopFont();
        }
        if (has_best_v4) {
            bool is_active = (p.route_mode == RouteMode::DirectP2P && p.active_endpoint == p.best_v4_endpoint);
            if (st.font_mono) ImGui::PushFont(st.font_mono);
            ImGui::TextColored(is_active ? g_pal.success : g_pal.muted,
                               "  [IPv4] %s (%u ms)%s",
                               p.best_v4_endpoint.toString().c_str(),
                               p.rtt_v4_ms,
                               is_active ? " [Active]" : " [Fallback]");
            if (st.font_mono) ImGui::PopFont();
        }
    }

    // Hole-punch diagnostics: explains why traffic is still relayed
    if (p.route_mode != RouteMode::DirectP2P) {
        ImGui::Spacing();
        ImGui::TextDisabled("Hole Punch:");
        ImGui::SameLine();
        if (p.hole_punch_gave_up) {
            ImGui::TextColored(g_pal.warning, "Gave up after %u cycles (%llu probes)",
                               p.hole_punch_cycles,
                               static_cast<unsigned long long>(p.punch_probes_sent));
        } else {
            ImGui::Text("%u cycles, %llu probes sent",
                        p.hole_punch_cycles,
                        static_cast<unsigned long long>(p.punch_probes_sent));
        }
        ImGui::TextDisabled("  %zu candidate endpoint(s) advertised", p.candidate_endpoints.size());
    }

    // RTT history sparkline (Feature 8)
    auto rtt_it = st.peer_rtt_history.find(p.vip);
    if (rtt_it != st.peer_rtt_history.end()) {
        const auto& h = rtt_it->second;
        float max_rtt = 0.0f;
        float min_rtt = 0.0f;
        float sum_rtt = 0.0f;
        int   n_rtt   = 0;
        for (float v : h.samples) {
            if (v <= 0.0f) continue;
            if (v > max_rtt) max_rtt = v;
            if (n_rtt == 0 || v < min_rtt) min_rtt = v;
            sum_rtt += v;
            ++n_rtt;
        }
        const float scale_max = (max_rtt > 20.0f) ? max_rtt : 20.0f;
        ImGui::Spacing();
        ImGui::TextDisabled("Ping History (last 30s):");
        ImGui::PushStyleColor(ImGuiCol_PlotLines, g_pal.plot_rx);
        ImGui::PlotLines("##PeerRttHistory", h.samples.data(), static_cast<int>(h.samples.size()),
                         static_cast<int>(h.offset), nullptr, 0.0f, scale_max, ImVec2(st.S(220.0f), st.S(38.0f)));
        ImGui::PopStyleColor();
        if (n_rtt > 0) {
            ImGui::TextDisabled("  Min %u ms  |  Avg %u ms  |  Peak %u ms  |  Current %u ms",
                                static_cast<unsigned>(min_rtt),
                                static_cast<unsigned>(sum_rtt / static_cast<float>(n_rtt)),
                                static_cast<unsigned>(max_rtt),
                                p.rtt_ms);
        } else {
            ImGui::TextDisabled("  No samples yet");
        }
    }

    ImGui::EndTooltip();
}

void updatePeerRxPulses(GuiState& st, const ClientStatusSnapshot& snap) {
    double now_sec = ImGui::GetTime();
    uint64_t now_us = platform::monotonicMicros();
    // 140ms bright flash + 80ms dim recovery gap so high-rate packet streams visibly blink/pulse
    constexpr double kPulseCycleSec = 0.22;

    for (const auto& p : snap.peers) {
        auto& ps = st.peer_rx_pulses[p.vip];
        if (ps.last_seen_recv_us == 0) {
            ps.last_seen_recv_us = p.last_recv_us;
            if (p.last_recv_us != 0 && now_us >= p.last_recv_us && (now_us - p.last_recv_us) < 200'000) {
                ps.pulse_start_sec = now_sec;
            }
        } else if (p.last_recv_us != ps.last_seen_recv_us) {
            if (now_sec - ps.pulse_start_sec >= kPulseCycleSec) {
                ps.pulse_start_sec = now_sec;
                ps.last_seen_recv_us = p.last_recv_us;
            }
        }
    }
}

void renderPeerRxActivityDotAt(const GuiState& st, ImVec2 top_left, const PeerViewInfo& peer) {
    constexpr double kFlashDurationSec = 0.14; // 140ms flash duration
    float flash = 0.0f;
    auto it = st.peer_rx_pulses.find(peer.vip);
    if (it != st.peer_rx_pulses.end()) {
        double elapsed = ImGui::GetTime() - it->second.pulse_start_sec;
        if (elapsed >= 0.0 && elapsed < kFlashDurationSec) {
            float t = static_cast<float>(1.0 - (elapsed / kFlashDurationSec));
            flash = t * t;
        }
    }

    float line_h = ImGui::GetTextLineHeight();
    float box_w = st.S(16.0f);
    ImVec2 center(top_left.x + box_w * 0.5f, top_left.y + line_h * 0.5f);

    ImDrawList* dl = ImGui::GetWindowDrawList();
    if (flash > 0.01f) {
        int glow_alpha = static_cast<int>(135.0f * flash);
        float glow_radius = st.S(3.8f + 2.2f * flash); // Max 6.0f, stays well inside 16x16 box
        dl->AddCircleFilled(center, glow_radius, IM_COL32(60, 255, 155, glow_alpha), 24);
    }

    int r = static_cast<int>(20.0f + (105.0f - 20.0f) * flash);
    int g = static_cast<int>(135.0f + (255.0f - 135.0f) * flash);
    int b = static_cast<int>(75.0f + (180.0f - 75.0f) * flash);
    int a = static_cast<int>(220.0f + (255.0f - 220.0f) * flash);
    float core_radius = st.S(3.4f + 0.8f * flash);
    dl->AddCircleFilled(center, core_radius, IM_COL32(r, g, b, a), 24);
}

bool focusProcessWindow(uint32_t pid) {
    if (pid == 0) return false;
    struct EnumCtx {
        DWORD target_pid = 0;
        HWND  found_hwnd = nullptr;
    } ctx{static_cast<DWORD>(pid), nullptr};

    EnumWindows([](HWND hWnd, LPARAM lParam) -> BOOL {
        auto* c = reinterpret_cast<EnumCtx*>(lParam);
        DWORD wnd_pid = 0;
        GetWindowThreadProcessId(hWnd, &wnd_pid);
        if (wnd_pid == c->target_pid && IsWindowVisible(hWnd) && GetWindow(hWnd, GW_OWNER) == nullptr) {
            c->found_hwnd = hWnd;
            return FALSE;
        }
        return TRUE;
    }, reinterpret_cast<LPARAM>(&ctx));

    if (!ctx.found_hwnd) return false;
    if (IsIconic(ctx.found_hwnd)) {
        ShowWindow(ctx.found_hwnd, SW_RESTORE);
    }
    SetForegroundWindow(ctx.found_hwnd);
    return true;
}

void openExecutableFolder(const std::string& exe_path) {
    if (exe_path.empty()) return;
    std::filesystem::path p = pathFromUtf8(exe_path);
    std::filesystem::path dir = p.has_parent_path() ? p.parent_path() : getExeDir();
    ShellExecuteW(nullptr, L"open", dir.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
}

// Samples process liveness on a timer and caches it for the renderers. Called
// once per frame but only performs the (comparatively expensive) OpenProcess /
// GetExitCodeProcess round-trips at most twice per second, or immediately when a
// process that is not in the cache yet shows up.
void refreshProcessStatusCache(GuiState& st, const ClientStatusSnapshot& snap) {
    constexpr double kRefreshIntervalSec = 0.5;
    const double now_sec = ImGui::GetTime();

    bool cache_complete = true;
    for (const auto& pr : snap.launched_processes) {
        if (pr.pid == 0) continue;
        if (st.proc_status_cache.find(pr.pid) == st.proc_status_cache.end()) {
            cache_complete = false;
            break;
        }
    }
    if (cache_complete && st.proc_status_refresh_sec >= 0.0 &&
        (now_sec - st.proc_status_refresh_sec) < kRefreshIntervalSec) {
        return;
    }
    st.proc_status_refresh_sec = now_sec;

    const uint64_t now_us = platform::monotonicMicros();
    std::unordered_set<uint32_t> live;
    for (const auto& pr : snap.launched_processes) {
        if (pr.pid == 0) continue;
        live.insert(pr.pid);

        bool alive = false;
        uint32_t exit_code = 0;
        auto& cached = st.proc_status_cache[pr.pid];
        if (platform::ProcessLauncher::getProcessStatus(pr.pid, alive, exit_code)) {
            cached.alive = alive;
            cached.exit_code = exit_code;
        } else {
            // Query failed (e.g. the process is already gone): fall back to the
            // last exit code we managed to observe.
            cached.alive = false;
            auto ec_it = st.cached_proc_exit_codes.find(pr.pid);
            cached.exit_code = (ec_it != st.cached_proc_exit_codes.end()) ? ec_it->second : 0;
        }

        if (!cached.alive) {
            st.cached_proc_exit_codes[pr.pid] = cached.exit_code;
            if (st.proc_exited_at_us.find(pr.pid) == st.proc_exited_at_us.end()) {
                st.proc_exited_at_us[pr.pid] = now_us;
            }
        }
    }

    // Drop cache entries for processes that are no longer tracked.
    for (auto it = st.proc_status_cache.begin(); it != st.proc_status_cache.end(); ) {
        if (live.find(it->first) == live.end()) {
            it = st.proc_status_cache.erase(it);
        } else {
            ++it;
        }
    }
}

void updateProcessActivityPulses(GuiState& st, const ClientStatusSnapshot& snap) {
    double now_sec = ImGui::GetTime();
    uint64_t now_us = platform::monotonicMicros();
    constexpr double kPulseCycleSec = 0.22;

    for (const auto& pr : snap.launched_processes) {
        if (pr.pid == 0) continue;

        auto tel_it = snap.process_telemetry.find(pr.pid);
        if (tel_it != snap.process_telemetry.end()) {
            uint64_t act_us = tel_it->second.last_activity_us;
            auto& ps = st.proc_activity_pulses[pr.pid];
            if (ps.last_seen_recv_us == 0) {
                ps.last_seen_recv_us = act_us;
                if (act_us != 0 && now_us >= act_us && (now_us - act_us) < 200'000) {
                    ps.pulse_start_sec = now_sec;
                }
            } else if (act_us != ps.last_seen_recv_us) {
                if (now_sec - ps.pulse_start_sec >= kPulseCycleSec) {
                    ps.pulse_start_sec = now_sec;
                    ps.last_seen_recv_us = act_us;
                }
            }
        }
    }
}

void renderProcessActivityDotAt(const GuiState& st, ImVec2 top_left, uint32_t pid, bool alive) {
    float line_h = ImGui::GetTextLineHeight();
    float box_w = st.S(16.0f);
    ImVec2 center(top_left.x + box_w * 0.5f, top_left.y + line_h * 0.5f);
    ImDrawList* dl = ImGui::GetWindowDrawList();

    if (!alive) {
        dl->AddCircleFilled(center, st.S(3.2f), IM_COL32(105, 115, 130, 170), 20);
        return;
    }

    constexpr double kFlashDurationSec = 0.14;
    float flash = 0.0f;
    auto it = st.proc_activity_pulses.find(pid);
    if (it != st.proc_activity_pulses.end()) {
        double elapsed = ImGui::GetTime() - it->second.pulse_start_sec;
        if (elapsed >= 0.0 && elapsed < kFlashDurationSec) {
            float t = static_cast<float>(1.0 - (elapsed / kFlashDurationSec));
            flash = t * t;
        }
    }

    if (flash > 0.01f) {
        int glow_alpha = static_cast<int>(135.0f * flash);
        float glow_radius = st.S(3.8f + 2.2f * flash);
        dl->AddCircleFilled(center, glow_radius, IM_COL32(60, 255, 155, glow_alpha), 24);
    }

    int r = static_cast<int>(20.0f + (105.0f - 20.0f) * flash);
    int g = static_cast<int>(135.0f + (255.0f - 135.0f) * flash);
    int b = static_cast<int>(75.0f + (180.0f - 75.0f) * flash);
    int a = static_cast<int>(220.0f + (255.0f - 220.0f) * flash);
    float core_radius = st.S(3.4f + 0.8f * flash);
    dl->AddCircleFilled(center, core_radius, IM_COL32(r, g, b, a), 24);
}

void renderProcessHoverTooltip(
    const GuiState& st,
    const ClientStatusSnapshot& snap,
    const platform::LaunchedProcessInfo& pr,
    bool alive,
    uint32_t exit_code,
    uint64_t end_or_now_us,
    const ProcessTrafficViewInfo* tel) {

    std::string short_name = fileNameUtf8(pr.exe_path);
    if (short_name.empty()) short_name = pr.exe_path;

    ImGui::BeginTooltip();
    if (st.font_bold) ImGui::PushFont(st.font_bold);
    ImGui::Text("%s (PID %u)", short_name.c_str(), pr.pid);
    if (st.font_bold) ImGui::PopFont();
    ImGui::Separator();

    // Status & Exit Code
    ImGui::TextDisabled("Process Status:");
    ImGui::SameLine();
    if (alive) {
        ImGui::TextColored(g_pal.success, "Running (Winsock Hook Active)");
    } else if (exit_code == 0) {
        ImGui::TextColored(g_pal.neutral, "Exited Cleanly (Code 0)");
    } else {
        ImGui::TextColored(g_pal.danger, "Exited with Code %u (0x%08X)", exit_code, exit_code);
    }

    // Architecture & Parent PID
    ImGui::TextDisabled("Architecture:");
    ImGui::SameLine();
    ImGui::Text("%s",
                (pr.arch == platform::BinaryArch::X86_64) ? "64-bit (x64 — apivpn_hook64.dll)" :
                (pr.arch == platform::BinaryArch::X86_32) ? "32-bit (x86 — apivpn_hook32.dll)" : "Unknown");
    if (pr.parent_pid != 0) {
        ImGui::TextDisabled("Parent Process:");
        ImGui::SameLine();
        ImGui::TextColored(g_pal.accent, "Spawned by PID %u (Child Process Hook)", pr.parent_pid);
    }

    // Launched Since / Duration
    if (pr.launched_since_us != 0) {
        ImGui::TextDisabled(alive ? "Launched Since:" : "Ran For:");
        ImGui::SameLine();
        ImGui::TextUnformatted(formatConnectedSince(pr.launched_epoch_sec, pr.launched_since_us, end_or_now_us).c_str());
    }

    // Injected Virtual IP
    if (pr.injected_vip != 0) {
        ImGui::TextDisabled("Assigned Virtual IP:");
        ImGui::SameLine();
        bool vip_mismatch = snap.connected && snap.local_vip != 0 && pr.injected_vip != snap.local_vip;
        if (st.font_mono) ImGui::PushFont(st.font_mono);
        if (vip_mismatch) {
            ImGui::TextColored(g_pal.warning, "%s (Client is now %s — relaunch recommended)",
                               ipv4ToString(pr.injected_vip).c_str(),
                               ipv4ToString(snap.local_vip).c_str());
        } else {
            ImGui::TextColored(g_pal.accent, "%s", ipv4ToString(pr.injected_vip).c_str());
        }
        if (st.font_mono) ImGui::PopFont();
    }

    // Executable Path, Args, Working Dir
    ImGui::Spacing();
    ImGui::TextDisabled("Executable:");
    ImGui::SameLine();
    if (st.font_mono) ImGui::PushFont(st.font_mono);
    ImGui::TextUnformatted(pr.exe_path.c_str());
    if (st.font_mono) ImGui::PopFont();

    if (!pr.args.empty()) {
        ImGui::TextDisabled("Arguments:");
        ImGui::SameLine();
        if (st.font_mono) ImGui::PushFont(st.font_mono);
        ImGui::TextUnformatted(pr.args.c_str());
        if (st.font_mono) ImGui::PopFont();
    }
    if (!pr.working_dir.empty()) {
        ImGui::TextDisabled("Working Dir:");
        ImGui::SameLine();
        if (st.font_mono) ImGui::PushFont(st.font_mono);
        ImGui::TextUnformatted(pr.working_dir.c_str());
        if (st.font_mono) ImGui::PopFont();
    }

    // VPN Traffic
    uint64_t tx_b = tel ? tel->tx_bytes : 0;
    uint64_t rx_b = tel ? tel->rx_bytes : 0;
    uint64_t tx_p = tel ? tel->tx_packets : 0;
    uint64_t rx_p = tel ? tel->rx_packets : 0;
    ImGui::Spacing();
    ImGui::TextDisabled("VPN Traffic:");
    ImGui::SameLine();
    ImGui::Text("TX: %s (%llu pkts)  |  RX: %s (%llu pkts)",
                formatBytes(tx_b).c_str(), static_cast<unsigned long long>(tx_p),
                formatBytes(rx_b).c_str(), static_cast<unsigned long long>(rx_p));

    // Bound Virtual Ports
    ImGui::Spacing();
    size_t bound_count = tel ? tel->bound_ports.size() : 0;
    ImGui::TextDisabled("Bound Virtual Sockets (%zu):", bound_count);
    if (bound_count == 0) {
        ImGui::TextDisabled("  None currently bound");
    } else {
        std::string vip_s = ipv4ToString(pr.injected_vip != 0 ? pr.injected_vip : snap.local_vip);
        if (st.font_mono) ImGui::PushFont(st.font_mono);
        for (const auto& bp : tel->bound_ports) {
            bool is_tcp = (bp.proto == IpcProto::Tcp);
            ImVec4 col = is_tcp ? g_pal.accent : g_pal.success;
            ImGui::TextColored(col, "  [%s] %s:%u  ->  127.0.0.1:%u",
                               is_tcp ? "TCP" : "UDP",
                               vip_s.c_str(),
                               bp.virtual_port,
                               bp.actual_port);
        }
        if (st.font_mono) ImGui::PopFont();
    }

    // Active Virtual TCP Tunnels
    size_t tcp_count = tel ? tel->tcp_tunnels.size() : 0;
    if (tcp_count > 0) {
        ImGui::Spacing();
        ImGui::TextDisabled("Active Virtual TCP Streams (%zu):", tcp_count);
        if (st.font_mono) ImGui::PushFont(st.font_mono);
        for (const auto& tv : tel->tcp_tunnels) {
            std::string peer_label = ipv4ToString(tv.remote_vip);
            for (const auto& p : snap.peers) {
                if (p.vip == tv.remote_vip && !p.nickname.empty()) {
                    peer_label = p.nickname + " (" + peer_label + ")";
                    break;
                }
            }
            ImGui::TextColored(g_pal.accent,
                               "  Local :%u <-> %s:%u  [%s | RTT: %u ms | CWND: %u]",
                               tv.local_vport,
                               peer_label.c_str(),
                               tv.remote_vport,
                               tv.handshake_done ? "ESTABLISHED" : "SYN_SENT",
                               tv.rtt_ms,
                               tv.cwnd_segments);
        }
        if (st.font_mono) ImGui::PopFont();
    }

    ImGui::EndTooltip();
}

void updateTrafficTelemetry(GuiState& st, const ClientStatusSnapshot& snap) {
    uint64_t total_tx_bytes = 0;
    uint64_t total_rx_bytes = 0;
    for (const auto& p : snap.peers) {
        total_tx_bytes += p.tx_bytes;
        total_rx_bytes += p.rx_bytes;
    }

    auto now = std::chrono::steady_clock::now();
    if (!st.rate_initialized) {
        st.last_total_tx_bytes = total_tx_bytes;
        st.last_total_rx_bytes = total_rx_bytes;
        st.last_rate_sample_time = now;
        st.rate_initialized = true;
        return;
    }

    double elapsed_sec = std::chrono::duration<double>(now - st.last_rate_sample_time).count();
    if (elapsed_sec >= 0.25) {
        uint64_t d_tx = (total_tx_bytes >= st.last_total_tx_bytes) ? (total_tx_bytes - st.last_total_tx_bytes) : 0;
        uint64_t d_rx = (total_rx_bytes >= st.last_total_rx_bytes) ? (total_rx_bytes - st.last_total_rx_bytes) : 0;

        st.current_tx_kbps = static_cast<float>((static_cast<double>(d_tx) / 1024.0) / elapsed_sec);
        st.current_rx_kbps = static_cast<float>((static_cast<double>(d_rx) / 1024.0) / elapsed_sec);

        st.tx_rate_history[st.rate_history_offset] = st.current_tx_kbps;
        st.rx_rate_history[st.rate_history_offset] = st.current_rx_kbps;
        st.rate_history_offset = (st.rate_history_offset + 1) % st.tx_rate_history.size();

        st.last_total_tx_bytes = total_tx_bytes;
        st.last_total_rx_bytes = total_rx_bytes;
        st.last_rate_sample_time = now;
    }
}

void updatePeerRttHistory(GuiState& st, const ClientStatusSnapshot& snap) {
    double now = ImGui::GetTime();
    std::unordered_set<uint32_t> live;
    for (const auto& p : snap.peers) {
        live.insert(p.vip);
        auto& h = st.peer_rtt_history[p.vip];
        if (now - h.last_sample_sec >= 0.5) {
            h.samples[h.offset] = (p.rtt_ms > 0) ? static_cast<float>(p.rtt_ms) : 0.0f;
            h.offset = (h.offset + 1) % h.samples.size();
            h.last_sample_sec = now;
        }
    }
    // Drop histories for peers that are gone.
    for (auto it = st.peer_rtt_history.begin(); it != st.peer_rtt_history.end();) {
        if (live.find(it->first) == live.end()) it = st.peer_rtt_history.erase(it);
        else ++it;
    }
}

void renderNetworkPeersView(GuiState& st, const ClientStatusSnapshot& snap) {    if (st.font_heading) ImGui::PushFont(st.font_heading);
    ImGui::TextUnformatted("Network & Virtual LAN Peers");
    if (st.font_heading) ImGui::PopFont();
    ImGui::TextDisabled("Connect to your coordination server and monitor P2P UDP hole-punched routes.");
    ImGui::Spacing();

    // 1. Connection Settings Card (AutoResizeY eliminates vertical scrollbars & clipping)
    ImGui::BeginChild("##ConnSettingsCard", ImVec2(0, 0),
                      ImGuiChildFlags_Borders | ImGuiChildFlags_AutoResizeY,
                      ImGuiWindowFlags_NoScrollbar);
    {
        if (st.font_bold) ImGui::PushFont(st.font_bold);
        ImGui::TextUnformatted("VPN Coordination Server");
        if (st.font_bold) ImGui::PopFont();
        if (snap.connected) {
            ImGui::SameLine(0, st.S(10.0f));
            bool srv_v6 = snap.server_endpoint.isIpv6();
            drawPillBadge(st, srv_v6 ? "Connected via IPv6" : "Connected via IPv4",
                          (srv_v6 ? pillColors(PillTone::Blue) : pillColors(PillTone::Green)).bg,
                          (srv_v6 ? pillColors(PillTone::Blue) : pillColors(PillTone::Green)).fg);
            if (ImGui::IsItemHovered()) {
                ImGui::SetTooltip("Active server endpoint: %s (%s)",
                                  snap.server_endpoint.toString().c_str(),
                                  snap.server_endpoint.versionString());
            }
        } else if (snap.reconnecting) {
            ImGui::SameLine(0, st.S(10.0f));
            std::string reconn_badge = "Auto-Reconnecting to " + platform::formatHostPort(snap.server_host, snap.server_port);
            drawPillBadge(st, reconn_badge.c_str(),
                          pillColors(PillTone::Amber).bg,
                          pillColors(PillTone::Amber).fg);
        } else {
            ImGui::SameLine(0, st.S(10.0f));
            ImGui::TextDisabled("(Supports Hostnames, IPv6, and IPv4 — Prefers IPv6)");
        }
        ImGui::Spacing();

        const bool conn_fields_locked = snap.connected || snap.reconnecting || st.connect_in_progress;
        ImGui::BeginDisabled(conn_fields_locked);
        ImGui::AlignTextToFramePadding();
        ImGui::TextUnformatted("Server Host / IP:");
        ImGui::SameLine(0, st.S(6.0f));
        ImGui::SetNextItemWidth(st.S(175.0f));
        bool enter_connect = ImGui::InputTextWithHint("##ServerIP", "Hostname, IPv6, or IPv4...",
                                                      st.server_ip_buf, sizeof(st.server_ip_buf),
                                                      ImGuiInputTextFlags_EnterReturnsTrue);
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip("Enter a hostname (e.g. localhost, vpn.example.com), IPv6 (e.g. ::1), or IPv4 (e.g. 127.0.0.1)");
        }

        ImGui::SameLine(0, st.S(3.0f));
        if (ImGui::Button("v##RecentSrvBtn", ImVec2(st.S(26.0f), 0))) {
            ImGui::OpenPopup("RecentServersPopup");
        }
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip("Select from Recent VPN Servers");
        }
        if (ImGui::BeginPopup("RecentServersPopup")) {
            ImGui::TextDisabled("Recent VPN Servers");
            ImGui::Separator();
            for (const auto& srv : st.recent_servers) {
                if (ImGui::Selectable(srv.c_str())) {
                    std::string host_part;
                    uint16_t port_val = kDefaultServerPort;
                    if (platform::splitHostPort(srv, host_part, port_val)) {
                        std::strncpy(st.server_ip_buf, host_part.c_str(), sizeof(st.server_ip_buf) - 1);
                        st.server_ip_buf[sizeof(st.server_ip_buf) - 1] = '\0';
                        std::snprintf(st.server_port_buf, sizeof(st.server_port_buf), "%u", port_val);
                    } else {
                        std::strncpy(st.server_ip_buf, srv.c_str(), sizeof(st.server_ip_buf) - 1);
                        st.server_ip_buf[sizeof(st.server_ip_buf) - 1] = '\0';
                    }
                    saveSettings(st);
                }
            }
            ImGui::EndPopup();
        }

        ImGui::SameLine(0, st.S(12.0f));
        ImGui::TextUnformatted("Port:");
        ImGui::SameLine(0, st.S(6.0f));
        ImGui::SetNextItemWidth(st.S(72.0f));
        enter_connect |= ImGui::InputText("##ServerPort", st.server_port_buf, sizeof(st.server_port_buf),
                                          ImGuiInputTextFlags_CharsDecimal | ImGuiInputTextFlags_EnterReturnsTrue);

        ImGui::SameLine(0, st.S(12.0f));
        ImGui::TextUnformatted("Nickname:");
        ImGui::SameLine(0, st.S(6.0f));
        ImGui::SetNextItemWidth(st.S(130.0f));
        enter_connect |= ImGui::InputText("##Nickname", st.nickname_buf, sizeof(st.nickname_buf), ImGuiInputTextFlags_EnterReturnsTrue);
        ImGui::EndDisabled();
        if (conn_fields_locked && ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
            ImGui::SetTooltip((snap.reconnecting || (st.connect_in_progress && !snap.connected))
                                  ? "Connecting... fields unlock once the attempt finishes or is stopped."
                                  : "Disconnect first to edit the server, port, or nickname.");
        }

        ImGui::SameLine(0, st.S(14.0f));
        if (st.connect_in_progress && !snap.connected && !snap.reconnecting) {
            // connectToServer() runs on a worker thread; it cannot be interrupted, but a
            // cancel request (tray menu) is applied as soon as it returns.
            ImGui::BeginDisabled();
            ImGui::Button("Connecting...", ImVec2(st.S(115.0f), 0));
            ImGui::EndDisabled();
        } else if (snap.connected || snap.reconnecting) {
            ImGui::PushStyleColor(ImGuiCol_Button, g_pal.btn_red);
            ImGui::PushStyleColor(ImGuiCol_ButtonHovered, g_pal.btn_red_hover);
            ImGui::PushStyleColor(ImGuiCol_ButtonActive, g_pal.btn_red_active);
            if (ImGui::Button(snap.reconnecting ? "Stop Retry" : "Disconnect", ImVec2(st.S(115.0f), 0))) {
                triggerConnectOrDisconnect(st);
            }
            ImGui::PopStyleColor(3);
        } else {
            ImGui::PushStyleColor(ImGuiCol_Button, g_pal.btn_blue);
            ImGui::PushStyleColor(ImGuiCol_ButtonHovered, g_pal.btn_blue_hover);
            ImGui::PushStyleColor(ImGuiCol_ButtonActive, g_pal.btn_blue_active);
            if (ImGui::Button("Connect", ImVec2(st.S(115.0f), 0)) || enter_connect) {
                triggerConnectOrDisconnect(st);
            }
            ImGui::PopStyleColor(3);
        }

        ImGui::SameLine(0, st.S(14.0f));
        if (ImGui::Checkbox("Force Relay", &st.force_relay)) {
            st.client->setForceRelay(st.force_relay);
            saveSettings(st);
        }
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip("Route all traffic via Server Relay instead of UDP hole-punching");
        }
    }
    ImGui::EndChild();

    // 2. Live Throughput Telemetry Row (AutoResizeY so both text lines and plots fit cleanly)
    ImGui::BeginChild("##TelemetryRow", ImVec2(0, 0),
                      ImGuiChildFlags_Borders | ImGuiChildFlags_AutoResizeY,
                      ImGuiWindowFlags_NoScrollbar);
    {
        float half_w = ImGui::GetContentRegionAvail().x * 0.5f;
        float plot_h = ImGui::GetTextLineHeightWithSpacing() * 2.0f;
        float plot_w = st.S(130.0f);

        // Upload Graph + Stats
        float max_tx = 10.0f;
        for (float v : st.tx_rate_history) if (v > max_tx) max_tx = v;
        ImGui::PushStyleColor(ImGuiCol_PlotLines, g_pal.plot_tx);
        ImGui::PlotLines("##TxGraphBig", st.tx_rate_history.data(), static_cast<int>(st.tx_rate_history.size()),
                         static_cast<int>(st.rate_history_offset), nullptr, 0.0f, max_tx, ImVec2(plot_w, plot_h));
        ImGui::PopStyleColor();

        ImGui::SameLine(0, st.S(12.0f));
        ImGui::BeginGroup();
        ImGui::TextColored(g_pal.rate_tx, "Upload: %s", formatRateKbps(st.current_tx_kbps).c_str());
        ImGui::TextDisabled("Total TX: %s (%llu pkts)",
                            formatBytes(st.last_total_tx_bytes).c_str(),
                            static_cast<unsigned long long>(snap.total_tx_packets));
        ImGui::EndGroup();

        ImGui::SameLine(half_w);

        // Download Graph + Stats
        float max_rx = 10.0f;
        for (float v : st.rx_rate_history) if (v > max_rx) max_rx = v;
        ImGui::PushStyleColor(ImGuiCol_PlotLines, g_pal.plot_rx);
        ImGui::PlotLines("##RxGraphBig", st.rx_rate_history.data(), static_cast<int>(st.rx_rate_history.size()),
                         static_cast<int>(st.rate_history_offset), nullptr, 0.0f, max_rx, ImVec2(plot_w, plot_h));
        ImGui::PopStyleColor();

        ImGui::SameLine(0, st.S(12.0f));
        ImGui::BeginGroup();
        ImGui::TextColored(g_pal.rate_rx, "Download: %s", formatRateKbps(st.current_rx_kbps).c_str());
        ImGui::TextDisabled("Total RX: %s (%llu pkts)",
                            formatBytes(st.last_total_rx_bytes).c_str(),
                            static_cast<unsigned long long>(snap.total_rx_packets));
        ImGui::EndGroup();
    }
    ImGui::EndChild();

    // 3. Full-Size Virtual LAN Peers Table
    ImGui::BeginChild("##PeersTableCard", ImVec2(0, 0), ImGuiChildFlags_Borders);
    {
        if (st.font_bold) ImGui::PushFont(st.font_bold);
        ImGui::TextUnformatted("Connected Peers in Virtual Subnet (10.42.0.0/24)");
        if (st.font_bold) ImGui::PopFont();
        ImGui::SameLine(0, st.S(10.0f));
        ImGui::TextDisabled("|  Right-click any peer to copy Virtual IP or @mention in Chat");
        ImGui::Spacing();

        // Peer filter (Feature 11)
        ImGui::SetNextItemWidth(st.S(260.0f));
        ImGui::InputTextWithHint("##PeerFilter", "Filter peers by name or IP...",
                                 st.peer_filter_buf, sizeof(st.peer_filter_buf));
        if (st.peer_filter_buf[0] != '\0') {
            ImGui::SameLine(0, st.S(8.0f));
            if (ImGui::SmallButton("Clear##PeerFilter")) {
                st.peer_filter_buf[0] = '\0';
            }
        }
        ImGui::Spacing();

        // Filtered index list (sorting is applied below)
        std::vector<size_t> rows;
        rows.reserve(snap.peers.size());
        for (size_t i = 0; i < snap.peers.size(); ++i) {
            const auto& p = snap.peers[i];
            std::string vip = ipv4ToString(p.vip);
            if (containsNoCase(p.nickname, st.peer_filter_buf) || containsNoCase(vip, st.peer_filter_buf)) {
                rows.push_back(i);
            }
        }

        ImGuiTableFlags table_flags =
            ImGuiTableFlags_BordersInnerH |
            ImGuiTableFlags_RowBg |
            ImGuiTableFlags_Resizable |
            ImGuiTableFlags_ScrollY |
            ImGuiTableFlags_SizingStretchProp |
            ImGuiTableFlags_Sortable |
            ImGuiTableFlags_SortTristate;

        float table_h = rows.empty() ? (ImGui::GetFrameHeightWithSpacing() * 1.4f) : 0.0f;
        if (ImGui::BeginTable("PeersTableFull", 7, table_flags, ImVec2(0, table_h))) {
            ImGui::TableSetupScrollFreeze(0, 1);
            ImGui::TableSetupColumn("Nickname", ImGuiTableColumnFlags_WidthStretch, 1.1f);
            ImGui::TableSetupColumn("Virtual IP", ImGuiTableColumnFlags_WidthStretch, 0.9f);
            ImGui::TableSetupColumn("Connection Route", ImGuiTableColumnFlags_WidthStretch, 1.2f);
            ImGui::TableSetupColumn("IP Version", ImGuiTableColumnFlags_WidthStretch, 0.7f);
            ImGui::TableSetupColumn("Ping (RTT)", ImGuiTableColumnFlags_WidthStretch | ImGuiTableColumnFlags_DefaultSort, 0.7f);
            ImGui::TableSetupColumn("TX (Upload)", ImGuiTableColumnFlags_WidthStretch, 0.8f);
            ImGui::TableSetupColumn("RX (Download)", ImGuiTableColumnFlags_WidthStretch, 0.8f);
            ImGui::TableHeadersRow();

            if (ImGuiTableSortSpecs* specs = ImGui::TableGetSortSpecs()) {
                if (specs->SpecsCount > 0) {
                    const ImGuiTableColumnSortSpecs& spec = specs->Specs[0];
                    bool asc = (spec.SortDirection == ImGuiSortDirection_Ascending);
                    std::stable_sort(rows.begin(), rows.end(), [&](size_t a, size_t b) {
                        const auto& pa = snap.peers[a];
                        const auto& pb = snap.peers[b];
                        int c = 0;
                        switch (spec.ColumnIndex) {
                            case 0: c = _stricmp(pa.nickname.c_str(), pb.nickname.c_str()); break;
                            case 1: c = (pa.vip < pb.vip) ? -1 : (pa.vip > pb.vip ? 1 : 0); break;
                            case 2: c = static_cast<int>(pa.route_mode) - static_cast<int>(pb.route_mode); break;
                            case 3: c = static_cast<int>(pa.ip_version) - static_cast<int>(pb.ip_version); break;
                            case 4: c = (pa.rtt_ms < pb.rtt_ms) ? -1 : (pa.rtt_ms > pb.rtt_ms ? 1 : 0); break;
                            case 5: c = (pa.tx_bytes < pb.tx_bytes) ? -1 : (pa.tx_bytes > pb.tx_bytes ? 1 : 0); break;
                            case 6: c = (pa.rx_bytes < pb.rx_bytes) ? -1 : (pa.rx_bytes > pb.rx_bytes ? 1 : 0); break;
                            default: break;
                        }
                        if (c == 0) return a < b;
                        return asc ? (c < 0) : (c > 0);
                    });
                }
            }

            for (size_t r = 0; r < rows.size(); ++r) {
                const size_t i = rows[r];
                const auto& p = snap.peers[i];
                std::string peer_vip_str = ipv4ToString(p.vip);

                ImGui::TableNextRow();
                // Nicknames are remote-controlled: the ID comes from the VIP and the
                // "###" suffix, never from the nickname text.
                ImGui::PushID(static_cast<int>(p.vip));

                ImGui::TableSetColumnIndex(0);
                ImVec2 dot_pos = ImGui::GetCursorScreenPos();
                ImGui::Dummy(ImVec2(st.S(16.0f), ImGui::GetTextLineHeight()));
                ImGui::SameLine(0, st.S(4.0f));
                const std::string peer_label = imguiLabelText(p.nickname) + "###PeerRow";
                ImGui::Selectable(peer_label.c_str(), false, ImGuiSelectableFlags_SpanAllColumns | ImGuiSelectableFlags_AllowOverlap);
                bool row_hovered = ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenBlockedByActiveItem);
                renderPeerRxActivityDotAt(st, dot_pos, p);
                if (row_hovered) {
                    renderPeerHoverTooltip(st, snap, p, peer_vip_str);
                }
                if (ImGui::BeginPopupContextItem("PeerCtxMenu")) {
                    ImGui::TextDisabled("Peer: %s (%s)", p.nickname.c_str(), peer_vip_str.c_str());
                    ImGui::Separator();
                    if (ImGui::MenuItem("Copy Virtual IP")) {
                        copyTextToClipboard(st.hWnd, peer_vip_str);
                        showBanner(st, "Copied peer Virtual IP: " + peer_vip_str, false, 2.5);
                    }
                    if (p.active_endpoint.port_host != 0 && ImGui::MenuItem("Copy Endpoint Address")) {
                        copyTextToClipboard(st.hWnd, p.active_endpoint.toString());
                    }
                    if (ImGui::BeginMenu("Candidate Endpoints...")) {
                        if (p.candidate_endpoints.empty()) {
                            ImGui::TextDisabled("None advertised");
                        } else {
                            ImGui::TextDisabled("%zu advertised endpoint(s)", p.candidate_endpoints.size());
                            ImGui::Separator();
                            // Unlike a tooltip, a menu popup is interactive, so this child can scroll.
                            ImGui::BeginChild("##PeerCandidateMenuList", ImVec2(st.S(320.0f), st.S(180.0f)));
                            if (st.font_mono) ImGui::PushFont(st.font_mono);
                            for (const auto& ep : p.candidate_endpoints) {
                                const bool is_active = (p.route_mode == RouteMode::DirectP2P && ep == p.active_endpoint);
                                const bool is_fallback = !is_active &&
                                    ((p.best_v6_endpoint.port_host != 0 && ep == p.best_v6_endpoint) ||
                                     (p.best_v4_endpoint.port_host != 0 && ep == p.best_v4_endpoint));
                                const char* suffix = is_active ? " [Active]" : (is_fallback ? " [Fallback]" : "");
                                const ImVec4 col = is_active ? g_pal.success : (is_fallback ? g_pal.accent : g_pal.muted);
                                ImGui::TextColored(col, "[%s] %s%s", ep.versionString(), ep.toString().c_str(), suffix);
                            }
                            if (st.font_mono) ImGui::PopFont();
                            ImGui::EndChild();
                            if (ImGui::MenuItem("Copy All Endpoints")) {
                                std::string all;
                                for (const auto& ep : p.candidate_endpoints) {
                                    if (!all.empty()) all += "\n";
                                    all += ep.toString();
                                }
                                copyTextToClipboard(st.hWnd, all);
                                showBanner(st, "Copied " + std::to_string(p.candidate_endpoints.size()) + " peer endpoint(s)", false, 2.5);
                            }
                        }
                        ImGui::EndMenu();
                    }
                    if (ImGui::MenuItem("Message / @Mention in Lobby Chat")) {
                        std::string mention = "@" + p.nickname + " ";
                        std::strncat(st.chat_input_buf, mention.c_str(), sizeof(st.chat_input_buf) - std::strlen(st.chat_input_buf) - 1);
                        st.active_tab = NavTab::LobbyChat;
                        st.focus_chat_input = true;
                    }
                    ImGui::EndPopup();
                }

                ImGui::TableSetColumnIndex(1);
                if (st.font_mono) ImGui::PushFont(st.font_mono);
                ImGui::TextColored(g_pal.accent, "%s", peer_vip_str.c_str());
                if (st.font_mono) ImGui::PopFont();

                ImGui::TableSetColumnIndex(2);
                if (p.route_mode == RouteMode::DirectP2P) {
                    drawPillBadge(st, "Direct UDP (P2P)", pillColors(PillTone::Green).bg, pillColors(PillTone::Green).fg);
                } else {
                    const char* relay_lbl = p.hole_punch_gave_up ? "Server Relay (Punch Stopped)" : "Server Relay";
                    drawPillBadge(st, relay_lbl, pillColors(PillTone::Orange).bg, pillColors(PillTone::Orange).fg);
                }

                ImGui::TableSetColumnIndex(3);
                if (p.ip_version == platform::IpVersion::V6) {
                    drawPillBadge(st, "IPv6", pillColors(PillTone::Blue).bg, pillColors(PillTone::Blue).fg);
                } else if (p.ip_version == platform::IpVersion::V4) {
                    drawPillBadge(st, "IPv4", pillColors(PillTone::Slate).bg, pillColors(PillTone::Slate).fg);
                } else {
                    ImGui::TextDisabled("-");
                }

                ImGui::TableSetColumnIndex(4);
                if (p.rtt_ms > 0 || p.last_recv_us != 0) {
                    ImVec4 ping_col = (p.rtt_ms < 50)  ? g_pal.success :
                                      (p.rtt_ms < 120) ? g_pal.warning :
                                                         g_pal.danger;
                    // Symbol + text so meaning isn't conveyed by color alone (Feature 18-adjacent)
                    const char* ping_tag = (p.rtt_ms < 50) ? "Good" : (p.rtt_ms < 120) ? "Fair" : "Poor";
                    ImGui::TextColored(ping_col, "%u ms (%s)", p.rtt_ms, ping_tag);
                } else {
                    ImGui::TextDisabled("-");
                }

                ImGui::TableSetColumnIndex(5);
                ImGui::Text("TX: %s (%llu)",
                            formatBytes(p.tx_bytes).c_str(),
                            static_cast<unsigned long long>(p.tx_packets));

                ImGui::TableSetColumnIndex(6);
                ImGui::Text("RX: %s (%llu)",
                            formatBytes(p.rx_bytes).c_str(),
                            static_cast<unsigned long long>(p.rx_packets));

                ImGui::PopID();
            }
            ImGui::EndTable();
        }

        // Full-width readable empty state message outside of individual narrow columns
        if (snap.peers.empty() || rows.empty()) {
            ImGui::Spacing();
            if (!snap.peers.empty() && rows.empty()) {
                ImGui::TextDisabled("No peers match the current filter.");
            } else if (snap.connected) {
                const char* empty_msg = "No other peers connected in the lobby yet. Share your server IP and port with friends!";
                float avail_w = ImGui::GetContentRegionAvail().x;
                float txt_w = ImGui::CalcTextSize(empty_msg).x;
                if (avail_w > txt_w) {
                    ImGui::SetCursorPosX(ImGui::GetCursorPosX() + (avail_w - txt_w) * 0.5f);
                }
                ImGui::TextDisabled("%s", empty_msg);
            } else {
                const char* empty_msg = "Currently disconnected. Join the Virtual LAN to see peers here.";
                float avail_w = ImGui::GetContentRegionAvail().x;
                float txt_w = ImGui::CalcTextSize(empty_msg).x;
                float btn_w = st.S(130.0f);
                float group_w = std::max(txt_w, btn_w);
                if (avail_w > group_w) {
                    ImGui::SetCursorPosX(ImGui::GetCursorPosX() + (avail_w - group_w) * 0.5f);
                }
                ImGui::BeginGroup();
                ImGui::TextDisabled("%s", empty_msg);
                ImGui::PushStyleColor(ImGuiCol_Button, g_pal.btn_blue);
                ImGui::PushStyleColor(ImGuiCol_ButtonHovered, g_pal.btn_blue_hover);
                ImGui::PushStyleColor(ImGuiCol_ButtonActive, g_pal.btn_blue_active);
                ImGui::BeginDisabled(st.connect_in_progress || snap.reconnecting);
                if (ImGui::Button(st.connect_in_progress ? "Connecting...##EmptyStateCta" : "Connect Now##EmptyStateCta",
                                  ImVec2(btn_w, 0))) {
                    triggerConnectOrDisconnect(st);
                }
                ImGui::EndDisabled();
                ImGui::PopStyleColor(3);
                ImGui::EndGroup();
            }
        }
    }
    ImGui::EndChild();
}

void renderLobbyChatView(GuiState& st, const ClientStatusSnapshot& snap) {
    st.seen_chat_count = snap.chat_messages.size();
    st.unread_chat_count = 0;

    if (st.font_heading) ImGui::PushFont(st.font_heading);
    ImGui::TextUnformatted("Lobby Chat");
    if (st.font_heading) ImGui::PopFont();
    ImGui::TextDisabled("Coordinate matches, share host Virtual IPs, and chat with everyone in the lobby.");
    ImGui::Spacing();

    float avail_w = ImGui::GetContentRegionAvail().x;
    float members_w = st.S(215.0f);
    float chat_w = std::max(st.S(260.0f), avail_w - members_w - ImGui::GetStyle().ItemSpacing.x);

    // Left: Full-height Chat History & Input
    ImGui::BeginChild("##ChatRoomMain", ImVec2(chat_w, 0), ImGuiChildFlags_Borders);
    {
        float input_row_h = ImGui::GetFrameHeightWithSpacing() + st.S(8.0f);
        ImGui::BeginChild("##ChatScrollArea", ImVec2(0, -input_row_h), ImGuiChildFlags_None);
        {
            if (snap.chat_messages.empty()) {
                ImGui::Spacing();
                ImGui::TextDisabled(snap.connected
                    ? "No messages in the lobby yet. Type a message below and press Enter!"
                    : "You are currently disconnected. Connect on the Network tab to chat.");
            } else {
                for (size_t mi = 0; mi < snap.chat_messages.size(); ++mi) {
                    const auto& m = snap.chat_messages[mi];
                    bool is_self = (snap.local_vip != 0 && m.sender_vip == snap.local_vip);
                    bool mentioned = !is_self && snap.connected && !snap.nickname.empty() &&
                                     containsNoCase(m.message, "@" + snap.nickname);

                    if (mentioned) {
                        ImVec2 p0 = ImGui::GetCursorScreenPos();
                        float w = ImGui::GetContentRegionAvail().x;
                        ImGui::GetWindowDrawList()->AddRectFilled(
                            ImVec2(p0.x - st.S(4.0f), p0.y - st.S(2.0f)),
                            ImVec2(p0.x + w, p0.y + ImGui::GetTextLineHeight() + st.S(2.0f)),
                            g_pal.mention_bg, st.S(4.0f));
                    }

                    // Timestamp (Feature 10)
                    if (mi < st.chat_times.size() && st.chat_times[mi] > 0) {
                        std::time_t tt = static_cast<std::time_t>(st.chat_times[mi]);
                        std::tm local_tm{};
                        if (localtime_s(&local_tm, &tt) == 0) {
                            char tb[16];
                            if (std::strftime(tb, sizeof(tb), "%H:%M:%S", &local_tm) > 0) {
                                if (st.font_mono) ImGui::PushFont(st.font_mono);
                                ImGui::TextDisabled("%s", tb);
                                if (st.font_mono) ImGui::PopFont();
                                ImGui::SameLine(0, st.S(8.0f));
                            }
                        }
                    }

                    if (st.font_mono) ImGui::PushFont(st.font_mono);
                    ImGui::TextColored(g_pal.chat_vip, "[%s]", ipv4ToString(m.sender_vip).c_str());
                    if (st.font_mono) ImGui::PopFont();

                    ImGui::SameLine(0, st.S(8.0f));
                    if (st.font_bold) ImGui::PushFont(st.font_bold);
                    ImGui::TextColored(is_self ? g_pal.chat_self : g_pal.chat_other,
                                       "%s:", m.sender_name.c_str());
                    if (st.font_bold) ImGui::PopFont();

                    if (mentioned) {
                        ImGui::SameLine(0, st.S(6.0f));
                        drawPillBadge(st, "mentioned you", pillColors(PillTone::Amber).bg, pillColors(PillTone::Amber).fg);
                    }

                    ImGui::SameLine(0, st.S(8.0f));
                    ImGui::PushTextWrapPos(0.0f);
                    if (mentioned) {
                        ImGui::TextColored(g_pal.mention_text, "%s", m.message.c_str());
                    } else {
                        ImGui::TextUnformatted(m.message.c_str());
                    }
                    ImGui::PopTextWrapPos();
                }
            }

            // Remember whether the user is parked at the bottom (Feature 3)
            st.chat_at_bottom = (ImGui::GetScrollMaxY() <= 0.0f) ||
                                (ImGui::GetScrollY() >= ImGui::GetScrollMaxY() - st.S(6.0f));
            if (st.chat_at_bottom) {
                st.new_chat_available = false;
            }

            if (st.scroll_chat_to_bottom) {
                ImGui::SetScrollHereY(1.0f);
                st.scroll_chat_to_bottom = false;
                st.new_chat_available = false;
            }
        }
        ImGui::EndChild();

        // "New messages" jump button shown only when the user has scrolled up (Feature 3)
        if (st.new_chat_available) {
            const char* nm_lbl = "New messages below";
            float nm_w = ImGui::CalcTextSize(nm_lbl).x + ImGui::GetStyle().FramePadding.x * 2.0f;
            float nm_avail = ImGui::GetContentRegionAvail().x;
            if (nm_avail > nm_w) {
                ImGui::SetCursorPosX(ImGui::GetCursorPosX() + (nm_avail - nm_w));
            }
            ImGui::PushStyleColor(ImGuiCol_Button, g_pal.btn_blue);
            ImGui::PushStyleColor(ImGuiCol_ButtonHovered, g_pal.btn_blue_hover);
            if (ImGui::SmallButton(nm_lbl)) {
                st.scroll_chat_to_bottom = true;
                st.new_chat_available = false;
            }
            ImGui::PopStyleColor(2);
        }

        ImGui::Separator();

        float send_btn_w = st.S(88.0f);
        ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x - send_btn_w - st.S(8.0f));
        if (st.focus_chat_input) {
            ImGui::SetKeyboardFocusHere();
            st.focus_chat_input = false;
        }
        bool send_pressed = ImGui::InputTextWithHint("##ChatInputFull",
                                                     snap.connected ? "Message lobby (Press Enter to send)..." : "Connect to server to chat...",
                                                     st.chat_input_buf, sizeof(st.chat_input_buf),
                                                     ImGuiInputTextFlags_EnterReturnsTrue);

        // @mention autocomplete (Feature 10)
        {
            bool input_active = ImGui::IsItemActive();
            st.mention_query.clear();
            const char* buf = st.chat_input_buf;
            size_t blen = std::strlen(buf);
            for (size_t k = blen; k-- > 0; ) {
                char ch = buf[k];
                if (ch == '@') { st.mention_query.assign(buf + k + 1); break; }
                if (ch == ' ' || ch == '\n' || ch == '\t') break;
            }

            if (input_active && !st.mention_query.empty()) {
                ImGui::OpenPopup("ChatMentionPopup");
            }
            if (ImGui::BeginPopup("ChatMentionPopup")) {
                bool any = false;
                for (const auto& p : snap.peers) {
                    if (!st.mention_query.empty() && !containsNoCase(p.nickname, st.mention_query)) continue;
                    any = true;
                    ImGui::PushID(static_cast<int>(p.vip));
                    const std::string mention_label = imguiLabelText(p.nickname) + "###MentionPick";
                    const bool picked = ImGui::Selectable(mention_label.c_str());
                    ImGui::PopID();
                    if (picked) {
                        std::string nb = st.chat_input_buf;
                        size_t at = nb.rfind('@');
                        if (at != std::string::npos) {
                            nb = nb.substr(0, at) + "@" + p.nickname + " ";
                            std::strncpy(st.chat_input_buf, nb.c_str(), sizeof(st.chat_input_buf) - 1);
                            st.chat_input_buf[sizeof(st.chat_input_buf) - 1] = '\0';
                        }
                        st.focus_chat_input = true;
                        st.mention_query.clear();
                    }
                }
                if (!any) ImGui::TextDisabled("No matching members");
                ImGui::EndPopup();
            }
        }
        ImGui::SameLine(0, st.S(8.0f));
        ImGui::PushStyleColor(ImGuiCol_Button, g_pal.btn_blue);
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, g_pal.btn_blue_hover);
        if (ImGui::Button("Send", ImVec2(send_btn_w, 0))) {
            send_pressed = true;
        }
        ImGui::PopStyleColor(2);

        if (send_pressed) {
            std::string msg = trimStr(st.chat_input_buf);
            if (!msg.empty()) {
                if (st.client->sendChatMessage(msg)) {
                    st.chat_input_buf[0] = '\0';
                    st.scroll_chat_to_bottom = true;
                } else {
                    showBanner(st, "Cannot send chat: Not connected to a VPN server.", true, 3.0);
                }
            }
            ImGui::SetKeyboardFocusHere(-1);
        }
    }
    ImGui::EndChild();

    ImGui::SameLine();

    // Right: Compact Online Members List
    ImGui::BeginChild("##ChatMembersSidebar", ImVec2(0, 0), ImGuiChildFlags_Borders);
    {
        if (st.font_bold) ImGui::PushFont(st.font_bold);
        ImGui::Text("Members (%zu)", snap.peers.size() + (snap.connected ? 1 : 0));
        if (st.font_bold) ImGui::PopFont();
        ImGui::Separator();

        if (snap.connected) {
            ImGui::TextColored(g_pal.chat_self, "%s (You)", snap.nickname.c_str());
            if (st.font_mono) ImGui::PushFont(st.font_mono);
            ImGui::TextDisabled("  %s", ipv4ToString(snap.local_vip).c_str());
            if (st.font_mono) ImGui::PopFont();
            ImGui::Spacing();
        } else {
            ImGui::TextDisabled("Offline");
        }

        for (size_t i = 0; i < snap.peers.size(); ++i) {
            const auto& p = snap.peers[i];
            std::string peer_vip = ipv4ToString(p.vip);
            ImGui::PushID(static_cast<int>(p.vip));
            ImVec2 dot_pos = ImGui::GetCursorScreenPos();
            ImGui::Dummy(ImVec2(st.S(16.0f), ImGui::GetTextLineHeight()));
            ImGui::SameLine(0, st.S(4.0f));
            const std::string member_label = imguiLabelText(p.nickname) + "###ChatMember";
            if (ImGui::Selectable(member_label.c_str())) {
                std::string mention = "@" + p.nickname + " ";
                std::strncat(st.chat_input_buf, mention.c_str(), sizeof(st.chat_input_buf) - std::strlen(st.chat_input_buf) - 1);
                st.focus_chat_input = true;
            }
            renderPeerRxActivityDotAt(st, dot_pos, p);
            if (ImGui::IsItemHovered()) {
                ImGui::SetTooltip("Click to @mention %s\nRight-click to copy Virtual IP (%s)", p.nickname.c_str(), peer_vip.c_str());
            }
            if (ImGui::BeginPopupContextItem("ChatMemberCtx")) {
                if (ImGui::MenuItem("Copy Virtual IP")) {
                    copyTextToClipboard(st.hWnd, peer_vip);
                    showBanner(st, "Copied " + peer_vip + " to clipboard!", false, 2.5);
                }
                ImGui::EndPopup();
            }
            if (st.font_mono) ImGui::PushFont(st.font_mono);
            ImGui::TextDisabled("  %s", peer_vip.c_str());
            if (st.font_mono) ImGui::PopFont();
            ImGui::PopID();
        }
    }
    ImGui::EndChild();
}

void renderGameLauncherView(GuiState& st, const ClientStatusSnapshot& snap) {
    if (st.font_heading) ImGui::PushFont(st.font_heading);
    ImGui::TextUnformatted("Game Library & Process Launcher");
    if (st.font_heading) ImGui::PopFont();
    ImGui::SameLine(0, st.S(10.0f));
    drawPillBadge(st, "Drag & Drop .exe Anywhere", pillColors(PillTone::Blue).bg, pillColors(PillTone::Blue).fg);
    ImGui::TextDisabled("Launch 32-bit (x86) or 64-bit (x64) applications with automatic Winsock Virtual LAN hooking.");
    ImGui::Spacing();

    // 1. Launch / Add Game Card (AutoResizeY so it never clips or scrolls)
    ImGui::BeginChild("##LaunchSetupCard", ImVec2(0, 0),
                      ImGuiChildFlags_Borders | ImGuiChildFlags_AutoResizeY,
                      ImGuiWindowFlags_NoScrollbar);
    {
        if (st.font_bold) ImGui::PushFont(st.font_bold);
        ImGui::TextUnformatted("Launch or Save Application");
        if (st.font_bold) ImGui::PopFont();
        ImGui::Spacing();

        // Row 1: Executable Path + Browse + Quick Pick
        ImGui::AlignTextToFramePadding();
        ImGui::TextUnformatted("Executable:");
        ImGui::SameLine(0, st.S(8.0f));

        float browse_w = st.S(86.0f);
        float qpick_w = st.S(104.0f);
        float btns_w = browse_w + qpick_w + st.S(16.0f);
        ImGui::SetNextItemWidth(std::max(st.S(140.0f), ImGui::GetContentRegionAvail().x - btns_w));
        if (st.focus_app_path) {
            ImGui::SetKeyboardFocusHere();
            st.focus_app_path = false;
        }
        bool enter_launch = ImGui::InputTextWithHint("##AppPath", "Drop an .exe on the window, Browse, or use Quick Pick...",
                                                     st.app_path_buf, sizeof(st.app_path_buf),
                                                     ImGuiInputTextFlags_EnterReturnsTrue);

        ImGui::SameLine(0, st.S(6.0f));
        if (ImGui::Button("Browse...", ImVec2(browse_w, 0))) {
            wchar_t file_buf[1024] = {};
            OPENFILENAMEW ofn{};
            ofn.lStructSize = sizeof(ofn);
            ofn.hwndOwner = st.hWnd;
            ofn.lpstrFilter = L"Executables (*.exe)\0*.exe\0All Files (*.*)\0*.*\0";
            ofn.lpstrFile = file_buf;
            ofn.nMaxFile = static_cast<DWORD>(sizeof(file_buf) / sizeof(file_buf[0]));
            ofn.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR;
            if (GetOpenFileNameW(&ofn)) {
                const std::string picked = platform::wideToUtf8(file_buf);
                if (picked.size() < sizeof(st.app_path_buf)) {
                    std::strncpy(st.app_path_buf, picked.c_str(), sizeof(st.app_path_buf) - 1);
                    st.app_path_buf[sizeof(st.app_path_buf) - 1] = '\0';
                    saveSettings(st);
                } else {
                    showBanner(st, "Selected path is too long.", true, 3.0);
                }
            }
        }

        ImGui::SameLine(0, st.S(6.0f));
        if (ImGui::Button("Quick Pick v", ImVec2(qpick_w, 0))) {
            refreshRunningProcesses(st);
            ImGui::OpenPopup("QuickPickAppPopup");
        }
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip("Pick from running desktop processes or saved executables");
        }

        if (ImGui::BeginPopup("QuickPickAppPopup")) {
            ImGui::TextColored(g_pal.rate_tx, "Running Desktop Processes (Click to Select .exe)");
            ImGui::Separator();
            ImGui::SetNextItemWidth(st.S(300.0f));
            ImGui::InputTextWithHint("##QuickPickFilter", "Filter processes...",
                                     st.proc_filter_buf, sizeof(st.proc_filter_buf));
            ImGui::BeginChild("##RunningProcList", ImVec2(st.S(480.0f), st.S(210.0f)));
            for (const auto& rp : st.running_desktop_procs) {
                if (!containsNoCase(rp.name, st.proc_filter_buf) && !containsNoCase(rp.full_path, st.proc_filter_buf)) {
                    continue;
                }
                std::string label = imguiLabelText(rp.name + " (PID " + std::to_string(rp.pid) + ")") +
                                    "###RunningProc" + std::to_string(rp.pid);
                if (ImGui::Selectable(label.c_str())) {
                    std::strncpy(st.app_path_buf, rp.full_path.c_str(), sizeof(st.app_path_buf) - 1);
                    saveSettings(st);
                    ImGui::CloseCurrentPopup();
                }
                if (ImGui::IsItemHovered()) {
                    ImGui::SetTooltip("%s", rp.full_path.c_str());
                }
            }
            ImGui::EndChild();
            ImGui::EndPopup();
        }

        // Row 2: Arguments + Optional Profile Name + Save + Start in VPN
        ImGui::AlignTextToFramePadding();
        ImGui::TextUnformatted("Arguments:");
        ImGui::SameLine(0, st.S(10.0f));
        ImGui::SetNextItemWidth(st.S(175.0f));
        enter_launch |= ImGui::InputTextWithHint("##AppArgs", "Optional CLI args...",
                                                 st.app_args_buf, sizeof(st.app_args_buf),
                                                 ImGuiInputTextFlags_EnterReturnsTrue);

        ImGui::SameLine(0, st.S(12.0f));
        ImGui::TextUnformatted("Profile Name:");
        ImGui::SameLine(0, st.S(6.0f));
        ImGui::SetNextItemWidth(st.S(135.0f));
        ImGui::InputTextWithHint("##ProfName", "Auto from file", st.profile_name_buf, sizeof(st.profile_name_buf));

        ImGui::SameLine(0, st.S(8.0f));
        if (ImGui::Button("Save Profile", ImVec2(st.S(100.0f), 0))) {
            std::string exe = trimStr(st.app_path_buf);
            if (!exe.empty()) {
                addOrUpdateSavedGame(st, exe, trimStr(st.app_args_buf), st.profile_name_buf);
                st.profile_name_buf[0] = '\0';
                showBanner(st, "Saved to Game Library!", false, 2.5);
            } else {
                showBanner(st, "Select an executable path first.", true, 2.5);
            }
        }

        ImGui::SameLine(0, st.S(8.0f));
        // Launched games get the virtual IP the server assigned, so launching needs a
        // live connection (triggerLaunchApp() also reports this for Enter / other paths).
        const bool can_launch = snap.connected && snap.local_vip != 0;
        ImGui::BeginDisabled(!can_launch);
        ImGui::PushStyleColor(ImGuiCol_Button, g_pal.btn_green);
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, g_pal.btn_green_hover);
        ImGui::PushStyleColor(ImGuiCol_ButtonActive, g_pal.btn_green_active);
        const bool start_clicked = ImGui::Button("Start in VPN", ImVec2(std::max(st.S(115.0f), ImGui::GetContentRegionAvail().x), 0));
        ImGui::PopStyleColor(3);
        ImGui::EndDisabled();
        if (!can_launch && ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
            ImGui::SetTooltip("Connect to a VPN server first: games are started with the virtual IP it assigns.");
        }
        if (start_clicked || enter_launch) {
            triggerLaunchApp(st, st.app_path_buf, st.app_args_buf, st.profile_name_buf);
        }
    }
    ImGui::EndChild();

    // 2. Saved Game Library & Active Hooked Processes
    float avail_h = ImGui::GetContentRegionAvail().y;
    float library_h = std::clamp(avail_h * 0.42f, st.S(135.0f), st.S(230.0f));

    ImGui::BeginChild("##SavedGamesCard", ImVec2(0, library_h), ImGuiChildFlags_Borders);
    {
        if (st.font_bold) ImGui::PushFont(st.font_bold);
        ImGui::Text("Saved Games & Quick Launch Library (%zu)", st.saved_games.size());
        if (st.font_bold) ImGui::PopFont();
        ImGui::Spacing();

        ImGui::SetNextItemWidth(st.S(260.0f));
        ImGui::InputTextWithHint("##GameFilter", "Filter games by name or path...",
                                 st.game_filter_buf, sizeof(st.game_filter_buf));
        if (st.game_filter_buf[0] != '\0') {
            ImGui::SameLine(0, st.S(8.0f));
            if (ImGui::SmallButton("Clear##GameFilter")) {
                st.game_filter_buf[0] = '\0';
            }
        }
        ImGui::Spacing();

        std::optional<SavedGameProfile> pending_launch;
        std::vector<size_t> grows;
        grows.reserve(st.saved_games.size());
        for (size_t i = 0; i < st.saved_games.size(); ++i) {
            const auto& g = st.saved_games[i];
            if (containsNoCase(g.name, st.game_filter_buf) || containsNoCase(g.exe_path, st.game_filter_buf)) {
                grows.push_back(i);
            }
        }

        ImGuiTableFlags sflags =
            ImGuiTableFlags_BordersInnerH |
            ImGuiTableFlags_RowBg |
            ImGuiTableFlags_Resizable |
            ImGuiTableFlags_ScrollY |
            ImGuiTableFlags_SizingStretchProp |
            ImGuiTableFlags_Sortable |
            ImGuiTableFlags_SortTristate;

        if (ImGui::BeginTable("SavedGamesTableFull", 4, sflags)) {
            ImGui::TableSetupScrollFreeze(0, 1);
            ImGui::TableSetupColumn("Game / Profile", ImGuiTableColumnFlags_WidthStretch | ImGuiTableColumnFlags_DefaultSort, 1.2f);
            ImGui::TableSetupColumn("Executable Path", ImGuiTableColumnFlags_WidthStretch, 2.4f);
            ImGui::TableSetupColumn("Arguments", ImGuiTableColumnFlags_WidthStretch, 0.9f);
            ImGui::TableSetupColumn("Actions", ImGuiTableColumnFlags_WidthFixed | ImGuiTableColumnFlags_NoSort, st.S(185.0f));
            ImGui::TableHeadersRow();

            if (ImGuiTableSortSpecs* specs = ImGui::TableGetSortSpecs()) {
                if (specs->SpecsCount > 0) {
                    const ImGuiTableColumnSortSpecs& spec = specs->Specs[0];
                    bool asc = (spec.SortDirection == ImGuiSortDirection_Ascending);
                    std::stable_sort(grows.begin(), grows.end(), [&](size_t a, size_t b) {
                        const auto& ga = st.saved_games[a];
                        const auto& gb = st.saved_games[b];
                        int c = 0;
                        switch (spec.ColumnIndex) {
                            case 0: c = _stricmp(ga.name.c_str(), gb.name.c_str()); break;
                            case 1: c = _stricmp(ga.exe_path.c_str(), gb.exe_path.c_str()); break;
                            case 2: c = _stricmp(ga.args.c_str(), gb.args.c_str()); break;
                            default: break;
                        }
                        if (c == 0) return a < b;
                        return asc ? (c < 0) : (c > 0);
                    });
                }
            }

            for (size_t r = 0; r < grows.size(); ++r) {
                const size_t i = grows[r];
                const auto& g = st.saved_games[i];
                ImGui::TableNextRow();
                ImGui::PushID(static_cast<int>(i));

                ImGui::TableSetColumnIndex(0);
                if (st.font_bold) ImGui::PushFont(st.font_bold);
                ImGui::TextUnformatted(g.name.c_str());
                if (st.font_bold) ImGui::PopFont();

                ImGui::TableSetColumnIndex(1);
                ImGui::TextDisabled("%s", g.exe_path.c_str());
                if (ImGui::IsItemHovered()) {
                    ImGui::SetTooltip("%s", g.exe_path.c_str());
                }

                ImGui::TableSetColumnIndex(2);
                if (g.args.empty()) {
                    ImGui::TextDisabled("-");
                } else {
                    if (st.font_mono) ImGui::PushFont(st.font_mono);
                    ImGui::TextUnformatted(g.args.c_str());
                    if (st.font_mono) ImGui::PopFont();
                }

                ImGui::TableSetColumnIndex(3);
                ImGui::PushStyleColor(ImGuiCol_Button, g_pal.btn_green);
                ImGui::PushStyleColor(ImGuiCol_ButtonHovered, g_pal.btn_green_hover);
                if (ImGui::SmallButton("Launch")) {
                    pending_launch = g; // launched after the loop, see below
                }
                ImGui::PopStyleColor(2);

                ImGui::SameLine(0, st.S(6.0f));
                if (ImGui::SmallButton("Edit")) {
                    std::strncpy(st.app_path_buf, g.exe_path.c_str(), sizeof(st.app_path_buf) - 1);
                    std::strncpy(st.app_args_buf, g.args.c_str(), sizeof(st.app_args_buf) - 1);
                    std::strncpy(st.profile_name_buf, g.name.c_str(), sizeof(st.profile_name_buf) - 1);
                }

                ImGui::SameLine(0, st.S(6.0f));
                if (ImGui::SmallButton("Remove")) {
                    // Ask for confirmation before deleting (Feature 5)
                    st.confirm_remove_game_idx = static_cast<int>(i);
                    st.confirm_remove_game_name = g.name;
                    st.open_confirm_remove_game = true;
                }

                ImGui::PopID();
            }

            ImGui::EndTable();
        }

        // Launching can insert into st.saved_games (addOrUpdateSavedGame), which would
        // invalidate the `g` references held by the row loop above, so it runs afterwards
        // on a copy.
        if (pending_launch) {
            triggerLaunchApp(st, pending_launch->exe_path, pending_launch->args, pending_launch->name);
        }

        if (st.saved_games.empty()) {
            ImGui::Spacing();
            ImGui::TextDisabled("No saved games yet. Browse for an .exe above, or drag & drop one onto the window.");
        } else if (grows.empty()) {
            ImGui::Spacing();
            ImGui::TextDisabled("No saved games match the current filter.");
        }

        // Confirmation modal for removing a saved game (Feature 5)
        if (st.open_confirm_remove_game) {
            ImGui::OpenPopup("Remove Saved Game");
            st.open_confirm_remove_game = false;
        }
        if (ImGui::BeginPopupModal("Remove Saved Game", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
            ImGui::Text("Remove \"%s\" from the game library?", st.confirm_remove_game_name.c_str());
            ImGui::Spacing();
            ImGui::PushStyleColor(ImGuiCol_Button, g_pal.btn_red);
            ImGui::PushStyleColor(ImGuiCol_ButtonHovered, g_pal.btn_red_hover);
            if (ImGui::Button("Remove", ImVec2(st.S(110.0f), 0))) {
                int idx = st.confirm_remove_game_idx;
                if (idx >= 0 && static_cast<size_t>(idx) < st.saved_games.size()) {
                    st.saved_games.erase(st.saved_games.begin() + idx);
                    saveSettings(st);
                    showBanner(st, "Removed from game library.", false, 2.5);
                }
                st.confirm_remove_game_idx = -1;
                ImGui::CloseCurrentPopup();
            }
            ImGui::PopStyleColor(2);
            ImGui::SameLine();
            if (ImGui::Button("Cancel", ImVec2(st.S(110.0f), 0))) {
                st.confirm_remove_game_idx = -1;
                ImGui::CloseCurrentPopup();
            }
            ImGui::EndPopup();
        }
    }
    ImGui::EndChild();

    // 3. Active Hooked Processes Table
    ImGui::BeginChild("##HookedProcsCard", ImVec2(0, 0), ImGuiChildFlags_Borders);
    {
        size_t running_count = 0;
        size_t exited_count = 0;
        for (const auto& pr : snap.launched_processes) {
            auto status_it = st.proc_status_cache.find(pr.pid);
            if (status_it != st.proc_status_cache.end() && status_it->second.alive) {
                ++running_count;
            } else {
                ++exited_count;
            }
        }

        if (st.font_bold) ImGui::PushFont(st.font_bold);
        ImGui::Text("Launched VPN Processes (%zu active / %zu total)", running_count, snap.launched_processes.size());
        if (st.font_bold) ImGui::PopFont();
        ImGui::SameLine(0, st.S(10.0f));
        ImGui::TextDisabled("|  Hover row for socket & tunnel details, or right-click for process actions");

        // Process filter (Feature 11)
        ImGui::SetNextItemWidth(st.S(240.0f));
        ImGui::InputTextWithHint("##ProcFilter", "Filter processes by name or PID...",
                                 st.proc_filter_buf, sizeof(st.proc_filter_buf));
        if (st.proc_filter_buf[0] != '\0') {
            ImGui::SameLine(0, st.S(8.0f));
            if (ImGui::SmallButton("Clear##ProcFilter")) {
                st.proc_filter_buf[0] = '\0';
            }
        }

        if (exited_count > 0) {
            const char* clear_lbl = "Clear Exited";
            float btn_w = ImGui::CalcTextSize(clear_lbl).x + ImGui::GetStyle().FramePadding.x * 2.0f;
            rightAlignItem(btn_w);
            if (ImGui::SmallButton(clear_lbl)) {
                st.open_confirm_clear_exited = true;
            }
        }
        ImGui::Spacing();

        std::vector<size_t> prows;
        prows.reserve(snap.launched_processes.size());
        for (size_t i = 0; i < snap.launched_processes.size(); ++i) {
            const auto& pr = snap.launched_processes[i];
            std::string nm = fileNameUtf8(pr.exe_path);
            if (nm.empty()) nm = pr.exe_path;
            if (containsNoCase(nm, st.proc_filter_buf) ||
                containsNoCase(pr.exe_path, st.proc_filter_buf) ||
                containsNoCase(std::to_string(pr.pid), st.proc_filter_buf)) {
                prows.push_back(i);
            }
        }

        ImGuiTableFlags pflags =
            ImGuiTableFlags_BordersInnerH |
            ImGuiTableFlags_RowBg |
            ImGuiTableFlags_Resizable |
            ImGuiTableFlags_ScrollY |
            ImGuiTableFlags_SizingStretchProp |
            ImGuiTableFlags_Sortable |
            ImGuiTableFlags_SortTristate;

        float ptable_h = prows.empty() ? (ImGui::GetFrameHeightWithSpacing() * 1.4f) : 0.0f;
        if (ImGui::BeginTable("LaunchedProcsTableFull", 7, pflags, ImVec2(0, ptable_h))) {
            ImGui::TableSetupScrollFreeze(0, 1);
            ImGui::TableSetupColumn("Application", ImGuiTableColumnFlags_WidthStretch | ImGuiTableColumnFlags_DefaultSort, 1.45f);
            ImGui::TableSetupColumn("PID / Arch", ImGuiTableColumnFlags_WidthStretch, 0.85f);
            ImGui::TableSetupColumn("Status & Uptime", ImGuiTableColumnFlags_WidthStretch, 1.35f);
            ImGui::TableSetupColumn("Bound Virtual Ports", ImGuiTableColumnFlags_WidthStretch, 1.50f);
            ImGui::TableSetupColumn("TCP Streams", ImGuiTableColumnFlags_WidthStretch, 0.80f);
            ImGui::TableSetupColumn("VPN Traffic (TX / RX)", ImGuiTableColumnFlags_WidthStretch, 1.45f);
            ImGui::TableSetupColumn("Actions", ImGuiTableColumnFlags_WidthFixed | ImGuiTableColumnFlags_NoSort, st.S(145.0f));
            ImGui::TableHeadersRow();

            if (ImGuiTableSortSpecs* specs = ImGui::TableGetSortSpecs()) {
                if (specs->SpecsCount > 0) {
                    const ImGuiTableColumnSortSpecs& spec = specs->Specs[0];
                    bool asc = (spec.SortDirection == ImGuiSortDirection_Ascending);
                    std::stable_sort(prows.begin(), prows.end(), [&](size_t a, size_t b) {
                        const auto& pa = snap.launched_processes[a];
                        const auto& pb = snap.launched_processes[b];
                        auto shortname = [](const platform::LaunchedProcessInfo& p) {
                            std::string n = fileNameUtf8(p.exe_path);
                            return n.empty() ? p.exe_path : n;
                        };
                        auto tel_of = [&](uint32_t pid) -> const ProcessTrafficViewInfo* {
                            auto it = snap.process_telemetry.find(pid);
                            return (it != snap.process_telemetry.end()) ? &it->second : nullptr;
                        };
                        int c = 0;
                        switch (spec.ColumnIndex) {
                            case 0:
                                c = _stricmp(shortname(pa).c_str(), shortname(pb).c_str());
                                break;
                            case 1:
                                c = (pa.pid < pb.pid) ? -1 : (pa.pid > pb.pid ? 1 : 0);
                                break;
                            case 2: {
                                bool ae = st.proc_exited_at_us.count(pa.pid) > 0;
                                bool be = st.proc_exited_at_us.count(pb.pid) > 0;
                                c = static_cast<int>(be) - static_cast<int>(ae); // running first
                                break;
                            }
                            case 3: {
                                const auto* ta = tel_of(pa.pid);
                                const auto* tb = tel_of(pb.pid);
                                size_t ca = ta ? ta->bound_ports.size() : 0;
                                size_t cb = tb ? tb->bound_ports.size() : 0;
                                c = (ca < cb) ? -1 : (ca > cb ? 1 : 0);
                                break;
                            }
                            case 4: {
                                const auto* ta = tel_of(pa.pid);
                                const auto* tb = tel_of(pb.pid);
                                size_t ca = ta ? ta->tcp_tunnels.size() : 0;
                                size_t cb = tb ? tb->tcp_tunnels.size() : 0;
                                c = (ca < cb) ? -1 : (ca > cb ? 1 : 0);
                                break;
                            }
                            case 5: {
                                const auto* ta = tel_of(pa.pid);
                                const auto* tb = tel_of(pb.pid);
                                uint64_t va = ta ? (ta->tx_bytes + ta->rx_bytes) : 0;
                                uint64_t vb = tb ? (tb->tx_bytes + tb->rx_bytes) : 0;
                                c = (va < vb) ? -1 : (va > vb ? 1 : 0);
                                break;
                            }
                            default:
                                break;
                        }
                        if (c == 0) return a < b;
                        return asc ? (c < 0) : (c > 0);
                    });
                }
            }

            uint64_t now_us = platform::monotonicMicros();

            for (size_t r = 0; r < prows.size(); ++r) {
                const size_t i = prows[r];
                const auto& pr = snap.launched_processes[i];
                bool alive = false;
                uint32_t exit_code = 0;
                auto status_it = st.proc_status_cache.find(pr.pid);
                if (status_it != st.proc_status_cache.end()) {
                    alive = status_it->second.alive;
                    exit_code = status_it->second.exit_code;
                } else {
                    auto ec_it = st.cached_proc_exit_codes.find(pr.pid);
                    if (ec_it != st.cached_proc_exit_codes.end()) {
                        exit_code = ec_it->second;
                    }
                }

                uint64_t end_or_now_us = now_us;
                if (!alive) {
                    auto ex_it = st.proc_exited_at_us.find(pr.pid);
                    if (ex_it != st.proc_exited_at_us.end()) {
                        end_or_now_us = ex_it->second;
                    }
                }

                const ProcessTrafficViewInfo* tel = nullptr;
                auto tel_it = snap.process_telemetry.find(pr.pid);
                if (tel_it != snap.process_telemetry.end()) {
                    tel = &tel_it->second;
                }

                std::string short_name = fileNameUtf8(pr.exe_path);
                if (short_name.empty()) short_name = pr.exe_path;

                ImGui::TableNextRow();
                ImGui::PushID(static_cast<int>(i));

                // Column 0: Application Name + Activity Dot + Child Tag + Context Menu + Hover Tooltip
                ImGui::TableSetColumnIndex(0);
                ImVec2 dot_pos = ImGui::GetCursorScreenPos();
                ImGui::Dummy(ImVec2(st.S(16.0f), ImGui::GetTextLineHeight()));
                ImGui::SameLine(0, st.S(4.0f));
                const std::string proc_label = imguiLabelText(short_name) + "###ProcRow";
                ImGui::Selectable(proc_label.c_str(), false, ImGuiSelectableFlags_SpanAllColumns | ImGuiSelectableFlags_AllowOverlap);
                bool row_hovered = ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenBlockedByActiveItem);
                renderProcessActivityDotAt(st, dot_pos, pr.pid, alive);

                if (pr.parent_pid != 0) {
                    ImGui::SameLine(0, st.S(6.0f));
                    char child_lbl[48];
                    std::snprintf(child_lbl, sizeof(child_lbl), "Child of %u", pr.parent_pid);
                    drawPillBadge(st, child_lbl, pillColors(PillTone::Blue).bg, pillColors(PillTone::Blue).fg);
                } else if (!pr.args.empty()) {
                    ImGui::SameLine(0, st.S(6.0f));
                    ImGui::TextDisabled("(%s)", pr.args.c_str());
                }

                if (row_hovered) {
                    renderProcessHoverTooltip(st, snap, pr, alive, exit_code, end_or_now_us, tel);
                }

                if (ImGui::BeginPopupContextItem("ProcCtxMenu")) {
                    ImGui::TextDisabled("%s (PID %u)", short_name.c_str(), pr.pid);
                    ImGui::Separator();
                    if (alive) {
                        if (ImGui::MenuItem("Bring Window to Front")) {
                            if (!focusProcessWindow(pr.pid)) {
                                showBanner(st, "No visible top-level window found for PID " + std::to_string(pr.pid), true, 2.5);
                            }
                        }
                        if (ImGui::MenuItem("Terminate Process")) {
                            st.confirm_kill_pid = pr.pid;
                            st.confirm_kill_name = short_name;
                            st.open_confirm_kill = true;
                        }
                        ImGui::Separator();
                    }
                    if (pr.parent_pid == 0 && ImGui::MenuItem("Relaunch in VPN")) {
                        triggerLaunchApp(st, pr.exe_path, pr.args);
                    }
                    if (ImGui::MenuItem("Open Executable Folder")) {
                        openExecutableFolder(pr.exe_path);
                    }
                    if (ImGui::MenuItem("Copy Executable Path")) {
                        copyTextToClipboard(st.hWnd, pr.exe_path);
                        showBanner(st, "Copied executable path to clipboard!", false, 2.5);
                    }
                    if (ImGui::MenuItem("Copy PID")) {
                        copyTextToClipboard(st.hWnd, std::to_string(pr.pid));
                    }
                    ImGui::EndPopup();
                }

                // Column 1: PID & Architecture
                ImGui::TableSetColumnIndex(1);
                const char* arch_tag = (pr.arch == platform::BinaryArch::X86_64) ? "x64" :
                                       (pr.arch == platform::BinaryArch::X86_32) ? "x86" : "?";
                if (st.font_mono) ImGui::PushFont(st.font_mono);
                ImGui::Text("%u (%s)", pr.pid, arch_tag);
                if (st.font_mono) ImGui::PopFont();

                // Column 2: Status & Uptime / Exit Code + VIP Mismatch Warning
                ImGui::TableSetColumnIndex(2);
                uint64_t elapsed_sec = 0;
                if (pr.launched_since_us != 0 && end_or_now_us >= pr.launched_since_us) {
                    elapsed_sec = (end_or_now_us - pr.launched_since_us) / 1'000'000ULL;
                }
                char dur_short[32];
                if (elapsed_sec < 60) {
                    std::snprintf(dur_short, sizeof(dur_short), "%llus", static_cast<unsigned long long>(elapsed_sec));
                } else if (elapsed_sec < 3600) {
                    std::snprintf(dur_short, sizeof(dur_short), "%llum %llus",
                                  static_cast<unsigned long long>(elapsed_sec / 60),
                                  static_cast<unsigned long long>(elapsed_sec % 60));
                } else {
                    std::snprintf(dur_short, sizeof(dur_short), "%lluh %llum",
                                  static_cast<unsigned long long>(elapsed_sec / 3600),
                                  static_cast<unsigned long long>((elapsed_sec % 3600) / 60));
                }

                if (alive) {
                    drawPillBadge(st, "Running", pillColors(PillTone::Green).bg, pillColors(PillTone::Green).fg);
                    ImGui::SameLine(0, st.S(6.0f));
                    ImGui::TextDisabled("%s", dur_short);
                    if (pr.injected_vip != 0 && snap.connected && snap.local_vip != 0 && pr.injected_vip != snap.local_vip) {
                        ImGui::SameLine(0, st.S(6.0f));
                        drawPillBadge(st, "VIP Changed", pillColors(PillTone::Amber).bg, pillColors(PillTone::Amber).fg);
                    }
                } else {
                    char exit_badge[48];
                    if (exit_code == 0) {
                        std::snprintf(exit_badge, sizeof(exit_badge), "Exited (0)");
                        drawPillBadge(st, exit_badge, pillColors(PillTone::Muted).bg, pillColors(PillTone::Muted).fg);
                    } else if (exit_code > 0xFFFFu) {
                        std::snprintf(exit_badge, sizeof(exit_badge), "Exited (0x%X)", exit_code);
                        drawPillBadge(st, exit_badge, pillColors(PillTone::Red).bg, pillColors(PillTone::Red).fg);
                    } else {
                        std::snprintf(exit_badge, sizeof(exit_badge), "Exited (%u)", exit_code);
                        drawPillBadge(st, exit_badge, pillColors(PillTone::Orange).bg, pillColors(PillTone::Orange).fg);
                    }
                    ImGui::SameLine(0, st.S(6.0f));
                    ImGui::TextDisabled("%s", dur_short);
                }

                // Column 3: Bound Virtual Ports
                ImGui::TableSetColumnIndex(3);
                if (!tel || tel->bound_ports.empty()) {
                    ImGui::TextDisabled(alive ? "No sockets bound" : "-");
                } else {
                    size_t max_show = std::min<size_t>(tel->bound_ports.size(), 3);
                    for (size_t b_idx = 0; b_idx < max_show; ++b_idx) {
                        if (b_idx > 0) ImGui::SameLine(0, st.S(4.0f));
                        const auto& bp = tel->bound_ports[b_idx];
                        bool is_tcp = (bp.proto == IpcProto::Tcp);
                        char port_lbl[32];
                        std::snprintf(port_lbl, sizeof(port_lbl), "%s :%u", is_tcp ? "TCP" : "UDP", bp.virtual_port);
                        drawPillBadge(st, port_lbl,
                                      (is_tcp ? pillColors(PillTone::DeepBlue) : pillColors(PillTone::TealGreen)).bg,
                                      (is_tcp ? pillColors(PillTone::DeepBlue) : pillColors(PillTone::TealGreen)).fg);
                    }
                    if (tel->bound_ports.size() > max_show) {
                        ImGui::SameLine(0, st.S(4.0f));
                        char more_lbl[24];
                        std::snprintf(more_lbl, sizeof(more_lbl), "+%zu", tel->bound_ports.size() - max_show);
                        drawPillBadge(st, more_lbl, pillColors(PillTone::Slate).bg, pillColors(PillTone::Slate).fg);
                    }
                }

                // Column 4: Active Virtual TCP Streams
                ImGui::TableSetColumnIndex(4);
                if (!tel || tel->tcp_tunnels.empty()) {
                    ImGui::TextDisabled("-");
                } else {
                    char tcp_lbl[32];
                    std::snprintf(tcp_lbl, sizeof(tcp_lbl), "%zu active", tel->tcp_tunnels.size());
                    drawPillBadge(st, tcp_lbl, pillColors(PillTone::DeepBlue).bg, pillColors(PillTone::DeepBlue).fg);
                }

                // Column 5: Per-Process VPN Traffic (TX / RX)
                ImGui::TableSetColumnIndex(5);
                uint64_t tx_b = tel ? tel->tx_bytes : 0;
                uint64_t rx_b = tel ? tel->rx_bytes : 0;
                uint64_t tx_p = tel ? tel->tx_packets : 0;
                uint64_t rx_p = tel ? tel->rx_packets : 0;
                if (tx_p == 0 && rx_p == 0) {
                    ImGui::TextDisabled("TX: 0 B  /  RX: 0 B");
                } else {
                    ImGui::Text("TX: %s (%llu) / RX: %s (%llu)",
                                formatBytes(tx_b).c_str(), static_cast<unsigned long long>(tx_p),
                                formatBytes(rx_b).c_str(), static_cast<unsigned long long>(rx_p));
                }

                // Column 6: Quick Actions
                ImGui::TableSetColumnIndex(6);
                if (alive) {
                    if (ImGui::SmallButton("Focus")) {
                        if (!focusProcessWindow(pr.pid)) {
                            showBanner(st, "No visible window found for PID " + std::to_string(pr.pid), true, 2.5);
                        }
                    }
                    if (ImGui::IsItemHovered()) {
                        ImGui::SetTooltip("Bring application window to front");
                    }
                    ImGui::SameLine(0, st.S(6.0f));
                    ImGui::PushStyleColor(ImGuiCol_Button, g_pal.btn_red);
                    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, g_pal.btn_red_hover);
                    if (ImGui::SmallButton("Kill")) {
                        st.confirm_kill_pid = pr.pid;
                        st.confirm_kill_name = short_name;
                        st.open_confirm_kill = true;
                    }
                    ImGui::PopStyleColor(2);
                    if (ImGui::IsItemHovered()) {
                        ImGui::SetTooltip("Terminate process (PID %u)", pr.pid);
                    }
                } else {
                    if (pr.parent_pid == 0) {
                        ImGui::PushStyleColor(ImGuiCol_Button, g_pal.btn_green);
                        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, g_pal.btn_green_hover);
                        if (ImGui::SmallButton("Relaunch")) {
                            triggerLaunchApp(st, pr.exe_path, pr.args);
                        }
                        ImGui::PopStyleColor(2);
                        ImGui::SameLine(0, st.S(6.0f));
                    }
                    if (ImGui::SmallButton("Folder")) {
                        openExecutableFolder(pr.exe_path);
                    }
                    if (ImGui::IsItemHovered()) {
                        ImGui::SetTooltip("Open folder in Explorer");
                    }
                }

                ImGui::PopID();
            }
            ImGui::EndTable();
        }

        if (snap.launched_processes.empty()) {
            ImGui::Spacing();
            const char* empty_proc = "No processes launched in this session yet. Click 'Launch' on a saved game above or 'Start in VPN'.";
            float avail_w = ImGui::GetContentRegionAvail().x;
            float txt_w = ImGui::CalcTextSize(empty_proc).x;
            if (avail_w > txt_w) {
                ImGui::SetCursorPosX(ImGui::GetCursorPosX() + (avail_w - txt_w) * 0.5f);
            }
            ImGui::TextDisabled("%s", empty_proc);
        } else if (prows.empty()) {
            ImGui::Spacing();
            ImGui::TextDisabled("No launched processes match the current filter.");
        }

        // Confirmation modal for terminating a process (Feature 5)
        if (st.open_confirm_kill) {
            ImGui::OpenPopup("Terminate Process");
            st.open_confirm_kill = false;
        }
        if (ImGui::BeginPopupModal("Terminate Process", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
            ImGui::Text("Terminate \"%s\" (PID %u)?", st.confirm_kill_name.c_str(), st.confirm_kill_pid);
            ImGui::TextDisabled("Unsaved progress in the application will be lost.");
            ImGui::Spacing();
            ImGui::PushStyleColor(ImGuiCol_Button, g_pal.btn_red);
            ImGui::PushStyleColor(ImGuiCol_ButtonHovered, g_pal.btn_red_hover);
            if (ImGui::Button("Terminate", ImVec2(st.S(110.0f), 0))) {
                if (st.client->terminateLaunchedApp(st.confirm_kill_pid)) {
                    showBanner(st, "Terminated PID " + std::to_string(st.confirm_kill_pid), false, 2.5);
                } else {
                    showBanner(st, "Failed to terminate PID " + std::to_string(st.confirm_kill_pid), true, 2.5);
                }
                st.confirm_kill_pid = 0;
                ImGui::CloseCurrentPopup();
            }
            ImGui::PopStyleColor(2);
            ImGui::SameLine();
            if (ImGui::Button("Cancel", ImVec2(st.S(110.0f), 0))) {
                st.confirm_kill_pid = 0;
                ImGui::CloseCurrentPopup();
            }
            ImGui::EndPopup();
        }

        // Confirmation modal for clearing exited processes (Feature 5)
        if (st.open_confirm_clear_exited) {
            ImGui::OpenPopup("Clear Exited Processes");
            st.open_confirm_clear_exited = false;
        }
        if (ImGui::BeginPopupModal("Clear Exited Processes", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
            ImGui::Text("Remove all exited processes from the list?");
            ImGui::Spacing();
            if (ImGui::Button("Clear", ImVec2(st.S(110.0f), 0))) {
                st.client->clearExitedProcesses();
                showBanner(st, "Cleared exited processes from list.", false, 2.5);
                ImGui::CloseCurrentPopup();
            }
            ImGui::SameLine();
            if (ImGui::Button("Cancel", ImVec2(st.S(110.0f), 0))) {
                ImGui::CloseCurrentPopup();
            }
            ImGui::EndPopup();
        }
    }
    ImGui::EndChild();
}

void renderSettingsView(GuiState& st, const ClientStatusSnapshot& snap) {
    if (st.font_heading) ImGui::PushFont(st.font_heading);
    ImGui::TextUnformatted("Settings & Gateway Diagnostics");
    if (st.font_heading) ImGui::PopFont();
    ImGui::TextDisabled("Configure packet routing, system tray behavior, and inspect local Winsock shim gateways.");
    ImGui::Spacing();

    ImGui::BeginChild("##RoutingSettingsCard", ImVec2(0, 0),
                      ImGuiChildFlags_Borders | ImGuiChildFlags_AutoResizeY,
                      ImGuiWindowFlags_NoScrollbar);
    {
        if (st.font_bold) ImGui::PushFont(st.font_bold);
        ImGui::TextUnformatted("Virtual LAN Routing");
        if (st.font_bold) ImGui::PopFont();
        ImGui::Spacing();

        if (ImGui::Checkbox("Force Server Relay Mode (Disable Direct P2P UDP Hole-Punching)", &st.force_relay)) {
            st.client->setForceRelay(st.force_relay);
            saveSettings(st);
        }
        ImGui::PushTextWrapPos(0.0f);
        ImGui::TextDisabled("When unchecked (recommended), APIVPN automatically negotiates low-latency direct UDP P2P links "
                            "with peers and falls back to the Server Relay only when strict NATs block hole-punching.");
        ImGui::PopTextWrapPos();
    }
    ImGui::EndChild();

    ImGui::BeginChild("##TraySettingsCard", ImVec2(0, 0),
                      ImGuiChildFlags_Borders | ImGuiChildFlags_AutoResizeY,
                      ImGuiWindowFlags_NoScrollbar);
    {
        if (st.font_bold) ImGui::PushFont(st.font_bold);
        ImGui::TextUnformatted("Window & System Tray Behavior");
        if (st.font_bold) ImGui::PopFont();
        ImGui::Spacing();

        if (ImGui::Checkbox("Minimize to System Tray when window is minimized", &st.minimize_to_tray)) {
            saveSettings(st);
        }
        if (ImGui::Checkbox("Keep APIVPN running in System Tray when closing window (X button)", &st.close_to_tray)) {
            saveSettings(st);
        }
        if (ImGui::Checkbox("Exit APIVPN automatically when launched game ends", &st.exit_when_game_ends)) {
            // Arm only for games that are still running now (or launched later), so
            // ticking this after an earlier game already ended does not exit at once.
            armExitWhenGameEnds(st, st.exit_when_game_ends && st.client->hasActiveLaunchedApps());
        }
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip("Applies to games that are running now or launched after enabling this.");
        }
        ImGui::Spacing();
        if (ImGui::Button("Hide Window to Tray Now")) {
            hideWindowToTray(st.hWnd, st);
        }
    }
    ImGui::EndChild();

    ImGui::BeginChild("##AppearanceCard", ImVec2(0, 0),
                      ImGuiChildFlags_Borders | ImGuiChildFlags_AutoResizeY,
                      ImGuiWindowFlags_NoScrollbar);
    {
        if (st.font_bold) ImGui::PushFont(st.font_bold);
        ImGui::TextUnformatted("Appearance & Notifications");
        if (st.font_bold) ImGui::PopFont();
        ImGui::Spacing();

        const char* theme_items[] = { "Dark", "Light", "Follow Windows" };
        int cur_theme = static_cast<int>(st.theme_mode);
        ImGui::AlignTextToFramePadding();
        ImGui::TextUnformatted("Theme:");
        ImGui::SameLine(0, st.S(8.0f));
        ImGui::SetNextItemWidth(st.S(200.0f));
        if (ImGui::Combo("##ThemeCombo", &cur_theme, theme_items, IM_ARRAYSIZE(theme_items))) {
            st.theme_mode = static_cast<ThemeMode>(cur_theme);
            applyThemeAndWindow(st.hWnd, st);
            saveSettings(st);
        }

        if (ImGui::Checkbox("Notify me when peers join or leave the lobby", &st.notify_peer_events)) {
            saveSettings(st);
        }
        if (ImGui::Checkbox("Play a sound on peer join/leave", &st.notify_sounds)) {
            saveSettings(st);
        }
    }
    ImGui::EndChild();

    ImGui::BeginChild("##DiagnosticsCard", ImVec2(0, 0), ImGuiChildFlags_Borders);
    {
        if (st.font_bold) ImGui::PushFont(st.font_bold);
        ImGui::TextUnformatted("Local Hook Gateway & Configuration Diagnostics");
        if (st.font_bold) ImGui::PopFont();
        ImGui::Spacing();

        if (st.font_mono) ImGui::PushFont(st.font_mono);
        ImGui::Text("Virtual LAN Subnet : 10.42.0.0/24");
        ImGui::Text("Assigned Virtual IP: %s", snap.connected ? ipv4ToString(snap.local_vip).c_str() : "Not connected (games cannot be launched)");
        ImGui::Text("Server Host / Port : %s", snap.connected ? platform::formatHostPort(snap.server_host, snap.server_port).c_str() : "Not connected");
        ImGui::Text("Server Endpoint    : %s (%s)",
                    snap.connected ? snap.server_endpoint.toString().c_str() : "-",
                    snap.connected ? snap.server_endpoint.versionString() : "None");
        ImGui::Text("Local IPC Port     : 127.0.0.1:%u", snap.ipc_port);
        ImGui::Text("Local UDP Gateway  : 127.0.0.1:%u", snap.udp_gw_port);
        ImGui::Text("Local TCP Gateway  : 127.0.0.1:%u", snap.tcp_gw_port);
        ImGui::Text("Config File Path   : %s", pathToUtf8(getConfigFilePath()).c_str());
        if (st.font_mono) ImGui::PopFont();

        ImGui::Spacing();
        if (ImGui::Button("Copy Diagnostics to Clipboard")) {
            std::ostringstream oss;
            oss << "APIVPN Diagnostics\n"
                << "Connected: " << (snap.connected ? "Yes" : "No") << "\n"
                << "Server: " << (snap.connected ? platform::formatHostPort(snap.server_host, snap.server_port) : "None")
                << " [" << snap.server_endpoint.versionString() << " " << snap.server_endpoint.toString() << "]\n"
                << "Virtual IP: " << ipv4ToString(snap.local_vip) << "\n"
                << "IPC Port: " << snap.ipc_port << " | UDP-GW: " << snap.udp_gw_port << " | TCP-GW: " << snap.tcp_gw_port << "\n";
            copyTextToClipboard(st.hWnd, oss.str());
            showBanner(st, "Copied gateway diagnostics to clipboard!", false, 2.5);
        }
    }
    ImGui::EndChild();

    ImGui::BeginChild("##TcpHealthCard", ImVec2(0, 0),
                      ImGuiChildFlags_Borders | ImGuiChildFlags_AutoResizeY,
                      ImGuiWindowFlags_NoScrollbar);
    {
        if (st.font_bold) ImGui::PushFont(st.font_bold);
        ImGui::TextUnformatted("Virtual TCP Tunnel Health (Session Counters)");
        if (st.font_bold) ImGui::PopFont();
        ImGui::Spacing();

        if (st.font_mono) ImGui::PushFont(st.font_mono);
        ImGui::Text("Fast Retransmits   : %llu", static_cast<unsigned long long>(snap.tcp_fast_retransmits));
        ImGui::Text("RTO Retransmits    : %llu", static_cast<unsigned long long>(snap.tcp_rto_retransmits));
        ImGui::Text("SACK Events        : %llu", static_cast<unsigned long long>(snap.tcp_sack_events));
        ImGui::Text("Simulated Drops    : %llu", static_cast<unsigned long long>(snap.tcp_simulated_drops));
        ImGui::Text("Congestion Window  : %llu scale-ups / %llu scale-downs",
                    static_cast<unsigned long long>(snap.tcp_cwnd_scale_ups),
                    static_cast<unsigned long long>(snap.tcp_cwnd_scale_downs));
        if (st.font_mono) ImGui::PopFont();

        ImGui::Spacing();
        ImGui::PushTextWrapPos(0.0f);
        ImGui::TextDisabled("Counters accumulate for the current session and reset on reconnect. High RTO retransmits or "
                            "scale-downs can indicate packet loss on the active route; try toggling Force Relay to compare paths.");
        ImGui::PopTextWrapPos();
    }
    ImGui::EndChild();
}

void processPeerNotifications(GuiState& st, const ClientStatusSnapshot& snap) {
    if (!snap.connected) {
        // Reset the baseline while offline so reconnects don't emit spurious join/leave spam.
        if (st.peer_set_initialized) {
            st.peer_set_initialized = false;
            st.known_peer_vips.clear();
            st.known_peer_names.clear();
        }
        return;
    }

    std::unordered_set<uint32_t> current;
    for (const auto& p : snap.peers) current.insert(p.vip);

    if (!st.peer_set_initialized) {
        st.peer_set_initialized = true;
        st.known_peer_vips = current;
        st.known_peer_names.clear();
        for (const auto& p : snap.peers) st.known_peer_names[p.vip] = p.nickname;
        return;
    }

    if (st.notify_peer_events) {
        for (const auto& p : snap.peers) {
            if (st.known_peer_vips.find(p.vip) == st.known_peer_vips.end()) {
                showBanner(st, p.nickname + " (" + ipv4ToString(p.vip) + ") joined the lobby", false, 4.0);
                if (st.notify_sounds) MessageBeep(MB_ICONASTERISK);
            }
        }
        for (uint32_t vip : st.known_peer_vips) {
            if (current.find(vip) == current.end()) {
                auto it = st.known_peer_names.find(vip);
                std::string name = (it != st.known_peer_names.end() && !it->second.empty()) ? it->second : ipv4ToString(vip);
                showBanner(st, name + " left the lobby", true, 4.0);
                if (st.notify_sounds) MessageBeep(MB_ICONHAND);
            }
        }
    }

    st.known_peer_vips = current;
    st.known_peer_names.clear();
    for (const auto& p : snap.peers) st.known_peer_names[p.vip] = p.nickname;
}


} // namespace apivpn::client
