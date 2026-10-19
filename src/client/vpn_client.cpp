#include "apivpn/client/vpn_client.hpp"
#include "apivpn/common/logger.hpp"

#include <algorithm>
#include <chrono>
#include <cstring>
#include <memory>
#include <thread>
#include <vector>

// Purpose: VpnClient lifecycle, configuration, and control-channel helpers.

namespace apivpn::client {

VpnClient::VpnClient() = default;

VpnClient::~VpnClient() {
    stop();
}

bool VpnClient::initLocalGateways() {
    if (gateways_running_.load()) return true;

    ipc_listener_ = platform::NetSocket::createTcp(platform::IpVersion::V4);
    if (!ipc_listener_.isValid() ||
        !ipc_listener_.bind(kLoopbackIp, 0) ||
        !ipc_listener_.listen(64)) {
        return false;
    }
    ipc_listener_.setNonBlocking(true);
    platform::Endpoint ep{};
    ipc_listener_.getLocalEndpoint(ep);
    ipc_port_ = ep.port_host;

    udp_gateway_sock_ = platform::NetSocket::createUdp(platform::IpVersion::V4);
    if (!udp_gateway_sock_.isValid() || !udp_gateway_sock_.bind(kLoopbackIp, 0)) {
        return false;
    }
    udp_gateway_sock_.setNonBlocking(true);
    udp_gateway_sock_.setRecvBufferSize(4 * 1024 * 1024);
    udp_gateway_sock_.setSendBufferSize(4 * 1024 * 1024);
    udp_gateway_sock_.getLocalEndpoint(ep);
    udp_gw_port_ = ep.port_host;

    tcp_gateway_listener_ = platform::NetSocket::createTcp(platform::IpVersion::V4);
    if (!tcp_gateway_listener_.isValid() ||
        !tcp_gateway_listener_.bind(kLoopbackIp, 0) ||
        !tcp_gateway_listener_.listen(64)) {
        return false;
    }
    tcp_gateway_listener_.setNonBlocking(true);
    tcp_gateway_listener_.getLocalEndpoint(ep);
    tcp_gw_port_ = ep.port_host;

    udp_transport_sock_ = platform::NetSocket::createUdp(platform::IpVersion::V4);
    if (!udp_transport_sock_.isValid() || !udp_transport_sock_.bind(0, 0)) {
        return false;
    }
    udp_transport_sock_.setNonBlocking(true);
    udp_transport_sock_.setRecvBufferSize(4 * 1024 * 1024);
    udp_transport_sock_.setSendBufferSize(4 * 1024 * 1024);
    udp_transport_sock_.getLocalEndpoint(ep);
    udp_transport_port_ = ep.port_host;

    udp_transport_sock_v6_ = platform::NetSocket::createUdp(platform::IpVersion::V6);
    if (udp_transport_sock_v6_.isValid()) {
        if (!udp_transport_sock_v6_.bind(platform::Endpoint::anyV6(udp_transport_port_))) {
            if (!udp_transport_sock_v6_.bind(platform::Endpoint::anyV6(0))) {
                udp_transport_sock_v6_.close();
            }
        }
        if (udp_transport_sock_v6_.isValid()) {
            udp_transport_sock_v6_.setNonBlocking(true);
            udp_transport_sock_v6_.setRecvBufferSize(4 * 1024 * 1024);
            udp_transport_sock_v6_.setSendBufferSize(4 * 1024 * 1024);
            platform::Endpoint ep6{};
            if (udp_transport_sock_v6_.getLocalEndpoint(ep6)) {
                udp_transport_port_v6_ = ep6.port_host;
            }
        }
    }

    gateways_running_.store(true);
    startIpcConnectWorkers();
    ipc_thread_ = std::thread(&VpnClient::ipcWorkerLoop, this);
    network_thread_ = std::thread(&VpnClient::networkWorkerLoop, this);

    logInfo("Local gateways initialized (IPC:" + std::to_string(ipc_port_) +
            " UDP-GW:" + std::to_string(udp_gw_port_) +
            " TCP-GW:" + std::to_string(tcp_gw_port_) +
            " P2P-UDP4:" + std::to_string(udp_transport_port_) +
            " P2P-UDP6:" + std::to_string(udp_transport_port_v6_) + ")");
    return true;
}

void VpnClient::buildAndQueueClientHello() {
    MsgClientHello hello{};
    std::strncpy(hello.nickname, nickname_.c_str(), kMaxNicknameLen - 1);
    hello.udp_listen_port = udp_transport_port_;
    hello.udp_listen_port_v6 = udp_transport_port_v6_;
    hello.preferred_vip = preferred_vip_;
    hello.prev_session_token = session_token_;

    std::vector<platform::Endpoint> local_eps;
    if (udp_transport_sock_v6_.isValid() && udp_transport_port_v6_ != 0) {
        auto local_v6 = platform::getLocalIpv6Addresses();
        for (const auto& v6 : local_v6) {
            if (local_eps.size() < kMaxEndpointsPerPeer) {
                local_eps.push_back(platform::Endpoint::fromIpv6(v6, udp_transport_port_v6_));
            }
        }
    }
    if (udp_transport_sock_.isValid() && udp_transport_port_ != 0) {
        auto local_v4 = platform::getLocalIpv4Addresses();
        for (uint32_t v4 : local_v4) {
            if (local_eps.size() < kMaxEndpointsPerPeer) {
                local_eps.push_back(platform::Endpoint::fromIpv4(v4, udp_transport_port_));
            }
        }
    }

    hello.local_ep_count = static_cast<uint8_t>(std::min(local_eps.size(), kMaxEndpointsPerPeer));
    for (uint8_t i = 0; i < hello.local_ep_count; ++i) {
        hello.local_endpoints[i] = platform::toEndpointWire(local_eps[i]);
    }
    queueServerControlMsg(ControlMsgType::ClientHello, &hello, sizeof(hello));
}

bool VpnClient::connectToServer(const std::string& server_ip, uint16_t server_port, const std::string& nickname) {
    if (!initLocalGateways()) return false;
    disconnect();

    std::string clean_host;
    uint16_t effective_port = (server_port != 0) ? server_port : kDefaultServerPort;
    if (!platform::splitHostPort(server_ip, clean_host, effective_port)) {
        logError("Invalid server host/IP: " + server_ip);
        return false;
    }

    // Snapshot of the disconnect generation: a disconnect() (or another connect) from another
    // thread while DNS / the TCP handshake run unlocked below bumps it, and this attempt then
    // gives up instead of re-enabling the connection the user just dropped.
    uint64_t generation = 0;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        server_ip_str_ = clean_host;
        server_tcp_port_ = effective_port;
        server_udp_port_ = effective_port;
        nickname_ = nickname.empty() ? "Player" : nickname;
        reconnect_attempts_.store(0);
        generation = reconnect_generation_;
    }

