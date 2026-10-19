#include "apivpn/hook/socket_shim_core.hpp"
#include "apivpn/platform/net_socket.hpp"

#ifdef _WIN32
#include "apivpn/platform/win32_utf8.hpp"
#endif

#include <cstdlib>
#include <string>

namespace apivpn::hook {

namespace {

// Read an environment variable (UTF-8). On Windows this goes through GetEnvironmentVariableW so
// the value is taken from the live process environment, not a CRT copy in the ANSI code page.
bool readEnv(const char* name, std::string& out) {
#ifdef _WIN32
    return platform::getEnvUtf8(name, out);
#else
    const char* value = std::getenv(name);
    if (!value) return false;
    out = value;
    return true;
#endif
}

uint32_t bindQueryKey(IpcProto proto, uint16_t vport) {
    return (static_cast<uint32_t>(proto) << 16) | vport;
}

} // namespace

SocketShimCore& SocketShimCore::instance() {
    static SocketShimCore inst;
    return inst;
}

bool SocketShimCore::initFromEnvironment() {
    std::lock_guard<std::mutex> lock(mutex_);
    if (initialized_.load(std::memory_order_relaxed)) return true;

    std::string vip_str, ipc_str, udp_str, tcp_str, tok_str;
    if (!readEnv(kEnvVirtualIp, vip_str) || !readEnv(kEnvIpcPort, ipc_str) ||
        !readEnv(kEnvUdpGwPort, udp_str) || !readEnv(kEnvTcpGwPort, tcp_str)) {
        return false;
    }

    if (!parseIpv4String(vip_str, local_vip_)) {
        return false;
    }
    ipc_port_ = static_cast<uint16_t>(std::strtoul(ipc_str.c_str(), nullptr, 10));
    udp_gw_port_ = static_cast<uint16_t>(std::strtoul(udp_str.c_str(), nullptr, 10));
    tcp_gw_port_ = static_cast<uint16_t>(std::strtoul(tcp_str.c_str(), nullptr, 10));
    if (readEnv(kEnvSessionToken, tok_str)) {
        session_token_ = std::strtoull(tok_str.c_str(), nullptr, 10);
    }
    pid_ = platform::currentProcessId();
    const bool ok = (local_vip_ != 0 && ipc_port_ != 0 && udp_gw_port_ != 0 && tcp_gw_port_ != 0);
    // Release: everything above is visible to any thread that sees isInitialized() == true
    initialized_.store(ok, std::memory_order_release);
    return ok;
}

void SocketShimCore::registerSocket(uint64_t handle, int af, int type) {
    std::lock_guard<std::mutex> lock(mutex_);
    VirtualSocketState st{};
    st.handle = handle;
    st.af = af;
    st.type = type;
    sockets_[handle] = st;
}

void SocketShimCore::unregisterSocket(uint64_t handle) {
    VirtualSocketState st{};
    bool had_bind = false;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        auto it = sockets_.find(handle);
        if (it != sockets_.end()) {
            st = it->second;
            // Only the socket that registered the binding releases it; an accepted TCP socket
            // shares its listener's ports and closing it must not unregister the listener.
            had_bind = st.owns_binding;
            sockets_.erase(it);
        }
    }
    if (had_bind) {
        IpcProto proto = (st.type == 1 /* SOCK_STREAM */) ? IpcProto::Tcp : IpcProto::Udp;
        ipcUnregisterBind(proto, st.virtual_port, st.actual_loopback_port);
    }
}

bool SocketShimCore::getSocketState(uint64_t handle, VirtualSocketState& out_state) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = sockets_.find(handle);
    if (it == sockets_.end()) return false;
    out_state = it->second;
    return true;
}

void SocketShimCore::updateSocketState(uint64_t handle, const VirtualSocketState& state) {
    std::lock_guard<std::mutex> lock(mutex_);
    sockets_[handle] = state;
}

bool SocketShimCore::performIpc(const IpcRequest& req, IpcResponse& resp) {
    RawIpcExchangeFn fn;
    uint16_t port = 0;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        fn = ipc_exchange_fn_;
        port = ipc_port_;
    }
    if (!fn || port == 0) return false;
    return fn(port, req, resp);
}

void SocketShimCore::cacheBindQuery(IpcProto proto, uint16_t vport, bool bound) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (bind_query_cache_.size() >= kMaxBindQueryCacheEntries) {
        bind_query_cache_.clear();
    }
    BindQueryEntry& entry = bind_query_cache_[bindQueryKey(proto, vport)];
    entry.bound = bound;
    entry.expires = std::chrono::steady_clock::now() + kBindQueryCacheTtl;
}

