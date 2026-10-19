#pragma once

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>

namespace apivpn {

// Virtual LAN configuration: 10.42.0.0/24
constexpr uint32_t kVirtualSubnetBase   = 0x0A2A0000u; // 10.42.0.0 (Host byte order)
constexpr uint32_t kVirtualSubnetMask   = 0xFFFFFF00u; // 255.255.255.0
constexpr uint32_t kVirtualServerIp     = 0x0A2A0001u; // 10.42.0.1
constexpr uint32_t kVirtualBroadcastIp  = 0x0A2A00FFu; // 10.42.0.255
constexpr uint32_t kGlobalBroadcastIp   = 0xFFFFFFFFu; // 255.255.255.255
constexpr uint32_t kLoopbackIp          = 0x7F000001u; // 127.0.0.1

// Magic numbers
// Bumped from "AVP1" when peer-pair authentication was added to PeerEntryWire and
// UdpPunchPacket, so mixed old/new builds reject each other's frames instead of misparsing them.
constexpr uint32_t kProtocolMagic       = 0x41565032u; // "AVP2"
constexpr uint32_t kLoopbackUdpMagic    = 0x4156504Eu; // "AVPN"
constexpr uint32_t kIpcMagic            = 0x41564950u; // "AVIP"

constexpr uint16_t kDefaultServerPort         = 34200;
constexpr size_t   kMaxPacketSize             = 65507;
constexpr size_t   kMaxTcpSegmentPayload      = 1200;  // Safe across mobile LTE/5G (CLAT/GTP) & cable DS-Lite MTUs without fragmentation
constexpr size_t   kMaxTcpWindowSegments      = 2048;  // Up to ~2.4 MB in flight for high-BDP WAN/mobile file transfers
constexpr size_t   kMaxTcpRecvBufBytes        = 4 * 1024 * 1024; // 4 MB receive buffer per TCP tunnel
constexpr size_t   kMaxNicknameLen            = 32;
constexpr size_t   kMaxEndpointsPerPeer       = 16;

// Maximum amount of socket input processed per network-loop tick. Without a cap,
// a sustained datagram flood can keep a read loop running for the entire tick and
// starve TCP pumping, keep-alives and server-send draining. Anything not read
// stays in the socket receive buffer and is picked up on the next tick.
constexpr int      kMaxDatagramsPerTick       = 64;
constexpr int      kMaxTcpRecvChunksPerTick   = 8;
constexpr uint64_t kHolePunchTimeoutUs        = 10'000'000ULL; // 10 seconds hole-punch attempt window
constexpr uint64_t kHolePunchRetryIntervalUs  = 30'000'000ULL; // 30 seconds periodic hole-punch retry when on Server Relay
constexpr uint64_t kTunnelKeepAliveIntervalUs = 5'000'000ULL;  // 5 seconds idle keep-alive
constexpr uint64_t kTunnelKeepAliveTimeoutUs  = 15'000'000ULL; // 15 seconds direct tunnel timeout
// Virtual-TCP tunnel liveness: an idle established tunnel emits a lightweight liveness
// probe every interval; if no frame is received for the idle timeout the tunnel is reaped
// so a silently-dead peer cannot leak loopback sockets or 4 MB receive buffers.
constexpr uint64_t kTcpTunnelKeepAliveIntervalUs    = 15'000'000ULL;
constexpr uint64_t kTcpTunnelIdleTimeoutUs          = 60'000'000ULL;
constexpr uint64_t kTcpTunnelBridgeConnectTimeoutUs = 3'000'000ULL;  // loopback app-connect budget
constexpr uint64_t kTcpZeroWindowProbeIntervalUs    = 200'000ULL;    // receive-window-0 probe cadence
constexpr uint16_t kTcpRwndUpdateThresholdSegments  = 16;            // min receive-window growth to advertise
constexpr uint64_t kServerKeepAliveIntervalUs = 5'000'000ULL;  // 5 seconds client->server keep-alive ping
constexpr uint64_t kServerPeerTimeoutUs       = 30'000'000ULL; // 30 seconds peer/server inactivity timeout
constexpr uint64_t kServerReconnectIntervalUs = 3'000'000ULL;  // 3 seconds periodic server reconnect attempt

inline constexpr uint32_t makeIpv4Host(uint8_t a, uint8_t b, uint8_t c, uint8_t d) {
    return (static_cast<uint32_t>(a) << 24) |
           (static_cast<uint32_t>(b) << 16) |
           (static_cast<uint32_t>(c) << 8)  |
           static_cast<uint32_t>(d);
}

// Wrap-safe 32-bit sequence comparison (RFC 1982 serial number arithmetic).
// TCP sequence numbers are assigned modulo 2^32, so plain '<'/'>' break across the
// wrap boundary; these helpers compare within a half-space window instead.
inline constexpr bool seqLess(uint32_t a, uint32_t b) {
    return static_cast<int32_t>(a - b) < 0;
}
inline constexpr bool seqGreater(uint32_t a, uint32_t b) {
    return static_cast<int32_t>(a - b) > 0;
}

inline constexpr bool isVirtualUnicast(uint32_t ip_host) {
    return (ip_host & kVirtualSubnetMask) == kVirtualSubnetBase &&
           (ip_host & 0xFFu) != 0 &&
           (ip_host & 0xFFu) != 255;
}

inline constexpr bool isVirtualSubnetBroadcast(uint32_t ip_host) {
    return ip_host == kVirtualBroadcastIp;
}

inline constexpr bool isGlobalBroadcast(uint32_t ip_host) {
    return ip_host == kGlobalBroadcastIp;
}

inline constexpr bool isMulticastIp(uint32_t ip_host) {
    return (ip_host & 0xF0000000u) == 0xE0000000u; // 224.0.0.0/4
}

inline constexpr bool isBroadcastOrMulticast(uint32_t ip_host) {
    return isVirtualSubnetBroadcast(ip_host) || isGlobalBroadcast(ip_host) || isMulticastIp(ip_host);
}

inline constexpr bool isVpnTargetIp(uint32_t ip_host) {
    return isVirtualUnicast(ip_host) || isBroadcastOrMulticast(ip_host);
}

inline std::string ipv4ToString(uint32_t ip_host) {
    return std::to_string((ip_host >> 24) & 0xFF) + "." +
           std::to_string((ip_host >> 16) & 0xFF) + "." +
           std::to_string((ip_host >> 8) & 0xFF) + "." +
           std::to_string(ip_host & 0xFF);
}

inline bool parseIpv4String(const std::string& str, uint32_t& out_ip_host) {
    unsigned int a = 0, b = 0, c = 0, d = 0;
    char tail = 0;
    if (std::sscanf(str.c_str(), "%u.%u.%u.%u%c", &a, &b, &c, &d, &tail) != 4) {
        return false;
    }
    if (a > 255 || b > 255 || c > 255 || d > 255) {
        return false;
    }
    out_ip_host = makeIpv4Host(
        static_cast<uint8_t>(a),
        static_cast<uint8_t>(b),
        static_cast<uint8_t>(c),
        static_cast<uint8_t>(d));
    return true;
}

// Environment variable names passed to hooked child processes
constexpr const char* kEnvVirtualIp     = "APIVPN_VIP";
constexpr const char* kEnvIpcPort       = "APIVPN_IPC_PORT";
constexpr const char* kEnvUdpGwPort     = "APIVPN_UDP_GW_PORT";
constexpr const char* kEnvTcpGwPort     = "APIVPN_TCP_GW_PORT";
constexpr const char* kEnvSessionToken  = "APIVPN_TOKEN";
constexpr const char* kEnvBinDir        = "APIVPN_BIN_DIR";

#pragma pack(push, 1)

// Endpoint representation (supports both IPv4 and IPv6)
struct EndpointWire {
    uint8_t  family;      // 4 = IPv4, 6 = IPv6
    uint8_t  addr[16];    // IPv4: addr[0..3] in network order; IPv6: addr[0..15] in network order
    uint16_t port_host;   // Host byte order
};

// ============================================================================
// 1. Control & Relay Protocol (TCP Client <-> Server)
// ============================================================================
enum class ControlMsgType : uint8_t {
    ClientHello     = 1,
    ServerWelcome   = 2,
    PeerListUpdate  = 3,
    ChatMessage     = 4,
    RelayPacket     = 5,
    PingKeepalive   = 6,
    PongKeepalive   = 7,
};

struct ControlHeader {
    uint32_t magic;       // kProtocolMagic
    uint8_t  type;        // ControlMsgType
    uint32_t payload_len; // Length of payload following this header
};

struct MsgClientHello {
    char         nickname[kMaxNicknameLen];
    uint16_t     udp_listen_port;
    uint16_t     udp_listen_port_v6;
    uint8_t      local_ep_count;
    EndpointWire local_endpoints[kMaxEndpointsPerPeer];
    uint32_t     preferred_vip;
    uint64_t     prev_session_token;
};

struct MsgServerWelcome {
    uint32_t assigned_vip;
    uint64_t session_token;
    uint16_t server_udp_port;
};

struct PeerEntryWire {
    uint32_t     vip;
    char         nickname[kMaxNicknameLen];
    uint8_t      endpoint_count;
    EndpointWire endpoints[kMaxEndpointsPerPeer];
    // Secret shared only by the recipient of this list and this peer (the server personalizes
    // the list per recipient). Direct P2P punch/keep-alive packets must carry it, so neither an
    // off-path host nor a third peer can claim this peer's VIP on the direct path.
    uint64_t     pair_key;
};

struct MsgPeerListHeader {
    uint16_t peer_count;
    // Followed by peer_count * PeerEntryWire
};

struct MsgChat {
    uint32_t sender_vip;
    char     sender_name[kMaxNicknameLen];
    uint16_t text_len;
    // Followed by text_len bytes of UTF-8 text
};

// Encapsulates either a VpnUdpData or VpnTcpFrame when routed via Server Relay
struct MsgRelayHeader {
    uint32_t src_vip;
    uint32_t dst_vip;     // Individual VIP or 0xFFFFFFFF / 10.42.0.255 for broadcast
    uint8_t  inner_type;  // UdpPacketType (VpnUdpData or VpnTcpFrame)
    uint16_t data_len;
    // Followed by inner frame bytes
};

// ============================================================================
// 2. UDP Peer-to-Peer & Server Registration Protocol
// ============================================================================
enum class UdpPacketType : uint8_t {
    UdpRegister  = 10, // Client -> Server UDP port to register external NAT endpoint
    UdpRegAck    = 11, // Server -> Client UDP ack
    PunchPing    = 12, // Peer <-> Peer hole punch probe
    PunchPong    = 13, // Peer <-> Peer hole punch response
    KeepAlive    = 14, // Peer <-> Peer tunnel keep-alive (sent every 5s if nothing else transmitted)
    VpnUdpData   = 20, // Virtual UDP datagram between peers
    VpnTcpFrame  = 30, // Virtual TCP multiplexed segment between peers
};

struct UdpWireHeader {
    uint32_t magic; // kProtocolMagic
    uint8_t  type;  // UdpPacketType
};

struct UdpRegisterPacket {
    UdpWireHeader hdr;
    uint32_t      src_vip;
    uint64_t      session_token;
};

// Used for PunchPing, PunchPong and KeepAlive.
struct UdpPunchPacket {
    UdpWireHeader hdr;
    uint32_t      src_vip;
    uint32_t      dst_vip;
    uint64_t      timestamp_us;
    uint64_t      pair_key;     // PeerEntryWire::pair_key for (src_vip, dst_vip); 0 is never valid
};

struct VpnUdpDataHeader {
    uint32_t src_vip;
    uint32_t dst_vip;
    uint16_t src_vport;
    uint16_t dst_vport;
    uint16_t payload_len;
    // Followed by payload_len bytes
};

enum class VpnTcpFlags : uint8_t {
    Syn    = 0x01,
    SynAck = 0x02,
    Data   = 0x04,
    Ack    = 0x08,
    Fin    = 0x10,
    Rst    = 0x20,
};

struct VpnTcpFrameHeader {
    uint32_t conn_id;
    uint32_t src_vip;
    uint32_t dst_vip;
    uint16_t src_vport;
    uint16_t dst_vport;
    uint8_t  flags;       // VpnTcpFlags bitmask
    uint32_t seq_num;
    uint32_t ack_num;     // Cumulative ACK (next_expected_seq)
    uint64_t sack_bitmap; // Bit i (0..63) = 1 if segment (ack_num + 1 + i) has been received
    uint16_t recv_window; // Available receive buffer capacity in segments
    uint16_t payload_len;
    // Followed by payload_len bytes
};

// ============================================================================
// 3. Hook DLL <-> Local Client Daemon Protocols (Bitness-Agnostic)
// ============================================================================

// Header prepended to every UDP packet sent between hooked sockets and local UDP gateway
struct LoopbackUdpHeader {
    uint32_t magic;      // kLoopbackUdpMagic
    uint32_t src_vip;    // Host byte order
    uint32_t dst_vip;    // Host byte order
    uint16_t src_vport;  // Host byte order
    uint16_t dst_vport;  // Host byte order
};

enum class IpcCommand : uint8_t {
    RegisterBind       = 1,
    UnregisterBind     = 2,
    PrepareTcpConnect  = 3,
    QueryTcpPeer       = 4,
    RegisterProcess    = 5,
    // Is (proto, virtual_port) bound by a hooked socket of this client? status 0 = yes, 1 = no.
    // Lets the hook tell a game's own virtual service on 127.0.0.1 apart from a real local one.
    QueryBind          = 6,
};

enum class IpcProto : uint8_t {
    Udp = 1,
    Tcp = 2,
};

struct IpcRequest {
    uint32_t magic;                // kIpcMagic
    uint8_t  command;              // IpcCommand
    uint8_t  proto;                // IpcProto
    uint16_t virtual_port;         // Host order
    uint16_t actual_loopback_port; // Host order
    uint32_t target_vip;           // Host order (for PrepareTcpConnect)
    uint16_t target_vport;         // Host order (for PrepareTcpConnect)
    uint32_t pid;                  // Process ID
};

struct IpcResponse {
    uint32_t magic;                // kIpcMagic
    uint8_t  status;               // 0 = OK, non-zero = error
    uint16_t assigned_vport;       // Host order
    uint32_t peer_vip;             // Host order (for QueryTcpPeer)
    uint16_t peer_vport;           // Host order (for QueryTcpPeer)
};

#pragma pack(pop)

} // namespace apivpn
