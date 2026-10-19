#include "apivpn/platform/net_socket.hpp"
#include "apivpn/common/protocol.hpp"

#include <arpa/inet.h>
#include <cctype>
#include <cerrno>
#include <chrono>
#include <csignal>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <ifaddrs.h>
#include <net/if.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <strings.h>
#include <sys/random.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>

#include <algorithm>
#include <thread>

#ifndef MSG_NOSIGNAL
#define MSG_NOSIGNAL 0
#endif

namespace apivpn::platform {

namespace {

int toNative(SocketHandle h) {
    return (h == kInvalidSocket) ? -1 : static_cast<int>(h);
}

SocketHandle fromNative(int fd) {
    return (fd < 0) ? kInvalidSocket : static_cast<SocketHandle>(fd);
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

bool toSockAddr(const Endpoint& ep, sockaddr_storage& out_ss, socklen_t& out_len) {
    std::memset(&out_ss, 0, sizeof(out_ss));
    if (ep.isIpv6()) {
        auto* sin6 = reinterpret_cast<sockaddr_in6*>(&out_ss);
        sin6->sin6_family = AF_INET6;
        sin6->sin6_port = htons(ep.port_host);
        std::memcpy(&sin6->sin6_addr, ep.ipv6.data(), 16);
        out_len = sizeof(sockaddr_in6);
        return true;
    }
    if (ep.isIpv4()) {
        auto* sin4 = reinterpret_cast<sockaddr_in*>(&out_ss);
        sin4->sin_family = AF_INET;
        sin4->sin_port = htons(ep.port_host);
        sin4->sin_addr.s_addr = htonl(ep.ip_host);
        out_len = sizeof(sockaddr_in);
        return true;
    }
    out_len = 0;
    return false;
}

bool fromSockAddr(const sockaddr_storage& ss, Endpoint& out_ep) {
    if (ss.ss_family == AF_INET6) {
        const auto* sin6 = reinterpret_cast<const sockaddr_in6*>(&ss);
        const auto* b = reinterpret_cast<const uint8_t*>(&sin6->sin6_addr);
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

bool isWouldBlockError(int err) {
    return err == EAGAIN || err == EWOULDBLOCK;
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
            out_host = s;
        }
    }

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

    if (::strcasecmp(host.c_str(), "localhost") == 0) {
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

    std::vector<Endpoint> result;
    result.reserve(v6_list.size() + v4_list.size());
    for (const auto& ep : v6_list) result.push_back(ep);
    for (const auto& ep : v4_list) result.push_back(ep);
    return result;
}

bool initNetwork() {
    std::signal(SIGPIPE, SIG_IGN);
    return true;
}

void cleanupNetwork() {
}

std::vector<uint32_t> getLocalIpv4Addresses() {
    std::vector<uint32_t> result;
    result.push_back(kLoopbackIp);

    struct ifaddrs* ifaddr = nullptr;
    if (::getifaddrs(&ifaddr) == -1) {
        return result;
    }

    for (struct ifaddrs* ifa = ifaddr; ifa != nullptr; ifa = ifa->ifa_next) {
        if (!ifa->ifa_addr || ifa->ifa_addr->sa_family != AF_INET) continue;
        if (!(ifa->ifa_flags & IFF_UP) || (ifa->ifa_flags & IFF_LOOPBACK)) continue;

        auto* sin = reinterpret_cast<sockaddr_in*>(ifa->ifa_addr);
        uint32_t ip = ntohl(sin->sin_addr.s_addr);
        if (ip != 0 && ip != kLoopbackIp && (ip >> 24) != 169) {
            if (std::find(result.begin(), result.end(), ip) == result.end()) {
                result.push_back(ip);
            }
        }
    }

    ::freeifaddrs(ifaddr);
    return result;
}

std::vector<std::array<uint8_t, 16>> getLocalIpv6Addresses() {
    std::vector<std::array<uint8_t, 16>> result;
    std::array<uint8_t, 16> loopback_v6{};
    loopback_v6[15] = 1;
    result.push_back(loopback_v6);

    struct ifaddrs* ifaddr = nullptr;
    if (::getifaddrs(&ifaddr) == -1) {
        return result;
    }

    for (struct ifaddrs* ifa = ifaddr; ifa != nullptr; ifa = ifa->ifa_next) {
        if (!ifa->ifa_addr || ifa->ifa_addr->sa_family != AF_INET6) continue;
        if (!(ifa->ifa_flags & IFF_UP) || (ifa->ifa_flags & IFF_LOOPBACK)) continue;

        auto* sin6 = reinterpret_cast<sockaddr_in6*>(ifa->ifa_addr);
        std::array<uint8_t, 16> addr{};
        std::memcpy(addr.data(), &sin6->sin6_addr, 16);

        if (addr == std::array<uint8_t, 16>{} || addr == loopback_v6) continue;
        if (addr[0] == 0xFF) continue;
        if (addr[0] == 0xFE && (addr[1] & 0xC0) == 0x80) continue;

        if (std::find(result.begin(), result.end(), addr) == result.end()) {
            result.push_back(addr);
        }
    }

    ::freeifaddrs(ifaddr);
    return result;
}

uint64_t monotonicMicros() {
    auto now = std::chrono::steady_clock::now();
    return static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::microseconds>(now.time_since_epoch()).count());
}

uint32_t currentProcessId() {
    return static_cast<uint32_t>(::getpid());
}

bool secureRandom(void* buf, size_t len) {
    auto* out = static_cast<uint8_t*>(buf);
    while (len > 0) {
        ssize_t got = ::getrandom(out, len, 0);
        if (got < 0) {
            if (errno == EINTR) continue;
            return false;
        }
        out += got;
        len -= static_cast<size_t>(got);
    }
    return true;
}

uint64_t secureRandom64() {
    uint64_t v = 0;
    while (v == 0) {
        if (!secureRandom(&v, sizeof(v))) {
            // Never fall back to a predictable value for something used as a credential.
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
    int fd = ::socket(af, SOCK_STREAM, IPPROTO_TCP);
    if (fd >= 0 && version == IpVersion::V6) {
        int v6only = 1;
        ::setsockopt(fd, IPPROTO_IPV6, IPV6_V6ONLY, &v6only, sizeof(v6only));
    }
    return NetSocket(fromNative(fd), (version == IpVersion::V6) ? IpVersion::V6 : IpVersion::V4);
}

NetSocket NetSocket::createUdp(IpVersion version) {
    int af = (version == IpVersion::V6) ? AF_INET6 : AF_INET;
    int fd = ::socket(af, SOCK_DGRAM, IPPROTO_UDP);
    if (fd >= 0 && version == IpVersion::V6) {
        int v6only = 1;
        ::setsockopt(fd, IPPROTO_IPV6, IPV6_V6ONLY, &v6only, sizeof(v6only));
    }
    return NetSocket(fromNative(fd), (version == IpVersion::V6) ? IpVersion::V6 : IpVersion::V4);
}

void NetSocket::close() {
    if (handle_ != kInvalidSocket) {
        ::close(toNative(handle_));
        handle_ = kInvalidSocket;
    }
}

bool NetSocket::shutdownSend() {
    if (!isValid()) return false;
    return ::shutdown(toNative(handle_), SHUT_WR) == 0;
}

bool NetSocket::setAbortiveClose() {
    if (!isValid()) return false;
    linger lg{};
    lg.l_onoff = 1;
    lg.l_linger = 0;
    return ::setsockopt(toNative(handle_), SOL_SOCKET, SO_LINGER, &lg, sizeof(lg)) == 0;
}

bool NetSocket::setNonBlocking(bool non_blocking) {
    if (!isValid()) return false;
    int fd = toNative(handle_);
    int flags = ::fcntl(fd, F_GETFL, 0);
    if (flags == -1) return false;
    if (non_blocking) {
        flags |= O_NONBLOCK;
    } else {
        flags &= ~O_NONBLOCK;
    }
    return ::fcntl(fd, F_SETFL, flags) == 0;
}

bool NetSocket::setReuseAddr(bool reuse) {
    if (!isValid()) return false;
    int val = reuse ? 1 : 0;
    return ::setsockopt(toNative(handle_), SOL_SOCKET, SO_REUSEADDR, &val, sizeof(val)) == 0;
}

bool NetSocket::setNoDelay(bool no_delay) {
    if (!isValid()) return false;
    int val = no_delay ? 1 : 0;
    return ::setsockopt(toNative(handle_), IPPROTO_TCP, TCP_NODELAY, &val, sizeof(val)) == 0;
}

bool NetSocket::setBroadcast(bool enable) {
    if (!isValid()) return false;
    int val = enable ? 1 : 0;
    return ::setsockopt(toNative(handle_), SOL_SOCKET, SO_BROADCAST, &val, sizeof(val)) == 0;
}

bool NetSocket::setRecvBufferSize(int bytes) {
    if (!isValid()) return false;
    return ::setsockopt(toNative(handle_), SOL_SOCKET, SO_RCVBUF, &bytes, sizeof(bytes)) == 0;
}

bool NetSocket::setSendBufferSize(int bytes) {
    if (!isValid()) return false;
    return ::setsockopt(toNative(handle_), SOL_SOCKET, SO_SNDBUF, &bytes, sizeof(bytes)) == 0;
}

bool NetSocket::bind(uint32_t ip_host, uint16_t port_host) {
    return bind(Endpoint::fromIpv4(ip_host, port_host));
}

bool NetSocket::bind(const Endpoint& ep) {
    if (!isValid() || ep.version != version_) return false;
    sockaddr_storage ss{};
    socklen_t len = 0;
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
    socklen_t len = sizeof(ss);
    if (::getsockname(toNative(handle_), reinterpret_cast<sockaddr*>(&ss), &len) != 0) {
        return false;
    }
    return fromSockAddr(ss, out_ep);
}

bool NetSocket::getPeerEndpoint(Endpoint& out_ep) const {
    if (!isValid()) return false;
    sockaddr_storage ss{};
    socklen_t len = sizeof(ss);
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
    int fd = toNative(handle_);
    sockaddr_storage ss{};
    socklen_t addr_len = 0;
    if (!toSockAddr(ep, ss, addr_len)) return false;

    if (timeout_ms <= 0) {
        return ::connect(fd, reinterpret_cast<const sockaddr*>(&ss), addr_len) == 0;
    }

    setNonBlocking(true);
    int rc = ::connect(fd, reinterpret_cast<const sockaddr*>(&ss), addr_len);
    if (rc == 0) {
        setNonBlocking(false);
        return true;
    }

    if (errno != EINPROGRESS && !isWouldBlockError(errno)) {
        setNonBlocking(false);
        return false;
    }

    struct pollfd pfd{};
    pfd.fd = fd;
    pfd.events = POLLOUT;
    int poll_rc = ::poll(&pfd, 1, timeout_ms);
    if (poll_rc <= 0 || (pfd.revents & (POLLERR | POLLHUP | POLLNVAL))) {
        setNonBlocking(false);
        return false;
    }

    int so_error = 0;
    socklen_t opt_len = sizeof(so_error);
    if (::getsockopt(fd, SOL_SOCKET, SO_ERROR, &so_error, &opt_len) != 0) {
        setNonBlocking(false);
        return false;
    }
    setNonBlocking(false);
    return so_error == 0;
}

int NetSocket::startConnect(const Endpoint& ep) {
    if (!isValid() || ep.version != version_) return -1;
    int fd = toNative(handle_);
    sockaddr_storage ss{};
    socklen_t addr_len = 0;
    if (!toSockAddr(ep, ss, addr_len)) return -1;

    setNonBlocking(true);
    int rc = ::connect(fd, reinterpret_cast<const sockaddr*>(&ss), addr_len);
    if (rc == 0) return 1;
    if (errno == EINPROGRESS || errno == EALREADY || isWouldBlockError(errno)) return 0;
    return -1;
}

int NetSocket::checkConnect() {
    if (!isValid()) return -1;
    int fd = toNative(handle_);
    struct pollfd pfd{};
    pfd.fd = fd;
    pfd.events = POLLOUT;
    int prc = ::poll(&pfd, 1, 0);
    if (prc < 0) return -1;
    if (prc == 0) return 0; // still connecting

    int so_error = 0;
    socklen_t opt_len = sizeof(so_error);
    if (::getsockopt(fd, SOL_SOCKET, SO_ERROR, &so_error, &opt_len) != 0) return -1;
    if (so_error == 0) return 1;
    if (so_error == EINPROGRESS || so_error == EALREADY || isWouldBlockError(so_error)) return 0;
    return -1;
}

NetSocket NetSocket::accept(Endpoint* out_peer) {
    if (!isValid()) return NetSocket();
    sockaddr_storage peer_ss{};
    socklen_t len = sizeof(peer_ss);
    int client_fd = ::accept(toNative(handle_), reinterpret_cast<sockaddr*>(&peer_ss), &len);
    if (client_fd < 0) {
        return NetSocket();
    }
    Endpoint ep{};
    fromSockAddr(peer_ss, ep);
    if (out_peer) {
        *out_peer = ep;
    }
    return NetSocket(fromNative(client_fd), version_);
}

int NetSocket::sendAll(const void* data, size_t len) {
    if (!isValid()) return -1;
    int fd = toNative(handle_);
    const auto* ptr = static_cast<const char*>(data);
    size_t total = 0;
    while (total < len) {
        ssize_t sent = ::send(fd, ptr + total, len - total, MSG_NOSIGNAL);
        if (sent > 0) {
            total += static_cast<size_t>(sent);
        } else if (sent < 0 && errno == EINTR) {
            continue;
        } else if (sent < 0 && isWouldBlockError(errno)) {
            struct pollfd pfd{};
            pfd.fd = fd;
            pfd.events = POLLOUT;
            if (::poll(&pfd, 1, 1000) <= 0) return -1;
        } else {
            return -1;
        }
    }
    return static_cast<int>(total);
}

int NetSocket::sendNonBlocking(const void* data, size_t len, bool& out_would_block) {
    out_would_block = false;
    if (!isValid()) return -1;
    int fd = toNative(handle_);
    while (true) {
        ssize_t sent = ::send(fd, data, len, MSG_NOSIGNAL);
        if (sent >= 0) return static_cast<int>(sent);
        if (errno == EINTR) continue;
        if (isWouldBlockError(errno)) {
            out_would_block = true;
            return 0;
        }
        return -1;
    }
}

int NetSocket::recvNonBlocking(void* buf, size_t max_len, bool& out_would_block) {
    out_would_block = false;
    if (!isValid()) return -1;
    int fd = toNative(handle_);
    while (true) {
        ssize_t got = ::recv(fd, buf, max_len, 0);
        if (got > 0) return static_cast<int>(got);
        if (got == 0) return 0; // EOF
        if (errno == EINTR) continue;
        if (isWouldBlockError(errno)) {
            out_would_block = true;
            return 0;
        }
        return -1;
    }
}

int NetSocket::recvExact(void* buf, size_t len, int timeout_ms) {
    if (!isValid()) return -1;
    int fd = toNative(handle_);
    auto* ptr = static_cast<char*>(buf);
    size_t total = 0;
    while (total < len) {
        if (timeout_ms > 0) {
            struct pollfd pfd{};
            pfd.fd = fd;
            pfd.events = POLLIN;
            int prc = ::poll(&pfd, 1, timeout_ms);
            if (prc <= 0) return -1;
        }
        ssize_t got = ::recv(fd, ptr + total, len - total, 0);
        if (got < 0 && errno == EINTR) continue;
        if (got <= 0) return -1;
        total += static_cast<size_t>(got);
    }
    return static_cast<int>(total);
}

int NetSocket::sendTo(const void* data, size_t len, const Endpoint& dst) {
    if (!isValid() || dst.version != version_) return -1;
    sockaddr_storage ss{};
    socklen_t addr_len = 0;
    if (!toSockAddr(dst, ss, addr_len)) return -1;
    ssize_t sent = ::sendto(toNative(handle_), data, len, MSG_NOSIGNAL,
                            reinterpret_cast<const sockaddr*>(&ss), addr_len);
    return (sent >= 0) ? static_cast<int>(sent) : -1;
}

int NetSocket::recvFrom(void* buf, size_t max_len, Endpoint& out_src, bool& out_would_block) {
    out_would_block = false;
    if (!isValid()) return -1;
    int fd = toNative(handle_);
    while (true) {
        sockaddr_storage src_ss{};
        socklen_t addr_len = sizeof(src_ss);
        ssize_t got = ::recvfrom(fd, buf, max_len, 0,
                                 reinterpret_cast<sockaddr*>(&src_ss), &addr_len);
        if (got >= 0) {
            fromSockAddr(src_ss, out_src);
            return static_cast<int>(got);
        }
        if (errno == EINTR) continue;
        if (isWouldBlockError(errno) || errno == ECONNREFUSED) {
            out_would_block = true;
            return 0;
        }
        return -1;
    }
}

int pollSockets(std::vector<PollItem>& items, int timeout_ms) {
    if (items.empty()) {
        if (timeout_ms > 0) {
            std::this_thread::sleep_for(std::chrono::milliseconds(timeout_ms));
        }
        return 0;
    }

    // Grow-only thread-local poll array: avoids a heap allocation on every tick.
    thread_local std::vector<struct pollfd> fds;
    if (fds.size() < items.size()) {
        fds.resize(items.size());
    }
    for (size_t i = 0; i < items.size(); ++i) {
        fds[i].fd = toNative(items[i].handle);
        fds[i].events = 0;
        if (items[i].want_read)  fds[i].events |= POLLIN;
        if (items[i].want_write) fds[i].events |= POLLOUT;
        fds[i].revents = 0;
        items[i].can_read = false;
        items[i].can_write = false;
        items[i].has_error = false;
    }

    int rc = ::poll(fds.data(), static_cast<nfds_t>(items.size()), timeout_ms);
    if (rc > 0) {
        for (size_t i = 0; i < items.size(); ++i) {
            short rev = fds[i].revents;
            if (rev & (POLLIN | POLLHUP))   items[i].can_read = true;
            if (rev & POLLOUT)              items[i].can_write = true;
            if (rev & (POLLERR | POLLNVAL)) items[i].has_error = true;
        }
    }
    return rc;
}

} // namespace apivpn::platform
