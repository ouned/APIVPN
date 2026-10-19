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

#include "apivpn/hook/socket_shim_core.hpp"
#include "apivpn/platform/hook_backend.hpp"
#include "apivpn/platform/process_launcher.hpp"
#include "apivpn/platform/win32_utf8.hpp"
#include "apivpn/common/protocol.hpp"

#include <algorithm>
#include <atomic>
#include <climits>
#include <cstring>
#include <memory>
#include <mutex>
#include <string>
#include <type_traits>
#include <unordered_map>
#include <vector>

#ifndef SIO_UDP_CONNRESET
#define SIO_UDP_CONNRESET _WSAIOW(IOC_VENDOR, 12)
#endif

namespace {

using namespace apivpn;
using namespace apivpn::hook;
using namespace apivpn::platform;

// Reentrancy guard per thread so internalWinsock calls bypass detours cleanly
thread_local bool g_bypass_hooks = false;

// True once every completion API detour (WSAGetOverlappedResult, GetOverlappedResult(Ex),
// GetQueuedCompletionStatus(Ex)) is installed. Without them an event/IOCP overlapped receive on
// a virtualized UDP socket cannot be fixed up, so it falls back to plain pass-through; overlapped
// I/O with a completion routine does not need them (the routine itself is wrapped).
std::atomic<bool> g_overlapped_fixup_ok{false};

struct ScopedHookBypass {
    bool prev;
    ScopedHookBypass() : prev(g_bypass_hooks) { g_bypass_hooks = true; }
    ~ScopedHookBypass() { g_bypass_hooks = prev; }
};

// Diagnostics go to the debugger / DebugView; the hook has no log file of its own.
void hookLog(const char* msg, const char* detail = nullptr) {
    std::string line = "[apivpn-hook] ";
    line += msg;
    if (detail) {
        line += ": ";
        line += detail;
    }
    line += "\n";
    OutputDebugStringA(line.c_str());
}

constexpr DWORD kUdpHdrLen = static_cast<DWORD>(sizeof(LoopbackUdpHeader));
// NTSTATUS STATUS_BUFFER_OVERFLOW as stored in (the low 32 bits of) OVERLAPPED::Internal
constexpr DWORD kStatusBufferOverflow = 0x80000005UL;
// Upper bound for app WSABUF arrays we copy; real calls never come close
constexpr DWORD kMaxWsaBufs = 1024;

// Stack-backed scratch buffer for the per-datagram hot path. The common case
// (small LAN game datagrams) is served from an inline stack array so that no
// heap allocation occurs per sendto()/recvfrom() call. Oversized payloads
// transparently fall back to a heap allocation.
class ScratchBuffer {
public:
    explicit ScratchBuffer(size_t size) : size_(size) {
        if (size > kInlineBytes) {
            heap_.resize(size);
            data_ = heap_.data();
        } else {
            data_ = inline_;
        }
    }

    ScratchBuffer(const ScratchBuffer&) = delete;
    ScratchBuffer& operator=(const ScratchBuffer&) = delete;

    uint8_t*       data()       { return data_; }
    const uint8_t* data() const { return data_; }
    size_t         size() const { return size_; }

private:
    static constexpr size_t kInlineBytes = 4096;
    uint8_t              inline_[kInlineBytes];
    std::vector<uint8_t> heap_;
    uint8_t*             data_ = nullptr;
    size_t               size_ = 0;
};

// Grow-only thread-local scratch buffer for the receive hot path. ScratchBuffer
// above is ideal for sendto() (exactly one allocation-free use per call), but
// receive calls happen even when no datagram is pending and callers
// often pass multi-kilobyte buffers. Reusing a single thread-local buffer means
// the allocation and value-initialisation (memset) happen at most once per size
// increase instead of on every recvfrom()/WSARecvFrom() call.
uint8_t* recvScratch(size_t size) {
    thread_local std::vector<uint8_t> scratch;
    if (scratch.size() < size) {
        scratch.resize(size);
    }
    return scratch.data();
}

// WSABUF list with a leading slot for the 16-byte loopback header followed by the app's own
// buffers (scatter/gather instead of copying the payload). Inline storage covers the usual case.
class HeaderedWsaBufs {
public:
    HeaderedWsaBufs(void* header, const WSABUF* app, DWORD app_count) {
        const size_t n = static_cast<size_t>(app_count) + 1;
        if (n > kInline) {
            heap_.resize(n);
            data_ = heap_.data();
        } else {
            data_ = inline_;
        }
        data_[0].len = kUdpHdrLen;
        data_[0].buf = static_cast<char*>(header);
        for (DWORD i = 0; i < app_count; ++i) {
            data_[i + 1] = app[i];
        }
        count_ = static_cast<DWORD>(n);
    }

    HeaderedWsaBufs(const HeaderedWsaBufs&) = delete;
    HeaderedWsaBufs& operator=(const HeaderedWsaBufs&) = delete;

