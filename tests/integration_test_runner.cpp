// APIVPN end-to-end integration test runner.
//
// Each test is independent and owns a fresh VpnServer + client fixture, so one
// failure never prevents the remaining tests from running and no test leaks
// configuration into another. The runner prints a readable per-test report and
// returns a non-zero exit code if any test fails.

#include "apivpn/client/vpn_client.hpp"
#include "apivpn/common/logger.hpp"
#include "apivpn/platform/net_socket.hpp"
#include "apivpn/platform/process_launcher.hpp"
#include "apivpn/server/vpn_server.hpp"

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <initializer_list>
#include <iostream>
#include <memory>
#include <string>
#include <thread>
#include <vector>

using namespace apivpn;

// ============================================================================
// Minimal test framework
// ============================================================================
namespace {

using TestFn = bool (*)();
struct TestCase {
    std::string name;
    TestFn      fn;
};

std::vector<TestCase>& testRegistry() {
    static std::vector<TestCase> reg;
    return reg;
}

bool registerTest(const char* name, TestFn fn) {
    testRegistry().push_back({name, fn});
    return true;
}

} // namespace

#define APIVPN_TEST(name)                                                        \
    static bool name();                                                          \
    [[maybe_unused]] static const bool name##_registered = registerTest(#name, name); \
    static bool name()

namespace {

// ============================================================================
// Globals & small utilities
// ============================================================================
std::string g_bin_dir;
std::string g_lastError;
std::string g_runTag;              // runner PID: concurrent suite runs never share report files
std::string g_currentTest;         // running test's name, also part of every report path
int64_t     g_testStartEpochSec = 0;

// Everything the current test launched. ~TestCluster reaps apps that are still alive (also
// when a CHECK returned early), and main() deletes the report files once the test passed
// (they are kept for inspection when it failed).
struct LaunchedApps {
    std::vector<uint32_t>    all;     // every app PID (roots for the descendant search)
    std::vector<uint32_t>    running; // PIDs not yet observed to exit
    std::vector<std::string> reports;
};
LaunchedApps g_apps;

#define CHECK(cond, msg) do { if (!(cond)) { g_lastError = (msg); return false; } } while (0)

// Fixed transfer sizes for the bulk TCP scenarios.
constexpr int kBulkSizeKb  = 4096;
constexpr int kSmallSizeKb = 1024;

template <typename Pred>
bool waitUntil(Pred pred, int timeout_ms, int step_ms = 25) {
    int elapsed = 0;
    while (elapsed < timeout_ms) {
        if (pred()) return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(step_ms));
        elapsed += step_ms;
    }
    return pred();
}

bool waitForProcessExit(uint32_t pid, int timeout_ms) {
    int elapsed = 0;
    while (elapsed < timeout_ms) {
        if (!platform::ProcessLauncher::isProcessAlive(pid)) return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
        elapsed += 50;
    }
    return !platform::ProcessLauncher::isProcessAlive(pid);
}

std::string appPath(const char* arch) {
    return g_bin_dir + "\\test_game_app" + arch + ".exe";
}

std::string reportPath(const std::string& name) {
    return g_bin_dir + "\\report_" + g_runTag + "_" + g_currentTest + "_" + name + ".txt";
}

bool startsWith(const std::string& s, const char* prefix) {
    return s.rfind(prefix, 0) == 0;
}

std::string readFirstLine(const std::string& path) {
    std::string line;
    std::ifstream in(path);
    if (in.is_open()) std::getline(in, line);
    return line;
}

const client::PeerViewInfo* findPeer(const client::ClientStatusSnapshot& s, uint32_t vip) {
    for (const auto& p : s.peers) {
        if (p.vip == vip) return &p;
    }
    return nullptr;
}

int parseUintAfter(const std::string& s, const std::string& key) {
    auto pos = s.find(key);
    if (pos == std::string::npos) return -1;
    pos += key.size();
    int v = 0;
    bool any = false;
    while (pos < s.size() && std::isdigit(static_cast<unsigned char>(s[pos]))) {
        v = v * 10 + (s[pos] - '0');
        ++pos;
        any = true;
    }
    return any ? v : -1;
}

struct AppHandle {
    uint32_t    pid = 0;
    std::string report_path;
    std::string report;
};

bool launchAppPid(client::VpnClient& c, const std::string& exe, const std::string& args,
                  const std::string& rep_path, AppHandle& h) {
    h.report_path = rep_path;
    if (!rep_path.empty()) {
        std::remove(rep_path.c_str());
        g_apps.reports.push_back(rep_path);
    }
    auto info = c.launchApp(exe, args);
    if (!info.success) {
        g_lastError = "launchApp failed: " + info.error_msg;
        return false;
    }
    h.pid = info.pid;
    g_apps.all.push_back(info.pid);
    g_apps.running.push_back(info.pid);
    return true;
}

// Overload that derives the report path from a `--report "<path>"` argument.
bool launchAppPid(client::VpnClient& c, const std::string& exe, const std::string& args,
                  AppHandle& h) {
    std::string rep;
    auto pos = args.find("--report \"");
    if (pos != std::string::npos) {
        auto start = pos + 10;
        auto end = args.find('"', start);
        if (end != std::string::npos) rep = args.substr(start, end - start);
    }
    return launchAppPid(c, exe, args, rep, h);
}

bool waitApp(AppHandle& h, int timeout_ms) {
    bool exited = waitForProcessExit(h.pid, timeout_ms);
    if (!exited) {
        platform::ProcessLauncher::terminateProcess(h.pid);
        waitForProcessExit(h.pid, 3000);
    }
    auto& running = g_apps.running;
    running.erase(std::remove(running.begin(), running.end(), h.pid), running.end());
    h.report = readFirstLine(h.report_path);
    return exited;
}

// Waits for every app against one shared deadline. Unlike chaining waitApp() calls with &&,
// this never short-circuits, so one timed-out app cannot leave the others unwaited.
bool waitApps(std::initializer_list<AppHandle*> apps, int timeout_ms) {
    auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
    bool all_exited = true;
    for (AppHandle* h : apps) {
        auto remain = std::chrono::duration_cast<std::chrono::milliseconds>(
            deadline - std::chrono::steady_clock::now()).count();
        if (!waitApp(*h, remain > 0 ? static_cast<int>(remain) : 0)) all_exited = false;
    }
    return all_exited;
}

// Blocks until a launched app reports "READY" (written once it has bound/listened), so the
// peer app can start without a fixed sleep. A finished app (PASS) also counts as ready; a
// FAIL report or an app that exited without one does not.
bool waitReady(const AppHandle& h, int timeout_ms = 10000) {
    waitUntil([&]() {
        std::string line = readFirstLine(h.report_path);
        return startsWith(line, "READY") || startsWith(line, "PASS") || startsWith(line, "FAIL") ||
               !platform::ProcessLauncher::isProcessAlive(h.pid);
    }, timeout_ms, 20);
    std::string line = readFirstLine(h.report_path);
    return startsWith(line, "READY") || startsWith(line, "PASS");
}

std::string notReady(const char* who, const AppHandle& h) {
    return std::string(who) + " never became ready (report: '" + readFirstLine(h.report_path) + "')";
}

// Terminates whatever the current test launched and is still alive, descendants first (e.g.
// the cross-bitness child) so killing a parent cannot orphan them, then waits for the exits.
void reapLaunchedApps() {
    if (g_apps.all.empty()) return;
    std::vector<uint32_t> victims =
        platform::ProcessLauncher::findAliveChildProcesses(g_apps.all, g_testStartEpochSec);
    // Only PIDs not yet seen exiting: an exited app's PID may already belong to another process.
    victims.insert(victims.end(), g_apps.running.begin(), g_apps.running.end());
    for (uint32_t pid : victims) {
        if (platform::ProcessLauncher::isProcessAlive(pid)) {
            platform::ProcessLauncher::terminateProcess(pid);
        }
    }
    for (uint32_t pid : victims) waitForProcessExit(pid, 3000);
    g_apps.all.clear();
    g_apps.running.clear();
}

// ============================================================================
// Fixture: fresh server + clients per test
// ============================================================================
struct TestCluster {
    server::VpnServer server;
    std::vector<std::unique_ptr<client::VpnClient>> clients;
    uint16_t port = 0;

    bool startServer(uint16_t p = 0) {
        if (!server.start(p)) return false;
        port = server.tcpPort();
        return true;
    }

    client::VpnClient& connect(const std::string& nick, const std::string& host = "localhost") {
        auto c = std::make_unique<client::VpnClient>();
        c->connectToServer(host, port, nick);
        clients.push_back(std::move(c));
        return *clients.back();
    }

    // Create a client without connecting (so test knobs can be set first).
    client::VpnClient& addClient() {
        auto c = std::make_unique<client::VpnClient>();
        clients.push_back(std::move(c));
        return *clients.back();
    }

    bool connectClient(size_t i, const std::string& nick, const std::string& host = "localhost") {
        return clients[i]->connectToServer(host, port, nick);
    }

    uint32_t vip(size_t i) const { return clients[i]->localVip(); }

    bool waitConnected(size_t i, int timeout_ms = 4000) {
        return waitUntil([&]() { return clients[i]->localVip() != 0; }, timeout_ms);
    }

    bool waitAllConnected(int timeout_ms = 4000) {
        return waitUntil([&]() {
            for (auto& c : clients) {
                if (c->localVip() == 0) return false;
            }
            return true;
        }, timeout_ms);
    }

    bool waitPeerCount(size_t expected, int timeout_ms = 6000) {
        return waitUntil([&]() {
            for (auto& c : clients) {
                if (c->getSnapshot().peers.size() != expected) return false;
            }
            return true;
        }, timeout_ms);
    }

    ~TestCluster() {
        // RAII guard: no app outlives its test, even when a CHECK returned early. Kill the
        // apps before stopping the clients whose gateways they are attached to.
        reapLaunchedApps();
        for (auto& c : clients) c->stop();
        server.stop();
    }
};

bool waitDirectPair(TestCluster& cluster, size_t a, size_t b, bool want_v6, int timeout_ms = 12000) {
    return waitUntil([&]() {
        auto sa = cluster.clients[a]->getSnapshot();
        auto sb = cluster.clients[b]->getSnapshot();
        auto pa = findPeer(sa, cluster.vip(b));
        auto pb = findPeer(sb, cluster.vip(a));
        if (!pa || !pb) return false;
        if (pa->route_mode != client::RouteMode::DirectP2P) return false;
        if (pb->route_mode != client::RouteMode::DirectP2P) return false;
        if (want_v6) return pa->ip_version == platform::IpVersion::V6 && pb->ip_version == platform::IpVersion::V6;
        return pa->ip_version == platform::IpVersion::V4 && pb->ip_version == platform::IpVersion::V4;
    }, timeout_ms);
}

bool waitDirectPairAny(TestCluster& cluster, size_t a, size_t b, int timeout_ms = 12000) {
    return waitUntil([&]() {
        auto sa = cluster.clients[a]->getSnapshot();
        auto sb = cluster.clients[b]->getSnapshot();
        auto pa = findPeer(sa, cluster.vip(b));
        auto pb = findPeer(sb, cluster.vip(a));
        return pa && pb && pa->route_mode == client::RouteMode::DirectP2P &&
               pb->route_mode == client::RouteMode::DirectP2P;
    }, timeout_ms);
}

// ---- CLI builders for the scriptable test app ------------------------------
std::string basicArgs(const std::string& role, const std::string& report) {
    return "--role " + role + " --report \"" + report + "\"";
}

std::string basicSpawnArgs(const std::string& child_exe, const std::string& child_role,
                           const std::string& report) {
    return "--spawn-child \"" + child_exe + "\" " + child_role + " --report \"" + report + "\"";
}

std::string tcpArgs(const std::string& role, const std::string& mode, uint16_t port,
                    int conns, int expect, int size_kb, int delay_ms,
                    const std::string& report, const std::string& dst = "",
                    const std::string& dsts = "") {
    std::string a = "--scenario tcp --role " + role + " --mode " + mode +
                    " --port " + std::to_string(port);
    if (conns > 0)   a += " --conns " + std::to_string(conns);
    if (expect > 0)  a += " --expect " + std::to_string(expect);
    if (size_kb > 0) a += " --size-kb " + std::to_string(size_kb);
    if (delay_ms > 0)a += " --delay-ms " + std::to_string(delay_ms);
    if (!dst.empty())  a += " --dst " + dst;
    if (!dsts.empty()) a += " --dsts " + dsts;
    a += " --report \"" + report + "\"";
    return a;
}

