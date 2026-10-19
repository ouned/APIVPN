#pragma once

#include "apivpn/common/byte_buffer.hpp"
#include "apivpn/common/protocol.hpp"
#include "apivpn/platform/net_socket.hpp"

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

namespace apivpn::server {

// Integer token bucket (no floating point): level_scaled holds tokens * 1'000'000 and refills at
// rate_per_s tokens per second up to burst tokens. A fresh bucket starts full.
struct TokenBucket {
    uint64_t level_scaled = 0;
    uint64_t last_us      = 0;
    bool     primed       = false;

    bool take(uint64_t now_us, uint64_t rate_per_s, uint64_t burst, uint64_t cost) {
        const uint64_t cap = burst * 1'000'000ULL;
        if (!primed) {
            level_scaled = cap;
            last_us = now_us;
            primed = true;
        } else if (now_us > last_us) {
            // Clamp the idle gap so the multiplication cannot overflow; 60 s refills any bucket used here.
            const uint64_t elapsed = std::min<uint64_t>(now_us - last_us, 60'000'000ULL);
            level_scaled = std::min<uint64_t>(cap, level_scaled + elapsed * rate_per_s);
            last_us = now_us;
        }
        const uint64_t need = cost * 1'000'000ULL;
        if (level_scaled < need) return false;
        level_scaled -= need;
        return true;
    }
};

// Source address of a peer with the port ignored, used to key per-IP limits.
// IPv4 addresses are stored IPv4-mapped (::ffff:a.b.c.d).
struct AddrKey {
    uint64_t hi = 0;
    uint64_t lo = 0;
    bool operator==(const AddrKey& o) const { return hi == o.hi && lo == o.lo; }
};

struct AddrKeyHash {
    size_t operator()(const AddrKey& k) const noexcept {
        return std::hash<uint64_t>{}(k.hi ^ (k.lo * 0x9E3779B97F4A7C15ULL));
    }
};

// A UDP source endpoint observed via UdpRegister (the client's NAT mapping).
struct ObservedEndpoint {
    platform::Endpoint ep;
    uint64_t           last_seen_us = 0;
};

// Rate limiter for a repeated log warning (see allowLog in vpn_server.cpp).
struct LogLimiter {
    uint64_t last_us    = 0;
    uint64_t suppressed = 0;
};

struct ClientSession {
    uint32_t                        vip                = 0;
    uint64_t                        session_token      = 0;
    std::string                     nickname;
    platform::NetSocket             tcp_sock;
    platform::Endpoint              tcp_peer_ep;
    uint16_t                        declared_udp_port    = 0;
    uint16_t                        declared_udp_port_v6 = 0;
    // Endpoints from ClientHello (observed TCP address + declared UDP port, then the client's
    // self-reported LAN/loopback endpoints). Kept for the lifetime of the session.
    std::vector<platform::Endpoint> reported_endpoints;
    // UDP-observed NAT endpoints, newest first; dropped when not refreshed for a while.
    std::vector<ObservedEndpoint>   observed_endpoints;
    // Merged list published to peers (see VpnServer::rebuildEndpoints).
    std::vector<platform::Endpoint> endpoints;
    ByteBuffer                      recv_buf;
    ByteBuffer                      send_buf;
    bool                            authenticated      = false;
    // Set when the session must be dropped (e.g. its send queue overflowed). Sessions are only
    // erased at the end of a tick, so code iterating sessions_ just skips closing ones.
    bool                            closing            = false;
    uint64_t                        connected_us       = 0;
    uint64_t                        last_active_us     = 0;
    // Start of the current period in which send_buf was non-empty but nothing could be sent.
    uint64_t                        send_stall_since_us = 0;
    TokenBucket                     broadcast_bucket;  // relayed broadcast/multicast bytes
    TokenBucket                     chat_bucket;       // chat bytes fanned out to all peers
    // Index of this session's entry in the current poll list (SIZE_MAX = none),
    // so the tick does not have to search the poll array for every session.
    size_t                          poll_index         = static_cast<size_t>(-1);
};

// Upper bound on a single session's pending relay queue. Beyond this the oldest
// behaviour (unbounded growth) could let one slow peer exhaust server memory;
// dropping a relayed frame is safe because virtual TCP retransmits it.
constexpr size_t kMaxSessionSendBufBytes = 8 * 1024 * 1024;

class VpnServer {
public:
    VpnServer();
    ~VpnServer();

    bool start(uint16_t port = kDefaultServerPort);
    void stop();

    bool isRunning() const { return running_.load(); }
    uint16_t tcpPort() const { return tcp_port_; }
    uint16_t udpPort() const { return udp_port_; }
    bool hasIpv6() const { return tcp_listener_v6_.isValid(); }
    size_t connectedClientCount() const;