void SocketShimCore::forgetBindQuery(IpcProto proto, uint16_t vport) {
    std::lock_guard<std::mutex> lock(mutex_);
    bind_query_cache_.erase(bindQueryKey(proto, vport));
}

bool SocketShimCore::ipcRegisterBind(IpcProto proto, uint16_t vport, uint16_t actual_port, uint16_t& out_assigned_vport) {
    IpcRequest req{};
    req.magic = kIpcMagic;
    req.command = static_cast<uint8_t>(IpcCommand::RegisterBind);
    req.proto = static_cast<uint8_t>(proto);
    req.virtual_port = vport;
    req.actual_loopback_port = actual_port;
    req.pid = pid_;

    IpcResponse resp{};
    if (!performIpc(req, resp) || resp.magic != kIpcMagic || resp.status != 0) {
        return false;
    }
    out_assigned_vport = resp.assigned_vport;
    // Our own binding is known to exist now; don't let a stale "not bound" answer misroute traffic
    cacheBindQuery(proto, resp.assigned_vport, true);
    return true;
}

bool SocketShimCore::ipcUnregisterBind(IpcProto proto, uint16_t vport, uint16_t actual_port) {
    IpcRequest req{};
    req.magic = kIpcMagic;
    req.command = static_cast<uint8_t>(IpcCommand::UnregisterBind);
    req.proto = static_cast<uint8_t>(proto);
    req.virtual_port = vport;
    req.actual_loopback_port = actual_port;
    req.pid = pid_;

    // Another socket may still hold the same virtual port, so re-ask VpnClient next time
    forgetBindQuery(proto, vport);
    IpcResponse resp{};
    return performIpc(req, resp) && resp.magic == kIpcMagic && resp.status == 0;
}

bool SocketShimCore::ipcQueryBind(IpcProto proto, uint16_t vport) {
    const auto now = std::chrono::steady_clock::now();
    {
        std::lock_guard<std::mutex> lock(mutex_);
        auto it = bind_query_cache_.find(bindQueryKey(proto, vport));
        if (it != bind_query_cache_.end() && now < it->second.expires) {
            return it->second.bound;
        }
    }

    IpcRequest req{};
    req.magic = kIpcMagic;
    req.command = static_cast<uint8_t>(IpcCommand::QueryBind);
    req.proto = static_cast<uint8_t>(proto);
    req.virtual_port = vport;
    req.pid = pid_;

    IpcResponse resp{};
    if (!performIpc(req, resp) || resp.magic != kIpcMagic) {
        return false;
    }
    const bool bound = (resp.status == 0);
    cacheBindQuery(proto, vport, bound);
    return bound;
}

bool SocketShimCore::ipcPrepareTcpConnect(uint16_t actual_local_port, uint32_t target_vip, uint16_t target_vport, uint16_t local_vport, uint16_t* out_connect_port) {
    IpcRequest req{};
    req.magic = kIpcMagic;
    req.command = static_cast<uint8_t>(IpcCommand::PrepareTcpConnect);
    req.proto = static_cast<uint8_t>(IpcProto::Tcp);
    req.virtual_port = (local_vport != 0) ? local_vport : actual_local_port;
    req.actual_loopback_port = actual_local_port;
    req.target_vip = target_vip;
    req.target_vport = target_vport;
    req.pid = pid_;

    IpcResponse resp{};
    if (!performIpc(req, resp) || resp.magic != kIpcMagic || resp.status != 0) {
        return false;
    }
    if (out_connect_port) {
        *out_connect_port = (resp.assigned_vport != 0) ? resp.assigned_vport : tcp_gw_port_;
    }
    return true;
}

bool SocketShimCore::ipcQueryTcpPeer(uint16_t accepted_peer_loopback_port, uint32_t& out_peer_vip, uint16_t& out_peer_vport) {
    IpcRequest req{};
    req.magic = kIpcMagic;
    req.command = static_cast<uint8_t>(IpcCommand::QueryTcpPeer);
    req.proto = static_cast<uint8_t>(IpcProto::Tcp);
    req.actual_loopback_port = accepted_peer_loopback_port;
    req.pid = pid_;

    IpcResponse resp{};
    if (!performIpc(req, resp) || resp.magic != kIpcMagic || resp.status != 0) {
        return false;
    }
    out_peer_vip = resp.peer_vip;
    out_peer_vport = resp.peer_vport;
    return true;
}

bool SocketShimCore::ipcRegisterProcess(uint32_t child_pid) {
    if (child_pid == 0) return false;
    IpcRequest req{};
    req.magic = kIpcMagic;
    req.command = static_cast<uint8_t>(IpcCommand::RegisterProcess);
    req.pid = child_pid;

    IpcResponse resp{};
    return performIpc(req, resp) && resp.magic == kIpcMagic && resp.status == 0;
}

} // namespace apivpn::hook
