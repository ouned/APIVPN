#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif

#include <winsock2.h>
#include <ws2tcpip.h>
#include <iphlpapi.h>
#include <mswsock.h>
#include <windows.h>
#include <bcrypt.h>

#include "apivpn/platform/net_socket.hpp"
#include "apivpn/common/protocol.hpp"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdlib>
#include <cstring>

#ifndef SIO_UDP_CONNRESET
#define SIO_UDP_CONNRESET _WSAIOW(IOC_VENDOR, 12)
#endif

namespace apivpn::platform {

namespace {

SOCKET toNative(SocketHandle h) {
    return (h == kInvalidSocket) ? INVALID_SOCKET : static_cast<SOCKET>(h);
}

SocketHandle fromNative(SOCKET s) {
    return (s == INVALID_SOCKET) ? kInvalidSocket : static_cast<SocketHandle>(s);
}

std::string trimWhitespace(const std::string& s) {
    size_t start = s.find_first_not_of(" \t\r\n");
    if (start == std::string::npos) return "";
    size_t end = s.find_last_not_of(" \t\r\n");
    return s.substr(start, end - start + 1);
}

bool isAllDigits(const std::string& s) {
    if (s.empty()) return false;
    for (char c : s) {
        if (!std::isdigit(static_cast<unsigned char>(c))) return false;
    }
    return true;
}

bool toSockAddr(const Endpoint& ep, sockaddr_storage& out_ss, int& out_len) {
    std::memset(&out_ss, 0, sizeof(out_ss));
    if (ep.isIpv6()) {
        auto* sin6 = reinterpret_cast<sockaddr_in6*>(&out_ss);
        sin6->sin6_family = AF_INET6;
        sin6->sin6_port = htons(ep.port_host);
        std::memcpy(&sin6->sin6_addr, ep.ipv6.data(), 16);
        out_len = static_cast<int>(sizeof(sockaddr_in6));
        return true;
    }
    if (ep.isIpv4()) {
        auto* sin4 = reinterpret_cast<sockaddr_in*>(&out_ss);
        sin4->sin_family = AF_INET;
        sin4->sin_port = htons(ep.port_host);
        sin4->sin_addr.s_addr = htonl(ep.ip_host);
        out_len = static_cast<int>(sizeof(sockaddr_in));
        return true;
    }
    out_len = 0;
    return false;
}

bool fromSockAddr(const sockaddr_storage& ss, Endpoint& out_ep) {
    if (ss.ss_family == AF_INET6) {
        const auto* sin6 = reinterpret_cast<const sockaddr_in6*>(&ss);
        const auto* b = reinterpret_cast<const uint8_t*>(&sin6->sin6_addr);
        // Check for IPv4-mapped IPv6 address (::ffff:a.b.c.d)
        bool v4_mapped = (b[0] == 0 && b[1] == 0 && b[2] == 0 && b[3] == 0 &&
                          b[4] == 0 && b[5] == 0 && b[6] == 0 && b[7] == 0 &&
                          b[8] == 0 && b[9] == 0 && b[10] == 0xFF && b[11] == 0xFF);
        uint16_t port = ntohs(sin6->sin6_port);
        if (v4_mapped) {
            out_ep = Endpoint::fromIpv4(makeIpv4Host(b[12], b[13], b[14], b[15]), port);
        } else {
            out_ep = Endpoint::fromIpv6(b, port);
        }
        return true;
    }
    if (ss.ss_family == AF_INET) {
        const auto* sin4 = reinterpret_cast<const sockaddr_in*>(&ss);
        out_ep = Endpoint::fromIpv4(ntohl(sin4->sin_addr.s_addr), ntohs(sin4->sin_port));
        return true;
    }
    return false;
}

} // namespace

std::string Endpoint::ipToString() const {
    if (version == IpVersion::V6) {
        char buf[INET6_ADDRSTRLEN] = {};
        in6_addr a6{};
        std::memcpy(&a6, ipv6.data(), 16);
        if (::inet_ntop(AF_INET6, &a6, buf, sizeof(buf))) {
            return std::string(buf);
        }
        return "::";
    }
    return ipv4ToString(ip_host);
}

std::string Endpoint::toString() const {
    if (version == IpVersion::V6) {
        return "[" + ipToString() + "]:" + std::to_string(port_host);
    }
    return ipv4ToString(ip_host) + ":" + std::to_string(port_host);
}

bool splitHostPort(const std::string& input, std::string& out_host, uint16_t& inout_port) {
    std::string s = trimWhitespace(input);
    if (s.empty()) {
        out_host.clear();
        return false;
    }

    if (s.front() == '[') {
        size_t close_bracket = s.find(']');
        if (close_bracket == std::string::npos) {
            return false;
        }
        out_host = s.substr(1, close_bracket - 1);
        if (close_bracket + 1 < s.size()) {
            if (s[close_bracket + 1] != ':') return false;
            std::string port_str = s.substr(close_bracket + 2);
            if (!isAllDigits(port_str)) return false;
            unsigned long p = std::strtoul(port_str.c_str(), nullptr, 10);
            if (p == 0 || p > 65535) return false;
            inout_port = static_cast<uint16_t>(p);
        }
    } else {
        size_t first_colon = s.find(':');
        size_t last_colon = s.rfind(':');
        if (first_colon != std::string::npos && first_colon == last_colon) {
            // Single colon: host:port
            std::string host_part = s.substr(0, first_colon);
            std::string port_part = s.substr(first_colon + 1);
            if (isAllDigits(port_part)) {
                unsigned long p = std::strtoul(port_part.c_str(), nullptr, 10);
                if (p > 0 && p <= 65535) {
                    out_host = host_part;
                    inout_port = static_cast<uint16_t>(p);
                } else {
                    out_host = s;
                }
            } else {
                out_host = s;
            }
        } else {
            // Either no colon (hostname/IPv4) or multiple colons (unbracketed IPv6 literal)
            out_host = s;
        }
    }

    // Strip optional IPv6 scope ID suffix (%...)
    size_t pct = out_host.find('%');
    if (pct != std::string::npos) {
        out_host = out_host.substr(0, pct);
    }

    return !out_host.empty();
}

std::string formatHostPort(const std::string& host, uint16_t port) {
    std::string clean_host = host;
    uint16_t parsed_port = port;
    splitHostPort(host, clean_host, parsed_port);
    if (clean_host.find(':') != std::string::npos) {
        return "[" + clean_host + "]:" + std::to_string(parsed_port);
    }
    return clean_host + ":" + std::to_string(parsed_port);
}

bool parseIpString(const std::string& str, Endpoint& out_ep, uint16_t default_port) {
    std::string host;
    uint16_t port = default_port;
    if (!splitHostPort(str, host, port)) {
        return false;
    }

    in6_addr a6{};
    if (::inet_pton(AF_INET6, host.c_str(), &a6) == 1) {
        out_ep = Endpoint::fromIpv6(reinterpret_cast<const uint8_t*>(&a6), port);
        return true;
    }

    uint32_t ip4 = 0;
    if (parseIpv4String(host, ip4)) {
        out_ep = Endpoint::fromIpv4(ip4, port);
        return true;
    }

    return false;
}

std::vector<Endpoint> resolveHost(const std::string& host_or_ip, uint16_t port) {
    std::string host;
    uint16_t resolved_port = port;
    if (!splitHostPort(host_or_ip, host, resolved_port)) {
        return {};
    }

    Endpoint literal_ep{};
    if (parseIpString(host, literal_ep, resolved_port)) {
        return {literal_ep};
    }

    std::vector<Endpoint> v6_list;
    std::vector<Endpoint> v4_list;

    auto addUnique = [](std::vector<Endpoint>& vec, const Endpoint& ep) {
        if (!ep.isValidAddress()) return;
        if (std::find(vec.begin(), vec.end(), ep) == vec.end()) {
            vec.push_back(ep);
        }
    };

    // For "localhost", always ensure ::1 (IPv6) is first and 127.0.0.1 (IPv4) is fallback
    if (_stricmp(host.c_str(), "localhost") == 0) {
        addUnique(v6_list, Endpoint::loopbackV6(resolved_port));
        addUnique(v4_list, Endpoint::loopbackV4(resolved_port));
    }

    addrinfo hints{};
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;

    addrinfo* res = nullptr;
    if (::getaddrinfo(host.c_str(), nullptr, &hints, &res) == 0 && res) {
        for (addrinfo* cur = res; cur != nullptr; cur = cur->ai_next) {
            if (!cur->ai_addr) continue;
            if (cur->ai_family == AF_INET6 && cur->ai_addrlen >= sizeof(sockaddr_in6)) {
                const auto* sin6 = reinterpret_cast<const sockaddr_in6*>(cur->ai_addr);
                Endpoint ep = Endpoint::fromIpv6(
                    reinterpret_cast<const uint8_t*>(&sin6->sin6_addr), resolved_port);
                addUnique(v6_list, ep);
            } else if (cur->ai_family == AF_INET && cur->ai_addrlen >= sizeof(sockaddr_in)) {
                const auto* sin4 = reinterpret_cast<const sockaddr_in*>(cur->ai_addr);
                Endpoint ep = Endpoint::fromIpv4(ntohl(sin4->sin_addr.s_addr), resolved_port);
                addUnique(v4_list, ep);
            }
        }
        ::freeaddrinfo(res);
    }

    // Prefer IPv6 in all cases: place all IPv6 endpoints before IPv4 endpoints
    std::vector<Endpoint> result;
    result.reserve(v6_list.size() + v4_list.size());
    for (const auto& ep : v6_list) result.push_back(ep);
    for (const auto& ep : v4_list) result.push_back(ep);
    return result;
}

bool initNetwork() {
    WSADATA wsa{};
    return WSAStartup(MAKEWORD(2, 2), &wsa) == 0;
}

void cleanupNetwork() {
    WSACleanup();
}

std::vector<uint32_t> getLocalIpv4Addresses() {
    std::vector<uint32_t> result;
    // Always include loopback first so same-machine peers can hole-punch directly over 127.0.0.1
    result.push_back(kLoopbackIp);

    ULONG buf_len = 16384;
    std::vector<uint8_t> buffer(buf_len);
    auto* adapters = reinterpret_cast<PIP_ADAPTER_ADDRESSES>(buffer.data());

    ULONG flags = GAA_FLAG_SKIP_ANYCAST | GAA_FLAG_SKIP_MULTICAST | GAA_FLAG_SKIP_DNS_SERVER;
    ULONG ret = GetAdaptersAddresses(AF_INET, flags, nullptr, adapters, &buf_len);
    if (ret == ERROR_BUFFER_OVERFLOW) {
        buffer.resize(buf_len);
        adapters = reinterpret_cast<PIP_ADAPTER_ADDRESSES>(buffer.data());
        ret = GetAdaptersAddresses(AF_INET, flags, nullptr, adapters, &buf_len);
    }

    if (ret == NO_ERROR) {
        for (auto* curr = adapters; curr != nullptr; curr = curr->Next) {
            if (curr->OperStatus != IfOperStatusUp) continue;
            for (auto* ua = curr->FirstUnicastAddress; ua != nullptr; ua = ua->Next) {
                if (ua->Address.lpSockaddr && ua->Address.lpSockaddr->sa_family == AF_INET) {
                    auto* sin = reinterpret_cast<sockaddr_in*>(ua->Address.lpSockaddr);
                    uint32_t ip = ntohl(sin->sin_addr.s_addr);
                    if (ip != 0 && ip != kLoopbackIp && (ip >> 24) != 169) {
                        if (std::find(result.begin(), result.end(), ip) == result.end()) {
                            result.push_back(ip);
                        }
                    }
                }
            }
        }
    }
    return result;
}

std::vector<std::array<uint8_t, 16>> getLocalIpv6Addresses() {
    std::vector<std::array<uint8_t, 16>> result;
    // Always include IPv6 loopback (::1) first so same-machine peers can hole-punch over IPv6
    std::array<uint8_t, 16> loopback_v6{};
    loopback_v6[15] = 1;
    result.push_back(loopback_v6);

    ULONG buf_len = 16384;
    std::vector<uint8_t> buffer(buf_len);
    auto* adapters = reinterpret_cast<PIP_ADAPTER_ADDRESSES>(buffer.data());

    ULONG flags = GAA_FLAG_SKIP_ANYCAST | GAA_FLAG_SKIP_MULTICAST | GAA_FLAG_SKIP_DNS_SERVER;
    ULONG ret = GetAdaptersAddresses(AF_INET6, flags, nullptr, adapters, &buf_len);
    if (ret == ERROR_BUFFER_OVERFLOW) {
        buffer.resize(buf_len);
        adapters = reinterpret_cast<PIP_ADAPTER_ADDRESSES>(buffer.data());
        ret = GetAdaptersAddresses(AF_INET6, flags, nullptr, adapters, &buf_len);
    }

    if (ret == NO_ERROR) {
        for (auto* curr = adapters; curr != nullptr; curr = curr->Next) {
            if (curr->OperStatus != IfOperStatusUp) continue;
            for (auto* ua = curr->FirstUnicastAddress; ua != nullptr; ua = ua->Next) {
                if (ua->Address.lpSockaddr && ua->Address.lpSockaddr->sa_family == AF_INET6) {
                    auto* sin6 = reinterpret_cast<sockaddr_in6*>(ua->Address.lpSockaddr);
                    std::array<uint8_t, 16> addr{};
                    std::memcpy(addr.data(), &sin6->sin6_addr, 16);

                    // Skip unspecified (::), loopback (::1), multicast (ff00::/8), and link-local (fe80::/10)
                    if (addr == std::array<uint8_t, 16>{} || addr == loopback_v6) continue;
                    if (addr[0] == 0xFF) continue;
                    if (addr[0] == 0xFE && (addr[1] & 0xC0) == 0x80) continue;

                    if (std::find(result.begin(), result.end(), addr) == result.end()) {
                        result.push_back(addr);
                    }
                }
            }
        }
    }
    return result;
}

uint64_t monotonicMicros() {
    auto now = std::chrono::steady_clock::now();
    return static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::microseconds>(now.time_since_epoch()).count());
}

