#include "apivpn/client/vpn_client.hpp"
#include "apivpn/common/logger.hpp"

#include <algorithm>
#include <chrono>
#include <cstring>
#include <memory>
#include <thread>
#include <vector>

// Purpose: UDP hole punching, per-peer routing, and VPN frame ingress/egress.

namespace apivpn::client {

void VpnClient::startSynchronizedHolePunch(PeerState& peer, uint64_t now_us, bool send_relay_sync) {
    peer.punch_start_us = now_us;
    peer.punch_gave_up = false;
    peer.punch_gave_up_us = 0;
    peer.last_punch_sent_us = 0;
    peer.hole_punch_cycles++;

    uint32_t my_vip = local_vip_.load();
    if (my_vip != 0 && !simulate_silent_drop_.load()) {
        sendServerUdpRegister(my_vip, session_token_);
        last_udp_reg_sent_us_ = now_us;
    }

    if (send_relay_sync && !force_relay_.load() && !simulate_silent_drop_.load() && my_vip != 0) {
        // emitRelayDirect() drops it if the relay is down or its queue is full.
        UdpPunchPacket rping = makePunchPacket(UdpPacketType::PunchPing, peer, now_us);
        emitRelayDirect(peer.vip, UdpPacketType::PunchPing, &rping, sizeof(rping));
    }
}

UdpPunchPacket VpnClient::makePunchPacket(UdpPacketType type, const PeerState& peer, uint64_t timestamp_us) const {
    UdpPunchPacket pkt{};
    pkt.hdr.magic = kProtocolMagic;
    pkt.hdr.type = static_cast<uint8_t>(type);
    pkt.src_vip = local_vip_.load();
    pkt.dst_vip = peer.vip;
    pkt.timestamp_us = timestamp_us;
    pkt.pair_key = peer.pair_key;
    return pkt;
}

VpnClient::PeerState* VpnClient::authenticatePunchPacket(const UdpPunchPacket& pkt) {
    // Accept only packets from a known peer, addressed to us, carrying the secret the server
    // shared with exactly that peer. Anything else is dropped without a reply, so neither an
    // off-path host nor another peer can steer a route by claiming someone else's VIP.
    uint32_t my_vip = local_vip_.load();
    if (my_vip == 0 || pkt.dst_vip != my_vip || pkt.pair_key == 0) return nullptr;
    auto it = peers_.find(pkt.src_vip);
    if (it == peers_.end() || it->second.pair_key == 0 || it->second.pair_key != pkt.pair_key) {
        return nullptr;
    }
    return &it->second;
}

bool VpnClient::isVerifiedDirectSource(const PeerState& peer, const platform::Endpoint& src, uint64_t now_us) const {
    if (src.port_host == 0) return false;
    if (peer.route_mode == RouteMode::DirectP2P && src == peer.active_endpoint) return true;
    // The peer may still be sending from its other verified endpoint (e.g. it has not switched
    // families yet, or our route is forced onto the relay); accept that while it is fresh.
    const uint64_t timeout_us = direct_keepalive_timeout_us_.load();
    auto fresh = [&](uint64_t last_us) {
        return last_us != 0 && now_us >= last_us && (now_us - last_us) <= timeout_us;
    };
    if (src.isIpv6()) {
        return src == peer.best_v6_endpoint && fresh(peer.last_direct_v6_recv_us);
    }
    return src == peer.best_v4_endpoint && fresh(peer.last_direct_v4_recv_us);
}

void VpnClient::resetPeerDirectRoute(PeerState& peer) {
    peer.route_mode = RouteMode::ServerRelay;
    peer.active_endpoint = {};
    peer.best_v4_endpoint = {};
    peer.best_v6_endpoint = {};
    peer.last_direct_recv_us = 0;
    peer.last_direct_v4_recv_us = 0;
    peer.last_direct_v6_recv_us = 0;
}

void VpnClient::sendHolePunchProbes(uint64_t now_us) {
    uint32_t my_vip = local_vip_.load();
    if (my_vip == 0 || simulate_silent_drop_.load()) return;

    auto sendRelayKeepAlive = [&](PeerState& peer) {
        if (!connected_.load() || !server_tcp_sock_.isValid()) return;
        UdpPunchPacket ka = makePunchPacket(UdpPacketType::KeepAlive, peer, now_us);
        emitRelayDirect(peer.vip, UdpPacketType::KeepAlive, &ka, sizeof(ka));

        peer.last_keepalive_sent_us = now_us;
        peer.last_tunnel_tx_us = now_us;
        peer.keepalive_tx_count++;
    };

    uint64_t direct_timeout_us = direct_keepalive_timeout_us_.load();
    uint64_t retry_interval_us = hole_punch_retry_interval_us_.load();
    uint64_t ka_interval_us = tunnel_keepalive_interval_us_.load();
    uint64_t punch_window_us = hole_punch_timeout_us_.load();

    for (auto& kv : peers_) {
        auto& peer = kv.second;
        bool peer_force_relay = force_relay_.load() || force_relay_peers_.count(peer.vip) != 0;
        if (peer.punch_start_us == 0) {
            peer.punch_start_us = (join_time_us_ != 0) ? join_time_us_ : now_us;
        }
        if (peer.last_keepalive_sent_us == 0) {
            peer.last_keepalive_sent_us = now_us;
        }

        if (peer_force_relay) {
            if (now_us >= peer.last_keepalive_sent_us &&
                (now_us - peer.last_keepalive_sent_us) >= ka_interval_us) {
                sendRelayKeepAlive(peer);
            }
            continue;
        }

        // Downgrade if direct P2P hasn't received anything within the keep-alive timeout
        if (peer.route_mode == RouteMode::DirectP2P) {
            if (peer.active_endpoint.isIpv6()) {
                if (now_us > peer.last_direct_v6_recv_us &&
                    (now_us - peer.last_direct_v6_recv_us) > direct_timeout_us) {
                    if (peer.best_v4_endpoint.port_host != 0 &&
                        peer.last_direct_v4_recv_us != 0 &&
                        now_us >= peer.last_direct_v4_recv_us &&
                        (now_us - peer.last_direct_v4_recv_us) <= direct_timeout_us) {
                        peer.active_endpoint = peer.best_v4_endpoint;
                        peer.rtt_ms = peer.rtt_v4_ms;
                        peer.last_direct_recv_us = peer.last_direct_v4_recv_us;
                        logWarn("Direct IPv6 P2P keepalive timeout for " + ipv4ToString(peer.vip) +
                                ", falling back to Direct IPv4 P2P");
                    } else {
                        peer.route_mode = RouteMode::ServerRelay;
                        peer.active_endpoint = {};
                        peer.best_v6_endpoint = {};
                        peer.best_v4_endpoint = {};
                        logWarn("Direct P2P keepalive timeout for " + ipv4ToString(peer.vip) +
                                ", falling back to Server Relay and re-attempting synchronized hole punch");
                        startSynchronizedHolePunch(peer, now_us, true);
                    }
                }
            } else if (peer.active_endpoint.isIpv4()) {
                if (now_us > peer.last_direct_v4_recv_us &&
                    (now_us - peer.last_direct_v4_recv_us) > direct_timeout_us) {
                    peer.route_mode = RouteMode::ServerRelay;
                    peer.active_endpoint = {};
                    peer.best_v6_endpoint = {};
                    peer.best_v4_endpoint = {};
                    logWarn("Direct P2P keepalive timeout for " + ipv4ToString(peer.vip) +
                            ", falling back to Server Relay and re-attempting synchronized hole punch");
                    startSynchronizedHolePunch(peer, now_us, true);
                }
            }
        }

        // Enforce 10-second hole punching window, then pause and periodically retry in sync
        bool in_punch_window = (now_us >= peer.punch_start_us) &&
                               ((now_us - peer.punch_start_us) <= punch_window_us);

        if (!in_punch_window && peer.route_mode != RouteMode::DirectP2P) {
            if (!peer.punch_gave_up) {
                peer.punch_gave_up = true;
                peer.punch_gave_up_us = now_us;
                logInfo("Hole punching timed out after 10s for " + ipv4ToString(peer.vip) +
                        ", pausing hole punch attempts and using Server Relay (will retry periodically)");
            } else if (retry_interval_us > 0 && peer.punch_gave_up_us != 0 &&
                       now_us >= peer.punch_gave_up_us &&
                       (now_us - peer.punch_gave_up_us) >= retry_interval_us) {
                logInfo("Periodic hole-punch retry triggered for " + ipv4ToString(peer.vip) +
                        ", starting synchronized hole-punch attempt");
                startSynchronizedHolePunch(peer, now_us, true);
                in_punch_window = true;
            }

            if (peer.punch_gave_up) {
                if (now_us >= peer.last_keepalive_sent_us &&
                    (now_us - peer.last_keepalive_sent_us) >= ka_interval_us) {
                    sendRelayKeepAlive(peer);
                }
                continue;
            }
        }

        bool has_v6_candidates = false;
        if (udp_transport_sock_v6_.isValid()) {
            for (const auto& ep : peer.candidate_endpoints) {
                if (ep.isIpv6()) {
                    has_v6_candidates = true;
                    break;
                }
            }
        }

        bool settled_direct = (peer.route_mode == RouteMode::DirectP2P) &&
                              (peer.active_endpoint.isIpv6() || !in_punch_window || !has_v6_candidates);

        if (settled_direct && peer.active_endpoint.port_host != 0) {
            // Tunnel keep-alive & periodic RTT ping: send a KeepAlive ping packet every 5 seconds
            // so the NAT mapping stays open and the GUI ping (RTT) updates regularly every 5s.
            if (now_us >= peer.last_keepalive_sent_us &&
                (now_us - peer.last_keepalive_sent_us) >= ka_interval_us) {
                UdpPunchPacket ka = makePunchPacket(UdpPacketType::KeepAlive, peer, now_us);
                sendDirectUdp(peer.active_endpoint, &ka, sizeof(ka));
                peer.last_keepalive_sent_us = now_us;
                peer.last_tunnel_tx_us = now_us;
                peer.keepalive_tx_count++;
            }
            continue;
        }

        // If still on ServerRelay while hole punching is in progress, also refresh Relay RTT every 5s
        if (peer.route_mode == RouteMode::ServerRelay &&
            now_us >= peer.last_keepalive_sent_us &&
            (now_us - peer.last_keepalive_sent_us) >= ka_interval_us) {
            sendRelayKeepAlive(peer);
        }

        uint64_t interval_us = 200'000;
        if (peer.last_punch_sent_us != 0 &&
            now_us >= peer.last_punch_sent_us &&
            (now_us - peer.last_punch_sent_us) < interval_us) {
            continue;
        }
        peer.last_punch_sent_us = now_us;
        peer.last_tunnel_tx_us = now_us;

        UdpPunchPacket ping = makePunchPacket(UdpPacketType::PunchPing, peer, now_us);

        // Try both IPv6 and IPv4 candidate endpoints simultaneously
        for (const auto& ep : peer.candidate_endpoints) {
            if (ep.isIpv6()) {
                if (udp_transport_sock_v6_.isValid()) {
                    udp_transport_sock_v6_.sendTo(&ping, sizeof(ping), ep);
                    peer.punch_probes_sent++;
                }
            } else if (ep.isIpv4()) {
                if (udp_transport_sock_.isValid()) {
                    udp_transport_sock_.sendTo(&ping, sizeof(ping), ep);
                    peer.punch_probes_sent++;
                }
            }
        }
    }
}

void VpnClient::onDirectPacketReceived(
    PeerState& peer,
    const platform::Endpoint& src_ep,
    uint64_t now_us,
    const uint32_t* rtt_ms) {

    // Only reached for pair-key-authenticated PunchPong / KeepAlive packets.
    if (src_ep.isIpv6() && disable_ipv6_p2p_.load()) return;
    peer.last_any_recv_us = now_us;

    // Remember the verified endpoint per family even while relaying is forced, so the peer's
    // own direct traffic is still accepted (see isVerifiedDirectSource()).
    if (src_ep.isIpv6()) {
        peer.last_direct_v6_recv_us = now_us;
        peer.best_v6_endpoint = src_ep;
        if (rtt_ms) {
            peer.rtt_v6_ms = *rtt_ms;
        }
    } else if (src_ep.isIpv4()) {
        peer.last_direct_v4_recv_us = now_us;
        peer.best_v4_endpoint = src_ep;
        if (rtt_ms) {
            peer.rtt_v4_ms = *rtt_ms;
        }
    } else {
        return;
    }

    if (force_relay_.load() || force_relay_peers_.count(peer.vip) != 0) return;
    peer.punch_gave_up = false;
    peer.punch_gave_up_us = 0;
    if (peer.last_tunnel_tx_us == 0) {
        peer.last_tunnel_tx_us = now_us;
    }

    auto sendConfirmPunchPing = [&](const platform::Endpoint& ep) {
        if (local_vip_.load() == 0 || simulate_silent_drop_.load()) return;
        UdpPunchPacket confirm = makePunchPacket(UdpPacketType::PunchPing, peer, now_us);
        sendDirectUdp(ep, &confirm, sizeof(confirm));
    };

    if (src_ep.isIpv6()) {
        bool was_v6_direct = (peer.route_mode == RouteMode::DirectP2P && peer.active_endpoint.isIpv6());
        peer.last_direct_recv_us = now_us;
        peer.active_endpoint = src_ep;
        peer.route_mode = RouteMode::DirectP2P;
        if (rtt_ms) {
            peer.rtt_ms = *rtt_ms;
            peer.last_rtt_gui_update_us = now_us;
        }
        if (!was_v6_direct) {
            peer.last_tunnel_tx_us = now_us;
            peer.last_keepalive_sent_us = now_us;
            sendConfirmPunchPing(src_ep);
            logInfo("Direct IPv6 UDP Hole-Punched P2P established with " +
                    ipv4ToString(peer.vip) + " via " + src_ep.toString());
        }
    } else {
        // Only use IPv4 as active_endpoint if IPv6 is not currently active
        bool has_active_v6 = (peer.route_mode == RouteMode::DirectP2P && peer.active_endpoint.isIpv6());
        if (!has_active_v6) {
            bool was_v4_direct = (peer.route_mode == RouteMode::DirectP2P && peer.active_endpoint.isIpv4());
            peer.last_direct_recv_us = now_us;
            peer.active_endpoint = src_ep;
            peer.route_mode = RouteMode::DirectP2P;
            if (rtt_ms) {
                peer.rtt_ms = *rtt_ms;
                peer.last_rtt_gui_update_us = now_us;
            }
            if (!was_v4_direct) {
                peer.last_tunnel_tx_us = now_us;
                peer.last_keepalive_sent_us = now_us;
                sendConfirmPunchPing(src_ep);
                logInfo("Direct IPv4 UDP Hole-Punched P2P established with " +
                        ipv4ToString(peer.vip) + " via " + src_ep.toString());
            }
        }
    }
}

void VpnClient::handleUdpTransportReadable(platform::NetSocket& sock) {
    if (!sock.isValid()) return;
    uint8_t buf[65536];
    uint64_t now_us = platform::monotonicMicros();

    // Bounded per tick so a datagram flood cannot starve the rest of the loop.
    for (int budget = kMaxDatagramsPerTick; budget > 0; --budget) {
        platform::Endpoint src_ep{};
        bool would_block = false;
        int got = sock.recvFrom(buf, sizeof(buf), src_ep, would_block);
        if (got < 0) break;                 // real socket error
        if (got == 0) {
            if (would_block) break;         // drained
            continue;                       // zero-length datagram: skip, keep the batch going
        }

        if (simulate_silent_drop_.load()) {
            continue;
        }

        if (src_ep.isIpv6() && disable_ipv6_p2p_.load()) {
            continue;
        }

        if (static_cast<size_t>(got) < sizeof(UdpWireHeader)) continue;
        UdpWireHeader wh{};
        std::memcpy(&wh, buf, sizeof(wh));
        if (wh.magic != kProtocolMagic) continue;

        auto pkt_type = static_cast<UdpPacketType>(wh.type);
        if (pkt_type == UdpPacketType::UdpRegAck) {
            continue;
        }

        if ((pkt_type == UdpPacketType::PunchPing ||
             pkt_type == UdpPacketType::PunchPong ||
             pkt_type == UdpPacketType::KeepAlive) &&
            static_cast<size_t>(got) >= sizeof(UdpPunchPacket)) {
            if (simulate_punch_blocked_.load()) continue;
            UdpPunchPacket pkt{};
            std::memcpy(&pkt, buf, sizeof(pkt));

            // Unauthenticated (unknown peer, wrong destination or wrong pair key): drop silently
            // and never reply, so it can neither move a route nor be used as a reflector.
            PeerState* auth_peer = authenticatePunchPacket(pkt);
            if (!auth_peer) continue;
            PeerState& peer = *auth_peer;

            if (pkt_type == UdpPacketType::PunchPing) {
                peer.last_any_recv_us = now_us;
                if (peer.punch_gave_up && !force_relay_.load()) {
                    startSynchronizedHolePunch(peer, now_us, false);
                }

                // Reply with PunchPong immediately on the same socket (echoing the timestamp)
                UdpPunchPacket pong = makePunchPacket(UdpPacketType::PunchPong, peer, pkt.timestamp_us);
                sock.sendTo(&pong, sizeof(pong), src_ep);
                peer.last_tunnel_tx_us = now_us;

                // Record the authenticated source as a peer-reflexive candidate so
                // sendHolePunchProbes also probes it back
                if (std::find(peer.candidate_endpoints.begin(),
                              peer.candidate_endpoints.end(), src_ep) == peer.candidate_endpoints.end() &&
                    peer.candidate_endpoints.size() < kMaxEndpointsPerPeer * 2) {
                    if (src_ep.isIpv6()) {
                        peer.candidate_endpoints.insert(peer.candidate_endpoints.begin(), src_ep);
                    } else {
                        peer.candidate_endpoints.push_back(src_ep);
                    }
                }
            } else if (pkt_type == UdpPacketType::PunchPong) {
                uint64_t rtt_us = (now_us >= pkt.timestamp_us) ? (now_us - pkt.timestamp_us) : 0;
                uint32_t rtt_ms = static_cast<uint32_t>((rtt_us + 500) / 1000);
                onDirectPacketReceived(peer, src_ep, now_us, &rtt_ms);
            } else {
                peer.keepalive_rx_count++;
                onDirectPacketReceived(peer, src_ep, now_us, nullptr);
                // Echo back as PunchPong with original timestamp_us so sender updates its RTT every 5s
                UdpPunchPacket pong = makePunchPacket(UdpPacketType::PunchPong, peer, pkt.timestamp_us);
                sock.sendTo(&pong, sizeof(pong), src_ep);
            }
            continue;
        }

        if (pkt_type == UdpPacketType::VpnUdpData || pkt_type == UdpPacketType::VpnTcpFrame) {
            const uint8_t* inner = buf + sizeof(UdpWireHeader);
            size_t inner_len = static_cast<size_t>(got) - sizeof(UdpWireHeader);
            uint32_t src_vip = 0;
            if (pkt_type == UdpPacketType::VpnUdpData && inner_len >= sizeof(VpnUdpDataHeader)) {
                std::memcpy(&src_vip, inner, sizeof(uint32_t));
            } else if (pkt_type == UdpPacketType::VpnTcpFrame && inner_len >= sizeof(VpnTcpFrameHeader)) {
                std::memcpy(&src_vip, inner + sizeof(uint32_t), sizeof(uint32_t));
            }
            if (src_vip == 0) continue;
            // Direct data is trusted only from an endpoint already verified for the claimed peer
            // by pair-key-authenticated punch/keep-alive traffic; it never creates or moves a
            // route by itself. Frames claiming unknown VIPs are dropped.
            auto peer_it = peers_.find(src_vip);
            if (peer_it == peers_.end() || !isVerifiedDirectSource(peer_it->second, src_ep, now_us)) {
                continue;
            }
            processIncomingVpnFrame(src_vip, pkt_type, inner, inner_len, true, src_ep);
        }
    }
}

void VpnClient::processIncomingVpnFrame(
    uint32_t verified_src_vip,
    UdpPacketType type,
    const uint8_t* data,
    size_t len,
    bool from_direct_udp,
    const platform::Endpoint& udp_src) {

    if (simulate_silent_drop_.load()) return;

    uint64_t now_us = platform::monotonicMicros();

    // Handle relayed PunchPing / KeepAlive / PunchPong for Server Relay RTT measurement & Hole-Punch Sync
    if (type == UdpPacketType::PunchPing || type == UdpPacketType::KeepAlive ||
        type == UdpPacketType::PunchPong) {
        if (len < sizeof(UdpPunchPacket)) return;
        UdpPunchPacket pkt{};
        std::memcpy(&pkt, data, sizeof(pkt));
        // The server vouches for the relay header's source; the pair key for the rest.
        if (pkt.src_vip != verified_src_vip) return;
        PeerState* auth_peer = authenticatePunchPacket(pkt);
        if (!auth_peer) return; // drop silently, no reply
        PeerState& peer = *auth_peer;
        peer.last_any_recv_us = now_us;

        if (type == UdpPacketType::PunchPong) {
            uint64_t rtt_us = (now_us >= pkt.timestamp_us) ? (now_us - pkt.timestamp_us) : 0;
            uint32_t rtt_ms = static_cast<uint32_t>((rtt_us + 500) / 1000);
            if (force_relay_.load() || peer.route_mode == RouteMode::ServerRelay) {
                peer.rtt_ms = rtt_ms;
                peer.last_rtt_gui_update_us = now_us;
            }
            return;
        }

        if (type == UdpPacketType::KeepAlive) {
            peer.keepalive_rx_count++;
        } else if (!from_direct_udp && !force_relay_.load()) {
            // Synchronize our hole-punch window whenever the remote peer initiates or re-attempts hole punching
            bool in_punch_window = (peer.punch_start_us != 0 && now_us >= peer.punch_start_us &&
                                    (now_us - peer.punch_start_us) <= hole_punch_timeout_us_.load());
            bool direct_stale = (peer.route_mode == RouteMode::DirectP2P) &&
                                (peer.last_direct_recv_us == 0 ||
                                 (now_us > peer.last_direct_recv_us &&
                                  (now_us - peer.last_direct_recv_us) > 100'000ULL));
            if (peer.punch_gave_up || !in_punch_window || direct_stale) {
                if (direct_stale) {
                    peer.route_mode = RouteMode::ServerRelay;
                    peer.active_endpoint = {};
                    peer.best_v6_endpoint = {};
                    peer.best_v4_endpoint = {};
                    logInfo("Peer " + ipv4ToString(peer.vip) +
                            " requested synchronized hole-punch re-attempt; resetting stale Direct P2P route");
                } else {
                    logInfo("Synchronizing UDP hole-punch window with peer " + ipv4ToString(peer.vip));
                }
                startSynchronizedHolePunch(peer, now_us, false);
                sendHolePunchProbes(now_us);
            }
        }

        // Relayed echo (capped by emitRelayDirect) so the sender can measure the relay RTT
        UdpPunchPacket pong = makePunchPacket(UdpPacketType::PunchPong, peer, pkt.timestamp_us);
        emitRelayDirect(verified_src_vip, UdpPacketType::PunchPong, &pong, sizeof(pong));
        return;
    }

    // Direct frames were already checked against the peer's verified endpoints by
    // handleUdpTransportReadable(); relayed ones carry a server-enforced source VIP.
    auto peer_it = peers_.find(verified_src_vip);
    if (peer_it != peers_.end()) {
        peer_it->second.rx_packets++;
        peer_it->second.rx_bytes += len;
        peer_it->second.last_any_recv_us = now_us;
        // Data from the active endpoint shows the direct path is alive; it never switches or
        // creates a route by itself (only authenticated punch/keep-alive packets do).
        if (from_direct_udp && udp_src == peer_it->second.active_endpoint) {
            peer_it->second.last_direct_recv_us = now_us;
        }
    }
    total_rx_packets_++;

    if (type == UdpPacketType::VpnUdpData) {
        if (len < sizeof(VpnUdpDataHeader)) return;
        VpnUdpDataHeader uh{};
        std::memcpy(&uh, data, sizeof(uh));
        if (sizeof(VpnUdpDataHeader) + uh.payload_len > len) return;

        const uint8_t* payload = data + sizeof(VpnUdpDataHeader);
        deliverUdpToLocalSockets(verified_src_vip, uh.dst_vip, uh.src_vport, uh.dst_vport, payload, uh.payload_len);
    } else if (type == UdpPacketType::VpnTcpFrame) {
        if (len < sizeof(VpnTcpFrameHeader)) return;
        VpnTcpFrameHeader th{};
        std::memcpy(&th, data, sizeof(th));
        if (sizeof(VpnTcpFrameHeader) + th.payload_len > len) return;
        if (static_cast<size_t>(th.payload_len) > kMaxTcpSegmentPayload) return;
        // Only accept frames actually addressed to us.
        if (th.dst_vip != local_vip_.load()) return;
        th.src_vip = verified_src_vip;
        handleIncomingVpnTcpFrame(th, data + sizeof(VpnTcpFrameHeader), now_us);
    }
}

void VpnClient::emitFrameToPeer(
    PeerState& peer,
    UdpPacketType inner_type,
    const void* data,
    size_t len,
    bool force_relay_override) {

    uint32_t my_vip = local_vip_.load();
    if (my_vip == 0) return;

    peer.tx_packets++;
    peer.tx_bytes += len;
    peer.last_tunnel_tx_us = platform::monotonicMicros();
    total_tx_packets_++;

    bool peer_forced_relay = force_relay_peers_.count(peer.vip) != 0;
    // A per-frame relay preference (retransmits) is moot while the server is unreachable:
    // keep retained direct sessions working on their own.
    const bool relay_up = connected_.load() && server_tcp_sock_.isValid();
    bool use_direct = !force_relay_.load() &&
                      !peer_forced_relay &&
                      (!force_relay_override || !relay_up) &&
                      peer.route_mode == RouteMode::DirectP2P &&
                      peer.active_endpoint.port_host != 0;

    if (use_direct) {
        UdpWireHeader wh{};
        wh.magic = kProtocolMagic;
        wh.type = static_cast<uint8_t>(inner_type);
        uint8_t stack_pkt[2048];
        const size_t pkt_len = sizeof(wh) + len;
        if (pkt_len <= sizeof(stack_pkt)) {
            std::memcpy(stack_pkt, &wh, sizeof(wh));
            if (len > 0 && data) {
                std::memcpy(stack_pkt + sizeof(wh), data, len);
            }
            sendDirectUdp(peer.active_endpoint, stack_pkt, pkt_len);
        } else {
            ByteBuffer pkt(pkt_len);
            pkt.appendPod(wh);
            pkt.append(data, len);
            sendDirectUdp(peer.active_endpoint, pkt.data(), pkt.size());
        }
    } else if (connected_.load() && server_tcp_sock_.isValid()) {
        if (server_send_buf_.size() + sizeof(ControlHeader) + sizeof(MsgRelayHeader) + len >
            kMaxServerSendBufBytes) {
            return; // relay backlog: drop (virtual TCP retransmits)
        }
        MsgRelayHeader rh{};
        rh.src_vip = my_vip;
        rh.dst_vip = peer.vip;
        rh.inner_type = static_cast<uint8_t>(inner_type);
        rh.data_len = static_cast<uint16_t>(len);

        ControlHeader ch{};
        ch.magic = kProtocolMagic;
        ch.type = static_cast<uint8_t>(ControlMsgType::RelayPacket);
        ch.payload_len = static_cast<uint32_t>(sizeof(rh) + len);
        server_send_buf_.appendPod(ch);
        server_send_buf_.appendPod(rh);
        if (len > 0 && data) {
            server_send_buf_.append(data, len);
        }
    }
}

void VpnClient::emitRelayDirect(uint32_t dst_vip, UdpPacketType inner_type, const void* data, size_t len) {
    uint32_t my_vip = local_vip_.load();
    if (my_vip == 0 || !connected_.load() || !server_tcp_sock_.isValid()) return;
    if (server_send_buf_.size() + sizeof(ControlHeader) + sizeof(MsgRelayHeader) + len >
        kMaxServerSendBufBytes) {
        return; // relay backlog: drop rather than grow without bound
    }

    MsgRelayHeader rh{};
    rh.src_vip = my_vip;
    rh.dst_vip = dst_vip;
    rh.inner_type = static_cast<uint8_t>(inner_type);
    rh.data_len = static_cast<uint16_t>(len);

    ControlHeader ch{};
    ch.magic = kProtocolMagic;
    ch.type = static_cast<uint8_t>(ControlMsgType::RelayPacket);
    ch.payload_len = static_cast<uint32_t>(sizeof(rh) + len);
    server_send_buf_.appendPod(ch);
    server_send_buf_.appendPod(rh);
    if (len > 0 && data) {
        server_send_buf_.append(data, len);
    }
}

bool VpnClient::impairmentActive() const {
    return simulate_reorder_percent_.load() > 0 ||
           simulate_duplicate_percent_.load() > 0 ||
           simulate_latency_us_.load() > 0;
}

uint32_t VpnClient::nextSimRand() {
    uint32_t x = sim_prng_state_;
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    sim_prng_state_ = x;
    return x;
}

void VpnClient::enqueueImpairedFrame(
    uint32_t dst_vip,
    UdpPacketType inner_type,
    const void* data,
    size_t len,
    bool force_relay_override) {

    uint64_t now_us = platform::monotonicMicros();
    uint64_t base_delay = simulate_latency_us_.load();

    auto push = [&](uint64_t extra_delay_us) {
        PendingOutFrame f{};
        f.dst_vip = dst_vip;
        f.type = inner_type;
        f.force_relay = force_relay_override;
        f.bytes.assign(static_cast<const uint8_t*>(data), static_cast<const uint8_t*>(data) + len);
        f.due_us = now_us + base_delay + extra_delay_us;
        pending_out_frames_.push_back(std::move(f));
    };

    // Reordering: delay this frame by an extra slot so subsequent frames overtake it
    uint32_t reorder_pct = simulate_reorder_percent_.load();
    uint64_t reorder_delay = (reorder_pct > 0 && (nextSimRand() % 100u) < reorder_pct) ? 30'000ULL : 0;
    push(reorder_delay);

    // Duplication: emit the same frame a second time slightly later
    uint32_t dup_pct = simulate_duplicate_percent_.load();
    if (dup_pct > 0 && (nextSimRand() % 100u) < dup_pct) {
        push(1'000ULL);
    }
}

void VpnClient::flushPendingOutFrames(uint64_t now_us) {
    if (pending_out_frames_.empty()) return;

    std::vector<PendingOutFrame> ready;
    for (auto it = pending_out_frames_.begin(); it != pending_out_frames_.end(); ) {
        if (now_us >= it->due_us) {
            ready.push_back(std::move(*it));
            it = pending_out_frames_.erase(it);
        } else {
            ++it;
        }
    }

    for (auto& f : ready) {
        auto peer_it = peers_.find(f.dst_vip);
        if (peer_it == peers_.end()) continue;
        emitFrameToPeer(peer_it->second, f.type, f.bytes.data(), f.bytes.size(), f.force_relay);
    }
}

void VpnClient::sendVpnFrameToTarget(
    uint32_t dst_vip,
    UdpPacketType inner_type,
    const void* data,
    size_t len,
    bool force_relay_override) {

    uint32_t my_vip = local_vip_.load();
    if (my_vip == 0) return;

    const bool impair = impairmentActive() && inner_type == UdpPacketType::VpnTcpFrame;

    auto routeToPeer = [&](PeerState& peer) {
        if (impair) {
            enqueueImpairedFrame(peer.vip, inner_type, data, len, force_relay_override);
        } else {
            emitFrameToPeer(peer, inner_type, data, len, force_relay_override);
        }
    };

    if (isBroadcastOrMulticast(dst_vip)) {
        for (auto& kv : peers_) {
            routeToPeer(kv.second);
        }
    } else if (isVirtualUnicast(dst_vip)) {
        auto it = peers_.find(dst_vip);
        if (it != peers_.end()) {
            routeToPeer(it->second);
        } else {
            emitRelayDirect(dst_vip, inner_type, data, len);
        }
    }
}


} // namespace apivpn::client
