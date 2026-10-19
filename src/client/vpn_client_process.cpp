#include "apivpn/client/vpn_client.hpp"
#include "apivpn/common/logger.hpp"

#include <algorithm>
#include <chrono>
#include <cstring>
#include <memory>
#include <thread>
#include <vector>

// Purpose: VpnClient launched-process tracking, launch/terminate, and status snapshots.

namespace apivpn::client {

void VpnClient::ensureProcessTrackedLocked(uint32_t pid) {
    if (pid == 0) return;
    for (const auto& existing : launched_processes_) {
        if (existing.pid == pid) return;
    }

    std::string exe_path;
    uint32_t parent_pid = 0;
    platform::ProcessLauncher::queryProcessDetails(pid, exe_path, parent_pid);
    if (!exe_path.empty()) {
        size_t slash = exe_path.find_last_of("\\/");
        const char* base = (slash != std::string::npos) ? (exe_path.c_str() + slash + 1) : exe_path.c_str();
        if (_stricmp(base, "conhost.exe") == 0 ||
            _stricmp(base, "apivpn_injector32.exe") == 0 ||
            _stricmp(base, "apivpn_injector64.exe") == 0 ||
            _stricmp(base, "werfault.exe") == 0) {
            return;
        }
    } else {
        exe_path = "PID " + std::to_string(pid);
    }

    platform::LaunchedProcessInfo info{};
    info.pid = pid;
    info.parent_pid = parent_pid;
    info.arch = platform::ProcessLauncher::detectProcessArch(pid);
    info.exe_path = exe_path;
    size_t slash_pos = exe_path.find_last_of("\\/");
    if (slash_pos != std::string::npos) {
        info.working_dir = exe_path.substr(0, slash_pos);
    }
    // 0 when not connected: never guess a VIP that may belong to another peer.
    info.injected_vip = local_vip_.load();
    info.launched_since_us = platform::monotonicMicros();
    info.launched_epoch_sec = std::chrono::duration_cast<std::chrono::seconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
    info.success = true;
    launched_processes_.push_back(std::move(info));
}

platform::LaunchedProcessInfo VpnClient::launchApp(
    const std::string& exe_path,
    const std::string& args,
    const std::string& working_dir) {

    uint32_t vip = 0;
    uint64_t token = 0;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        vip = local_vip_.load();
        token = session_token_;
    }
    if (vip == 0) {
        // Injecting a guessed VIP would make the app impersonate whichever peer holds it.
        platform::LaunchedProcessInfo failed{};
        failed.exe_path = exe_path;
        failed.args = args;
        failed.working_dir = working_dir;
        failed.success = false;
        failed.error_msg = "Not connected to a VPN server (no virtual IP assigned yet)";
        return failed;
    }

    std::map<std::string, std::string> env;
    env[kEnvVirtualIp] = ipv4ToString(vip);
    env[kEnvIpcPort] = std::to_string(ipc_port_);
    env[kEnvUdpGwPort] = std::to_string(udp_gw_port_);
    env[kEnvTcpGwPort] = std::to_string(tcp_gw_port_);
    env[kEnvSessionToken] = std::to_string(token);

    auto info = platform::ProcessLauncher::launchHookedProcess(exe_path, args, working_dir, env);
    if (info.success) {
        info.injected_vip = vip;
        info.launched_since_us = platform::monotonicMicros();
        info.launched_epoch_sec = std::chrono::duration_cast<std::chrono::seconds>(
            std::chrono::system_clock::now().time_since_epoch()).count();
        std::lock_guard<std::mutex> lock(mutex_);
        launched_processes_.push_back(info);
        proc_telemetry_[info.pid]; // initialize telemetry entry
    }
    return info;
}

bool VpnClient::terminateLaunchedApp(uint32_t pid) {
    if (pid == 0) return false;
    bool ok = platform::ProcessLauncher::terminateProcess(pid);
    if (ok) {
        std::lock_guard<std::mutex> lock(mutex_);
        bound_sockets_.erase(
            std::remove_if(bound_sockets_.begin(), bound_sockets_.end(),
                           [pid](const BoundSocketEntry& b) { return b.pid == pid; }),
            bound_sockets_.end());
    }
    return ok;
}