uint32_t currentProcessId() {
    return static_cast<uint32_t>(GetCurrentProcessId());
}

bool secureRandom(void* buf, size_t len) {
    auto* out = static_cast<uint8_t*>(buf);
    while (len > 0) {
        ULONG chunk = static_cast<ULONG>(std::min<size_t>(len, 0x10000000u));
        if (!BCRYPT_SUCCESS(BCryptGenRandom(nullptr, out, chunk, BCRYPT_USE_SYSTEM_PREFERRED_RNG))) {
            return false;
        }
        out += chunk;
        len -= chunk;
    }
    return true;
}

uint64_t secureRandom64() {
    uint64_t v = 0;
    while (v == 0) {
        if (!secureRandom(&v, sizeof(v))) {
            // The system RNG cannot fail on supported Windows versions; never fall back to a
            // predictable value for something used as a credential.
            std::abort();
        }
    }
    return v;
}

NetSocket::~NetSocket() {
    close();
}

NetSocket NetSocket::createTcp(IpVersion version) {
    int af = (version == IpVersion::V6) ? AF_INET6 : AF_INET;
    SOCKET s = ::socket(af, SOCK_STREAM, IPPROTO_TCP);
    if (s != INVALID_SOCKET && version == IpVersion::V6) {
        DWORD v6only = 1;
        ::setsockopt(s, IPPROTO_IPV6, IPV6_V6ONLY, reinterpret_cast<const char*>(&v6only), sizeof(v6only));
    }
    return NetSocket(fromNative(s), (version == IpVersion::V6) ? IpVersion::V6 : IpVersion::V4);
}

