#include "apivpn/server/vpn_server.hpp"
#include "apivpn/common/logger.hpp"
#include "apivpn/platform/net_socket.hpp"

#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <thread>

namespace {
std::atomic<bool> g_stop{false};
void onSignal(int) { g_stop.store(true); }

// Strict port parser: decimal digits only, 1..65535 (strtoul would accept "-1", "12abc", "").
bool parsePort(const char* text, uint16_t& out_port) {
    if (text == nullptr || *text == '\0') return false;
    unsigned long value = 0;
    for (const char* p = text; *p != '\0'; ++p) {
        if (*p < '0' || *p > '9') return false;
        value = value * 10 + static_cast<unsigned long>(*p - '0');
        if (value > 65535) return false;
    }
    if (value == 0) return false;
    out_port = static_cast<uint16_t>(value);
    return true;
}

void printUsage(const char* exe) {
    std::fprintf(stderr, "Usage: %s [--port|-p <1-65535>]\n", exe);
}

constexpr int kExitUsage = 64;
} // namespace

int main(int argc, char* argv[]) {
    uint16_t port = apivpn::kDefaultServerPort;
    const char* exe = (argc > 0 && argv[0]) ? argv[0] : "apivpn_server";
    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--port") == 0 || std::strcmp(argv[i], "-p") == 0) {
            if (i + 1 >= argc) {
                std::fprintf(stderr, "Error: %s requires a value\n", argv[i]);
                printUsage(exe);
                return kExitUsage;
            }
            const char* value = argv[++i];
            if (!parsePort(value, port)) {
                std::fprintf(stderr, "Error: invalid port '%s' (expected a number from 1 to 65535)\n", value);
                printUsage(exe);
                return kExitUsage;
            }
        } else if (std::strcmp(argv[i], "--help") == 0 || std::strcmp(argv[i], "-h") == 0) {
            printUsage(exe);
            return 0;
        } else {
            std::fprintf(stderr, "Error: unknown argument '%s'\n", argv[i]);
            printUsage(exe);
            return kExitUsage;
        }
    }

    std::signal(SIGINT, onSignal);
    std::signal(SIGTERM, onSignal);

    apivpn::Logger::instance().setTag("Server");
    if (!apivpn::platform::initNetwork()) {
        apivpn::logError("Failed to initialize network stack");
        return 1;
    }

    apivpn::server::VpnServer server;
    if (!server.start(port)) {
        apivpn::logError("Failed to start VPN Server on port " + std::to_string(port));
        apivpn::platform::cleanupNetwork();
        return 2;
    }

    while (!g_stop.load() && server.isRunning()) {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }

    server.stop();
    apivpn::platform::cleanupNetwork();
    return 0;
}