    auto resolved = platform::resolveHost(clean_host, effective_port);
    if (resolved.empty()) {
        logError("Failed to resolve server host/IP: " + server_ip + " (will retry periodically)");
        std::lock_guard<std::mutex> lock(mutex_);
        if (reconnect_generation_ != generation) return false;
        want_connected_.store(true);
        reconnecting_.store(true);
        last_reconnect_attempt_us_ = platform::monotonicMicros();
        return false;
    }

    // Try resolved endpoints in order (resolveHost places all IPv6 endpoints first, then IPv4)
    platform::NetSocket tcp;
    platform::Endpoint connected_ep{};
    for (const auto& candidate : resolved) {
        platform::NetSocket attempt_sock = platform::NetSocket::createTcp(candidate.version);
        if (!attempt_sock.isValid()) continue;
        if (attempt_sock.connect(candidate, 3000)) {
            tcp = std::move(attempt_sock);
            connected_ep = candidate;
            break;
        }
    }

    if (!tcp.isValid()) {
        logError("Failed to connect to server " + clean_host + ":" + std::to_string(effective_port) +
                 " (will retry periodically)");
        std::lock_guard<std::mutex> lock(mutex_);
        if (reconnect_generation_ != generation) return false;
        want_connected_.store(true);
        reconnecting_.store(true);
        last_reconnect_attempt_us_ = platform::monotonicMicros();
        return false;
    }
    tcp.setNoDelay(true);
    tcp.setNonBlocking(true);
    tcp.setRecvBufferSize(4 * 1024 * 1024);
    tcp.setSendBufferSize(4 * 1024 * 1024);