NetSocket NetSocket::createUdp(IpVersion version) {
    int af = (version == IpVersion::V6) ? AF_INET6 : AF_INET;
    SOCKET s = ::socket(af, SOCK_DGRAM, IPPROTO_UDP);
    if (s != INVALID_SOCKET) {
        if (version == IpVersion::V6) {
            DWORD v6only = 1;
            ::setsockopt(s, IPPROTO_IPV6, IPV6_V6ONLY, reinterpret_cast<const char*>(&v6only), sizeof(v6only));
        }
        // Prevent ICMP Port Unreachable from triggering WSAECONNRESET on subsequent recvfrom calls
        BOOL new_behavior = FALSE;
        DWORD bytes_returned = 0;
        WSAIoctl(s, SIO_UDP_CONNRESET, &new_behavior, sizeof(new_behavior),
                 nullptr, 0, &bytes_returned, nullptr, nullptr);
    }
    return NetSocket(fromNative(s), (version == IpVersion::V6) ? IpVersion::V6 : IpVersion::V4);
}

void NetSocket::close() {
    if (handle_ != kInvalidSocket) {
        ::closesocket(toNative(handle_));
        handle_ = kInvalidSocket;
    }
}

bool NetSocket::shutdownSend() {
    if (!isValid()) return false;
    return ::shutdown(toNative(handle_), SD_SEND) == 0;
}