    LPWSABUF data() { return data_; }
    DWORD    count() const { return count_; }

private:
    static constexpr size_t kInline = 8;
    WSABUF              inline_[kInline];
    std::vector<WSABUF> heap_;
    LPWSABUF            data_  = nullptr;
    DWORD               count_ = 0;
};

// Our own layout-compatible copy of OVERLAPPED_ENTRY, so the detour does not depend on SDK version macros
struct CompletionEntry {
    ULONG_PTR    lpCompletionKey;
    LPOVERLAPPED lpOverlapped;
    ULONG_PTR    Internal;
    DWORD        dwNumberOfBytesTransferred;
};

// Function pointer typedefs with explicit WINAPI / WSAAPI for 32-bit stdcall compatibility
using FnSocket              = SOCKET(WSAAPI*)(int, int, int);
using FnWSASocketA          = SOCKET(WSAAPI*)(int, int, int, LPWSAPROTOCOL_INFOA, GROUP, DWORD);
using FnWSASocketW          = SOCKET(WSAAPI*)(int, int, int, LPWSAPROTOCOL_INFOW, GROUP, DWORD);
using FnCloseSocket         = int(WSAAPI*)(SOCKET);
using FnBind                = int(WSAAPI*)(SOCKET, const sockaddr*, int);
using FnGetSockName         = int(WSAAPI*)(SOCKET, sockaddr*, int*);
using FnGetPeerName         = int(WSAAPI*)(SOCKET, sockaddr*, int*);
using FnSendTo              = int(WSAAPI*)(SOCKET, const char*, int, int, const sockaddr*, int);
using FnWSASendTo           = int(WSAAPI*)(SOCKET, LPWSABUF, DWORD, LPDWORD, DWORD, const sockaddr*, int, LPWSAOVERLAPPED, LPWSAOVERLAPPED_COMPLETION_ROUTINE);
using FnRecvFrom            = int(WSAAPI*)(SOCKET, char*, int, int, sockaddr*, int*);
using FnWSARecvFrom         = int(WSAAPI*)(SOCKET, LPWSABUF, DWORD, LPDWORD, LPDWORD, sockaddr*, LPINT, LPWSAOVERLAPPED, LPWSAOVERLAPPED_COMPLETION_ROUTINE);
using FnSend                = int(WSAAPI*)(SOCKET, const char*, int, int);
using FnWSASend             = int(WSAAPI*)(SOCKET, LPWSABUF, DWORD, LPDWORD, DWORD, LPWSAOVERLAPPED, LPWSAOVERLAPPED_COMPLETION_ROUTINE);
using FnRecv                = int(WSAAPI*)(SOCKET, char*, int, int);
using FnWSARecv             = int(WSAAPI*)(SOCKET, LPWSABUF, DWORD, LPDWORD, LPDWORD, LPWSAOVERLAPPED, LPWSAOVERLAPPED_COMPLETION_ROUTINE);
using FnConnect             = int(WSAAPI*)(SOCKET, const sockaddr*, int);
using FnWSAConnect          = int(WSAAPI*)(SOCKET, const sockaddr*, int, LPWSABUF, LPWSABUF, LPQOS, LPQOS);
using FnAccept              = SOCKET(WSAAPI*)(SOCKET, sockaddr*, int*);
using FnWSAAccept           = SOCKET(WSAAPI*)(SOCKET, sockaddr*, LPINT, LPCONDITIONPROC, DWORD_PTR);
using FnGetHostByName       = hostent*(WSAAPI*)(const char*);
using FnGetAddrInfo         = INT(WSAAPI*)(PCSTR, PCSTR, const ADDRINFOA*, PADDRINFOA*);
using FnWSAIoctl            = int(WSAAPI*)(SOCKET, DWORD, LPVOID, DWORD, LPVOID, DWORD, LPDWORD, LPWSAOVERLAPPED, LPWSAOVERLAPPED_COMPLETION_ROUTINE);
using FnWSAGetOverlappedResult = BOOL(WSAAPI*)(SOCKET, LPWSAOVERLAPPED, LPDWORD, BOOL, LPDWORD);
using FnGetOverlappedResult = BOOL(WINAPI*)(HANDLE, LPOVERLAPPED, LPDWORD, BOOL);
using FnGetOverlappedResultEx = BOOL(WINAPI*)(HANDLE, LPOVERLAPPED, LPDWORD, DWORD, BOOL);
using FnGetQueuedCompletionStatus = BOOL(WINAPI*)(HANDLE, LPDWORD, PULONG_PTR, LPOVERLAPPED*, DWORD);
using FnGetQueuedCompletionStatusEx = BOOL(WINAPI*)(HANDLE, CompletionEntry*, ULONG, PULONG, DWORD, BOOL);
using FnGetAdaptersInfo     = ULONG(WINAPI*)(PIP_ADAPTER_INFO, PULONG);
using FnGetAdaptersAddresses= ULONG(WINAPI*)(ULONG, ULONG, PVOID, PIP_ADAPTER_ADDRESSES, PULONG);
using FnCreateProcessA      = BOOL(WINAPI*)(LPCSTR, LPSTR, LPSECURITY_ATTRIBUTES, LPSECURITY_ATTRIBUTES, BOOL, DWORD, LPVOID, LPCSTR, LPSTARTUPINFOA, LPPROCESS_INFORMATION);
using FnCreateProcessW      = BOOL(WINAPI*)(LPCWSTR, LPWSTR, LPSECURITY_ATTRIBUTES, LPSECURITY_ATTRIBUTES, BOOL, DWORD, LPVOID, LPCWSTR, LPSTARTUPINFOW, LPPROCESS_INFORMATION);
using FnCreateProcessInternalW = BOOL(WINAPI*)(HANDLE, LPCWSTR, LPWSTR, LPSECURITY_ATTRIBUTES, LPSECURITY_ATTRIBUTES, BOOL, DWORD, LPVOID, LPCWSTR, LPSTARTUPINFOW, LPPROCESS_INFORMATION, PHANDLE);
using FnAttachConsole       = BOOL(WINAPI*)(DWORD);
using FnLoadLibraryExW      = HMODULE(WINAPI*)(LPCWSTR, HANDLE, DWORD);
using FnCreateEventExW      = HANDLE(WINAPI*)(LPSECURITY_ATTRIBUTES, LPCWSTR, DWORD, DWORD);
using FnOpenEventW          = HANDLE(WINAPI*)(DWORD, BOOL, LPCWSTR);
using FnCreateMutexExW      = HANDLE(WINAPI*)(LPSECURITY_ATTRIBUTES, LPCWSTR, DWORD, DWORD);
using FnOpenMutexW          = HANDLE(WINAPI*)(DWORD, BOOL, LPCWSTR);
using FnCreateFileMappingNumaW = HANDLE(WINAPI*)(HANDLE, LPSECURITY_ATTRIBUTES, DWORD, DWORD, DWORD, LPCWSTR, DWORD);
using FnOpenFileMappingW    = HANDLE(WINAPI*)(DWORD, BOOL, LPCWSTR);

// Each orig_* is either the detour trampoline or, if the detour could not be installed, the
// unpatched export itself (see hookExport), so it is never null once its detour can be reached.
FnSocket               orig_socket               = nullptr;
FnWSASocketA           orig_WSASocketA           = nullptr;
FnWSASocketW           orig_WSASocketW           = nullptr;
FnCloseSocket          orig_closesocket          = nullptr;
FnBind                 orig_bind                 = nullptr;
FnGetSockName          orig_getsockname          = nullptr;
FnGetPeerName          orig_getpeername          = nullptr;
FnSendTo               orig_sendto               = nullptr;
FnWSASendTo            orig_WSASendTo            = nullptr;
FnRecvFrom             orig_recvfrom             = nullptr;
FnWSARecvFrom          orig_WSARecvFrom          = nullptr;
FnSend                 orig_send                 = nullptr;
FnWSASend              orig_WSASend              = nullptr;
FnRecv                 orig_recv                 = nullptr;
FnWSARecv              orig_WSARecv              = nullptr;
FnConnect              orig_connect              = nullptr;
FnWSAConnect           orig_WSAConnect           = nullptr;
FnAccept               orig_accept               = nullptr;
FnWSAAccept            orig_WSAAccept            = nullptr;
FnGetHostByName        orig_gethostbyname        = nullptr;
FnGetAddrInfo          orig_getaddrinfo          = nullptr;
FnWSAIoctl             orig_WSAIoctl             = nullptr;
FnWSAGetOverlappedResult orig_WSAGetOverlappedResult = nullptr;
FnGetOverlappedResult  orig_GetOverlappedResult  = nullptr;
FnGetOverlappedResultEx orig_GetOverlappedResultEx = nullptr;
FnGetQueuedCompletionStatus orig_GetQueuedCompletionStatus = nullptr;
FnGetQueuedCompletionStatusEx orig_GetQueuedCompletionStatusEx = nullptr;
FnGetAdaptersInfo      orig_GetAdaptersInfo      = nullptr;
FnGetAdaptersAddresses orig_GetAdaptersAddresses = nullptr;
FnCreateProcessA       orig_CreateProcessA       = nullptr;
FnCreateProcessW       orig_CreateProcessW       = nullptr;
FnCreateProcessInternalW orig_CreateProcessInternalW = nullptr;
FnAttachConsole        orig_AttachConsole        = nullptr;
FnLoadLibraryExW       orig_LoadLibraryExW       = nullptr;
FnCreateEventExW       orig_CreateEventExW       = nullptr;
FnOpenEventW           orig_OpenEventW           = nullptr;
FnCreateMutexExW       orig_CreateMutexExW       = nullptr;
FnOpenMutexW           orig_OpenMutexW           = nullptr;
FnCreateFileMappingNumaW orig_CreateFileMappingNumaW = nullptr;
FnOpenFileMappingW     orig_OpenFileMappingW     = nullptr;

void disableUdpConnReset(SOCKET s) {
    if (!orig_WSAIoctl) return;
    BOOL new_behavior = FALSE;
    DWORD bytes_returned = 0;
    orig_WSAIoctl(s, SIO_UDP_CONNRESET, &new_behavior, sizeof(new_behavior),
                  nullptr, 0, &bytes_returned, nullptr, nullptr);
}

// Ensure we have state for socket `s`, querying family/type if it was created before the hook
// (or is an IPv6 socket, which hooked_socket does not register)
VirtualSocketState getOrDiscoverSocketState(SOCKET s) {
    VirtualSocketState st{};
    uint64_t h = static_cast<uint64_t>(s);
    if (SocketShimCore::instance().getSocketState(h, st)) {
        return st;
    }
    WSAPROTOCOL_INFOW info{};
    int optlen = sizeof(info);
    if (::getsockopt(s, SOL_SOCKET, SO_PROTOCOL_INFOW, reinterpret_cast<char*>(&info), &optlen) == 0) {
        SocketShimCore::instance().registerSocket(h, info.iAddressFamily, info.iSocketType);
        SocketShimCore::instance().getSocketState(h, st);
    }
    return st;
}

bool isVirtualIpv4Socket(const VirtualSocketState& st) {
    return st.af == AF_INET && st.is_virtualized && !st.passthrough;
}

bool isVirtualUdpSocket(const VirtualSocketState& st) {
    return st.type == SOCK_DGRAM && isVirtualIpv4Socket(st);
}

// Bind `s` to 127.0.0.1:0 and return the OS-assigned port. A socket the OS already bound
// implicitly (e.g. an earlier real sendto() bound it to 0.0.0.0:P) keeps its port instead:
// VpnClient reaches it on 127.0.0.1:P either way, and a second bind would fail.
bool bindLoopbackOrAdopt(SOCKET s, uint16_t& out_port) {
    sockaddr_in loop_addr{};
    loop_addr.sin_family = AF_INET;
    loop_addr.sin_addr.s_addr = htonl(kLoopbackIp);
    loop_addr.sin_port = 0;

    const bool bound_now = orig_bind(s, reinterpret_cast<const sockaddr*>(&loop_addr), sizeof(loop_addr)) == 0;
    if (!bound_now && WSAGetLastError() != WSAEINVAL) {
        return false;
    }

    sockaddr_in actual_addr{};
    int actual_len = sizeof(actual_addr);
    if (orig_getsockname(s, reinterpret_cast<sockaddr*>(&actual_addr), &actual_len) != 0 ||
        actual_addr.sin_family != AF_INET || actual_addr.sin_port == 0) {
        if (!bound_now) WSASetLastError(WSAEINVAL);
        return false;
    }
    const uint32_t actual_ip = ntohl(actual_addr.sin_addr.s_addr);
    if (!bound_now && actual_ip != 0 && actual_ip != kLoopbackIp) {
        // Already bound to a real interface address: loopback traffic cannot reach it
        WSASetLastError(WSAEINVAL);
        return false;
    }
    out_port = ntohs(actual_addr.sin_port);
    return true;
}

// Serializes implicit binds, so two threads doing the first sendto() on the same unbound socket
// cannot both bind it (the loser would fail) or register it twice. Rare (once per socket), so a
// single process-wide lock is enough.
std::mutex g_implicit_bind_mutex;

// Bind an unbound socket to 127.0.0.1:0 and register its ephemeral port with VpnClient.
// On success `st` holds the refreshed state; on failure the Winsock error is set.
bool ensureImplicitLoopbackBind(SOCKET s, VirtualSocketState& st) {
    if (st.is_virtualized && st.actual_loopback_port != 0) {
        return true;
    }

    auto& shim = SocketShimCore::instance();
    const uint64_t h = static_cast<uint64_t>(s);
    std::lock_guard<std::mutex> guard(g_implicit_bind_mutex);

    VirtualSocketState cur{};
    if (!shim.getSocketState(h, cur) || cur.passthrough) {
        WSASetLastError(WSAEINVAL);
        return false;
    }
    if (cur.is_virtualized && cur.actual_loopback_port != 0) {
        st = cur; // another thread finished the bind meanwhile
        return true;
    }

    uint16_t actual_port = cur.actual_loopback_port;
    if (!cur.is_bound || actual_port == 0) {
        if (cur.type == SOCK_DGRAM) {
            disableUdpConnReset(s);
        }
        if (!bindLoopbackOrAdopt(s, actual_port)) {
            return false;
        }
        shim.modifySocketState(h, [&](VirtualSocketState& x) {
            x.is_bound = true;
            x.actual_loopback_port = actual_port;
        });
    }

    // Bound but not registered yet (or VpnClient was unreachable last time): (re)register
    uint16_t assigned_vport = actual_port;
    IpcProto proto = (cur.type == SOCK_STREAM) ? IpcProto::Tcp : IpcProto::Udp;
    if (!shim.ipcRegisterBind(proto, actual_port, actual_port, assigned_vport)) {
        hookLog("RegisterBind failed for an implicit bind");
        WSASetLastError(WSAENETUNREACH);
        return false;
    }

    const uint32_t local_vip = shim.localVip();
    shim.modifySocketState(h, [&](VirtualSocketState& x) {
        x.is_bound = true;
        x.is_virtualized = true;
        x.owns_binding = true;
        x.bound_vip = local_vip;
        x.virtual_port = assigned_vport;
        x.actual_loopback_port = actual_port;
    });
    shim.getSocketState(h, st);
    return true;
}

enum class UdpRoute { Real, Virtual, Failed };

// Decide whether a datagram from `s` to dst_ip:dst_port travels over the virtual LAN.
// - Stream sockets, non-IPv4 sockets and sockets explicitly bound to a real address: Real.
// - 127.0.0.1:X is virtual only if a hooked socket of this client holds virtual UDP port X
//   (a game talking to its own server); otherwise it is a real local service.
// On Virtual the socket is bound/registered, `st` is current and `out_dst_vip` is set.
// On Failed the Winsock error is set.
UdpRoute resolveUdpRoute(SOCKET s, uint32_t dst_ip, uint16_t dst_port,
                         VirtualSocketState& st, uint32_t& out_dst_vip) {
    st = getOrDiscoverSocketState(s);
    if (st.type != SOCK_DGRAM || st.af != AF_INET || st.passthrough) {
        return UdpRoute::Real;
    }
    if (dst_ip == kLoopbackIp) {
        if (!SocketShimCore::instance().ipcQueryBind(IpcProto::Udp, dst_port)) {
            return UdpRoute::Real;
        }
        out_dst_vip = SocketShimCore::instance().localVip();
    } else if (isVpnTargetIp(dst_ip)) {
        out_dst_vip = dst_ip;
    } else {
        return UdpRoute::Real;
    }
    if (!ensureImplicitLoopbackBind(s, st)) {
        return UdpRoute::Failed;
    }
    return UdpRoute::Virtual;
}

sockaddr_in udpGatewayAddr() {
    sockaddr_in gw_addr{};
    gw_addr.sin_family = AF_INET;
    gw_addr.sin_addr.s_addr = htonl(kLoopbackIp);
    gw_addr.sin_port = htons(SocketShimCore::instance().udpGatewayPort());
    return gw_addr;
}

LoopbackUdpHeader makeLoopbackHeader(const VirtualSocketState& st, uint32_t dst_vip, uint16_t dst_vport) {
    LoopbackUdpHeader hdr{};
    hdr.magic = kLoopbackUdpMagic;
    hdr.src_vip = SocketShimCore::instance().localVip();
    hdr.dst_vip = dst_vip;
    hdr.src_vport = st.virtual_port;
    hdr.dst_vport = dst_vport;
    return hdr;
}

// True if a received datagram came from VpnClient's UDP gateway (127.0.0.1:APIVPN_UDP_GW_PORT)
// and starts with a loopback header. Anything else on a virtualized socket (a real local sender)
// is delivered unchanged, with its real source address.
bool isGatewayDatagram(const sockaddr_storage& from, int from_len, const uint8_t* data, size_t len) {
    if (len < kUdpHdrLen || from.ss_family != AF_INET || from_len < static_cast<int>(sizeof(sockaddr_in))) {
        return false;
    }
    const auto* sin = reinterpret_cast<const sockaddr_in*>(&from);
    if (ntohl(sin->sin_addr.s_addr) != kLoopbackIp ||
        ntohs(sin->sin_port) != SocketShimCore::instance().udpGatewayPort()) {
        return false;
    }
    uint32_t magic = 0;
    std::memcpy(&magic, data, sizeof(magic));
    return magic == kLoopbackUdpMagic;
}

// Fill a caller's from/fromlen with a virtual IPv4 source (only if it fits, like recvfrom()).
void writeVirtualSource(sockaddr* out, int* out_len, uint32_t vip, uint16_t vport) {
    if (!out || !out_len || *out_len < static_cast<int>(sizeof(sockaddr_in))) return;
    sockaddr_in sin_out{};
    sin_out.sin_family = AF_INET;
    sin_out.sin_addr.s_addr = htonl(vip);
    sin_out.sin_port = htons(vport);
    std::memcpy(out, &sin_out, sizeof(sin_out));
    *out_len = static_cast<int>(sizeof(sin_out));
}

void copyRealSource(sockaddr* out, int* out_len, const sockaddr_storage& from, int from_len) {
    if (!out || !out_len || from_len <= 0 || *out_len < from_len) return;
    std::memcpy(out, &from, static_cast<size_t>(from_len));
    *out_len = from_len;
}

size_t scatterToWsaBufs(const WSABUF* bufs, DWORD count, const uint8_t* src, size_t len) {
    size_t copied = 0;
    for (DWORD i = 0; i < count && copied < len; ++i) {
        const size_t chunk = std::min<size_t>(len - copied, bufs[i].len);
        if (chunk > 0 && bufs[i].buf) {
            std::memcpy(bufs[i].buf, src + copied, chunk);
            copied += chunk;
        }
    }
    return copied;
}

// The hook needs its own Winsock reference: a process may talk to VpnClient (e.g. to register a
// child process) without ever having called WSAStartup itself. Taken lazily on the first IPC,
// never from DllMain (WSAStartup must not run under the loader lock), and never released.
bool ensureHookWinsockStarted() {
    static const bool started = [] {
        WSADATA wsa{};
        return WSAStartup(MAKEWORD(2, 2), &wsa) == 0;
    }();
    return started;
}

// Raw IPC exchange using original unhooked Winsock trampolines
bool rawIpcExchangeWin32(uint16_t ipc_port, const IpcRequest& req, IpcResponse& resp) {
    ScopedHookBypass bypass;
    if (!orig_socket || !orig_connect || !orig_send || !orig_recv || !orig_closesocket) {
        return false;
    }
    if (!ensureHookWinsockStarted()) {
        return false;
    }

    SOCKET s = orig_socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (s == INVALID_SOCKET) return false;

    BOOL nodelay = TRUE;
    ::setsockopt(s, IPPROTO_TCP, TCP_NODELAY, reinterpret_cast<const char*>(&nodelay), sizeof(nodelay));

    DWORD timeout_ms = 3000;
    ::setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char*>(&timeout_ms), sizeof(timeout_ms));
    ::setsockopt(s, SOL_SOCKET, SO_SNDTIMEO, reinterpret_cast<const char*>(&timeout_ms), sizeof(timeout_ms));

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(kLoopbackIp);
    addr.sin_port = htons(ipc_port);

    if (orig_connect(s, reinterpret_cast<const sockaddr*>(&addr), sizeof(addr)) != 0) {
        orig_closesocket(s);
        return false;
    }

    int sent = orig_send(s, reinterpret_cast<const char*>(&req), sizeof(req), 0);
    if (sent != sizeof(req)) {
        orig_closesocket(s);
        return false;
    }

    char* dst = reinterpret_cast<char*>(&resp);
    int total = 0;
    while (total < static_cast<int>(sizeof(resp))) {
        int got = orig_recv(s, dst + total, static_cast<int>(sizeof(resp)) - total, 0);
        if (got <= 0) {
            orig_closesocket(s);
            return false;
        }
        total += got;
    }

    orig_closesocket(s);
    return true;
}

// ============================================================================
// Overlapped I/O on virtualized UDP sockets
// ============================================================================
// Virtual datagrams carry a 16-byte LoopbackUdpHeader on the loopback wire. Synchronous calls
// add/strip it inline. Overlapped calls complete later, so each one is issued as scatter/gather
// I/O with WSABUF[0] = a per-operation header slot followed by the app's own buffers (receives
// also get a per-operation source-address buffer). The operation is recorded by its OVERLAPPED
// and fixed up exactly once, the first time its completion is observed through
// WSAGetOverlappedResult, GetOverlappedResult(Ex), GetQueuedCompletionStatus(Ex) or the
// (wrapped) completion routine: the header is subtracted from the byte count, the virtual source
// is written into the app's lpFrom, and OVERLAPPED::InternalHigh is rewritten so later queries
// agree. Datagrams that did not come from the UDP gateway are shifted back into the app's
// buffers and delivered unchanged with their real source.
//
// Known limitations:
// - An app that polls OVERLAPPED::Internal/InternalHigh directly (HasOverlappedIoCompleted)
//   without ever calling one of the APIs above sees the raw byte count (+16) and, for receives,
//   the header in its buffers / the gateway as source.
// - Thread-pool I/O (CreateThreadpoolIo, BindIoCompletionCallback) dequeues completions inside
//   ntdll, not through GetQueuedCompletionStatus, so its callbacks see raw counts as well.

struct OverlappedUdpOp {
    SOCKET    s           = INVALID_SOCKET;
    bool      is_send     = false;
    bool      fixed       = false; // completion already post-processed
    bool      truncated   = false; // a raw datagram was cut to fit the app's buffers
    bool      orphaned    = false; // socket closed while the operation was outstanding
    DWORD     final_bytes = 0;     // byte count reported to the app once fixed
    ULONGLONG tick        = 0;     // when it was fixed / orphaned (for the purge)
    LPWSAOVERLAPPED_COMPLETION_ROUTINE app_routine = nullptr;
    LoopbackUdpHeader hdr{};       // send: gathered in front of the payload; recv: scattered into
    // Receive only
    std::vector<WSABUF> app_bufs;
    sockaddr*        app_from     = nullptr;
    LPINT            app_fromlen  = nullptr;
    sockaddr_storage raw_from{};
    INT              raw_from_len = 0;
};
using OverlappedUdpOpPtr = std::shared_ptr<OverlappedUdpOp>;

