#include "apivpn/client/vpn_client.hpp"
#include "apivpn/common/logger.hpp"

#include <algorithm>
#include <chrono>
#include <cstring>
#include <memory>
#include <thread>
#include <vector>

// Purpose: Local loopback UDP gateway bridging hooked sockets to the VPN.

namespace apivpn::client {

void VpnClient::handleLocalUdpGatewayReadable() {
    uint8_t buf[65536];
    // Bounded per tick so a datagram flood cannot starve the rest of the loop.
    for (int budget = kMaxDatagramsPerTick; budget > 0; --budget) {
        platform::Endpoint src_ep{};
        bool would_block = false;
        int got = udp_gateway_sock_.recvFrom(buf, sizeof(buf), src_ep, would_block);
        if (got < 0) break;                 // real socket error
        if (got == 0) {
            if (would_block) break;         // drained
            continue;                       // zero-length datagram: skip, keep the batch going
        }

        if (static_cast<size_t>(got) < sizeof(LoopbackUdpHeader)) continue;
        LoopbackUdpHeader lhdr{};
        std::memcpy(&lhdr, buf, sizeof(lhdr));
        if (lhdr.magic != kLoopbackUdpMagic) continue;

        const uint8_t* payload = buf + sizeof(LoopbackUdpHeader);
        size_t payload_len = static_cast<size_t>(got) - sizeof(LoopbackUdpHeader);

        uint64_t now_us = platform::monotonicMicros();
        for (const auto& b : bound_sockets_) {
            if (b.proto == IpcProto::Udp &&
                (b.actual_port == src_ep.port_host || b.virtual_port == lhdr.src_vport)) {
                if (b.pid != 0) {
                    auto& pt = proc_telemetry_[b.pid];
                    pt.tx_packets++;
                    pt.tx_bytes += payload_len;
                    pt.last_activity_us = now_us;
                }
                break;
            }
        }

        uint32_t my_vip = local_vip_.load();
        if (my_vip == 0) my_vip = lhdr.src_vip;

        if (lhdr.dst_vip == my_vip) {
            deliverUdpToLocalSockets(my_vip, lhdr.dst_vip, lhdr.src_vport, lhdr.dst_vport, payload, payload_len);
            continue;
        }

        VpnUdpDataHeader vhdr{};
        vhdr.src_vip = my_vip;
        vhdr.dst_vip = lhdr.dst_vip;
        vhdr.src_vport = lhdr.src_vport;
        vhdr.dst_vport = lhdr.dst_vport;
        vhdr.payload_len = static_cast<uint16_t>(payload_len);

        uint8_t stack_frame[2048];
        const size_t frame_len = sizeof(vhdr) + payload_len;
        if (frame_len <= sizeof(stack_frame)) {
            std::memcpy(stack_frame, &vhdr, sizeof(vhdr));
            if (payload_len > 0) {
                std::memcpy(stack_frame + sizeof(vhdr), payload, payload_len);
            }
            sendVpnFrameToTarget(lhdr.dst_vip, UdpPacketType::VpnUdpData, stack_frame, frame_len);
        } else {
            ByteBuffer frame(frame_len);
            frame.appendPod(vhdr);
            frame.append(payload, payload_len);
            sendVpnFrameToTarget(lhdr.dst_vip, UdpPacketType::VpnUdpData, frame.data(), frame.size());
        }
    }
}

void VpnClient::deliverUdpToLocalSockets(
    uint32_t src_vip,
    uint32_t dst_vip,
    uint16_t src_vport,
    uint16_t dst_vport,
    const uint8_t* payload,
    size_t len) {

    LoopbackUdpHeader lhdr{};
    lhdr.magic = kLoopbackUdpMagic;
    lhdr.src_vip = src_vip;
    lhdr.dst_vip = dst_vip;
    lhdr.src_vport = src_vport;
    lhdr.dst_vport = dst_vport;

    uint64_t now_us = platform::monotonicMicros();
    uint8_t stack_pkt[2048];
    const size_t pkt_len = sizeof(lhdr) + len;
    if (pkt_len <= sizeof(stack_pkt)) {
        std::memcpy(stack_pkt, &lhdr, sizeof(lhdr));
        if (len > 0 && payload) {
            std::memcpy(stack_pkt + sizeof(lhdr), payload, len);
        }
        for (const auto& b : bound_sockets_) {
            if (b.proto == IpcProto::Udp && b.virtual_port == dst_vport) {
                udp_gateway_sock_.sendTo(stack_pkt, pkt_len, {kLoopbackIp, b.actual_port});
                if (b.pid != 0) {
                    auto& pt = proc_telemetry_[b.pid];
                    pt.rx_packets++;
                    pt.rx_bytes += len;
                    pt.last_activity_us = now_us;
                }
            }
        }
    } else {
        ByteBuffer pkt(pkt_len);
        pkt.appendPod(lhdr);
        pkt.append(payload, len);

        for (const auto& b : bound_sockets_) {
            if (b.proto == IpcProto::Udp && b.virtual_port == dst_vport) {
                udp_gateway_sock_.sendTo(pkt.data(), pkt.size(), {kLoopbackIp, b.actual_port});
                if (b.pid != 0) {
                    auto& pt = proc_telemetry_[b.pid];
                    pt.rx_packets++;
                    pt.rx_bytes += len;
                    pt.last_activity_us = now_us;
                }
            }
        }
    }
}


} // namespace apivpn::client