bool NetSocket::setAbortiveClose() {
    if (!isValid()) return false;
    linger lg{};
    lg.l_onoff = 1;
    lg.l_linger = 0;
    return ::setsockopt(toNative(handle_), SOL_SOCKET, SO_LINGER,
                        reinterpret_cast<const char*>(&lg), sizeof(lg)) == 0;
}

bool NetSocket::setNonBlocking(bool non_blocking) {
    if (!isValid()) return false;
    u_long mode = non_blocking ? 1 : 0;
    return ::ioctlsocket(toNative(handle_), FIONBIO, &mode) == 0;
}

bool NetSocket::setReuseAddr(bool reuse) {
    // Windows' SO_REUSEADDR does not mean what it means on POSIX: it lets any other socket bind
    // the same port and receive part of its traffic. Callers use setReuseAddr for a server socket
    // that must be restartable on a fixed port, so on Windows this sets SO_EXCLUSIVEADDRUSE, which
    // keeps the port exclusive to this socket. (Only the server calls it; see vpn_server.cpp.)
    if (!isValid()) return false;
    BOOL val = reuse ? TRUE : FALSE;
    return ::setsockopt(toNative(handle_), SOL_SOCKET, SO_EXCLUSIVEADDRUSE,
                        reinterpret_cast<const char*>(&val), sizeof(val)) == 0;
}