// Fixed or orphaned records that no completion path consumed (event/poll-based apps) are dropped
// after this long. Records with a completion routine are only ever consumed by the routine thunk.
constexpr ULONGLONG kOverlappedRecordRetentionMs = 10000;

std::mutex g_ovl_mutex;
std::unordered_map<const void*, OverlappedUdpOpPtr> g_ovl_ops; // keyed by the app's OVERLAPPED
std::atomic<size_t> g_ovl_op_count{0}; // lets the completion detours skip the lock when idle
ULONGLONG g_ovl_last_purge = 0;

void publishOverlappedCountLocked() {
    g_ovl_op_count.store(g_ovl_ops.size(), std::memory_order_release);
}

void purgeOverlappedOpsLocked() {
    const ULONGLONG now = GetTickCount64();
    if (now - g_ovl_last_purge < 1000) return;
    g_ovl_last_purge = now;
    for (auto it = g_ovl_ops.begin(); it != g_ovl_ops.end();) {
        const OverlappedUdpOp& op = *it->second;
        if (!op.app_routine && (op.fixed || op.orphaned) && now - op.tick >= kOverlappedRecordRetentionMs) {
            it = g_ovl_ops.erase(it);
        } else {
            ++it;
        }
    }
}

void registerOverlappedOp(LPWSAOVERLAPPED ovl, const OverlappedUdpOpPtr& op) {
    std::lock_guard<std::mutex> lock(g_ovl_mutex);
    purgeOverlappedOpsLocked();
    // Reusing an OVERLAPPED means its previous operation has completed: replace that record
    g_ovl_ops[ovl] = op;
    publishOverlappedCountLocked();
}

void unregisterOverlappedOp(LPWSAOVERLAPPED ovl, const OverlappedUdpOpPtr& op) {
    std::lock_guard<std::mutex> lock(g_ovl_mutex);
    auto it = g_ovl_ops.find(ovl);
    if (it != g_ovl_ops.end() && it->second == op) {
        g_ovl_ops.erase(it);
        publishOverlappedCountLocked();
    }
}

// closesocket() cancels outstanding I/O; the cancellations still complete (and may still be
// observed), so the records stay until consumed or purged.
void orphanOverlappedOps(SOCKET s) {
    if (g_ovl_op_count.load(std::memory_order_acquire) == 0) return;
    std::lock_guard<std::mutex> lock(g_ovl_mutex);
    const ULONGLONG now = GetTickCount64();
    for (auto& entry : g_ovl_ops) {
        OverlappedUdpOp& op = *entry.second;
        if (op.s == s && !op.orphaned) {
            op.orphaned = true;
            if (!op.fixed) op.tick = now;
        }
    }
}

// The scatter read put the first 16 bytes of a datagram that is not virtual-LAN traffic into our
// header slot: shift everything back into the app's buffers and report the real source.
DWORD deliverRawDatagramLocked(OverlappedUdpOp& op, DWORD raw_bytes) {
    size_t app_cap = 0;
    for (const WSABUF& b : op.app_bufs) {
        app_cap += b.len;
    }
    if (raw_bytes > 0) {
        std::vector<uint8_t> flat(raw_bytes);
        const size_t head = std::min<size_t>(raw_bytes, kUdpHdrLen);
        std::memcpy(flat.data(), &op.hdr, head);
        size_t gathered = head;
        for (const WSABUF& b : op.app_bufs) {
            if (gathered >= raw_bytes) break;
            const size_t chunk = std::min<size_t>(raw_bytes - gathered, b.len);
            if (chunk > 0 && b.buf) {
                std::memcpy(flat.data() + gathered, b.buf, chunk);
                gathered += chunk;
            }
        }
        scatterToWsaBufs(op.app_bufs.data(), static_cast<DWORD>(op.app_bufs.size()),
                         flat.data(), std::min(gathered, app_cap));
    }
    if (raw_bytes > app_cap) {
        op.truncated = true;
    }
    copyRealSource(op.app_from, op.app_fromlen, op.raw_from, op.raw_from_len);
    return static_cast<DWORD>(std::min<size_t>(raw_bytes, app_cap));
}

// Post-process a completed operation exactly once; later observers get the same final count.
DWORD fixupOverlappedOpLocked(OverlappedUdpOp& op, LPOVERLAPPED ovl, DWORD raw_bytes, bool data_valid) {
    if (op.fixed) {
        return op.final_bytes;
    }
    op.fixed = true;
    op.tick = GetTickCount64();
    if (op.is_send) {
        op.final_bytes = (raw_bytes >= kUdpHdrLen) ? raw_bytes - kUdpHdrLen : 0;
    } else if (!data_valid) {
        op.final_bytes = 0;
    } else if (isGatewayDatagram(op.raw_from, op.raw_from_len,
                                 reinterpret_cast<const uint8_t*>(&op.hdr), raw_bytes)) {
        writeVirtualSource(op.app_from, op.app_fromlen, op.hdr.src_vip, op.hdr.src_vport);
        op.final_bytes = raw_bytes - kUdpHdrLen;
    } else {
        op.final_bytes = deliverRawDatagramLocked(op, raw_bytes);
    }
    if (ovl) {
        ovl->InternalHigh = op.final_bytes;
        if (op.truncated) {
            ovl->Internal = kStatusBufferOverflow;
        }
    }
    return op.final_bytes;
}

// Apply the deferred fix-up when the app observes a completion. `expected_socket` (unless
// INVALID_SOCKET) guards against a stale record for an OVERLAPPED reused on another handle.
// `consume` drops the record (a dequeued IOCP packet is the operation's last notification).
// Returns false if the operation is not one of ours; nothing is changed then.
bool observeOverlappedCompletion(LPOVERLAPPED ovl, SOCKET expected_socket, DWORD& inout_bytes,
                                 bool data_valid, bool consume, bool truncated_hint,
                                 bool& out_truncated) {
    std::lock_guard<std::mutex> lock(g_ovl_mutex);
    auto it = g_ovl_ops.find(ovl);
    if (it == g_ovl_ops.end()) {
        return false;
    }
    OverlappedUdpOpPtr op = it->second;
    if (expected_socket != INVALID_SOCKET && op->s != expected_socket) {
        return false;
    }
    // The OS truncates a datagram to the combined header + app-buffer capacity, so the
    // raw byte count alone cannot tell an exactly-sized datagram from a truncated one.
    // The completion status (WSAEMSGSIZE / ERROR_MORE_DATA / STATUS_BUFFER_OVERFLOW) is
    // the authoritative signal.
    if (truncated_hint) op->truncated = true;
    inout_bytes = fixupOverlappedOpLocked(*op, ovl, inout_bytes, data_valid);
    out_truncated = op->truncated;
    if (consume && !op->app_routine) {
        g_ovl_ops.erase(it);
        publishOverlappedCountLocked();
    }
    return true;
}

// Wraps the app's completion routine for overlapped I/O on virtualized UDP sockets.
void CALLBACK overlappedUdpCompletionThunk(DWORD dwError, DWORD cbTransferred,
                                           LPWSAOVERLAPPED lpOverlapped, DWORD dwFlags) {
    LPWSAOVERLAPPED_COMPLETION_ROUTINE app_routine = nullptr;
    DWORD bytes = cbTransferred;
    DWORD error = dwError;
    DWORD flags = dwFlags;
    {
        std::lock_guard<std::mutex> lock(g_ovl_mutex);
        auto it = g_ovl_ops.find(lpOverlapped);
        if (it != g_ovl_ops.end()) {
            OverlappedUdpOpPtr op = it->second;
            const bool data_valid = (dwError == 0 || dwError == static_cast<DWORD>(WSAEMSGSIZE));
            if (dwError == static_cast<DWORD>(WSAEMSGSIZE)) op->truncated = true;
            bytes = fixupOverlappedOpLocked(*op, lpOverlapped, cbTransferred, data_valid);
            if (op->truncated) {
                error = static_cast<DWORD>(WSAEMSGSIZE);
                flags |= static_cast<DWORD>(MSG_PARTIAL);
            }
            app_routine = op->app_routine;
            // Erase before calling the app: its routine commonly re-issues I/O on the same OVERLAPPED
            g_ovl_ops.erase(it);
            publishOverlappedCountLocked();
        }
    }
    if (!app_routine) {
        hookLog("completion routine record missing; completion dropped");
        return;
    }
    // The APC may run during an alertable wait inside one of our own (bypassed) Winsock calls;
    // the app's routine must still go through its hooks.
    const bool prev_bypass = g_bypass_hooks;
    g_bypass_hooks = false;
    app_routine(error, bytes, lpOverlapped, flags);
    g_bypass_hooks = prev_bypass;
}

LPWSAOVERLAPPED_COMPLETION_ROUTINE completionThunkFor(LPWSAOVERLAPPED_COMPLETION_ROUTINE app_routine) {
    LPWSAOVERLAPPED_COMPLETION_ROUTINE thunk = nullptr;
    if (app_routine) {
        thunk = &overlappedUdpCompletionThunk;
    }
    return thunk;
}

// Common tail of an overlapped WSASendTo/WSARecvFrom issued with `op` already registered.
int finishOverlappedStart(LPWSAOVERLAPPED ovl, const OverlappedUdpOpPtr& op, int rc,
                          DWORD raw_bytes, LPDWORD lpNumberOfBytes) {
    const int err = (rc == 0) ? 0 : WSAGetLastError();
    if (rc == 0 || err == WSAEMSGSIZE) {
        // Completed at once. The regular completion notification still follows; the fix-up is
        // idempotent, so whichever path runs first does the work.
        if (rc != 0) {
            raw_bytes = static_cast<DWORD>(ovl->InternalHigh);
        }
        DWORD final_bytes = 0;
        {
            std::lock_guard<std::mutex> lock(g_ovl_mutex);
            if (err == WSAEMSGSIZE) op->truncated = true;
            final_bytes = fixupOverlappedOpLocked(*op, ovl, raw_bytes, true);
        }
        if (lpNumberOfBytes) {
            *lpNumberOfBytes = final_bytes;
        }
    } else if (err != WSA_IO_PENDING) {
        // Failed outright: Winsock queues no completion for it
        unregisterOverlappedOp(ovl, op);
    }
    if (rc != 0) {
        WSASetLastError(err);
    }
    return rc;
}

int startOverlappedUdpRecv(
    SOCKET s, LPWSABUF lpBuffers, DWORD dwBufferCount, LPDWORD lpNumberOfBytesRecvd,
    LPDWORD lpFlags, sockaddr* lpFrom, LPINT lpFromlen,
    LPWSAOVERLAPPED lpOverlapped, LPWSAOVERLAPPED_COMPLETION_ROUTINE lpCompletionRoutine) {

    if (!lpCompletionRoutine && !g_overlapped_fixup_ok.load(std::memory_order_acquire)) {
        // No completion detours: nothing would ever fix this receive up, so keep the
        // pre-virtualization behaviour (the app sees the raw loopback header).
        return orig_WSARecvFrom(s, lpBuffers, dwBufferCount, lpNumberOfBytesRecvd, lpFlags,
                                lpFrom, lpFromlen, lpOverlapped, lpCompletionRoutine);
    }

    if (lpFrom && (!lpFromlen || *lpFromlen < static_cast<INT>(sizeof(sockaddr_in)))) {
        WSASetLastError(WSAEFAULT);
        return SOCKET_ERROR;
    }

    ScopedHookBypass bypass;
    auto op = std::make_shared<OverlappedUdpOp>();
    op->s = s;
    op->app_routine = lpCompletionRoutine;
    op->app_bufs.assign(lpBuffers, lpBuffers + dwBufferCount);
    op->app_from = lpFrom;
    op->app_fromlen = lpFrom ? lpFromlen : nullptr;
    op->raw_from_len = static_cast<INT>(sizeof(op->raw_from));
    HeaderedWsaBufs bufs(&op->hdr, lpBuffers, dwBufferCount);
    // Registered before the call: another thread may observe the completion before we return
    registerOverlappedOp(lpOverlapped, op);

    DWORD raw_recvd = 0;
    const int rc = orig_WSARecvFrom(s, bufs.data(), bufs.count(), &raw_recvd, lpFlags,
                                    reinterpret_cast<sockaddr*>(&op->raw_from), &op->raw_from_len,
                                    lpOverlapped, completionThunkFor(lpCompletionRoutine));
    return finishOverlappedStart(lpOverlapped, op, rc, raw_recvd, lpNumberOfBytesRecvd);
}

// ============================================================================
// Detour Implementations
// ============================================================================

SOCKET WSAAPI hooked_socket(int af, int type, int protocol) {
    SOCKET s = orig_socket(af, type, protocol);
    if (!g_bypass_hooks && s != INVALID_SOCKET && af == AF_INET && SocketShimCore::instance().isInitialized()) {
        if (type == SOCK_DGRAM) {
            disableUdpConnReset(s);
        }
        SocketShimCore::instance().registerSocket(static_cast<uint64_t>(s), af, type);
    }
    return s;
}

SOCKET WSAAPI hooked_WSASocketA(int af, int type, int protocol, LPWSAPROTOCOL_INFOA lpProtocolInfo, GROUP g, DWORD dwFlags) {
    SOCKET s = orig_WSASocketA(af, type, protocol, lpProtocolInfo, g, dwFlags);
    if (!g_bypass_hooks && s != INVALID_SOCKET && af == AF_INET && SocketShimCore::instance().isInitialized()) {
        if (type == SOCK_DGRAM) {
            disableUdpConnReset(s);
        }
        SocketShimCore::instance().registerSocket(static_cast<uint64_t>(s), af, type);
    }
    return s;
}

SOCKET WSAAPI hooked_WSASocketW(int af, int type, int protocol, LPWSAPROTOCOL_INFOW lpProtocolInfo, GROUP g, DWORD dwFlags) {
    SOCKET s = orig_WSASocketW(af, type, protocol, lpProtocolInfo, g, dwFlags);
    if (!g_bypass_hooks && s != INVALID_SOCKET && af == AF_INET && SocketShimCore::instance().isInitialized()) {
        if (type == SOCK_DGRAM) {
            disableUdpConnReset(s);
        }
        SocketShimCore::instance().registerSocket(static_cast<uint64_t>(s), af, type);
    }
    return s;
}

int WSAAPI hooked_closesocket(SOCKET s) {
    if (!g_bypass_hooks && s != INVALID_SOCKET) {
        ScopedHookBypass bypass;
        orphanOverlappedOps(s);
        SocketShimCore::instance().unregisterSocket(static_cast<uint64_t>(s));
    }
    return orig_closesocket(s);
}