    {
        std::unique_lock<std::mutex> lock(mutex_);
        if (reconnect_generation_ != generation) return false; // `tcp` was never polled: closed here
        uint64_t now_us = platform::monotonicMicros();
        server_endpoint_ = connected_ep;
        retireSocket(server_tcp_sock_);
        server_tcp_sock_ = std::move(tcp);
        server_recv_buf_.clear();
        server_send_buf_.clear();
        peers_.clear();
        local_vip_.store(0);
        join_time_us_ = now_us;
        last_server_recv_us_ = now_us;
        last_server_ping_sent_us_ = now_us;
        last_udp_reg_sent_us_ = 0;
        want_connected_.store(true);
        connected_.store(true);
        reconnecting_.store(false);

        setServerResolvedEndpoints(connected_ep, resolved, effective_port);
        buildAndQueueClientHello();
    }

    // Wait up to 3 seconds for ServerWelcome assigning our Virtual IP
    std::unique_lock<std::mutex> lock(mutex_);
    bool welcomed = cv_.wait_for(lock, std::chrono::milliseconds(3000), [this, generation]() {
        return local_vip_.load() != 0 || !connected_.load() || reconnect_generation_ != generation;
    });

    if (reconnect_generation_ != generation) return false; // superseded by disconnect()/stop()
    if (!welcomed || local_vip_.load() == 0) {
        handleServerConnectionLost(platform::monotonicMicros());
        return false;
    }

    return true;
}

void VpnClient::setServerResolvedEndpoints(
    const platform::Endpoint& connected_ep,
    const std::vector<platform::Endpoint>& resolved,
    uint16_t port) {

    server_resolved_endpoints_.clear();
    auto addResolved = [&](const platform::Endpoint& ep) {
        if (!ep.isValidAddress()) return;
        for (const auto& existing : server_resolved_endpoints_) {
            if (existing.version == ep.version &&
                ((ep.isIpv6() && existing.ipv6 == ep.ipv6) ||
                 (ep.isIpv4() && existing.ip_host == ep.ip_host))) {
                return;
            }
        }
        server_resolved_endpoints_.push_back(ep);
    };
    addResolved(connected_ep);
    for (const auto& ep : resolved) {
        addResolved(ep);
    }
    if (connected_ep.isLoopback()) {
        addResolved(platform::Endpoint::loopbackV6(port));
        addResolved(platform::Endpoint::loopbackV4(port));
    }
}

void VpnClient::retireSocket(platform::NetSocket& sock) {
    if (sock.isValid()) {
        retired_sockets_.push_back(std::move(sock));
    }
}

void VpnClient::closeRetiredSockets() {
    retired_sockets_.clear(); // NetSocket destructors close the handles
}

void VpnClient::dropRelayOnlyPeers() {
    const bool global_relay = force_relay_.load();
    for (auto it = peers_.begin(); it != peers_.end(); ) {
        const PeerState& p = it->second;
        const bool direct = !global_relay && force_relay_peers_.count(p.vip) == 0 &&
                            p.route_mode == RouteMode::DirectP2P && p.active_endpoint.port_host != 0;
        if (direct) {
            ++it;
        } else {
            it = peers_.erase(it);
        }
    }
    // Tunnels whose peer is gone have no path left (the relay is down, so no RST either).
    for (auto& kv : tcp_tunnels_) {
        if (peers_.find(kv.second->remote_vip) == peers_.end()) {
            abortTcpTunnel(*kv.second, false);
        }
    }
    // With no direct session left there is nothing our VIP is still valid for until the
    // server assigns one again.
    if (peers_.empty()) {
        local_vip_.store(0);
    }
}

void VpnClient::handleServerConnectionLost(uint64_t now_us) {
    bool was_connected = connected_.exchange(false);
    retireSocket(server_tcp_sock_);
    server_recv_buf_.clear();
    server_send_buf_.clear();
    // Peers on a verified direct route (and their tunnels) do not need the server: keep them,
    // with our VIP, so a coordination-server outage does not kill running game sessions.
    // ServerWelcome on reconnect resets everything if the VIP changes.
    dropRelayOnlyPeers();
    if (want_connected_.load()) {
        reconnecting_.store(true);
        last_reconnect_attempt_us_ = now_us;
        if (was_connected) {
            logWarn("Lost connection to VPN Server; will re-attempt connection periodically");
        }
    } else {
        reconnecting_.store(false);
    }
    cv_.notify_all();
}