void VpnClient::clearExitedProcesses() {
    std::lock_guard<std::mutex> lock(mutex_);
    launched_processes_.erase(
        std::remove_if(launched_processes_.begin(), launched_processes_.end(),
                       [](const platform::LaunchedProcessInfo& pr) {
                           return pr.pid == 0 || !platform::ProcessLauncher::isProcessAlive(pr.pid);
                       }),
        launched_processes_.end());
}

bool VpnClient::hasActiveLaunchedApps() {
    std::vector<uint32_t> known_pids;
    int64_t min_epoch_sec = 0;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (launched_processes_.empty()) return false;
        for (const auto& pr : launched_processes_) {
            if (pr.pid == 0) continue;
            known_pids.push_back(pr.pid);
            if (pr.launched_epoch_sec > 0 && (min_epoch_sec == 0 || pr.launched_epoch_sec < min_epoch_sec)) {
                min_epoch_sec = pr.launched_epoch_sec;
            }
            if (!platform::ProcessLauncher::isBackgroundHelperExecutable(pr.exe_path) &&
                platform::ProcessLauncher::isProcessAlive(pr.pid)) {
                return true;
            }
        }
    }

    auto alive_children = platform::ProcessLauncher::findAliveChildProcesses(known_pids, min_epoch_sec);
    if (!alive_children.empty()) {
        std::lock_guard<std::mutex> lock(mutex_);
        for (uint32_t child_pid : alive_children) {
            ensureProcessTrackedLocked(child_pid);
            proc_telemetry_[child_pid];
        }
        return true;
    }
    return false;
}