int WSAAPI hooked_bind(SOCKET s, const sockaddr* name, int namelen) {
    if (g_bypass_hooks || !SocketShimCore::instance().isInitialized() ||
        !name || namelen < static_cast<int>(sizeof(sockaddr_in)) || name->sa_family != AF_INET) {
        return orig_bind(s, name, namelen);
    }

    const auto* sin = reinterpret_cast<const sockaddr_in*>(name);
    uint32_t req_ip = ntohl(sin->sin_addr.s_addr);
    uint16_t req_port = ntohs(sin->sin_port);
    auto& shim = SocketShimCore::instance();
    const uint64_t h = static_cast<uint64_t>(s);

    // Intercept binds to 0.0.0.0 (INADDR_ANY), virtual subnet 10.42.0.x, or broadcast. An explicit
    // real address (127.0.0.1, a physical NIC) binds untouched and the socket stays a plain OS
    // socket for good: no implicit loopback bind or header handling is ever applied to it.
    if (req_ip != 0 && !isVirtualUnicast(req_ip) && !isBroadcastOrMulticast(req_ip)) {
        int rc = orig_bind(s, name, namelen);
        if (rc == 0) {
            ScopedHookBypass bypass;
            getOrDiscoverSocketState(s);
            shim.modifySocketState(h, [](VirtualSocketState& x) { x.passthrough = true; });
        }
        return rc;
    }

    ScopedHookBypass bypass;
    VirtualSocketState st = getOrDiscoverSocketState(s);
    if (st.af != AF_INET) {
        return orig_bind(s, name, namelen);
    }
    if (st.type == SOCK_DGRAM) {
        disableUdpConnReset(s);
    }

    // Bind real OS socket to 127.0.0.1:0 so multiple clients/games never collide on physical ports
    sockaddr_in loop_addr{};
    loop_addr.sin_family = AF_INET;
    loop_addr.sin_addr.s_addr = htonl(kLoopbackIp);
    loop_addr.sin_port = 0;

    int rc = orig_bind(s, reinterpret_cast<const sockaddr*>(&loop_addr), sizeof(loop_addr));
    if (rc != 0) {
        return rc;
    }

    sockaddr_in actual_addr{};
    int actual_len = sizeof(actual_addr);
    if (orig_getsockname(s, reinterpret_cast<sockaddr*>(&actual_addr), &actual_len) != 0) {
        return SOCKET_ERROR;
    }

    uint16_t actual_port = ntohs(actual_addr.sin_port);
    uint16_t vport = (req_port == 0) ? actual_port : req_port;
    uint16_t assigned_vport = vport;

    IpcProto proto = (st.type == SOCK_STREAM) ? IpcProto::Tcp : IpcProto::Udp;
    if (!shim.ipcRegisterBind(proto, vport, actual_port, assigned_vport)) {
        // VpnClient does not route to this socket, so it is unreachable on the virtual LAN. A bind
        // cannot be undone: leave it as a plain loopback socket and tell the app the bind failed.
        shim.modifySocketState(h, [&](VirtualSocketState& x) {
            x.is_bound = true;
            x.passthrough = true;
            x.actual_loopback_port = actual_port;
        });
        hookLog("RegisterBind failed; bind() reported WSAEADDRNOTAVAIL");
        WSASetLastError(WSAEADDRNOTAVAIL);
        return SOCKET_ERROR;
    }

    shim.modifySocketState(h, [&](VirtualSocketState& x) {
        x.is_bound = true;
        x.is_virtualized = true;
        x.owns_binding = true;
        x.bound_vip = req_ip;
        x.virtual_port = assigned_vport;
        x.actual_loopback_port = actual_port;
    });
    return 0;
}

int WSAAPI hooked_getsockname(SOCKET s, sockaddr* name, int* namelen) {
    if (g_bypass_hooks) {
        return orig_getsockname(s, name, namelen);
    }
    ScopedHookBypass bypass;
    int rc = orig_getsockname(s, name, namelen);
    if (rc == 0 && SocketShimCore::instance().isInitialized() &&
        name && namelen && *namelen >= static_cast<int>(sizeof(sockaddr_in))) {
        VirtualSocketState st{};
        if (SocketShimCore::instance().getSocketState(static_cast<uint64_t>(s), st) &&
            isVirtualIpv4Socket(st) && name->sa_family == AF_INET) {
            auto* sin = reinterpret_cast<sockaddr_in*>(name);
            uint32_t report_ip = (st.bound_vip != 0) ? st.bound_vip :
                                 (st.has_remote_peer ? SocketShimCore::instance().localVip() : 0);
            sin->sin_addr.s_addr = htonl(report_ip);
            sin->sin_port = htons(st.virtual_port);
        }
    }
    return rc;
}

int WSAAPI hooked_getpeername(SOCKET s, sockaddr* name, int* namelen) {
    if (g_bypass_hooks) {
        return orig_getpeername(s, name, namelen);
    }
    ScopedHookBypass bypass;
    int rc = orig_getpeername(s, name, namelen);
    if (rc == 0 && SocketShimCore::instance().isInitialized() &&
        name && namelen && *namelen >= static_cast<int>(sizeof(sockaddr_in))) {
        VirtualSocketState st{};
        if (SocketShimCore::instance().getSocketState(static_cast<uint64_t>(s), st) &&
            st.af == AF_INET && !st.passthrough && st.has_remote_peer && name->sa_family == AF_INET) {
            auto* sin = reinterpret_cast<sockaddr_in*>(name);
            sin->sin_addr.s_addr = htonl(st.remote_vip);
            sin->sin_port = htons(st.remote_vport);
        }
    }
    return rc;
}

int WSAAPI hooked_sendto(SOCKET s, const char* buf, int len, int flags, const sockaddr* to, int tolen) {
    if (g_bypass_hooks || !SocketShimCore::instance().isInitialized() ||
        !to || tolen < static_cast<int>(sizeof(sockaddr_in)) || to->sa_family != AF_INET) {
        return orig_sendto(s, buf, len, flags, to, tolen);
    }

    const auto* sin_to = reinterpret_cast<const sockaddr_in*>(to);
    uint32_t dst_ip = ntohl(sin_to->sin_addr.s_addr);
    uint16_t dst_port = ntohs(sin_to->sin_port);
    if (!isVpnTargetIp(dst_ip) && dst_ip != kLoopbackIp) {
        return orig_sendto(s, buf, len, flags, to, tolen);
    }

    ScopedHookBypass bypass;
    VirtualSocketState st{};
    uint32_t dst_vip = 0;
    const UdpRoute route = resolveUdpRoute(s, dst_ip, dst_port, st, dst_vip);
    if (route == UdpRoute::Real) {
        // Includes stream sockets: Winsock ignores `to` there, so never prepend a header
        return orig_sendto(s, buf, len, flags, to, tolen);
    }
    if (route == UdpRoute::Failed) {
        return SOCKET_ERROR;
    }
    if (len < 0 || (len > 0 && !buf)) {
        WSASetLastError(WSAEFAULT);
        return SOCKET_ERROR;
    }

    const LoopbackUdpHeader hdr = makeLoopbackHeader(st, dst_vip, dst_port);
    ScratchBuffer packet(sizeof(hdr) + static_cast<size_t>(len));
    std::memcpy(packet.data(), &hdr, sizeof(hdr));
    if (len > 0) {
        std::memcpy(packet.data() + sizeof(hdr), buf, static_cast<size_t>(len));
    }

    const sockaddr_in gw_addr = udpGatewayAddr();
    int sent = orig_sendto(s, reinterpret_cast<const char*>(packet.data()),
                           static_cast<int>(packet.size()), flags,
                           reinterpret_cast<const sockaddr*>(&gw_addr), sizeof(gw_addr));
    if (sent == SOCKET_ERROR) {
        return SOCKET_ERROR;
    }
    return (sent >= static_cast<int>(sizeof(hdr))) ? sent - static_cast<int>(sizeof(hdr)) : 0;
}

int WSAAPI hooked_WSASendTo(
    SOCKET s, LPWSABUF lpBuffers, DWORD dwBufferCount, LPDWORD lpNumberOfBytesSent,
    DWORD dwFlags, const sockaddr* lpTo, int iTolen,
    LPWSAOVERLAPPED lpOverlapped, LPWSAOVERLAPPED_COMPLETION_ROUTINE lpCompletionRoutine) {

    if (g_bypass_hooks || !SocketShimCore::instance().isInitialized() ||
        !lpTo || iTolen < static_cast<int>(sizeof(sockaddr_in)) || lpTo->sa_family != AF_INET) {
        return orig_WSASendTo(s, lpBuffers, dwBufferCount, lpNumberOfBytesSent, dwFlags,
                              lpTo, iTolen, lpOverlapped, lpCompletionRoutine);
    }

    const auto* sin_to = reinterpret_cast<const sockaddr_in*>(lpTo);
    uint32_t dst_ip = ntohl(sin_to->sin_addr.s_addr);
    uint16_t dst_port = ntohs(sin_to->sin_port);
    if (!isVpnTargetIp(dst_ip) && dst_ip != kLoopbackIp) {
        return orig_WSASendTo(s, lpBuffers, dwBufferCount, lpNumberOfBytesSent, dwFlags,
                              lpTo, iTolen, lpOverlapped, lpCompletionRoutine);
    }

    ScopedHookBypass bypass;
    VirtualSocketState st{};
    uint32_t dst_vip = 0;
    const UdpRoute route = resolveUdpRoute(s, dst_ip, dst_port, st, dst_vip);
    if (route == UdpRoute::Real) {
        return orig_WSASendTo(s, lpBuffers, dwBufferCount, lpNumberOfBytesSent, dwFlags,
                              lpTo, iTolen, lpOverlapped, lpCompletionRoutine);
    }
    if (route == UdpRoute::Failed) {
        return SOCKET_ERROR;
    }
    if ((dwBufferCount > 0 && !lpBuffers) || dwBufferCount > kMaxWsaBufs) {
        WSASetLastError(dwBufferCount > kMaxWsaBufs ? WSAENOBUFS : WSAEFAULT);
        return SOCKET_ERROR;
    }

    // The header goes out as WSABUF[0] in front of the app's buffers (gather send, no copy)
    const sockaddr_in gw_addr = udpGatewayAddr();
    const auto* gw = reinterpret_cast<const sockaddr*>(&gw_addr);
    const int gw_len = static_cast<int>(sizeof(gw_addr));

    if (!lpOverlapped) {
        LoopbackUdpHeader hdr = makeLoopbackHeader(st, dst_vip, dst_port);
        HeaderedWsaBufs bufs(&hdr, lpBuffers, dwBufferCount);
        DWORD raw_sent = 0;
        int rc = orig_WSASendTo(s, bufs.data(), bufs.count(), &raw_sent, dwFlags,
                                gw, gw_len, nullptr, nullptr);
        if (rc == 0 && lpNumberOfBytesSent) {
            *lpNumberOfBytesSent = (raw_sent >= kUdpHdrLen) ? raw_sent - kUdpHdrLen : 0;
        }
        return rc;
    }

    // Overlapped: the header must stay valid until the send completes, so it lives in the
    // per-operation record, which also corrects the byte count on every completion path.
    auto op = std::make_shared<OverlappedUdpOp>();
    op->s = s;
    op->is_send = true;
    op->app_routine = lpCompletionRoutine;
    op->hdr = makeLoopbackHeader(st, dst_vip, dst_port);
    HeaderedWsaBufs bufs(&op->hdr, lpBuffers, dwBufferCount);
    registerOverlappedOp(lpOverlapped, op);

    DWORD raw_sent = 0;
    int rc = orig_WSASendTo(s, bufs.data(), bufs.count(), &raw_sent, dwFlags, gw, gw_len,
                            lpOverlapped, completionThunkFor(lpCompletionRoutine));
    return finishOverlappedStart(lpOverlapped, op, rc, raw_sent, lpNumberOfBytesSent);
}

// Shared body for recvfrom()/recv() on a virtualized datagram socket. Mirrors Winsock: a datagram
// larger than the buffer fills it with the (header-stripped) start and fails with WSAEMSGSIZE;
// MSG_PEEK is passed through, so a peek never consumes but still strips the header.
int recvFromVirtual(SOCKET s, char* buf, int len, int flags, sockaddr* from, int* fromlen) {
    if (from && (!fromlen || *fromlen < static_cast<int>(sizeof(sockaddr_in)))) {
        WSASetLastError(WSAEFAULT);
        return SOCKET_ERROR;
    }

    ScopedHookBypass bypass;
    const size_t want = static_cast<size_t>(len) + kUdpHdrLen;
    const int scratch_len = static_cast<int>(std::min<size_t>(want, static_cast<size_t>(INT_MAX)));
    uint8_t* temp = recvScratch(static_cast<size_t>(scratch_len));
    sockaddr_storage raw_from{};
    int raw_from_len = sizeof(raw_from);

    int got = orig_recvfrom(s, reinterpret_cast<char*>(temp), scratch_len, flags,
                            reinterpret_cast<sockaddr*>(&raw_from), &raw_from_len);
    bool os_truncated = false;
    if (got == SOCKET_ERROR) {
        if (WSAGetLastError() != WSAEMSGSIZE) {
            return SOCKET_ERROR;
        }
        os_truncated = true;
        got = scratch_len; // a truncated datagram fills the whole buffer
    }

    const uint8_t* payload = temp;
    size_t payload_len = static_cast<size_t>(got);
    if (isGatewayDatagram(raw_from, raw_from_len, temp, payload_len)) {
        LoopbackUdpHeader hdr{};
        std::memcpy(&hdr, temp, sizeof(hdr));
        payload += sizeof(hdr);
        payload_len -= sizeof(hdr);
        writeVirtualSource(from, fromlen, hdr.src_vip, hdr.src_vport);
    } else {
        // Not virtual-LAN traffic (a real local sender): deliver unchanged with its real source
        copyRealSource(from, fromlen, raw_from, raw_from_len);
    }

    const size_t copy_len = std::min(payload_len, static_cast<size_t>(len));
    if (copy_len > 0 && buf) {
        std::memcpy(buf, payload, copy_len);
    }
    if (os_truncated || payload_len > copy_len) {
        WSASetLastError(WSAEMSGSIZE);
        return SOCKET_ERROR;
    }
    return static_cast<int>(copy_len);
}

int WSAAPI hooked_recvfrom(SOCKET s, char* buf, int len, int flags, sockaddr* from, int* fromlen) {
    if (g_bypass_hooks || !SocketShimCore::instance().isInitialized() || len < 0) {
        return orig_recvfrom(s, buf, len, flags, from, fromlen);
    }

    VirtualSocketState st{};
    if (!SocketShimCore::instance().getSocketState(static_cast<uint64_t>(s), st) || !isVirtualUdpSocket(st)) {
        return orig_recvfrom(s, buf, len, flags, from, fromlen);
    }

    return recvFromVirtual(s, buf, len, flags, from, fromlen);
}