bool NetSocket::setNoDelay(bool no_delay) {
    if (!isValid()) return false;
    BOOL val = no_delay ? TRUE : FALSE;
    return ::setsockopt(toNative(handle_), IPPROTO_TCP, TCP_NODELAY,
                        reinterpret_cast<const char*>(&val), sizeof(val)) == 0;
}

bool NetSocket::setBroadcast(bool enable) {
    if (!isValid()) return false;
    BOOL val = enable ? TRUE : FALSE;
    return ::setsockopt(toNative(handle_), SOL_SOCKET, SO_BROADCAST,
                        reinterpret_cast<const char*>(&val), sizeof(val)) == 0;
}

bool NetSocket::setRecvBufferSize(int bytes) {
    if (!isValid()) return false;
    return ::setsockopt(toNative(handle_), SOL_SOCKET, SO_RCVBUF,
                        reinterpret_cast<const char*>(&bytes), sizeof(bytes)) == 0;
}

bool NetSocket::setSendBufferSize(int bytes) {
    if (!isValid()) return false;
    return ::setsockopt(toNative(handle_), SOL_SOCKET, SO_SNDBUF,
                        reinterpret_cast<const char*>(&bytes), sizeof(bytes)) == 0;
}

bool NetSocket::bind(uint32_t ip_host, uint16_t port_host) {
    return bind(Endpoint::fromIpv4(ip_host, port_host));
}

bool NetSocket::bind(const Endpoint& ep) {
    if (!isValid() || ep.version != version_) return false;
    sockaddr_storage ss{};
    int len = 0;
    if (!toSockAddr(ep, ss, len)) return false;
    return ::bind(toNative(handle_), reinterpret_cast<const sockaddr*>(&ss), len) == 0;
}

bool NetSocket::bindAny(uint16_t port_host) {
    if (version_ == IpVersion::V6) {
        return bind(Endpoint::anyV6(port_host));
    }
    return bind(Endpoint::anyV4(port_host));
}

bool NetSocket::listen(int backlog) {
    if (!isValid()) return false;
    return ::listen(toNative(handle_), backlog) == 0;
}

bool NetSocket::getLocalEndpoint(Endpoint& out_ep) const {
    if (!isValid()) return false;
    sockaddr_storage ss{};
    int len = sizeof(ss);
    if (::getsockname(toNative(handle_), reinterpret_cast<sockaddr*>(&ss), &len) != 0) {
        return false;
    }
    return fromSockAddr(ss, out_ep);
}

bool NetSocket::getPeerEndpoint(Endpoint& out_ep) const {
    if (!isValid()) return false;
    sockaddr_storage ss{};
    int len = sizeof(ss);
    if (::getpeername(toNative(handle_), reinterpret_cast<sockaddr*>(&ss), &len) != 0) {
        return false;
    }
    return fromSockAddr(ss, out_ep);
}

bool NetSocket::connect(uint32_t ip_host, uint16_t port_host, int timeout_ms) {
    return connect(Endpoint::fromIpv4(ip_host, port_host), timeout_ms);
}

