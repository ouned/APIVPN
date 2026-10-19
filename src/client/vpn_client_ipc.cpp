#include "apivpn/client/vpn_client.hpp"
#include "apivpn/common/logger.hpp"

#include <algorithm>
#include <chrono>
#include <cstring>
#include <memory>
#include <thread>
#include <vector>

// Purpose: Local IPC server loop: hook DLL bind registration and TCP connect preparation.

namespace apivpn::client {

void VpnClient::ipcWorkerLoop() {
    std::vector<platform::PollItem> items;
    while (gateways_running_.load()) {
        items.clear();
        items.push_back({ipc_listener_.handle(), true, false, false, false, false});
        platform::pollSockets(items, 15);
        if (!items[0].can_read) continue;

        while (gateways_running_.load()) {
            platform::NetSocket client = ipc_listener_.accept();
            if (!client.isValid()) break;
            client.setNoDelay(true);

            IpcRequest req{};
            if (client.recvExact(&req, sizeof(req), 1000) != sizeof(req) || req.magic != kIpcMagic) {
                continue;
            }

            IpcResponse resp{};
            resp.magic = kIpcMagic;
            resp.status = 0;

            auto cmd = static_cast<IpcCommand>(req.command);
            if (cmd == IpcCommand::RegisterBind) {
                std::lock_guard<std::mutex> lock(mutex_);
                ensureProcessTrackedLocked(req.pid);
                IpcProto proto = static_cast<IpcProto>(req.proto);
                bound_sockets_.erase(
                    std::remove_if(bound_sockets_.begin(), bound_sockets_.end(),
                                   [&](const BoundSocketEntry& b) {
                                       return b.proto == proto && b.actual_port == req.actual_loopback_port;
                                   }),
                    bound_sockets_.end());

                BoundSocketEntry entry{};
                entry.proto = proto;
                entry.virtual_port = req.virtual_port;
                entry.actual_port = req.actual_loopback_port;
                entry.pid = req.pid;
                bound_sockets_.push_back(entry);
                resp.assigned_vport = req.virtual_port;
            } else if (cmd == IpcCommand::UnregisterBind) {
                // Match the exact registration: an accepted TCP socket shares its listener's
                // loopback port, so matching on the port alone would drop the listener too.
                std::lock_guard<std::mutex> lock(mutex_);
                IpcProto proto = static_cast<IpcProto>(req.proto);
                const uint16_t vport = req.virtual_port;
                const uint16_t aport = req.actual_loopback_port;
                bound_sockets_.erase(
                    std::remove_if(bound_sockets_.begin(), bound_sockets_.end(),
                                   [&](const BoundSocketEntry& b) {
                                       return b.proto == proto && b.virtual_port == vport && b.actual_port == aport;
                                   }),
                    bound_sockets_.end());
            } else if (cmd == IpcCommand::QueryBind) {
                // status 0 if a hooked socket of this client is bound to (proto, virtual_port)
                std::lock_guard<std::mutex> lock(mutex_);
                IpcProto proto = static_cast<IpcProto>(req.proto);
                resp.status = 1;
                for (const auto& b : bound_sockets_) {
                    if (b.proto == proto && b.virtual_port == req.virtual_port) {
                        resp.status = 0;
                        resp.assigned_vport = b.actual_port;
                        break;
                    }
                }
            } else if (cmd == IpcCommand::PrepareTcpConnect) {
                uint32_t my_vip = local_vip_.load();
                if ((my_vip != 0 && req.target_vip == my_vip) || req.target_vip == kLoopbackIp) {
                    std::lock_guard<std::mutex> lock(mutex_);
                    ensureProcessTrackedLocked(req.pid);
                    uint16_t target_actual_port = 0;
                    for (const auto& b : bound_sockets_) {
                        if (b.proto == IpcProto::Tcp && b.virtual_port == req.target_vport) {
                            target_actual_port = b.actual_port;
                            break;
                        }
                    }
                    if (target_actual_port != 0) {
                        uint16_t src_vport = (req.virtual_port != 0) ? req.virtual_port : req.actual_loopback_port;
                        incoming_tcp_peer_map_[req.actual_loopback_port] = {my_vip, src_vport};
                        resp.assigned_vport = target_actual_port;
                        resp.status = 0;
                    } else {
                        // No hooked listener: non-zero so the hook falls back to a real OS connect.
                        resp.status = 1;
                    }
                } else {
                    // Hand the (potentially multi-second) remote handshake to a
                    // worker so the IPC thread keeps serving other processes.
                    IpcPendingConnect job;
                    job.sock = std::move(client);
                    job.req = req;
                    {
                        std::lock_guard<std::mutex> qlock(ipc_queue_mutex_);
                        ipc_connect_queue_.push_back(std::move(job));
                    }
                    ipc_queue_cv_.notify_one();
                    continue; // the worker writes the response
                }
            } else if (cmd == IpcCommand::QueryTcpPeer) {
                std::lock_guard<std::mutex> lock(mutex_);
                auto it = incoming_tcp_peer_map_.find(req.actual_loopback_port);
                if (it != incoming_tcp_peer_map_.end()) {
                    resp.peer_vip = it->second.first;
                    resp.peer_vport = it->second.second;
                } else {
                    resp.status = 1;
                }
            } else if (cmd == IpcCommand::RegisterProcess) {
                std::lock_guard<std::mutex> lock(mutex_);
                ensureProcessTrackedLocked(req.pid);
                if (req.pid != 0) {
                    proc_telemetry_[req.pid];
                }
                resp.status = 0;
            } else {
                resp.status = 1; // unknown command
            }

            client.sendAll(&resp, sizeof(resp));
        }
    }
}

void VpnClient::startIpcConnectWorkers() {
    constexpr size_t kConnectWorkers = 4;
    {
        std::lock_guard<std::mutex> lock(ipc_queue_mutex_);
        ipc_connect_stopping_ = false;
    }
    ipc_connect_workers_.reserve(kConnectWorkers);
    for (size_t i = 0; i < kConnectWorkers; ++i) {
        ipc_connect_workers_.emplace_back(&VpnClient::ipcConnectWorkerLoop, this);
    }
}

void VpnClient::stopIpcConnectWorkers() {
    {
        std::lock_guard<std::mutex> lock(ipc_queue_mutex_);
        ipc_connect_stopping_ = true;
    }
    ipc_queue_cv_.notify_all();
    for (auto& worker : ipc_connect_workers_) {
        if (worker.joinable()) worker.join();
    }
    ipc_connect_workers_.clear();
    // Drop anything that was queued but never started (closes those sockets).
    std::lock_guard<std::mutex> lock(ipc_queue_mutex_);
    ipc_connect_queue_.clear();
}

void VpnClient::ipcConnectWorkerLoop() {
    while (true) {
        IpcPendingConnect job;
        {
            std::unique_lock<std::mutex> lock(ipc_queue_mutex_);
            ipc_queue_cv_.wait(lock, [this]() {
                return ipc_connect_stopping_ || !ipc_connect_queue_.empty();
            });
            if (ipc_connect_stopping_) return;
            job = std::move(ipc_connect_queue_.front());
            ipc_connect_queue_.pop_front();
        }
        handleIpcRemoteTcpConnect(std::move(job));
    }
}

void VpnClient::handleIpcRemoteTcpConnect(IpcPendingConnect job) {
    IpcResponse resp{};
    resp.magic = kIpcMagic;
    resp.status = 0;

    uint32_t cid = 0;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        ensureProcessTrackedLocked(job.req.pid);
        if (local_vip_.load() == 0 || tcp_tunnels_.size() >= kMaxTcpTunnelsTotal) {
            // No VIP yet (nothing could be sent) or tunnel table full: refuse right away.
            resp.status = 1;
            job.sock.sendAll(&resp, sizeof(resp));
            job.sock.close();
            return;
        }
        cid = allocateConnId();
        auto tunnel = std::make_unique<TcpTunnel>();
        tunnel->conn_id = cid;
        tunnel->created_us = platform::monotonicMicros();
        tunnel->pid = job.req.pid;
        tunnel->remote_vip = job.req.target_vip;
        tunnel->remote_vport = job.req.target_vport;
        tunnel->local_vport = (job.req.virtual_port != 0) ? job.req.virtual_port : job.req.actual_loopback_port;
        tunnel->expected_local_port = job.req.actual_loopback_port;
        auto peer_it = peers_.find(job.req.target_vip);
        if (peer_it != peers_.end() && peer_it->second.rtt_ms > 0) {
            tunnel->updateRtt(static_cast<uint64_t>(peer_it->second.rtt_ms) * 1000ULL);
        }

        sendTcpFrame(*tunnel, static_cast<uint8_t>(VpnTcpFlags::Syn), 0, nullptr, 0);
        tcp_tunnels_[cid] = std::move(tunnel);
    }

