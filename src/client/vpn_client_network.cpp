#include "apivpn/client/vpn_client.hpp"
#include "apivpn/common/logger.hpp"

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstring>
#include <memory>
#include <thread>
#include <vector>

// Purpose: Server control-channel I/O and low-level UDP transport helpers.

namespace apivpn::client {

void VpnClient::sendDirectUdp(const platform::Endpoint& dst, const void* data, size_t len) {
    if (dst.isIpv6()) {
        if (udp_transport_sock_v6_.isValid() && !disable_ipv6_p2p_.load()) {
            udp_transport_sock_v6_.sendTo(data, len, dst);
        }
    } else if (dst.isIpv4()) {
        if (udp_transport_sock_.isValid()) {
            udp_transport_sock_.sendTo(data, len, dst);
        }
    }
}

void VpnClient::sendServerUdpRegister(uint32_t vip, uint64_t token) {
    if (vip == 0 || server_udp_port_ == 0) return;

    UdpRegisterPacket reg{};
    reg.hdr.magic = kProtocolMagic;
    reg.hdr.type = static_cast<uint8_t>(UdpPacketType::UdpRegister);
    reg.src_vip = vip;
    reg.session_token = token;

    for (const auto& base_ep : server_resolved_endpoints_) {
        platform::Endpoint dst = base_ep;
        dst.port_host = server_udp_port_;
        sendDirectUdp(dst, &reg, sizeof(reg));
    }
}

void VpnClient::networkWorkerLoop() {
    // Reused across iterations: rebuilding this list every millisecond would
    // otherwise reallocate the vector (and its capacity growth) 1000x/second.
    std::vector<platform::PollItem> poll_items;

    while (gateways_running_.load()) {
        poll_items.clear();
        size_t idx_udp_gw = ~static_cast<size_t>(0);
        size_t idx_tcp_gw = ~static_cast<size_t>(0);
        size_t idx_udp_v6 = ~static_cast<size_t>(0);
        size_t idx_udp_v4 = ~static_cast<size_t>(0);
        bool has_tunnels = false;

        {
            std::lock_guard<std::mutex> lock(mutex_);
            has_tunnels = !tcp_tunnels_.empty();
            idx_udp_gw = poll_items.size();
            poll_items.push_back({udp_gateway_sock_.handle(), true, false, false, false, false});

            idx_tcp_gw = poll_items.size();
            poll_items.push_back({tcp_gateway_listener_.handle(), true, false, false, false, false});

            if (udp_transport_sock_v6_.isValid()) {
                idx_udp_v6 = poll_items.size();
                poll_items.push_back({udp_transport_sock_v6_.handle(), true, false, false, false, false});
            }
            if (udp_transport_sock_.isValid()) {
                idx_udp_v4 = poll_items.size();
                poll_items.push_back({udp_transport_sock_.handle(), true, false, false, false, false});
            }

            if (connected_.load() && server_tcp_sock_.isValid()) {
                poll_items.push_back({
                    server_tcp_sock_.handle(),
                    true,
                    !server_send_buf_.empty(),
                    false, false, false
                });
            }

            for (const auto& kv : tcp_tunnels_) {
                const auto& t = kv.second;
                if (t->local_sock.isValid()) {
                    size_t max_wnd = t->effectiveSendWindow();
                    bool within_sack_span = !t->hasSackHole() || ((t->next_send_seq - t->last_rx_ack_num) < 64);
                    bool can_read_from_local = !t->local_fin &&
                                               (t->inFlightSegments() < max_wnd) &&
                                               (t->unacked_segments.size() < kMaxTcpWindowSegments) &&
                                               within_sack_span;
                    bool has_data_to_local = !t->to_local_buf.empty();
                    if (can_read_from_local || has_data_to_local) {
                        poll_items.push_back({
                            t->local_sock.handle(),
                            can_read_from_local,
                            has_data_to_local,
                            false, false, false
                        });
                    }
                }
            }
        }

        int poll_timeout_ms = (simulate_bw_fluctuation_.load() || has_tunnels) ? 1 : 5;
        platform::pollSockets(poll_items, poll_timeout_ms);
        uint64_t now_us = platform::monotonicMicros();

        std::lock_guard<std::mutex> lock(mutex_);

        // Not polling right now, so sockets retired by other threads can be closed safely.
        closeRetiredSockets();
        if (reconnect_result_ready_) {
            finishServerReconnect(now_us);
        }

        if (idx_udp_gw < poll_items.size() && poll_items[idx_udp_gw].can_read) {
            handleLocalUdpGatewayReadable();
        }
        if (idx_tcp_gw < poll_items.size() && poll_items[idx_tcp_gw].can_read) {
            handleLocalTcpGatewayAccept();
        }
        // Process IPv6 UDP transport first so IPv6 is always preferred over IPv4
        if (idx_udp_v6 < poll_items.size() && poll_items[idx_udp_v6].can_read) {
            handleUdpTransportReadable(udp_transport_sock_v6_);
        }
        if (idx_udp_v4 < poll_items.size() && poll_items[idx_udp_v4].can_read) {
            handleUdpTransportReadable(udp_transport_sock_);
        }

        if (connected_.load() && server_tcp_sock_.isValid()) {
            handleServerTcpReadable();

            now_us = platform::monotonicMicros();
            uint64_t srv_timeout = server_timeout_us_.load();
            if (connected_.load() && srv_timeout > 0 &&
                last_server_recv_us_ != 0 && now_us > last_server_recv_us_ &&
                (now_us - last_server_recv_us_) > srv_timeout) {
                logWarn("Server connection timed out after " +
                        std::to_string((now_us - last_server_recv_us_) / 1'000'000ULL) + "s of no response");
                handleServerConnectionLost(now_us);
            }

            if (connected_.load() && server_tcp_sock_.isValid() && !simulate_silent_drop_.load()) {
                uint64_t srv_ka_interval = kServerKeepAliveIntervalUs;
                if (srv_timeout > 0 && srv_timeout / 3 < srv_ka_interval) {
                    srv_ka_interval = std::max<uint64_t>(100'000ULL, srv_timeout / 3);
                }
                if (last_server_ping_sent_us_ == 0 ||
                    (now_us >= last_server_ping_sent_us_ &&
                     (now_us - last_server_ping_sent_us_) >= srv_ka_interval)) {
                    queueServerControlMsg(ControlMsgType::PingKeepalive, nullptr, 0);
                    last_server_ping_sent_us_ = now_us;
                }

                // Periodically register our UDP ports with Server and send hole-punch probes
                if (local_vip_.load() != 0) {
                    uint64_t udp_reg_interval = std::min<uint64_t>(2'000'000ULL, srv_ka_interval);
                    if (last_udp_reg_sent_us_ == 0 ||
                        (now_us >= last_udp_reg_sent_us_ && now_us - last_udp_reg_sent_us_ > udp_reg_interval)) {
                        sendServerUdpRegister(local_vip_.load(), session_token_);
                        last_udp_reg_sent_us_ = now_us;
                    }
                    sendHolePunchProbes(now_us);
                }
            }
        } else {
            // Server unreachable: keep retained direct sessions alive (direct keep-alives and
            // their timeouts) and drop any peer that fell back to the (unavailable) relay.
            if (!connected_.load() && local_vip_.load() != 0 && !peers_.empty() &&
                !simulate_silent_drop_.load()) {
                sendHolePunchProbes(now_us);
                dropRelayOnlyPeers();
            }
            if (want_connected_.load() && !simulate_silent_drop_.load()) {
                attemptServerReconnect(now_us);
            }
        }

        now_us = platform::monotonicMicros();
        if (last_pid_cleanup_us_ == 0 ||
            (now_us >= last_pid_cleanup_us_ && now_us - last_pid_cleanup_us_ > 1'000'000)) {
            last_pid_cleanup_us_ = now_us;
            bound_sockets_.erase(
                std::remove_if(bound_sockets_.begin(), bound_sockets_.end(),
                               [](const BoundSocketEntry& b) {
                                   return b.pid != 0 && !platform::ProcessLauncher::isProcessAlive(b.pid);
                               }),
                bound_sockets_.end());
        }
        flushPendingOutFrames(now_us);
        pumpTcpTunnels(now_us);
        flushPendingTcpAcks();

        if (connected_.load() && server_tcp_sock_.isValid() && !server_send_buf_.empty()) {
            while (!server_send_buf_.empty()) {
                bool would_block = false;
                int sent = server_tcp_sock_.sendNonBlocking(
                    server_send_buf_.data(), server_send_buf_.size(), would_block);
                if (sent > 0) {
                    server_send_buf_.consumeFront(static_cast<size_t>(sent));
                } else if (sent < 0) {
                    handleServerConnectionLost(platform::monotonicMicros());
                    break;
                } else {
                    break;
                }
            }
        }
    }
}

void VpnClient::handleServerTcpReadable() {
    uint8_t temp[65536];
    // Bounded per tick: a fast server must not monopolise the shared network
    // worker. Remaining bytes stay buffered in the socket for the next tick.
    for (int budget = kMaxTcpRecvChunksPerTick; budget > 0 && server_tcp_sock_.isValid(); --budget) {
        bool would_block = false;
        int got = server_tcp_sock_.recvNonBlocking(temp, sizeof(temp), would_block);
        if (got > 0) {
            server_recv_buf_.append(temp, static_cast<size_t>(got));
            last_server_recv_us_ = platform::monotonicMicros();
        } else if ((got == 0 && !would_block) || got < 0) {
            handleServerConnectionLost(platform::monotonicMicros());
            return;
        } else {
            break;
        }
    }

    while (server_recv_buf_.size() >= sizeof(ControlHeader)) {
        ControlHeader hdr{};
        std::memcpy(&hdr, server_recv_buf_.data(), sizeof(hdr));
        if (hdr.magic != kProtocolMagic || hdr.payload_len > kMaxPacketSize * 2) {
            handleServerConnectionLost(platform::monotonicMicros());
            return;
        }
        size_t total_len = sizeof(ControlHeader) + hdr.payload_len;
        if (server_recv_buf_.size() < total_len) break;

        const uint8_t* payload = server_recv_buf_.data() + sizeof(ControlHeader);
        processServerControlMsg(static_cast<ControlMsgType>(hdr.type), payload, hdr.payload_len);
        server_recv_buf_.consumeFront(total_len);
    }
}

void VpnClient::processServerControlMsg(ControlMsgType type, const uint8_t* payload, size_t len) {
    last_server_recv_us_ = platform::monotonicMicros();
    switch (type) {
        case ControlMsgType::ServerWelcome: {
            if (len < sizeof(MsgServerWelcome)) break;
            MsgServerWelcome w{};
            std::memcpy(&w, payload, sizeof(w));
            uint32_t prev_vip = local_vip_.load();
            if (prev_vip != 0 && prev_vip != w.assigned_vip) {
                // Direct sessions retained across a server outage belong to our old VIP: abort
                // their tunnels (RST still carries the old VIP) and start over with no peers.
                logWarn("Server assigned a different Virtual IP (" + ipv4ToString(w.assigned_vip) +
                        " instead of " + ipv4ToString(prev_vip) + "); resetting all peers and tunnels");
                for (auto& kv : tcp_tunnels_) {
                    abortTcpTunnel(*kv.second, true);
                }
                peers_.clear();
                pending_out_frames_.clear();
            }
            local_vip_.store(w.assigned_vip);
            preferred_vip_ = w.assigned_vip;
            session_token_ = w.session_token;
            server_udp_port_ = w.server_udp_port;
            join_time_us_ = platform::monotonicMicros();
            last_server_recv_us_ = join_time_us_;
            last_udp_reg_sent_us_ = 0;

            // Immediately send UDP registration packets (IPv6 & IPv4) to Server's UDP port
            if (!simulate_silent_drop_.load()) {
                sendServerUdpRegister(w.assigned_vip, w.session_token);
                last_udp_reg_sent_us_ = join_time_us_;
            }

            logInfo("Assigned Virtual IP: " + ipv4ToString(w.assigned_vip) +
                    " (Server connected via " + std::string(server_endpoint_.versionString()) +
                    " " + server_endpoint_.toString() + ")");
            cv_.notify_all();
            break;
        }

        case ControlMsgType::PeerListUpdate: {
            if (len < sizeof(MsgPeerListHeader)) break;
            MsgPeerListHeader hdr{};
            std::memcpy(&hdr, payload, sizeof(hdr));
            size_t expected = sizeof(MsgPeerListHeader) + hdr.peer_count * sizeof(PeerEntryWire);
            if (len < expected) break;

            const uint8_t* entries = payload + sizeof(MsgPeerListHeader);
            std::unordered_map<uint32_t, bool> seen;
            uint32_t my_vip = local_vip_.load();
            uint64_t now_us = platform::monotonicMicros();

            for (uint16_t i = 0; i < hdr.peer_count; ++i) {
                PeerEntryWire e{};
                std::memcpy(&e, entries + static_cast<size_t>(i) * sizeof(PeerEntryWire), sizeof(e));
                if (e.vip == my_vip || e.vip == 0) continue;
                seen[e.vip] = true;

                auto& p = peers_[e.vip];
                bool is_new_peer = (p.vip == 0);
                const uint64_t new_pair_key = e.pair_key;
                if (!is_new_peer && p.pair_key != new_pair_key) {
                    // The server only issues a new pair key when this VIP changed hands: whoever
                    // we had a direct route / tunnels with is not the current holder any more.
                    logWarn("Pair key for " + ipv4ToString(e.vip) +
                            " changed (VIP now held by a different client); dropping its direct route and tunnels");
                    abortTcpTunnelsToPeer(e.vip, true);
                    resetPeerDirectRoute(p);
                    p.candidate_endpoints.clear();
                    if (!force_relay_.load() && force_relay_peers_.count(e.vip) == 0) {
                        startSynchronizedHolePunch(p, now_us, false);
                    }
                }
                p.pair_key = new_pair_key;
                if (is_new_peer) {
                    p.vip = e.vip;
                    p.punch_start_us = now_us;
                    p.punch_gave_up_us = 0;
                    p.last_keepalive_sent_us = now_us;
                    p.last_any_recv_us = now_us;
                    p.connected_since_us = now_us;
                    p.connected_epoch_sec = std::chrono::duration_cast<std::chrono::seconds>(
                        std::chrono::system_clock::now().time_since_epoch()).count();
                    p.punch_gave_up = false;
                    p.hole_punch_cycles = 1;

                    // Send an immediate relayed PunchPing to measure Relay RTT and synchronize hole-punching
                    if (!simulate_silent_drop_.load()) {
                        UdpPunchPacket rping = makePunchPacket(UdpPacketType::PunchPing, p, now_us);
                        emitRelayDirect(e.vip, UdpPacketType::PunchPing, &rping, sizeof(rping));
                    }
                }
                char nick[kMaxNicknameLen] = {};
                std::memcpy(nick, e.nickname, kMaxNicknameLen - 1);
                p.nickname = nick;

                // Collect IPv6 and IPv4 candidate endpoints (preferring IPv6 first)
                std::vector<platform::Endpoint> v6_eps;
                std::vector<platform::Endpoint> v4_eps;
                uint8_t ep_count = std::min<uint8_t>(e.endpoint_count, static_cast<uint8_t>(kMaxEndpointsPerPeer));
                for (uint8_t k = 0; k < ep_count; ++k) {
                    platform::Endpoint ep = platform::fromEndpointWire(e.endpoints[k]);
                    if (!ep.isValidAddress() || ep.port_host == 0) continue;
                    if (ep.isIpv6()) {
                        if (std::find(v6_eps.begin(), v6_eps.end(), ep) == v6_eps.end()) {
                            v6_eps.push_back(ep);
                        }
                    } else if (ep.isIpv4()) {
                        if (std::find(v4_eps.begin(), v4_eps.end(), ep) == v4_eps.end()) {
                            v4_eps.push_back(ep);
                        }
                    }
                }
                std::vector<platform::Endpoint> new_candidates;
                new_candidates.reserve(v6_eps.size() + v4_eps.size());
                for (const auto& ep : v6_eps) new_candidates.push_back(ep);
                for (const auto& ep : v4_eps) new_candidates.push_back(ep);

                bool candidates_changed = !is_new_peer && (new_candidates != p.candidate_endpoints);
                p.candidate_endpoints = std::move(new_candidates);

                if (candidates_changed && p.route_mode != RouteMode::DirectP2P && !force_relay_.load()) {
                    // Peer registered new NAT endpoint(s) with server; ensure synchronized probing of new endpoints
                    if (p.punch_gave_up) {
                        startSynchronizedHolePunch(p, now_us, false);
                    } else {
                        p.last_punch_sent_us = 0;
                    }
                }
            }

            for (auto it = peers_.begin(); it != peers_.end(); ) {
                if (seen.find(it->first) == seen.end()) {
                    // The peer left; its VIP may be reassigned, so its tunnels must not outlive it.
                    abortTcpTunnelsToPeer(it->first, true);
                    it = peers_.erase(it);
                } else {
                    ++it;
                }
            }

            // Trigger immediate hole punch probes for peers still within their 10s hole-punch window
            if (!simulate_silent_drop_.load()) {
                sendHolePunchProbes(now_us);
            }
            break;
        }

        case ControlMsgType::ChatMessage: {
            if (len < sizeof(MsgChat)) break;
            MsgChat chat{};
            std::memcpy(&chat, payload, sizeof(chat));
            if (sizeof(MsgChat) + chat.text_len > len) break;
            char name_buf[kMaxNicknameLen] = {};
            std::memcpy(name_buf, chat.sender_name, kMaxNicknameLen - 1);
            std::string msg(reinterpret_cast<const char*>(payload + sizeof(MsgChat)), chat.text_len);
            chat_history_.push_back({chat.sender_vip, name_buf, msg});
            if (chat_history_.size() > kMaxChatHistoryMessages) {
                chat_history_.erase(chat_history_.begin(),
                                    chat_history_.begin() +
                                        static_cast<std::ptrdiff_t>(chat_history_.size() - kMaxChatHistoryMessages));
            }
            break;
        }

        case ControlMsgType::RelayPacket: {
            if (len < sizeof(MsgRelayHeader)) break;
            MsgRelayHeader rh{};
            std::memcpy(&rh, payload, sizeof(rh));
            if (sizeof(MsgRelayHeader) + rh.data_len > len) break;

            const uint8_t* inner = payload + sizeof(MsgRelayHeader);
            processIncomingVpnFrame(
                rh.src_vip,
                static_cast<UdpPacketType>(rh.inner_type),
                inner,
                rh.data_len,
                false,
                {});
            break;
        }

        case ControlMsgType::PongKeepalive:
            break;

        default:
            break;
    }
}


} // namespace apivpn::client
