#pragma once

#include "apivpn/common/protocol.hpp"
#include <atomic>
#include <chrono>
#include <cstdint>
#include <functional>
#include <mutex>
#include <unordered_map>

namespace apivpn::hook {

struct VirtualSocketState {
    uint64_t handle               = 0;
    int      af                   = 0;
    int      type                 = 0; // SOCK_DGRAM or SOCK_STREAM
    bool     is_bound             = false;
    bool     is_virtualized       = false;
    // True only for the socket that registered (proto, virtual_port, actual_loopback_port) with
    // VpnClient itself. Accepted TCP sockets copy their listener's ports for getsockname() but must
    // never unregister the listener's binding when they are closed.
    bool     owns_binding         = false;
    // Explicitly bound to a real address (127.0.0.1, a physical NIC, ...) or a failed virtual bind:
    // the socket is a plain OS socket and every detour passes it through untouched.
    bool     passthrough          = false;
    uint32_t bound_vip            = 0; // Host byte order (0 = INADDR_ANY)
    uint16_t virtual_port         = 0; // Host byte order
    uint16_t actual_loopback_port = 0; // Host byte order
    bool     has_remote_peer      = false;
    uint32_t remote_vip           = 0; // Host byte order
    uint16_t remote_vport         = 0; // Host byte order
};

// Portable core managing virtual socket state and communicating with local VpnClient daemon
class SocketShimCore {
public:
    static SocketShimCore& instance();

    // Initialize from environment variables set by launcher/client
    bool initFromEnvironment();

    bool isInitialized() const { return initialized_.load(std::memory_order_acquire); }
    uint32_t localVip() const { return local_vip_; }
    uint16_t ipcPort() const { return ipc_port_; }
    uint16_t udpGatewayPort() const { return udp_gw_port_; }
    uint16_t tcpGatewayPort() const { return tcp_gw_port_; }
    uint64_t sessionToken() const { return session_token_; }

    void registerSocket(uint64_t handle, int af, int type);
    void unregisterSocket(uint64_t handle);
    bool getSocketState(uint64_t handle, VirtualSocketState& out_state);
    void updateSocketState(uint64_t handle, const VirtualSocketState& state);

    // Atomically applies fn(VirtualSocketState&) to a known socket under the shim mutex, so
    // concurrent field updates cannot overwrite each other. fn must be quick and must not block
    // (no IPC, no Winsock calls). Returns false if the socket is unknown.
    template <typename Fn>
    bool modifySocketState(uint64_t handle, Fn&& fn) {
        std::lock_guard<std::mutex> lock(mutex_);
        auto it = sockets_.find(handle);
        if (it == sockets_.end()) return false;
        fn(it->second);
        return true;
    }

    // IPC operations with local VpnClient daemon
    bool ipcRegisterBind(IpcProto proto, uint16_t vport, uint16_t actual_port, uint16_t& out_assigned_vport);
    bool ipcUnregisterBind(IpcProto proto, uint16_t vport, uint16_t actual_port);
    bool ipcPrepareTcpConnect(uint16_t actual_local_port, uint32_t target_vip, uint16_t target_vport, uint16_t local_vport = 0, uint16_t* out_connect_port = nullptr);
    bool ipcQueryTcpPeer(uint16_t accepted_peer_loopback_port, uint32_t& out_peer_vip, uint16_t& out_peer_vport);
    bool ipcRegisterProcess(uint32_t child_pid);
    // True if a hooked socket of this client holds the virtual binding (proto, vport), i.e. traffic
    // to 127.0.0.1:vport is meant for a virtual-LAN service rather than a real local one. Answers
    // are cached for kBindQueryCacheTtl; an IPC failure counts as "not bound" (and is not cached).
    bool ipcQueryBind(IpcProto proto, uint16_t vport);

    // Custom raw IPC sender provided by the platform hook layer (bypasses hooks)
    using RawIpcExchangeFn = std::function<bool(uint16_t ipc_port, const IpcRequest& req, IpcResponse& resp)>;
    void setRawIpcExchange(RawIpcExchangeFn fn) {
        std::lock_guard<std::mutex> lock(mutex_);
        ipc_exchange_fn_ = std::move(fn);
    }

private:
    SocketShimCore() = default;
    bool performIpc(const IpcRequest& req, IpcResponse& resp);
    void cacheBindQuery(IpcProto proto, uint16_t vport, bool bound);
    void forgetBindQuery(IpcProto proto, uint16_t vport);

    static constexpr std::chrono::milliseconds kBindQueryCacheTtl{1000};
    static constexpr size_t kMaxBindQueryCacheEntries = 1024;

    struct BindQueryEntry {
        bool bound = false;
        std::chrono::steady_clock::time_point expires{};
    };

    std::mutex mutex_;
    std::atomic<bool> initialized_{false};
    uint32_t local_vip_    = 0;
    uint16_t ipc_port_     = 0;
    uint16_t udp_gw_port_  = 0;
    uint16_t tcp_gw_port_  = 0;
    uint64_t session_token_= 0;
    uint32_t pid_          = 0;

    RawIpcExchangeFn ipc_exchange_fn_;
    std::unordered_map<uint64_t, VirtualSocketState> sockets_;
    std::unordered_map<uint32_t, BindQueryEntry> bind_query_cache_; // key: (proto << 16) | vport
};

} // namespace apivpn::hook