bool NetSocket::connect(const Endpoint& ep, int timeout_ms) {
    if (!isValid() || ep.version != version_) return false;
    SOCKET s = toNative(handle_);
    sockaddr_storage ss{};
    int addr_len = 0;
    if (!toSockAddr(ep, ss, addr_len)) return false;

    if (timeout_ms <= 0) {
        return ::connect(s, reinterpret_cast<const sockaddr*>(&ss), addr_len) == 0;
    }

    setNonBlocking(true);
    int rc = ::connect(s, reinterpret_cast<const sockaddr*>(&ss), addr_len);
    if (rc == 0) {
        setNonBlocking(false);
        return true;
    }

    int err = WSAGetLastError();
    if (err != WSAEWOULDBLOCK && err != WSAEINPROGRESS) {
        setNonBlocking(false);
        return false;
    }

    WSAPOLLFD pfd{};
    pfd.fd = s;
    pfd.events = POLLOUT;
    int poll_rc = WSAPoll(&pfd, 1, timeout_ms);
    if (poll_rc <= 0 || (pfd.revents & (POLLERR | POLLHUP))) {
        setNonBlocking(false);
        return false;
    }

    int so_error = 0;
    int opt_len = sizeof(so_error);
    ::getsockopt(s, SOL_SOCKET, SO_ERROR, reinterpret_cast<char*>(&so_error), &opt_len);
    setNonBlocking(false);
    return so_error == 0;
}

int NetSocket::startConnect(const Endpoint& ep) {
    if (!isValid() || ep.version != version_) return -1;
    SOCKET s = toNative(handle_);
    sockaddr_storage ss{};
    int addr_len = 0;
    if (!toSockAddr(ep, ss, addr_len)) return -1;

    setNonBlocking(true);
    int rc = ::connect(s, reinterpret_cast<const sockaddr*>(&ss), addr_len);
    if (rc == 0) return 1;
    int err = WSAGetLastError();
    if (err == WSAEWOULDBLOCK || err == WSAEINPROGRESS || err == WSAEALREADY) return 0;
    return -1;
}

int NetSocket::checkConnect() {
    if (!isValid()) return -1;
    SOCKET s = toNative(handle_);
    WSAPOLLFD pfd{};
    pfd.fd = s;
    pfd.events = POLLOUT;
    int prc = WSAPoll(&pfd, 1, 0);
    if (prc < 0) return -1;
    if (prc == 0) return 0; // still connecting

    int so_error = 0;
    int opt_len = sizeof(so_error);
    if (::getsockopt(s, SOL_SOCKET, SO_ERROR, reinterpret_cast<char*>(&so_error), &opt_len) != 0) return -1;
    if (so_error == 0) return 1;
    if (so_error == WSAEWOULDBLOCK || so_error == WSAEINPROGRESS || so_error == WSAEALREADY) return 0;
    return -1;
}

NetSocket NetSocket::accept(Endpoint* out_peer) {
    if (!isValid()) return NetSocket();
    sockaddr_storage peer_ss{};
    int len = sizeof(peer_ss);
    SOCKET client = ::accept(toNative(handle_), reinterpret_cast<sockaddr*>(&peer_ss), &len);
    if (client == INVALID_SOCKET) {
        return NetSocket();
    }
    Endpoint ep{};
    fromSockAddr(peer_ss, ep);
    if (out_peer) {
        *out_peer = ep;
    }
    return NetSocket(fromNative(client), version_);
}

int NetSocket::sendAll(const void* data, size_t len) {
    if (!isValid()) return -1;
    SOCKET s = toNative(handle_);
    const auto* ptr = static_cast<const char*>(data);
    size_t total = 0;
    while (total < len) {
        int sent = ::send(s, ptr + total, static_cast<int>(len - total), 0);
        if (sent > 0) {
            total += static_cast<size_t>(sent);
        } else if (sent == SOCKET_ERROR && WSAGetLastError() == WSAEWOULDBLOCK) {
            WSAPOLLFD pfd{};
            pfd.fd = s;
            pfd.events = POLLOUT;
            if (WSAPoll(&pfd, 1, 1000) <= 0) return -1;
        } else {
            return -1;
        }
    }
    return static_cast<int>(total);
}

int NetSocket::sendNonBlocking(const void* data, size_t len, bool& out_would_block) {
    out_would_block = false;
    if (!isValid()) return -1;
    int sent = ::send(toNative(handle_), static_cast<const char*>(data), static_cast<int>(len), 0);
    if (sent >= 0) return sent;
    int err = WSAGetLastError();
    if (err == WSAEWOULDBLOCK) {
        out_would_block = true;
        return 0;
    }
    return -1;
}

