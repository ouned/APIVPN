#pragma once

#include "apivpn/common/protocol.hpp"

#include <array>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

namespace apivpn::platform {

using SocketHandle = uint64_t;
constexpr SocketHandle kInvalidSocket = ~static_cast<SocketHandle>(0);

enum class IpVersion : uint8_t {
    None = 0,
    V4   = 4,
    V6   = 6,
};

struct Endpoint {
    IpVersion               version   = IpVersion::V4;
    uint32_t                ip_host   = 0;  // Host byte order (e.g. 0x7F000001 for 127.0.0.1)
    std::array<uint8_t, 16> ipv6      = {}; // Network byte order (16 bytes for IPv6)
    uint16_t                port_host = 0;  // Host byte order

    Endpoint() = default;
    Endpoint(uint32_t ipv4_host, uint16_t port)
        : version(IpVersion::V4), ip_host(ipv4_host), ipv6{}, port_host(port) {}
    Endpoint(const std::array<uint8_t, 16>& v6_net, uint16_t port)
        : version(IpVersion::V6), ip_host(0), ipv6(v6_net), port_host(port) {}

    static Endpoint fromIpv4(uint32_t ipv4_host, uint16_t port) {
        return Endpoint(ipv4_host, port);
    }
    static Endpoint fromIpv6(const uint8_t v6_net[16], uint16_t port) {
        std::array<uint8_t, 16> arr{};
        std::memcpy(arr.data(), v6_net, 16);
        return Endpoint(arr, port);
    }
    static Endpoint fromIpv6(const std::array<uint8_t, 16>& v6_net, uint16_t port) {
        return Endpoint(v6_net, port);
    }
    static Endpoint loopbackV4(uint16_t port = 0) {
        return Endpoint(kLoopbackIp, port);
    }
    static Endpoint loopbackV6(uint16_t port = 0) {
        std::array<uint8_t, 16> arr{};
        arr[15] = 1;
        return Endpoint(arr, port);
    }
    static Endpoint anyV4(uint16_t port = 0) {
        return Endpoint(0u, port);
    }
    static Endpoint anyV6(uint16_t port = 0) {
        std::array<uint8_t, 16> arr{};
        return Endpoint(arr, port);
    }

    bool isIpv4() const { return version == IpVersion::V4; }
    bool isIpv6() const { return version == IpVersion::V6; }

    bool isUnspecified() const {
        if (version == IpVersion::V4) return ip_host == 0;
        if (version == IpVersion::V6) return ipv6 == std::array<uint8_t, 16>{};
        return true;
    }

    bool isLoopback() const {
        if (version == IpVersion::V4) return (ip_host >> 24) == 127;
        if (version == IpVersion::V6) {
            std::array<uint8_t, 16> lb{};
            lb[15] = 1;
            return ipv6 == lb;
        }
        return false;
    }

    bool isValidAddress() const {
        return (version == IpVersion::V4 || version == IpVersion::V6) && !isUnspecified();
    }

    const char* versionString() const {
        if (version == IpVersion::V6) return "IPv6";
        if (version == IpVersion::V4) return "IPv4";
        return "None";
    }

    bool operator==(const Endpoint& o) const {
        if (version != o.version || port_host != o.port_host) return false;
        if (version == IpVersion::V6) return ipv6 == o.ipv6;
        return ip_host == o.ip_host;
    }
    bool operator!=(const Endpoint& o) const {
        return !(*this == o);
    }

    std::string ipToString() const;
    std::string toString() const;
};

inline EndpointWire toEndpointWire(const Endpoint& ep) {
    EndpointWire w{};
    w.port_host = ep.port_host;
    if (ep.isIpv6()) {
        w.family = 6;
        std::memcpy(w.addr, ep.ipv6.data(), 16);
    } else if (ep.isIpv4()) {
        w.family = 4;
        w.addr[0] = static_cast<uint8_t>((ep.ip_host >> 24) & 0xFF);
        w.addr[1] = static_cast<uint8_t>((ep.ip_host >> 16) & 0xFF);
        w.addr[2] = static_cast<uint8_t>((ep.ip_host >> 8) & 0xFF);
        w.addr[3] = static_cast<uint8_t>(ep.ip_host & 0xFF);
    }
    return w;
}

inline Endpoint fromEndpointWire(const EndpointWire& w) {
    if (w.family == 6) {
        return Endpoint::fromIpv6(w.addr, w.port_host);
    } else if (w.family == 4) {
        uint32_t ip = makeIpv4Host(w.addr[0], w.addr[1], w.addr[2], w.addr[3]);
        return Endpoint::fromIpv4(ip, w.port_host);
    }
    return Endpoint{};
}

// Split "host:port", "[ipv6]:port", "ipv6", or "host" into host and optional port
bool splitHostPort(const std::string& input, std::string& out_host, uint16_t& inout_port);
std::string formatHostPort(const std::string& host, uint16_t port);

// Parse literal IPv4 or IPv6 address string (with optional port)
bool parseIpString(const std::string& str, Endpoint& out_ep, uint16_t default_port = 0);

// Resolve hostname or IP literal (IPv6 or IPv4) to endpoints, ALWAYS preferring IPv6 first
std::vector<Endpoint> resolveHost(const std::string& host_or_ip, uint16_t port);

// Initialize/cleanup OS networking stack (WinSock2 on Windows, no-op on POSIX)
bool initNetwork();
void cleanupNetwork();

// Enumerate local IPv4 and IPv6 addresses of the host machine (including loopback first)
std::vector<uint32_t> getLocalIpv4Addresses();
std::vector<std::array<uint8_t, 16>> getLocalIpv6Addresses();

// High-resolution monotonic timestamp in microseconds
uint64_t monotonicMicros();

// Current OS process ID
uint32_t currentProcessId();

// Fill buf with bytes from the OS cryptographically secure RNG (BCryptGenRandom / getrandom).
bool secureRandom(void* buf, size_t len);
// Non-zero 64-bit value from the OS CSPRNG. Aborts if the RNG is unavailable rather than
// returning a predictable value, since callers use it for session tokens and pair keys.
uint64_t secureRandom64();

class NetSocket {
public:
    NetSocket() = default;
    explicit NetSocket(SocketHandle handle, IpVersion version = IpVersion::V4)
        : handle_(handle), version_(version) {}
    ~NetSocket();