// Shared body for synchronous WSARecvFrom()/WSARecv(); see recvFromVirtual() above. On truncation
// MSG_PARTIAL is reported in *lpFlags as well.
int wsaRecvFromVirtual(
    SOCKET s, LPWSABUF lpBuffers, DWORD dwBufferCount, LPDWORD lpNumberOfBytesRecvd,
    LPDWORD lpFlags, sockaddr* lpFrom, LPINT lpFromlen) {

    if (lpFrom && (!lpFromlen || *lpFromlen < static_cast<INT>(sizeof(sockaddr_in)))) {
        WSASetLastError(WSAEFAULT);
        return SOCKET_ERROR;
    }

    ScopedHookBypass bypass;
    size_t total_cap = 0;
    for (DWORD i = 0; i < dwBufferCount; ++i) {
        total_cap += lpBuffers[i].len;
    }

    const size_t scratch_size = total_cap + kUdpHdrLen;
    uint8_t* temp = recvScratch(scratch_size);
    WSABUF wbuf{};
    wbuf.buf = reinterpret_cast<char*>(temp);
    wbuf.len = static_cast<ULONG>(scratch_size);

    DWORD raw_recvd = 0;
    sockaddr_storage raw_from{};
    INT raw_from_len = sizeof(raw_from);

    int rc = orig_WSARecvFrom(s, &wbuf, 1, &raw_recvd, lpFlags,
                              reinterpret_cast<sockaddr*>(&raw_from), &raw_from_len,
                              nullptr, nullptr);
    bool os_truncated = false;
    if (rc != 0) {
        if (WSAGetLastError() != WSAEMSGSIZE) {
            return rc;
        }
        os_truncated = true;
        raw_recvd = wbuf.len; // a truncated datagram fills the whole buffer
    }

    const uint8_t* payload = temp;
    size_t payload_len = raw_recvd;
    if (isGatewayDatagram(raw_from, raw_from_len, temp, payload_len)) {
        LoopbackUdpHeader hdr{};
        std::memcpy(&hdr, temp, sizeof(hdr));
        payload += sizeof(hdr);
        payload_len -= sizeof(hdr);
        writeVirtualSource(lpFrom, lpFromlen, hdr.src_vip, hdr.src_vport);
    } else {
        copyRealSource(lpFrom, lpFromlen, raw_from, raw_from_len);
    }

    const size_t copied = scatterToWsaBufs(lpBuffers, dwBufferCount, payload, payload_len);
    if (lpNumberOfBytesRecvd) {
        *lpNumberOfBytesRecvd = static_cast<DWORD>(copied);
    }
    if (os_truncated || payload_len > copied) {
        if (lpFlags) {
            *lpFlags |= static_cast<DWORD>(MSG_PARTIAL);
        }
        WSASetLastError(WSAEMSGSIZE);
        return SOCKET_ERROR;
    }
    return 0;
}

int WSAAPI hooked_WSARecvFrom(
    SOCKET s, LPWSABUF lpBuffers, DWORD dwBufferCount, LPDWORD lpNumberOfBytesRecvd,
    LPDWORD lpFlags, sockaddr* lpFrom, LPINT lpFromlen,
    LPWSAOVERLAPPED lpOverlapped, LPWSAOVERLAPPED_COMPLETION_ROUTINE lpCompletionRoutine) {

    if (g_bypass_hooks || !SocketShimCore::instance().isInitialized() ||
        dwBufferCount == 0 || !lpBuffers || dwBufferCount > kMaxWsaBufs) {
        return orig_WSARecvFrom(s, lpBuffers, dwBufferCount, lpNumberOfBytesRecvd, lpFlags,
                                lpFrom, lpFromlen, lpOverlapped, lpCompletionRoutine);
    }

    VirtualSocketState st{};
    if (!SocketShimCore::instance().getSocketState(static_cast<uint64_t>(s), st) || !isVirtualUdpSocket(st)) {
        return orig_WSARecvFrom(s, lpBuffers, dwBufferCount, lpNumberOfBytesRecvd, lpFlags,
                                lpFrom, lpFromlen, lpOverlapped, lpCompletionRoutine);
    }

    if (lpOverlapped) {
        return startOverlappedUdpRecv(s, lpBuffers, dwBufferCount, lpNumberOfBytesRecvd, lpFlags,
                                      lpFrom, lpFromlen, lpOverlapped, lpCompletionRoutine);
    }
    return wsaRecvFromVirtual(s, lpBuffers, dwBufferCount, lpNumberOfBytesRecvd, lpFlags, lpFrom, lpFromlen);
}

int WSAAPI hooked_send(SOCKET s, const char* buf, int len, int flags) {
    if (!g_bypass_hooks && SocketShimCore::instance().isInitialized()) {
        VirtualSocketState st{};
        if (SocketShimCore::instance().getSocketState(static_cast<uint64_t>(s), st) &&
            st.type == SOCK_DGRAM && st.has_remote_peer) {
            sockaddr_in dst{};
            dst.sin_family = AF_INET;
            dst.sin_addr.s_addr = htonl(st.remote_vip);
            dst.sin_port = htons(st.remote_vport);
            return hooked_sendto(s, buf, len, flags, reinterpret_cast<const sockaddr*>(&dst), sizeof(dst));
        }
    }
    return orig_send(s, buf, len, flags);
}

int WSAAPI hooked_WSASend(
    SOCKET s, LPWSABUF lpBuffers, DWORD dwBufferCount, LPDWORD lpNumberOfBytesSent,
    DWORD dwFlags, LPWSAOVERLAPPED lpOverlapped, LPWSAOVERLAPPED_COMPLETION_ROUTINE lpCompletionRoutine) {
    if (!g_bypass_hooks && SocketShimCore::instance().isInitialized()) {
        VirtualSocketState st{};
        if (SocketShimCore::instance().getSocketState(static_cast<uint64_t>(s), st) &&
            st.type == SOCK_DGRAM && st.has_remote_peer) {
            sockaddr_in dst{};
            dst.sin_family = AF_INET;
            dst.sin_addr.s_addr = htonl(st.remote_vip);
            dst.sin_port = htons(st.remote_vport);
            return hooked_WSASendTo(s, lpBuffers, dwBufferCount, lpNumberOfBytesSent, dwFlags,
                                    reinterpret_cast<const sockaddr*>(&dst), sizeof(dst),
                                    lpOverlapped, lpCompletionRoutine);
        }
    }
    return orig_WSASend(s, lpBuffers, dwBufferCount, lpNumberOfBytesSent, dwFlags,
                        lpOverlapped, lpCompletionRoutine);
}

int WSAAPI hooked_recv(SOCKET s, char* buf, int len, int flags) {
    if (!g_bypass_hooks && SocketShimCore::instance().isInitialized() && len >= 0) {
        VirtualSocketState st{};
        if (SocketShimCore::instance().getSocketState(static_cast<uint64_t>(s), st) && isVirtualUdpSocket(st)) {
            return recvFromVirtual(s, buf, len, flags, nullptr, nullptr);
        }
    }
    return orig_recv(s, buf, len, flags);
}

int WSAAPI hooked_WSARecv(
    SOCKET s, LPWSABUF lpBuffers, DWORD dwBufferCount, LPDWORD lpNumberOfBytesRecvd,
    LPDWORD lpFlags, LPWSAOVERLAPPED lpOverlapped, LPWSAOVERLAPPED_COMPLETION_ROUTINE lpCompletionRoutine) {
    if (!g_bypass_hooks && SocketShimCore::instance().isInitialized() &&
        dwBufferCount > 0 && lpBuffers && dwBufferCount <= kMaxWsaBufs) {
        VirtualSocketState st{};
        if (SocketShimCore::instance().getSocketState(static_cast<uint64_t>(s), st) && isVirtualUdpSocket(st)) {
            if (lpOverlapped) {
                return startOverlappedUdpRecv(s, lpBuffers, dwBufferCount, lpNumberOfBytesRecvd, lpFlags,
                                              nullptr, nullptr, lpOverlapped, lpCompletionRoutine);
            }
            return wsaRecvFromVirtual(s, lpBuffers, dwBufferCount, lpNumberOfBytesRecvd, lpFlags,
                                      nullptr, nullptr);
        }
    }
    return orig_WSARecv(s, lpBuffers, dwBufferCount, lpNumberOfBytesRecvd, lpFlags,
                        lpOverlapped, lpCompletionRoutine);
}

int WSAAPI hooked_connect(SOCKET s, const sockaddr* name, int namelen) {
    if (g_bypass_hooks || !SocketShimCore::instance().isInitialized() ||
        !name || namelen < static_cast<int>(sizeof(sockaddr_in)) || name->sa_family != AF_INET) {
        return orig_connect(s, name, namelen);
    }

    const auto* sin = reinterpret_cast<const sockaddr_in*>(name);
    uint32_t dst_ip = ntohl(sin->sin_addr.s_addr);
    uint16_t dst_port = ntohs(sin->sin_port);
    const bool to_loopback = (dst_ip == kLoopbackIp);
    if (!isVirtualUnicast(dst_ip) && !to_loopback) {
        return orig_connect(s, name, namelen);
    }

    ScopedHookBypass bypass;
    auto& shim = SocketShimCore::instance();
    const uint64_t h = static_cast<uint64_t>(s);
    VirtualSocketState st = getOrDiscoverSocketState(s);
    if (st.af != AF_INET || st.passthrough) {
        return orig_connect(s, name, namelen);
    }
    const uint32_t target_vip = to_loopback ? shim.localVip() : dst_ip;

    if (st.type == SOCK_DGRAM) {
        // 127.0.0.1:X is only virtual if one of this client's hooked sockets holds UDP port X
        if (to_loopback && !shim.ipcQueryBind(IpcProto::Udp, dst_port)) {
            int rc = orig_connect(s, name, namelen);
            if (rc == 0) {
                shim.modifySocketState(h, [](VirtualSocketState& x) { x.has_remote_peer = false; });
            }
            return rc;
        }
        if (!ensureImplicitLoopbackBind(s, st)) {
            return SOCKET_ERROR;
        }
        shim.modifySocketState(h, [&](VirtualSocketState& x) {
            x.has_remote_peer = true;
            x.remote_vip = target_vip;
            x.remote_vport = dst_port;
        });
        return 0;
    }

    if (st.type != SOCK_STREAM) {
        return orig_connect(s, name, namelen);
    }

    // A loopback destination with no virtual listener behind it is a real local service
    if (to_loopback && !shim.ipcQueryBind(IpcProto::Tcp, dst_port)) {
        return orig_connect(s, name, namelen);
    }

    uint16_t local_port = st.actual_loopback_port;
    if (!st.is_bound || local_port == 0) {
        if (!bindLoopbackOrAdopt(s, local_port)) {
            return SOCKET_ERROR;
        }
        shim.modifySocketState(h, [&](VirtualSocketState& x) {
            x.is_bound = true;
            x.actual_loopback_port = local_port;
            if (x.virtual_port == 0) {
                x.virtual_port = local_port;
            }
        });
        shim.getSocketState(h, st);
    }
    const uint16_t local_vport = (st.virtual_port != 0) ? st.virtual_port : local_port;

    uint16_t connect_loopback_port = shim.tcpGatewayPort();
    if (!shim.ipcPrepareTcpConnect(local_port, target_vip, dst_port, local_vport, &connect_loopback_port)) {
        if (to_loopback) {
            // The virtual listener went away meanwhile: let a real local service answer instead.
            // The socket is bound to 127.0.0.1, which reaches any loopback listener.
            return orig_connect(s, name, namelen);
        }
        WSASetLastError(WSAECONNREFUSED);
        return SOCKET_ERROR;
    }

    const uint32_t local_vip = shim.localVip();
    shim.modifySocketState(h, [&](VirtualSocketState& x) {
        x.is_virtualized = true;
        x.bound_vip = local_vip;
        x.has_remote_peer = true;
        x.remote_vip = target_vip;
        x.remote_vport = dst_port;
    });

    sockaddr_in gw_addr{};
    gw_addr.sin_family = AF_INET;
    gw_addr.sin_addr.s_addr = htonl(kLoopbackIp);
    gw_addr.sin_port = htons(connect_loopback_port);

    int rc = orig_connect(s, reinterpret_cast<const sockaddr*>(&gw_addr), sizeof(gw_addr));
    if (rc == SOCKET_ERROR && WSAGetLastError() == WSAEWOULDBLOCK) {
        // Since ipcPrepareTcpConnect already completed the remote VPN TCP handshake,
        // let the local 127.0.0.1 loopback TCP handshake finish (<0.1ms) and return 0
        // so non-blocking callers like DirectPlay (dpwsockx.dll) mark the socket connected immediately.
        fd_set wfds;
        FD_ZERO(&wfds);
        FD_SET(s, &wfds);
        timeval tv{};
        tv.tv_sec = 0;
        tv.tv_usec = 50000; // up to 50ms
        if (::select(0, nullptr, &wfds, nullptr, &tv) > 0) {
            WSASetLastError(0);
            return 0;
        }
        WSASetLastError(WSAEWOULDBLOCK);
    }
    return rc;
}

int WSAAPI hooked_WSAConnect(
    SOCKET s, const sockaddr* name, int namelen,
    LPWSABUF lpCallerData, LPWSABUF lpCalleeData, LPQOS lpSQOS, LPQOS lpGQOS) {
    if (!g_bypass_hooks && SocketShimCore::instance().isInitialized() &&
        name && namelen >= static_cast<int>(sizeof(sockaddr_in)) && name->sa_family == AF_INET) {
        const auto* sin = reinterpret_cast<const sockaddr_in*>(name);
        uint32_t dst_ip = ntohl(sin->sin_addr.s_addr);
        if (isVirtualUnicast(dst_ip) || dst_ip == kLoopbackIp) {
            return hooked_connect(s, name, namelen);
        }
    }
    return orig_WSAConnect(s, name, namelen, lpCallerData, lpCalleeData, lpSQOS, lpGQOS);
}