    void setPeerTimeoutUs(uint64_t timeout_us) { peer_timeout_us_.store(timeout_us); }
    uint64_t peerTimeoutUs() const { return peer_timeout_us_.load(); }

    // Cap the number of concurrently authenticated clients (for capacity tests)
    void setMaxClients(size_t max_clients) { max_clients_.store(max_clients); }
    size_t maxClients() const { return max_clients_.load(); }

    // Per-source-IP caps on concurrent TCP connections and on authenticated clients.
    // Loopback peers are exempt; 0 disables a cap.
    void setMaxConnectionsPerIp(size_t max_conns) { max_conns_per_ip_.store(max_conns); }
    void setMaxClientsPerIp(size_t max_clients) { max_clients_per_ip_.store(max_clients); }

private:
    void workerLoop();
    unsigned bindTcpListeners(uint16_t port, bool protect);
    unsigned bindUdpSockets(uint16_t port, bool protect);
    void handleNewTcpConnection(platform::NetSocket& listener, uint64_t now_us);
    void handleClientReadable(ClientSession& session, bool& out_disconnect);
    void processControlMessage(ClientSession& session, ControlMsgType type, const uint8_t* payload, size_t len);
    void handleUdpReadable(platform::NetSocket& sock, uint64_t now_us);
    bool allowUdpRegister(const platform::Endpoint& src, uint64_t now_us);
    void housekeeping(uint64_t now_us);

    void resetVipPool();
    uint32_t allocateVip(uint32_t preferred_vip, uint64_t prev_token, uint64_t now_us);
    void releaseVip(uint32_t vip, uint64_t reserve_for_token, uint64_t now_us);
    uint64_t newSessionToken() const;
    uint64_t pairKey(uint32_t vip_a, uint32_t vip_b);
    void erasePairKeys(uint32_t vip);
    void broadcastPeerList();
    void queueControlMessage(ClientSession& session, ControlMsgType type, const void* payload, size_t len);
    static bool addUniqueEndpoint(std::vector<platform::Endpoint>& list, const platform::Endpoint& ep);
    static bool noteObservedEndpoint(ClientSession& session, const platform::Endpoint& ep, uint64_t now_us);
    static bool rebuildEndpoints(ClientSession& session);

    std::atomic<bool> running_{false};
    std::atomic<uint64_t> peer_timeout_us_{kServerPeerTimeoutUs};
    std::atomic<size_t> max_clients_{253};
    std::atomic<size_t> max_conns_per_ip_{24};
    std::atomic<size_t> max_clients_per_ip_{16};
    uint16_t tcp_port_ = 0;
    uint16_t udp_port_ = 0;

    platform::NetSocket tcp_listener_v6_;
    platform::NetSocket tcp_listener_;
    platform::NetSocket udp_sock_v6_;
    platform::NetSocket udp_sock_;
    std::thread worker_thread_;

    mutable std::mutex mutex_;
    std::vector<std::unique_ptr<ClientSession>> sessions_;
    // vip -> session, so relayed unicast packets do not require a linear scan of
    // all sessions. Pointers stay valid because sessions_ owns them by value
    // (unique_ptr) and erase only moves the pointers themselves.
    std::unordered_map<uint32_t, ClientSession*> sessions_by_vip_;
    bool vip_in_use_[256] = {};

    // A disconnected client's VIP is held for its session token for a while, so only that
    // client (reconnecting with prev_session_token) can take it back. token 0 = not reserved.
    struct VipReservation {
        uint64_t token      = 0;
        uint64_t expires_us = 0;
    };
    VipReservation vip_reservations_[256] = {};

    // Secret per unordered VIP pair ((lo << 32) | hi), created lazily and handed only to the two
    // peers of the pair in their personalized peer lists. Erased when a VIP gets a new holder.
    std::unordered_map<uint64_t, uint64_t> pair_keys_;

    // Per-source-IP UdpRegister rate limiting.
    std::unordered_map<AddrKey, TokenBucket, AddrKeyHash> udp_reg_buckets_;

    // Peer-list changes are coalesced and broadcast at most once per kPeerListMinIntervalUs.
    bool peer_list_dirty_ = false;
    uint64_t last_peer_list_us_ = 0;
    uint64_t last_housekeeping_us_ = 0;
    // While set, the listeners are not polled (accept() was failing, e.g. out of descriptors).
    uint64_t accept_backoff_until_us_ = 0;

    LogLimiter log_capacity_;
    LogLimiter log_conn_limit_;
    LogLimiter log_overflow_;
    LogLimiter log_flood_;
};

} // namespace apivpn::server