ClientStatusSnapshot VpnClient::getSnapshot() const {
    std::lock_guard<std::mutex> lock(mutex_);
    ClientStatusSnapshot snap{};
    snap.connected = connected_.load();
    snap.reconnecting = reconnecting_.load();
    snap.reconnect_attempts = reconnect_attempts_.load();
    snap.last_reconnect_us = last_reconnect_attempt_us_;
    snap.reconnect_interval_us = reconnect_interval_us_.load();
    snap.server_host = server_ip_str_;
    snap.server_port = server_tcp_port_;
    snap.server_endpoint = server_endpoint_;
    snap.server_ip_version = connected_.load() ? server_endpoint_.version : platform::IpVersion::None;
    snap.nickname = nickname_;
    snap.local_vip = local_vip_.load();
    snap.force_relay = force_relay_.load();
    snap.ipc_port = ipc_port_;
    snap.udp_gw_port = udp_gw_port_;
    snap.tcp_gw_port = tcp_gw_port_;
    snap.total_tx_packets = total_tx_packets_;
    snap.total_rx_packets = total_rx_packets_;
    snap.tcp_simulated_drops = tcp_simulated_drops_;
    snap.tcp_fast_retransmits = tcp_fast_retransmits_;
    snap.tcp_rto_retransmits = tcp_rto_retransmits_;
    snap.tcp_sack_events = tcp_sack_events_;
    snap.tcp_cwnd_scale_ups = tcp_cwnd_scale_ups_;
    snap.tcp_cwnd_scale_downs = tcp_cwnd_scale_downs_;

    for (const auto& kv : peers_) {
        const auto& p = kv.second;
        PeerViewInfo pv{};
        pv.vip = p.vip;
        pv.nickname = p.nickname;
        pv.route_mode = force_relay_.load() ? RouteMode::ServerRelay : p.route_mode;
        pv.active_endpoint = p.active_endpoint;
        pv.best_v4_endpoint = p.best_v4_endpoint;
        pv.best_v6_endpoint = p.best_v6_endpoint;
        pv.candidate_endpoints = p.candidate_endpoints;
        if (pv.route_mode == RouteMode::DirectP2P && p.active_endpoint.port_host != 0) {
            pv.ip_version = p.active_endpoint.version;
        } else {
            pv.ip_version = snap.server_ip_version;
        }
        pv.hole_punch_gave_up = p.punch_gave_up;
        pv.hole_punch_cycles = p.hole_punch_cycles;
        pv.punch_probes_sent = p.punch_probes_sent;
        pv.keepalive_tx = p.keepalive_tx_count;
        pv.keepalive_rx = p.keepalive_rx_count;
        pv.connected_since_us = p.connected_since_us != 0 ? p.connected_since_us : p.punch_start_us;
        pv.connected_epoch_sec = p.connected_epoch_sec;
        pv.last_recv_us = p.last_any_recv_us;
        pv.rtt_ms = p.rtt_ms;
        pv.rtt_v4_ms = p.rtt_v4_ms;
        pv.rtt_v6_ms = p.rtt_v6_ms;
        pv.tx_packets = p.tx_packets;
        pv.rx_packets = p.rx_packets;
        pv.tx_bytes = p.tx_bytes;
        pv.rx_bytes = p.rx_bytes;
        snap.peers.push_back(pv);
    }
    std::sort(snap.peers.begin(), snap.peers.end(), [](const PeerViewInfo& a, const PeerViewInfo& b) {
        return a.vip < b.vip;
    });

    snap.chat_messages = chat_history_;
    snap.launched_processes = launched_processes_;

    snap.peer_count = static_cast<uint32_t>(peers_.size());
    snap.bound_socket_count = static_cast<uint32_t>(bound_sockets_.size());
    snap.tcp_tunnel_count = static_cast<uint32_t>(tcp_tunnels_.size());

    for (const auto& pr : launched_processes_) {
        if (pr.pid == 0) continue;
        auto& pt = snap.process_telemetry[pr.pid];
        pt.pid = pr.pid;
        auto tel_it = proc_telemetry_.find(pr.pid);
        if (tel_it != proc_telemetry_.end()) {
            pt.tx_packets = tel_it->second.tx_packets;
            pt.rx_packets = tel_it->second.rx_packets;
            pt.tx_bytes = tel_it->second.tx_bytes;
            pt.rx_bytes = tel_it->second.rx_bytes;
            pt.last_activity_us = tel_it->second.last_activity_us;
        }
    }

    for (const auto& b : bound_sockets_) {
        if (b.pid == 0) continue;
        auto& pt = snap.process_telemetry[b.pid];
        pt.pid = b.pid;
        BoundPortViewInfo bp{};
        bp.proto = b.proto;
        bp.virtual_port = b.virtual_port;
        bp.actual_port = b.actual_port;
        bp.pid = b.pid;
        pt.bound_ports.push_back(bp);
    }

    for (const auto& kv : tcp_tunnels_) {
        const auto& t = *kv.second;
        if (t.pid == 0 || t.handshake_failed) continue;
        auto& pt = snap.process_telemetry[t.pid];
        pt.pid = t.pid;
        TcpTunnelViewInfo tv{};
        tv.conn_id = t.conn_id;
        tv.pid = t.pid;
        tv.local_vport = t.local_vport;
        tv.remote_vip = t.remote_vip;
        tv.remote_vport = t.remote_vport;
        tv.handshake_done = t.handshake_done;
        tv.rtt_ms = static_cast<uint32_t>((t.srtt_us + 500) / 1000);
        tv.cwnd_segments = static_cast<uint32_t>(t.cwnd_segments);
        tv.in_flight_segments = static_cast<uint32_t>(t.inFlightSegments());
        pt.tcp_tunnels.push_back(tv);
    }

    // Top-level tunnel list (includes pid==0 bridges) for lifecycle / leak assertions
    snap.tcp_tunnels.reserve(tcp_tunnels_.size());
    for (const auto& kv : tcp_tunnels_) {
        const auto& t = *kv.second;
        TcpTunnelViewInfo tv{};
        tv.conn_id = t.conn_id;
        tv.pid = t.pid;
        tv.local_vport = t.local_vport;
        tv.remote_vip = t.remote_vip;
        tv.remote_vport = t.remote_vport;
        tv.handshake_done = t.handshake_done;
        tv.rtt_ms = static_cast<uint32_t>((t.srtt_us + 500) / 1000);
        tv.cwnd_segments = static_cast<uint32_t>(t.cwnd_segments);
        tv.in_flight_segments = static_cast<uint32_t>(t.inFlightSegments());
        snap.tcp_tunnels.push_back(tv);
    }

    return snap;
}


} // namespace apivpn::client