    // Wait up to 2.5s for the remote VpnTcpSynAck (retransmitting after 200ms via relay if needed)
    {
        std::unique_lock<std::mutex> lock(mutex_);
        bool done = false;
        for (int attempt = 0; attempt < 12 && gateways_running_.load(); ++attempt) {
            done = cv_.wait_for(lock, std::chrono::milliseconds(200), [&]() {
                if (!gateways_running_.load()) return true;
                auto it = tcp_tunnels_.find(cid);
                if (it == tcp_tunnels_.end()) return true;
                return it->second->handshake_done || it->second->handshake_failed;
            });
            if (done) break;
            auto it = tcp_tunnels_.find(cid);
            if (it != tcp_tunnels_.end() && !it->second->handshake_done) {
                sendTcpFrame(*it->second, static_cast<uint8_t>(VpnTcpFlags::Syn), 0, nullptr, 0, attempt >= 1);
            }
        }

        auto it = tcp_tunnels_.find(cid);
        if (it == tcp_tunnels_.end() || !it->second->handshake_done || it->second->handshake_failed) {
            if (it != tcp_tunnels_.end()) {
                // RST in case the peer accepted after all (its SYN-ACK lost or late); this also
                // retires the socket, which the network thread may be polling.
                abortTcpTunnel(*it->second, true);
                tcp_tunnels_.erase(it);
            }
            resp.status = 1; // Connection refused / timed out
        } else {
            resp.assigned_vport = tcp_gw_port_;
        }
    }

    job.sock.sendAll(&resp, sizeof(resp));
    job.sock.close();
}


} // namespace apivpn::client