std::string udpRecvArgs(uint16_t port, const std::string& token, int timeout_ms,
                        int expect, const std::string& report) {
    return "--scenario udp-recv --port " + std::to_string(port) +
           " --token " + token + " --timeout-ms " + std::to_string(timeout_ms) +
           " --expect " + std::to_string(expect) +
           " --report \"" + report + "\"";
}

std::string udpSendArgs(uint16_t port, const std::string& dst, const std::string& token,
                        int count, const std::string& report) {
    return "--scenario udp-send --port " + std::to_string(port) + " --dst " + dst +
           " --token " + token + " --count " + std::to_string(count) +
           " --report \"" + report + "\"";
}

std::string udpLargeArgs(const std::string& role, uint16_t port, const std::string& dst,
                         int size_kb, const std::string& report) {
    std::string a = "--scenario udp-large --role " + role + " --port " + std::to_string(port) +
                    " --size-kb " + std::to_string(size_kb);
    if (!dst.empty()) a += " --dst " + dst;
    a += " --report \"" + report + "\"";
    return a;
}

std::string udpOverlappedArgs(const std::string& role, uint16_t port, const std::string& dst,
                              const std::string& report) {
    return "--scenario udp-overlapped --role " + role + " --port " + std::to_string(port) +
           " --dst " + dst + " --report \"" + report + "\"";
}

std::string udpOverlappedEdgeArgs(const std::string& role, uint16_t port,
                                  const std::string& dst, const std::string& report) {
    return "--scenario udp-overlapped-edge --role " + role + " --port " +
           std::to_string(port) + " --dst " + dst + " --report \"" + report + "\"";
}

std::string udpApiArgs(const std::string& role, uint16_t port, const std::string& dst,
                       const std::string& report) {
    return "--scenario udp-api --role " + role + " --port " + std::to_string(port) +
           " --dst " + dst + " --report \"" + report + "\"";
}

std::string udpEdgeArgs(const std::string& role, uint16_t port, const std::string& dst,
                        const std::string& report) {
    return "--scenario udp-edge --role " + role + " --port " + std::to_string(port) +
           " --dst " + dst + " --report \"" + report + "\"";
}

std::string udpWaitArgs(const std::string& role, uint16_t port, const std::string& dst,
                        const std::string& report) {
    return "--scenario udp-wait --role " + role + " --port " + std::to_string(port) +
           " --dst " + dst + " --report \"" + report + "\"";
}

std::string adapterDnsArgs(const std::string& report) {
    return "--scenario adapter-dns --report \"" + report + "\"";
}

std::string spawnWArgs(const std::string& child_exe, const std::string& child_report,
                       const std::string& report) {
    return "--scenario spawn-w --child-exe \"" + child_exe + "\" --child-report \"" +
           child_report + "\" --report \"" + report + "\"";
}

std::string spawnFailArgs(const std::string& child_exe, const std::string& child_report,
                          const std::string& report) {
    return "--scenario spawn-fail --child-exe \"" + child_exe + "\" --child-report \"" +
           child_report + "\" --report \"" + report + "\"";
}

// ============================================================================
// Step 0: hostname / IPv6 / IPv4 parsing & resolution
// ============================================================================
APIVPN_TEST(host_and_ip_resolution) {
    std::string host;
    uint16_t port = 34200;
    CHECK(platform::splitHostPort("[::1]:45000", host, port) && host == "::1" && port == 45000,
          "splitHostPort [::1]:45000");
    port = 34200;
    CHECK(platform::splitHostPort("::1", host, port) && host == "::1" && port == 34200,
          "splitHostPort ::1");
    port = 34200;
    CHECK(platform::splitHostPort("localhost:39999", host, port) && host == "localhost" && port == 39999,
          "splitHostPort localhost:39999");
    CHECK(platform::formatHostPort("::1", 34200) == "[::1]:34200", "formatHostPort ipv6");
    CHECK(platform::formatHostPort("localhost", 34200) == "localhost:34200", "formatHostPort host");

    platform::Endpoint ep6{};
    CHECK(platform::parseIpString("[::1]:34200", ep6) && ep6.isIpv6() && ep6.isLoopback() &&
              ep6.port_host == 34200, "parseIpString ipv6");
    platform::Endpoint ep4{};
    CHECK(platform::parseIpString("127.0.0.1:34200", ep4) && ep4.isIpv4() && ep4.isLoopback() &&
              ep4.port_host == 34200, "parseIpString ipv4");

    auto resolved = platform::resolveHost("localhost", 34200);
    CHECK(resolved.size() >= 2, "resolveHost(localhost) >= 2 endpoints");
    CHECK(resolved.front().isIpv6(), "resolveHost prefers IPv6 first");
    bool found_v4 = false;
    for (const auto& r : resolved) {
        if (r.isIpv4()) found_v4 = true;
    }
    CHECK(found_v4, "resolveHost includes IPv4 fallback");
    return true;
}

// ============================================================================
// Connection / lifecycle
// ============================================================================
APIVPN_TEST(client_server_connect_ipv4) {
    TestCluster cluster;
    CHECK(cluster.startServer(), "server start");
    client::VpnClient& c = cluster.connect("IPv4_Test", "127.0.0.1");
    CHECK(cluster.waitConnected(0), "client did not connect via 127.0.0.1");
    auto snap = c.getSnapshot();
    CHECK(snap.server_ip_version == platform::IpVersion::V4, "expected IPv4 server connection");
    CHECK(snap.local_vip == makeIpv4Host(10, 42, 0, 2), "expected VIP 10.42.0.2");
    return true;
}

APIVPN_TEST(client_server_connect_ipv6_preference) {
    TestCluster cluster;
    CHECK(cluster.startServer(), "server start");
    CHECK(cluster.server.hasIpv6(), "server did not bind IPv6 listener");
    client::VpnClient& a = cluster.connect("Alice_ClientA", "localhost");
    client::VpnClient& b = cluster.connect("Bob_ClientB", "[::1]");
    CHECK(cluster.waitAllConnected(), "clients did not connect over IPv6");
    auto sa = a.getSnapshot();
    auto sb = b.getSnapshot();
    CHECK(sa.server_ip_version == platform::IpVersion::V6, "localhost did not prefer IPv6");
    CHECK(sb.server_ip_version == platform::IpVersion::V6, "[::1] did not use IPv6");
    CHECK(sa.local_vip == makeIpv4Host(10, 42, 0, 2), "ClientA VIP 10.42.0.2");
    CHECK(sb.local_vip == makeIpv4Host(10, 42, 0, 3), "ClientB VIP 10.42.0.3");
    return true;
}

APIVPN_TEST(lobby_chat_two_peers) {
    TestCluster cluster;
    CHECK(cluster.startServer(), "server start");
    client::VpnClient& a = cluster.connect("Alice_Chat");
    client::VpnClient& b = cluster.connect("Bob_Chat");
    CHECK(cluster.waitAllConnected() && cluster.waitPeerCount(1), "peers not visible");
    a.sendChatMessage("Hello from Alice!");
    b.sendChatMessage("Ready to play!");
    CHECK(waitUntil([&]() {
        return a.getSnapshot().chat_messages.size() >= 2 && b.getSnapshot().chat_messages.size() >= 2;
    }, 4000), "chat messages did not propagate");
    return true;
}

APIVPN_TEST(unreachable_server_sets_reconnecting) {
    server::VpnServer srv;
    CHECK(srv.start(0), "server start");
    uint16_t port = srv.tcpPort();
    srv.stop(); // nothing listening on `port` now

    client::VpnClient c;
    bool ok = c.connectToServer("127.0.0.1", port, "Orphan");
    CHECK(!ok, "connectToServer unexpectedly succeeded to a dead server");
    CHECK(c.isReconnecting(), "client did not enter reconnecting state");
    c.disconnect();
    return true;
}

