#include "apivpn/server/vpn_server.hpp"
#include "apivpn/common/logger.hpp"

#include <algorithm>
#include <cstring>

namespace apivpn::server {

namespace {

// Extra room above kMaxSessionSendBufBytes reserved for control traffic (welcome, peer lists,
// chat, pongs). Relayed frames stop at kMaxSessionSendBufBytes and are dropped; a control frame
// that would push the queue past the hard cap means the client is not reading its socket.
constexpr size_t   kSessionControlHeadroomBytes = 2 * 1024 * 1024;
constexpr size_t   kMaxSessionQueueHardBytes    = kMaxSessionSendBufBytes + kSessionControlHeadroomBytes;

constexpr uint64_t kPeerListMinIntervalUs   = 250'000ULL;    // coalesced peer-list broadcasts
constexpr uint64_t kObservedEndpointTtlUs   = 60'000'000ULL; // UDP-observed endpoint expiry
constexpr uint64_t kVipReservationUs        = 60'000'000ULL; // VIP held for a reconnecting client
constexpr uint64_t kHelloDeadlineUs         = 10'000'000ULL; // new connection must authenticate
constexpr uint64_t kAcceptBackoffUs         = 100'000ULL;    // listener pause after accept() fails
constexpr uint64_t kHousekeepingIntervalUs  = 1'000'000ULL;
constexpr uint64_t kLogRateLimitUs          = 10'000'000ULL; // repeated warnings: one per 10 s
constexpr size_t   kUnauthenticatedHeadroom = 64;            // sessions allowed beyond max_clients_
constexpr int      kMaxAcceptsPerTick       = 64;

constexpr uint64_t kUdpRegRatePerSec     = 20;
constexpr uint64_t kUdpRegBurst          = 40;
constexpr size_t   kMaxTrackedUdpSources = 4096;
constexpr uint64_t kUdpSourceIdleUs      = 10'000'000ULL;

constexpr uint64_t kBroadcastRelayBytesPerSec = 512 * 1024;
constexpr uint64_t kBroadcastRelayBurstBytes  = 1024 * 1024;
constexpr uint64_t kChatBytesPerSec           = 32 * 1024;
constexpr uint64_t kChatBurstBytes            = 128 * 1024;

constexpr unsigned kFamilyV6 = 1u;
constexpr unsigned kFamilyV4 = 2u;

unsigned familyCount(unsigned mask) {
    return ((mask & kFamilyV6) ? 1u : 0u) + ((mask & kFamilyV4) ? 1u : 0u);
}

std::string familyString(unsigned mask) {
    if (mask == (kFamilyV6 | kFamilyV4)) return "IPv6 + IPv4";
    if (mask & kFamilyV6) return "IPv6 only";
    if (mask & kFamilyV4) return "IPv4 only";
    return "none";
}

// Make a server socket non-shareable before bind. On Windows NetSocket::setReuseAddr maps to
// SO_EXCLUSIVEADDRUSE (no other socket may bind the port); on POSIX it is SO_REUSEADDR, which a
// TCP listener needs to rebind over TIME_WAIT but which would let another socket share a UDP
// port, so UDP sockets skip it there.
void protectServerSocket(platform::NetSocket& sock, bool is_udp) {
    if (!sock.isValid()) return;
#ifdef _WIN32
    (void)is_udp;
    sock.setReuseAddr(true);
#else
    if (!is_udp) sock.setReuseAddr(true);
#endif
}

AddrKey addrKeyOf(const platform::Endpoint& ep) {
    AddrKey k{};
    if (ep.isIpv6()) {
        for (size_t i = 0; i < 8; ++i) {
            k.hi = (k.hi << 8) | ep.ipv6[i];
            k.lo = (k.lo << 8) | ep.ipv6[i + 8];
        }
    } else {
        k.lo = 0x0000FFFF00000000ULL | ep.ip_host;
    }
    return k;
}

// Replace ASCII control characters (including DEL) so a nickname cannot inject terminal escapes
// or fake log lines; bytes >= 0x80 are kept so UTF-8 names survive.
std::string sanitizeNickname(const char* raw, size_t max_len) {
    std::string out;
    for (size_t i = 0; i < max_len && raw[i] != '\0'; ++i) {
        const auto c = static_cast<unsigned char>(raw[i]);
        out.push_back((c < 0x20 || c == 0x7F) ? '?' : raw[i]);
    }
    return out;
}

// Rate-limits a repeated warning: returns false while suppressed. When it fires again,
// out_suffix reports how many were suppressed in between.
bool allowLog(LogLimiter& limiter, uint64_t now_us, std::string& out_suffix) {
    if (limiter.last_us != 0 && now_us - limiter.last_us < kLogRateLimitUs) {
        ++limiter.suppressed;
        return false;
    }
    out_suffix = (limiter.suppressed != 0)
        ?(" (" + std::to_string(limiter.suppressed) + " similar suppressed)")
        : std::string();
    limiter.last_us = now_us;
    limiter.suppressed = 0;
    return true;
}

} // namespace

VpnServer::VpnServer() {
    resetVipPool();
}

VpnServer::~VpnServer() {
    stop();
}

void VpnServer::resetVipPool() {
    std::memset(vip_in_use_, 0, sizeof(vip_in_use_));
    vip_in_use_[0] = true;   // 10.42.0.0 network
    vip_in_use_[1] = true;   // 10.42.0.1 server
    vip_in_use_[255] = true; // 10.42.0.255 broadcast
    for (auto& r : vip_reservations_) {
        r = VipReservation{};
    }
}

// Binds the IPv6 ([::]) and IPv4 (0.0.0.0) TCP listeners to `port` (0 = the first successful
// bind picks a port and the other family reuses it). Returns a kFamily* mask of the listeners
// that are up; failed ones are closed. Sets tcp_port_ when at least one is up.
unsigned VpnServer::bindTcpListeners(uint16_t port, bool protect) {
    tcp_listener_v6_ = platform::NetSocket::createTcp(platform::IpVersion::V6);
    tcp_listener_ = platform::NetSocket::createTcp(platform::IpVersion::V4);
    if (protect) {
        protectServerSocket(tcp_listener_v6_, false);
        protectServerSocket(tcp_listener_, false);
    }

    unsigned mask = 0;
    uint16_t candidate_port = port;
    if (tcp_listener_v6_.isValid()) {
        tcp_listener_v6_.setNonBlocking(true);
        platform::Endpoint ep6{};
        if (tcp_listener_v6_.bind(platform::Endpoint::anyV6(candidate_port)) &&
            tcp_listener_v6_.listen(64) && tcp_listener_v6_.getLocalEndpoint(ep6)) {
            candidate_port = ep6.port_host;
            mask |= kFamilyV6;
        } else {
            tcp_listener_v6_.close();
        }
    }

    if (tcp_listener_.isValid()) {
        tcp_listener_.setNonBlocking(true);
        platform::Endpoint ep4{};
        if (tcp_listener_.bind(platform::Endpoint::anyV4(candidate_port)) &&
            tcp_listener_.listen(64) && tcp_listener_.getLocalEndpoint(ep4)) {
            candidate_port = ep4.port_host;
            mask |= kFamilyV4;
        } else {
            tcp_listener_.close();
        }
    }

    if (mask != 0) tcp_port_ = candidate_port;
    return mask;
}

// UDP counterpart of bindTcpListeners; sets udp_port_ when at least one socket is up.
unsigned VpnServer::bindUdpSockets(uint16_t port, bool protect) {
    udp_sock_v6_ = platform::NetSocket::createUdp(platform::IpVersion::V6);
    udp_sock_ = platform::NetSocket::createUdp(platform::IpVersion::V4);
    if (protect) {
        protectServerSocket(udp_sock_v6_, true);
        protectServerSocket(udp_sock_, true);
    }

    unsigned mask = 0;
    uint16_t candidate_port = port;
    if (udp_sock_v6_.isValid()) {
        udp_sock_v6_.setNonBlocking(true);
        udp_sock_v6_.setRecvBufferSize(4 * 1024 * 1024);
        udp_sock_v6_.setSendBufferSize(4 * 1024 * 1024);
        platform::Endpoint ep6{};
        if (udp_sock_v6_.bind(platform::Endpoint::anyV6(candidate_port)) &&
            udp_sock_v6_.getLocalEndpoint(ep6)) {
            candidate_port = ep6.port_host;
            mask |= kFamilyV6;
        } else {
            udp_sock_v6_.close();
        }
    }

    if (udp_sock_.isValid()) {
        udp_sock_.setNonBlocking(true);
        udp_sock_.setRecvBufferSize(4 * 1024 * 1024);
        udp_sock_.setSendBufferSize(4 * 1024 * 1024);
        platform::Endpoint ep4{};
        if (udp_sock_.bind(platform::Endpoint::anyV4(candidate_port)) &&
            udp_sock_.getLocalEndpoint(ep4)) {
            candidate_port = ep4.port_host;
            mask |= kFamilyV4;
        } else {
            udp_sock_.close();
        }
    }

    if (mask != 0) udp_port_ = candidate_port;
    return mask;
}

bool VpnServer::start(uint16_t port) {
    if (running_.load()) return true;

    tcp_port_ = 0;
    udp_port_ = 0;

    // TCP listeners on both IPv6 and IPv4, on the same port.
    unsigned tcp_mask = 0;
    if (port != 0) {
        // Exclusive bind first. Windows refuses an exclusive rebind while connections accepted by a
        // previous instance are still winding down, so retry once with default options and keep
        // whichever attempt brought up more address families.
        tcp_mask = bindTcpListeners(port, true);
        if (tcp_mask != (kFamilyV6 | kFamilyV4)) {
            const unsigned plain_mask = bindTcpListeners(port, false);
            tcp_mask = (familyCount(plain_mask) < familyCount(tcp_mask))
                ? bindTcpListeners(port, true)
                : plain_mask;
        }
    } else {
        // Ephemeral port: retry until both families share one port. IPv4-only is accepted when the
        // host has no usable IPv6. The later attempts drop the exclusive option in case the
        // platform refuses it.
        for (int attempt = 0; attempt < 16; ++attempt) {
            tcp_mask = bindTcpListeners(0, attempt < 8);
            if (tcp_mask == (kFamilyV6 | kFamilyV4) || tcp_mask == kFamilyV4) break;
            tcp_mask = 0;
        }
    }

    if (tcp_mask == 0 || tcp_port_ == 0) {
        tcp_listener_v6_.close();
        tcp_listener_.close();
        logError("Could not bind a TCP listener on port " + std::to_string(port));
        return false;
    }

    // UDP sockets on the TCP port if possible (otherwise an ephemeral one, announced in the
    // welcome), covering every address family that has a TCP listener.
    unsigned udp_mask = 0;
    bool udp_bound = false;
    for (int attempt = 0; attempt < 16 && !udp_bound; ++attempt) {
        const uint16_t target_udp_port = (attempt == 0) ? tcp_port_ : 0;
        udp_mask = bindUdpSockets(target_udp_port, attempt < 8);
        if (udp_mask != 0 && (udp_mask & tcp_mask) == tcp_mask) {
            udp_bound = true;
        } else {
            udp_sock_v6_.close();
            udp_sock_.close();
        }
    }

    if (!udp_bound || udp_port_ == 0) {
        udp_sock_v6_.close();
        udp_sock_.close();
        tcp_listener_v6_.close();
        tcp_listener_.close();
        logError("Could not bind the UDP sockets for TCP port " + std::to_string(tcp_port_));
        return false;
    }

    if (port != 0 && tcp_mask != (kFamilyV6 | kFamilyV4)) {
        logWarn(std::string("Could not bind the ") + ((tcp_mask & kFamilyV6) ? "IPv4" : "IPv6") +
                " listener on port " + std::to_string(port) + "; serving " + familyString(tcp_mask) +
                ". Clients using the other address family cannot connect.");
    }

    running_.store(true);
    worker_thread_ = std::thread(&VpnServer::workerLoop, this);
    logInfo("VPN Server started on TCP:" + std::to_string(tcp_port_) +
            " UDP:" + std::to_string(udp_port_) +
            " (" + familyString(tcp_mask) + ")");
    return true;
}

void VpnServer::stop() {
    if (!running_.exchange(false)) return;
    if (worker_thread_.joinable()) {
        worker_thread_.join();
    }
    std::lock_guard<std::mutex> lock(mutex_);
    sessions_.clear();
    sessions_by_vip_.clear();
    resetVipPool();
    pair_keys_.clear();
    udp_reg_buckets_.clear();
    peer_list_dirty_ = false;
    last_peer_list_us_ = 0;
    last_housekeeping_us_ = 0;
    accept_backoff_until_us_ = 0;
    tcp_listener_v6_.close();
    tcp_listener_.close();
    udp_sock_v6_.close();
    udp_sock_.close();
}

size_t VpnServer::connectedClientCount() const {
    std::lock_guard<std::mutex> lock(mutex_);
    size_t count = 0;
    for (const auto& s : sessions_) {
        if (s->authenticated) ++count;
    }
    return count;
}

uint32_t VpnServer::allocateVip(uint32_t preferred_vip, uint64_t prev_token, uint64_t now_us) {
    // A reconnecting client whose previous session still holds a reservation gets that VIP back,
    // keeping its pair keys.
    if (prev_token != 0) {
        for (int i = 2; i <= 254; ++i) {
            if (!vip_in_use_[i] && vip_reservations_[i].token == prev_token &&
                vip_reservations_[i].expires_us > now_us) {
                vip_reservations_[i] = VipReservation{};
                vip_in_use_[i] = true;
                return kVirtualSubnetBase | static_cast<uint32_t>(i);
            }
        }
    }

    auto is_free = [this, now_us](int id) -> bool {
        return !vip_in_use_[id] &&
               (vip_reservations_[id].token == 0 || vip_reservations_[id].expires_us <= now_us);
    };
    // A VIP going to a new holder gets fresh pair keys, so the new holder cannot use the
    // previous holder's direct-path credentials (and vice versa).
    auto take = [this](int id) -> uint32_t {
        vip_reservations_[id] = VipReservation{};
        vip_in_use_[id] = true;
        const uint32_t vip = kVirtualSubnetBase | static_cast<uint32_t>(id);
        erasePairKeys(vip);
        return vip;
    };

    if (isVirtualUnicast(preferred_vip)) {
        const int pref_id = static_cast<int>(preferred_vip & 0xFFu);
        if (pref_id >= 2 && pref_id <= 254 && is_free(pref_id)) {
            return take(pref_id);
        }
    }
    for (int i = 2; i <= 254; ++i) {
        if (is_free(i)) return take(i);
    }

    // Pool exhausted by reservations (e.g. connect/disconnect churn): rather than refusing new
    // clients, hand out the reservation that would expire first.
    int oldest = -1;
    for (int i = 2; i <= 254; ++i) {
        if (!vip_in_use_[i] && vip_reservations_[i].token != 0 &&
            (oldest < 0 || vip_reservations_[i].expires_us < vip_reservations_[oldest].expires_us)) {
            oldest = i;
        }
    }
    return (oldest >= 0) ? take(oldest) : 0;
}

void VpnServer::releaseVip(uint32_t vip, uint64_t reserve_for_token, uint64_t now_us) {
    if (!isVirtualUnicast(vip)) return;
    const uint8_t host_id = static_cast<uint8_t>(vip & 0xFFu);
    if (host_id < 2) return;
    vip_in_use_[host_id] = false;
    if (reserve_for_token != 0) {
        vip_reservations_[host_id] = VipReservation{reserve_for_token, now_us + kVipReservationUs};
    }
}

uint64_t VpnServer::newSessionToken() const {
    while (true) {
        const uint64_t token = platform::secureRandom64();
        bool in_use = false;
        for (const auto& s : sessions_) {
            if (s->session_token == token) {
                in_use = true;
                break;
            }
        }
        for (size_t i = 0; i < 256 && !in_use; ++i) {
            if (vip_reservations_[i].token == token) in_use = true;
        }
        if (!in_use) return token;
    }
}

uint64_t VpnServer::pairKey(uint32_t vip_a, uint32_t vip_b) {
    const uint32_t lo = std::min(vip_a, vip_b);
    const uint32_t hi = std::max(vip_a, vip_b);
    const uint64_t pair_id = (static_cast<uint64_t>(lo) << 32) | hi;
    auto it = pair_keys_.find(pair_id);
    if (it != pair_keys_.end()) return it->second;
    const uint64_t key = platform::secureRandom64();
    pair_keys_.emplace(pair_id, key);
    return key;
}

void VpnServer::erasePairKeys(uint32_t vip) {
    for (auto it = pair_keys_.begin(); it != pair_keys_.end();) {
        const auto lo = static_cast<uint32_t>(it->first >> 32);
        const auto hi = static_cast<uint32_t>(it->first & 0xFFFFFFFFu);
        if (lo == vip || hi == vip) {
            it = pair_keys_.erase(it);
        } else {
            ++it;
        }
    }
}

bool VpnServer::addUniqueEndpoint(std::vector<platform::Endpoint>& list, const platform::Endpoint& ep) {
    if (!ep.isValidAddress() || ep.port_host == 0) return false;
    if (list.size() >= 2 * kMaxEndpointsPerPeer) return false;
    if (std::find(list.begin(), list.end(), ep) != list.end()) return false;
    list.push_back(ep);
    return true;
}

// Records a UDP-observed endpoint. Returns true if it is new (a refresh only bumps its timestamp).
bool VpnServer::noteObservedEndpoint(ClientSession& session, const platform::Endpoint& ep, uint64_t now_us) {
    if (!ep.isValidAddress() || ep.port_host == 0) return false;
    for (auto& o : session.observed_endpoints) {
        if (o.ep == ep) {
            o.last_seen_us = now_us;
            return false;
        }
    }
    if (session.observed_endpoints.size() >= kMaxEndpointsPerPeer) {
        auto stalest = std::min_element(
            session.observed_endpoints.begin(), session.observed_endpoints.end(),
            [](const ObservedEndpoint& a, const ObservedEndpoint& b) { return a.last_seen_us < b.last_seen_us; });
        session.observed_endpoints.erase(stalest);
    }
    session.observed_endpoints.insert(session.observed_endpoints.begin(), ObservedEndpoint{ep, now_us});
    return true;
}

// Rebuilds the published endpoint list: IPv6 before IPv4 and, within a family, UDP-observed
// (NAT) endpoints, newest first, before ClientHello ones; capped at kMaxEndpointsPerPeer.
// Returns true if the published list changed.
bool VpnServer::rebuildEndpoints(ClientSession& session) {
    std::vector<platform::Endpoint> merged;
    merged.reserve(kMaxEndpointsPerPeer);
    auto add = [&merged](const platform::Endpoint& candidate) {
        if (merged.size() >= kMaxEndpointsPerPeer) return;
        if (std::find(merged.begin(), merged.end(), candidate) != merged.end()) return;
        merged.push_back(candidate);
    };
    for (int pass = 0; pass < 2; ++pass) {
        const bool want_v6 = (pass == 0);
        for (const auto& o : session.observed_endpoints) {
            if (o.ep.isIpv6() == want_v6) add(o.ep);
        }
        for (const auto& r : session.reported_endpoints) {
            if (r.isIpv6() == want_v6) add(r);
        }
    }
    if (merged == session.endpoints) return false;
    session.endpoints.swap(merged);
    return true;
}

void VpnServer::queueControlMessage(ClientSession& session, ControlMsgType type, const void* payload, size_t len) {
    if (session.closing) return;
    if (session.send_buf.size() + sizeof(ControlHeader) + len > kMaxSessionQueueHardBytes) {
        // The client is not draining its socket. Dropping a control frame would desync it (missed
        // peer list or welcome), so the session is closed instead; it is removed at the end of the tick.
        session.closing = true;
        std::string suffix;
        if (allowLog(log_overflow_, platform::monotonicMicros(), suffix)) {
            logWarn("Disconnecting client '" + (session.nickname.empty() ? "unauthenticated" : session.nickname) +
                    "' (" + session.tcp_peer_ep.toString() + "): send queue full, client is not reading" + suffix);
        }
        return;
    }
    ControlHeader hdr{};
    hdr.magic = kProtocolMagic;
    hdr.type = static_cast<uint8_t>(type);
    hdr.payload_len = static_cast<uint32_t>(len);
    session.send_buf.appendPod(hdr);
    if (len > 0 && payload) {
        session.send_buf.append(payload, len);
    }
}

void VpnServer::broadcastPeerList() {
    // Entries are built once; each recipient then gets its own copy carrying the pair key it
    // shares with each listed peer (0 for its own entry).
    std::vector<PeerEntryWire> entries;
    entries.reserve(sessions_.size());
    for (const auto& s : sessions_) {
        if (!s->authenticated || s->closing) continue;
        PeerEntryWire e{};
        e.vip = s->vip;
        std::strncpy(e.nickname, s->nickname.c_str(), kMaxNicknameLen - 1);
        e.endpoint_count = static_cast<uint8_t>(std::min(s->endpoints.size(), kMaxEndpointsPerPeer));
        for (size_t i = 0; i < e.endpoint_count; ++i) {
            e.endpoints[i] = platform::toEndpointWire(s->endpoints[i]);
        }
        e.pair_key = 0;
        entries.push_back(e);
    }

    MsgPeerListHeader hdr{};
    hdr.peer_count = static_cast<uint16_t>(entries.size());
    const size_t entries_bytes = entries.size() * sizeof(PeerEntryWire);

    ByteBuffer payload(sizeof(hdr) + entries_bytes);
    for (auto& r : sessions_) {
        if (!r->authenticated || r->closing) continue;
        for (auto& e : entries) {
            e.pair_key = (e.vip == r->vip) ? 0 : pairKey(r->vip, e.vip);
        }
        payload.clear();
        payload.appendPod(hdr);
        if (!entries.empty()) {
            payload.append(entries.data(), entries_bytes);
        }
        queueControlMessage(*r, ControlMsgType::PeerListUpdate, payload.data(), payload.size());
    }
}

void VpnServer::handleNewTcpConnection(platform::NetSocket& listener, uint64_t now_us) {
    if (!listener.isValid()) return;
    int handled = 0;
    for (; handled < kMaxAcceptsPerTick; ++handled) {
        platform::Endpoint peer_ep{};
        platform::NetSocket client_sock = listener.accept(&peer_ep);
        if (!client_sock.isValid()) break;

        // Rejected connections are accepted and closed right away (client_sock goes out of
        // scope); leaving them in the backlog would keep the listener readable forever.
        const size_t max_sessions = max_clients_.load() + kUnauthenticatedHeadroom;
        if (sessions_.size() >= max_sessions) {
            std::string suffix;
            if (allowLog(log_conn_limit_, now_us, suffix)) {
                logWarn("Refusing connection from " + peer_ep.toString() + ": too many connections (" +
                        std::to_string(sessions_.size()) + ")" + suffix);
            }
            continue;
        }
        const size_t per_ip_cap = max_conns_per_ip_.load();
        if (per_ip_cap > 0 && !peer_ep.isLoopback()) {
            const AddrKey key = addrKeyOf(peer_ep);
            size_t same_ip = 0;
            for (const auto& s : sessions_) {
                if (addrKeyOf(s->tcp_peer_ep) == key) ++same_ip;
            }
            if (same_ip >= per_ip_cap) {
                std::string suffix;
                if (allowLog(log_conn_limit_, now_us, suffix)) {
                    logWarn("Refusing connection from " + peer_ep.toString() + ": per-IP connection limit (" +
                            std::to_string(per_ip_cap) + ") reached" + suffix);
                }
                continue;
            }
        }

        client_sock.setNonBlocking(true);
        client_sock.setNoDelay(true);
        client_sock.setRecvBufferSize(4 * 1024 * 1024);
        client_sock.setSendBufferSize(4 * 1024 * 1024);

        auto session = std::make_unique<ClientSession>();
        session->tcp_sock = std::move(client_sock);
        session->tcp_peer_ep = peer_ep;
        session->connected_us = now_us;
        session->last_active_us = now_us;
        sessions_.push_back(std::move(session));
    }

    // The listener polled readable but nothing could be accepted: accept() is failing (e.g. out
    // of descriptors, which leaves the connection queued). Pause polling the listeners briefly
    // instead of spinning on it at full CPU.
    if (handled == 0) {
        accept_backoff_until_us_ = now_us + kAcceptBackoffUs;
    }
}

void VpnServer::handleClientReadable(ClientSession& session, bool& out_disconnect) {
    out_disconnect = false;
    uint8_t temp[65536];
    bool peer_closed = false;
    // Bounded per tick so one fast sender cannot monopolise the server tick.
    for (int budget = kMaxTcpRecvChunksPerTick; budget > 0; --budget) {
        bool would_block = false;
        int got = session.tcp_sock.recvNonBlocking(temp, sizeof(temp), would_block);
        if (got > 0) {
            session.recv_buf.append(temp, static_cast<size_t>(got));
            session.last_active_us = platform::monotonicMicros();
        } else if (would_block) {
            break;
        } else {
            peer_closed = true; // EOF (got == 0) or socket error (got < 0)
            break;
        }
    }

    // Frames that arrived together with the FIN are still processed before the teardown.
    while (!session.closing && session.recv_buf.size() >= sizeof(ControlHeader)) {
        ControlHeader hdr{};
        std::memcpy(&hdr, session.recv_buf.data(), sizeof(hdr));
        if (hdr.magic != kProtocolMagic || hdr.payload_len > kMaxPacketSize * 2) {
            out_disconnect = true;
            return;
        }
        size_t total_frame = sizeof(ControlHeader) + hdr.payload_len;
        if (session.recv_buf.size() < total_frame) {
            break;
        }

        const uint8_t* payload = session.recv_buf.data() + sizeof(ControlHeader);
        processControlMessage(session, static_cast<ControlMsgType>(hdr.type), payload, hdr.payload_len);
        session.recv_buf.consumeFront(total_frame);
    }

    if (peer_closed) {
        out_disconnect = true;
    }
}

void VpnServer::processControlMessage(ClientSession& session, ControlMsgType type, const uint8_t* payload, size_t len) {
    const uint64_t now_us = platform::monotonicMicros();

    if (type == ControlMsgType::ClientHello) {
        if (len < sizeof(MsgClientHello) || session.authenticated) return;
        MsgClientHello hello{};
        std::memcpy(&hello, payload, sizeof(hello));
        hello.nickname[kMaxNicknameLen - 1] = '\0';
        const std::string nick = sanitizeNickname(hello.nickname, kMaxNicknameLen);

        // Single pass: replace the stale session of a client reconnecting before its previous
        // session timed out (its VIP is reserved for that token and reclaimed below), and count
        // authenticated sessions overall and from this client's IP for the capacity checks.
        const AddrKey src_key = addrKeyOf(session.tcp_peer_ep);
        size_t authed = 0;
        size_t authed_same_ip = 0;
        for (auto& existing : sessions_) {
            if (existing.get() == &session || !existing->authenticated) continue;
            if (hello.prev_session_token != 0 && existing->session_token == hello.prev_session_token) {
                logInfo("Replacing stale session for reconnecting client '" + existing->nickname +
                        "' (" + ipv4ToString(existing->vip) + ")");
                sessions_by_vip_.erase(existing->vip);
                releaseVip(existing->vip, existing->session_token, now_us);
                existing->authenticated = false;
                existing->vip = 0;
                existing->closing = true;
                existing->tcp_sock.close();
                peer_list_dirty_ = true;
                continue;
            }
            ++authed;
            if (addrKeyOf(existing->tcp_peer_ep) == src_key) ++authed_same_ip;
        }

        // Enforce server capacity before allocating a VIP
        const size_t max_clients = max_clients_.load();
        if (authed >= max_clients) {
            std::string suffix;
            if (allowLog(log_capacity_, now_us, suffix)) {
                logWarn("Rejecting client '" + nick + "': server at capacity (" + std::to_string(authed) +
                        "/" + std::to_string(max_clients) + ")" + suffix);
            }
            return;
        }
        const size_t per_ip_cap = max_clients_per_ip_.load();
        if (per_ip_cap > 0 && !session.tcp_peer_ep.isLoopback() && authed_same_ip >= per_ip_cap) {
            std::string suffix;
            if (allowLog(log_capacity_, now_us, suffix)) {
                logWarn("Rejecting client '" + nick + "' from " + session.tcp_peer_ep.ipToString() +
                        ": per-IP client limit (" + std::to_string(per_ip_cap) + ") reached" + suffix);
            }
            return;
        }

        uint32_t vip = allocateVip(hello.preferred_vip, hello.prev_session_token, now_us);
        if (vip == 0) {
            std::string suffix;
            if (allowLog(log_capacity_, now_us, suffix)) {
                logWarn("Rejecting client '" + nick + "': no free virtual IP" + suffix);
            }
            return;
        }

        session.vip = vip;
        session.session_token = newSessionToken();
        session.nickname = nick.empty() ? ("Peer_" + std::to_string(vip & 0xFF)) : nick;
        session.declared_udp_port = hello.udp_listen_port;
        session.declared_udp_port_v6 = hello.udp_listen_port_v6 ? hello.udp_listen_port_v6 : hello.udp_listen_port;
        session.authenticated = true;
        sessions_by_vip_[session.vip] = &session;

        // Observed TCP IP + declared UDP port, plus client-reported LAN/loopback IPv6 & IPv4
        // endpoints (invalid, unspecified and port-0 entries are ignored).
        platform::Endpoint observed_tcp_udp = session.tcp_peer_ep;
        observed_tcp_udp.port_host = session.tcp_peer_ep.isIpv6()
            ? session.declared_udp_port_v6
            : session.declared_udp_port;
        session.reported_endpoints.clear();
        addUniqueEndpoint(session.reported_endpoints, observed_tcp_udp);

        uint8_t count = std::min<uint8_t>(hello.local_ep_count, static_cast<uint8_t>(kMaxEndpointsPerPeer));
        for (uint8_t i = 0; i < count; ++i) {
            addUniqueEndpoint(session.reported_endpoints, platform::fromEndpointWire(hello.local_endpoints[i]));
        }
        rebuildEndpoints(session);

        MsgServerWelcome welcome{};
        welcome.assigned_vip = session.vip;
        welcome.session_token = session.session_token;
        welcome.server_udp_port = udp_port_;
        queueControlMessage(session, ControlMsgType::ServerWelcome, &welcome, sizeof(welcome));

        logInfo("Client '" + session.nickname + "' joined via " +
                std::string(session.tcp_peer_ep.versionString()) +
                " (" + session.tcp_peer_ep.toString() + ") -> VIP " + ipv4ToString(session.vip));
        peer_list_dirty_ = true;
        return;
    }

    if (!session.authenticated) return;

    switch (type) {
        case ControlMsgType::ChatMessage: {
            if (len < sizeof(MsgChat)) break;
            MsgChat chat{};
            std::memcpy(&chat, payload, sizeof(chat));
            if (sizeof(MsgChat) + chat.text_len > len) break;

            // Chat is copied to every peer; cap how much one client can make the server fan out.
            if (!session.chat_bucket.take(now_us, kChatBytesPerSec, kChatBurstBytes,
                                          sizeof(MsgChat) + chat.text_len)) {
                std::string suffix;
                if (allowLog(log_flood_, now_us, suffix)) {
                    logWarn("Dropping chat from '" + session.nickname + "': rate limit exceeded" + suffix);
                }
                break;
            }

            // Enforce authoritative sender VIP & nickname (sanitized at join)
            chat.sender_vip = session.vip;
            std::memset(chat.sender_name, 0, sizeof(chat.sender_name));
            std::strncpy(chat.sender_name, session.nickname.c_str(), kMaxNicknameLen - 1);

            ByteBuffer buf(sizeof(MsgChat) + chat.text_len);
            buf.appendPod(chat);
            buf.append(payload + sizeof(MsgChat), chat.text_len);

            for (auto& peer : sessions_) {
                if (peer->authenticated && !peer->closing) {
                    queueControlMessage(*peer, ControlMsgType::ChatMessage, buf.data(), buf.size());
                }
            }
            break;
        }

        case ControlMsgType::RelayPacket: {
            if (len < sizeof(MsgRelayHeader)) break;
            MsgRelayHeader rh{};
            std::memcpy(&rh, payload, sizeof(rh));
            if (sizeof(MsgRelayHeader) + rh.data_len > len) break;

            rh.src_vip = session.vip;
            ControlHeader ch{};
            ch.magic = kProtocolMagic;
            ch.type = static_cast<uint8_t>(ControlMsgType::RelayPacket);
            ch.payload_len = static_cast<uint32_t>(sizeof(MsgRelayHeader) + rh.data_len);
            const uint8_t* inner_data = payload + sizeof(MsgRelayHeader);

            if (isBroadcastOrMulticast(rh.dst_vip)) {
                // Each relayed broadcast is copied to every peer, so one client could make the server
                // send N times its own upload; excess beyond the per-session budget is dropped.
                if (!session.broadcast_bucket.take(now_us, kBroadcastRelayBytesPerSec, kBroadcastRelayBurstBytes,
                                                   sizeof(MsgRelayHeader) + rh.data_len)) {
                    std::string suffix;
                    if (allowLog(log_flood_, now_us, suffix)) {
                        logWarn("Dropping relayed broadcast from '" + session.nickname +
                                "': rate limit exceeded" + suffix);
                    }
                    break;
                }
                for (auto& peer : sessions_) {
                    if (peer->authenticated && !peer->closing && peer->vip != session.vip) {
                        if (peer->send_buf.size() + sizeof(ch) + sizeof(rh) + rh.data_len >
                            kMaxSessionSendBufBytes) {
                            continue; // overloaded peer: drop (virtual TCP retransmits)
                        }
                        peer->send_buf.appendPod(ch);
                        peer->send_buf.appendPod(rh);
                        if (rh.data_len > 0) {
                            peer->send_buf.append(inner_data, rh.data_len);
                        }
                    }
                }
            } else if (isVirtualUnicast(rh.dst_vip)) {
                auto target_it = sessions_by_vip_.find(rh.dst_vip);
                if (target_it != sessions_by_vip_.end()) {
                    ClientSession* peer = target_it->second;
                    if (peer && peer->authenticated && !peer->closing &&
                        peer->send_buf.size() + sizeof(ch) + sizeof(rh) + rh.data_len <=
                            kMaxSessionSendBufBytes) {
                        peer->send_buf.appendPod(ch);
                        peer->send_buf.appendPod(rh);
                        if (rh.data_len > 0) {
                            peer->send_buf.append(inner_data, rh.data_len);
                        }
                    }
                }
            }
            break;
        }

        case ControlMsgType::PingKeepalive: {
            queueControlMessage(session, ControlMsgType::PongKeepalive, nullptr, 0);
            break;
        }

        default:
            break;
    }
}

bool VpnServer::allowUdpRegister(const platform::Endpoint& src, uint64_t now_us) {
    if (src.isLoopback()) return true;
    const AddrKey key = addrKeyOf(src);
    auto it = udp_reg_buckets_.find(key);
    if (it == udp_reg_buckets_.end()) {
        // Fail open when the table is full (e.g. a spoofed-source flood): session tokens are
        // 64-bit CSPRNG values, so this limiter is defence in depth, not what keeps them secret.
        if (udp_reg_buckets_.size() >= kMaxTrackedUdpSources) return true;
        it = udp_reg_buckets_.emplace(key, TokenBucket{}).first;
    }
    return it->second.take(now_us, kUdpRegRatePerSec, kUdpRegBurst, 1);
}

void VpnServer::handleUdpReadable(platform::NetSocket& sock, uint64_t now_us) {
    if (!sock.isValid()) return;
    uint8_t buf[2048];
    // Bounded per tick so a registration flood cannot monopolise the server tick.
    for (int budget = kMaxDatagramsPerTick; budget > 0; --budget) {
        platform::Endpoint src_ep{};
        bool would_block = false;
        int got = sock.recvFrom(buf, sizeof(buf), src_ep, would_block);
        if (got < 0 || would_block) break; // drained or socket error
        // Empty, short or foreign datagrams are skipped without ending the batch.
        if (static_cast<size_t>(got) < sizeof(UdpRegisterPacket)) continue;

        UdpRegisterPacket reg{};
        std::memcpy(&reg, buf, sizeof(reg));
        if (reg.hdr.magic != kProtocolMagic ||
            reg.hdr.type != static_cast<uint8_t>(UdpPacketType::UdpRegister)) {
            continue;
        }
        if (!allowUdpRegister(src_ep, now_us)) continue;

        const uint32_t src_vip = reg.src_vip;
        const uint64_t token = reg.session_token;
        auto it = sessions_by_vip_.find(src_vip);
        if (it == sessions_by_vip_.end()) continue;
        ClientSession* s = it->second;
        if (!s || !s->authenticated || s->closing || s->session_token != token) continue;

        s->last_active_us = now_us;
        // Only a genuinely new NAT mapping that changes the published list triggers a broadcast.
        if (noteObservedEndpoint(*s, src_ep, now_us) && rebuildEndpoints(*s)) {
            peer_list_dirty_ = true;
        }

        UdpWireHeader ack{};
        ack.magic = kProtocolMagic;
        ack.type = static_cast<uint8_t>(UdpPacketType::UdpRegAck);
        sock.sendTo(&ack, sizeof(ack), src_ep);
    }
}

void VpnServer::housekeeping(uint64_t now_us) {
    // Expired VIP reservations: the VIP is free for anyone again, its pair keys are dropped.
    for (int i = 2; i <= 254; ++i) {
        if (vip_reservations_[i].token != 0 && vip_reservations_[i].expires_us <= now_us) {
            vip_reservations_[i] = VipReservation{};
            erasePairKeys(kVirtualSubnetBase | static_cast<uint32_t>(i));
        }
    }

    // UDP-observed endpoints that are no longer refreshed (old NAT mappings).
    for (auto& s : sessions_) {
        if (!s->authenticated || s->observed_endpoints.empty()) continue;
        const size_t before = s->observed_endpoints.size();
        s->observed_endpoints.erase(
            std::remove_if(s->observed_endpoints.begin(), s->observed_endpoints.end(),
                           [now_us](const ObservedEndpoint& o) {
                               return now_us > o.last_seen_us && now_us - o.last_seen_us > kObservedEndpointTtlUs;
                           }),
            s->observed_endpoints.end());
        if (s->observed_endpoints.size() != before && rebuildEndpoints(*s)) {
            peer_list_dirty_ = true;
        }
    }

    // Idle UdpRegister rate-limit buckets (a fresh bucket starts full, so forgetting them is lossless).
    for (auto it = udp_reg_buckets_.begin(); it != udp_reg_buckets_.end();) {
        if (now_us > it->second.last_us && now_us - it->second.last_us > kUdpSourceIdleUs) {
            it = udp_reg_buckets_.erase(it);
        } else {
            ++it;
        }
    }
}

void VpnServer::workerLoop() {
    // Reused across iterations so the poll list does not reallocate every tick.
    std::vector<platform::PollItem> poll_items;
    std::vector<bool> session_disconnect;

    while (running_.load()) {
        poll_items.clear();
        size_t idx_tcp_v6 = ~static_cast<size_t>(0);
        size_t idx_tcp_v4 = ~static_cast<size_t>(0);
        size_t idx_udp_v6 = ~static_cast<size_t>(0);
        size_t idx_udp_v4 = ~static_cast<size_t>(0);
        size_t sessions_poll_start = 0;

        {
            std::lock_guard<std::mutex> lock(mutex_);
            poll_items.reserve(4 + sessions_.size());
            const bool poll_listeners = platform::monotonicMicros() >= accept_backoff_until_us_;
            if (poll_listeners && tcp_listener_v6_.isValid()) {
                idx_tcp_v6 = poll_items.size();
                poll_items.push_back({tcp_listener_v6_.handle(), true, false, false, false, false});
            }
            if (poll_listeners && tcp_listener_.isValid()) {
                idx_tcp_v4 = poll_items.size();
                poll_items.push_back({tcp_listener_.handle(), true, false, false, false, false});
            }
            if (udp_sock_v6_.isValid()) {
                idx_udp_v6 = poll_items.size();
                poll_items.push_back({udp_sock_v6_.handle(), true, false, false, false, false});
            }
            if (udp_sock_.isValid()) {
                idx_udp_v4 = poll_items.size();
                poll_items.push_back({udp_sock_.handle(), true, false, false, false, false});
            }

            sessions_poll_start = poll_items.size();
            for (const auto& s : sessions_) {
                s->poll_index = poll_items.size();
                poll_items.push_back({
                    s->tcp_sock.handle(),
                    true,
                    !s->send_buf.empty(),
                    false, false, false
                });
            }
        }

        platform::pollSockets(poll_items, 15);

        std::lock_guard<std::mutex> lock(mutex_);
        uint64_t now_us = platform::monotonicMicros();
        if (idx_tcp_v6 < poll_items.size() && poll_items[idx_tcp_v6].can_read) {
            handleNewTcpConnection(tcp_listener_v6_, now_us);
        }
        if (idx_tcp_v4 < poll_items.size() && poll_items[idx_tcp_v4].can_read) {
            handleNewTcpConnection(tcp_listener_, now_us);
        }
        if (idx_udp_v6 < poll_items.size() && poll_items[idx_udp_v6].can_read) {
            handleUdpReadable(udp_sock_v6_, now_us);
        }
        if (idx_udp_v4 < poll_items.size() && poll_items[idx_udp_v4].can_read) {
            handleUdpReadable(udp_sock_, now_us);
        }

        // Pass 1: Read from all readable client sessions (which may enqueue relay packets into other peers' send_buf)
        session_disconnect.assign(sessions_.size(), false);
        for (size_t i = 0; i < sessions_.size(); ++i) {
            auto& s = sessions_[i];
            if (!s->tcp_sock.isValid() || s->closing) {
                session_disconnect[i] = true;
                continue;
            }
            // Sessions created during this tick have no poll entry (poll_index is
            // stale/SIZE_MAX), so they are simply skipped until the next tick.
            const size_t p = s->poll_index;
            if (p < sessions_poll_start || p >= poll_items.size()) {
                continue;
            }
            if (poll_items[p].has_error) {
                session_disconnect[i] = true;
            } else if (poll_items[p].can_read) {
                bool disc = false;
                handleClientReadable(*s, disc);
                session_disconnect[i] = disc;
            }
        }

        // Unauthenticated sessions must send a valid ClientHello within kHelloDeadlineUs; everyone
        // is subject to the 30-second (or configured) inactivity timeout.
        now_us = platform::monotonicMicros();
        uint64_t timeout_us = peer_timeout_us_.load();
        for (size_t i = 0; i < sessions_.size(); ++i) {
            auto& s = sessions_[i];
            if (session_disconnect[i]) continue;
            if (!s->authenticated && now_us > s->connected_us &&
                now_us - s->connected_us > kHelloDeadlineUs) {
                logDebug("Closing connection from " + s->tcp_peer_ep.toString() + ": not authenticated in time");
                session_disconnect[i] = true;
            } else if (timeout_us > 0 &&
                s->last_active_us != 0 && now_us > s->last_active_us &&
                (now_us - s->last_active_us) > timeout_us) {
                logWarn("Client '" + (s->nickname.empty() ? "unauthenticated" : s->nickname) +
                        "' (" + ipv4ToString(s->vip) + ") timed out after " +
                        std::to_string((now_us - s->last_active_us) / 1'000'000ULL) + "s of no reaction");
                session_disconnect[i] = true;
            }
        }

        // Pass 2: Flush send_buf for all sessions immediately in the same tick and remove disconnected sessions.
        // A client whose queue makes no progress for the inactivity timeout is not reading and is dropped
        // (receiving its keep-alives alone would otherwise keep it alive forever).
        for (size_t i = 0; i < sessions_.size(); ) {
            auto& s = sessions_[i];
            bool disconnect = ((i < session_disconnect.size()) ? session_disconnect[i] : false) || s->closing;

            if (!disconnect) {
                bool progressed = false;
                while (!s->send_buf.empty()) {
                    bool would_block = false;
                    int sent = s->tcp_sock.sendNonBlocking(s->send_buf.data(), s->send_buf.size(), would_block);
                    if (sent > 0) {
                        s->send_buf.consumeFront(static_cast<size_t>(sent));
                        progressed = true;
                    } else if (sent < 0) {
                        disconnect = true;
                        break;
                    } else {
                        break;
                    }
                }
                if (s->send_buf.empty()) {
                    s->send_stall_since_us = 0;
                } else if (progressed || s->send_stall_since_us == 0) {
                    s->send_stall_since_us = now_us;
                } else if (timeout_us > 0 && now_us > s->send_stall_since_us &&
                           now_us - s->send_stall_since_us > timeout_us) {
                    logWarn("Client '" + (s->nickname.empty() ? "unauthenticated" : s->nickname) +
                            "' (" + ipv4ToString(s->vip) + ") is not reading; disconnecting");
                    disconnect = true;
                }
            }

            if (disconnect) {
                if (s->authenticated) {
                    logInfo("Client '" + s->nickname + "' (" + ipv4ToString(s->vip) + ") disconnected");
                    sessions_by_vip_.erase(s->vip);
                    // Hold the VIP for this client's token so nobody else can take it over while it reconnects.
                    releaseVip(s->vip, s->session_token, now_us);
                    peer_list_dirty_ = true;
                }
                sessions_.erase(sessions_.begin() + static_cast<std::ptrdiff_t>(i));
                if (i < session_disconnect.size()) {
                    session_disconnect.erase(session_disconnect.begin() + static_cast<std::ptrdiff_t>(i));
                }
            } else {
                ++i;
            }
        }

        if (now_us - last_housekeeping_us_ >= kHousekeepingIntervalUs) {
            last_housekeeping_us_ = now_us;
            housekeeping(now_us);
        }

        // Coalesced peer-list broadcast: at most one per tick and per kPeerListMinIntervalUs. It is
        // queued here and flushed on the next tick (whose poll wakes immediately for pending sends).
        if (peer_list_dirty_ && now_us - last_peer_list_us_ >= kPeerListMinIntervalUs) {
            peer_list_dirty_ = false;
            last_peer_list_us_ = now_us;
            broadcastPeerList();
        }
    }
}

} // namespace apivpn::server