void VpnClient::attemptServerReconnect(uint64_t now_us) {
    if (!want_connected_.load() || connected_.load() || simulate_silent_drop_.load()) return;
    if (reconnect_in_flight_ || reconnect_result_ready_) return;
    uint64_t interval_us = reconnect_interval_us_.load();
    if (last_reconnect_attempt_us_ != 0 &&
        now_us >= last_reconnect_attempt_us_ &&
        (now_us - last_reconnect_attempt_us_) < interval_us) {
        return;
    }

    last_reconnect_attempt_us_ = now_us;
    uint32_t attempt_num = ++reconnect_attempts_;
    reconnecting_.store(true);

    std::string host = server_ip_str_;
    uint16_t port = server_tcp_port_;
    if (host.empty() || port == 0) return;

    logInfo("Re-attempting connection to VPN Server " + host + ":" + std::to_string(port) +
            " (attempt #" + std::to_string(attempt_num) + ")...");

    // DNS and the TCP handshake can block for seconds; run them on a helper thread so the
    // network thread keeps pumping retained direct P2P sessions meanwhile. The previous helper
    // has already handed off its result (reconnect_in_flight_ is false), so this join only
    // reaps an exited thread and never waits on mutex_.
    if (reconnect_thread_.joinable()) {
        reconnect_thread_.join();
    }
    reconnect_in_flight_ = true;
    reconnect_thread_ = std::thread(&VpnClient::serverReconnectWorker, this,
                                    std::move(host), port, reconnect_generation_);
}

void VpnClient::serverReconnectWorker(std::string host, uint16_t port, uint64_t generation) {
    std::vector<platform::Endpoint> resolved = platform::resolveHost(host, port);
    platform::NetSocket tcp;
    platform::Endpoint connected_ep{};
    for (const auto& candidate : resolved) {
        if (!want_connected_.load() || !gateways_running_.load() || simulate_silent_drop_.load()) break;
        platform::NetSocket attempt_sock = platform::NetSocket::createTcp(candidate.version);
        if (!attempt_sock.isValid()) continue;
        if (attempt_sock.connect(candidate, 1000)) {
            tcp = std::move(attempt_sock);
            connected_ep = candidate;
            break;
        }
    }

    std::lock_guard<std::mutex> lock(mutex_);
    reconnect_result_sock_ = std::move(tcp);
    reconnect_result_ep_ = connected_ep;
    reconnect_result_resolved_ = std::move(resolved);
    reconnect_result_port_ = port;
    reconnect_result_generation_ = generation;
    reconnect_result_ready_ = true;
    reconnect_in_flight_ = false;
}

void VpnClient::finishServerReconnect(uint64_t now_us) {
    reconnect_result_ready_ = false;
    platform::NetSocket tcp = std::move(reconnect_result_sock_);
    std::vector<platform::Endpoint> resolved = std::move(reconnect_result_resolved_);
    reconnect_result_resolved_.clear();
    const platform::Endpoint connected_ep = reconnect_result_ep_;
    const uint16_t port = reconnect_result_port_;
    last_reconnect_attempt_us_ = now_us;

    // `tcp` was never polled, so closing it here (on scope exit) is safe.
    if (reconnect_result_generation_ != reconnect_generation_ ||
        !want_connected_.load() || !gateways_running_.load() || connected_.load() ||
        simulate_silent_drop_.load() || !tcp.isValid()) {
        return;
    }

    tcp.setNoDelay(true);
    tcp.setNonBlocking(true);
    tcp.setRecvBufferSize(4 * 1024 * 1024);
    tcp.setSendBufferSize(4 * 1024 * 1024);

    server_endpoint_ = connected_ep;
    server_udp_port_ = port;
    retireSocket(server_tcp_sock_);
    server_tcp_sock_ = std::move(tcp);
    server_recv_buf_.clear();
    server_send_buf_.clear();
    // Retained direct peers and local_vip_ are kept; ServerWelcome resets them if the server
    // hands out a different VIP, and PeerListUpdate if a peer's pair key changed.
    join_time_us_ = now_us;
    last_server_recv_us_ = now_us;
    last_server_ping_sent_us_ = now_us;
    last_udp_reg_sent_us_ = 0;
    connected_.store(true);
    reconnecting_.store(false);

    setServerResolvedEndpoints(connected_ep, resolved, port);
    buildAndQueueClientHello();
    logInfo("TCP connection to VPN Server re-established; awaiting ServerWelcome...");
}