APIVPN_TEST(server_max_clients_capacity) {
    TestCluster cluster;
    cluster.server.setMaxClients(2);
    CHECK(cluster.startServer(), "server start");

    client::VpnClient& a = cluster.connect("Cap_A");
    client::VpnClient& b = cluster.connect("Cap_B");
    CHECK(cluster.waitAllConnected(), "first two clients did not connect");
    CHECK(cluster.server.connectedClientCount() == 2, "server should have 2 clients");

    // Third client is rejected while the server is at capacity.
    auto third = std::make_unique<client::VpnClient>();
    third->setReconnectIntervalUs(300'000ULL);
    bool ok = third->connectToServer("127.0.0.1", cluster.port, "Cap_C");
    CHECK(!ok, "third client should have been rejected at capacity");
    CHECK(third->localVip() == 0, "rejected client must not get a VIP");

    // Free a slot and the rejected client should eventually join.
    b.disconnect();
    CHECK(waitUntil([&]() { return cluster.server.connectedClientCount() <= 1; }, 3000),
          "server did not release the disconnected client");
    CHECK(waitUntil([&]() { return third->localVip() != 0; }, 6000),
          "rejected client did not join after a slot freed up");
    third->stop();
    (void)a;
    return true;
}

APIVPN_TEST(malformed_frame_does_not_kill_server) {
    TestCluster cluster;
    CHECK(cluster.startServer(), "server start");

    // Raw TCP peer sends garbage (bad magic) - the server must drop only that session.
    {
        platform::NetSocket raw = platform::NetSocket::createTcp(platform::IpVersion::V4);
        CHECK(raw.isValid(), "raw socket create");
        CHECK(raw.connect(platform::Endpoint::loopbackV4(cluster.port), 2000), "raw connect");
        const char junk[16] = {0x11, 0x22, 0x33, 0x44, 0x55, 0x66, 0x77, 0x00};
        raw.sendAll(junk, sizeof(junk));
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
    }
    // A second raw peer sends a valid magic but an absurd payload length.
    {
        platform::NetSocket raw = platform::NetSocket::createTcp(platform::IpVersion::V4);
        CHECK(raw.isValid(), "raw socket create 2");
        CHECK(raw.connect(platform::Endpoint::loopbackV4(cluster.port), 2000), "raw connect 2");
        struct { uint32_t magic; uint8_t type; uint32_t payload_len; } hdr{};
        hdr.magic = kProtocolMagic;
        hdr.type = static_cast<uint8_t>(ControlMsgType::ChatMessage);
        hdr.payload_len = 0xFFFFFFF0u; // way above the allowed maximum
        raw.sendAll(&hdr, sizeof(hdr));
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
    }

    // The server must still accept and serve a legitimate client.
    client::VpnClient& a = cluster.connect("Survivor");
    CHECK(cluster.waitConnected(0), "server did not survive malformed frames");
    CHECK(a.localVip() == makeIpv4Host(10, 42, 0, 2), "survivor VIP should be 10.42.0.2");
    return true;
}

APIVPN_TEST(server_restart_reconnect) {
    TestCluster cluster;
    CHECK(cluster.startServer(), "server start");
    client::VpnClient& a = cluster.connect("Restart_A");
    client::VpnClient& b = cluster.connect("Restart_B");
    CHECK(cluster.waitAllConnected() && cluster.waitPeerCount(1), "initial peers not visible");
    CHECK(waitDirectPairAny(cluster, 0, 1),
          "no direct P2P before restart");

    uint16_t port = cluster.port;
    a.setReconnectIntervalUs(300'000ULL);
    b.setReconnectIntervalUs(300'000ULL);

    cluster.server.stop();
    CHECK(waitUntil([&]() {
        return !a.getSnapshot().connected && a.getSnapshot().reconnecting &&
               !b.getSnapshot().connected && b.getSnapshot().reconnecting;
    }, 4000), "clients did not enter reconnecting state");

    CHECK(cluster.server.start(port), "server restart");
    CHECK(waitUntil([&]() {
        auto sa = a.getSnapshot();
        auto sb = b.getSnapshot();
        return sa.connected && sb.connected &&
               sa.local_vip == makeIpv4Host(10, 42, 0, 2) &&
               sb.local_vip == makeIpv4Host(10, 42, 0, 3);
    }, 6000), "clients did not reconnect with VIPs preserved after restart");
    return true;
}

APIVPN_TEST(automatic_direct_to_relay_fallback) {
    TestCluster cluster;
    CHECK(cluster.startServer(), "server start");
    client::VpnClient& a = cluster.connect("AR_A");
    client::VpnClient& b = cluster.connect("AR_B");
    CHECK(cluster.waitAllConnected() && cluster.waitPeerCount(1), "peers not visible");
    CHECK(waitDirectPairAny(cluster, 0, 1), "no direct P2P before fallback");

    // Stop direct keep-alives and drop incoming punch/keepalive packets, then shrink the
    // direct-path timeout. The relay path stays healthy, so the route must downgrade.
    for (client::VpnClient* c : {&a, &b}) {
        c->setSimulateHolePunchBlocked(true);
        c->setTunnelKeepAliveIntervalUs(60'000'000ULL);
        c->setDirectKeepAliveTimeoutUs(1'000'000ULL);
    }
    CHECK(waitUntil([&]() {
        auto sa = a.getSnapshot();
        auto sb = b.getSnapshot();
        auto pa = findPeer(sa, cluster.vip(1));
        auto pb = findPeer(sb, cluster.vip(0));
        return pa && pb && pa->route_mode == client::RouteMode::ServerRelay &&
               pb->route_mode == client::RouteMode::ServerRelay;
    }, 6000), "direct route did not fall back to the relay");

    AppHandle host, join;
    CHECK(launchAppPid(b, appPath("64"),
                       udpRecvArgs(6112, "FALLBACK", 6000, 1, reportPath("ar_host")), host),
          "launch fallback host");
    CHECK(waitReady(host), notReady("fallback host", host));
    CHECK(launchAppPid(a, appPath("64"),
                       udpSendArgs(6112, ipv4ToString(cluster.vip(1)), "FALLBACK", 5,
                                   reportPath("ar_join")), join),
          "launch fallback sender");
    CHECK(waitApps({&host, &join}, 12000), "fallback traffic apps timed out");
    CHECK(parseUintAfter(host.report, "recv=") >= 1,
          "relay traffic failed after automatic fallback: " + host.report);
    CHECK(join.report.rfind("PASS", 0) == 0, "fallback sender report: " + join.report);
    return true;
}

APIVPN_TEST(server_outage_keeps_direct_traffic) {
    TestCluster cluster;
    CHECK(cluster.startServer(), "server start");
    client::VpnClient& a = cluster.connect("OUT_A");
    client::VpnClient& b = cluster.connect("OUT_B");
    CHECK(cluster.waitAllConnected() && cluster.waitPeerCount(1), "peers not visible");
    CHECK(waitDirectPairAny(cluster, 0, 1), "no direct P2P before outage");

    AppHandle host;
    CHECK(launchAppPid(b, appPath("64"),
                       udpRecvArgs(6112, "OUTAGE", 8000, 1, reportPath("out_host")), host),
          "launch outage host");
    CHECK(waitReady(host), notReady("outage host", host));

    cluster.server.stop();
    CHECK(waitUntil([&]() {
        auto sa = a.getSnapshot();
        auto sb = b.getSnapshot();
        return !sa.connected && sa.reconnecting && !sb.connected && sb.reconnecting;
    }, 4000), "clients did not enter reconnecting state");
    CHECK(a.localVip() != 0 && b.localVip() != 0, "VIPs were lost during the server outage");
    CHECK(waitUntil([&]() {
        auto sa = a.getSnapshot();
        auto sb = b.getSnapshot();
        auto pa = findPeer(sa, cluster.vip(1));
        auto pb = findPeer(sb, cluster.vip(0));
        return pa && pb && pa->route_mode == client::RouteMode::DirectP2P &&
               pb->route_mode == client::RouteMode::DirectP2P;
    }, 2000), "direct route was dropped during the server outage");

    AppHandle join;
    CHECK(launchAppPid(a, appPath("64"),
                       udpSendArgs(6112, ipv4ToString(cluster.vip(1)), "OUTAGE", 5,
                                   reportPath("out_join")), join),
          "launch outage sender");
    CHECK(waitApps({&host, &join}, 12000), "outage traffic apps timed out");
    CHECK(parseUintAfter(host.report, "recv=") >= 1,
          "direct traffic failed during server outage: " + host.report);
    CHECK(join.report.rfind("PASS", 0) == 0, "outage sender report: " + join.report);
    return true;
}

APIVPN_TEST(server_peer_inactivity_timeout) {
    TestCluster cluster;
    cluster.server.setPeerTimeoutUs(1'200'000ULL);
    CHECK(cluster.startServer(), "server start");
    client::VpnClient& a = cluster.connect("Timeout_A");
    client::VpnClient& b = cluster.connect("Timeout_B");
    CHECK(cluster.waitAllConnected() && cluster.waitPeerCount(1), "peers not visible");

    a.setServerTimeoutUs(1'200'000ULL);
    b.setSimulateSilentDrop(true);

    CHECK(waitUntil([&]() {
        auto sa = a.getSnapshot();
        return cluster.server.connectedClientCount() == 1 && sa.connected && sa.peers.empty();
    }, 5000), "server did not time out the silent client");
    b.setSimulateSilentDrop(false);
    return true;
}

// ============================================================================
// Multi-peer (3 clients)
// ============================================================================
APIVPN_TEST(three_peer_topology) {
    TestCluster cluster;
    CHECK(cluster.startServer(), "server start");
    client::VpnClient& a = cluster.connect("Topo_A");
    client::VpnClient& b = cluster.connect("Topo_B");
    client::VpnClient& c = cluster.connect("Topo_C");
    CHECK(cluster.waitAllConnected(), "clients not connected");
    CHECK(cluster.vip(0) == makeIpv4Host(10, 42, 0, 2) &&
          cluster.vip(1) == makeIpv4Host(10, 42, 0, 3) &&
          cluster.vip(2) == makeIpv4Host(10, 42, 0, 4), "unexpected VIP allocation");
    CHECK(cluster.waitPeerCount(2), "each client should see exactly 2 peers");
    auto sa = a.getSnapshot();
    CHECK(findPeer(sa, cluster.vip(1)) != nullptr && findPeer(sa, cluster.vip(2)) != nullptr,
          "client A missing peers B/C");
    (void)b;
    (void)c;
    return true;
}

APIVPN_TEST(lobby_chat_three_peers) {
    TestCluster cluster;
    CHECK(cluster.startServer(), "server start");
    client::VpnClient& a = cluster.connect("Chat3_A");
    client::VpnClient& b = cluster.connect("Chat3_B");
    client::VpnClient& c = cluster.connect("Chat3_C");
    CHECK(cluster.waitAllConnected() && cluster.waitPeerCount(2), "peers not visible");

    a.sendChatMessage("A here");
    b.sendChatMessage("B here");
    c.sendChatMessage("C here");
    CHECK(waitUntil([&]() {
        return a.getSnapshot().chat_messages.size() >= 3 &&
               b.getSnapshot().chat_messages.size() >= 3 &&
               c.getSnapshot().chat_messages.size() >= 3;
    }, 5000), "three-peer chat did not fully propagate");
    return true;
}

APIVPN_TEST(udp_broadcast_fanout_three_peers) {
    TestCluster cluster;
    CHECK(cluster.startServer(), "server start");
    client::VpnClient& a = cluster.connect("Fan_A");
    client::VpnClient& b = cluster.connect("Fan_B");
    client::VpnClient& c = cluster.connect("Fan_C");
    CHECK(cluster.waitAllConnected() && cluster.waitPeerCount(2), "peers not visible");

    AppHandle rb, rc, sa;
    CHECK(launchAppPid(b, appPath("64"), udpRecvArgs(6112, "BCAST", 6000, 1, reportPath("fan_b")), rb),
          "launch B receiver");
    CHECK(launchAppPid(c, appPath("64"), udpRecvArgs(6112, "BCAST", 6000, 1, reportPath("fan_c")), rc),
          "launch C receiver");
    CHECK(waitReady(rb), notReady("B receiver", rb));
    CHECK(waitReady(rc), notReady("C receiver", rc));
    CHECK(launchAppPid(a, appPath("64"),
                       udpSendArgs(6112, "10.42.0.255", "BCAST", 5, reportPath("fan_a")), sa),
          "launch A sender");

    CHECK(waitApps({&sa, &rb, &rc}, 9000), "fan-out apps timed out");
    CHECK(sa.report.rfind("PASS", 0) == 0, "A send failed: " + sa.report);
    int got_b = parseUintAfter(rb.report, "recv=");
    int got_c = parseUintAfter(rc.report, "recv=");
    CHECK(got_b >= 1, "B did not receive broadcast (got " + std::to_string(got_b) + ")");
    CHECK(got_c >= 1, "C did not receive broadcast (got " + std::to_string(got_c) + ")");
    return true;
}

APIVPN_TEST(udp_unicast_isolation_three_peers) {
    TestCluster cluster;
    CHECK(cluster.startServer(), "server start");
    client::VpnClient& a = cluster.connect("Iso_A");
    client::VpnClient& b = cluster.connect("Iso_B");
    client::VpnClient& c = cluster.connect("Iso_C");
    CHECK(cluster.waitAllConnected() && cluster.waitPeerCount(2), "peers not visible");

    std::string vip_b = ipv4ToString(cluster.vip(1));
    AppHandle rb, rc, sa;
    CHECK(launchAppPid(b, appPath("64"), udpRecvArgs(6112, "UNI", 6000, 1, reportPath("iso_b")), rb),
          "launch B receiver");
    // C must NOT receive the unicast: wait the full (short) window expecting zero.
    CHECK(launchAppPid(c, appPath("64"), udpRecvArgs(6112, "UNI", 3000, 0, reportPath("iso_c")), rc),
          "launch C receiver");
    CHECK(waitReady(rb), notReady("B receiver", rb));
    CHECK(waitReady(rc), notReady("C receiver", rc));
    CHECK(launchAppPid(a, appPath("64"),
                       udpSendArgs(6112, vip_b, "UNI", 5, reportPath("iso_a")), sa),
          "launch A unicast sender");

    CHECK(waitApps({&sa, &rb, &rc}, 9000), "isolation apps timed out");
    int got_b = parseUintAfter(rb.report, "recv=");
    int got_c = parseUintAfter(rc.report, "recv=");
    CHECK(got_b >= 1, "unicast target B did not receive (got " + std::to_string(got_b) + ")");
    CHECK(got_c == 0, "unicast leaked to non-target C (got " + std::to_string(got_c) + ")");
    return true;
}

APIVPN_TEST(tcp_mesh_three_peers) {
    TestCluster cluster;
    CHECK(cluster.startServer(), "server start");
    client::VpnClient& a = cluster.connect("Mesh_A");
    client::VpnClient& b = cluster.connect("Mesh_B");
    client::VpnClient& c = cluster.connect("Mesh_C");
    CHECK(cluster.waitAllConnected() && cluster.waitPeerCount(2), "peers not visible");

    std::string vip_b = ipv4ToString(cluster.vip(1));
    std::string vip_c = ipv4ToString(cluster.vip(2));

    AppHandle hb, hc, ja;
    CHECK(launchAppPid(b, appPath("64"),
                       tcpArgs("host", "B", 6112, 1, 1, 128, 0, reportPath("mesh_hb")), hb),
          "launch B host");
    CHECK(launchAppPid(c, appPath("64"),
                       tcpArgs("host", "B", 6112, 1, 1, 128, 0, reportPath("mesh_hc")), hc),
          "launch C host");
    CHECK(waitReady(hb), notReady("B host", hb));
    CHECK(waitReady(hc), notReady("C host", hc));
    CHECK(launchAppPid(a, appPath("64"),
                       tcpArgs("joiner", "B", 6112, 1, 0, 128, 0, reportPath("mesh_ja"),
                               "", vip_b + "," + vip_c), ja),
          "launch A mesh joiner");

    CHECK(waitApps({&hb, &hc, &ja}, 12000), "mesh apps timed out");
    CHECK(hb.report.rfind("PASS", 0) == 0, "B host failed: " + hb.report);
    CHECK(hc.report.rfind("PASS", 0) == 0, "C host failed: " + hc.report);
    CHECK(ja.report.rfind("PASS", 0) == 0, "A mesh joiner failed: " + ja.report);
    return true;
}

APIVPN_TEST(mixed_route_modes_three_peers) {
    TestCluster cluster;
    CHECK(cluster.startServer(), "server start");
    client::VpnClient& a = cluster.connect("Mix_A");
    client::VpnClient& b = cluster.connect("Mix_B");
    client::VpnClient& c = cluster.connect("Mix_C");
    CHECK(cluster.waitAllConnected() && cluster.waitPeerCount(2), "peers not visible");

    uint32_t vip_b = cluster.vip(1);
    uint32_t vip_c = cluster.vip(2);

    // Pin C onto the relay path while B keeps direct P2P.
    a.setForceRelayForPeer(vip_c, true);
    CHECK(waitUntil([&]() {
        auto sa = a.getSnapshot();
        auto pb = findPeer(sa, vip_b);
        auto pc = findPeer(sa, vip_c);
        return pb && pc && pb->route_mode == client::RouteMode::DirectP2P &&
               pc->route_mode == client::RouteMode::ServerRelay;
    }, 12000), "mixed route modes not reached (B direct + C relay)");

    std::string vip_b_str = ipv4ToString(vip_b);
    std::string vip_c_str = ipv4ToString(vip_c);

    AppHandle rb, rc, s_b, s_c;
    CHECK(launchAppPid(b, appPath("64"), udpRecvArgs(6112, "MIX", 8000, 1, reportPath("mix_b")), rb),
          "launch B receiver");
    CHECK(launchAppPid(c, appPath("64"), udpRecvArgs(6112, "MIX", 8000, 1, reportPath("mix_c")), rc),
          "launch C receiver");
    CHECK(waitReady(rb), notReady("B receiver", rb));
    CHECK(waitReady(rc), notReady("C receiver", rc));
    CHECK(launchAppPid(a, appPath("64"), udpSendArgs(6112, vip_b_str, "MIX", 4, reportPath("mix_ab")), s_b),
          "launch A->B sender");
    CHECK(waitApp(s_b, 6000), "A->B sender timed out");
    CHECK(launchAppPid(a, appPath("64"), udpSendArgs(6112, vip_c_str, "MIX", 4, reportPath("mix_ac")), s_c),
          "launch A->C sender");
    CHECK(waitApps({&s_c, &rb, &rc}, 10000), "mixed-route apps timed out");

    CHECK(parseUintAfter(rb.report, "recv=") >= 1, "direct peer B did not receive");
    CHECK(parseUintAfter(rc.report, "recv=") >= 1, "relay peer C did not receive");
    return true;
}

// ============================================================================
// Hole punching / routing lifecycle
// ============================================================================
APIVPN_TEST(holepunch_prefers_ipv6_direct) {
    TestCluster cluster;
    CHECK(cluster.startServer(), "server start");
    client::VpnClient& a = cluster.connect("HP_A");
    client::VpnClient& b = cluster.connect("HP_B");
    CHECK(cluster.waitAllConnected() && cluster.waitPeerCount(1), "peers not visible");
    CHECK(waitDirectPair(cluster, 0, 1, true, 12000), "did not establish direct IPv6 P2P");
    (void)a;
    (void)b;
    return true;
}

APIVPN_TEST(holepunch_ipv4_fallback_and_upgrade) {
    TestCluster cluster;
    CHECK(cluster.startServer(), "server start");
    client::VpnClient& a = cluster.connect("FB_A");
    client::VpnClient& b = cluster.connect("FB_B");
    a.setDisableIpv6P2P(true);
    b.setDisableIpv6P2P(true);
    CHECK(cluster.waitAllConnected() && cluster.waitPeerCount(1), "peers not visible");
    CHECK(waitDirectPair(cluster, 0, 1, false, 12000), "did not fall back to direct IPv4 P2P");

    a.setDisableIpv6P2P(false);
    b.setDisableIpv6P2P(false);
    CHECK(waitDirectPair(cluster, 0, 1, true, 12000), "did not upgrade to direct IPv6 P2P");
    return true;
}

APIVPN_TEST(tunnel_keepalive_interval) {
    TestCluster cluster;
    CHECK(cluster.startServer(), "server start");

    // Inject the interval before connecting so every keep-alive in this test runs on the short
    // cadence; with the 5 s default one could fire at any point during the P2P wait below.
    constexpr uint64_t kIntervalMs = 500;
    client::VpnClient& a = cluster.addClient();
    client::VpnClient& b = cluster.addClient();
    a.setTunnelKeepAliveIntervalUs(kIntervalMs * 1000ULL);
    b.setTunnelKeepAliveIntervalUs(kIntervalMs * 1000ULL);
    CHECK(cluster.connectClient(0, "KA_A") && cluster.connectClient(1, "KA_B"), "clients connect");
    CHECK(cluster.waitAllConnected() && cluster.waitPeerCount(1), "peers not visible");
    // Wait for the settled (IPv6) direct path: while an IPv4 path still awaits its IPv6
    // upgrade the client sends punch probes instead of keep-alives.
    CHECK(waitDirectPair(cluster, 0, 1, true), "no settled direct IPv6 P2P");

    struct KaCounts {
        bool     ok   = false;
        uint64_t tx_a = 0, rx_a = 0, tx_b = 0, rx_b = 0;
    };
    auto sample = [&]() {
        KaCounts k;
        auto snap_a = a.getSnapshot();
        auto snap_b = b.getSnapshot();
        auto pa = findPeer(snap_a, cluster.vip(1));
        auto pb = findPeer(snap_b, cluster.vip(0));
        if (pa && pb) {
            k.ok = true;
            k.tx_a = pa->keepalive_tx;
            k.rx_a = pa->keepalive_rx;
            k.tx_b = pb->keepalive_tx;
            k.rx_b = pb->keepalive_rx;
        }
        return k;
    };

    // Count keep-alives over a measured window relative to a baseline: they must flow, but
    // never more often than the injected interval allows.
    auto t0 = std::chrono::steady_clock::now();
    KaCounts before = sample();
    CHECK(before.ok, "peers vanished");
    std::this_thread::sleep_for(std::chrono::milliseconds(3000));
    KaCounts after = sample();
    uint64_t window_ms = static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - t0).count());
    CHECK(after.ok, "peers vanished");

    uint64_t sent_a = after.tx_a - before.tx_a;
    uint64_t sent_b = after.tx_b - before.tx_b;
    uint64_t max_sent = window_ms / kIntervalMs + 1;
    uint64_t min_sent = window_ms / kIntervalMs / 2;
    std::string counts = "A=" + std::to_string(sent_a) + " B=" + std::to_string(sent_b) +
                         " in " + std::to_string(window_ms) + " ms, interval " +
                         std::to_string(kIntervalMs) + " ms";
    CHECK(sent_a <= max_sent && sent_b <= max_sent,
          "keep-alives sent faster than the injected interval (" + counts + ")");
    CHECK(sent_a >= min_sent && sent_b >= min_sent,
          "keep-alives not sent at the injected interval (" + counts + ")");
    CHECK(after.rx_a > before.rx_a && after.rx_b > before.rx_b,
          "keep-alives were not received by the peer");
    return true;
}

APIVPN_TEST(holepunch_timeout_then_retry_recovers) {
    TestCluster cluster;
    CHECK(cluster.startServer(), "server start");

    // Create clients and configure the injected timers / blocking *before* connecting
    // so hole punching is already blocked when the peers first discover each other.
    client::VpnClient& a = cluster.addClient();
    client::VpnClient& b = cluster.addClient();
    a.setHolePunchTimeoutUs(1'000'000ULL);
    b.setHolePunchTimeoutUs(1'000'000ULL);
    a.setHolePunchRetryIntervalUs(10'000'000ULL);
    b.setHolePunchRetryIntervalUs(10'000'000ULL);
    a.setSimulateHolePunchBlocked(true);
    b.setSimulateHolePunchBlocked(true);

    CHECK(cluster.connectClient(0, "TO_A") && cluster.connectClient(1, "TO_B"), "clients connect");
    CHECK(cluster.waitAllConnected() && cluster.waitPeerCount(1), "peers not visible");

    // Punching is actively trying shortly after join.
    std::this_thread::sleep_for(std::chrono::milliseconds(300));
    {
        auto sa = a.getSnapshot();
        auto pa = findPeer(sa, cluster.vip(1));
        CHECK(pa && !pa->hole_punch_gave_up && pa->punch_probes_sent > 0,
              "expected active probing before the timeout");
    }

    // After the injected 1s window it must give up and settle on relay.
    CHECK(waitUntil([&]() {
        auto sa = a.getSnapshot();
        auto sb = b.getSnapshot();
        auto pa = findPeer(sa, cluster.vip(1));
        auto pb = findPeer(sb, cluster.vip(0));
        return pa && pb && pa->hole_punch_gave_up && pb->hole_punch_gave_up &&
               pa->route_mode == client::RouteMode::ServerRelay &&
               pb->route_mode == client::RouteMode::ServerRelay;
    }, 5000), "hole punching did not give up after the injected timeout");

    // Unblock and shorten the retry interval: the periodic retry must re-establish direct P2P.
    a.setHolePunchRetryIntervalUs(300'000ULL);
    a.setSimulateHolePunchBlocked(false);
    b.setSimulateHolePunchBlocked(false);
    CHECK(waitUntil([&]() {
        auto sa = a.getSnapshot();
        auto sb = b.getSnapshot();
        auto pa = findPeer(sa, cluster.vip(1));
        auto pb = findPeer(sb, cluster.vip(0));
        return pa && pb && pa->route_mode == client::RouteMode::DirectP2P &&
               pb->route_mode == client::RouteMode::DirectP2P &&
               pa->hole_punch_cycles >= 2 && pb->hole_punch_cycles >= 2;
    }, 8000), "periodic hole-punch retry did not recover direct P2P");
    return true;
}

APIVPN_TEST(spoofed_punch_does_not_hijack_route) {
    TestCluster cluster;
    CHECK(cluster.startServer(), "server start");
    client::VpnClient& a = cluster.connect("Spoof_A");
    client::VpnClient& b = cluster.connect("Spoof_B");
    CHECK(cluster.waitAllConnected() && cluster.waitPeerCount(1), "peers not visible");
    CHECK(waitDirectPair(cluster, 0, 1, true), "no settled direct IPv6 P2P");
    const uint32_t vip_a = cluster.vip(0);
    const uint32_t vip_b = cluster.vip(1);

    auto directEndpoint = [](client::VpnClient& c, uint32_t peer_vip, platform::Endpoint& out) {
        auto snap = c.getSnapshot();
        auto p = findPeer(snap, peer_vip);
        if (!p || p->route_mode != client::RouteMode::DirectP2P) return false;
        out = p->active_endpoint;
        return true;
    };

    // Baseline: A's route to B must be stable for three samples (1 s), so a legitimate
    // endpoint change cannot be mistaken for a hijack later.
    platform::Endpoint real_b{};
    int stable_samples = 0;
    CHECK(waitUntil([&]() {
        platform::Endpoint cur{};
        if (!directEndpoint(a, vip_b, cur)) {
            stable_samples = 0;
            return false;
        }
        stable_samples = (cur == real_b) ? stable_samples + 1 : 1;
        real_b = cur;
        return stable_samples >= 3;
    }, 8000, 500), "A's direct route to B never settled");

    // Everything B knows about A's UDP transport: the attacker floods all of it.
    std::vector<platform::Endpoint> a_eps;
    {
        auto snap_b = b.getSnapshot();
        auto pb = findPeer(snap_b, vip_a);
        CHECK(pb != nullptr, "B lost peer A");
        auto addEndpoint = [&a_eps](const platform::Endpoint& ep) {
            if (!ep.isValidAddress() || ep.port_host == 0) return;
            for (const auto& known : a_eps) {
                if (known == ep) return;
            }
            a_eps.push_back(ep);
        };
        addEndpoint(pb->active_endpoint);
        addEndpoint(pb->best_v4_endpoint);
        addEndpoint(pb->best_v6_endpoint);
        for (const auto& cand : pb->candidate_endpoints) addEndpoint(cand);
    }
    CHECK(!a_eps.empty(), "no UDP endpoint of A known to B");

    // Off-path attacker: raw UDP sockets on ports B never used, forging B's VIP with a
    // missing (0) or wrong pair_key.
    platform::NetSocket spoof4 = platform::NetSocket::createUdp(platform::IpVersion::V4);
    platform::NetSocket spoof6 = platform::NetSocket::createUdp(platform::IpVersion::V6);
    CHECK(spoof4.isValid() && spoof4.bindAny(0), "spoof IPv4 socket");
    const bool have_v6 = spoof6.isValid() && spoof6.bindAny(0);
    std::vector<uint16_t> attacker_ports;
    platform::Endpoint local_ep{};
    if (spoof4.getLocalEndpoint(local_ep)) attacker_ports.push_back(local_ep.port_host);
    if (have_v6 && spoof6.getLocalEndpoint(local_ep)) attacker_ports.push_back(local_ep.port_host);

    const uint64_t wrong_keys[] = {0ULL, platform::secureRandom64()};
    const UdpPacketType forged_types[] = {UdpPacketType::KeepAlive, UdpPacketType::PunchPong,
                                          UdpPacketType::PunchPing};
    std::string hijack;
    auto checkRoute = [&]() {
        platform::Endpoint cur{};
        if (!directEndpoint(a, vip_b, cur)) {
            if (hijack.empty()) hijack = "A dropped its direct route to B";
            return;
        }
        if (cur != real_b && hijack.empty()) {
            hijack = "A's endpoint for B moved from " + real_b.toString() + " to " + cur.toString();
            for (uint16_t port : attacker_ports) {
                if (cur.port_host == port) hijack += " (the spoofer)";
            }
        }
    };
    for (int round = 0; round < 10; ++round) {
        for (UdpPacketType type : forged_types) {
            for (uint64_t key : wrong_keys) {
                UdpPunchPacket pkt{};
                pkt.hdr.magic = kProtocolMagic;
                pkt.hdr.type = static_cast<uint8_t>(type);
                pkt.src_vip = vip_b;
                pkt.dst_vip = vip_a;
                pkt.timestamp_us = platform::monotonicMicros();
                pkt.pair_key = key;
                for (const auto& ep : a_eps) {
                    if (ep.isIpv6() && !have_v6) continue;
                    (ep.isIpv6() ? spoof6 : spoof4).sendTo(&pkt, sizeof(pkt), ep);
                }
            }
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
        checkRoute();
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(500));
    checkRoute();
    CHECK(hijack.empty(), "spoofed punch packets changed the route: " + hijack);

    // Real traffic must still flow both ways over the untouched route. One direction at a time:
    // the sender binds virtual port 6112 too, so it must not share a client with the receiver.
    AppHandle rb, send_ab;
    CHECK(launchAppPid(b, appPath("64"), udpRecvArgs(6112, "SPOOF", 8000, 1, reportPath("spoof_rb")), rb),
          "launch B receiver");
    CHECK(waitReady(rb), notReady("B receiver", rb));
    CHECK(launchAppPid(a, appPath("64"),
                       udpSendArgs(6112, ipv4ToString(vip_b), "SPOOF", 5, reportPath("spoof_ab")), send_ab),
          "launch A->B sender");
    CHECK(waitApps({&send_ab, &rb}, 10000), "A->B traffic apps timed out");
    CHECK(parseUintAfter(rb.report, "recv=") >= 1, "A->B traffic broken after spoofing: " + rb.report);

    AppHandle ra, send_ba;
    CHECK(launchAppPid(a, appPath("64"), udpRecvArgs(6112, "SPOOF", 8000, 1, reportPath("spoof_ra")), ra),
          "launch A receiver");
    CHECK(waitReady(ra), notReady("A receiver", ra));
    CHECK(launchAppPid(b, appPath("64"),
                       udpSendArgs(6112, ipv4ToString(vip_a), "SPOOF", 5, reportPath("spoof_ba")), send_ba),
          "launch B->A sender");
    CHECK(waitApps({&send_ba, &ra}, 10000), "B->A traffic apps timed out");
    CHECK(parseUintAfter(ra.report, "recv=") >= 1, "B->A traffic broken after spoofing: " + ra.report);

    checkRoute();
    CHECK(hijack.empty(), "route changed after post-spoof traffic: " + hijack);
    return true;
}

// ============================================================================
// TCP tunneling
// ============================================================================
APIVPN_TEST(tcp_basic_p2p_udp_and_stream) {
    TestCluster cluster;
    CHECK(cluster.startServer(), "server start");
    client::VpnClient& a = cluster.connect("R1_A");
    client::VpnClient& b = cluster.connect("R1_B");
    CHECK(cluster.waitAllConnected() && cluster.waitPeerCount(1), "peers not visible");
    CHECK(waitDirectPairAny(cluster, 0, 1),
          "no direct P2P");

    AppHandle host, join;
    CHECK(launchAppPid(a, appPath("64"), basicArgs("host", reportPath("r1_host")), host),
          "launch host");
    CHECK(waitReady(host), notReady("host", host));
    CHECK(launchAppPid(b, appPath("32"), basicArgs("joiner", reportPath("r1_join")), join),
          "launch joiner");
    CHECK(waitApps({&host, &join}, 20000), "basic p2p apps timed out");
    CHECK(host.report.rfind("PASS", 0) == 0, "host report: " + host.report);
    CHECK(join.report.rfind("PASS", 0) == 0, "joiner report: " + join.report);
    return true;
}

APIVPN_TEST(tcp_basic_relay_fallback) {
    TestCluster cluster;
    CHECK(cluster.startServer(), "server start");
    client::VpnClient& a = cluster.connect("R2_A");
    client::VpnClient& b = cluster.connect("R2_B");
    CHECK(cluster.waitAllConnected() && cluster.waitPeerCount(1), "peers not visible");
    a.setForceRelay(true);
    b.setForceRelay(true);

    AppHandle host, join;
    CHECK(launchAppPid(a, appPath("32"), basicArgs("host", reportPath("r2_host")), host),
          "launch host");
    CHECK(waitReady(host), notReady("host", host));
    CHECK(launchAppPid(b, appPath("64"), basicArgs("joiner", reportPath("r2_join")), join),
          "launch joiner");
    CHECK(waitApps({&host, &join}, 20000), "relay apps timed out");
    CHECK(host.report.rfind("PASS", 0) == 0, "host report: " + host.report);
    CHECK(join.report.rfind("PASS", 0) == 0, "joiner report: " + join.report);
    return true;
}

APIVPN_TEST(tcp_concurrent_connections) {
    TestCluster cluster;
    CHECK(cluster.startServer(), "server start");
    client::VpnClient& a = cluster.connect("CC_A");
    client::VpnClient& b = cluster.connect("CC_B");
    CHECK(cluster.waitAllConnected() && cluster.waitPeerCount(1), "peers not visible");

    const int conns = 4;
    AppHandle host, join;
    CHECK(launchAppPid(b, appPath("64"),
                       tcpArgs("host", "C", 6112, conns, conns, kSmallSizeKb, 0,
                               reportPath("cc_host")), host),
          "launch host");
    CHECK(waitReady(host), notReady("host", host));
    CHECK(launchAppPid(a, appPath("64"),
                       tcpArgs("joiner", "C", 6112, conns, 0, kSmallSizeKb, 0,
                               reportPath("cc_join"), ipv4ToString(cluster.vip(1))), join),
          "launch joiner");
    CHECK(waitApps({&host, &join}, 25000), "concurrent apps timed out");
    CHECK(host.report.rfind("PASS", 0) == 0, "host report: " + host.report);
    CHECK(join.report.rfind("PASS", 0) == 0, "joiner report: " + join.report);
    return true;
}

APIVPN_TEST(tcp_full_duplex) {
    TestCluster cluster;
    CHECK(cluster.startServer(), "server start");
    client::VpnClient& a = cluster.connect("DX_A");
    client::VpnClient& b = cluster.connect("DX_B");
    CHECK(cluster.waitAllConnected() && cluster.waitPeerCount(1), "peers not visible");

    AppHandle host, join;
    CHECK(launchAppPid(b, appPath("64"),
                       tcpArgs("host", "D", 6112, 1, 1, kSmallSizeKb, 0, reportPath("dx_host")), host),
          "launch host");
    CHECK(waitReady(host), notReady("host", host));
    CHECK(launchAppPid(a, appPath("64"),
                       tcpArgs("joiner", "D", 6112, 1, 0, kSmallSizeKb, 0, reportPath("dx_join"),
                               ipv4ToString(cluster.vip(1))), join),
          "launch joiner");
    CHECK(waitApps({&host, &join}, 25000), "duplex apps timed out");
    CHECK(host.report.rfind("PASS", 0) == 0, "host report: " + host.report);
    CHECK(join.report.rfind("PASS", 0) == 0, "joiner report: " + join.report);
    return true;
}

APIVPN_TEST(tcp_half_close) {
    TestCluster cluster;
    CHECK(cluster.startServer(), "server start");
    client::VpnClient& a = cluster.connect("HC_A");
    client::VpnClient& b = cluster.connect("HC_B");
    CHECK(cluster.waitAllConnected() && cluster.waitPeerCount(1), "peers not visible");

    AppHandle host, join;
    CHECK(launchAppPid(b, appPath("64"),
                       tcpArgs("host", "H", 6112, 1, 1, kSmallSizeKb, 0, reportPath("hc_host")), host),
          "launch host");
    CHECK(waitReady(host), notReady("host", host));
    CHECK(launchAppPid(a, appPath("64"),
                       tcpArgs("joiner", "H", 6112, 1, 0, kSmallSizeKb, 0, reportPath("hc_join"),
                               ipv4ToString(cluster.vip(1))), join),
          "launch joiner");
    CHECK(waitApps({&host, &join}, 25000), "half-close apps timed out");
    CHECK(host.report.rfind("PASS", 0) == 0, "host report: " + host.report);
    CHECK(join.report.rfind("PASS", 0) == 0, "joiner report: " + join.report);
    return true;
}

APIVPN_TEST(tcp_abortive_close) {
    TestCluster cluster;
    CHECK(cluster.startServer(), "server start");
    client::VpnClient& a = cluster.connect("AB_A");
    client::VpnClient& b = cluster.connect("AB_B");
    CHECK(cluster.waitAllConnected() && cluster.waitPeerCount(1), "peers not visible");

    AppHandle host, join;
    CHECK(launchAppPid(b, appPath("64"),
                       tcpArgs("host", "A", 6112, 1, 1, 64, 0, reportPath("ab_host")), host),
          "launch host");
    CHECK(waitReady(host), notReady("host", host));
    CHECK(launchAppPid(a, appPath("64"),
                       tcpArgs("joiner", "A", 6112, 1, 0, 64, 0, reportPath("ab_join"),
                               ipv4ToString(cluster.vip(1))), join),
          "launch joiner");
    CHECK(waitApps({&host, &join}, 20000), "abort apps timed out");
    CHECK(host.report.rfind("PASS", 0) == 0, "host did not observe abortive close: " + host.report);
    CHECK(join.report.rfind("PASS", 0) == 0, "joiner report: " + join.report);
    return true;
}

APIVPN_TEST(tcp_connect_refused) {
    TestCluster cluster;
    CHECK(cluster.startServer(), "server start");
    client::VpnClient& a = cluster.connect("RF_A");
    client::VpnClient& b = cluster.connect("RF_B");
    CHECK(cluster.waitAllConnected() && cluster.waitPeerCount(1), "peers not visible");

    // Nothing is listening on virtual TCP port 6113 on peer B.
    AppHandle join;
    CHECK(launchAppPid(a, appPath("64"),
                       tcpArgs("joiner", "refused", 6113, 1, 0, 16, 0, reportPath("rf_join"),
                               ipv4ToString(cluster.vip(1))), join),
          "launch refused joiner");
    CHECK(waitApp(join, 15000), "refused app timed out");
    CHECK(join.report.rfind("PASS", 0) == 0,
          "connect() to a closed virtual port should have failed: " + join.report);
    (void)b;
    return true;
}

APIVPN_TEST(tcp_flow_control_backpressure) {
    TestCluster cluster;
    CHECK(cluster.startServer(), "server start");
    client::VpnClient& a = cluster.connect("FC_A");
    client::VpnClient& b = cluster.connect("FC_B");
    CHECK(cluster.waitAllConnected() && cluster.waitPeerCount(1), "peers not visible");

    // Receiver stalls ~700ms before draining; the sender must respect the window.
    AppHandle host, join;
    CHECK(launchAppPid(b, appPath("64"),
                       tcpArgs("host", "F", 6112, 1, 1, 2048, 0, reportPath("fc_host")), host),
          "launch host");
    CHECK(waitReady(host), notReady("host", host));
    CHECK(launchAppPid(a, appPath("64"),
                       tcpArgs("joiner", "F", 6112, 1, 0, 2048, 700, reportPath("fc_join"),
                               ipv4ToString(cluster.vip(1))), join),
          "launch joiner");
    CHECK(waitApps({&host, &join}, 25000), "flow-control apps timed out");
    CHECK(host.report.rfind("PASS", 0) == 0, "host report: " + host.report);
    CHECK(join.report.rfind("PASS", 0) == 0, "joiner report: " + join.report);
    return true;
}

APIVPN_TEST(tcp_reorder_and_duplicate) {
    TestCluster cluster;
    CHECK(cluster.startServer(), "server start");
    client::VpnClient& a = cluster.connect("RO_A");
    client::VpnClient& b = cluster.connect("RO_B");
    a.setSimulateReorderPercent(25);
    b.setSimulateReorderPercent(25);
    a.setSimulateDuplicatePercent(10);
    b.setSimulateDuplicatePercent(10);
    CHECK(cluster.waitAllConnected() && cluster.waitPeerCount(1), "peers not visible");

    AppHandle host, join;
    CHECK(launchAppPid(b, appPath("64"),
                       tcpArgs("host", "B", 6112, 1, 1, kSmallSizeKb, 0, reportPath("ro_host")), host),
          "launch host");
    CHECK(waitReady(host), notReady("host", host));
    CHECK(launchAppPid(a, appPath("64"),
                       tcpArgs("joiner", "B", 6112, 1, 0, kSmallSizeKb, 0, reportPath("ro_join"),
                               ipv4ToString(cluster.vip(1))), join),
          "launch joiner");
    CHECK(waitApps({&host, &join}, 30000), "reorder apps timed out");
    CHECK(host.report.rfind("PASS", 0) == 0, "host report: " + host.report);
    CHECK(join.report.rfind("PASS", 0) == 0, "joiner report: " + join.report);
    return true;
}

APIVPN_TEST(tcp_packet_loss_sack_recovery) {
    TestCluster cluster;
    CHECK(cluster.startServer(), "server start");
    client::VpnClient& a = cluster.connect("Loss_A");
    client::VpnClient& b = cluster.connect("Loss_B");
    CHECK(cluster.waitAllConnected() && cluster.waitPeerCount(1), "peers not visible");
    a.setSimulatePacketLossPercent(5);
    b.setSimulatePacketLossPercent(5);

    AppHandle host, join;
    CHECK(launchAppPid(b, appPath("64"),
                       tcpArgs("host", "B", 6112, 1, 1, kBulkSizeKb, 0, reportPath("loss_host")), host),
          "launch host");
    CHECK(waitReady(host), notReady("host", host));
    CHECK(launchAppPid(a, appPath("64"),
                       tcpArgs("joiner", "B", 6112, 1, 0, kBulkSizeKb, 0, reportPath("loss_join"),
                               ipv4ToString(cluster.vip(1))), join),
          "launch joiner");
    CHECK(waitApps({&host, &join}, 30000), "loss apps timed out");
    CHECK(host.report.rfind("PASS", 0) == 0, "host report: " + host.report);
    CHECK(join.report.rfind("PASS", 0) == 0, "joiner report: " + join.report);

    auto sa = a.getSnapshot();
    auto sb = b.getSnapshot();
    uint64_t drops = sa.tcp_simulated_drops + sb.tcp_simulated_drops;
    uint64_t rexmit = sa.tcp_fast_retransmits + sa.tcp_rto_retransmits +
                      sb.tcp_fast_retransmits + sb.tcp_rto_retransmits;
    uint64_t sack = sa.tcp_sack_events + sb.tcp_sack_events;
    uint64_t downs = sa.tcp_cwnd_scale_downs + sb.tcp_cwnd_scale_downs;
    CHECK(drops >= 20, "expected simulated drops, got " + std::to_string(drops));
    CHECK(rexmit >= drops, "retransmits did not cover simulated drops");
    CHECK(sack > 0, "no SACK events recorded");
    CHECK(downs > 0, "congestion window never backed off");
    a.setSimulatePacketLossPercent(0);
    b.setSimulatePacketLossPercent(0);
    return true;
}

APIVPN_TEST(tcp_bandwidth_fluctuation) {
    TestCluster cluster;
    CHECK(cluster.startServer(), "server start");
    client::VpnClient& a = cluster.connect("BW_A");
    client::VpnClient& b = cluster.connect("BW_B");
    CHECK(cluster.waitAllConnected() && cluster.waitPeerCount(1), "peers not visible");
    a.setSimulateBandwidthFluctuation(true);
    b.setSimulateBandwidthFluctuation(true);

    AppHandle host, join;
    CHECK(launchAppPid(b, appPath("64"),
                       tcpArgs("host", "B", 6112, 1, 1, kBulkSizeKb, 0, reportPath("bw_host")), host),
          "launch host");
    CHECK(waitReady(host), notReady("host", host));
    CHECK(launchAppPid(a, appPath("64"),
                       tcpArgs("joiner", "B", 6112, 1, 0, kBulkSizeKb, 0, reportPath("bw_join"),
                               ipv4ToString(cluster.vip(1))), join),
          "launch joiner");
    CHECK(waitApps({&host, &join}, 30000), "bandwidth apps timed out");
    CHECK(host.report.rfind("PASS", 0) == 0, "host report: " + host.report);
    CHECK(join.report.rfind("PASS", 0) == 0, "joiner report: " + join.report);

    auto sa = a.getSnapshot();
    auto sb = b.getSnapshot();
    uint64_t drops = sa.tcp_simulated_drops + sb.tcp_simulated_drops;
    uint64_t ups = sa.tcp_cwnd_scale_ups + sb.tcp_cwnd_scale_ups;
    uint64_t downs = sa.tcp_cwnd_scale_downs + sb.tcp_cwnd_scale_downs;
    CHECK(drops > 0, "bandwidth fluctuation produced no tail drops");
    CHECK(ups > 0 && downs > 0, "dynamic cwnd scaling did not engage");
    a.setSimulateBandwidthFluctuation(false);
    b.setSimulateBandwidthFluctuation(false);
    return true;
}

APIVPN_TEST(tcp_route_switch_mid_transfer) {
    TestCluster cluster;
    CHECK(cluster.startServer(), "server start");
    client::VpnClient& a = cluster.connect("SW_A");
    client::VpnClient& b = cluster.connect("SW_B");
    CHECK(cluster.waitAllConnected() && cluster.waitPeerCount(1), "peers not visible");
    CHECK(waitDirectPairAny(cluster, 0, 1),
          "no direct P2P before transfer");

    AppHandle host, join;
    CHECK(launchAppPid(b, appPath("64"),
                       tcpArgs("host", "B", 6112, 1, 1, kBulkSizeKb, 0, reportPath("sw_host")), host),
          "launch host");
    CHECK(waitReady(host), notReady("host", host));
    CHECK(launchAppPid(a, appPath("64"),
                       tcpArgs("joiner", "B", 6112, 1, 0, kBulkSizeKb, 0, reportPath("sw_join"),
                               ipv4ToString(cluster.vip(1))), join),
          "launch joiner");

    // Flip the sender onto the relay path while the stream is in flight.
    std::this_thread::sleep_for(std::chrono::milliseconds(300));
    a.setForceRelay(true);

    CHECK(waitApps({&host, &join}, 30000), "route-switch apps timed out");
    CHECK(host.report.rfind("PASS", 0) == 0, "host report: " + host.report);
    CHECK(join.report.rfind("PASS", 0) == 0, "joiner report: " + join.report);
    return true;
}

APIVPN_TEST(tcp_tunnel_cleanup_after_close) {
    TestCluster cluster;
    CHECK(cluster.startServer(), "server start");
    client::VpnClient& a = cluster.connect("Cln_A");
    client::VpnClient& b = cluster.connect("Cln_B");
    CHECK(cluster.waitAllConnected() && cluster.waitPeerCount(1), "peers not visible");

    AppHandle host, join;
    CHECK(launchAppPid(b, appPath("64"),
                       tcpArgs("host", "B", 6112, 1, 1, 128, 0, reportPath("cln_host")), host),
          "launch host");
    CHECK(waitReady(host), notReady("host", host));
    CHECK(launchAppPid(a, appPath("64"),
                       tcpArgs("joiner", "B", 6112, 1, 0, 128, 0, reportPath("cln_join"),
                               ipv4ToString(cluster.vip(1))), join),
          "launch joiner");
    CHECK(waitApps({&host, &join}, 20000), "cleanup apps timed out");
    CHECK(host.report.rfind("PASS", 0) == 0 && join.report.rfind("PASS", 0) == 0,
          "transfer failed before cleanup check");

    CHECK(waitUntil([&]() {
        return a.getSnapshot().tcp_tunnel_count == 0 && b.getSnapshot().tcp_tunnel_count == 0;
    }, 8000), "TCP tunnels were not reclaimed after close (A=" +
        std::to_string(a.getSnapshot().tcp_tunnel_count) + " B=" +
        std::to_string(b.getSnapshot().tcp_tunnel_count) + ")");
    return true;
}

APIVPN_TEST(tcp_listener_survives_accepted_close) {
    TestCluster cluster;
    CHECK(cluster.startServer(), "server start");
    client::VpnClient& a = cluster.connect("LS_A");
    client::VpnClient& b = cluster.connect("LS_B");
    CHECK(cluster.waitAllConnected() && cluster.waitPeerCount(1), "peers not visible");

    // Regression: closing an accepted socket used to unregister its virtual listener, so every
    // later connect to the same port was refused. In --sequential mode the joiner waits for the
    // host's close before reconnecting, so connections #2 and #3 always follow such a close.
    const int conns = 3;
    AppHandle host, join;
    CHECK(launchAppPid(b, appPath("64"),
                       tcpArgs("host", "B", 6112, conns, conns, 64, 0, reportPath("ls_host")) +
                           " --sequential", host),
          "launch host");
    CHECK(waitReady(host), notReady("host", host));
    CHECK(launchAppPid(a, appPath("64"),
                       tcpArgs("joiner", "B", 6112, conns, 0, 64, 0, reportPath("ls_join"),
                               ipv4ToString(cluster.vip(1))) + " --sequential", join),
          "launch joiner");
    CHECK(waitApps({&host, &join}, 30000), "sequential listener apps timed out");
    CHECK(host.report.rfind("PASS", 0) == 0, "host report: " + host.report);
    CHECK(join.report.rfind("PASS", 0) == 0, "joiner report: " + join.report);
    return true;
}

APIVPN_TEST(seq_wraparound_comparison) {
    // The virtual-TCP sequence space is modulo 2^32; plain unsigned comparisons invert
    // across the wrap boundary, which RFC 1982 serial-number arithmetic must not.
    CHECK(seqLess(0xFFFFFFFFu, 0x00000001u), "0xFFFFFFFF must sort before 0x1 across the wrap");
    CHECK(seqGreater(0x00000001u, 0xFFFFFFFFu), "0x1 must sort after 0xFFFFFFFF across the wrap");
    CHECK(!seqLess(0x00000001u, 0xFFFFFFFFu), "wrap must not invert forward ordering");
    CHECK(seqLess(1u, 2u) && seqGreater(2u, 1u), "plain forward ordering");
    CHECK(!seqLess(5u, 5u) && !seqGreater(5u, 5u), "equal sequence numbers compare neither way");

    // A window that straddles the wrap boundary.
    uint32_t ack = 0xFFFFFFFEu;             // next_expected_seq
    uint32_t ooo = 0x00000000u;             // out-of-order segment two slots ahead
    uint32_t lost = 0xFFFFFFFFu;            // missing segment one slot ahead
    CHECK(seqGreater(ooo, ack) && seqGreater(lost, ack), "segments above acked seq across wrap");
    CHECK(seqLess(ack, ooo) && seqLess(ack, lost), "acked seq below straddling window");
    CHECK(seqLess(lost, ooo) && seqGreater(ooo, lost), "ordering within straddling window");
    return true;
}

APIVPN_TEST(tcp_tunnel_idle_timeout_reaps_dead_peer) {
    TestCluster cluster;
    CHECK(cluster.startServer(), "server start");
    client::VpnClient& a = cluster.connect("Idle_A");
    client::VpnClient& b = cluster.connect("Idle_B");

    // Shrink the liveness timers on both sides so the reap happens in test time
    // (defaults are 15s probe / 60s idle).
    a.setTcpTunnelKeepAliveIntervalUs(300'000ULL);
    a.setTcpTunnelIdleTimeoutUs(1'500'000ULL);
    b.setTcpTunnelKeepAliveIntervalUs(300'000ULL);
    b.setTcpTunnelIdleTimeoutUs(1'500'000ULL);

    CHECK(cluster.waitAllConnected() && cluster.waitPeerCount(1), "peers not visible");
    CHECK(waitDirectPairAny(cluster, 0, 1), "no direct P2P");

    // Host streams a burst; the joiner sleeps (delay) before reading so the tunnel is
    // established and has carried data by the time we cut the peer off.
    AppHandle host, join;
    CHECK(launchAppPid(b, appPath("64"),
                       tcpArgs("host", "F", 6112, 1, 1, 64, 0, reportPath("idle_host")), host),
          "launch host");
    CHECK(waitReady(host), notReady("host", host));
    CHECK(launchAppPid(a, appPath("64"),
                       tcpArgs("joiner", "F", 6112, 1, 0, 64, 6000, reportPath("idle_join"),
                               ipv4ToString(cluster.vip(1))), join),
          "launch joiner");

    CHECK(waitUntil([&]() { return a.getSnapshot().tcp_tunnel_count >= 1; }, 8000),
          "virtual TCP tunnel was not established on A");

    // Let the tunnel sit idle for several idle-timeouts: the periodic liveness probes
    // must keep BOTH live tunnels from being reaped.
    std::this_thread::sleep_for(std::chrono::milliseconds(2500));
    CHECK(a.getSnapshot().tcp_tunnel_count == 1 && b.getSnapshot().tcp_tunnel_count == 1,
          "idle-but-alive tunnels were wrongly reaped (A=" +
          std::to_string(a.getSnapshot().tcp_tunnel_count) + " B=" +
          std::to_string(b.getSnapshot().tcp_tunnel_count) + ")");

    // Abruptly remove peer B. A must now detect the silence and reap the tunnel instead of
    // leaking its loopback socket and up to 4 MB receive buffer forever.
    cluster.clients[1]->stop();

    bool reaped = waitUntil([&]() { return a.getSnapshot().tcp_tunnel_count == 0; }, 8000);
    uint32_t left = a.getSnapshot().tcp_tunnel_count;

    // The host/joiner apps are still blocked on the dead tunnel; ~TestCluster reaps them.
    CHECK(reaped, "tunnel to the dead peer was not reaped (still " + std::to_string(left) + ")");
    return true;
}

APIVPN_TEST(tcp_idle_timeout_resets_app) {
    TestCluster cluster;
    CHECK(cluster.startServer(), "server start");
    client::VpnClient& a = cluster.connect("IR_A");
    client::VpnClient& b = cluster.connect("IR_B");

    // A reaps after 1.5 s of silence. B stays in the peer list but never probes (1 h interval)
    // and its app sends nothing, so only A's idle timeout can end the tunnel.
    a.setTcpTunnelKeepAliveIntervalUs(300'000ULL);
    a.setTcpTunnelIdleTimeoutUs(1'500'000ULL);
    b.setTcpTunnelKeepAliveIntervalUs(3'600'000'000ULL);

    CHECK(cluster.waitAllConnected() && cluster.waitPeerCount(1), "peers not visible");

    AppHandle host, join;
    CHECK(launchAppPid(b, appPath("64"),
                       tcpArgs("host", "I", 6112, 1, 1, 0, 0, reportPath("ir_host")), host),
          "launch host");
    CHECK(waitReady(host), notReady("host", host));
    CHECK(launchAppPid(a, appPath("64"),
                       tcpArgs("joiner", "I", 6112, 1, 0, 0, 0, reportPath("ir_join"),
                               ipv4ToString(cluster.vip(1))), join),
          "launch joiner");

    // The joiner passes only if its idle connection ends in a reset, not an EOF.
    CHECK(waitApp(join, 20000), "idle joiner timed out");
    CHECK(join.report.rfind("PASS", 0) == 0, "joiner report: " + join.report);
    CHECK(waitUntil([&]() { return a.getSnapshot().tcp_tunnel_count == 0; }, 5000),
          "idle tunnel was not reaped on A");
    return true;
}

APIVPN_TEST(tcp_half_close_survives_idle_timeout) {
    TestCluster cluster;
    CHECK(cluster.startServer(), "server start");
    client::VpnClient& a = cluster.connect("HI_A");
    client::VpnClient& b = cluster.connect("HI_B");

    // Idle timeout 1.5 s on both sides. The host stays silent for 3 s after the joiner's FIN,
    // so the tunnel survives only if the joiner's side keeps probing after sending its FIN.
    for (client::VpnClient* c : {&a, &b}) {
        c->setTcpTunnelKeepAliveIntervalUs(300'000ULL);
        c->setTcpTunnelIdleTimeoutUs(1'500'000ULL);
    }

    CHECK(cluster.waitAllConnected() && cluster.waitPeerCount(1), "peers not visible");

    AppHandle host, join;
    CHECK(launchAppPid(b, appPath("64"),
                       tcpArgs("host", "H", 6112, 1, 1, 64, 3000, reportPath("hi_host")), host),
          "launch host");
    CHECK(waitReady(host), notReady("host", host));
    CHECK(launchAppPid(a, appPath("64"),
                       tcpArgs("joiner", "H", 6112, 1, 0, 64, 0, reportPath("hi_join"),
                               ipv4ToString(cluster.vip(1))), join),
          "launch joiner");
    CHECK(waitApps({&host, &join}, 25000), "half-close idle apps timed out");
    CHECK(host.report.rfind("PASS", 0) == 0, "host report: " + host.report);
    CHECK(join.report.rfind("PASS", 0) == 0, "joiner report: " + join.report);
    return true;
}

APIVPN_TEST(udp_large_datagram) {
    TestCluster cluster;
    CHECK(cluster.startServer(), "server start");
    client::VpnClient& a = cluster.connect("UL_A");
    client::VpnClient& b = cluster.connect("UL_B");
    CHECK(cluster.waitAllConnected() && cluster.waitPeerCount(1), "peers not visible");

    AppHandle host, join;
    CHECK(launchAppPid(b, appPath("64"),
                       udpLargeArgs("host", 6112, "", 6, reportPath("ul_host")),
                       reportPath("ul_host"), host),
          "launch host");
    CHECK(waitReady(host), notReady("host", host));
    CHECK(launchAppPid(a, appPath("64"),
                       udpLargeArgs("joiner", 6112, ipv4ToString(cluster.vip(1)), 6,
                                    reportPath("ul_join")),
                       reportPath("ul_join"), join),
          "launch joiner");
    CHECK(waitApps({&host, &join}, 20000), "large-datagram apps timed out");
    CHECK(host.report.rfind("PASS", 0) == 0, "host report: " + host.report);
    CHECK(join.report.rfind("PASS", 0) == 0, "joiner report: " + join.report);
    return true;
}

APIVPN_TEST(udp_api_surface) {
    TestCluster cluster;
    CHECK(cluster.startServer(), "server start");
    client::VpnClient& a = cluster.connect("API_A");
    client::VpnClient& b = cluster.connect("API_B");
    CHECK(cluster.waitAllConnected() && cluster.waitPeerCount(1), "peers not visible");

    AppHandle host, join;
    CHECK(launchAppPid(b, appPath("64"),
                       udpApiArgs("host", 6112, ipv4ToString(cluster.vip(0)),
                                  reportPath("api_host")),
                       reportPath("api_host"), host),
          "launch udp-api host");
    CHECK(waitReady(host), notReady("udp-api host", host));
    CHECK(launchAppPid(a, appPath("32"),
                       udpApiArgs("joiner", 6112, ipv4ToString(cluster.vip(1)),
                                  reportPath("api_join")),
                       reportPath("api_join"), join),
          "launch udp-api joiner");
    CHECK(waitApps({&host, &join}, 20000), "udp-api apps timed out");
    CHECK(host.report.rfind("PASS", 0) == 0, "udp-api host report: " + host.report);
    CHECK(join.report.rfind("PASS", 0) == 0, "udp-api joiner report: " + join.report);
    return true;
}

APIVPN_TEST(udp_edge_semantics) {
    TestCluster cluster;
    CHECK(cluster.startServer(), "server start");
    client::VpnClient& a = cluster.connect("EDGE_A");
    client::VpnClient& b = cluster.connect("EDGE_B");
    CHECK(cluster.waitAllConnected() && cluster.waitPeerCount(1), "peers not visible");

    AppHandle host, join;
    CHECK(launchAppPid(b, appPath("64"),
                       udpEdgeArgs("host", 6112, ipv4ToString(cluster.vip(0)),
                                   reportPath("edge_host")),
                       reportPath("edge_host"), host),
          "launch udp-edge host");
    CHECK(waitReady(host), notReady("udp-edge host", host));
    CHECK(launchAppPid(a, appPath("64"),
                       udpEdgeArgs("joiner", 6112, ipv4ToString(cluster.vip(1)),
                                   reportPath("edge_join")),
                       reportPath("edge_join"), join),
          "launch udp-edge joiner");
    CHECK(waitApps({&host, &join}, 20000), "udp-edge apps timed out");
    CHECK(host.report.rfind("PASS", 0) == 0, "udp-edge host report: " + host.report);
    CHECK(join.report.rfind("PASS", 0) == 0, "udp-edge joiner report: " + join.report);
    return true;
}

APIVPN_TEST(adapter_dns_virtualization) {
    TestCluster cluster;
    CHECK(cluster.startServer(), "server start");
    client::VpnClient& a = cluster.connect("AD_A");
    CHECK(cluster.waitConnected(0), "client did not connect");

    AppHandle app;
    CHECK(launchAppPid(a, appPath("64"),
                       adapterDnsArgs(reportPath("ad_app")),
                       reportPath("ad_app"), app),
          "launch adapter/DNS app");
    CHECK(waitApp(app, 10000), "adapter/DNS app timed out");
    CHECK(app.report.rfind("PASS", 0) == 0, "adapter/DNS report: " + app.report);
    return true;
}

APIVPN_TEST(udp_wait_models) {
    TestCluster cluster;
    CHECK(cluster.startServer(), "server start");
    client::VpnClient& a = cluster.connect("WAIT_A");
    client::VpnClient& b = cluster.connect("WAIT_B");
    CHECK(cluster.waitAllConnected() && cluster.waitPeerCount(1), "peers not visible");

    AppHandle host, join;
    CHECK(launchAppPid(b, appPath("64"),
                       udpWaitArgs("host", 6112, ipv4ToString(cluster.vip(0)),
                                   reportPath("wait_host")),
                       reportPath("wait_host"), host),
          "launch udp-wait host");
    CHECK(waitReady(host), notReady("udp-wait host", host));
    CHECK(launchAppPid(a, appPath("64"),
                       udpWaitArgs("joiner", 6112, ipv4ToString(cluster.vip(1)),
                                   reportPath("wait_join")),
                       reportPath("wait_join"), join),
          "launch udp-wait joiner");
    CHECK(waitApps({&host, &join}, 20000), "udp-wait apps timed out");
    CHECK(host.report.rfind("PASS", 0) == 0, "udp-wait host report: " + host.report);
    CHECK(join.report.rfind("PASS", 0) == 0, "udp-wait joiner report: " + join.report);
    return true;
}

APIVPN_TEST(tcp_nonblocking_connect) {
    TestCluster cluster;
    CHECK(cluster.startServer(), "server start");
    client::VpnClient& a = cluster.connect("NB_A");
    client::VpnClient& b = cluster.connect("NB_B");
    CHECK(cluster.waitAllConnected() && cluster.waitPeerCount(1), "peers not visible");

    AppHandle host, join;
    CHECK(launchAppPid(b, appPath("64"),
                       tcpArgs("host", "B", 6112, 1, 1, 128, 0, reportPath("nb_host")), host),
          "launch non-blocking host");
    CHECK(waitReady(host), notReady("non-blocking host", host));
    CHECK(launchAppPid(a, appPath("64"),
                       tcpArgs("joiner", "N", 6112, 1, 0, 128, 0, reportPath("nb_join"),
                               ipv4ToString(cluster.vip(1))), join),
          "launch non-blocking joiner");
    CHECK(waitApps({&host, &join}, 25000), "non-blocking connect apps timed out");
    CHECK(host.report.rfind("PASS", 0) == 0, "non-blocking host report: " + host.report);
    CHECK(join.report.rfind("PASS", 0) == 0, "non-blocking joiner report: " + join.report);
    return true;
}

APIVPN_TEST(relay_broadcast_multicast_fanout) {
    TestCluster cluster;
    CHECK(cluster.startServer(), "server start");
    client::VpnClient& a = cluster.connect("RB_A");
    client::VpnClient& b = cluster.connect("RB_B");
    client::VpnClient& c = cluster.connect("RB_C");
    CHECK(cluster.waitAllConnected() && cluster.waitPeerCount(2), "peers not visible");
    a.setForceRelay(true);
    b.setForceRelay(true);
    c.setForceRelay(true);
    CHECK(waitUntil([&]() {
        auto sa = a.getSnapshot();
        auto sb = b.getSnapshot();
        auto sc = c.getSnapshot();
        auto pa_b = findPeer(sa, cluster.vip(1));
        auto pa_c = findPeer(sa, cluster.vip(2));
        return pa_b && pa_c && pa_b->route_mode == client::RouteMode::ServerRelay &&
               pa_c->route_mode == client::RouteMode::ServerRelay;
    }, 5000), "force-relay routes did not settle");

    auto run_fanout = [&](const std::string& token, const std::string& dst,
                          const std::string& suffix) -> bool {
        AppHandle rb, rc, sa;
        if (!launchAppPid(b, appPath("64"),
                          udpRecvArgs(6112, token, 5000, 1, reportPath(suffix + "_b")), rb) ||
            !launchAppPid(c, appPath("64"),
                          udpRecvArgs(6112, token, 5000, 1, reportPath(suffix + "_c")), rc)) {
            return false;
        }
        if (!waitReady(rb) || !waitReady(rc)) return false;
        if (!launchAppPid(a, appPath("64"),
                          udpSendArgs(6112, dst, token, 5, reportPath(suffix + "_a")), sa)) {
            return false;
        }
        if (!waitApps({&sa, &rb, &rc}, 12000)) return false;
        return sa.report.rfind("PASS", 0) == 0 &&
               parseUintAfter(rb.report, "recv=") >= 1 &&
               parseUintAfter(rc.report, "recv=") >= 1;
    };

    CHECK(run_fanout("RBCAST", "10.42.0.255", "rbf_bcast"),
          "relayed subnet broadcast did not reach both peers");
    CHECK(run_fanout("RMCAST", "239.255.0.1", "rbf_mcast"),
          "relayed multicast did not reach both peers");
    return true;
}

APIVPN_TEST(udp_overlapped_io) {
    TestCluster cluster;
    CHECK(cluster.startServer(), "server start");
    client::VpnClient& a = cluster.connect("OVL_A");
    client::VpnClient& b = cluster.connect("OVL_B");
    CHECK(cluster.waitAllConnected() && cluster.waitPeerCount(1), "peers not visible");

    AppHandle host, join;
    CHECK(launchAppPid(a, appPath("64"),
                       udpOverlappedArgs("host", 6112, ipv4ToString(cluster.vip(1)),
                                         reportPath("ovl_host")),
                       reportPath("ovl_host"), host),
          "launch overlapped host");
    CHECK(waitReady(host), notReady("overlapped host", host));
    CHECK(launchAppPid(b, appPath("32"),
                       udpOverlappedArgs("joiner", 6112, ipv4ToString(cluster.vip(0)),
                                         reportPath("ovl_join")),
                       reportPath("ovl_join"), join),
          "launch overlapped joiner");
    CHECK(waitApps({&host, &join}, 30000), "overlapped-I/O apps timed out");
    CHECK(host.report.rfind("PASS", 0) == 0, "overlapped host report: " + host.report);
    CHECK(join.report.rfind("PASS", 0) == 0, "overlapped joiner report: " + join.report);
    return true;
}

APIVPN_TEST(udp_overlapped_edge_cases) {
    TestCluster cluster;
    CHECK(cluster.startServer(), "server start");
    client::VpnClient& a = cluster.connect("OVLE_A");
    client::VpnClient& b = cluster.connect("OVLE_B");
    CHECK(cluster.waitAllConnected() && cluster.waitPeerCount(1), "peers not visible");

    AppHandle host, join;
    CHECK(launchAppPid(b, appPath("64"),
                       udpOverlappedEdgeArgs("host", 6112, ipv4ToString(cluster.vip(0)),
                                             reportPath("ovle_host")),
                       reportPath("ovle_host"), host),
          "launch overlapped-edge host");
    CHECK(waitReady(host), notReady("overlapped-edge host", host));
    CHECK(launchAppPid(a, appPath("64"),
                       udpOverlappedEdgeArgs("joiner", 6112, ipv4ToString(cluster.vip(1)),
                                             reportPath("ovle_join")),
                       reportPath("ovle_join"), join),
          "launch overlapped-edge joiner");
    CHECK(waitApps({&host, &join}, 25000), "overlapped-edge apps timed out");
    CHECK(host.report.rfind("PASS", 0) == 0, "overlapped-edge host report: " + host.report);
    CHECK(join.report.rfind("PASS", 0) == 0, "overlapped-edge joiner report: " + join.report);
    return true;
}

APIVPN_TEST(cross_bitness_child_spawn) {
    TestCluster cluster;
    CHECK(cluster.startServer(), "server start");
    client::VpnClient& a = cluster.connect("XBit_A");
    client::VpnClient& b = cluster.connect("XBit_B");
    CHECK(cluster.waitAllConnected() && cluster.waitPeerCount(1), "peers not visible");

    AppHandle host, join;
    CHECK(launchAppPid(a, appPath("32"),
                       basicSpawnArgs(appPath("64"), "host", reportPath("xb_host")),
                       reportPath("xb_host"), host),
          "launch 32-bit parent (host)");
    CHECK(waitReady(host), notReady("host child", host));
    CHECK(launchAppPid(b, appPath("64"),
                       basicSpawnArgs(appPath("32"), "joiner", reportPath("xb_join")),
                       reportPath("xb_join"), join),
          "launch 64-bit parent (joiner)");
    CHECK(waitApps({&host, &join}, 20000), "cross-bitness apps timed out");
    CHECK(host.report.rfind("PASS", 0) == 0, "host report: " + host.report);
    CHECK(join.report.rfind("PASS", 0) == 0, "joiner report: " + join.report);
    return true;
}

APIVPN_TEST(process_injection_createprocessw) {
    TestCluster cluster;
    CHECK(cluster.startServer(), "server start");
    client::VpnClient& a = cluster.connect("PI_A");
    CHECK(cluster.waitConnected(0), "client did not connect");

    const std::string parent_report = reportPath("pi_parent");
    const std::string child_report = reportPath("pi_child");
    std::remove(child_report.c_str());
    g_apps.reports.push_back(child_report);

    AppHandle parent;
    CHECK(launchAppPid(a, appPath("64"),
                       spawnWArgs(appPath("64"), child_report, parent_report),
                       parent_report, parent),
          "launch CreateProcessW parent");
    CHECK(waitApp(parent, 20000), "CreateProcessW parent timed out");
    CHECK(parent.report.rfind("PASS", 0) == 0, "CreateProcessW parent report: " + parent.report);
    return true;
}

APIVPN_TEST(process_injection_fail_closed) {
    TestCluster cluster;
    CHECK(cluster.startServer(), "server start");
    client::VpnClient& a = cluster.connect("PIF_A");
    CHECK(cluster.waitConnected(0), "client did not connect");

    const std::string parent_report = reportPath("pif_parent");
    const std::string child_report = reportPath("pif_child");
    std::remove(child_report.c_str());
    g_apps.reports.push_back(child_report);

    AppHandle parent;
    CHECK(launchAppPid(a, appPath("64"),
                       spawnFailArgs(appPath("64"), child_report, parent_report),
                       parent_report, parent),
          "launch fail-closed parent");
    CHECK(waitApp(parent, 15000), "fail-closed parent timed out");
    CHECK(parent.report.rfind("PASS", 0) == 0, "fail-closed parent report: " + parent.report);
    return true;
}

APIVPN_TEST(directplay_legacy_stack) {
    TestCluster cluster;
    CHECK(cluster.startServer(), "server start");
    client::VpnClient& a = cluster.connect("DP_A");
    client::VpnClient& b = cluster.connect("DP_B");
    CHECK(cluster.waitAllConnected() && cluster.waitPeerCount(1), "peers not visible");

    AppHandle host, join;
    CHECK(launchAppPid(a, appPath("32"),
                       "--dplay --role host --report \"" + reportPath("dp_host") + "\"",
                       reportPath("dp_host"), host),
          "launch dplay host");
    CHECK(waitReady(host, 15000), notReady("dplay host", host));
    CHECK(launchAppPid(b, appPath("32"),
                       "--dplay --role joiner --report \"" + reportPath("dp_join") + "\"",
                       reportPath("dp_join"), join),
          "launch dplay joiner");
    CHECK(waitApps({&host, &join}, 25000), "dplay apps timed out");
    CHECK(host.report.rfind("PASS", 0) == 0, "dplay host report: " + host.report);
    CHECK(join.report.rfind("PASS", 0) == 0, "dplay joiner report: " + join.report);
    return true;
}

// ============================================================================
// Runner main
// ============================================================================
} // namespace

namespace {

struct Result {
    std::string name;
    bool        passed = false;
    std::string detail;
    uint64_t    elapsed_ms = 0;
};

constexpr size_t kNameWidth = 44;

void printResult(const Result& r, size_t index, size_t total) {
    std::string label = "[" + std::to_string(index) + "/" + std::to_string(total) + "] " + r.name;
    if (label.size() < kNameWidth) {
        label.append(kNameWidth - label.size(), '.');
    }
    std::cout << label << " " << (r.passed ? "PASS" : "FAIL") << "  ("
              << r.elapsed_ms << " ms)" << std::endl;
    if (!r.passed) {
        std::cout << "      reason: " << r.detail << std::endl;
    }
}

// Deletes the test's app reports once it passed; keeps (and lists) them when it failed.
void finishReports(bool passed) {
    for (const auto& path : g_apps.reports) {
        if (passed) {
            std::remove(path.c_str());
        } else if (std::ifstream(path).good()) {
            std::cout << "      report kept: " << path << std::endl;
        }
    }
    g_apps.reports.clear();
}

} // namespace

int main(int argc, char** argv) {
    const std::string filter = (argc > 1) ? argv[1] : "";
    // Flush after every insertion so per-test progress reaches the console/pipe
    // immediately. Without this the C++ stream buffer is only drained on a full
    // buffer or a clean exit, so a hang, crash, or timeout leaves CI with an
    // empty log even though tests were running.
    std::cout.setf(std::ios::unitbuf);
    std::cerr.setf(std::ios::unitbuf);
    // Same for the C stdio layer, which MinGW's runtime may use for the console.
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    std::setvbuf(stderr, nullptr, _IONBF, 0);

    Logger::instance().setTag("IntegrationTest");
    // Keep the test report clean: silence the VPN stack's own logging.
    Logger::instance().setMinLevel(LogLevel::Off);

    std::cout << "APIVPN integration tests: initializing network stack..." << std::endl;

    if (!platform::initNetwork()) {
        // The logger is silenced above, so report this on the real console -
        // otherwise the runner exits non-zero with no diagnostic whatsoever.
        std::cerr << "FATAL: failed to initialize network stack" << std::endl;
        return 1;
    }
    g_bin_dir = platform::ProcessLauncher::getCurrentModuleDirectory();
    g_runTag = std::to_string(platform::currentProcessId());

    std::vector<TestCase> tests;
    for (const auto& t : testRegistry()) {
        if (filter.empty() || t.name.find(filter) != std::string::npos) {
            tests.push_back(t);
        }
    }

    std::cout << "============================================================\n";
    std::cout << " APIVPN Integration Test Suite (" << tests.size() << " tests)\n";
    std::cout << "============================================================\n";

    std::vector<Result> results;
    results.reserve(tests.size());
    auto suite_start = std::chrono::steady_clock::now();

    for (size_t i = 0; i < tests.size(); ++i) {
        Result r;
        r.name = tests[i].name;
        g_currentTest = r.name;
        g_testStartEpochSec = static_cast<int64_t>(std::chrono::duration_cast<std::chrono::seconds>(
            std::chrono::system_clock::now().time_since_epoch()).count());
        auto t0 = std::chrono::steady_clock::now();
        g_lastError.clear();
        bool ok = false;
        try {
            ok = tests[i].fn();
        } catch (const std::exception& e) {
            g_lastError = std::string("uncaught exception: ") + e.what();
        } catch (...) {
            g_lastError = "uncaught unknown exception";
        }
        // Safety net for apps launched outside a TestCluster (normally already reaped).
        reapLaunchedApps();
        r.elapsed_ms = static_cast<uint64_t>(
            std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now() - t0).count());
        r.passed = ok;
        r.detail = ok ? "" : (g_lastError.empty() ? "failed" : g_lastError);
        printResult(r, i + 1, tests.size());
        finishReports(ok);
        results.push_back(std::move(r));
    }

    uint64_t total_ms = static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - suite_start).count());

    size_t passed = 0;
    for (const auto& r : results) {
        if (r.passed) ++passed;
    }
    size_t failed = results.size() - passed;

    std::cout << "------------------------------------------------------------\n";
    std::cout << " PASSED: " << passed << "   FAILED: " << failed
              << "   TOTAL: " << results.size() << "   (" << total_ms << " ms)\n";
    std::cout << "------------------------------------------------------------\n";
    if (failed == 0) {
        std::cout << " ALL TESTS PASSED\n";
    } else {
        std::cout << " " << failed << " TEST(S) FAILED\n";
    }
    std::cout << "============================================================\n";

    platform::cleanupNetwork();
    return failed == 0 ? 0 : 1;
}
