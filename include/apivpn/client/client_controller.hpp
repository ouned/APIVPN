#pragma once

#include "apivpn/common/protocol.hpp"
#include "apivpn/platform/net_socket.hpp"
#include "apivpn/platform/process_launcher.hpp"
#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

namespace apivpn::client {

enum class RouteMode {
    ServerRelay = 0,
    DirectP2P   = 1,
};

struct PeerViewInfo {
    uint32_t                        vip                 = 0;
    std::string                     nickname;
    RouteMode                       route_mode          = RouteMode::ServerRelay;
    platform::Endpoint              active_endpoint;
    platform::Endpoint              best_v4_endpoint;
    platform::Endpoint              best_v6_endpoint;
    std::vector<platform::Endpoint> candidate_endpoints;
    platform::IpVersion             ip_version          = platform::IpVersion::None;
    bool                            hole_punch_gave_up  = false;
    uint32_t                        hole_punch_cycles   = 0;
    uint64_t                        punch_probes_sent   = 0;
    uint64_t                        keepalive_tx        = 0;
    uint64_t                        keepalive_rx        = 0;
    uint64_t                        connected_since_us  = 0;
    int64_t                         connected_epoch_sec = 0;
    uint64_t                        last_recv_us        = 0;
    uint32_t                        rtt_ms              = 0;
    uint32_t                        rtt_v4_ms           = 0;
    uint32_t                        rtt_v6_ms           = 0;
    uint64_t                        tx_packets          = 0;
    uint64_t                        rx_packets          = 0;
    uint64_t                        tx_bytes            = 0;
    uint64_t                        rx_bytes            = 0;
};

struct ChatHistoryEntry {
    uint32_t    sender_vip = 0;
    std::string sender_name;
    std::string message;
};

struct BoundPortViewInfo {
    IpcProto proto        = IpcProto::Udp;
    uint16_t virtual_port = 0;
    uint16_t actual_port  = 0;
    uint32_t pid          = 0;
};

struct TcpTunnelViewInfo {
    uint32_t conn_id            = 0;
    uint32_t pid                = 0;
    uint16_t local_vport        = 0;
    uint32_t remote_vip         = 0;
    uint16_t remote_vport       = 0;
    bool     handshake_done     = false;
    uint32_t rtt_ms             = 0;
    uint32_t cwnd_segments      = 0;
    uint32_t in_flight_segments = 0;
};

struct ProcessTrafficViewInfo {
    uint32_t                       pid              = 0;
    uint64_t                       tx_packets       = 0;
    uint64_t                       rx_packets       = 0;
    uint64_t                       tx_bytes         = 0;
    uint64_t                       rx_bytes         = 0;
    uint64_t                       last_activity_us = 0;
    std::vector<BoundPortViewInfo> bound_ports;
    std::vector<TcpTunnelViewInfo> tcp_tunnels;
};

struct ClientStatusSnapshot {
    bool                connected             = false;
    bool                reconnecting          = false;
    uint32_t            reconnect_attempts    = 0;
    uint64_t            last_reconnect_us     = 0;
    uint64_t            reconnect_interval_us = 0;
    std::string         server_host;
    uint16_t            server_port       = 0;
    platform::Endpoint  server_endpoint;
    platform::IpVersion server_ip_version = platform::IpVersion::None;
    std::string         nickname;
    uint32_t            local_vip         = 0;
    bool                force_relay       = false;
    uint16_t            ipc_port          = 0;
    uint16_t            udp_gw_port       = 0;
    uint16_t            tcp_gw_port       = 0;
    uint64_t            total_tx_packets  = 0;
    uint64_t            total_rx_packets  = 0;
    uint64_t            tcp_simulated_drops  = 0;
    uint64_t            tcp_fast_retransmits = 0;
    uint64_t            tcp_rto_retransmits  = 0;
    uint64_t            tcp_sack_events      = 0;
    uint64_t            tcp_cwnd_scale_ups   = 0;
    uint64_t            tcp_cwnd_scale_downs = 0;
    // Aggregate counts for leak / lifecycle assertions (do not require walking telemetry)
    uint32_t            peer_count           = 0;
    uint32_t            bound_socket_count   = 0;
    uint32_t            tcp_tunnel_count     = 0;
    std::vector<PeerViewInfo>                              peers;
    std::vector<TcpTunnelViewInfo>                         tcp_tunnels;
    std::vector<ChatHistoryEntry>                          chat_messages;
    std::vector<platform::LaunchedProcessInfo>             launched_processes;
    std::unordered_map<uint32_t, ProcessTrafficViewInfo>   process_telemetry;
};

struct GuiLaunchOptions {
    bool start_in_tray       = false;
    bool exit_when_game_ends = false;

    // Connect on startup (in the background) with these settings; empty / 0 fields
    // keep the values loaded from the INI. All strings are UTF-8.
    bool        auto_connect = false;
    std::string server_host;
    uint16_t    server_port  = 0;
    std::string nickname;
    bool        force_relay  = false;

    // Executable launched once the client has a virtual IP (empty = none).
    // launch_args is an already-quoted command-line fragment.
    std::string launch_exe;
    std::string launch_args;
};

} // namespace apivpn::client