    NetSocket(const NetSocket&) = delete;
    NetSocket& operator=(const NetSocket&) = delete;

    NetSocket(NetSocket&& other) noexcept : handle_(other.handle_), version_(other.version_) {
        other.handle_ = kInvalidSocket;
    }
    NetSocket& operator=(NetSocket&& other) noexcept {
        if (this != &other) {
            close();
            handle_ = other.handle_;
            version_ = other.version_;
            other.handle_ = kInvalidSocket;
        }
        return *this;
    }

    static NetSocket createTcp(IpVersion version = IpVersion::V4);
    static NetSocket createUdp(IpVersion version = IpVersion::V4);

    bool isValid() const { return handle_ != kInvalidSocket; }
    SocketHandle handle() const { return handle_; }
    IpVersion ipVersion() const { return version_; }
    SocketHandle release() {
        SocketHandle h = handle_;
        handle_ = kInvalidSocket;
        return h;
    }

    void close();

    // Half-close: stop sending on a TCP socket while keeping the receive direction open
    bool shutdownSend();

    // Make the next close() abortive (SO_LINGER {on, 0}): the peer sees a TCP reset instead of
    // an orderly EOF. Only sets the option, so it is safe while another thread still polls the
    // socket; the close itself can happen later.
    bool setAbortiveClose();

    bool setNonBlocking(bool non_blocking);
    // Listening-socket address option. POSIX: SO_REUSEADDR (fast restart past TIME_WAIT).
    // Windows: SO_EXCLUSIVEADDRUSE, because Winsock's SO_REUSEADDR would let another
    // process bind the same port and steal traffic.
    bool setReuseAddr(bool reuse);
    bool setNoDelay(bool no_delay);
    bool setBroadcast(bool enable);
    bool setRecvBufferSize(int bytes);
    bool setSendBufferSize(int bytes);

    // Bind to ip_host:port_host or Endpoint (pass port_host=0 for OS-assigned ephemeral port)
    bool bind(uint32_t ip_host, uint16_t port_host);
    bool bind(const Endpoint& ep);
    bool bindAny(uint16_t port_host);
    bool listen(int backlog = 64);

    // Query bound local endpoint (useful when bound to port 0)
    bool getLocalEndpoint(Endpoint& out_ep) const;
    bool getPeerEndpoint(Endpoint& out_ep) const;

    // TCP connect (with optional timeout in ms)
    bool connect(uint32_t ip_host, uint16_t port_host, int timeout_ms = 3000);
    bool connect(const Endpoint& ep, int timeout_ms = 3000);
    // Begin a non-blocking connect. Returns 1 if connected immediately, 0 if the
    // connection is in progress, -1 on immediate failure. The socket is left in
    // non-blocking mode; poll completion with checkConnect().
    int startConnect(const Endpoint& ep);
    // Poll a connect started with startConnect(): 1 = connected, 0 = in progress, -1 = failed.
    int checkConnect();
    NetSocket accept(Endpoint* out_peer = nullptr);

    // Returns >0 bytes sent/received, 0 on clean EOF or WouldBlock (check out_would_block), -1 on error
    int sendAll(const void* data, size_t len);
    int sendNonBlocking(const void* data, size_t len, bool& out_would_block);
    int recvNonBlocking(void* buf, size_t max_len, bool& out_would_block);
    int recvExact(void* buf, size_t len, int timeout_ms = 2000);

    // UDP datagram operations
    int sendTo(const void* data, size_t len, const Endpoint& dst);
    int recvFrom(void* buf, size_t max_len, Endpoint& out_src, bool& out_would_block);

private:
    SocketHandle handle_  = kInvalidSocket;
    IpVersion    version_ = IpVersion::V4;
};

struct PollItem {
    SocketHandle handle       = kInvalidSocket;
    bool         want_read    = true;
    bool         want_write   = false;
    bool         can_read     = false;
    bool         can_write    = false;
    bool         has_error    = false;
};

int pollSockets( std::vector<PollItem>& items, int timeout_ms);

} // namespace apivpn::platform
