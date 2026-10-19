#pragma once

#include "apivpn/client/client_controller.hpp"
#include "apivpn/common/byte_buffer.hpp"
#include "apivpn/common/protocol.hpp"
#include "apivpn/platform/net_socket.hpp"
#include "apivpn/platform/process_launcher.hpp"

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace apivpn::client {

// Upper bound on queued relay/control bytes for the server TCP connection.
// Dropping relayed frames past this point is safe because the virtual TCP layer
// retransmits them, and it keeps one slow relay path from exhausting memory.
constexpr size_t kMaxServerSendBufBytes = 8 * 1024 * 1024;

// Bounds on concurrently tracked virtual-TCP tunnels so a peer flooding SYNs cannot exhaust
// sockets or memory. SYNs beyond either cap are refused with RST.
constexpr size_t kMaxTcpTunnelsPerPeer = 256;
constexpr size_t kMaxTcpTunnelsTotal   = 1024;

// An outgoing tunnel whose hooked app never connects to the loopback TCP gateway is aborted
// (RST to the peer) after this long instead of lingering until the client stops.
constexpr uint64_t kTcpTunnelAttachTimeoutUs = 30'000'000ULL;

// A segment retransmitted more than this many times aborts its tunnel (RST), unless the peer
// is advertising a closed receive window (persist: the app on the other side is just slow).
constexpr uint32_t kMaxTcpSegmentRetransmits = 30;

// Lobby chat lines kept for the GUI (which copies the history every frame); oldest dropped first.
constexpr size_t kMaxChatHistoryMessages = 500;

class VpnClient {
public:
    VpnClient();
    ~VpnClient();

    // Start local hook gateways (IPC, UDP loopback gateway, TCP loopback gateway)
    bool initLocalGateways();

    // Connect to central coordination & relay server
    bool connectToServer(const std::string& server_ip, uint16_t server_port, const std::string& nickname);
    void disconnect();
    void stop();

    bool isConnected() const { return connected_.load(); }
    bool isReconnecting() const { return reconnecting_.load(); }
    uint32_t localVip() const { return local_vip_.load(); }

    void setForceRelay(bool force);
    bool isForceRelay() const { return force_relay_.load(); }

    // Timeout & periodic retry configuration (defaults to protocol constants)
    void setServerTimeoutUs(uint64_t timeout_us) { server_timeout_us_.store(timeout_us); }
    uint64_t serverTimeoutUs() const { return server_timeout_us_.load(); }
    void setPeerTimeoutUs(uint64_t timeout_us) { peer_timeout_us_.store(timeout_us); }
    uint64_t peerTimeoutUs() const { return peer_timeout_us_.load(); }
    void setDirectKeepAliveTimeoutUs(uint64_t timeout_us) { direct_keepalive_timeout_us_.store(timeout_us); }
    uint64_t directKeepAliveTimeoutUs() const { return direct_keepalive_timeout_us_.load(); }
    void setReconnectIntervalUs(uint64_t interval_us) { reconnect_interval_us_.store(interval_us); }
    uint64_t reconnectIntervalUs() const { return reconnect_interval_us_.load(); }
    void setHolePunchRetryIntervalUs(uint64_t interval_us) { hole_punch_retry_interval_us_.store(interval_us); }
    uint64_t holePunchRetryIntervalUs() const { return hole_punch_retry_interval_us_.load(); }

    // Test & diagnostic controls for P2P hole-punching and link impairment simulation
    void setDisableIpv6P2P(bool disable);
    bool isDisableIpv6P2P() const { return disable_ipv6_p2p_.load(); }
    void setSimulateHolePunchBlocked(bool blocked);
    bool isSimulateHolePunchBlocked() const { return simulate_punch_blocked_.load(); }
    void setSimulateSilentDrop(bool silent);
    bool isSimulateSilentDrop() const { return simulate_silent_drop_.load(); }
    void setSimulatePacketLossPercent(uint32_t loss_percent);
    uint32_t simulatePacketLossPercent() const { return simulate_loss_percent_.load(); }
    void setSimulateBandwidthFluctuation(bool enable);
    bool isSimulateBandwidthFluctuation() const { return simulate_bw_fluctuation_.load(); }

    // Injectable protocol timers so tests do not have to sleep for real seconds
    void setTunnelKeepAliveIntervalUs(uint64_t interval_us) { tunnel_keepalive_interval_us_.store(interval_us); }
    uint64_t tunnelKeepAliveIntervalUs() const { return tunnel_keepalive_interval_us_.load(); }
    void setHolePunchTimeoutUs(uint64_t timeout_us) { hole_punch_timeout_us_.store(timeout_us); }
    uint64_t holePunchTimeoutUs() const { return hole_punch_timeout_us_.load(); }

    // Injectable virtual-TCP tunnel liveness timers (idle keep-alive probe cadence and reap timeout)
    void setTcpTunnelKeepAliveIntervalUs(uint64_t interval_us) { tcp_tunnel_keepalive_interval_us_.store(interval_us); }
    uint64_t tcpTunnelKeepAliveIntervalUs() const { return tcp_tunnel_keepalive_interval_us_.load(); }
    void setTcpTunnelIdleTimeoutUs(uint64_t timeout_us) { tcp_tunnel_idle_timeout_us_.store(timeout_us); }
    uint64_t tcpTunnelIdleTimeoutUs() const { return tcp_tunnel_idle_timeout_us_.load(); }

    // Outgoing TCP-frame link impairment simulation (reorder / duplicate / fixed latency)
    void setSimulateReorderPercent(uint32_t reorder_percent);
    uint32_t simulateReorderPercent() const { return simulate_reorder_percent_.load(); }
    void setSimulateDuplicatePercent(uint32_t duplicate_percent);
    uint32_t simulateDuplicatePercent() const { return simulate_duplicate_percent_.load(); }
    void setSimulateLatencyUs(uint64_t latency_us) { simulate_latency_us_.store(latency_us); }
    uint64_t simulateLatencyUs() const { return simulate_latency_us_.load(); }

    // Force a single peer onto the Server Relay path while other peers keep Direct P2P
    void setForceRelayForPeer(uint32_t vip, bool force);
    bool isForceRelayForPeer(uint32_t vip) const;

    bool sendChatMessage(const std::string& text);

    // Launch a 32-bit or 64-bit executable inside this client's virtual network
    platform::LaunchedProcessInfo launchApp(
        const std::string& exe_path,
        const std::string& args = "",
        const std::string& working_dir = "");

    // Terminate a launched or hooked process by PID
    bool terminateLaunchedApp(uint32_t pid);

    // Remove exited processes from the launched process history
    void clearExitedProcesses();

    // Returns true if any launched application (or non-helper child process) is still running
    bool hasActiveLaunchedApps();

    // Thread-safe snapshot for GUI or automated test verification
    ClientStatusSnapshot getSnapshot() const;

    uint16_t ipcPort() const { return ipc_port_; }
    uint16_t udpGatewayPort() const { return udp_gw_port_; }
    uint16_t tcpGatewayPort() const { return tcp_gw_port_; }

private:
    struct BoundSocketEntry {
        IpcProto proto        = IpcProto::Udp;
        uint16_t virtual_port = 0;
        uint16_t actual_port  = 0;
        uint32_t pid          = 0;
    };

    struct ProcessTelemetryState {
        uint64_t tx_packets       = 0;
        uint64_t rx_packets       = 0;
        uint64_t tx_bytes         = 0;
        uint64_t rx_bytes         = 0;
        uint64_t last_activity_us = 0;
    };

    struct PeerState {
        uint32_t                        vip                    = 0;
        // Secret shared only with this peer (from PeerListUpdate). Direct and relayed
        // PunchPing/PunchPong/KeepAlive must carry it; only authenticated packets may set or
        // switch active_endpoint / best_v4_endpoint / best_v6_endpoint.
        uint64_t                        pair_key               = 0;
        std::string                     nickname;
        std::vector<platform::Endpoint> candidate_endpoints;
        RouteMode                       route_mode             = RouteMode::ServerRelay;
        platform::Endpoint              active_endpoint;
        platform::Endpoint              best_v4_endpoint;
        platform::Endpoint              best_v6_endpoint;
        uint32_t                        rtt_ms                 = 0;
        uint32_t                        rtt_v4_ms              = 0;
        uint32_t                        rtt_v6_ms              = 0;
        uint64_t                        last_direct_recv_us    = 0;
        uint64_t                        last_direct_v4_recv_us = 0;
        uint64_t                        last_direct_v6_recv_us = 0;
        uint64_t                        last_any_recv_us       = 0;
        uint64_t                        last_punch_sent_us     = 0;
        uint64_t                        last_tunnel_tx_us      = 0;
        uint64_t                        last_keepalive_sent_us = 0;
        uint64_t                        last_rtt_gui_update_us = 0;
        uint64_t                        punch_start_us         = 0;
        uint64_t                        punch_gave_up_us       = 0;
        uint64_t                        connected_since_us     = 0;
        int64_t                         connected_epoch_sec    = 0;
        bool                            punch_gave_up          = false;
        uint32_t                        hole_punch_cycles      = 1;
        uint64_t                        punch_probes_sent      = 0;
        uint64_t                        keepalive_tx_count     = 0;
        uint64_t                        keepalive_rx_count     = 0;
        uint64_t                        tx_packets             = 0;
        uint64_t                        rx_packets             = 0;
        uint64_t                        tx_bytes               = 0;
        uint64_t                        rx_bytes               = 0;
    };

    struct TcpUnackedSegment {
        uint32_t             seq_num       = 0;
        uint8_t              flags         = 0;
        std::vector<uint8_t> payload;
        uint64_t             first_sent_us = 0;
        uint64_t             last_sent_us  = 0;
        uint32_t             send_count    = 0;
        bool                 sacked        = false;
        bool                 fast_rexmit   = false;
    };

    struct TcpOutOfOrderSegment {
        std::vector<uint8_t> payload;
        uint8_t              flags = 0;
    };

    struct TcpTunnel {
        uint32_t                conn_id            = 0;
        uint32_t                pid                = 0;
        uint32_t                remote_vip         = 0;
        uint16_t                remote_vport       = 0;
        uint16_t                local_vport        = 0;
        uint16_t                expected_local_port= 0;
        uint16_t                bridge_local_port  = 0; // incoming: loopback port of our bridge socket
        platform::NetSocket     local_sock;
        bool                    handshake_done     = false;
        bool                    handshake_failed   = false;
        bool                    local_attached     = false;
        bool                    remote_fin         = false;
        bool                    local_fin          = false;
        bool                    local_shutdown_sent= false;
        bool                    ack_needed         = false;
        uint32_t                rx_since_last_ack  = 0;
        uint32_t                next_send_seq      = 1;
        uint32_t                next_expected_seq  = 1;
        uint32_t                last_rx_ack_num    = 1;
        uint32_t                dup_ack_count      = 0;

        // Incoming-bridge (app-side) non-blocking connect state machine
        bool                    bridge_connecting  = false;
        uint64_t                bridge_connect_start_us = 0;

        // Liveness: last frame received from the peer, and last idle keep-alive emitted
        uint64_t                last_rx_us         = 0;
        uint64_t                last_keepalive_sent_us = 0;

        // Outgoing tunnels: creation time, for the loopback-attach timeout
        uint64_t                created_us         = 0;

        // Adaptive congestion control (Slow Start + AIMD) & flow control
        double                  cwnd_segments      = 64.0;
        double                  ssthresh_segments  = 512.0;
        uint16_t                peer_recv_window   = static_cast<uint16_t>(kMaxTcpWindowSegments);
        uint16_t                last_advertised_rwnd = static_cast<uint16_t>(kMaxTcpWindowSegments);
        uint64_t                last_wnd_probe_us  = 0;
        uint64_t                last_cwnd_drop_us  = 0;
        uint32_t                consecutive_rto_count = 0;
        bool                    direct_data_acked  = false;

        // Dynamic RTT estimator (RFC 6298, in microseconds)
        uint64_t                srtt_us            = 0;
        uint64_t                rttvar_us          = 0;
        uint64_t                rto_us             = 200'000;

        ByteBuffer              to_local_buf;
        std::deque<TcpUnackedSegment> unacked_segments;
        std::map<uint32_t, TcpOutOfOrderSegment> out_of_order_segments;
        size_t                  out_of_order_bytes = 0; // payload bytes held in out_of_order_segments

        // Cached SACK bitmap (see buildSackBitmap()).
        uint64_t                sack_bitmap_cache = 0;
        bool                    sack_bitmap_dirty = false;

        // Incrementally maintained counts over unacked_segments so the per-tick
        // hot path never has to walk the whole deque (up to kMaxTcpWindowSegments
        // entries) to answer "how much is in flight / is there a SACK hole?".
        // Invariant: every mutation of unacked_segments must go through one of
        // the note* helpers below so these stay exact.
        size_t                  inflight_segments = 0; // entries with sacked == false
        size_t                  sacked_segments   = 0; // entries with sacked == true

        void noteSegmentQueued() { ++inflight_segments; }

        // Must be called while the segment is still at the front of the deque.
        void noteSegmentPoppedFront() {
            if (unacked_segments.empty()) return;
            if (unacked_segments.front().sacked) {
                if (sacked_segments > 0) --sacked_segments;
            } else if (inflight_segments > 0) {
                --inflight_segments;
            }
        }

        void noteSegmentSacked(TcpUnackedSegment& seg) {
            if (seg.sacked) return;
            seg.sacked = true;
            if (inflight_segments > 0) --inflight_segments;
            ++sacked_segments;
        }

        void noteSegmentErased(bool was_sacked) {
            if (was_sacked) {
                if (sacked_segments > 0) --sacked_segments;
            } else if (inflight_segments > 0) {
                --inflight_segments;
            }
        }

        size_t effectiveSendWindow() const {
            size_t cwnd = static_cast<size_t>(cwnd_segments);
            if (cwnd < 2) cwnd = 2;
            if (cwnd > kMaxTcpWindowSegments) cwnd = kMaxTcpWindowSegments;
            size_t rwnd = static_cast<size_t>(peer_recv_window);
            return (cwnd < rwnd) ? cwnd : rwnd;
        }

        size_t inFlightSegments() const {
            return inflight_segments;
        }

        bool hasSackHole() const {
            return sacked_segments > 0 || dup_ack_count >= 3;
        }

        uint16_t localRecvWindow() const {
            if (to_local_buf.size() >= kMaxTcpRecvBufBytes) return 0;
            size_t avail = kMaxTcpRecvBufBytes - to_local_buf.size();
            size_t segs = avail / kMaxTcpSegmentPayload;
            if (segs > kMaxTcpWindowSegments) segs = kMaxTcpWindowSegments;
            return static_cast<uint16_t>(segs);
        }

        uint64_t buildSackBitmap() {
            // Called once per outgoing segment; under loss/reordering the
            // out-of-order map can hold up to kMaxTcpWindowSegments entries, so
            // the bitmap is cached until the receive state actually changes.
            if (!sack_bitmap_dirty) {
                return sack_bitmap_cache;
            }
            uint64_t mask = 0;
            for (const auto& kv : out_of_order_segments) {
                if (seqGreater(kv.first, next_expected_seq)) {
                    uint32_t bit = kv.first - next_expected_seq - 1;
                    if (bit < 64) {
                        mask |= (1ULL << bit);
                    }
                }
            }
            sack_bitmap_cache = mask;
            sack_bitmap_dirty = false;
            return mask;
        }

        // Must be called whenever next_expected_seq or out_of_order_segments change.
        void invalidateSackBitmap() { sack_bitmap_dirty = true; }

        void updateRtt(uint64_t sample_us) {
            if (sample_us == 0) sample_us = 1;
            if (srtt_us == 0) {
                srtt_us = sample_us;
                rttvar_us = sample_us / 2;
            } else {
                uint64_t diff = (srtt_us > sample_us) ? (srtt_us - sample_us) : (sample_us - srtt_us);
                rttvar_us = (3 * rttvar_us + diff) / 4;
                srtt_us = (7 * srtt_us + sample_us) / 8;
            }
            uint64_t var_floor = (srtt_us / 3 > 15'000ULL) ? (srtt_us / 3) : 15'000ULL;
            uint64_t var_term = (4 * rttvar_us > var_floor) ? (4 * rttvar_us) : var_floor;
            uint64_t next_rto = srtt_us + var_term;
            if (next_rto < 20'000ULL) next_rto = 20'000ULL;
            if (next_rto > 2'000'000ULL) next_rto = 2'000'000ULL;
            rto_us = next_rto;
        }
    };

    void networkWorkerLoop();
    void ipcWorkerLoop();

    // IPC virtual-TCP connect handshakes can block for seconds; run them on a
    // small worker pool so one slow connect cannot stall every other hooked
    // process's bind/connect/query request on the single IPC thread.
    struct IpcPendingConnect {
        platform::NetSocket sock;
        IpcRequest          req;
    };
    void startIpcConnectWorkers();
    void stopIpcConnectWorkers();
    void ipcConnectWorkerLoop();
    void handleIpcRemoteTcpConnect(IpcPendingConnect job);

    // Server TCP control channel handlers
    void handleServerTcpReadable();
    void processServerControlMsg(ControlMsgType type, const uint8_t* payload, size_t len);
    bool queueServerControlMsg(ControlMsgType type, const void* payload, size_t len);
    void handleServerConnectionLost(uint64_t now_us);
    void buildAndQueueClientHello();
    void setServerResolvedEndpoints(const platform::Endpoint& connected_ep,
                                    const std::vector<platform::Endpoint>& resolved, uint16_t port);

    // Automatic reconnect: DNS + TCP connect run on a short-lived helper thread so the network
    // thread keeps serving retained direct P2P sessions; the result is adopted on the next tick.
    void attemptServerReconnect(uint64_t now_us);
    void serverReconnectWorker(std::string host, uint16_t port, uint64_t generation);
    void finishServerReconnect(uint64_t now_us);

    // On server loss: drop peers (and their tunnels) that are not on a verified direct route.
    void dropRelayOnlyPeers();

    // Sockets polled by the network thread are never closed from another thread: they are
    // parked here and closed by the network thread between polls (or by stop() after the join).
    void retireSocket(platform::NetSocket& sock);
    void closeRetiredSockets();

    // P2P UDP transport handlers
    void handleUdpTransportReadable(platform::NetSocket& sock);
    void onDirectPacketReceived(PeerState& peer, const platform::Endpoint& src_ep, uint64_t now_us, const uint32_t* rtt_ms);
    void sendDirectUdp(const platform::Endpoint& dst, const void* data, size_t len);
    void sendServerUdpRegister(uint32_t vip, uint64_t token);
    void processIncomingVpnFrame(uint32_t verified_src_vip, UdpPacketType type, const uint8_t* data, size_t len, bool from_direct_udp, const platform::Endpoint& udp_src);
    void startSynchronizedHolePunch(PeerState& peer, uint64_t now_us, bool send_relay_sync);
    void sendHolePunchProbes(uint64_t now_us);

    // Peer-pair authentication of PunchPing/PunchPong/KeepAlive (see PeerState::pair_key)
    UdpPunchPacket makePunchPacket(UdpPacketType type, const PeerState& peer, uint64_t timestamp_us) const;
    PeerState* authenticatePunchPacket(const UdpPunchPacket& pkt);
    bool isVerifiedDirectSource(const PeerState& peer, const platform::Endpoint& src, uint64_t now_us) const;
    void resetPeerDirectRoute(PeerState& peer);

    // Local Hook Gateways (UDP & TCP)
    void handleLocalUdpGatewayReadable();
    void deliverUdpToLocalSockets(uint32_t src_vip, uint32_t dst_vip, uint16_t src_vport, uint16_t dst_vport, const uint8_t* payload, size_t len);
    void handleLocalTcpGatewayAccept();
    void pumpTcpTunnels(uint64_t now_us);
    void flushPendingTcpAcks();
    void handleIncomingVpnTcpFrame(const VpnTcpFrameHeader& hdr, const uint8_t* payload, uint64_t now_us);
    void sendTcpFrame(TcpTunnel& tunnel, uint8_t flags, uint32_t seq, const void* payload, uint16_t len, bool via_relay_fallback = false);
    // Abort a tunnel: optional RST to the peer, local socket retired, buffers released and
    // handshake_failed set so pumpTcpTunnels() reaps it. Safe to call more than once.
    void abortTcpTunnel(TcpTunnel& tunnel, bool send_rst);
    void abortTcpTunnelsToPeer(uint32_t vip, bool send_rst);
    bool drainOutOfOrderSegments(TcpTunnel& tunnel, uint64_t now_us);
    uint32_t allocateConnId();

    // Outbound VPN frame routing (Direct UDP P2P or Server Relay)
    void sendVpnFrameToTarget(uint32_t dst_vip, UdpPacketType inner_type, const void* data, size_t len, bool force_relay_override = false);
    void ensureProcessTrackedLocked(uint32_t pid);

    // Outgoing frame emission with optional link impairment (latency/reorder/duplicate)
    void emitFrameToPeer(PeerState& peer, UdpPacketType inner_type, const void* data, size_t len, bool force_relay_override);
    void emitRelayDirect(uint32_t dst_vip, UdpPacketType inner_type, const void* data, size_t len);
    void enqueueImpairedFrame(uint32_t dst_vip, UdpPacketType inner_type, const void* data, size_t len, bool force_relay_override);
    void flushPendingOutFrames(uint64_t now_us);
    bool impairmentActive() const;
    uint32_t nextSimRand();

    std::atomic<bool> gateways_running_{false};
    std::atomic<bool> connected_{false};
    std::atomic<bool> want_connected_{false};
    std::atomic<bool> reconnecting_{false};
    std::atomic<uint32_t> reconnect_attempts_{0};
    std::atomic<bool> force_relay_{false};
    std::atomic<bool> disable_ipv6_p2p_{false};
    std::atomic<bool> simulate_punch_blocked_{false};
    std::atomic<bool> simulate_silent_drop_{false};
    std::atomic<uint32_t> simulate_loss_percent_{0};
    std::atomic<bool> simulate_bw_fluctuation_{false};
    std::atomic<uint32_t> local_vip_{0};
    std::atomic<uint64_t> server_timeout_us_{kServerPeerTimeoutUs};
    std::atomic<uint64_t> peer_timeout_us_{kServerPeerTimeoutUs};
    std::atomic<uint64_t> direct_keepalive_timeout_us_{kTunnelKeepAliveTimeoutUs};
    std::atomic<uint64_t> reconnect_interval_us_{kServerReconnectIntervalUs};
    std::atomic<uint64_t> hole_punch_retry_interval_us_{kHolePunchRetryIntervalUs};
    std::atomic<uint64_t> tunnel_keepalive_interval_us_{kTunnelKeepAliveIntervalUs};
    std::atomic<uint64_t> hole_punch_timeout_us_{kHolePunchTimeoutUs};
    std::atomic<uint64_t> tcp_tunnel_keepalive_interval_us_{kTcpTunnelKeepAliveIntervalUs};
    std::atomic<uint64_t> tcp_tunnel_idle_timeout_us_{kTcpTunnelIdleTimeoutUs};
    std::atomic<uint32_t> simulate_reorder_percent_{0};
    std::atomic<uint32_t> simulate_duplicate_percent_{0};
    std::atomic<uint64_t> simulate_latency_us_{0};

    std::string server_ip_str_;
    platform::Endpoint server_endpoint_;
    std::vector<platform::Endpoint> server_resolved_endpoints_;
    uint16_t    server_tcp_port_ = 0;
    uint16_t    server_udp_port_ = 0;
    std::string nickname_;
    uint64_t    session_token_   = 0;
    uint32_t    preferred_vip_   = 0;
    uint64_t    join_time_us_    = 0;

    // Sockets
    platform::NetSocket ipc_listener_;
    platform::NetSocket udp_gateway_sock_;
    platform::NetSocket tcp_gateway_listener_;
    platform::NetSocket udp_transport_sock_v6_;
    platform::NetSocket udp_transport_sock_;
    platform::NetSocket server_tcp_sock_;

    uint16_t ipc_port_              = 0;
    uint16_t udp_gw_port_           = 0;
    uint16_t tcp_gw_port_           = 0;
    uint16_t udp_transport_port_v6_ = 0;
    uint16_t udp_transport_port_    = 0;

    std::thread network_thread_;
    std::thread ipc_thread_;

    // Reconnect helper thread and its hand-off (guarded by mutex_). disconnect() bumps the
    // generation so a result from an attempt that predates it is discarded.
    std::thread                     reconnect_thread_;
    bool                            reconnect_in_flight_         = false;
    bool                            reconnect_result_ready_      = false;
    uint64_t                        reconnect_generation_        = 0;
    uint64_t                        reconnect_result_generation_ = 0;
    uint16_t                        reconnect_result_port_       = 0;
    platform::NetSocket             reconnect_result_sock_;
    platform::Endpoint              reconnect_result_ep_;
    std::vector<platform::Endpoint> reconnect_result_resolved_;

    // See retireSocket() (guarded by mutex_)
    std::vector<platform::NetSocket> retired_sockets_;

    // Remote virtual-TCP connect jobs handed off by the IPC thread.
    std::mutex                    ipc_queue_mutex_;
    std::condition_variable       ipc_queue_cv_;
    std::deque<IpcPendingConnect> ipc_connect_queue_;
    bool                          ipc_connect_stopping_ = false;
    std::vector<std::thread>      ipc_connect_workers_;

    mutable std::mutex mutex_;
    std::condition_variable cv_;

    ByteBuffer server_recv_buf_;
    ByteBuffer server_send_buf_;
    uint64_t last_udp_reg_sent_us_      = 0;
    uint64_t last_server_recv_us_       = 0;
    uint64_t last_server_ping_sent_us_  = 0;
    uint64_t last_reconnect_attempt_us_ = 0;
    uint64_t last_pid_cleanup_us_       = 0;

    std::vector<BoundSocketEntry> bound_sockets_;
    std::unordered_map<uint32_t, PeerState> peers_;
    std::unordered_map<uint32_t, std::unique_ptr<TcpTunnel>> tcp_tunnels_;

    struct PendingOutFrame {
        uint32_t             dst_vip     = 0;
        UdpPacketType        type        = UdpPacketType::VpnTcpFrame;
        bool                 force_relay = false;
        std::vector<uint8_t> bytes;
        uint64_t             due_us      = 0;
    };
    std::unordered_set<uint32_t> force_relay_peers_;
    std::vector<PendingOutFrame> pending_out_frames_;
    // Maps bridge_local_port -> (remote_vip, remote_vport) for accepted TCP connections
    std::unordered_map<uint16_t, std::pair<uint32_t, uint16_t>> incoming_tcp_peer_map_;

    std::vector<ChatHistoryEntry> chat_history_;
    std::vector<platform::LaunchedProcessInfo> launched_processes_;
    std::unordered_map<uint32_t, ProcessTelemetryState> proc_telemetry_;
    uint64_t total_tx_packets_       = 0;
    uint64_t total_rx_packets_       = 0;
    uint64_t tcp_simulated_drops_    = 0;
    uint64_t tcp_fast_retransmits_   = 0;
    uint64_t tcp_rto_retransmits_    = 0;
    uint64_t tcp_sack_events_        = 0;
    uint64_t tcp_cwnd_scale_ups_     = 0;
    uint64_t tcp_cwnd_scale_downs_   = 0;
    uint32_t sim_prng_state_         = 0xA341316Cu;
    uint64_t bw_sim_start_us_        = 0;
    uint64_t bw_last_refill_us_      = 0;
    double   bw_tokens_bytes_        = 65536.0;
    uint32_t bw_prev_phase_          = 0;
    uint32_t bw_tail_drop_budget_    = 0;
};

} // namespace apivpn::client