// Post-process a connection accepted (peer address in `peer`) on a virtualized IPv4 listener:
// connections bridged by VpnClient get their virtual peer address, anything else (a real local
// client) is reported exactly like accept() would, honouring the caller's addrlen.
SOCKET finishVirtualAccept(SOCKET accepted, const VirtualSocketState& listener_st,
                           const sockaddr_storage& peer, int peer_len, sockaddr* addr, int* addrlen) {
    auto& shim = SocketShimCore::instance();
    const uint64_t h = static_cast<uint64_t>(accepted);
    // Fresh state also clears anything left over from an earlier socket with the same handle value
    shim.registerSocket(h, AF_INET, SOCK_STREAM);

    if (peer.ss_family == AF_INET) {
        const auto* peer_in = reinterpret_cast<const sockaddr_in*>(&peer);
        uint32_t peer_vip = 0;
        uint16_t peer_vport = 0;
        if (shim.ipcQueryTcpPeer(ntohs(peer_in->sin_port), peer_vip, peer_vport)) {
            const uint32_t local_vip = shim.localVip();
            shim.modifySocketState(h, [&](VirtualSocketState& x) {
                x.is_bound = true;
                x.is_virtualized = true;
                x.owns_binding = false; // the binding belongs to the listener
                x.bound_vip = local_vip;
                x.virtual_port = listener_st.virtual_port;
                x.actual_loopback_port = listener_st.actual_loopback_port;
                x.has_remote_peer = true;
                x.remote_vip = peer_vip;
                x.remote_vport = peer_vport;
            });
            writeVirtualSource(addr, addrlen, peer_vip, peer_vport);
            return accepted;
        }
    }

    if (addr && addrlen) {
        if (*addrlen < peer_len) {
            shim.unregisterSocket(h);
            orig_closesocket(accepted);
            WSASetLastError(WSAEFAULT);
            return INVALID_SOCKET;
        }
        std::memcpy(addr, &peer, static_cast<size_t>(peer_len));
        *addrlen = peer_len;
    }
    return accepted;
}

SOCKET WSAAPI hooked_accept(SOCKET s, sockaddr* addr, int* addrlen) {
    if (g_bypass_hooks || !SocketShimCore::instance().isInitialized()) {
        return orig_accept(s, addr, addrlen);
    }

    // Bypass from here on: hooked_WSAAccept forwards to this function, so an accept() that is
    // implemented on top of WSAAccept must not re-enter the detours.
    ScopedHookBypass bypass;
    VirtualSocketState listener_st{};
    if (!SocketShimCore::instance().getSocketState(static_cast<uint64_t>(s), listener_st) ||
        !isVirtualIpv4Socket(listener_st)) {
        // IPv6 and plain listeners: untouched, including the caller's addr/addrlen handling
        return orig_accept(s, addr, addrlen);
    }
    if (addr && addrlen && *addrlen < static_cast<int>(sizeof(sockaddr_in))) {
        WSASetLastError(WSAEFAULT);
        return INVALID_SOCKET;
    }

    sockaddr_storage peer{};
    int peer_len = sizeof(peer);
    SOCKET accepted = orig_accept(s, reinterpret_cast<sockaddr*>(&peer), &peer_len);
    if (accepted == INVALID_SOCKET) {
        return INVALID_SOCKET;
    }
    return finishVirtualAccept(accepted, listener_st, peer, peer_len, addr, addrlen);
}

SOCKET WSAAPI hooked_WSAAccept(
    SOCKET s, sockaddr* addr, LPINT addrlen, LPCONDITIONPROC lpfnCondition, DWORD_PTR dwCallbackData) {
    if (g_bypass_hooks || !SocketShimCore::instance().isInitialized()) {
        return orig_WSAAccept(s, addr, addrlen, lpfnCondition, dwCallbackData);
    }
    if (!lpfnCondition) {
        return hooked_accept(s, addr, addrlen);
    }

    VirtualSocketState listener_st{};
    if (!SocketShimCore::instance().getSocketState(static_cast<uint64_t>(s), listener_st) ||
        !isVirtualIpv4Socket(listener_st)) {
        return orig_WSAAccept(s, addr, addrlen, lpfnCondition, dwCallbackData);
    }
    if (addr && addrlen && *addrlen < static_cast<int>(sizeof(sockaddr_in))) {
        WSASetLastError(WSAEFAULT);
        return INVALID_SOCKET;
    }

    // No bypass around the call: the condition function is app code (it sees the loopback caller
    // address; only the address returned to the caller is virtualized).
    sockaddr_storage peer{};
    int peer_len = sizeof(peer);
    SOCKET accepted = orig_WSAAccept(s, reinterpret_cast<sockaddr*>(&peer), &peer_len,
                                     lpfnCondition, dwCallbackData);
    if (accepted == INVALID_SOCKET) {
        return INVALID_SOCKET;
    }
    ScopedHookBypass bypass;
    return finishVirtualAccept(accepted, listener_st, peer, peer_len, addr, addrlen);
}

// ============================================================================
// Completion APIs: apply the deferred overlapped UDP fix-up (see above)
// ============================================================================

BOOL WSAAPI hooked_WSAGetOverlappedResult(
    SOCKET s, LPWSAOVERLAPPED lpOverlapped, LPDWORD lpcbTransfer, BOOL fWait, LPDWORD lpdwFlags) {
    BOOL ok = orig_WSAGetOverlappedResult(s, lpOverlapped, lpcbTransfer, fWait, lpdwFlags);
    if (g_ovl_op_count.load(std::memory_order_acquire) == 0 || !lpOverlapped || !lpcbTransfer) {
        return ok;
    }
    const int saved_error = WSAGetLastError();
    if (!ok && saved_error != WSAEMSGSIZE) {
        return ok; // still pending, or failed without data: nothing to fix up
    }
    DWORD bytes = *lpcbTransfer;
    bool truncated = false;
    const bool truncated_hint = !ok && saved_error == WSAEMSGSIZE;
    if (observeOverlappedCompletion(lpOverlapped, s, bytes, true, false, truncated_hint, truncated)) {
        *lpcbTransfer = bytes;
        if (truncated) {
            if (lpdwFlags) {
                *lpdwFlags |= static_cast<DWORD>(MSG_PARTIAL);
            }
            WSASetLastError(WSAEMSGSIZE);
            return FALSE;
        }
    }
    WSASetLastError(saved_error);
    return ok;
}

// Shared tail of GetOverlappedResult()/GetOverlappedResultEx()
BOOL finishOverlappedResultQuery(HANDLE hFile, LPOVERLAPPED lpOverlapped, LPDWORD lpBytes, BOOL ok) {
    if (g_ovl_op_count.load(std::memory_order_acquire) == 0 || !lpOverlapped || !lpBytes) {
        return ok;
    }
    const DWORD saved_error = GetLastError();
    if (!ok && saved_error != ERROR_MORE_DATA) {
        return ok; // still pending / timed out / alerted, or failed without data
    }
    DWORD bytes = *lpBytes;
    bool truncated = false;
    const bool truncated_hint = !ok && saved_error == ERROR_MORE_DATA;
    if (observeOverlappedCompletion(lpOverlapped, reinterpret_cast<SOCKET>(hFile), bytes,
                                    true, false, truncated_hint, truncated)) {
        *lpBytes = bytes;
        if (ok && truncated) {
            SetLastError(ERROR_MORE_DATA);
            return FALSE;
        }
    }
    SetLastError(saved_error);
    return ok;
}

BOOL WINAPI hooked_GetOverlappedResult(
    HANDLE hFile, LPOVERLAPPED lpOverlapped, LPDWORD lpNumberOfBytesTransferred, BOOL bWait) {
    BOOL ok = orig_GetOverlappedResult(hFile, lpOverlapped, lpNumberOfBytesTransferred, bWait);
    return finishOverlappedResultQuery(hFile, lpOverlapped, lpNumberOfBytesTransferred, ok);
}

BOOL WINAPI hooked_GetOverlappedResultEx(
    HANDLE hFile, LPOVERLAPPED lpOverlapped, LPDWORD lpNumberOfBytesTransferred,
    DWORD dwMilliseconds, BOOL bAlertable) {
    BOOL ok = orig_GetOverlappedResultEx(hFile, lpOverlapped, lpNumberOfBytesTransferred,
                                         dwMilliseconds, bAlertable);
    return finishOverlappedResultQuery(hFile, lpOverlapped, lpNumberOfBytesTransferred, ok);
}

BOOL WINAPI hooked_GetQueuedCompletionStatus(
    HANDLE CompletionPort, LPDWORD lpNumberOfBytesTransferred, PULONG_PTR lpCompletionKey,
    LPOVERLAPPED* lpOverlapped, DWORD dwMilliseconds) {
    BOOL ok = orig_GetQueuedCompletionStatus(CompletionPort, lpNumberOfBytesTransferred,
                                             lpCompletionKey, lpOverlapped, dwMilliseconds);
    if (g_ovl_op_count.load(std::memory_order_acquire) == 0 || !lpOverlapped || !*lpOverlapped ||
        !lpNumberOfBytesTransferred) {
        return ok;
    }
    const DWORD saved_error = GetLastError();
    const bool data_valid = ok || saved_error == ERROR_MORE_DATA;
    const bool truncated_hint = !ok && saved_error == ERROR_MORE_DATA;
    DWORD bytes = *lpNumberOfBytesTransferred;
    bool truncated = false;
    if (observeOverlappedCompletion(*lpOverlapped, INVALID_SOCKET, bytes, data_valid, true,
                                    truncated_hint, truncated)) {
        *lpNumberOfBytesTransferred = bytes;
        if (ok && truncated) {
            SetLastError(ERROR_MORE_DATA);
            return FALSE;
        }
    }
    SetLastError(saved_error);
    return ok;
}

BOOL WINAPI hooked_GetQueuedCompletionStatusEx(
    HANDLE CompletionPort, CompletionEntry* lpCompletionPortEntries, ULONG ulCount,
    PULONG ulNumEntriesRemoved, DWORD dwMilliseconds, BOOL fAlertable) {
    BOOL ok = orig_GetQueuedCompletionStatusEx(CompletionPort, lpCompletionPortEntries, ulCount,
                                               ulNumEntriesRemoved, dwMilliseconds, fAlertable);
    if (!ok || g_ovl_op_count.load(std::memory_order_acquire) == 0 ||
        !lpCompletionPortEntries || !ulNumEntriesRemoved) {
        return ok;
    }
    const DWORD saved_error = GetLastError();
    const ULONG removed = std::min(*ulNumEntriesRemoved, ulCount);
    for (ULONG i = 0; i < removed; ++i) {
        CompletionEntry& entry = lpCompletionPortEntries[i];
        if (!entry.lpOverlapped) continue;
        const DWORD status = static_cast<DWORD>(entry.Internal);
        DWORD bytes = entry.dwNumberOfBytesTransferred;
        bool truncated = false;
        if (observeOverlappedCompletion(entry.lpOverlapped, INVALID_SOCKET, bytes,
                                        status == 0 || status == kStatusBufferOverflow, true,
                                        status == kStatusBufferOverflow, truncated)) {
            entry.dwNumberOfBytesTransferred = bytes;
            if (truncated) {
                entry.Internal = kStatusBufferOverflow;
            }
        }
    }
    SetLastError(saved_error);
    return ok;
}

// ============================================================================
// Network Interface & Host IP Virtualization
// ============================================================================

thread_local hostent g_tls_hostent{};
thread_local char    g_tls_hostname[256] = {};
thread_local in_addr g_tls_addr{};
thread_local char*   g_tls_addr_list[2]  = {};
thread_local char*   g_tls_aliases[1]    = { nullptr };

hostent* WSAAPI hooked_gethostbyname(const char* name) {
    if (!g_bypass_hooks && SocketShimCore::instance().isInitialized()) {
        char local_name[256] = {};
        ::gethostname(local_name, sizeof(local_name) - 1);
        if (!name || name[0] == '\0' || _stricmp(name, local_name) == 0 || _stricmp(name, "localhost") != 0) {
            if (!name || name[0] == '\0' || _stricmp(name, local_name) == 0) {
                strncpy_s(g_tls_hostname, local_name, _TRUNCATE);
                g_tls_addr.s_addr = htonl(SocketShimCore::instance().localVip());
                g_tls_addr_list[0] = reinterpret_cast<char*>(&g_tls_addr);
                g_tls_addr_list[1] = nullptr;
                g_tls_hostent.h_name = g_tls_hostname;
                g_tls_hostent.h_aliases = g_tls_aliases;
                g_tls_hostent.h_addrtype = AF_INET;
                g_tls_hostent.h_length = sizeof(in_addr);
                g_tls_hostent.h_addr_list = g_tls_addr_list;
                return &g_tls_hostent;
            }
        }
    }
    return orig_gethostbyname(name);
}

INT WSAAPI hooked_getaddrinfo(PCSTR pNodeName, PCSTR pServiceName, const ADDRINFOA* pHints, PADDRINFOA* ppResult) {
    INT rc = orig_getaddrinfo(pNodeName, pServiceName, pHints, ppResult);
    if (rc == 0 && !g_bypass_hooks && SocketShimCore::instance().isInitialized() && ppResult && *ppResult) {
        char local_name[256] = {};
        ::gethostname(local_name, sizeof(local_name) - 1);
        if (!pNodeName || pNodeName[0] == '\0' || _stricmp(pNodeName, local_name) == 0) {
            for (ADDRINFOA* p = *ppResult; p != nullptr; p = p->ai_next) {
                if (p->ai_family == AF_INET && p->ai_addr && p->ai_addrlen >= sizeof(sockaddr_in)) {
                    auto* sin = reinterpret_cast<sockaddr_in*>(p->ai_addr);
                    sin->sin_addr.s_addr = htonl(SocketShimCore::instance().localVip());
                }
            }
        }
    }
    return rc;
}

ULONG WINAPI hooked_GetAdaptersInfo(PIP_ADAPTER_INFO pAdapterInfo, PULONG pOutBufLen) {
    ULONG rc = orig_GetAdaptersInfo(pAdapterInfo, pOutBufLen);
    if (rc == NO_ERROR && !g_bypass_hooks && SocketShimCore::instance().isInitialized() && pAdapterInfo) {
        std::string vip_str = ipv4ToString(SocketShimCore::instance().localVip());
        strncpy_s(pAdapterInfo->IpAddressList.IpAddress.String, vip_str.c_str(), _TRUNCATE);
        strncpy_s(pAdapterInfo->IpAddressList.IpMask.String, "255.255.255.0", _TRUNCATE);
        strncpy_s(pAdapterInfo->GatewayList.IpAddress.String, "10.42.0.1", _TRUNCATE);
    }
    return rc;
}