void VpnClient::disconnect() {
    std::lock_guard<std::mutex> lock(mutex_);
    // Peers learn about it from the server's next peer list (or their idle timeout).
    for (auto& kv : tcp_tunnels_) {
        abortTcpTunnel(*kv.second, false);
    }
    want_connected_.store(false);
    reconnecting_.store(false);
    connected_.store(false);
    reconnect_attempts_.store(0);
    ++reconnect_generation_;
    local_vip_.store(0);
    preferred_vip_ = 0;
    session_token_ = 0;
    // Never close a socket the network thread may be polling right now (see retireSocket()).
    retireSocket(server_tcp_sock_);
    server_endpoint_ = {};
    server_resolved_endpoints_.clear();
    server_recv_buf_.clear();
    server_send_buf_.clear();
    peers_.clear();
    tcp_tunnels_.clear(); // local sockets were retired by abortTcpTunnel()
    incoming_tcp_peer_map_.clear();
    pending_out_frames_.clear();
    force_relay_peers_.clear();
    cv_.notify_all();
}

void VpnClient::stop() {
    disconnect();
    {
        std::lock_guard<std::mutex> lock(mutex_);
        for (const auto& pr : launched_processes_) {
            if (pr.pid != 0 &&
                platform::ProcessLauncher::isBackgroundHelperExecutable(pr.exe_path) &&
                platform::ProcessLauncher::isProcessAlive(pr.pid)) {
                platform::ProcessLauncher::terminateProcess(pr.pid);
            }
        }
    }
    if (gateways_running_.exchange(false)) {
        cv_.notify_all();
        if (ipc_thread_.joinable()) ipc_thread_.join();
        // No new connect jobs can be queued once the IPC thread is joined.
        stopIpcConnectWorkers();
        if (network_thread_.joinable()) network_thread_.join();
        // The network thread is the only spawner of reconnect helpers; this helper sees
        // gateways_running_ == false and gives up after its current connect attempt.
        if (reconnect_thread_.joinable()) reconnect_thread_.join();
        {
            std::lock_guard<std::mutex> lock(mutex_);
            reconnect_in_flight_ = false;
            reconnect_result_ready_ = false;
            reconnect_result_sock_.close();
            closeRetiredSockets(); // no thread polls any socket any more
        }
        ipc_listener_.close();
        udp_gateway_sock_.close();
        tcp_gateway_listener_.close();
        udp_transport_sock_v6_.close();
        udp_transport_sock_.close();
    }
}

void VpnClient::setForceRelay(bool force) {
    std::lock_guard<std::mutex> lock(mutex_);
    force_relay_.store(force);
    uint64_t now_us = platform::monotonicMicros();
    if (force) {
        for (auto& kv : peers_) {
            kv.second.route_mode = RouteMode::ServerRelay;
            kv.second.last_keepalive_sent_us = now_us;
            // Relayed keep-alive to refresh the relay RTT (dropped if the relay is down or full)
            UdpPunchPacket ka = makePunchPacket(UdpPacketType::KeepAlive, kv.second, now_us);
            emitRelayDirect(kv.second.vip, UdpPacketType::KeepAlive, &ka, sizeof(ka));
        }
    } else {
        for (auto& kv : peers_) {
            startSynchronizedHolePunch(kv.second, now_us, true);
        }
    }
}

void VpnClient::setDisableIpv6P2P(bool disable) {
    std::lock_guard<std::mutex> lock(mutex_);
    disable_ipv6_p2p_.store(disable);
    if (disable) {
        for (auto& kv : peers_) {
            if (kv.second.active_endpoint.isIpv6()) {
                if (kv.second.best_v4_endpoint.port_host != 0) {
                    kv.second.active_endpoint = kv.second.best_v4_endpoint;
                    kv.second.rtt_ms = kv.second.rtt_v4_ms;
                } else {
                    kv.second.route_mode = RouteMode::ServerRelay;
                    kv.second.active_endpoint = {};
                }
            }
        }
    } else {
        for (auto& kv : peers_) {
            kv.second.last_punch_sent_us = 0;
        }
    }
}

void VpnClient::setSimulateHolePunchBlocked(bool blocked) {
    std::lock_guard<std::mutex> lock(mutex_);
    simulate_punch_blocked_.store(blocked);
}