int NetSocket::recvNonBlocking(void* buf, size_t max_len, bool& out_would_block) {
    out_would_block = false;
    if (!isValid()) return -1;
    int got = ::recv(toNative(handle_), static_cast<char*>(buf), static_cast<int>(max_len), 0);
    if (got > 0) return got;
    if (got == 0) return 0; // EOF
    int err = WSAGetLastError();
    if (err == WSAEWOULDBLOCK) {
        out_would_block = true;
        return 0;
    }
    return -1;
}

int NetSocket::recvExact(void* buf, size_t len, int timeout_ms) {
    if (!isValid()) return -1;
    SOCKET s = toNative(handle_);
    auto* ptr = static_cast<char*>(buf);
    size_t total = 0;
    while (total < len) {
        if (timeout_ms > 0) {
            WSAPOLLFD pfd{};
            pfd.fd = s;
            pfd.events = POLLIN;
            int prc = WSAPoll(&pfd, 1, timeout_ms);
            if (prc <= 0) return -1;
        }
        int got = ::recv(s, ptr + total, static_cast<int>(len - total), 0);
        if (got <= 0) return -1;
        total += static_cast<size_t>(got);
    }
    return static_cast<int>(total);
}

int NetSocket::sendTo(const void* data, size_t len, const Endpoint& dst) {
    if (!isValid() || dst.version != version_) return -1;
    sockaddr_storage ss{};
    int addr_len = 0;
    if (!toSockAddr(dst, ss, addr_len)) return -1;
    return ::sendto(toNative(handle_), static_cast<const char*>(data), static_cast<int>(len), 0,
                    reinterpret_cast<const sockaddr*>(&ss), addr_len);
}

int NetSocket::recvFrom(void* buf, size_t max_len, Endpoint& out_src, bool& out_would_block) {
    out_would_block = false;
    if (!isValid()) return -1;
    sockaddr_storage src_ss{};
    int addr_len = sizeof(src_ss);
    int got = ::recvfrom(toNative(handle_), static_cast<char*>(buf), static_cast<int>(max_len), 0,
                         reinterpret_cast<sockaddr*>(&src_ss), &addr_len);
    if (got >= 0) {
        fromSockAddr(src_ss, out_src);
        return got;
    }
    int err = WSAGetLastError();
    if (err == WSAEMSGSIZE) {
        // Oversized datagram: Winsock fills the buffer with its first max_len bytes and discards
        // the rest, but reports an error. POSIX recvfrom returns the truncated length instead; do
        // the same so callers see identical behaviour (and their length checks apply) on both.
        fromSockAddr(src_ss, out_src);
        return static_cast<int>(max_len);
    }
    if (err == WSAEWOULDBLOCK || err == WSAECONNRESET) {
        out_would_block = true;
        return 0;
    }
    return -1;
}

int pollSockets(std::vector<PollItem>& items, int timeout_ms) {
    if (items.empty()) {
        if (timeout_ms > 0) Sleep(static_cast<DWORD>(timeout_ms));
        return 0;
    }
    // Grow-only thread-local poll array: avoids a heap allocation on every tick
    // (the client polls as often as once per millisecond while tunnels are open).
    thread_local std::vector<WSAPOLLFD> fds;
    if (fds.size() < items.size()) {
        fds.resize(items.size());
    }
    for (size_t i = 0; i < items.size(); ++i) {
        fds[i].fd = toNative(items[i].handle);
        fds[i].events = 0;
        if (items[i].want_read)  fds[i].events |= POLLRDNORM;
        if (items[i].want_write) fds[i].events |= POLLWRNORM;
        fds[i].revents = 0;
        items[i].can_read = false;
        items[i].can_write = false;
        items[i].has_error = false;
    }

    int rc = WSAPoll(fds.data(), static_cast<ULONG>(items.size()), timeout_ms);
    if (rc > 0) {
        for (size_t i = 0; i < items.size(); ++i) {
            short rev = fds[i].revents;
            if (rev & (POLLRDNORM | POLLIN | POLLHUP)) items[i].can_read = true;
            if (rev & (POLLWRNORM | POLLOUT))          items[i].can_write = true;
            if (rev & (POLLERR | POLLNVAL))            items[i].has_error = true;
        }
    }
    return rc;
}

} // namespace apivpn::platform