ULONG WINAPI hooked_GetAdaptersAddresses(
    ULONG Family, ULONG Flags, PVOID Reserved, PIP_ADAPTER_ADDRESSES AdapterAddresses, PULONG SizePointer) {
    ULONG rc = orig_GetAdaptersAddresses(Family, Flags, Reserved, AdapterAddresses, SizePointer);
    if (rc == NO_ERROR && !g_bypass_hooks && SocketShimCore::instance().isInitialized() && AdapterAddresses) {
        for (auto* curr = AdapterAddresses; curr != nullptr; curr = curr->Next) {
            if (curr->OperStatus != IfOperStatusUp || curr->IfType == IF_TYPE_SOFTWARE_LOOPBACK) continue;
            for (auto* ua = curr->FirstUnicastAddress; ua != nullptr; ua = ua->Next) {
                if (ua->Address.lpSockaddr && ua->Address.lpSockaddr->sa_family == AF_INET) {
                    auto* sin = reinterpret_cast<sockaddr_in*>(ua->Address.lpSockaddr);
                    sin->sin_addr.s_addr = htonl(SocketShimCore::instance().localVip());
                    ua->OnLinkPrefixLength = 24;
                    return rc;
                }
            }
        }
    }
    return rc;
}

int WSAAPI hooked_WSAIoctl(
    SOCKET s, DWORD dwIoControlCode, LPVOID lpvInBuffer, DWORD cbInBuffer,
    LPVOID lpvOutBuffer, DWORD cbOutBuffer, LPDWORD lpcbBytesReturned,
    LPWSAOVERLAPPED lpOverlapped, LPWSAOVERLAPPED_COMPLETION_ROUTINE lpCompletionRoutine) {

    int rc = orig_WSAIoctl(s, dwIoControlCode, lpvInBuffer, cbInBuffer,
                           lpvOutBuffer, cbOutBuffer, lpcbBytesReturned,
                           lpOverlapped, lpCompletionRoutine);
    if (rc == 0 && !g_bypass_hooks && SocketShimCore::instance().isInitialized() &&
        dwIoControlCode == SIO_GET_INTERFACE_LIST && lpvOutBuffer && lpcbBytesReturned) {
        size_t count = *lpcbBytesReturned / sizeof(INTERFACE_INFO);
        auto* list = static_cast<INTERFACE_INFO*>(lpvOutBuffer);
        for (size_t i = 0; i < count; ++i) {
            if (!(list[i].iiFlags & IFF_LOOPBACK) && (list[i].iiFlags & IFF_UP)) {
                list[i].iiAddress.AddressIn.sin_family = AF_INET;
                list[i].iiAddress.AddressIn.sin_addr.s_addr = htonl(SocketShimCore::instance().localVip());
                list[i].iiBroadcastAddress.AddressIn.sin_family = AF_INET;
                list[i].iiBroadcastAddress.AddressIn.sin_addr.s_addr = htonl(kVirtualBroadcastIp);
                list[i].iiNetmask.AddressIn.sin_family = AF_INET;
                list[i].iiNetmask.AddressIn.sin_addr.s_addr = htonl(kVirtualSubnetMask);
                break;
            }
        }
    }
    return rc;
}

// ============================================================================
// DirectPlay User-Mode Compatibility:
// 1. Block dpnathlp.dll (DirectPlay UPnP/NAT Helper) so dpwsockx.dll and
//    dplaysvr.exe never overwrite virtual 0.0.0.0 headers with physical/WAN IPs.
// 2. Namespace Local\__DPHelp* shared memory/mutex/events per VPN client
//    (by IPC port) so dpwsockx.dll always spawns a fresh hooked dplaysvr.exe
//    inside the VPN rather than attaching to an unhooked pre-existing instance,
//    and multiple VPN clients on one PC get isolated dplaysvr.exe instances.
// ============================================================================

bool isDpNatHlpLibrary(LPCWSTR name) {
    if (!name) return false;
    const wchar_t* base = name;
    for (const wchar_t* p = name; *p != L'\0'; ++p) {
        if (*p == L'\\' || *p == L'/') {
            base = p + 1;
        }
    }
    return _wcsicmp(base, L"dpnathlp.dll") == 0 || _wcsicmp(base, L"dpnathlp") == 0;
}

std::wstring namespaceDpHelpNameW(LPCWSTR lpName, bool& out_modified) {
    out_modified = false;
    if (!lpName || !SocketShimCore::instance().isInitialized()) {
        return {};
    }
    if (std::wcsstr(lpName, L"__DPHelp") != nullptr) {
        out_modified = true;
        return std::wstring(lpName) + L"_" + std::to_wstring(SocketShimCore::instance().ipcPort());
    }
    return {};
}

HMODULE WINAPI hooked_LoadLibraryExW(LPCWSTR lpLibFileName, HANDLE hFile, DWORD dwFlags) {
    if (!g_bypass_hooks && SocketShimCore::instance().isInitialized() && isDpNatHlpLibrary(lpLibFileName)) {
        SetLastError(ERROR_MOD_NOT_FOUND);
        return nullptr;
    }
    return orig_LoadLibraryExW(lpLibFileName, hFile, dwFlags);
}

HANDLE WINAPI hooked_CreateEventExW(
    LPSECURITY_ATTRIBUTES lpEventAttributes, LPCWSTR lpName, DWORD dwFlags, DWORD dwDesiredAccess) {
    bool mod = false;
    std::wstring custom = namespaceDpHelpNameW(lpName, mod);
    return orig_CreateEventExW(lpEventAttributes, mod ? custom.c_str() : lpName, dwFlags, dwDesiredAccess);
}

HANDLE WINAPI hooked_OpenEventW(DWORD dwDesiredAccess, BOOL bInheritHandle, LPCWSTR lpName) {
    bool mod = false;
    std::wstring custom = namespaceDpHelpNameW(lpName, mod);
    return orig_OpenEventW(dwDesiredAccess, bInheritHandle, mod ? custom.c_str() : lpName);
}

HANDLE WINAPI hooked_CreateMutexExW(
    LPSECURITY_ATTRIBUTES lpMutexAttributes, LPCWSTR lpName, DWORD dwFlags, DWORD dwDesiredAccess) {
    bool mod = false;
    std::wstring custom = namespaceDpHelpNameW(lpName, mod);
    return orig_CreateMutexExW(lpMutexAttributes, mod ? custom.c_str() : lpName, dwFlags, dwDesiredAccess);
}

HANDLE WINAPI hooked_OpenMutexW(DWORD dwDesiredAccess, BOOL bInheritHandle, LPCWSTR lpName) {
    bool mod = false;
    std::wstring custom = namespaceDpHelpNameW(lpName, mod);
    return orig_OpenMutexW(dwDesiredAccess, bInheritHandle, mod ? custom.c_str() : lpName);
}

HANDLE WINAPI hooked_CreateFileMappingNumaW(
    HANDLE hFile, LPSECURITY_ATTRIBUTES lpFileMappingAttributes, DWORD flProtect,
    DWORD dwMaximumSizeHigh, DWORD dwMaximumSizeLow, LPCWSTR lpName, DWORD nndPreferred) {
    bool mod = false;
    std::wstring custom = namespaceDpHelpNameW(lpName, mod);
    return orig_CreateFileMappingNumaW(hFile, lpFileMappingAttributes, flProtect,
                                       dwMaximumSizeHigh, dwMaximumSizeLow,
                                       mod ? custom.c_str() : lpName, nndPreferred);
}

HANDLE WINAPI hooked_OpenFileMappingW(DWORD dwDesiredAccess, BOOL bInheritHandle, LPCWSTR lpName) {
    bool mod = false;
    std::wstring custom = namespaceDpHelpNameW(lpName, mod);
    return orig_OpenFileMappingW(dwDesiredAccess, bInheritHandle, mod ? custom.c_str() : lpName);
}

// ============================================================================
// Cross-Bitness Child Process Propagation (32-bit <-> 64-bit)
// ============================================================================

// Variables a hooked child needs to reach the same VpnClient (ARCHITECTURE.md section 3)
constexpr const char* kPropagatedEnvVars[] = {
    kEnvVirtualIp, kEnvIpcPort, kEnvUdpGwPort, kEnvTcpGwPort, kEnvSessionToken, kEnvBinDir,
};

// Length of the variable name in "NAME=value". A leading '=' belongs to the name, so per-drive
// entries such as "=C:=C:\dir" keep their full name.
size_t envNameLength(const std::wstring& entry) {
    const size_t eq = entry.find(L'=', 1);
    return (eq == std::wstring::npos) ? entry.size() : eq;
}

int compareEnvNames(const std::wstring& a, size_t a_len, const std::wstring& b, size_t b_len) {
    return CompareStringOrdinal(a.c_str(), static_cast<int>(a_len), b.c_str(), static_cast<int>(b_len), TRUE);
}

// A caller-supplied environment block replaces ours entirely, so the APIVPN_* variables would
// not reach the child. Rebuild it as a Unicode block (the caller's may be ANSI or Unicode) with
// our values merged in, kept sorted by name as Windows expects.
std::vector<wchar_t> buildChildEnvironment(const void* env, bool is_unicode) {
    std::vector<std::wstring> entries;
    if (is_unicode) {
        for (const wchar_t* p = static_cast<const wchar_t*>(env); *p != L'\0'; p += std::wcslen(p) + 1) {
            entries.emplace_back(p);
        }
    } else {
        // ANSI blocks are in the active code page
        for (const char* p = static_cast<const char*>(env); *p != '\0'; p += std::strlen(p) + 1) {
            const int n = static_cast<int>(std::strlen(p));
            const int wn = MultiByteToWideChar(CP_ACP, 0, p, n, nullptr, 0);
            std::wstring w(static_cast<size_t>(wn > 0 ? wn : 0), L'\0');
            if (wn > 0) {
                MultiByteToWideChar(CP_ACP, 0, p, n, w.data(), wn);
            }
            entries.push_back(std::move(w));
        }
    }

    for (const char* name_ascii : kPropagatedEnvVars) {
        const std::wstring name = utf8ToWide(name_ascii);
        // Drop the caller's copy (if any): the child must talk to the same VpnClient as we do
        entries.erase(std::remove_if(entries.begin(), entries.end(), [&](const std::wstring& e) {
            const size_t e_len = envNameLength(e);
            return e_len == name.size() && compareEnvNames(e, e_len, name, name.size()) == CSTR_EQUAL;
        }), entries.end());

        std::string value;
        if (!getEnvUtf8(name_ascii, value)) {
            continue;
        }
        auto pos = std::find_if(entries.begin(), entries.end(), [&](const std::wstring& e) {
            return compareEnvNames(e, envNameLength(e), name, name.size()) == CSTR_GREATER_THAN;
        });
        entries.insert(pos, name + L"=" + utf8ToWide(value));
    }

    std::vector<wchar_t> block;
    for (const std::wstring& e : entries) {
        block.insert(block.end(), e.begin(), e.end());
        block.push_back(L'\0');
    }
    if (entries.empty()) {
        block.push_back(L'\0');
    }
    block.push_back(L'\0');
    return block;
}

// Creation flags/environment for a child that we inject before it runs.
DWORD prepareChildCreation(DWORD flags, LPVOID& inout_env, std::vector<wchar_t>& env_storage) {
    // Always create suspended so the hook DLL is loaded before the child's first instruction.
    // CREATE_NO_WINDOW is deliberately not added: console children keep normal Windows console
    // semantics. Taking over VpnClient's console is prevented by hooked_AttachConsole instead.
    DWORD effective = flags | CREATE_SUSPENDED;
    if (inout_env) {
        env_storage = buildChildEnvironment(inout_env, (flags & CREATE_UNICODE_ENVIRONMENT) != 0);
        inout_env = env_storage.data();
        effective |= CREATE_UNICODE_ENVIRONMENT;
    }
    return effective;
}

// Inject the hook into a child created suspended, then resume it unless the caller asked for a
// suspended child. Fails closed: a child we could not inject is terminated and the create call
// fails, so nothing ever runs unhooked on the real network.
BOOL finishChildLaunch(LPPROCESS_INFORMATION pi, bool caller_owns_pi, bool caller_wanted_suspended) {
    const uint32_t pid = static_cast<uint32_t>(pi->dwProcessId);
    if (!ProcessLauncher::injectIntoProcess(pid)) {
        hookLog("hook injection into child process failed; child terminated");
        TerminateProcess(pi->hProcess, ERROR_ACCESS_DENIED);
        CloseHandle(pi->hThread);
        CloseHandle(pi->hProcess);
        *pi = PROCESS_INFORMATION{};
        SetLastError(ERROR_ACCESS_DENIED);
        return FALSE;
    }
    if (!SocketShimCore::instance().ipcRegisterProcess(pid)) {
        hookLog("RegisterProcess for child process failed");
    }
    if (!caller_wanted_suspended) {
        ResumeThread(pi->hThread);
    }
    if (!caller_owns_pi) {
        CloseHandle(pi->hThread);
        CloseHandle(pi->hProcess);
    }
    return TRUE;
}

BOOL WINAPI hooked_CreateProcessInternalW(
    HANDLE hUserToken,
    LPCWSTR lpApplicationName,
    LPWSTR lpCommandLine,
    LPSECURITY_ATTRIBUTES lpProcessAttributes,
    LPSECURITY_ATTRIBUTES lpThreadAttributes,
    BOOL bInheritHandles,
    DWORD dwCreationFlags,
    LPVOID lpEnvironment,
    LPCWSTR lpCurrentDirectory,
    LPSTARTUPINFOW lpStartupInfo,
    LPPROCESS_INFORMATION lpProcessInformation,
    PHANDLE hNewToken) {

    if (g_bypass_hooks || !SocketShimCore::instance().isInitialized()) {
        return orig_CreateProcessInternalW(
            hUserToken, lpApplicationName, lpCommandLine, lpProcessAttributes,
            lpThreadAttributes, bInheritHandles, dwCreationFlags, lpEnvironment,
            lpCurrentDirectory, lpStartupInfo, lpProcessInformation, hNewToken);
    }

    if ((lpCommandLine && std::wcsstr(lpCommandLine, L"apivpn_injector") != nullptr) ||
        (lpApplicationName && std::wcsstr(lpApplicationName, L"apivpn_injector") != nullptr)) {
        return orig_CreateProcessInternalW(
            hUserToken, lpApplicationName, lpCommandLine, lpProcessAttributes,
            lpThreadAttributes, bInheritHandles, dwCreationFlags, lpEnvironment,
            lpCurrentDirectory, lpStartupInfo, lpProcessInformation, hNewToken);
    }

    ScopedHookBypass bypass;
    const bool caller_wanted_suspended = (dwCreationFlags & CREATE_SUSPENDED) != 0;
    std::vector<wchar_t> env_storage;
    LPVOID env = lpEnvironment;
    const DWORD effective_flags = prepareChildCreation(dwCreationFlags, env, env_storage);
    PROCESS_INFORMATION local_pi{};
    LPPROCESS_INFORMATION pi_ptr = lpProcessInformation ? lpProcessInformation : &local_pi;

    BOOL ok = orig_CreateProcessInternalW(
        hUserToken, lpApplicationName, lpCommandLine, lpProcessAttributes,
        lpThreadAttributes, bInheritHandles, effective_flags, env,
        lpCurrentDirectory, lpStartupInfo, pi_ptr, hNewToken);
    if (!ok) {
        return FALSE;
    }
    return finishChildLaunch(pi_ptr, lpProcessInformation != nullptr, caller_wanted_suspended);
}