void VpnClient::setSimulateSilentDrop(bool silent) {
    std::lock_guard<std::mutex> lock(mutex_);
    bool was_silent = simulate_silent_drop_.exchange(silent);
    if (was_silent && !silent) {
        last_reconnect_attempt_us_ = 0;
    }
}

void VpnClient::setSimulatePacketLossPercent(uint32_t loss_percent) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (loss_percent > 90) loss_percent = 90;
    simulate_loss_percent_.store(loss_percent);
    sim_prng_state_ = 0xA341316Cu;
    tcp_simulated_drops_ = 0;
    tcp_fast_retransmits_ = 0;
    tcp_rto_retransmits_ = 0;
    tcp_sack_events_ = 0;
    tcp_cwnd_scale_ups_ = 0;
    tcp_cwnd_scale_downs_ = 0;
}

void VpnClient::setSimulateBandwidthFluctuation(bool enable) {
    std::lock_guard<std::mutex> lock(mutex_);
    simulate_bw_fluctuation_.store(enable);
    uint64_t now_us = platform::monotonicMicros();
    bw_sim_start_us_ = now_us;
    bw_last_refill_us_ = now_us;
    bw_tokens_bytes_ = 32768.0;
    bw_prev_phase_ = 0;
    bw_tail_drop_budget_ = 0;
    if (enable) {
        tcp_simulated_drops_ = 0;
        tcp_fast_retransmits_ = 0;
        tcp_rto_retransmits_ = 0;
        tcp_sack_events_ = 0;
        tcp_cwnd_scale_ups_ = 0;
        tcp_cwnd_scale_downs_ = 0;
    }
}

void VpnClient::setSimulateReorderPercent(uint32_t reorder_percent) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (reorder_percent > 100) reorder_percent = 100;
    simulate_reorder_percent_.store(reorder_percent);
}

void VpnClient::setSimulateDuplicatePercent(uint32_t duplicate_percent) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (duplicate_percent > 100) duplicate_percent = 100;
    simulate_duplicate_percent_.store(duplicate_percent);
}

void VpnClient::setForceRelayForPeer(uint32_t vip, bool force) {
    std::lock_guard<std::mutex> lock(mutex_);
    uint64_t now_us = platform::monotonicMicros();
    if (force) {
        force_relay_peers_.insert(vip);
        auto it = peers_.find(vip);
        if (it != peers_.end()) {
            it->second.route_mode = RouteMode::ServerRelay;
            it->second.active_endpoint = {};
            it->second.punch_gave_up = true;
            it->second.punch_gave_up_us = now_us;
            it->second.last_keepalive_sent_us = now_us;
        }
    } else {
        force_relay_peers_.erase(vip);
        auto it = peers_.find(vip);
        if (it != peers_.end()) {
            startSynchronizedHolePunch(it->second, now_us, true);
        }
    }
}

bool VpnClient::isForceRelayForPeer(uint32_t vip) const {
    std::lock_guard<std::mutex> lock(mutex_);
    return force_relay_peers_.find(vip) != force_relay_peers_.end();
}

bool VpnClient::sendChatMessage(const std::string& text) {
    if (!connected_.load() || text.empty()) return false;
    std::lock_guard<std::mutex> lock(mutex_);
    MsgChat chat{};
    chat.sender_vip = local_vip_.load();
    std::strncpy(chat.sender_name, nickname_.c_str(), kMaxNicknameLen - 1);
    uint16_t len = static_cast<uint16_t>(std::min<size_t>(text.size(), 1024));
    chat.text_len = len;

    ByteBuffer payload(sizeof(chat) + len);
    payload.appendPod(chat);
    payload.append(text.data(), len);
    return queueServerControlMsg(ControlMsgType::ChatMessage, payload.data(), payload.size());
}

bool VpnClient::queueServerControlMsg(ControlMsgType type, const void* payload, size_t len) {
    if (server_send_buf_.size() + sizeof(ControlHeader) + len > kMaxServerSendBufBytes) {
        return false; // server backlog: drop rather than grow without bound
    }
    ControlHeader hdr{};
    hdr.magic = kProtocolMagic;
    hdr.type = static_cast<uint8_t>(type);
    hdr.payload_len = static_cast<uint32_t>(len);
    server_send_buf_.appendPod(hdr);
    if (len > 0 && payload) {
        server_send_buf_.append(payload, len);
    }
    return true;
}


} // namespace apivpn::client
