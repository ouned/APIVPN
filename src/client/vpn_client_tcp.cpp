#include "apivpn/client/vpn_client.hpp"
#include "apivpn/common/logger.hpp"

#include <algorithm>
#include <chrono>
#include <cstring>
#include <memory>
#include <thread>
#include <vector>

// Purpose: Reliable virtual TCP stream multiplexer (TcpTunnel) over the VPN.

namespace apivpn::client {

void VpnClient::handleLocalTcpGatewayAccept() {
    while (true) {
        platform::Endpoint peer_ep{};
        platform::NetSocket sock = tcp_gateway_listener_.accept(&peer_ep);
        if (!sock.isValid()) break;

        sock.setNonBlocking(true);
        sock.setNoDelay(true);
        sock.setRecvBufferSize(4 * 1024 * 1024);
        sock.setSendBufferSize(4 * 1024 * 1024);

        bool matched = false;
        for (auto& kv : tcp_tunnels_) {
            auto& t = kv.second;
            if (!t->local_attached && !t->handshake_failed && t->expected_local_port == peer_ep.port_host) {
                t->local_sock = std::move(sock);
                t->local_attached = true;
                matched = true;
                break;
            }
        }
        if (!matched) {
            logWarn("Dropping loopback TCP connect from port " + std::to_string(peer_ep.port_host) +
                    ": no matching virtual TCP tunnel");
        }
    }
}

void VpnClient::sendTcpFrame(
    TcpTunnel& tunnel,
    uint8_t flags,
    uint32_t seq,
    const void* payload,
    uint16_t len,
    bool via_relay_fallback) {

    VpnTcpFrameHeader th{};
    th.conn_id = tunnel.conn_id;
    th.src_vip = local_vip_.load();
    th.dst_vip = tunnel.remote_vip;
    th.src_vport = tunnel.local_vport;
    th.dst_vport = tunnel.remote_vport;
    th.flags = flags;
    th.seq_num = seq;
    th.ack_num = tunnel.next_expected_seq;
    th.sack_bitmap = tunnel.buildSackBitmap();
    th.recv_window = tunnel.localRecvWindow();
    th.payload_len = len;

    // Every frame below carries an implicit cumulative ACK (ack_num). Only clear the
    // pending-ACK bookkeeping when the frame actually carries ACK information, so a
    // future non-ACK frame type cannot silently swallow a needed acknowledgement.
    const uint8_t ack_carrying =
        static_cast<uint8_t>(VpnTcpFlags::Ack) |
        static_cast<uint8_t>(VpnTcpFlags::Data) |
        static_cast<uint8_t>(VpnTcpFlags::Fin);
    if (flags & ack_carrying) {
        tunnel.ack_needed = false;
        tunnel.rx_since_last_ack = 0;
    }
    tunnel.last_advertised_rwnd = th.recv_window;

    uint8_t stack_buf[sizeof(VpnTcpFrameHeader) + kMaxTcpSegmentPayload];
    const size_t total_len = sizeof(th) + len;
    if (total_len <= sizeof(stack_buf)) {
        std::memcpy(stack_buf, &th, sizeof(th));
        if (len > 0 && payload) {
            std::memcpy(stack_buf + sizeof(th), payload, len);
        }
        sendVpnFrameToTarget(tunnel.remote_vip, UdpPacketType::VpnTcpFrame, stack_buf, total_len, via_relay_fallback);
    } else {
        ByteBuffer buf(total_len);
        buf.appendPod(th);
        if (len > 0 && payload) {
            buf.append(payload, len);
        }
        sendVpnFrameToTarget(tunnel.remote_vip, UdpPacketType::VpnTcpFrame, buf.data(), buf.size(), via_relay_fallback);
    }
}

void VpnClient::abortTcpTunnel(TcpTunnel& tunnel, bool send_rst) {
    if (send_rst && !tunnel.handshake_failed) {
        sendTcpFrame(tunnel, static_cast<uint8_t>(VpnTcpFlags::Rst), 0, nullptr, 0);
    }
    tunnel.handshake_failed = true;
    tunnel.local_fin = true;
    tunnel.bridge_connecting = false;
    tunnel.ack_needed = false;
    tunnel.to_local_buf.clear();
    tunnel.unacked_segments.clear();
    tunnel.inflight_segments = 0;
    tunnel.sacked_segments = 0;
    tunnel.out_of_order_segments.clear();
    tunnel.out_of_order_bytes = 0;
    tunnel.invalidateSackBitmap();
    // Abortive close so the local app sees a reset, not an orderly EOF that would look like a
    // complete stream. Only the option is set here; the socket is retired, never closed here,
    // because this can run on a thread other than the network thread that polls it.
    tunnel.local_sock.setAbortiveClose();
    retireSocket(tunnel.local_sock);
    cv_.notify_all();
}

void VpnClient::abortTcpTunnelsToPeer(uint32_t vip, bool send_rst) {
    for (auto& kv : tcp_tunnels_) {
        if (kv.second->remote_vip == vip) {
            abortTcpTunnel(*kv.second, send_rst);
        }
    }
}

uint32_t VpnClient::allocateConnId() {
    // Upper 24 bits from the OS CSPRNG so a third party cannot guess a live tunnel's id; the
    // low byte is our VIP's, which keeps every peer's ids disjoint (incoming SYNs must carry
    // their sender's low byte, see handleIncomingVpnTcpFrame()).
    const uint32_t low = local_vip_.load() & 0xFFu;
    while (true) {
        const uint32_t high = static_cast<uint32_t>(platform::secureRandom64() >> 32) & 0xFFFFFF00u;
        if (high == 0) continue;
        const uint32_t cid = high | low;
        if (tcp_tunnels_.find(cid) == tcp_tunnels_.end()) {
            return cid;
        }
    }
}

bool VpnClient::drainOutOfOrderSegments(TcpTunnel& tunnel, uint64_t now_us) {
    // Move now-in-order segments from the out-of-order map into to_local_buf while they fit
    // under kMaxTcpRecvBufBytes. pumpTcpTunnels() calls this again as the app drains the
    // buffer, so segments the peer already sees as SACKed are never stranded.
    bool advanced = false;
    while (true) {
        auto ooo = tunnel.out_of_order_segments.find(tunnel.next_expected_seq);
        if (ooo == tunnel.out_of_order_segments.end()) break;
        const size_t seg_len = ooo->second.payload.size();
        if (seg_len > 0 && tunnel.to_local_buf.size() + seg_len > kMaxTcpRecvBufBytes) break;
        if (seg_len > 0) {
            tunnel.to_local_buf.append(ooo->second.payload.data(), seg_len);
            if (tunnel.pid != 0) {
                auto& pt = proc_telemetry_[tunnel.pid];
                pt.rx_packets++;
                pt.rx_bytes += seg_len;
                pt.last_activity_us = now_us;
            }
        }
        if (ooo->second.flags & static_cast<uint8_t>(VpnTcpFlags::Fin)) {
            tunnel.remote_fin = true;
        }
        tunnel.out_of_order_bytes = (tunnel.out_of_order_bytes > seg_len) ? (tunnel.out_of_order_bytes - seg_len) : 0;
        tunnel.out_of_order_segments.erase(ooo);
        tunnel.next_expected_seq++;
        tunnel.invalidateSackBitmap();
        advanced = true;
    }
    return advanced;
}

void VpnClient::flushPendingTcpAcks() {
    for (auto& kv : tcp_tunnels_) {
        TcpTunnel& t = *kv.second;
        if (t.ack_needed && !t.handshake_failed) {
            sendTcpFrame(t, static_cast<uint8_t>(VpnTcpFlags::Ack), 0, nullptr, 0);
        }
    }
}

void VpnClient::handleIncomingVpnTcpFrame(const VpnTcpFrameHeader& hdr, const uint8_t* payload, uint64_t now_us) {
    uint8_t flags = hdr.flags;
    if (static_cast<size_t>(hdr.payload_len) > kMaxTcpSegmentPayload) return;

    if (flags & static_cast<uint8_t>(VpnTcpFlags::Syn)) {
        // A peer's conn_ids carry its own VIP's low byte (allocateConnId()), so a SYN can never
        // name one of our outgoing tunnels or another peer's tunnel.
        if ((hdr.conn_id & 0xFFu) != (hdr.src_vip & 0xFFu)) return;
        auto existing = tcp_tunnels_.find(hdr.conn_id);
        if (existing != tcp_tunnels_.end()) {
            TcpTunnel& et = *existing->second;
            // Never touch a tunnel that belongs to someone else (or is already being torn down).
            if (et.remote_vip != hdr.src_vip || et.remote_vport != hdr.src_vport || et.handshake_failed) {
                return;
            }
            et.last_rx_us = now_us;
            // While the app-side bridge connect is still in progress, hold off on the
            // SYN-ACK (the peer simply retransmits its SYN until we are ready).
            if (!et.bridge_connecting) {
                sendTcpFrame(et, static_cast<uint8_t>(VpnTcpFlags::SynAck), 0, nullptr, 0);
            }
            return;
        }

        auto sendRefusal = [&](uint16_t bridge_port) {
            if (bridge_port != 0) {
                incoming_tcp_peer_map_.erase(bridge_port);
            }
            TcpTunnel tmp{};
            tmp.conn_id = hdr.conn_id;
            tmp.remote_vip = hdr.src_vip;
            tmp.remote_vport = hdr.src_vport;
            tmp.local_vport = hdr.dst_vport;
            sendTcpFrame(tmp, static_cast<uint8_t>(VpnTcpFlags::Rst), 0, nullptr, 0);
        };

        // SYN-flood bound: per remote peer and in total.
        size_t peer_tunnels = 0;
        for (const auto& kv : tcp_tunnels_) {
            if (kv.second->remote_vip == hdr.src_vip) ++peer_tunnels;
        }
        if (tcp_tunnels_.size() >= kMaxTcpTunnelsTotal || peer_tunnels >= kMaxTcpTunnelsPerPeer) {
            sendRefusal(0);
            return;
        }

        uint16_t target_actual_port = 0;
        uint32_t target_pid = 0;
        for (const auto& b : bound_sockets_) {
            if (b.proto == IpcProto::Tcp && b.virtual_port == hdr.dst_vport) {
                target_actual_port = b.actual_port;
                target_pid = b.pid;
                break;
            }
        }

        if (target_actual_port == 0) {
            sendRefusal(0);
            return;
        }

        platform::NetSocket bridge = platform::NetSocket::createTcp(platform::IpVersion::V4);
        if (!bridge.isValid() || !bridge.bind(kLoopbackIp, 0)) {
            return;
        }
        platform::Endpoint bridge_local_ep{};
        bridge.getLocalEndpoint(bridge_local_ep);
        incoming_tcp_peer_map_[bridge_local_ep.port_host] = {hdr.src_vip, hdr.src_vport};

        // Initiate the loopback connect WITHOUT blocking the shared network worker.
        // Completion (and the SYN-ACK) is handled from pumpTcpTunnels().
        int crc = bridge.startConnect(platform::Endpoint::fromIpv4(kLoopbackIp, target_actual_port));
        if (crc < 0) {
            sendRefusal(bridge_local_ep.port_host);
            return;
        }

        bridge.setNoDelay(true);
        bridge.setRecvBufferSize(4 * 1024 * 1024);
        bridge.setSendBufferSize(4 * 1024 * 1024);

        auto tunnel = std::make_unique<TcpTunnel>();
        tunnel->conn_id = hdr.conn_id;
        tunnel->pid = target_pid;
        tunnel->remote_vip = hdr.src_vip;
        tunnel->remote_vport = hdr.src_vport;
        tunnel->local_vport = hdr.dst_vport;
        tunnel->local_sock = std::move(bridge);
        tunnel->bridge_local_port = bridge_local_ep.port_host;
        tunnel->last_rx_us = now_us;
        if (hdr.recv_window > 0) {
            tunnel->peer_recv_window = hdr.recv_window;
        }
        auto peer_it = peers_.find(hdr.src_vip);
        if (peer_it != peers_.end() && peer_it->second.rtt_ms > 0) {
            tunnel->updateRtt(static_cast<uint64_t>(peer_it->second.rtt_ms) * 1000ULL);
        }

        if (crc == 1) {
            tunnel->handshake_done = true;
            tunnel->local_attached = true;
            sendTcpFrame(*tunnel, static_cast<uint8_t>(VpnTcpFlags::SynAck), 0, nullptr, 0);
        } else {
            tunnel->bridge_connecting = true;
            tunnel->bridge_connect_start_us = now_us;
        }
        tcp_tunnels_[hdr.conn_id] = std::move(tunnel);
        return;
    }

    auto it = tcp_tunnels_.find(hdr.conn_id);
    if (it == tcp_tunnels_.end()) {
        if (!(flags & static_cast<uint8_t>(VpnTcpFlags::Rst))) {
            TcpTunnel tmp{};
            tmp.conn_id = hdr.conn_id;
            tmp.remote_vip = hdr.src_vip;
            tmp.remote_vport = hdr.src_vport;
            tmp.local_vport = hdr.dst_vport;
            tmp.next_expected_seq = hdr.seq_num + 1;
            sendTcpFrame(tmp, static_cast<uint8_t>(VpnTcpFlags::Ack) | static_cast<uint8_t>(VpnTcpFlags::Rst), 0, nullptr, 0);
        }
        return;
    }
    TcpTunnel& tunnel = *it->second;
    // Frames must come from the peer endpoint that owns this tunnel. Anything else is dropped
    // silently (no RST: that would let a third party tear the tunnel down).
    if (tunnel.remote_vip != hdr.src_vip || tunnel.remote_vport != hdr.src_vport) return;
    if (tunnel.handshake_failed) return; // aborted, waiting to be reaped
    tunnel.last_rx_us = now_us;

    if (flags & static_cast<uint8_t>(VpnTcpFlags::Rst)) {
        abortTcpTunnel(tunnel, false);
        return;
    }

    tunnel.peer_recv_window = hdr.recv_window;

    if (flags & static_cast<uint8_t>(VpnTcpFlags::SynAck)) {
        tunnel.handshake_done = true;
        cv_.notify_all();
    }

    if (flags & static_cast<uint8_t>(VpnTcpFlags::Ack)) {
        size_t acked_count = 0;
        uint64_t rtt_sample_us = 0;
        uint64_t fallback_rtt_sample_us = 0;
        bool acked_first_attempt = false;
        while (!tunnel.unacked_segments.empty() &&
               seqLess(tunnel.unacked_segments.front().seq_num, hdr.ack_num)) {
            const auto& front_seg = tunnel.unacked_segments.front();
            if (front_seg.send_count == 1 && !front_seg.sacked &&
                front_seg.first_sent_us > 0 && now_us >= front_seg.first_sent_us) {
                rtt_sample_us = now_us - front_seg.first_sent_us;
                acked_first_attempt = true;
            } else if (front_seg.last_sent_us > 0 && now_us >= front_seg.last_sent_us) {
                fallback_rtt_sample_us = now_us - front_seg.last_sent_us;
            }
            tunnel.noteSegmentPoppedFront();
            tunnel.unacked_segments.pop_front();
            acked_count++;
        }

        // Update RTT estimator at most ONCE per incoming ACK packet (prevents rttvar collapse on cumulative ACKs)
        if (rtt_sample_us > 0) {
            tunnel.updateRtt(rtt_sample_us);
            auto peer_it = peers_.find(tunnel.remote_vip);
            if (peer_it != peers_.end() && tunnel.srtt_us > 0) {
                if (peer_it->second.last_rtt_gui_update_us == 0 ||
                    (now_us >= peer_it->second.last_rtt_gui_update_us &&
                     (now_us - peer_it->second.last_rtt_gui_update_us) >= kTunnelKeepAliveIntervalUs)) {
                    peer_it->second.rtt_ms = static_cast<uint32_t>((tunnel.srtt_us + 500) / 1000);
                    peer_it->second.last_rtt_gui_update_us = now_us;
                }
            }
        } else if (fallback_rtt_sample_us > tunnel.srtt_us) {
            tunnel.updateRtt(fallback_rtt_sample_us);
        }

        if (acked_first_attempt) {
            tunnel.consecutive_rto_count = 0;
            tunnel.direct_data_acked = true;
        }

        if (acked_count > 0) {
            tunnel.last_rx_ack_num = hdr.ack_num;
            tunnel.dup_ack_count = 0;
            double prev_cwnd = tunnel.cwnd_segments;
            for (size_t i = 0; i < acked_count; ++i) {
                if (tunnel.cwnd_segments < tunnel.ssthresh_segments) {
                    tunnel.cwnd_segments += 1.0; // Slow Start: exponential growth to discover link speed
                } else {
                    tunnel.cwnd_segments += 2.0 / tunnel.cwnd_segments; // Congestion Avoidance: wireless-friendly BDP probing
                }
            }
            if (tunnel.cwnd_segments > static_cast<double>(kMaxTcpWindowSegments)) {
                tunnel.cwnd_segments = static_cast<double>(kMaxTcpWindowSegments);
            }
            if (tunnel.cwnd_segments > prev_cwnd) {
                tcp_cwnd_scale_ups_++;
            }
        } else if (!(flags & (static_cast<uint8_t>(VpnTcpFlags::Data) | static_cast<uint8_t>(VpnTcpFlags::Fin))) &&
                   hdr.ack_num == tunnel.last_rx_ack_num &&
                   !tunnel.unacked_segments.empty()) {
            tunnel.dup_ack_count++;
        }

        // Process 64-bit Selective ACK bitmap (SACK)
        if (hdr.sack_bitmap != 0 && !tunnel.unacked_segments.empty()) {
            tcp_sack_events_++;
            for (auto& seg : tunnel.unacked_segments) {
                if (seqGreater(seg.seq_num, hdr.ack_num)) {
                    uint32_t bit = seg.seq_num - hdr.ack_num - 1;
                    if (bit < 64 && (hdr.sack_bitmap & (1ULL << bit))) {
                        tunnel.noteSegmentSacked(seg);
                    }
                }
            }
        }

        // SACK-based & DupACK Fast Retransmit of missing segments
        if (!tunnel.unacked_segments.empty()) {
            uint32_t sacked_above = 0;
            bool loss_detected = false;
            for (auto seg_it = tunnel.unacked_segments.rbegin(); seg_it != tunnel.unacked_segments.rend(); ++seg_it) {
                if (seg_it->sacked) {
                    sacked_above++;
                } else if (!seg_it->fast_rexmit &&
                           (sacked_above >= 3 ||
                            (seg_it->seq_num == hdr.ack_num && tunnel.dup_ack_count >= 3))) {
                    seg_it->fast_rexmit = true;
                    seg_it->last_sent_us = now_us;
                    seg_it->send_count++;
                    tcp_fast_retransmits_++;
                    loss_detected = true;

                    bool use_relay = false;
                    auto peer_it = peers_.find(tunnel.remote_vip);
                    if (peer_it != peers_.end() && peer_it->second.route_mode == RouteMode::DirectP2P) {
                        if (now_us > peer_it->second.last_direct_recv_us &&
                            (now_us - peer_it->second.last_direct_recv_us) > 1'000'000ULL) {
                            use_relay = true;
                        }
                    }
                    sendTcpFrame(tunnel, seg_it->flags, seg_it->seq_num,
                                 seg_it->payload.empty() ? nullptr : seg_it->payload.data(),
                                 static_cast<uint16_t>(seg_it->payload.size()),
                                 use_relay);
                }
            }
            if (loss_detected) {
                // Refresh timer on segments beyond the 64-bit SACK window so they wait for hole repair
                for (auto& seg : tunnel.unacked_segments) {
                    if (!seg.sacked && !seg.fast_rexmit && seqGreater(seg.seq_num, hdr.ack_num + 64)) {
                        seg.last_sent_us = now_us;
                    }
                }
                if ((now_us - tunnel.last_cwnd_drop_us) > tunnel.rto_us) {
                    tunnel.ssthresh_segments = std::max(16.0, tunnel.cwnd_segments * 0.7);
                    tunnel.cwnd_segments = tunnel.ssthresh_segments;
                    tunnel.last_cwnd_drop_us = now_us;
                    tcp_cwnd_scale_downs_++;
                }
            }
        }
    }

    if ((flags & static_cast<uint8_t>(VpnTcpFlags::Data)) ||
        (flags & static_cast<uint8_t>(VpnTcpFlags::Fin))) {

        bool immediate_ack = false;
        if (hdr.seq_num == tunnel.next_expected_seq) {
            if (hdr.payload_len > 0 && payload &&
                tunnel.to_local_buf.size() + static_cast<size_t>(hdr.payload_len) > kMaxTcpRecvBufBytes) {
                // Receive buffer full (the sender overran our window, e.g. with a zero-window
                // probe): drop the segment without acknowledging it. It is retransmitted, and
                // the window-update ACK from pumpTcpTunnels() reopens the flow once drained.
                return;
            }
            if (hdr.payload_len > 0 && payload) {
                tunnel.to_local_buf.append(payload, hdr.payload_len);
                if (tunnel.pid != 0) {
                    auto& pt = proc_telemetry_[tunnel.pid];
                    pt.rx_packets++;
                    pt.rx_bytes += hdr.payload_len;
                    pt.last_activity_us = now_us;
                }
            }
            tunnel.next_expected_seq++;
            tunnel.invalidateSackBitmap();

            if (flags & static_cast<uint8_t>(VpnTcpFlags::Fin)) {
                tunnel.remote_fin = true;
                immediate_ack = true;
            }

            const bool fin_before_drain = tunnel.remote_fin;
            drainOutOfOrderSegments(tunnel, now_us);
            if (!fin_before_drain && tunnel.remote_fin) {
                immediate_ack = true;
            }

            // Immediately flush in-order received data to the local socket so localRecvWindow() stays open
            if (tunnel.local_sock.isValid() && !tunnel.to_local_buf.empty()) {
                while (!tunnel.to_local_buf.empty()) {
                    bool would_block = false;
                    int sent = tunnel.local_sock.sendNonBlocking(
                        tunnel.to_local_buf.data(), tunnel.to_local_buf.size(), would_block);
                    if (sent > 0) {
                        tunnel.to_local_buf.consumeFront(static_cast<size_t>(sent));
                    } else if (sent < 0) {
                        // The local app's socket is broken: abort the virtual connection
                        // (RST to the peer) exactly like a local read error.
                        abortTcpTunnel(tunnel, true);
                        return;
                    } else {
                        break;
                    }
                }
            }
        } else if (seqGreater(hdr.seq_num, tunnel.next_expected_seq)) {
            // Bounded by segment count, distance from the cumulative ACK and bytes (together
            // with the in-order buffer) so a peer cannot make us buffer without limit.
            const size_t seg_len = (hdr.payload_len > 0 && payload) ? static_cast<size_t>(hdr.payload_len) : 0;
            const uint32_t ahead = hdr.seq_num - tunnel.next_expected_seq;
            if (ahead <= kMaxTcpWindowSegments &&
                tunnel.out_of_order_segments.size() < kMaxTcpWindowSegments &&
                tunnel.out_of_order_segments.find(hdr.seq_num) == tunnel.out_of_order_segments.end() &&
                tunnel.to_local_buf.size() + tunnel.out_of_order_bytes + seg_len <= kMaxTcpRecvBufBytes) {
                TcpOutOfOrderSegment ooo_seg{};
                if (seg_len > 0) {
                    ooo_seg.payload.assign(payload, payload + seg_len);
                }
                ooo_seg.flags = flags;
                tunnel.out_of_order_segments[hdr.seq_num] = std::move(ooo_seg);
                tunnel.out_of_order_bytes += seg_len;
                tunnel.invalidateSackBitmap();
            }
            immediate_ack = true; // Out-of-order: immediately send SACK bitmap
        } else {
            immediate_ack = true; // Duplicate segment: immediately send cumulative ACK
        }

        tunnel.ack_needed = true;
        tunnel.rx_since_last_ack++;
        if (immediate_ack || tunnel.rx_since_last_ack >= 16) {
            sendTcpFrame(tunnel, static_cast<uint8_t>(VpnTcpFlags::Ack), 0, nullptr, 0);
        }
    }
}

void VpnClient::pumpTcpTunnels(uint64_t now_us) {
    uint8_t buf[kMaxTcpSegmentPayload];
    bool bw_fluct = simulate_bw_fluctuation_.load();
    const size_t max_burst_segments = bw_fluct ? 64 : 256;
    if (bw_fluct && !tcp_tunnels_.empty()) {
        if (bw_sim_start_us_ == 0) {
            bw_sim_start_us_ = now_us;
            bw_last_refill_us_ = now_us;
        }
        uint32_t phase = static_cast<uint32_t>(((now_us - bw_sim_start_us_) / 15'000ULL) % 4ULL);
        // Fluctuate link speed every 15ms across 4 tiers: 16 MB/s -> 6 MB/s (dip) -> 48 MB/s (surge) -> 10 MB/s
        static constexpr double kPhaseRatesBps[4] = {
            16.0 * 1024.0 * 1024.0,
            6.0 * 1024.0 * 1024.0,
            48.0 * 1024.0 * 1024.0,
            10.0 * 1024.0 * 1024.0
        };
        static constexpr double kPhaseMaxBucket[4] = {
            32.0 * 1024.0,
            12.0 * 1024.0,
            64.0 * 1024.0,
            16.0 * 1024.0
        };
        uint64_t elapsed_us = (now_us > bw_last_refill_us_) ? (now_us - bw_last_refill_us_) : 0;
        bw_last_refill_us_ = now_us;
        bw_tokens_bytes_ += (static_cast<double>(elapsed_us) / 1e6) * kPhaseRatesBps[phase];
        if (bw_tokens_bytes_ > kPhaseMaxBucket[phase]) {
            bw_tokens_bytes_ = kPhaseMaxBucket[phase];
        }
        if (phase != bw_prev_phase_) {
            if (phase == 1 || phase == 3) {
                bw_tail_drop_budget_ = 2;
            }
            bw_prev_phase_ = phase;
        }
    }

    uint32_t loss_pct = simulate_loss_percent_.load();
    const uint64_t tcp_tunnel_idle_timeout_us = tcp_tunnel_idle_timeout_us_.load();
    const uint64_t tcp_tunnel_keepalive_interval_us = tcp_tunnel_keepalive_interval_us_.load();

    auto reapTunnel = [&](std::unordered_map<uint32_t, std::unique_ptr<TcpTunnel>>::iterator pos) {
        TcpTunnel& tt = *pos->second;
        incoming_tcp_peer_map_.erase(tt.bridge_local_port);
        incoming_tcp_peer_map_.erase(tt.expected_local_port);
        tt.local_sock.close();
        return tcp_tunnels_.erase(pos);
    };

    for (auto it = tcp_tunnels_.begin(); it != tcp_tunnels_.end(); ) {
        TcpTunnel& t = *it->second;

        // Aborted (RST, local error, retransmit give-up, peer gone): nothing left to do.
        if (t.handshake_failed) {
            it = reapTunnel(it);
            continue;
        }

        // Outgoing tunnel whose hooked app never connected to the loopback TCP gateway.
        if (!t.local_attached && t.expected_local_port != 0 && t.created_us != 0 &&
            now_us > t.created_us && (now_us - t.created_us) > kTcpTunnelAttachTimeoutUs) {
            logWarn("Virtual TCP tunnel " + std::to_string(t.conn_id) + " to " +
                    ipv4ToString(t.remote_vip) + ":" + std::to_string(t.remote_vport) +
                    " was never attached by the local app; aborting");
            abortTcpTunnel(t, true);
            it = reapTunnel(it);
            continue;
        }

        // Complete an in-progress app-side (loopback) connect without ever blocking
        // the shared network worker thread.
        if (t.bridge_connecting) {
            int crc = t.local_sock.checkConnect();
            if (crc == 1) {
                t.bridge_connecting = false;
                t.handshake_done = true;
                t.local_attached = true;
                sendTcpFrame(t, static_cast<uint8_t>(VpnTcpFlags::SynAck), 0, nullptr, 0);
            } else if (crc < 0 ||
                       (now_us > t.bridge_connect_start_us &&
                        (now_us - t.bridge_connect_start_us) > kTcpTunnelBridgeConnectTimeoutUs)) {
                // The local app never accepted: refuse the virtual connection.
                sendTcpFrame(t, static_cast<uint8_t>(VpnTcpFlags::Rst), 0, nullptr, 0);
                it = reapTunnel(it);
                continue;
            } else {
                ++it;
                continue; // still connecting - nothing else to do this tick
            }
        }

        // Liveness: reap a tunnel whose peer has gone silent past the idle timeout so
        // dead peers cannot leak loopback sockets and receive buffers. Each side emits a
        // lightweight liveness probe on its own cadence so an idle-but-alive peer keeps
        // refreshing the other side's timer.
        if (t.handshake_done && t.last_rx_us != 0) {
            if (now_us > t.last_rx_us &&
                (now_us - t.last_rx_us) > tcp_tunnel_idle_timeout_us) {
                logWarn("Virtual TCP tunnel " + std::to_string(t.conn_id) + " to " +
                        ipv4ToString(t.remote_vip) + ":" + std::to_string(t.remote_vport) +
                        " timed out after " +
                        std::to_string((now_us - t.last_rx_us) / 1'000'000ULL) + "s idle");
                // Abortive close (and RST, in case the peer is alive but unreachable one way):
                // the stream is incomplete, so the app must see a reset, not an orderly EOF.
                abortTcpTunnel(t, true);
                it = reapTunnel(it);
                continue;
            }
            // Probes continue after our FIN: a half-closed tunnel is still waiting for the
            // peer's data, and without them the peer would reap it while the app waits.
            if ((t.last_keepalive_sent_us == 0 ||
                 (now_us > t.last_keepalive_sent_us &&
                  (now_us - t.last_keepalive_sent_us) >= tcp_tunnel_keepalive_interval_us)) &&
                t.unacked_segments.empty()) {
                // Pure liveness frame (no ACK/Data/Fin flags): the peer only refreshes its
                // last-received timestamp, so duplicate-ACK tracking is left untouched.
                sendTcpFrame(t, 0, 0, nullptr, 0);
                t.last_keepalive_sent_us = now_us;
            }
        }

        if (t.local_sock.isValid()) {
            if (!t.to_local_buf.empty()) {
                bool write_failed = false;
                while (!t.to_local_buf.empty()) {
                    bool would_block = false;
                    int sent = t.local_sock.sendNonBlocking(t.to_local_buf.data(), t.to_local_buf.size(), would_block);
                    if (sent > 0) {
                        t.to_local_buf.consumeFront(static_cast<size_t>(sent));
                    } else if (sent < 0) {
                        write_failed = true;
                        break;
                    } else {
                        break;
                    }
                }
                if (write_failed) {
                    // Same handling as a local read error: RST to the peer, then reap.
                    abortTcpTunnel(t, true);
                    it = reapTunnel(it);
                    continue;
                }
                // Advertise a recovered receive window once it has grown enough to matter
                // (covers recovery out of the window-0 / small-window regime).
                uint16_t cur_rwnd = t.localRecvWindow();
                if (cur_rwnd > t.last_advertised_rwnd &&
                    (t.last_advertised_rwnd == 0 ||
                     static_cast<uint16_t>(cur_rwnd - t.last_advertised_rwnd) >= kTcpRwndUpdateThresholdSegments)) {
                    t.ack_needed = true;
                }
            }

            // Out-of-order segments that became in-order while the receive buffer was full
            if (!t.out_of_order_segments.empty() && drainOutOfOrderSegments(t, now_us)) {
                t.ack_needed = true;
            }

            if (t.remote_fin && t.to_local_buf.empty() && !t.local_shutdown_sent) {
                // Peer closed its send direction: propagate EOF to the app via a
                // half-close so the app can still send its own data back.
                t.local_sock.shutdownSend();
                t.local_shutdown_sent = true;
            }

            size_t burst_sent = 0;
            size_t inflight = t.inFlightSegments();
            size_t eff_wnd = t.effectiveSendWindow();
            if (t.peer_recv_window == 0 && t.handshake_done &&
                (now_us > t.last_wnd_probe_us &&
                 (now_us - t.last_wnd_probe_us) > kTcpZeroWindowProbeIntervalUs)) {
                // Window is closed: probe to elicit a window update. Re-send the oldest
                // unacknowledged segment if there is one (the receiver drops data beyond its
                // buffer, so new data would only pile up here); otherwise send exactly one new
                // segment beyond the window. Probes do not count toward the retransmit limit.
                t.last_wnd_probe_us = now_us;
                if (t.unacked_segments.empty()) {
                    eff_wnd = inflight + 1;
                } else {
                    TcpUnackedSegment& probe = t.unacked_segments.front();
                    probe.last_sent_us = now_us;
                    sendTcpFrame(t, probe.flags, probe.seq_num,
                                 probe.payload.empty() ? nullptr : probe.payload.data(),
                                 static_cast<uint16_t>(probe.payload.size()));
                }
            }

            bool sack_hole = t.hasSackHole();
            while (t.local_sock.isValid() && !t.local_fin &&
                   inflight < eff_wnd &&
                   t.unacked_segments.size() < kMaxTcpWindowSegments &&
                   (!sack_hole || (t.next_send_seq - t.last_rx_ack_num) < 64) &&
                   burst_sent < max_burst_segments) {
                if (bw_fluct && bw_tokens_bytes_ < static_cast<double>(kMaxTcpSegmentPayload)) {
                    break; // Paced by fluctuating link bandwidth token bucket
                }

                bool would_block = false;
                int got = t.local_sock.recvNonBlocking(buf, sizeof(buf), would_block);
                if (got > 0) {
                    if (t.pid != 0) {
                        auto& pt = proc_telemetry_[t.pid];
                        pt.tx_packets++;
                        pt.tx_bytes += static_cast<uint64_t>(got);
                        pt.last_activity_us = now_us;
                    }
                    uint32_t seq = t.next_send_seq++;
                    uint8_t flags = static_cast<uint8_t>(VpnTcpFlags::Data) | static_cast<uint8_t>(VpnTcpFlags::Ack);

                    TcpUnackedSegment seg{};
                    seg.seq_num = seq;
                    seg.flags = flags;
                    seg.payload.assign(buf, buf + got);
                    seg.first_sent_us = now_us;
                    seg.last_sent_us = now_us;
                    seg.send_count = 1;
                    t.unacked_segments.push_back(std::move(seg));
                    t.noteSegmentQueued();
                    inflight++;

                    if (bw_fluct) {
                        double prev_tokens = bw_tokens_bytes_;
                        bw_tokens_bytes_ -= static_cast<double>(got);
                        double thresh = static_cast<double>(kMaxTcpSegmentPayload * 5);
                        if (prev_tokens >= thresh && bw_tokens_bytes_ < thresh &&
                            t.cwnd_segments > 20.0 && bw_tail_drop_budget_ == 0) {
                            // Early congestion drop (RED-like) with 4 segments remaining in bucket so SACK triggers immediately
                            bw_tail_drop_budget_ = 1;
                        }
                    }

                    bool drop_simulated = false;
                    if (bw_tail_drop_budget_ > 0) {
                        bw_tail_drop_budget_--;
                        drop_simulated = true;
                    } else if (loss_pct > 0) {
                        uint32_t x = sim_prng_state_;
                        x ^= x << 13;
                        x ^= x >> 17;
                        x ^= x << 5;
                        sim_prng_state_ = x;
                        if ((x % 100u) < loss_pct) {
                            drop_simulated = true;
                        }
                    }

                    if (drop_simulated) {
                        tcp_simulated_drops_++;
                    } else {
                        sendTcpFrame(t, flags, seq, buf, static_cast<uint16_t>(got));
                    }
                    burst_sent++;
                } else if (got == 0 && !would_block) {
                    // Clean EOF: graceful half-close (shutdown(SD_SEND) from the app).
                    t.local_fin = true;
                    uint32_t seq = t.next_send_seq++;
                    uint8_t flags = static_cast<uint8_t>(VpnTcpFlags::Fin) | static_cast<uint8_t>(VpnTcpFlags::Ack);

                    TcpUnackedSegment seg{};
                    seg.seq_num = seq;
                    seg.flags = flags;
                    seg.first_sent_us = now_us;
                    seg.last_sent_us = now_us;
                    seg.send_count = 1;
                    t.unacked_segments.push_back(std::move(seg));
                    t.noteSegmentQueued();

                    sendTcpFrame(t, flags, seq, nullptr, 0);
                    // Keep the local loopback socket open: a half-close (shutdown(SD_SEND))
                    // must still allow the peer's data to be written back to the app. The
                    // socket is closed once the remote FIN arrives and the buffer drains.
                    break;
                } else if (got < 0) {
                    // Hard error on the local socket: abort the virtual connection with RST
                    // rather than presenting it to the peer as a graceful close.
                    abortTcpTunnel(t, true);
                    break;
                } else {
                    break;
                }
            }
        }

        bool rto_triggered = false;
        bool data_rto_triggered = false;
        size_t rto_rexmit_this_tick = 0;
        constexpr size_t kMaxRtoRetransmitsPerTick = 16;
        for (auto seg_it = t.unacked_segments.begin(); seg_it != t.unacked_segments.end(); ) {
            if (seg_it->sacked) {
                ++seg_it;
                continue;
            }
            if (seg_it->send_count > kMaxTcpSegmentRetransmits && t.peer_recv_window != 0) {
                // The peer has stopped acknowledging: abort (RST) instead of pretending it
                // closed gracefully. abortTcpTunnel() clears unacked_segments, so stop iterating.
                logWarn("Virtual TCP tunnel " + std::to_string(t.conn_id) + " to " +
                        ipv4ToString(t.remote_vip) + ":" + std::to_string(t.remote_vport) +
                        " aborted after " + std::to_string(kMaxTcpSegmentRetransmits) +
                        " retransmissions");
                abortTcpTunnel(t, true);
                break;
            }
            uint32_t shift = (seg_it->send_count > 1) ? std::min<uint32_t>(seg_it->send_count - 1, 4) : 0;
            uint64_t seg_timeout_us = std::min<uint64_t>(2'000'000ULL, t.rto_us << shift);
            if (now_us > seg_it->last_sent_us && (now_us - seg_it->last_sent_us) > seg_timeout_us) {
                if (rto_rexmit_this_tick >= kMaxRtoRetransmitsPerTick) {
                    // Defer remaining expired segments slightly so we don't flood the link with a whole window at once
                    seg_it->last_sent_us = now_us;
                    ++seg_it;
                    continue;
                }
                seg_it->last_sent_us = now_us;
                seg_it->send_count++;
                tcp_rto_retransmits_++;
                rto_rexmit_this_tick++;
                rto_triggered = true;
                if (seg_it->flags & static_cast<uint8_t>(VpnTcpFlags::Data)) {
                    data_rto_triggered = true;
                }

                bool use_relay = (seg_it->send_count >= 2);
                if (!use_relay) {
                    auto peer_it = peers_.find(t.remote_vip);
                    if (peer_it != peers_.end() && peer_it->second.route_mode == RouteMode::DirectP2P) {
                        if (now_us > peer_it->second.last_direct_recv_us &&
                            (now_us - peer_it->second.last_direct_recv_us) > 1'000'000ULL) {
                            use_relay = true;
                        }
                    }
                }

                sendTcpFrame(t, seg_it->flags, seg_it->seq_num,
                             seg_it->payload.empty() ? nullptr : seg_it->payload.data(),
                             static_cast<uint16_t>(seg_it->payload.size()),
                             use_relay);
            }
            ++seg_it;
        }

        if (rto_triggered && (now_us - t.last_cwnd_drop_us) > t.rto_us) {
            t.ssthresh_segments = std::max(16.0, t.cwnd_segments * 0.5);
            t.cwnd_segments = std::max(16.0, t.ssthresh_segments * 0.5);
            t.last_cwnd_drop_us = now_us;
            t.rto_us = std::min<uint64_t>(2'000'000ULL, t.rto_us + t.rto_us / 2);
            if (data_rto_triggered) {
                t.consecutive_rto_count++;
            }
            tcp_cwnd_scale_downs_++;

            // If Direct P2P has never delivered a direct data frame and repeatedly RTOs while Server Relay delivers them,
            // automatically switch peer route to Server Relay so bulk transfers don't stall on every window
            if (!t.direct_data_acked && t.consecutive_rto_count >= 3 &&
                loss_pct == 0 && !bw_fluct && !force_relay_.load()) {
                auto peer_it = peers_.find(t.remote_vip);
                if (peer_it != peers_.end() && peer_it->second.route_mode == RouteMode::DirectP2P) {
                    peer_it->second.route_mode = RouteMode::ServerRelay;
                    peer_it->second.active_endpoint = {};
                    peer_it->second.punch_gave_up = true;
                    peer_it->second.punch_gave_up_us = now_us;
                    logWarn("Direct P2P data path blackholed for " + ipv4ToString(t.remote_vip) +
                            ", switching to Server Relay for full throughput");
                }
            }
        }

        if ((t.local_fin && t.remote_fin && t.to_local_buf.empty() && t.unacked_segments.empty()) ||
            t.handshake_failed) {
            it = reapTunnel(it);
        } else {
            ++it;
        }
    }
}


} // namespace apivpn::client