BOOL WINAPI hooked_CreateProcessA(
    LPCSTR lpApplicationName, LPSTR lpCommandLine,
    LPSECURITY_ATTRIBUTES lpProcessAttributes, LPSECURITY_ATTRIBUTES lpThreadAttributes,
    BOOL bInheritHandles, DWORD dwCreationFlags, LPVOID lpEnvironment,
    LPCSTR lpCurrentDirectory, LPSTARTUPINFOA lpStartupInfo,
    LPPROCESS_INFORMATION lpProcessInformation) {

    if (g_bypass_hooks || !SocketShimCore::instance().isInitialized()) {
        return orig_CreateProcessA(lpApplicationName, lpCommandLine, lpProcessAttributes,
                                   lpThreadAttributes, bInheritHandles, dwCreationFlags,
                                   lpEnvironment, lpCurrentDirectory, lpStartupInfo, lpProcessInformation);
    }

    // Avoid hooking our own injector helper process
    if ((lpCommandLine && std::strstr(lpCommandLine, "apivpn_injector") != nullptr) ||
        (lpApplicationName && std::strstr(lpApplicationName, "apivpn_injector") != nullptr)) {
        return orig_CreateProcessA(lpApplicationName, lpCommandLine, lpProcessAttributes,
                                   lpThreadAttributes, bInheritHandles, dwCreationFlags,
                                   lpEnvironment, lpCurrentDirectory, lpStartupInfo, lpProcessInformation);
    }

    ScopedHookBypass bypass;
    const bool caller_wanted_suspended = (dwCreationFlags & CREATE_SUSPENDED) != 0;
    std::vector<wchar_t> env_storage;
    LPVOID env = lpEnvironment;
    const DWORD effective_flags = prepareChildCreation(dwCreationFlags, env, env_storage);
    PROCESS_INFORMATION local_pi{};
    LPPROCESS_INFORMATION pi_ptr = lpProcessInformation ? lpProcessInformation : &local_pi;

    // CreateProcessA accepts a Unicode environment block when CREATE_UNICODE_ENVIRONMENT is set
    BOOL ok = orig_CreateProcessA(
        lpApplicationName, lpCommandLine, lpProcessAttributes, lpThreadAttributes,
        bInheritHandles, effective_flags, env,
        lpCurrentDirectory, lpStartupInfo, pi_ptr);
    if (!ok) {
        return FALSE;
    }
    return finishChildLaunch(pi_ptr, lpProcessInformation != nullptr, caller_wanted_suspended);
}

BOOL WINAPI hooked_CreateProcessW(
    LPCWSTR lpApplicationName, LPWSTR lpCommandLine,
    LPSECURITY_ATTRIBUTES lpProcessAttributes, LPSECURITY_ATTRIBUTES lpThreadAttributes,
    BOOL bInheritHandles, DWORD dwCreationFlags, LPVOID lpEnvironment,
    LPCWSTR lpCurrentDirectory, LPSTARTUPINFOW lpStartupInfo,
    LPPROCESS_INFORMATION lpProcessInformation) {

    if (g_bypass_hooks || !SocketShimCore::instance().isInitialized()) {
        return orig_CreateProcessW(lpApplicationName, lpCommandLine, lpProcessAttributes,
                                   lpThreadAttributes, bInheritHandles, dwCreationFlags,
                                   lpEnvironment, lpCurrentDirectory, lpStartupInfo, lpProcessInformation);
    }

    if ((lpCommandLine && std::wcsstr(lpCommandLine, L"apivpn_injector") != nullptr) ||
        (lpApplicationName && std::wcsstr(lpApplicationName, L"apivpn_injector") != nullptr)) {
        return orig_CreateProcessW(lpApplicationName, lpCommandLine, lpProcessAttributes,
                                   lpThreadAttributes, bInheritHandles, dwCreationFlags,
                                   lpEnvironment, lpCurrentDirectory, lpStartupInfo, lpProcessInformation);
    }

    ScopedHookBypass bypass;
    const bool caller_wanted_suspended = (dwCreationFlags & CREATE_SUSPENDED) != 0;
    std::vector<wchar_t> env_storage;
    LPVOID env = lpEnvironment;
    const DWORD effective_flags = prepareChildCreation(dwCreationFlags, env, env_storage);
    PROCESS_INFORMATION local_pi{};
    LPPROCESS_INFORMATION pi_ptr = lpProcessInformation ? lpProcessInformation : &local_pi;

    BOOL ok = orig_CreateProcessW(
        lpApplicationName, lpCommandLine, lpProcessAttributes, lpThreadAttributes,
        bInheritHandles, effective_flags, env,
        lpCurrentDirectory, lpStartupInfo, pi_ptr);
    if (!ok) {
        return FALSE;
    }
    return finishChildLaunch(pi_ptr, lpProcessInformation != nullptr, caller_wanted_suspended);
}

BOOL WINAPI hooked_AttachConsole(DWORD dwProcessId) {
    if (!g_bypass_hooks && SocketShimCore::instance().isInitialized()) {
        if (dwProcessId == ATTACH_PARENT_PROCESS) {
            SetLastError(ERROR_ACCESS_DENIED);
            return FALSE;
        }
    }
    return orig_AttachConsole(dwProcessId);
}

// Detour one export. `orig` is set to the export itself first, so if the detour cannot be
// installed every caller of orig_* still reaches the real (unpatched) function; on success
// installDetour publishes the trampoline into `orig` before the patch goes live.
template <typename Fn>
bool hookExport(HMODULE module, const char* name, std::type_identity_t<Fn> detour, Fn& orig) {
    void* target = module ? reinterpret_cast<void*>(GetProcAddress(module, name)) : nullptr;
    if (!target) {
        hookLog("export not found", name);
        return false;
    }
    orig = reinterpret_cast<Fn>(target);
    if (HookBackend::installDetour(target, reinterpret_cast<void*>(detour), reinterpret_cast<void**>(&orig))) {
        return true;
    }
    orig = reinterpret_cast<Fn>(target);
    hookLog("detour installation failed", name);
    return false;
}

// Like hookExport, but an export that does not exist on this Windows version is not an error.
template <typename Fn>
bool hookExportIfPresent(HMODULE module, const char* name, std::type_identity_t<Fn> detour, Fn& orig) {
    if (!module || !GetProcAddress(module, name)) {
        return true;
    }
    return hookExport(module, name, detour, orig);
}

void installAllHooks() {
    // Keep iphlpapi.dll in the static import table so the OS loader maps it before DllMain
    void* volatile force_link_iphlp = reinterpret_cast<void*>(&::GetAdaptersInfo);
    (void)force_link_iphlp;

    SocketShimCore::instance().setRawIpcExchange(rawIpcExchangeWin32);

    // Every Winsock socket/data detour is required: with one missing the shim would virtualize
    // only part of the traffic. Then the shim is never initialized, so all detours pass straight
    // through to orig_*, which hookExport guarantees to be callable. Fail closed, never crash.
    // The completion APIs below are a separate group: losing one only disables the overlapped
    // receive fix-up (see g_overlapped_fixup_ok), not the whole virtual LAN.
    bool required_ok = true;
    auto require = [&](bool installed) { required_ok = required_ok && installed; };

    HMODULE hWs2 = GetModuleHandleW(L"ws2_32.dll");
    if (!hWs2) hWs2 = LoadLibraryW(L"ws2_32.dll");
    require(hWs2 != nullptr);
    require(hookExport(hWs2, "socket", &hooked_socket, orig_socket));
    require(hookExport(hWs2, "WSASocketA", &hooked_WSASocketA, orig_WSASocketA));
    require(hookExport(hWs2, "WSASocketW", &hooked_WSASocketW, orig_WSASocketW));
    require(hookExport(hWs2, "closesocket", &hooked_closesocket, orig_closesocket));
    require(hookExport(hWs2, "bind", &hooked_bind, orig_bind));
    require(hookExport(hWs2, "getsockname", &hooked_getsockname, orig_getsockname));
    require(hookExport(hWs2, "getpeername", &hooked_getpeername, orig_getpeername));
    require(hookExport(hWs2, "sendto", &hooked_sendto, orig_sendto));
    require(hookExport(hWs2, "WSASendTo", &hooked_WSASendTo, orig_WSASendTo));
    require(hookExport(hWs2, "recvfrom", &hooked_recvfrom, orig_recvfrom));
    require(hookExport(hWs2, "WSARecvFrom", &hooked_WSARecvFrom, orig_WSARecvFrom));
    require(hookExport(hWs2, "send", &hooked_send, orig_send));
    require(hookExport(hWs2, "WSASend", &hooked_WSASend, orig_WSASend));
    require(hookExport(hWs2, "recv", &hooked_recv, orig_recv));
    require(hookExport(hWs2, "WSARecv", &hooked_WSARecv, orig_WSARecv));
    require(hookExport(hWs2, "connect", &hooked_connect, orig_connect));
    require(hookExport(hWs2, "WSAConnect", &hooked_WSAConnect, orig_WSAConnect));
    require(hookExport(hWs2, "accept", &hooked_accept, orig_accept));
    require(hookExport(hWs2, "WSAAccept", &hooked_WSAAccept, orig_WSAAccept));
    require(hookExport(hWs2, "gethostbyname", &hooked_gethostbyname, orig_gethostbyname));
    require(hookExport(hWs2, "getaddrinfo", &hooked_getaddrinfo, orig_getaddrinfo));
    require(hookExport(hWs2, "WSAIoctl", &hooked_WSAIoctl, orig_WSAIoctl));

    HMODULE hKernelBase = GetModuleHandleW(L"kernelbase.dll");
    HMODULE hKernel32 = GetModuleHandleW(L"kernel32.dll");

    // Completion APIs live in kernelbase (kernel32 forwards there, and mswsock calls kernelbase
    // directly); on systems where kernelbase lacks the export, detour kernel32's.
    auto ioModule = [&](const char* export_name) -> HMODULE {
        if (hKernelBase && GetProcAddress(hKernelBase, export_name)) return hKernelBase;
        return hKernel32;
    };
    bool completion_ok = true;
    auto completion = [&](bool installed) { completion_ok = completion_ok && installed; };
    completion(hookExport(hWs2, "WSAGetOverlappedResult", &hooked_WSAGetOverlappedResult, orig_WSAGetOverlappedResult));
    completion(hookExport(ioModule("GetOverlappedResult"), "GetOverlappedResult",
                          &hooked_GetOverlappedResult, orig_GetOverlappedResult));
    completion(hookExport(ioModule("GetQueuedCompletionStatus"), "GetQueuedCompletionStatus",
                          &hooked_GetQueuedCompletionStatus, orig_GetQueuedCompletionStatus));
    completion(hookExportIfPresent(ioModule("GetQueuedCompletionStatusEx"), "GetQueuedCompletionStatusEx",
                                   &hooked_GetQueuedCompletionStatusEx, orig_GetQueuedCompletionStatusEx));
    completion(hookExportIfPresent(ioModule("GetOverlappedResultEx"), "GetOverlappedResultEx",
                                   &hooked_GetOverlappedResultEx, orig_GetOverlappedResultEx));
    g_overlapped_fixup_ok.store(completion_ok, std::memory_order_release);
    if (!completion_ok) {
        hookLog("a completion API detour is missing; overlapped UDP receives without a "
                "completion routine are passed through un-virtualized");
    }

    // Optional detours: a failure is logged by hookExport and only loses that feature.
    HMODULE hIphlp = GetModuleHandleW(L"iphlpapi.dll");
    if (!hIphlp) hIphlp = LoadLibraryW(L"iphlpapi.dll");
    if (hIphlp) {
        hookExport(hIphlp, "GetAdaptersInfo", &hooked_GetAdaptersInfo, orig_GetAdaptersInfo);
        hookExport(hIphlp, "GetAdaptersAddresses", &hooked_GetAdaptersAddresses, orig_GetAdaptersAddresses);
    }

    if (hKernelBase) {
        hookExport(hKernelBase, "CreateProcessInternalW", &hooked_CreateProcessInternalW, orig_CreateProcessInternalW);
        hookExport(hKernelBase, "LoadLibraryExW", &hooked_LoadLibraryExW, orig_LoadLibraryExW);
        hookExport(hKernelBase, "CreateEventExW", &hooked_CreateEventExW, orig_CreateEventExW);
        hookExport(hKernelBase, "OpenEventW", &hooked_OpenEventW, orig_OpenEventW);
        hookExport(hKernelBase, "CreateMutexExW", &hooked_CreateMutexExW, orig_CreateMutexExW);
        hookExport(hKernelBase, "OpenMutexW", &hooked_OpenMutexW, orig_OpenMutexW);
        hookExport(hKernelBase, "CreateFileMappingNumaW", &hooked_CreateFileMappingNumaW, orig_CreateFileMappingNumaW);
        hookExport(hKernelBase, "OpenFileMappingW", &hooked_OpenFileMappingW, orig_OpenFileMappingW);
    }

    if (hKernel32) {
        hookExport(hKernel32, "CreateProcessA", &hooked_CreateProcessA, orig_CreateProcessA);
        hookExport(hKernel32, "CreateProcessW", &hooked_CreateProcessW, orig_CreateProcessW);
        hookExport(hKernel32, "AttachConsole", &hooked_AttachConsole, orig_AttachConsole);
    }

    // Only now enable virtualization (all detours pass through until then)
    if (!required_ok) {
        hookLog("a required detour is missing; virtual LAN disabled for this process");
        return;
    }
    if (!SocketShimCore::instance().initFromEnvironment()) {
        hookLog("APIVPN_* environment missing or invalid; virtual LAN disabled for this process");
    }
}

} // namespace

BOOL APIENTRY DllMain(HMODULE hModule, DWORD ul_reason_for_call, LPVOID /*lpReserved*/) {
    if (ul_reason_for_call == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(hModule);
        installAllHooks();
    }
    return TRUE;
}
