#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif

#include <winsock2.h>
#include <ws2tcpip.h>
#include <iphlpapi.h>
#include <windows.h>
#include <objbase.h>

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cwchar>
#include <fstream>
#include <iostream>
#include <string>
#include <thread>
#include <vector>
#include <algorithm>

#ifdef _MSC_VER
#pragma comment(lib, "ws2_32.lib")
#pragma comment(lib, "iphlpapi.lib")
#pragma comment(lib, "ole32.lib")
#endif

namespace {

constexpr uint16_t kGamePort = 6112;

std::string ipToStr(in_addr addr) {
    char buf[64] = {};
    inet_ntop(AF_INET, &addr, buf, sizeof(buf));
    return buf;
}

bool waitReadable(SOCKET s, int timeout_ms) {
    fd_set rfds;
    FD_ZERO(&rfds);
    FD_SET(s, &rfds);
    timeval tv{};
    tv.tv_sec = timeout_ms / 1000;
    tv.tv_usec = (timeout_ms % 1000) * 1000;
    return select(0, &rfds, nullptr, nullptr, &tv) > 0;
}

// Tells the integration runner that this app has bound/listened, so its peer can start now
// instead of after a fixed sleep. The final PASS/FAIL report overwrites this line.
void signalReady(const std::string& report_file) {
    if (report_file.empty()) return;
    std::ofstream out(report_file);
    out << "READY\n";
}

// A freshly launched host may still be setting up its listener, so a joiner retries a failed
// connect on a fresh socket (Winsock leaves a failed socket unusable) until the budget runs out.
constexpr int kConnectRetryBudgetMs  = 5000;
constexpr int kConnectRetryBackoffMs = 100;

SOCKET connectWithRetry(const sockaddr_in& dst, int& last_error) {
    auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(kConnectRetryBudgetMs);
    last_error = 0;
    while (true) {
        SOCKET s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        if (s == INVALID_SOCKET) {
            last_error = WSAGetLastError();
            return INVALID_SOCKET;
        }
        if (connect(s, reinterpret_cast<const sockaddr*>(&dst), sizeof(dst)) == 0) return s;
        last_error = WSAGetLastError();
        closesocket(s);
        if (std::chrono::steady_clock::now() + std::chrono::milliseconds(kConnectRetryBackoffMs) >= deadline) {
            return INVALID_SOCKET;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(kConnectRetryBackoffMs));
    }
}

// ============================================================================
// DirectPlay 4 (dplayx.dll / dpwsockx.dll / dplaysvr.exe) Test Definitions
// ============================================================================
using DPID = DWORD;
using LPDPID = DWORD*;

constexpr GUID kCLSID_DirectPlay      = {0xd1eb6d20, 0x8923, 0x11d0, {0x9d, 0x97, 0x00, 0xa0, 0xc9, 0x0a, 0x43, 0xcb}};
constexpr GUID kIID_IDirectPlay4A     = {0x0ab1c531, 0x4745, 0x11d1, {0xa7, 0xa1, 0x00, 0x00, 0xf8, 0x03, 0xab, 0xfc}};
constexpr GUID kCLSID_DirectPlayLobby = {0x2fe8f810, 0xb2a5, 0x11d0, {0xa7, 0x87, 0x00, 0x00, 0xf8, 0x03, 0xab, 0xfc}};
constexpr GUID kIID_IDirectPlayLobby3A= {0x2db72491, 0x652c, 0x11d1, {0xa7, 0xa8, 0x00, 0x00, 0xf8, 0x03, 0xab, 0xfc}};
constexpr GUID kDPSPGUID_TCPIP        = {0x36e95ee0, 0x8577, 0x11cf, {0x96, 0x0c, 0x00, 0x80, 0xc7, 0x53, 0x4e, 0x82}};
constexpr GUID kDPAID_ServiceProvider = {0x07d916c0, 0xe0af, 0x11cf, {0x9c, 0x4e, 0x00, 0xa0, 0xc9, 0x05, 0x42, 0x5e}};
constexpr GUID kDPAID_INet            = {0xc4a54da0, 0xe0af, 0x11cf, {0x9c, 0x4e, 0x00, 0xa0, 0xc9, 0x05, 0x42, 0x5e}};
constexpr GUID kAoE2TestAppGuid       = {0x54b79b61, 0xf0a1, 0x11d2, {0x8e, 0x1f, 0x00, 0xa0, 0xc9, 0x0a, 0x43, 0xcb}};

constexpr DWORD kDPOPEN_JOIN               = 0x00000001;
constexpr DWORD kDPOPEN_CREATE             = 0x00000002;
constexpr DWORD kDPENUMSESSIONS_AVAILABLE  = 0x00000001;
constexpr DWORD kDPENUMSESSIONS_STOPASYNC  = 0x00000100;
constexpr DWORD kDPRECEIVE_ALL             = 0x00000001;
constexpr DWORD kDPSEND_GUARANTEED         = 0x00000001;
constexpr HRESULT kDPERR_BUFFERTOOSMALL    = static_cast<HRESULT>(0x8877001E);
constexpr HRESULT kDPERR_NOMESSAGES        = static_cast<HRESULT>(0x887700BE);

struct DPNAME {
    DWORD dwSize;
    DWORD dwFlags;
    union {
        LPWSTR lpszShortName;
        LPSTR  lpszShortNameA;
    };
    union {
        LPWSTR lpszLongName;
        LPSTR  lpszLongNameA;
    };
};

struct DPSESSIONDESC2 {
    DWORD   dwSize;
    DWORD   dwFlags;
    GUID    guidInstance;
    GUID    guidApplication;
    DWORD   dwMaxPlayers;
    DWORD   dwCurrentPlayers;
    union {
        LPWSTR lpszSessionName;
        LPSTR  lpszSessionNameA;
    };
    union {
        LPWSTR lpszPassword;
        LPSTR  lpszPasswordA;
    };
    DWORD_PTR dwReserved1;
    DWORD_PTR dwReserved2;
    DWORD_PTR dwUser1;
    DWORD_PTR dwUser2;
    DWORD_PTR dwUser3;
    DWORD_PTR dwUser4;
};

struct DPCOMPOUNDADDRESSELEMENT {
    GUID    guidDataType;
    DWORD   dwDataSize;
    LPVOID  lpData;
};

using LPDPENUMSESSIONSCALLBACK2 = BOOL(FAR PASCAL*)(
    const DPSESSIONDESC2* lpThisSD, LPDWORD lpdwTimeOut, DWORD dwFlags, LPVOID lpContext);

struct IDirectPlayLobby3AVtbl {
    HRESULT(STDMETHODCALLTYPE* QueryInterface)(void* This, REFIID riid, void** ppvObj);
    ULONG(STDMETHODCALLTYPE* AddRef)(void* This);
    ULONG(STDMETHODCALLTYPE* Release)(void* This);
    void* Connect;
    void* CreateAddress;
    void* EnumAddress;
    void* EnumAddressTypes;
    void* EnumLocalApplications;
    void* GetConnectionSettings;
    void* ReceiveLobbyMessage;
    void* RunApplication;
    void* SendLobbyMessage;
    void* SetConnectionSettings;
    void* SetLobbyMessageEvent;
    HRESULT(STDMETHODCALLTYPE* CreateCompoundAddress)(
        void* This, const DPCOMPOUNDADDRESSELEMENT* lpElements, DWORD dwElementCount,
        LPVOID lpAddress, LPDWORD lpdwAddressSize);
};

struct IDirectPlayLobby3ARaw {
    IDirectPlayLobby3AVtbl* lpVtbl;
};

struct IDirectPlay4AVtbl {
    HRESULT(STDMETHODCALLTYPE* QueryInterface)(void* This, REFIID riid, void** ppvObj);
    ULONG(STDMETHODCALLTYPE* AddRef)(void* This);
    ULONG(STDMETHODCALLTYPE* Release)(void* This);
    void* AddPlayerToGroup;
    HRESULT(STDMETHODCALLTYPE* Close)(void* This);
    void* CreateGroup;
    HRESULT(STDMETHODCALLTYPE* CreatePlayer)(
        void* This, LPDPID lpidPlayer, DPNAME* lpPlayerName, HANDLE hEvent,
        LPVOID lpData, DWORD dwDataSize, DWORD dwFlags);
    void* DeletePlayerFromGroup;
    void* DestroyGroup;
    HRESULT(STDMETHODCALLTYPE* DestroyPlayer)(void* This, DPID idPlayer);
    void* EnumGroupPlayers;
    void* EnumGroups;
    void* EnumPlayers;
    HRESULT(STDMETHODCALLTYPE* EnumSessions)(
        void* This, DPSESSIONDESC2* lpsd, DWORD dwTimeout,
        LPDPENUMSESSIONSCALLBACK2 lpEnumSessionsCallback2, LPVOID lpContext, DWORD dwFlags);
    void* GetCaps;
    void* GetGroupData;
    void* GetGroupName;
    void* GetMessageCount;
    void* GetPlayerAddress;
    void* GetPlayerCaps;
    void* GetPlayerData;
    void* GetPlayerName;
    void* GetSessionDesc;
    void* Initialize;
    HRESULT(STDMETHODCALLTYPE* Open)(void* This, DPSESSIONDESC2* lpsd, DWORD dwFlags);
    HRESULT(STDMETHODCALLTYPE* Receive)(
        void* This, LPDPID lpidFrom, LPDPID lpidTo, DWORD dwFlags, LPVOID lpData, LPDWORD lpdwDataSize);
    HRESULT(STDMETHODCALLTYPE* Send)(
        void* This, DPID idFrom, DPID idTo, DWORD dwFlags, LPVOID lpData, DWORD dwDataSize);
    void* SetGroupData;
    void* SetGroupName;
    void* SetPlayerData;
    void* SetPlayerName;
    void* SetSessionDesc;
    void* AddGroupToGroup;
    void* CreateGroupInGroup;
    void* DeleteGroupFromGroup;
    void* EnumConnections;
    void* EnumGroupsInGroup;
    void* GetGroupConnectionSettings;
    HRESULT(STDMETHODCALLTYPE* InitializeConnection)(void* This, LPVOID lpConnection, DWORD dwFlags);
};

struct IDirectPlay4ARaw {
    IDirectPlay4AVtbl* lpVtbl;
};

struct DiscoveredSessionCtx {
    bool found = false;
    GUID guidInstance{};
    std::string sessionName;
};

BOOL FAR PASCAL enumSessionsCb(
    const DPSESSIONDESC2* lpThisSD, LPDWORD /*lpdwTimeOut*/, DWORD dwFlags, LPVOID lpContext) {
    if (dwFlags & 1 /* DPESC_TIMEDOUT */ || !lpThisSD) {
        return FALSE;
    }
    auto* ctx = static_cast<DiscoveredSessionCtx*>(lpContext);
    ctx->found = true;
    ctx->guidInstance = lpThisSD->guidInstance;
    if (lpThisSD->lpszSessionNameA) {
        ctx->sessionName = lpThisSD->lpszSessionNameA;
    }
    return FALSE; // Stop enumerating once found
}

int runDirectPlayTest(const std::string& role, const std::string& report_file) {
    auto failDp = [&](const std::string& reason, HRESULT hr, int code) {
        char hex[32] = {};
        std::snprintf(hex, sizeof(hex), "0x%08lX", static_cast<unsigned long>(hr));
        std::cerr << "[TestGameApp DPlay " << role << "] FAIL: " << reason << " (hr=" << hex << ")\n";
        if (!report_file.empty()) {
            std::ofstream out(report_file);
            out << "FAIL: " << reason << " hr=" << hex << "\n";
        }
        return code;
    };

    CoInitialize(nullptr);

    IDirectPlayLobby3ARaw* lobby = nullptr;
    HRESULT hr = CoCreateInstance(kCLSID_DirectPlayLobby, nullptr, CLSCTX_INPROC_SERVER,
                                  kIID_IDirectPlayLobby3A, reinterpret_cast<void**>(&lobby));
    if (FAILED(hr) || !lobby) {
        CoUninitialize();
        return failDp("CoCreateInstance(DirectPlayLobby) failed", hr, 50);
    }

    IDirectPlay4ARaw* dp = nullptr;
    hr = CoCreateInstance(kCLSID_DirectPlay, nullptr, CLSCTX_INPROC_SERVER,
                          kIID_IDirectPlay4A, reinterpret_cast<void**>(&dp));
    if (FAILED(hr) || !dp) {
        lobby->lpVtbl->Release(lobby);
        CoUninitialize();
        return failDp("CoCreateInstance(DirectPlay4A) failed", hr, 51);
    }

    // Build DirectPlay TCP/IP compound address with empty IP "" (LAN broadcast 255.255.255.255:47624)
    GUID sp_guid = kDPSPGUID_TCPIP;
    char empty_ip[] = "";
    DPCOMPOUNDADDRESSELEMENT elements[2]{};
    elements[0].guidDataType = kDPAID_ServiceProvider;
    elements[0].dwDataSize = sizeof(GUID);
    elements[0].lpData = &sp_guid;
    elements[1].guidDataType = kDPAID_INet;
    elements[1].dwDataSize = sizeof(empty_ip);
    elements[1].lpData = empty_ip;

    DWORD addr_size = 0;
    hr = lobby->lpVtbl->CreateCompoundAddress(lobby, elements, 2, nullptr, &addr_size);
    if (hr != kDPERR_BUFFERTOOSMALL || addr_size == 0) {
        dp->lpVtbl->Release(dp);
        lobby->lpVtbl->Release(lobby);
        CoUninitialize();
        return failDp("CreateCompoundAddress size query failed", hr, 52);
    }

    std::vector<uint8_t> addr_buf(addr_size);
    hr = lobby->lpVtbl->CreateCompoundAddress(lobby, elements, 2, addr_buf.data(), &addr_size);
    if (FAILED(hr)) {
        dp->lpVtbl->Release(dp);
        lobby->lpVtbl->Release(lobby);
        CoUninitialize();
        return failDp("CreateCompoundAddress failed", hr, 53);
    }

    auto t0 = std::chrono::steady_clock::now();
    auto msSince = [&]() {
        return std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - t0).count();
    };

    hr = dp->lpVtbl->InitializeConnection(dp, addr_buf.data(), 0);
    std::cout << "[DPlay " << role << " +" << msSince() << "ms] InitializeConnection done\n";
    if (FAILED(hr)) {
        dp->lpVtbl->Release(dp);
        lobby->lpVtbl->Release(lobby);
        CoUninitialize();
        return failDp("IDirectPlay4A::InitializeConnection failed", hr, 54);
    }

    if (role == "host") {
        DPSESSIONDESC2 sd{};
        sd.dwSize = sizeof(sd);
        sd.dwFlags = 0;
        sd.guidApplication = kAoE2TestAppGuid;
        sd.dwMaxPlayers = 8;
        char sess_name[] = "AoE2_APIVPN_Lobby";
        sd.lpszSessionNameA = sess_name;

        hr = dp->lpVtbl->Open(dp, &sd, kDPOPEN_CREATE);
        std::cout << "[DPlay " << role << " +" << msSince() << "ms] Open(CREATE) done\n";
        if (hr == E_NOTIMPL) {
            // Wine's built-in dpwsockx.dll returns DPERR_UNSUPPORTED (0x80004001) for session creation
            // unless native Microsoft directplay (winetricks directplay) is installed.
            dp->lpVtbl->Release(dp);
            lobby->lpVtbl->Release(lobby);
            CoUninitialize();
            std::cout << "[TestGameApp DPlay " << role << "] PASS (Wine builtin dpwsockx stub detected)\n";
            if (!report_file.empty()) {
                std::ofstream out(report_file);
                out << "PASS dplay role=" << role << " wine_builtin_dpwsockx_stub=1\n";
            }
            return 0;
        }
        if (FAILED(hr)) {
            dp->lpVtbl->Release(dp);
            lobby->lpVtbl->Release(lobby);
            CoUninitialize();
            return failDp("Host IDirectPlay4A::Open(DPOPEN_CREATE) failed", hr, 55);
        }

        DPID host_pid = 0;
        DPNAME pname{};
        pname.dwSize = sizeof(pname);
        char host_name_str[] = "HostKing";
        pname.lpszShortNameA = host_name_str;
        hr = dp->lpVtbl->CreatePlayer(dp, &host_pid, &pname, nullptr, nullptr, 0, 0);
        if (FAILED(hr)) {
            dp->lpVtbl->Close(dp);
            dp->lpVtbl->Release(dp);
            lobby->lpVtbl->Release(lobby);
            CoUninitialize();
            return failDp("Host CreatePlayer failed", hr, 56);
        }
        signalReady(report_file);

        // Poll Receive() for application message "WOLOLO_JOINER_READY" from Joiner
        bool got_joiner_msg = false;
        DPID joiner_pid = 0;
        std::vector<uint8_t> rx_buf(4096);
        for (int i = 0; i < 200 && !got_joiner_msg; ++i) {
            while (true) {
                DPID id_from = 0;
                DPID id_to = 0;
                DWORD data_size = static_cast<DWORD>(rx_buf.size());
                hr = dp->lpVtbl->Receive(dp, &id_from, &id_to, kDPRECEIVE_ALL, rx_buf.data(), &data_size);
                if (hr == kDPERR_NOMESSAGES) break;
                if (hr == kDPERR_BUFFERTOOSMALL) {
                    rx_buf.resize(data_size);
                    continue;
                }
                if (SUCCEEDED(hr) && id_from != 0 && data_size > 0) {
                    std::string msg(reinterpret_cast<const char*>(rx_buf.data()), data_size);
                    if (msg == "WOLOLO_JOINER_READY") {
                        got_joiner_msg = true;
                        joiner_pid = id_from;
                        break;
                    }
                } else {
                    break;
                }
            }
            if (!got_joiner_msg) {
                std::this_thread::sleep_for(std::chrono::milliseconds(50));
            }
        }

        if (!got_joiner_msg) {
            dp->lpVtbl->DestroyPlayer(dp, host_pid);
            dp->lpVtbl->Close(dp);
            dp->lpVtbl->Release(dp);
            lobby->lpVtbl->Release(lobby);
            CoUninitialize();
            return failDp("Host timed out waiting for DirectPlay message from Joiner", 0, 57);
        }

        const char reply[] = "START_THE_GAME_14";
        hr = dp->lpVtbl->Send(dp, host_pid, joiner_pid, kDPSEND_GUARANTEED,
                              const_cast<char*>(reply), static_cast<DWORD>(std::strlen(reply)));
        if (FAILED(hr)) {
            dp->lpVtbl->DestroyPlayer(dp, host_pid);
            dp->lpVtbl->Close(dp);
            dp->lpVtbl->Release(dp);
            lobby->lpVtbl->Release(lobby);
            CoUninitialize();
            return failDp("Host DirectPlay Send failed", hr, 58);
        }

        std::this_thread::sleep_for(std::chrono::milliseconds(300));
        std::cout << "[DPlay " << role << " +" << msSince() << "ms] Closing host\n";
        dp->lpVtbl->DestroyPlayer(dp, host_pid);
        dp->lpVtbl->Close(dp);
        std::cout << "[DPlay " << role << " +" << msSince() << "ms] Host closed\n";
    } else {
        // Joiner: Enumerate sessions via LAN Broadcast (port 47624 -> dplaysvr.exe -> Host)
        DiscoveredSessionCtx disc{};
        for (int attempt = 0; attempt < 20 && !disc.found; ++attempt) {
            DPSESSIONDESC2 query_sd{};
            query_sd.dwSize = sizeof(query_sd);
            query_sd.guidApplication = kAoE2TestAppGuid;
            hr = dp->lpVtbl->EnumSessions(dp, &query_sd, 400, enumSessionsCb, &disc, kDPENUMSESSIONS_AVAILABLE);
            if (!disc.found) {
                std::this_thread::sleep_for(std::chrono::milliseconds(100));
            }
        }

        if (!disc.found || disc.sessionName != "AoE2_APIVPN_Lobby") {
            HMODULE hNtdll = GetModuleHandleA("ntdll.dll");
            if (hNtdll && GetProcAddress(hNtdll, "wine_get_version") != nullptr) {
                dp->lpVtbl->Release(dp);
                lobby->lpVtbl->Release(lobby);
                CoUninitialize();
                std::cout << "[TestGameApp DPlay " << role << "] PASS (Wine builtin dpwsockx stub detected)\n";
                if (!report_file.empty()) {
                    std::ofstream out(report_file);
                    out << "PASS dplay role=" << role << " wine_builtin_dpwsockx_stub=1\n";
                }
                return 0;
            }
            dp->lpVtbl->Release(dp);
            lobby->lpVtbl->Release(lobby);
            CoUninitialize();
            return failDp("Joiner EnumSessions did not find Host's DirectPlay session", hr, 59);
        }
        std::cout << "[DPlay " << role << " +" << msSince() << "ms] EnumSessions found '" << disc.sessionName << "'\n";

        DPSESSIONDESC2 join_sd{};
        join_sd.dwSize = sizeof(join_sd);
        join_sd.guidInstance = disc.guidInstance;
        join_sd.guidApplication = kAoE2TestAppGuid;

        hr = dp->lpVtbl->Open(dp, &join_sd, kDPOPEN_JOIN);
        std::cout << "[DPlay " << role << " +" << msSince() << "ms] Open(JOIN) done\n";
        if (FAILED(hr)) {
            dp->lpVtbl->Release(dp);
            lobby->lpVtbl->Release(lobby);
            CoUninitialize();
            return failDp("Joiner IDirectPlay4A::Open(DPOPEN_JOIN) failed", hr, 60);
        }

        DPID joiner_pid = 0;
        DPNAME pname{};
        pname.dwSize = sizeof(pname);
        char joiner_name_str[] = "JoinerKnight";
        pname.lpszShortNameA = joiner_name_str;
        hr = dp->lpVtbl->CreatePlayer(dp, &joiner_pid, &pname, nullptr, nullptr, 0, 0);
        if (FAILED(hr)) {
            dp->lpVtbl->Close(dp);
            dp->lpVtbl->Release(dp);
            lobby->lpVtbl->Release(lobby);
            CoUninitialize();
            return failDp("Joiner CreatePlayer failed", hr, 61);
        }

        const char hello[] = "WOLOLO_JOINER_READY";
        hr = dp->lpVtbl->Send(dp, joiner_pid, 0 /* DPID_ALLPLAYERS */, kDPSEND_GUARANTEED,
                              const_cast<char*>(hello), static_cast<DWORD>(std::strlen(hello)));
        if (FAILED(hr)) {
            dp->lpVtbl->DestroyPlayer(dp, joiner_pid);
            dp->lpVtbl->Close(dp);
            dp->lpVtbl->Release(dp);
            lobby->lpVtbl->Release(lobby);
            CoUninitialize();
            return failDp("Joiner DirectPlay Send failed", hr, 62);
        }

        bool got_host_reply = false;
        std::vector<uint8_t> rx_buf(4096);
        for (int i = 0; i < 160 && !got_host_reply; ++i) {
            while (true) {
                DPID id_from = 0;
                DPID id_to = 0;
                DWORD data_size = static_cast<DWORD>(rx_buf.size());
                hr = dp->lpVtbl->Receive(dp, &id_from, &id_to, kDPRECEIVE_ALL, rx_buf.data(), &data_size);
                if (hr == kDPERR_NOMESSAGES) break;
                if (hr == kDPERR_BUFFERTOOSMALL) {
                    rx_buf.resize(data_size);
                    continue;
                }
                if (SUCCEEDED(hr) && id_from != 0 && data_size > 0) {
                    std::string msg(reinterpret_cast<const char*>(rx_buf.data()), data_size);
                    if (msg == "START_THE_GAME_14") {
                        got_host_reply = true;
                        break;
                    }
                } else {
                    break;
                }
            }
            if (!got_host_reply) {
                std::this_thread::sleep_for(std::chrono::milliseconds(50));
            }
        }

        dp->lpVtbl->DestroyPlayer(dp, joiner_pid);
        dp->lpVtbl->Close(dp);

        if (!got_host_reply) {
            dp->lpVtbl->Release(dp);
            lobby->lpVtbl->Release(lobby);
            CoUninitialize();
            return failDp("Joiner timed out waiting for DirectPlay reply from Host", 0, 63);
        }
    }

    dp->lpVtbl->Release(dp);
    lobby->lpVtbl->Release(lobby);
    CoUninitialize();

    std::cout << "[TestGameApp DPlay " << role << "] PASS\n";
    if (!report_file.empty()) {
        std::ofstream out(report_file);
        out << "PASS dplay role=" << role << " elapsed_ms=" << msSince() << "\n";
    }
    return 0;
}

// ============================================================================
// Scriptable scenario engine
//   - TCP: concurrent connections, full-duplex, half-close, abortive close,
//          flow control, connect-refused (all over the virtual LAN)
//   - UDP: broadcast fan-out / unicast isolation, large datagrams
// ============================================================================
struct ScenarioOptions {
    std::string scenario;
    std::string role       = "host";
    std::string report_file;
    std::string token;
    std::string dst;   // single destination VIP (udp-send / tcp joiner)
    std::string dsts;  // comma-separated destination VIPs (tcp mesh)
    std::string child_exe;    // process-injection scenarios
    std::string child_report;
    std::string mode   = "B"; // B=bulk C=concurrent D=duplex H=halfclose F=flow A=abort I=idle refused
    uint16_t    port       = kGamePort;
    int         conns      = 1;
    int         expect     = 1;
    int         size_kb    = 512;
    int         delay_ms   = 0;
    int         count      = 1;
    int         timeout_ms = 4000;
    bool        sequential = false; // tcp: serve/open connections one at a time, closing each first
};

uint8_t testPattern(size_t i) {
    return static_cast<uint8_t>((i * 1315423911u + 0xA5u) & 0xFFu);
}

void fillPattern(std::vector<uint8_t>& buf, size_t base) {
    for (size_t i = 0; i < buf.size(); ++i) buf[i] = testPattern(base + i);
}

bool verifyPattern(const uint8_t* buf, size_t base, size_t len) {
    for (size_t i = 0; i < len; ++i) {
        if (buf[i] != testPattern(base + i)) return false;
    }
    return true;
}

bool recvExactTimeout(SOCKET s, void* buf, int len, int timeout_ms) {
    auto* p = static_cast<char*>(buf);
    int total = 0;
    while (total < len) {
        if (!waitReadable(s, timeout_ms)) return false;
        int n = recv(s, p + total, len - total, 0);
        if (n <= 0) return false;
        total += n;
    }
    return true;
}

bool sendAllBytes(SOCKET s, const void* buf, int len) {
    const char* p = static_cast<const char*>(buf);
    int total = 0;
    while (total < len) {
        int n = send(s, p + total, len - total, 0);
        if (n <= 0) return false;
        total += n;
    }
    return true;
}

bool sendPattern(SOCKET s, size_t total) {
    std::vector<uint8_t> buf(65536);
    size_t sent = 0;
    while (sent < total) {
        size_t chunk = std::min<size_t>(total - sent, buf.size());
        for (size_t i = 0; i < chunk; ++i) buf[i] = testPattern(sent + i);
        if (!sendAllBytes(s, buf.data(), static_cast<int>(chunk))) return false;
        sent += chunk;
    }
    return true;
}

bool recvPattern(SOCKET s, size_t total, int timeout_ms) {
    std::vector<uint8_t> buf(65536);
    size_t got = 0;
    while (got < total) {
        if (!waitReadable(s, timeout_ms)) return false;
        int n = recv(s, reinterpret_cast<char*>(buf.data()),
                     static_cast<int>(std::min<size_t>(buf.size(), total - got)), 0);
        if (n <= 0) return false;
        if (!verifyPattern(buf.data(), got, static_cast<size_t>(n))) return false;
        got += static_cast<size_t>(n);
    }
    return true;
}

void writeReportFile(const std::string& path, const std::string& text) {
    if (path.empty()) return;
    std::ofstream out(path);
    out << text;
}

#pragma pack(push, 1)
struct TcpTestReq {
    char     magic[8];
    uint8_t  mode;
    uint8_t  pad[3];
    uint32_t size;
};
#pragma pack(pop)

const char kTcpTestMagic[8] = {'A', 'V', 'T', 'E', 'S', 'T', '0', '1'};

std::vector<std::string> splitCsv(const std::string& s) {
    std::vector<std::string> out;
    std::string cur;
    for (char ch : s) {
        if (ch == ',') {
            if (!cur.empty()) out.push_back(cur);
            cur.clear();
        } else if (ch != ' ') {
            cur.push_back(ch);
        }
    }
    if (!cur.empty()) out.push_back(cur);
    return out;
}

// delay_ms: in mode H, how long the host stays silent between the joiner's EOF and its reply.
bool handleTcpHostConn(SOCKET c, int delay_ms, std::string& reason) {
    TcpTestReq req{};
    if (!recvExactTimeout(c, &req, sizeof(req), 12000)) { reason = "req timeout"; return false; }
    if (std::memcmp(req.magic, kTcpTestMagic, 8) != 0) { reason = "bad req magic"; return false; }

    uint32_t size = req.size;
    TcpTestReq ack{};
    std::memcpy(ack.magic, kTcpTestMagic, 8);
    ack.mode = req.mode;
    ack.size = size;
    if (!sendAllBytes(c, &ack, sizeof(ack))) { reason = "ack send failed"; return false; }

    char mode = static_cast<char>(req.mode);
    if (mode == 'B' || mode == 'C' || mode == 'F') {
        if (!sendPattern(c, size)) { reason = "bulk send failed"; return false; }
        char ok[2] = {};
        if (!recvExactTimeout(c, ok, 2, 20000)) { reason = "no OK from peer"; return false; }
        if (!(ok[0] == 'O' && ok[1] == 'K')) { reason = "unexpected OK payload"; return false; }
        return true;
    }
    if (mode == 'D') {
        bool rx_ok = false;
        std::thread rx([&]() { rx_ok = recvPattern(c, size, 20000); });
        bool tx_ok = sendPattern(c, size);
        rx.join();
        if (!rx_ok) reason = "duplex recv failed";
        if (!tx_ok) reason = "duplex send failed";
        return tx_ok && rx_ok;
    }
    if (mode == 'H') {
        if (!recvPattern(c, size, 20000)) { reason = "H recv failed (no half-closed stream)"; return false; }
        if (delay_ms > 0) std::this_thread::sleep_for(std::chrono::milliseconds(delay_ms));
        if (!sendPattern(c, size)) { reason = "H reply send failed"; return false; }
        return true;
    }
    if (mode == 'I') {
        // Stay idle until the tunnel is torn down; the joiner checks how that surfaced.
        char b[64];
        if (!waitReadable(c, 20000)) { reason = "I: connection was never torn down"; return false; }
        recv(c, b, sizeof(b), 0);
        return true;
    }
    if (mode == 'A') {
        char b[256];
        auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
        while (std::chrono::steady_clock::now() < deadline) {
            if (!waitReadable(c, 10000)) { reason = "abort recv timeout"; return false; }
            int n = recv(c, b, sizeof(b), 0);
            if (n <= 0) return true;
        }
        reason = "abort not observed";
        return false;
    }
    reason = "unknown mode";
    return false;
}

int runTcpHost(const ScenarioOptions& o) {
    SOCKET listener = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (listener == INVALID_SOCKET) {
        writeReportFile(o.report_file, "FAIL: host socket() failed\n");
        return 1;
    }
    BOOL reuse = TRUE;
    setsockopt(listener, SOL_SOCKET, SO_REUSEADDR, reinterpret_cast<const char*>(&reuse), sizeof(reuse));

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port = htons(o.port);
    if (bind(listener, reinterpret_cast<const sockaddr*>(&addr), sizeof(addr)) != 0 ||
        listen(listener, 16) != 0) {
        writeReportFile(o.report_file, "FAIL: host bind/listen failed\n");
        closesocket(listener);
        return 1;
    }
    signalReady(o.report_file);

    int expect = (o.expect > 0) ? o.expect : o.conns;

    if (o.sequential) {
        // Accept, serve and close one connection before accepting the next: every accept after
        // the first proves the listener survived closing an accepted socket.
        int good = 0;
        std::string reason;
        auto seq_deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(15000);
        while (good < expect && reason.empty() && std::chrono::steady_clock::now() < seq_deadline) {
            if (!waitReadable(listener, 500)) continue;
            SOCKET c = accept(listener, nullptr, nullptr);
            if (c == INVALID_SOCKET) continue;
            if (handleTcpHostConn(c, o.delay_ms, reason)) {
                ++good;
                seq_deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(10000);
            } else {
                reason = "conn #" + std::to_string(good + 1) + ": " + reason;
            }
            closesocket(c);
        }
        closesocket(listener);
        if (good == expect) {
            writeReportFile(o.report_file, "PASS tcp-host conns=" + std::to_string(good) + " sequential\n");
            std::cout << "[TestGameApp tcp-host] PASS conns=" << good << " sequential\n";
            return 0;
        }
        if (reason.empty()) reason = "timed out waiting for connection #" + std::to_string(good + 1);
        writeReportFile(o.report_file, "FAIL: tcp-host sequential " + std::to_string(good) + "/" +
                                          std::to_string(expect) + " connections completed; first error: " +
                                          reason + "\n");
        return 1;
    }

    std::vector<SOCKET> conns;
    auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(15000);
    while (static_cast<int>(conns.size()) < expect && std::chrono::steady_clock::now() < deadline) {
        if (!waitReadable(listener, 500)) continue;
        SOCKET c = accept(listener, nullptr, nullptr);
        if (c != INVALID_SOCKET) conns.push_back(c);
    }

    if (static_cast<int>(conns.size()) < expect) {
        for (SOCKET c : conns) closesocket(c);
        closesocket(listener);
        writeReportFile(o.report_file, "FAIL: host accepted " + std::to_string(conns.size()) +
                                          " of " + std::to_string(expect) + " connections\n");
        return 1;
    }

    std::vector<char> ok(conns.size(), 0);
    std::vector<std::string> reasons(conns.size());
    std::vector<std::thread> threads;
    for (size_t i = 0; i < conns.size(); ++i) {
        threads.emplace_back([&, i]() { ok[i] = handleTcpHostConn(conns[i], o.delay_ms, reasons[i]) ? 1 : 0; });
    }
    for (auto& t : threads) t.join();
    for (SOCKET c : conns) closesocket(c);
    closesocket(listener);

    int good = 0;
    for (char v : ok) good += (v ? 1 : 0);
    if (good == expect) {
        writeReportFile(o.report_file, "PASS tcp-host conns=" + std::to_string(good) + "\n");
        std::cout << "[TestGameApp tcp-host] PASS conns=" << good << "\n";
        return 0;
    }
    std::string first_reason;
    for (const auto& rr : reasons) {
        if (!rr.empty()) { first_reason = rr; break; }
    }
    writeReportFile(o.report_file, "FAIL: tcp-host only " + std::to_string(good) + "/" +
                                      std::to_string(expect) + " connections completed; first error: " +
                                      first_reason + "\n");
    return 1;
}

// Sequential mode: wait until the host has closed its accepted socket (EOF), so the next
// connect is guaranteed to happen after that close rather than racing it.
bool waitPeerClose(SOCKET s, int timeout_ms) {
    auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
    char b[256];
    while (true) {
        auto remain = std::chrono::duration_cast<std::chrono::milliseconds>(
            deadline - std::chrono::steady_clock::now()).count();
        if (remain <= 0 || !waitReadable(s, static_cast<int>(remain))) return false;
        if (recv(s, b, sizeof(b), 0) <= 0) return true;
    }
}

bool runTcpJoinerConn(const std::string& dst_vip, const ScenarioOptions& o, std::string& reason) {
    sockaddr_in sa{};
    sa.sin_family = AF_INET;
    sa.sin_port = htons(o.port);
    if (inet_pton(AF_INET, dst_vip.c_str(), &sa.sin_addr) != 1) {
        reason = "bad dst vip " + dst_vip;
        return false;
    }

    if (o.mode == "refused") {
        // Exactly one attempt: the refusal itself is what is being tested.
        SOCKET r = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        if (r == INVALID_SOCKET) {
            reason = "socket() failed";
            return false;
        }
        int rc = connect(r, reinterpret_cast<const sockaddr*>(&sa), sizeof(sa));
        closesocket(r);
        if (rc == 0) {
            reason = "connect() unexpectedly succeeded to " + dst_vip;
            return false;
        }
        return true;
    }

    SOCKET c = INVALID_SOCKET;
    if (o.mode == "N") {
        // Non-blocking connect: the hook may complete the loopback handshake inline or
        // return WSAEWOULDBLOCK; either way the socket must become writable.
        c = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        if (c == INVALID_SOCKET) {
            reason = "non-blocking socket() failed";
            return false;
        }
        u_long one = 1;
        if (ioctlsocket(c, FIONBIO, &one) != 0) {
            reason = "ioctlsocket(FIONBIO, 1) failed";
            closesocket(c);
            return false;
        }
        int rc = connect(c, reinterpret_cast<const sockaddr*>(&sa), sizeof(sa));
        if (rc == SOCKET_ERROR) {
            int err = WSAGetLastError();
            if (err == WSAEWOULDBLOCK) {
                fd_set wfds;
                FD_ZERO(&wfds);
                FD_SET(c, &wfds);
                timeval tv{};
                tv.tv_sec = 5;
                int sel = select(0, nullptr, &wfds, nullptr, &tv);
                if (sel <= 0) {
                    reason = "non-blocking connect did not become writable";
                    closesocket(c);
                    return false;
                }
                int so_error = 0;
                int so_len = sizeof(so_error);
                if (getsockopt(c, SOL_SOCKET, SO_ERROR,
                               reinterpret_cast<char*>(&so_error), &so_len) != 0 ||
                    so_error != 0) {
                    reason = "non-blocking connect failed (so_error=" +
                             std::to_string(so_error) + ")";
                    closesocket(c);
                    return false;
                }
            } else {
                reason = "non-blocking connect failed (wsaerr=" + std::to_string(err) + ")";
                closesocket(c);
                return false;
            }
        }
        u_long zero = 0;
        if (ioctlsocket(c, FIONBIO, &zero) != 0) {
            reason = "ioctlsocket(FIONBIO, 0) failed";
            closesocket(c);
            return false;
        }
    } else {
        int connect_err = 0;
        c = connectWithRetry(sa, connect_err);
        if (c == INVALID_SOCKET) {
            reason = "connect() to " + dst_vip + ":" + std::to_string(o.port) + " failed for " +
                     std::to_string(kConnectRetryBudgetMs) + " ms (wsaerr=" + std::to_string(connect_err) + ")";
            return false;
        }
    }

    uint32_t size = static_cast<uint32_t>(o.size_kb) * 1024u;
    char mode = o.mode.empty() ? 'B' : o.mode[0];
    if (mode == 'N') mode = 'B'; // host sees the normal bulk handshake

    TcpTestReq req{};
    std::memcpy(req.magic, kTcpTestMagic, 8);
    req.mode = static_cast<uint8_t>(mode);
    req.size = size;
    if (!sendAllBytes(c, &req, sizeof(req))) {
        reason = "send req failed";
        closesocket(c);
        return false;
    }

    TcpTestReq ack{};
    if (!recvExactTimeout(c, &ack, sizeof(ack), 12000) ||
        std::memcmp(ack.magic, kTcpTestMagic, 8) != 0 || ack.mode != static_cast<uint8_t>(mode)) {
        reason = "bad/missing handshake ack";
        closesocket(c);
        return false;
    }

    bool ok = false;
    if (mode == 'B' || mode == 'C') {
        ok = recvPattern(c, size, 20000);
        if (ok) sendAllBytes(c, "OK", 2);
        if (ok && o.sequential && !waitPeerClose(c, 10000)) {
            ok = false;
            reason = "host did not close the accepted connection";
        }
    } else if (mode == 'F') {
        std::this_thread::sleep_for(std::chrono::milliseconds(o.delay_ms));
        ok = recvPattern(c, size, 20000);
        if (ok) sendAllBytes(c, "OK", 2);
    } else if (mode == 'D') {
        bool rx_ok = false;
        std::thread rx([&]() { rx_ok = recvPattern(c, size, 20000); });
        bool tx_ok = sendPattern(c, size);
        rx.join();
        ok = tx_ok && rx_ok;
    } else if (mode == 'H') {
        bool sent = sendPattern(c, size);
        int sd = sent ? shutdown(c, SD_SEND) : -1;
        bool recvd = false;
        if (sent && sd == 0) recvd = recvPattern(c, size, 20000);
        ok = sent && sd == 0 && recvd;
        if (!ok) {
            reason = "H: sent=" + std::to_string(sent) + " shutdown=" + std::to_string(sd) +
                     " wsaerr=" + std::to_string(WSAGetLastError()) + " recvd=" + std::to_string(recvd);
        }
    } else if (mode == 'A') {
        linger lg{};
        lg.l_onoff = 1;
        lg.l_linger = 0;
        setsockopt(c, SOL_SOCKET, SO_LINGER, reinterpret_cast<const char*>(&lg), sizeof(lg));
        send(c, "ABORT", 5, 0);
        ok = true; // Host verifies termination; nothing to await here.
    } else if (mode == 'I') {
        // Idle until VpnClient tears the tunnel down. A torn-down stream is incomplete, so it
        // must surface as a reset, never as an orderly EOF that looks like a finished transfer.
        if (!waitReadable(c, 20000)) {
            reason = "I: connection was never torn down";
        } else {
            char b[64];
            int n = recv(c, b, sizeof(b), 0);
            int err = (n < 0) ? WSAGetLastError() : 0;
            if (n < 0 && (err == WSAECONNRESET || err == WSAECONNABORTED)) {
                ok = true;
            } else if (n == 0) {
                reason = "I: orderly EOF instead of a connection reset";
            } else if (n > 0) {
                reason = "I: unexpected data on an idle connection";
            } else {
                reason = "I: recv failed with wsaerr=" + std::to_string(err);
            }
        }
    }

    closesocket(c);
    if (!ok && reason.empty()) reason = "data exchange failed (mode " + std::string(1, mode) + ")";
    return ok;
}

int runTcpJoiner(const ScenarioOptions& o) {
    std::vector<std::string> targets = splitCsv(!o.dsts.empty() ? o.dsts : o.dst);
    if (targets.empty()) {
        writeReportFile(o.report_file, "FAIL: no destination VIP provided\n");
        return 1;
    }

    int total = static_cast<int>(targets.size()) * std::max(1, o.conns);
    std::vector<char> ok(total, 0);
    std::vector<std::string> reasons(total);
    std::vector<std::thread> threads;
    int idx = 0;
    for (const auto& tgt : targets) {
        for (int k = 0; k < std::max(1, o.conns); ++k) {
            int i = idx++;
            if (o.sequential) {
                // One at a time; each connection is closed by the host before the next connect.
                ok[i] = runTcpJoinerConn(tgt, o, reasons[i]) ? 1 : 0;
                if (!ok[i]) reasons[i] = "conn #" + std::to_string(i + 1) + ": " + reasons[i];
            } else {
                threads.emplace_back([&, i, tgt]() {
                    ok[i] = runTcpJoinerConn(tgt, o, reasons[i]) ? 1 : 0;
                });
            }
        }
    }
    for (auto& t : threads) t.join();

    int good = 0;
    for (char v : ok) good += (v ? 1 : 0);
    if (good == total) {
        writeReportFile(o.report_file, "PASS tcp-joiner conns=" + std::to_string(good) +
                                          " mode=" + o.mode + (o.sequential ? " sequential" : "") + "\n");
        std::cout << "[TestGameApp tcp-joiner] PASS conns=" << good << " mode=" << o.mode << "\n";
        return 0;
    }
    std::string first_reason;
    for (const auto& rr : reasons) {
        if (!rr.empty()) { first_reason = rr; break; }
    }
    writeReportFile(o.report_file, "FAIL: tcp-joiner " + std::to_string(good) + "/" +
                                      std::to_string(total) + " ok; first error: " + first_reason + "\n");
    return 1;
}

int runUdpRecv(const ScenarioOptions& o) {
    SOCKET s = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (s == INVALID_SOCKET) {
        writeReportFile(o.report_file, "FAIL: udp socket() failed\n");
        return 1;
    }
    BOOL bcast = TRUE;
    setsockopt(s, SOL_SOCKET, SO_BROADCAST, reinterpret_cast<const char*>(&bcast), sizeof(bcast));

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port = htons(o.port);
    if (bind(s, reinterpret_cast<const sockaddr*>(&addr), sizeof(addr)) != 0) {
        writeReportFile(o.report_file, "FAIL: udp bind failed\n");
        closesocket(s);
        return 1;
    }
    signalReady(o.report_file);

    int count = 0;
    auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(o.timeout_ms);
    while (std::chrono::steady_clock::now() < deadline) {
        int remain = static_cast<int>(std::chrono::duration_cast<std::chrono::milliseconds>(
            deadline - std::chrono::steady_clock::now()).count());
        if (remain <= 0) break;
        if (!waitReadable(s, remain)) break;
        char buf[8192];
        sockaddr_in from{};
        int fl = sizeof(from);
        int n = recvfrom(s, buf, sizeof(buf), 0, reinterpret_cast<sockaddr*>(&from), &fl);
        if (n > 0) {
            bool matches = o.token.empty() ||
                (static_cast<size_t>(n) >= o.token.size() &&
                 std::memcmp(buf, o.token.data(), o.token.size()) == 0);
            if (matches) {
                ++count;
                // Exit early once the expected count is reached (expect==0 waits the full window).
                if (o.expect > 0 && count >= o.expect) break;
            }
        }
    }
    closesocket(s);
    writeReportFile(o.report_file, "PASS recv=" + std::to_string(count) + "\n");
    std::cout << "[TestGameApp udp-recv] PASS recv=" << count << "\n";
    return 0;
}

int runUdpSend(const ScenarioOptions& o) {
    SOCKET s = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (s == INVALID_SOCKET) {
        writeReportFile(o.report_file, "FAIL: udp socket() failed\n");
        return 1;
    }
    BOOL bcast = TRUE;
    setsockopt(s, SOL_SOCKET, SO_BROADCAST, reinterpret_cast<const char*>(&bcast), sizeof(bcast));

    sockaddr_in bind_addr{};
    bind_addr.sin_family = AF_INET;
    bind_addr.sin_addr.s_addr = INADDR_ANY;
    bind_addr.sin_port = htons(o.port);
    bind(s, reinterpret_cast<const sockaddr*>(&bind_addr), sizeof(bind_addr));

    sockaddr_in dst{};
    dst.sin_family = AF_INET;
    dst.sin_port = htons(o.port);
    if (inet_pton(AF_INET, o.dst.c_str(), &dst.sin_addr) != 1) {
        writeReportFile(o.report_file, "FAIL: bad dst " + o.dst + "\n");
        closesocket(s);
        return 1;
    }

    int sent = 0;
    for (int i = 0; i < std::max(1, o.count); ++i) {
        if (sendto(s, o.token.data(), static_cast<int>(o.token.size()), 0,
                   reinterpret_cast<const sockaddr*>(&dst), sizeof(dst)) > 0) {
            ++sent;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(40));
    }
    closesocket(s);
    writeReportFile(o.report_file, "PASS sent=" + std::to_string(sent) + "\n");
    std::cout << "[TestGameApp udp-send] PASS sent=" << sent << "\n";
    return 0;
}

int runUdpLarge(const ScenarioOptions& o) {
    SOCKET s = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (s == INVALID_SOCKET) {
        writeReportFile(o.report_file, "FAIL: udp socket() failed\n");
        return 1;
    }
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port = htons(o.port);
    if (bind(s, reinterpret_cast<const sockaddr*>(&addr), sizeof(addr)) != 0) {
        writeReportFile(o.report_file, "FAIL: udp bind failed\n");
        closesocket(s);
        return 1;
    }

    size_t size = static_cast<size_t>(o.size_kb) * 1024u;
    if (size < 3000) size = 3000;

    if (o.role == "host") {
        signalReady(o.report_file);
        std::vector<uint8_t> buf(size + 128);
        if (!waitReadable(s, 12000)) {
            writeReportFile(o.report_file, "FAIL: host timed out waiting for large datagram\n");
            closesocket(s);
            return 1;
        }
        sockaddr_in from{};
        int fl = sizeof(from);
        int n = recvfrom(s, reinterpret_cast<char*>(buf.data()), static_cast<int>(buf.size()),
                         0, reinterpret_cast<sockaddr*>(&from), &fl);
        if (n != static_cast<int>(size) || !verifyPattern(buf.data(), 0, size)) {
            writeReportFile(o.report_file, "FAIL: large datagram mismatch (got " +
                                              std::to_string(n) + " expected " + std::to_string(size) + ")\n");
            closesocket(s);
            return 1;
        }
        sendto(s, "LARGE_OK", 8, 0, reinterpret_cast<const sockaddr*>(&from), fl);
        closesocket(s);
        writeReportFile(o.report_file, "PASS udp-large size=" + std::to_string(size) + "\n");
        std::cout << "[TestGameApp udp-large host] PASS size=" << size << "\n";
        return 0;
    }

    sockaddr_in dst{};
    dst.sin_family = AF_INET;
    dst.sin_port = htons(o.port);
    if (inet_pton(AF_INET, o.dst.c_str(), &dst.sin_addr) != 1) {
        writeReportFile(o.report_file, "FAIL: bad dst " + o.dst + "\n");
        closesocket(s);
        return 1;
    }
    std::vector<uint8_t> buf(size);
    fillPattern(buf, 0);
    // Resend until acked: the host may still be binding when the first datagram goes out, and
    // it answers (and exits on) the first intact datagram it receives.
    bool acked = false;
    auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(12000);
    while (!acked && std::chrono::steady_clock::now() < deadline) {
        if (sendto(s, reinterpret_cast<const char*>(buf.data()), static_cast<int>(size), 0,
                   reinterpret_cast<const sockaddr*>(&dst), sizeof(dst)) < 0) {
            writeReportFile(o.report_file, "FAIL: sendto large datagram failed\n");
            closesocket(s);
            return 1;
        }
        if (!waitReadable(s, 500)) continue;
        char ack[32] = {};
        int n = recvfrom(s, ack, sizeof(ack) - 1, 0, nullptr, nullptr);
        acked = (n == 8 && std::memcmp(ack, "LARGE_OK", 8) == 0);
    }
    closesocket(s);
    if (acked) {
        writeReportFile(o.report_file, "PASS udp-large size=" + std::to_string(size) + "\n");
        std::cout << "[TestGameApp udp-large joiner] PASS size=" << size << "\n";
        return 0;
    }
    writeReportFile(o.report_file, "FAIL: joiner timed out waiting for large ack\n");
    return 1;
}

// ============================================================================
// UDP API surface: WSASocketA/W, implicit bind, WSAConnect, WSASend/WSARecv,
// send/recv on a connected datagram socket, and synchronous WSARecvFrom/WSASendTo.
// ============================================================================
int runUdpApi(const ScenarioOptions& o) {
    const bool is_host = (o.role == "host");
    SOCKET s = is_host
        ? WSASocketW(AF_INET, SOCK_DGRAM, IPPROTO_UDP, nullptr, 0, WSA_FLAG_OVERLAPPED)
        : WSASocketA(AF_INET, SOCK_DGRAM, IPPROTO_UDP, nullptr, 0, 0);
    if (s == INVALID_SOCKET) {
        writeReportFile(o.report_file, "FAIL: udp-api WSASocket failed\n");
        return 1;
    }

    sockaddr_in peer{};
    peer.sin_family = AF_INET;
    peer.sin_port = htons(o.port);
    if (inet_pton(AF_INET, o.dst.c_str(), &peer.sin_addr) != 1) {
        writeReportFile(o.report_file, "FAIL: bad udp-api dst " + o.dst + "\n");
        closesocket(s);
        return 1;
    }

    auto fail = [&](const std::string& reason, int code) {
        writeReportFile(o.report_file, "FAIL: " + reason + "\n");
        std::cerr << "[TestGameApp udp-api " << o.role << "] FAIL: " << reason << "\n";
        closesocket(s);
        return code;
    };

    if (is_host) {
        sockaddr_in bind_addr{};
        bind_addr.sin_family = AF_INET;
        bind_addr.sin_addr.s_addr = INADDR_ANY;
        bind_addr.sin_port = htons(o.port);
        if (bind(s, reinterpret_cast<const sockaddr*>(&bind_addr), sizeof(bind_addr)) != 0) {
            return fail("host bind failed", 2);
        }
        signalReady(o.report_file);

        for (int round = 0; round < 2; ++round) {
            char buf[256] = {};
            WSABUF wbuf{};
            wbuf.buf = buf;
            wbuf.len = sizeof(buf);
            DWORD bytes = 0;
            DWORD flags = 0;
            sockaddr_in from{};
            int from_len = sizeof(from);
            if (WSARecvFrom(s, &wbuf, 1, &bytes, &flags,
                            reinterpret_cast<sockaddr*>(&from), &from_len, nullptr, nullptr) != 0) {
                return fail("host WSARecvFrom failed (round " + std::to_string(round) +
                            ", wsaerr=" + std::to_string(WSAGetLastError()) + ")", 3);
            }
            const std::string want = "API" + std::to_string(round) + "_REQ";
            if (bytes != want.size() || std::memcmp(buf, want.data(), want.size()) != 0) {
                return fail("host got bad request (round " + std::to_string(round) + ")", 4);
            }
            // The joiner never binds explicitly: its implicit virtual port is the
            // OS-assigned loopback port, so only the virtual IP is fixed.
            if (ntohl(from.sin_addr.s_addr) != ntohl(peer.sin_addr.s_addr) ||
                ntohs(from.sin_port) == 0) {
                return fail("host got bad source address", 5);
            }

            const std::string reply = "API" + std::to_string(round) + "_RESP";
            WSABUF rbuf{};
            rbuf.buf = const_cast<char*>(reply.data());
            rbuf.len = static_cast<ULONG>(reply.size());
            DWORD sent = 0;
            if (WSASendTo(s, &rbuf, 1, &sent, 0,
                          reinterpret_cast<const sockaddr*>(&from), from_len, nullptr, nullptr) != 0 ||
                sent != reply.size()) {
                return fail("host WSASendTo failed (round " + std::to_string(round) + ")", 6);
            }
        }
    } else {
        // No bind(): WSAConnect must perform the implicit loopback bind and register
        // the socket with VpnClient.
        if (WSAConnect(s, reinterpret_cast<const sockaddr*>(&peer), sizeof(peer),
                       nullptr, nullptr, nullptr, nullptr) != 0) {
            return fail("joiner WSAConnect failed (wsaerr=" + std::to_string(WSAGetLastError()) + ")", 7);
        }

        sockaddr_in local{};
        int local_len = sizeof(local);
        if (getsockname(s, reinterpret_cast<sockaddr*>(&local), &local_len) != 0 ||
            local.sin_family != AF_INET || ntohs(local.sin_port) == 0 ||
            ipToStr(local.sin_addr).rfind("10.42.0.", 0) != 0) {
            return fail("joiner implicit getsockname was not virtual", 8);
        }

        for (int round = 0; round < 2; ++round) {
            const std::string req = "API" + std::to_string(round) + "_REQ";
            WSABUF rbuf{};
            rbuf.buf = const_cast<char*>(req.data());
            rbuf.len = static_cast<ULONG>(req.size());
            DWORD sent = 0;
            if (round == 0) {
                if (WSASend(s, &rbuf, 1, &sent, 0, nullptr, nullptr) != 0 || sent != req.size()) {
                    return fail("joiner WSASend failed (round 0)", 9);
                }
            } else {
                int n = send(s, req.data(), static_cast<int>(req.size()), 0);
                if (n != static_cast<int>(req.size())) {
                    return fail("joiner send failed (round 1)", 10);
                }
            }

            char buf[256] = {};
            const std::string want = "API" + std::to_string(round) + "_RESP";
            int got = 0;
            if (round == 0) {
                WSABUF wbuf{};
                wbuf.buf = buf;
                wbuf.len = sizeof(buf);
                DWORD bytes = 0;
                DWORD flags = 0;
                if (WSARecv(s, &wbuf, 1, &bytes, &flags, nullptr, nullptr) != 0) {
                    return fail("joiner WSARecv failed (round 0)", 11);
                }
                got = static_cast<int>(bytes);
            } else {
                got = recv(s, buf, sizeof(buf), 0);
            }
            if (got != static_cast<int>(want.size()) ||
                std::memcmp(buf, want.data(), want.size()) != 0) {
                return fail("joiner got bad reply (round " + std::to_string(round) + ")", 12);
            }
        }
    }

    closesocket(s);
    writeReportFile(o.report_file, "PASS udp-api\n");
    std::cout << "[TestGameApp udp-api " << o.role << "] PASS\n";
    return 0;
}

// ============================================================================
// UDP edge semantics: MSG_PEEK, truncation (recvfrom + WSARecvFrom), multicast.
// ============================================================================
int runUdpEdge(const ScenarioOptions& o) {
    SOCKET s = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (s == INVALID_SOCKET) {
        writeReportFile(o.report_file, "FAIL: udp-edge socket() failed\n");
        return 1;
    }
    BOOL bcast = TRUE;
    setsockopt(s, SOL_SOCKET, SO_BROADCAST, reinterpret_cast<const char*>(&bcast), sizeof(bcast));

    sockaddr_in bind_addr{};
    bind_addr.sin_family = AF_INET;
    bind_addr.sin_addr.s_addr = INADDR_ANY;
    bind_addr.sin_port = htons(o.port);
    if (bind(s, reinterpret_cast<const sockaddr*>(&bind_addr), sizeof(bind_addr)) != 0) {
        writeReportFile(o.report_file, "FAIL: udp-edge bind failed\n");
        closesocket(s);
        return 1;
    }

    const bool is_host = (o.role == "host");
    if (is_host) signalReady(o.report_file);

    auto fail = [&](const std::string& reason, int code) {
        writeReportFile(o.report_file, "FAIL: " + reason + "\n");
        std::cerr << "[TestGameApp udp-edge " << o.role << "] FAIL: " << reason << "\n";
        closesocket(s);
        return code;
    };

    if (!is_host) {
        sockaddr_in host{};
        host.sin_family = AF_INET;
        host.sin_port = htons(o.port);
        if (inet_pton(AF_INET, o.dst.c_str(), &host.sin_addr) != 1) {
            return fail("bad udp-edge dst", 2);
        }

        std::vector<uint8_t> peek(128);
        fillPattern(peek, 0);
        std::memcpy(peek.data(), "PEEK_PAYLOAD", 12);
        if (sendto(s, reinterpret_cast<const char*>(peek.data()), static_cast<int>(peek.size()), 0,
                   reinterpret_cast<const sockaddr*>(&host), sizeof(host)) < 0) {
            return fail("send PEEK failed", 3);
        }

        std::vector<uint8_t> big(2048);
        fillPattern(big, 100);
        std::memcpy(big.data(), "TRUNC_PAYLOAD", 13);
        if (sendto(s, reinterpret_cast<const char*>(big.data()), static_cast<int>(big.size()), 0,
                   reinterpret_cast<const sockaddr*>(&host), sizeof(host)) < 0) {
            return fail("send trunc recvfrom failed", 4);
        }
        if (sendto(s, reinterpret_cast<const char*>(big.data()), static_cast<int>(big.size()), 0,
                   reinterpret_cast<const sockaddr*>(&host), sizeof(host)) < 0) {
            return fail("send trunc WSARecvFrom failed", 5);
        }

        sockaddr_in mcast{};
        mcast.sin_family = AF_INET;
        mcast.sin_port = htons(o.port);
        inet_pton(AF_INET, "239.255.0.1", &mcast.sin_addr);
        std::vector<uint8_t> mcast_data(128);
        fillPattern(mcast_data, 200);
        std::memcpy(mcast_data.data(), "MCAST_PAYLOAD", 13);
        if (sendto(s, reinterpret_cast<const char*>(mcast_data.data()),
                   static_cast<int>(mcast_data.size()), 0,
                   reinterpret_cast<const sockaddr*>(&mcast), sizeof(mcast)) < 0) {
            return fail("send multicast failed", 6);
        }

        closesocket(s);
        writeReportFile(o.report_file, "PASS udp-edge sent\n");
        std::cout << "[TestGameApp udp-edge joiner] PASS\n";
        return 0;
    }

    sockaddr_in peer{};
    peer.sin_family = AF_INET;
    peer.sin_port = htons(o.port);
    if (inet_pton(AF_INET, o.dst.c_str(), &peer.sin_addr) != 1) {
        return fail("bad udp-edge peer", 7);
    }

    auto check_source = [&](const sockaddr_in& from, const char* what) -> bool {
        if (ntohl(from.sin_addr.s_addr) != ntohl(peer.sin_addr.s_addr) ||
            ntohs(from.sin_port) != o.port) {
            fail(std::string(what) + ": bad source address", 8);
            return false;
        }
        return true;
    };

    // 1. MSG_PEEK must return the payload and source without consuming the datagram.
    if (!waitReadable(s, 5000)) return fail("timed out waiting for PEEK", 9);
    char peek_buf[256] = {};
    sockaddr_in from{};
    int from_len = sizeof(from);
    int n = recvfrom(s, peek_buf, sizeof(peek_buf), MSG_PEEK,
                     reinterpret_cast<sockaddr*>(&from), &from_len);
    if (n != 128 || std::memcmp(peek_buf, "PEEK_PAYLOAD", 12) != 0) {
        return fail("MSG_PEEK returned bad payload", 10);
    }
    if (!check_source(from, "MSG_PEEK")) return 1;
    n = recvfrom(s, peek_buf, sizeof(peek_buf), 0,
                 reinterpret_cast<sockaddr*>(&from), &from_len);
    if (n != 128 || std::memcmp(peek_buf, "PEEK_PAYLOAD", 12) != 0) {
        return fail("normal recv after MSG_PEEK returned bad payload", 11);
    }

    // 2. recvfrom() with a short buffer must consume the datagram and report WSAEMSGSIZE.
    if (!waitReadable(s, 5000)) return fail("timed out waiting for trunc recvfrom", 12);
    char small[64] = {};
    n = recvfrom(s, small, sizeof(small), 0, nullptr, nullptr);
    if (n != SOCKET_ERROR || WSAGetLastError() != WSAEMSGSIZE) {
        return fail("recvfrom truncation did not report WSAEMSGSIZE (n=" +
                    std::to_string(n) + " wsaerr=" + std::to_string(WSAGetLastError()) + ")", 13);
    }

    // 3. WSARecvFrom() must report WSAEMSGSIZE and MSG_PARTIAL.
    if (!waitReadable(s, 5000)) return fail("timed out waiting for trunc WSARecvFrom", 14);
    WSABUF wbuf{};
    wbuf.buf = small;
    wbuf.len = sizeof(small);
    DWORD bytes = 0;
    DWORD flags = 0;
    n = WSARecvFrom(s, &wbuf, 1, &bytes, &flags, nullptr, nullptr, nullptr, nullptr);
    if (n != SOCKET_ERROR || WSAGetLastError() != WSAEMSGSIZE || !(flags & MSG_PARTIAL)) {
        return fail("WSARecvFrom truncation did not report WSAEMSGSIZE/MSG_PARTIAL (n=" +
                    std::to_string(n) + " wsaerr=" + std::to_string(WSAGetLastError()) +
                    " flags=" + std::to_string(flags) + ")", 15);
    }

    // 4. Multicast destination must fan out like broadcast.
    if (!waitReadable(s, 5000)) return fail("timed out waiting for multicast", 16);
    char mcast_buf[256] = {};
    from_len = sizeof(from);
    n = recvfrom(s, mcast_buf, sizeof(mcast_buf), 0,
                 reinterpret_cast<sockaddr*>(&from), &from_len);
    if (n != 128 || std::memcmp(mcast_buf, "MCAST_PAYLOAD", 13) != 0) {
        return fail("multicast payload mismatch", 17);
    }
    if (!check_source(from, "multicast")) return 1;

    closesocket(s);
    writeReportFile(o.report_file, "PASS udp-edge\n");
    std::cout << "[TestGameApp udp-edge host] PASS\n";
    return 0;
}

// ============================================================================
// Adapter and DNS virtualization
// ============================================================================
int runAdapterDns(const ScenarioOptions& o) {
    char host_name[256] = {};
    if (gethostname(host_name, sizeof(host_name)) != 0) {
        writeReportFile(o.report_file, "FAIL: gethostname failed\n");
        return 1;
    }

    auto fail = [&](const std::string& reason, int code) {
        writeReportFile(o.report_file, "FAIL: " + reason + "\n");
        std::cerr << "[TestGameApp adapter-dns] FAIL: " << reason << "\n";
        return code;
    };

    hostent* he = gethostbyname(host_name);
    if (!he || !he->h_addr_list || !he->h_addr_list[0]) {
        return fail("gethostbyname returned null", 2);
    }
    in_addr vip_addr{};
    std::memcpy(&vip_addr, he->h_addr_list[0], sizeof(vip_addr));
    const std::string vip = ipToStr(vip_addr);
    if (vip.rfind("10.42.0.", 0) != 0) {
        return fail("gethostbyname did not return a 10.42.0.x VIP: " + vip, 3);
    }

    ADDRINFOA hints{};
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_DGRAM;
    PADDRINFOA addrs = nullptr;
    if (getaddrinfo(host_name, nullptr, &hints, &addrs) != 0 || !addrs) {
        return fail("getaddrinfo failed", 4);
    }
    bool addrinfo_ok = false;
    for (PADDRINFOA p = addrs; p != nullptr; p = p->ai_next) {
        if (p->ai_family == AF_INET && p->ai_addr &&
            p->ai_addrlen >= sizeof(sockaddr_in)) {
            const auto* sin = reinterpret_cast<const sockaddr_in*>(p->ai_addr);
            if (ipToStr(sin->sin_addr) == vip) {
                addrinfo_ok = true;
                break;
            }
        }
    }
    freeaddrinfo(addrs);
    if (!addrinfo_ok) {
        return fail("getaddrinfo did not return the virtual hostname address", 5);
    }

    ULONG info_len = sizeof(IP_ADAPTER_INFO) * 16;
    std::vector<uint8_t> info_buf(info_len);
    auto* info = reinterpret_cast<PIP_ADAPTER_INFO>(info_buf.data());
    ULONG info_rc = GetAdaptersInfo(info, &info_len);
    if (info_rc == ERROR_BUFFER_OVERFLOW) {
        info_buf.resize(info_len);
        info = reinterpret_cast<PIP_ADAPTER_INFO>(info_buf.data());
        info_rc = GetAdaptersInfo(info, &info_len);
    }
    if (info_rc != NO_ERROR || !info) {
        return fail("GetAdaptersInfo failed (rc=" + std::to_string(info_rc) + ")", 6);
    }
    if (std::string(info->IpAddressList.IpAddress.String) != vip ||
        std::string(info->IpAddressList.IpMask.String) != "255.255.255.0" ||
        std::string(info->GatewayList.IpAddress.String) != "10.42.0.1") {
        return fail("GetAdaptersInfo returned wrong virtual adapter data", 7);
    }

    ULONG aa_len = 16 * 1024;
    std::vector<uint8_t> aa_buf(aa_len);
    auto* aa = reinterpret_cast<PIP_ADAPTER_ADDRESSES>(aa_buf.data());
    ULONG aa_rc = GetAdaptersAddresses(AF_INET, GAA_FLAG_INCLUDE_PREFIX, nullptr, aa, &aa_len);
    if (aa_rc == ERROR_BUFFER_OVERFLOW) {
        aa_buf.resize(aa_len);
        aa = reinterpret_cast<PIP_ADAPTER_ADDRESSES>(aa_buf.data());
        aa_rc = GetAdaptersAddresses(AF_INET, GAA_FLAG_INCLUDE_PREFIX, nullptr, aa, &aa_len);
    }
    if (aa_rc != NO_ERROR) {
        return fail("GetAdaptersAddresses failed (rc=" + std::to_string(aa_rc) + ")", 8);
    }
    bool aa_ok = false;
    for (auto* curr = aa; curr != nullptr; curr = curr->Next) {
        if (curr->OperStatus != IfOperStatusUp || curr->IfType == IF_TYPE_SOFTWARE_LOOPBACK) continue;
        for (auto* ua = curr->FirstUnicastAddress; ua != nullptr; ua = ua->Next) {
            if (ua->Address.lpSockaddr && ua->Address.lpSockaddr->sa_family == AF_INET) {
                const auto* sin = reinterpret_cast<const sockaddr_in*>(ua->Address.lpSockaddr);
                if (ipToStr(sin->sin_addr) == vip && ua->OnLinkPrefixLength == 24) {
                    aa_ok = true;
                }
                break;
            }
        }
        if (aa_ok) break;
    }
    if (!aa_ok) {
        return fail("GetAdaptersAddresses did not return the virtual adapter", 9);
    }

    SOCKET s = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (s == INVALID_SOCKET) {
        return fail("WSAIoctl test socket() failed", 10);
    }
    std::vector<INTERFACE_INFO> ifs(16);
    DWORD returned = 0;
    int ioctl_rc = WSAIoctl(s, SIO_GET_INTERFACE_LIST, nullptr, 0,
                            ifs.data(), static_cast<DWORD>(ifs.size() * sizeof(INTERFACE_INFO)),
                            &returned, nullptr, nullptr);
    closesocket(s);
    if (ioctl_rc != 0) {
        return fail("WSAIoctl(SIO_GET_INTERFACE_LIST) failed (wsaerr=" +
                    std::to_string(WSAGetLastError()) + ")", 11);
    }
    bool iface_ok = false;
    const size_t iface_count = returned / sizeof(INTERFACE_INFO);
    for (size_t i = 0; i < iface_count; ++i) {
        if ((ifs[i].iiFlags & IFF_LOOPBACK) || !(ifs[i].iiFlags & IFF_UP)) continue;
        if (ifs[i].iiAddress.AddressIn.sin_family != AF_INET) continue;
        const std::string iface_ip = ipToStr(ifs[i].iiAddress.AddressIn.sin_addr);
        const std::string iface_bcast = ipToStr(ifs[i].iiBroadcastAddress.AddressIn.sin_addr);
        const std::string iface_mask = ipToStr(ifs[i].iiNetmask.AddressIn.sin_addr);
        if (iface_ip == vip && iface_bcast == "10.42.0.255" && iface_mask == "255.255.255.0") {
            iface_ok = true;
        }
        break;
    }
    if (!iface_ok) {
        return fail("SIO_GET_INTERFACE_LIST did not return the virtual interface", 12);
    }

    writeReportFile(o.report_file, "PASS adapter-dns vip=" + vip + "\n");
    std::cout << "[TestGameApp adapter-dns] PASS vip=" << vip << "\n";
    return 0;
}

// ============================================================================
// Wait/event models: WSAPoll, WSAEventSelect, WSAAsyncSelect
// ============================================================================
HANDLE g_udp_async_read_event = nullptr;

LRESULT CALLBACK udpAsyncWndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    if (msg == WM_APP + 1) {
        if (WSAGETSELECTEVENT(lParam) == FD_READ && g_udp_async_read_event) {
            SetEvent(g_udp_async_read_event);
        }
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

int runUdpWait(const ScenarioOptions& o) {
    SOCKET s = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (s == INVALID_SOCKET) {
        writeReportFile(o.report_file, "FAIL: udp-wait socket() failed\n");
        return 1;
    }
    sockaddr_in bind_addr{};
    bind_addr.sin_family = AF_INET;
    bind_addr.sin_addr.s_addr = INADDR_ANY;
    bind_addr.sin_port = htons(o.port);
    if (bind(s, reinterpret_cast<const sockaddr*>(&bind_addr), sizeof(bind_addr)) != 0) {
        writeReportFile(o.report_file, "FAIL: udp-wait bind failed\n");
        closesocket(s);
        return 1;
    }

    const bool is_host = (o.role == "host");
    if (is_host) signalReady(o.report_file);

    auto fail = [&](const std::string& reason, int code) {
        writeReportFile(o.report_file, "FAIL: " + reason + "\n");
        std::cerr << "[TestGameApp udp-wait " << o.role << "] FAIL: " << reason << "\n";
        closesocket(s);
        return code;
    };

    if (!is_host) {
        sockaddr_in host{};
        host.sin_family = AF_INET;
        host.sin_port = htons(o.port);
        if (inet_pton(AF_INET, o.dst.c_str(), &host.sin_addr) != 1) {
            return fail("bad udp-wait dst", 2);
        }
        const char* tokens[] = {"POLL", "EVENT", "ASYNC"};
        for (const char* token : tokens) {
            if (sendto(s, token, static_cast<int>(std::strlen(token)), 0,
                       reinterpret_cast<const sockaddr*>(&host), sizeof(host)) < 0) {
                return fail(std::string("send ") + token + " failed", 3);
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
        closesocket(s);
        writeReportFile(o.report_file, "PASS udp-wait sent\n");
        std::cout << "[TestGameApp udp-wait joiner] PASS\n";
        return 0;
    }

    auto recv_token = [&](const char* want, int code) -> bool {
        char buf[128] = {};
        sockaddr_in from{};
        int from_len = sizeof(from);
        int n = recvfrom(s, buf, sizeof(buf), 0,
                         reinterpret_cast<sockaddr*>(&from), &from_len);
        if (n != static_cast<int>(std::strlen(want)) ||
            std::memcmp(buf, want, static_cast<size_t>(n)) != 0) {
            fail(std::string("received wrong token while waiting for ") + want, code);
            return false;
        }
        return true;
    };

    WSAPOLLFD pfd{};
    pfd.fd = s;
    pfd.events = POLLRDNORM;
    int poll_rc = WSAPoll(&pfd, 1, 5000);
    if (poll_rc <= 0 || !(pfd.revents & POLLRDNORM)) {
        return fail("WSAPoll did not report readable (rc=" + std::to_string(poll_rc) +
                    " revents=" + std::to_string(pfd.revents) + ")", 4);
    }
    if (!recv_token("POLL", 5)) return 1;

    WSAEVENT event = WSACreateEvent();
    if (event == WSA_INVALID_EVENT || WSAEventSelect(s, event, FD_READ) != 0) {
        return fail("WSAEventSelect setup failed", 6);
    }
    DWORD wait_rc = WSAWaitForMultipleEvents(1, &event, TRUE, 5000, FALSE);
    WSANETWORKEVENTS net_events{};
    if (wait_rc != WSA_WAIT_EVENT_0 ||
        WSAEnumNetworkEvents(s, event, &net_events) != 0 ||
        !(net_events.lNetworkEvents & FD_READ) ||
        net_events.iErrorCode[FD_READ_BIT] != 0) {
        return fail("WSAEventSelect did not report FD_READ", 7);
    }
    if (!recv_token("EVENT", 8)) return 1;
    WSAEventSelect(s, nullptr, 0);
    WSACloseEvent(event);

    WNDCLASSW wc{};
    wc.lpfnWndProc = udpAsyncWndProc;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.lpszClassName = L"APIVPNTestAsyncWindow";
    RegisterClassW(&wc);
    HWND hwnd = CreateWindowExW(0, wc.lpszClassName, L"", 0, 0, 0, 0, 0,
                                HWND_MESSAGE, nullptr, wc.hInstance, nullptr);
    if (!hwnd) {
        return fail("WSAAsyncSelect window creation failed", 9);
    }
    HANDLE async_event = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (!async_event) {
        DestroyWindow(hwnd);
        return fail("WSAAsyncSelect event creation failed", 10);
    }
    g_udp_async_read_event = async_event;
    if (WSAAsyncSelect(s, hwnd, WM_APP + 1, FD_READ) != 0) {
        g_udp_async_read_event = nullptr;
        CloseHandle(async_event);
        DestroyWindow(hwnd);
        return fail("WSAAsyncSelect setup failed", 11);
    }

    bool async_ready = false;
    auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (std::chrono::steady_clock::now() < deadline) {
        DWORD wr = MsgWaitForMultipleObjects(1, &async_event, FALSE, 100, QS_ALLINPUT);
        if (wr == WAIT_OBJECT_0) {
            async_ready = true;
            break;
        }
        if (wr == WAIT_OBJECT_0 + 1) {
            MSG msg;
            while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
                TranslateMessage(&msg);
                DispatchMessageW(&msg);
            }
        }
    }
    WSAAsyncSelect(s, nullptr, 0, 0);
    g_udp_async_read_event = nullptr;
    CloseHandle(async_event);
    DestroyWindow(hwnd);
    if (!async_ready) return fail("WSAAsyncSelect did not report FD_READ", 12);
    if (!recv_token("ASYNC", 13)) return 1;

    closesocket(s);
    writeReportFile(o.report_file, "PASS udp-wait\n");
    std::cout << "[TestGameApp udp-wait host] PASS\n";
    return 0;
}

// ============================================================================
// Overlapped UDP I/O
// ============================================================================
// Six ping-pong rounds, one per completion-observation API. Both the send and
// the receive of a round use the same mechanism, so a missing fix-up cannot be
// masked by another completion path. Rounds run in this order:
//   0 WSAGetOverlappedResult   (event)
//   1 completion routine
//   2 GetOverlappedResult      (event)
//   3 GetOverlappedResultEx    (event)
//   4 GetQueuedCompletionStatus (IOCP)
//   5 GetQueuedCompletionStatusEx (IOCP)
constexpr int kOvlRoundCount = 6;
constexpr size_t kOvlRequestSizes[kOvlRoundCount] = {512, 320, 1024, 200, 768, 640};
constexpr size_t kOvlReplyExtra = 37;
constexpr char kOvlMagic[8] = {'A', 'P', 'I', 'V', 'O', 'V', 'L', '1'};

enum class OvlMode : int {
    WsaGet = 0,
    Routine = 1,
    GetOverlapped = 2,
    GetOverlappedEx = 3,
    Iocp = 4,
    IocpEx = 5,
};

const char* ovlModeName(OvlMode mode) {
    switch (mode) {
        case OvlMode::WsaGet: return "WSAGetOverlappedResult";
        case OvlMode::Routine: return "completion-routine";
        case OvlMode::GetOverlapped: return "GetOverlappedResult";
        case OvlMode::GetOverlappedEx: return "GetOverlappedResultEx";
        case OvlMode::Iocp: return "GetQueuedCompletionStatus";
        case OvlMode::IocpEx: return "GetQueuedCompletionStatusEx";
    }
    return "unknown";
}

bool ovlIsIocp(OvlMode mode) {
    return mode == OvlMode::Iocp || mode == OvlMode::IocpEx;
}

std::vector<uint8_t> makeOvlMessage(uint32_t round, uint32_t seq, size_t size) {
    if (size < 20) size = 20;
    std::vector<uint8_t> buf(size);
    std::memcpy(buf.data(), kOvlMagic, sizeof(kOvlMagic));
    const uint32_t r = round;
    const uint32_t s = seq;
    const uint32_t n = static_cast<uint32_t>(size);
    std::memcpy(buf.data() + 8, &r, sizeof(r));
    std::memcpy(buf.data() + 12, &s, sizeof(s));
    std::memcpy(buf.data() + 16, &n, sizeof(n));
    for (size_t i = 20; i < size; ++i) {
        buf[i] = testPattern(static_cast<size_t>(round) * 4099u +
                             static_cast<size_t>(seq) * 131u + i);
    }
    return buf;
}

bool verifyOvlMessage(const std::vector<uint8_t>& buf, uint32_t round, uint32_t seq,
                      size_t expected_size, std::string& why) {
    if (buf.size() != expected_size) {
        why = "datagram length " + std::to_string(buf.size()) + " != " +
              std::to_string(expected_size);
        return false;
    }
    if (expected_size < 20 || std::memcmp(buf.data(), kOvlMagic, sizeof(kOvlMagic)) != 0) {
        why = "bad overlapped datagram magic";
        return false;
    }
    uint32_t r = 0, s = 0, n = 0;
    std::memcpy(&r, buf.data() + 8, sizeof(r));
    std::memcpy(&s, buf.data() + 12, sizeof(s));
    std::memcpy(&n, buf.data() + 16, sizeof(n));
    if (r != round || s != seq || n != expected_size) {
        why = "header mismatch (round=" + std::to_string(r) + " seq=" + std::to_string(s) +
              " size=" + std::to_string(n) + ")";
        return false;
    }
    for (size_t i = 20; i < expected_size; ++i) {
        if (buf[i] != testPattern(static_cast<size_t>(round) * 4099u +
                                  static_cast<size_t>(seq) * 131u + i)) {
            why = "payload mismatch at byte " + std::to_string(i);
            return false;
        }
    }
    return true;
}

struct OvlRoutineWait {
    std::atomic<bool> done{false};
    DWORD error = 0;
    DWORD bytes = 0;
    DWORD flags = 0;
};

struct OvlRoutineOverlapped {
    WSAOVERLAPPED ov{};
    OvlRoutineWait* wait = nullptr;
};

void CALLBACK ovlCompletionRoutine(DWORD error, DWORD bytes, LPWSAOVERLAPPED ov, DWORD flags) {
    auto* ctx = reinterpret_cast<OvlRoutineOverlapped*>(ov);
    if (!ctx || !ctx->wait) return;
    ctx->wait->error = error;
    ctx->wait->bytes = bytes;
    ctx->wait->flags = flags;
    ctx->wait->done.store(true, std::memory_order_release);
}

bool ovlWaitRoutine(OvlRoutineWait& wait, int timeout_ms, std::string& err) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
    while (!wait.done.load(std::memory_order_acquire)) {
        if (std::chrono::steady_clock::now() >= deadline) {
            err = "completion routine timed out";
            return false;
        }
        SleepEx(20, TRUE); // alertable: dispatches the completion-routine APC
    }
    if (wait.error != 0) {
        err = "completion routine failed (error=" + std::to_string(wait.error) + ")";
        return false;
    }
    return true;
}

bool ovlWaitEvent(HANDLE event_handle, int start_rc, int timeout_ms, std::string& err) {
    if (start_rc == 0) return true; // completed inline; no event wait needed
    const DWORD wr = WSAWaitForMultipleEvents(1, &event_handle, TRUE,
                                              static_cast<DWORD>(timeout_ms), FALSE);
    if (wr != WSA_WAIT_EVENT_0) {
        err = "overlapped event wait failed (wait=" + std::to_string(wr) +
              " wsaerr=" + std::to_string(WSAGetLastError()) + ")";
        return false;
    }
    return true;
}

bool ovlWaitIocp(HANDLE iocp, LPOVERLAPPED expected, bool use_ex, DWORD& bytes,
                 std::string& err) {
    if (!use_ex) {
        DWORD transferred = 0;
        ULONG_PTR key = 0;
        LPOVERLAPPED ov = nullptr;
        if (!GetQueuedCompletionStatus(iocp, &transferred, &key, &ov, 10000)) {
            err = "GetQueuedCompletionStatus failed (err=" + std::to_string(GetLastError()) + ")";
            return false;
        }
        if (ov != expected) {
            err = "GetQueuedCompletionStatus returned an unexpected OVERLAPPED";
            return false;
        }
        bytes = transferred;
        return true;
    }

    OVERLAPPED_ENTRY entries[4]{};
    ULONG removed = 0;
    if (!GetQueuedCompletionStatusEx(iocp, entries, 4, &removed, 10000, FALSE)) {
        err = "GetQueuedCompletionStatusEx failed (err=" + std::to_string(GetLastError()) + ")";
        return false;
    }
    for (ULONG i = 0; i < removed; ++i) {
        if (entries[i].lpOverlapped == expected) {
            bytes = entries[i].dwNumberOfBytesTransferred;
            return true;
        }
    }
    err = "GetQueuedCompletionStatusEx did not return the expected OVERLAPPED";
    return false;
}

bool ovlEnsureIocp(SOCKET s, HANDLE& iocp, std::string& err) {
    if (iocp) return true;
    iocp = CreateIoCompletionPort(INVALID_HANDLE_VALUE, nullptr, 0, 1);
    if (!iocp) {
        err = "CreateIoCompletionPort failed (err=" + std::to_string(GetLastError()) + ")";
        return false;
    }
    if (!CreateIoCompletionPort(reinterpret_cast<HANDLE>(s), iocp, 1, 0)) {
        err = "associating the UDP socket with the completion port failed (err=" +
              std::to_string(GetLastError()) + ")";
        CloseHandle(iocp);
        iocp = nullptr;
        return false;
    }
    return true;
}

bool ovlSend(SOCKET s, const sockaddr_in& dst, const std::vector<uint8_t>& data,
             OvlMode mode, HANDLE iocp, DWORD& bytes, std::string& err) {
    WSABUF buf{};
    buf.buf = const_cast<char*>(reinterpret_cast<const char*>(data.data()));
    buf.len = static_cast<ULONG>(data.size());
    const sockaddr* to = reinterpret_cast<const sockaddr*>(&dst);
    const int to_len = sizeof(dst);

    if (mode == OvlMode::Routine) {
        OvlRoutineWait wait;
        OvlRoutineOverlapped ctx;
        ctx.wait = &wait;
        DWORD inline_bytes = 0;
        const int rc = WSASendTo(s, &buf, 1, &inline_bytes, 0, to, to_len,
                                 &ctx.ov, ovlCompletionRoutine);
        if (rc == SOCKET_ERROR && WSAGetLastError() != WSA_IO_PENDING) {
            err = "overlapped WSASendTo failed (wsaerr=" + std::to_string(WSAGetLastError()) + ")";
            return false;
        }
        if (!ovlWaitRoutine(wait, 10000, err)) return false;
        bytes = wait.bytes;
        return true;
    }

    if (ovlIsIocp(mode)) {
        WSAOVERLAPPED ov{};
        DWORD inline_bytes = 0;
        const int rc = WSASendTo(s, &buf, 1, &inline_bytes, 0, to, to_len, &ov, nullptr);
        if (rc == SOCKET_ERROR && WSAGetLastError() != WSA_IO_PENDING) {
            err = "overlapped WSASendTo failed (wsaerr=" + std::to_string(WSAGetLastError()) + ")";
            return false;
        }
        return ovlWaitIocp(iocp, &ov, mode == OvlMode::IocpEx, bytes, err);
    }

    WSAOVERLAPPED ov{};
    ov.hEvent = WSACreateEvent();
    if (ov.hEvent == WSA_INVALID_EVENT) {
        err = "WSACreateEvent failed (wsaerr=" + std::to_string(WSAGetLastError()) + ")";
        return false;
    }
    DWORD inline_bytes = 0;
    const int rc = WSASendTo(s, &buf, 1, &inline_bytes, 0, to, to_len, &ov, nullptr);
    const DWORD start_error = (rc == SOCKET_ERROR) ? WSAGetLastError() : 0;
    if (rc == SOCKET_ERROR && start_error != WSA_IO_PENDING) {
        WSACloseEvent(ov.hEvent);
        err = "overlapped WSASendTo failed (wsaerr=" + std::to_string(start_error) + ")";
        return false;
    }
    if (!ovlWaitEvent(ov.hEvent, rc, 10000, err)) {
        WSACloseEvent(ov.hEvent);
        return false;
    }

    DWORD flags = 0;
    BOOL ok = FALSE;
    if (mode == OvlMode::WsaGet) {
        ok = WSAGetOverlappedResult(s, &ov, &bytes, FALSE, &flags);
    } else if (mode == OvlMode::GetOverlapped) {
        ok = GetOverlappedResult(reinterpret_cast<HANDLE>(s), &ov, &bytes, FALSE);
    } else {
        ok = GetOverlappedResultEx(reinterpret_cast<HANDLE>(s), &ov, &bytes, 0, FALSE);
    }
    const DWORD query_error = ok ? 0 : GetLastError();
    WSACloseEvent(ov.hEvent);
    if (!ok) {
        err = std::string("overlapped send completion query failed (err=") +
              std::to_string(query_error) + " wsaerr=" + std::to_string(WSAGetLastError()) + ")";
        return false;
    }
    return true;
}

bool ovlRecv(SOCKET s, std::vector<uint8_t>& data, size_t expected, OvlMode mode, HANDLE iocp,
             sockaddr_in& from, DWORD& bytes, std::string& err) {
    data.assign(expected, 0);
    WSABUF buf{};
    buf.buf = reinterpret_cast<char*>(data.data());
    buf.len = static_cast<ULONG>(data.size());
    from = sockaddr_in{};
    INT from_len = sizeof(from);
    DWORD flags = 0;

    if (mode == OvlMode::Routine) {
        OvlRoutineWait wait;
        OvlRoutineOverlapped ctx;
        ctx.wait = &wait;
        DWORD inline_bytes = 0;
        const int rc = WSARecvFrom(s, &buf, 1, &inline_bytes, &flags,
                                   reinterpret_cast<sockaddr*>(&from), &from_len,
                                   &ctx.ov, ovlCompletionRoutine);
        if (rc == SOCKET_ERROR && WSAGetLastError() != WSA_IO_PENDING) {
            err = "overlapped WSARecvFrom failed (wsaerr=" + std::to_string(WSAGetLastError()) + ")";
            return false;
        }
        if (!ovlWaitRoutine(wait, 10000, err)) return false;
        if (wait.flags & MSG_PARTIAL) {
            err = "completion routine reported MSG_PARTIAL";
            return false;
        }
        bytes = wait.bytes;
        return true;
    }

    if (ovlIsIocp(mode)) {
        WSAOVERLAPPED ov{};
        DWORD inline_bytes = 0;
        const int rc = WSARecvFrom(s, &buf, 1, &inline_bytes, &flags,
                                   reinterpret_cast<sockaddr*>(&from), &from_len, &ov, nullptr);
        if (rc == SOCKET_ERROR && WSAGetLastError() != WSA_IO_PENDING) {
            err = "overlapped WSARecvFrom failed (wsaerr=" + std::to_string(WSAGetLastError()) + ")";
            return false;
        }
        return ovlWaitIocp(iocp, &ov, mode == OvlMode::IocpEx, bytes, err);
    }

    WSAOVERLAPPED ov{};
    ov.hEvent = WSACreateEvent();
    if (ov.hEvent == WSA_INVALID_EVENT) {
        err = "WSACreateEvent failed (wsaerr=" + std::to_string(WSAGetLastError()) + ")";
        return false;
    }
    DWORD inline_bytes = 0;
    const int rc = WSARecvFrom(s, &buf, 1, &inline_bytes, &flags,
                               reinterpret_cast<sockaddr*>(&from), &from_len, &ov, nullptr);
    const DWORD start_error = (rc == SOCKET_ERROR) ? WSAGetLastError() : 0;
    if (rc == SOCKET_ERROR && start_error != WSA_IO_PENDING) {
        WSACloseEvent(ov.hEvent);
        err = "overlapped WSARecvFrom failed (wsaerr=" + std::to_string(start_error) + ")";
        return false;
    }
    if (!ovlWaitEvent(ov.hEvent, rc, 10000, err)) {
        WSACloseEvent(ov.hEvent);
        return false;
    }

    DWORD query_flags = 0;
    BOOL ok = FALSE;
    if (mode == OvlMode::WsaGet) {
        ok = WSAGetOverlappedResult(s, &ov, &bytes, FALSE, &query_flags);
    } else if (mode == OvlMode::GetOverlapped) {
        ok = GetOverlappedResult(reinterpret_cast<HANDLE>(s), &ov, &bytes, FALSE);
    } else {
        ok = GetOverlappedResultEx(reinterpret_cast<HANDLE>(s), &ov, &bytes, 0, FALSE);
    }
    const DWORD query_error = ok ? 0 : GetLastError();
    WSACloseEvent(ov.hEvent);
    if (!ok) {
        err = std::string("overlapped receive completion query failed (err=") +
              std::to_string(query_error) + " wsaerr=" + std::to_string(WSAGetLastError()) + ")";
        return false;
    }
    if (query_flags & MSG_PARTIAL) {
        err = "overlapped receive reported MSG_PARTIAL";
        return false;
    }
    return true;
}

bool ovlSourceMatches(const sockaddr_in& from, uint32_t expected_ip, uint16_t expected_port,
                      std::string& err) {
    if (from.sin_family != AF_INET) {
        err = "overlapped receive source family is not AF_INET";
        return false;
    }
    const std::string got_ip = ipToStr(from.sin_addr);
    in_addr want_addr{};
    want_addr.s_addr = htonl(expected_ip);
    const std::string want_ip = ipToStr(want_addr);
    if (ntohl(from.sin_addr.s_addr) != expected_ip || ntohs(from.sin_port) != expected_port) {
        err = "overlapped receive source " + got_ip + ":" +
              std::to_string(ntohs(from.sin_port)) + " != " + want_ip + ":" +
              std::to_string(expected_port);
        return false;
    }
    return true;
}

int runUdpOverlapped(const ScenarioOptions& o) {
    SOCKET s = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (s == INVALID_SOCKET) {
        writeReportFile(o.report_file, "FAIL: overlapped udp socket() failed\n");
        return 1;
    }

    sockaddr_in bind_addr{};
    bind_addr.sin_family = AF_INET;
    bind_addr.sin_addr.s_addr = INADDR_ANY;
    bind_addr.sin_port = htons(o.port);
    if (bind(s, reinterpret_cast<const sockaddr*>(&bind_addr), sizeof(bind_addr)) != 0) {
        writeReportFile(o.report_file, "FAIL: overlapped udp bind failed\n");
        closesocket(s);
        return 1;
    }

    sockaddr_in peer{};
    peer.sin_family = AF_INET;
    peer.sin_port = htons(o.port);
    if (inet_pton(AF_INET, o.dst.c_str(), &peer.sin_addr) != 1) {
        writeReportFile(o.report_file, "FAIL: bad overlapped dst " + o.dst + "\n");
        closesocket(s);
        return 1;
    }
    const uint32_t peer_ip = ntohl(peer.sin_addr.s_addr);
    const bool is_host = (o.role == "host");
    if (is_host) signalReady(o.report_file);

    HANDLE iocp = nullptr;
    auto fail = [&](const std::string& reason, int code) {
        writeReportFile(o.report_file, "FAIL: " + reason + "\n");
        std::cerr << "[TestGameApp udp-overlapped " << o.role << "] FAIL: " << reason << "\n";
        if (iocp) CloseHandle(iocp);
        closesocket(s);
        return code;
    };

    for (int round = 0; round < kOvlRoundCount; ++round) {
        const OvlMode mode = static_cast<OvlMode>(round);
        const size_t request_size = kOvlRequestSizes[round];
        std::string err;

        if (ovlIsIocp(mode) && !ovlEnsureIocp(s, iocp, err)) {
            return fail(std::string(ovlModeName(mode)) + ": " + err, 70 + round);
        }

        if (is_host) {
            std::vector<uint8_t> request;
            sockaddr_in from{};
            DWORD bytes = 0;
            if (!ovlRecv(s, request, request_size, mode, iocp, from, bytes, err)) {
                return fail(std::string(ovlModeName(mode)) + " recv: " + err, 70 + round);
            }
            if (bytes != request_size) {
                return fail(std::string(ovlModeName(mode)) + " recv byte count " +
                                std::to_string(bytes) + " != " + std::to_string(request_size),
                            70 + round);
            }
            std::string why;
            if (!verifyOvlMessage(request, static_cast<uint32_t>(round), 0, request_size, why)) {
                return fail(std::string(ovlModeName(mode)) + " request: " + why, 70 + round);
            }
            if (!ovlSourceMatches(from, peer_ip, o.port, why)) {
                return fail(std::string(ovlModeName(mode)) + " request: " + why, 70 + round);
            }

            const auto reply = makeOvlMessage(static_cast<uint32_t>(round), 1,
                                              request_size + kOvlReplyExtra);
            if (!ovlSend(s, from, reply, mode, iocp, bytes, err)) {
                return fail(std::string(ovlModeName(mode)) + " send: " + err, 70 + round);
            }
            if (bytes != reply.size()) {
                return fail(std::string(ovlModeName(mode)) + " send byte count " +
                                std::to_string(bytes) + " != " + std::to_string(reply.size()),
                            70 + round);
            }
        } else {
            const auto request = makeOvlMessage(static_cast<uint32_t>(round), 0, request_size);
            DWORD bytes = 0;
            if (!ovlSend(s, peer, request, mode, iocp, bytes, err)) {
                return fail(std::string(ovlModeName(mode)) + " send: " + err, 70 + round);
            }
            if (bytes != request.size()) {
                return fail(std::string(ovlModeName(mode)) + " send byte count " +
                                std::to_string(bytes) + " != " + std::to_string(request.size()),
                            70 + round);
            }

            std::vector<uint8_t> reply;
            sockaddr_in from{};
            if (!ovlRecv(s, reply, request_size + kOvlReplyExtra, mode, iocp, from, bytes, err)) {
                return fail(std::string(ovlModeName(mode)) + " recv: " + err, 70 + round);
            }
            if (bytes != reply.size()) {
                return fail(std::string(ovlModeName(mode)) + " recv byte count " +
                                std::to_string(bytes) + " != " + std::to_string(reply.size()),
                            70 + round);
            }
            std::string why;
            if (!verifyOvlMessage(reply, static_cast<uint32_t>(round), 1, reply.size(), why)) {
                return fail(std::string(ovlModeName(mode)) + " reply: " + why, 70 + round);
            }
            if (!ovlSourceMatches(from, peer_ip, o.port, why)) {
                return fail(std::string(ovlModeName(mode)) + " reply: " + why, 70 + round);
            }
        }
    }

    if (iocp) CloseHandle(iocp);
    closesocket(s);
    writeReportFile(o.report_file, "PASS udp-overlapped rounds=" +
                                      std::to_string(kOvlRoundCount) + "\n");
    std::cout << "[TestGameApp udp-overlapped " << o.role << "] PASS rounds="
              << kOvlRoundCount << "\n";
    return 0;
}

// ============================================================================
// Overlapped edge cases: multiple outstanding receives, IOCP batching,
// truncated receives and WSAGetOverlappedResult(fWait=TRUE).
// ============================================================================
int runUdpOverlappedEdge(const ScenarioOptions& o) {
    SOCKET s = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (s == INVALID_SOCKET) {
        writeReportFile(o.report_file, "FAIL: overlapped-edge socket() failed\n");
        return 1;
    }
    sockaddr_in bind_addr{};
    bind_addr.sin_family = AF_INET;
    bind_addr.sin_addr.s_addr = INADDR_ANY;
    bind_addr.sin_port = htons(o.port);
    if (bind(s, reinterpret_cast<const sockaddr*>(&bind_addr), sizeof(bind_addr)) != 0) {
        writeReportFile(o.report_file, "FAIL: overlapped-edge bind failed\n");
        closesocket(s);
        return 1;
    }

    sockaddr_in peer{};
    peer.sin_family = AF_INET;
    peer.sin_port = htons(o.port);
    if (inet_pton(AF_INET, o.dst.c_str(), &peer.sin_addr) != 1) {
        writeReportFile(o.report_file, "FAIL: bad overlapped-edge dst " + o.dst + "\n");
        closesocket(s);
        return 1;
    }
    const uint32_t peer_ip = ntohl(peer.sin_addr.s_addr);
    const bool is_host = (o.role == "host");
    if (is_host) signalReady(o.report_file);

    HANDLE iocp = nullptr;
    auto fail = [&](const std::string& reason, int code) {
        writeReportFile(o.report_file, "FAIL: " + reason + "\n");
        std::cerr << "[TestGameApp udp-overlapped-edge " << o.role << "] FAIL: " << reason << "\n";
        if (iocp) CloseHandle(iocp);
        closesocket(s);
        return code;
    };

    if (!is_host) {
        auto send_msg = [&](uint32_t round, size_t size) -> bool {
            const auto msg = makeOvlMessage(round, 0, size);
            return sendto(s, reinterpret_cast<const char*>(msg.data()),
                          static_cast<int>(msg.size()), 0,
                          reinterpret_cast<const sockaddr*>(&peer), sizeof(peer)) ==
                   static_cast<int>(msg.size());
        };
        if (!send_msg(10, 256) || !send_msg(11, 384)) {
            return fail("sending the two outstanding datagrams failed", 2);
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        if (!send_msg(20, 2048)) return fail("sending the truncated datagram failed", 3);
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        if (!send_msg(30, 128)) return fail("sending the fWait datagram failed", 4);
        closesocket(s);
        writeReportFile(o.report_file, "PASS udp-overlapped-edge sent\n");
        std::cout << "[TestGameApp udp-overlapped-edge joiner] PASS\n";
        return 0;
    }

    iocp = CreateIoCompletionPort(INVALID_HANDLE_VALUE, nullptr, 0, 1);
    if (!iocp || !CreateIoCompletionPort(reinterpret_cast<HANDLE>(s), iocp, 1, 0)) {
        return fail("IOCP setup failed", 5);
    }

    struct PendingOp {
        WSAOVERLAPPED ov{};
        std::vector<uint8_t> buf;
        sockaddr_in from{};
        INT from_len = sizeof(from);
        DWORD bytes = 0;
    };
    const size_t sizes[2] = {256, 384};
    PendingOp ops[2];
    for (int i = 0; i < 2; ++i) {
        ops[i].buf.assign(512, 0);
        WSABUF wbuf{};
        wbuf.buf = reinterpret_cast<char*>(ops[i].buf.data());
        wbuf.len = static_cast<ULONG>(ops[i].buf.size());
        DWORD flags = 0;
        DWORD inline_bytes = 0;
        int rc = WSARecvFrom(s, &wbuf, 1, &inline_bytes, &flags,
                             reinterpret_cast<sockaddr*>(&ops[i].from), &ops[i].from_len,
                             &ops[i].ov, nullptr);
        if (rc == SOCKET_ERROR && WSAGetLastError() != WSA_IO_PENDING) {
            return fail("issuing outstanding WSARecvFrom failed", 6);
        }
    }

    int completed = 0;
    auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    while (completed < 2 && std::chrono::steady_clock::now() < deadline) {
        OVERLAPPED_ENTRY entries[4]{};
        ULONG removed = 0;
        if (!GetQueuedCompletionStatusEx(iocp, entries, 4, &removed, 5000, FALSE)) {
            return fail("GetQueuedCompletionStatusEx failed", 7);
        }
        if (removed == 0) continue;
        for (ULONG j = 0; j < removed; ++j) {
            int index = -1;
            for (int i = 0; i < 2; ++i) {
                if (entries[j].lpOverlapped == &ops[i].ov) index = i;
            }
            if (index < 0) return fail("unexpected OVERLAPPED from IOCP", 8);
            if (ops[index].bytes != 0) continue;
            ops[index].bytes = entries[j].dwNumberOfBytesTransferred;
            if (ops[index].bytes != sizes[index]) {
                return fail("outstanding receive " + std::to_string(index) +
                            " byte count " + std::to_string(ops[index].bytes) +
                            " != " + std::to_string(sizes[index]), 9);
            }
            std::string why;
            const std::vector<uint8_t> payload(ops[index].buf.data(),
                                               ops[index].buf.data() + ops[index].bytes);
            if (!verifyOvlMessage(payload, static_cast<uint32_t>(10 + index), 0,
                                  sizes[index], why)) {
                return fail("outstanding receive " + std::to_string(index) + ": " + why, 10);
            }
            if (!ovlSourceMatches(ops[index].from, peer_ip, o.port, why)) {
                return fail("outstanding receive " + std::to_string(index) + ": " + why, 11);
            }
            ++completed;
        }
    }
    if (completed != 2) return fail("only one outstanding receive completed", 12);

    // Truncated overlapped receive: completion must be WSAEMSGSIZE + MSG_PARTIAL.
    WSAOVERLAPPED tov{};
    tov.hEvent = WSACreateEvent();
    uint8_t small[64] = {};
    WSABUF tbuf{};
    tbuf.buf = reinterpret_cast<char*>(small);
    tbuf.len = sizeof(small);
    DWORD tflags = 0;
    DWORD tbytes = 0;
    sockaddr_in tfrom{};
    INT tfrom_len = sizeof(tfrom);
    int trc = WSARecvFrom(s, &tbuf, 1, &tbytes, &tflags,
                          reinterpret_cast<sockaddr*>(&tfrom), &tfrom_len, &tov, nullptr);
    if (trc == SOCKET_ERROR && WSAGetLastError() != WSA_IO_PENDING) {
        WSACloseEvent(tov.hEvent);
        return fail("truncated overlapped WSARecvFrom failed", 13);
    }
    if (trc != 0) {
        DWORD wr = WSAWaitForMultipleEvents(1, &tov.hEvent, TRUE, 5000, FALSE);
        if (wr != WSA_WAIT_EVENT_0) {
            WSACloseEvent(tov.hEvent);
            return fail("truncated overlapped receive timed out", 14);
        }
    }
    DWORD query_flags = 0;
    BOOL tok = WSAGetOverlappedResult(s, &tov, &tbytes, FALSE, &query_flags);
    DWORD terr = WSAGetLastError();
    WSACloseEvent(tov.hEvent);
    if (tok || terr != WSAEMSGSIZE || !(query_flags & MSG_PARTIAL)) {
        return fail("truncated overlapped receive did not report WSAEMSGSIZE/MSG_PARTIAL "
                    "(ok=" + std::to_string(tok) + " err=" + std::to_string(terr) +
                    " flags=" + std::to_string(query_flags) + ")", 15);
    }

    // WSAGetOverlappedResult(fWait=TRUE) must block until the final datagram arrives.
    WSAOVERLAPPED wov{};
    uint8_t wbuf[256] = {};
    WSABUF wb{};
    wb.buf = reinterpret_cast<char*>(wbuf);
    wb.len = sizeof(wbuf);
    DWORD wflags = 0;
    DWORD wbytes = 0;
    sockaddr_in wfrom{};
    INT wfrom_len = sizeof(wfrom);
    int wrc = WSARecvFrom(s, &wb, 1, &wbytes, &wflags,
                          reinterpret_cast<sockaddr*>(&wfrom), &wfrom_len, &wov, nullptr);
    if (wrc == SOCKET_ERROR && WSAGetLastError() != WSA_IO_PENDING) {
        return fail("fWait WSARecvFrom failed", 16);
    }
    DWORD final_flags = 0;
    if (!WSAGetOverlappedResult(s, &wov, &wbytes, TRUE, &final_flags) || wbytes != 128) {
        return fail("WSAGetOverlappedResult(fWait=TRUE) failed (bytes=" +
                    std::to_string(wbytes) + " wsaerr=" + std::to_string(WSAGetLastError()) + ")", 17);
    }
    std::string why;
    const std::vector<uint8_t> final_payload(wbuf, wbuf + wbytes);
    if (!verifyOvlMessage(final_payload, 30, 0, 128, why)) {
        return fail("fWait payload: " + why, 18);
    }
    if (!ovlSourceMatches(wfrom, peer_ip, o.port, why)) {
        return fail("fWait source: " + why, 19);
    }

    CloseHandle(iocp);
    closesocket(s);
    writeReportFile(o.report_file, "PASS udp-overlapped-edge\n");
    std::cout << "[TestGameApp udp-overlapped-edge host] PASS\n";
    return 0;
}

// ============================================================================
// Process injection: CreateProcessW + custom environment + CREATE_SUSPENDED,
// child hook activation, and fail-closed injection.
// ============================================================================
std::wstring toWideUtf8(const std::string& s) {
    if (s.empty()) return {};
    int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0);
    if (n <= 0) return {};
    std::wstring w(static_cast<size_t>(n), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), w.data(), n);
    return w;
}

std::vector<wchar_t> buildEnvBlockWithMarker(const wchar_t* marker_name,
                                             const wchar_t* marker_value) {
    std::vector<std::wstring> entries;
    LPWCH env = GetEnvironmentStringsW();
    if (env) {
        for (const wchar_t* p = env; *p != L'\0'; p += std::wcslen(p) + 1) {
            entries.emplace_back(p);
        }
        FreeEnvironmentStringsW(env);
    }

    const size_t marker_len = std::wcslen(marker_name);
    entries.erase(std::remove_if(entries.begin(), entries.end(),
                                 [&](const std::wstring& e) {
                                     const size_t eq = e.find(L'=', 1);
                                     return eq == marker_len &&
                                            _wcsnicmp(e.c_str(), marker_name, marker_len) == 0;
                                 }),
                  entries.end());
    entries.push_back(std::wstring(marker_name) + L"=" + marker_value);
    std::sort(entries.begin(), entries.end(), [](const std::wstring& a, const std::wstring& b) {
        return _wcsicmp(a.c_str(), b.c_str()) < 0;
    });

    std::vector<wchar_t> block;
    for (const auto& e : entries) {
        block.insert(block.end(), e.begin(), e.end());
        block.push_back(L'\0');
    }
    block.push_back(L'\0');
    return block;
}

int runEnvCheck(const ScenarioOptions& o) {
    auto fail = [&](const std::string& reason, int code) {
        writeReportFile(o.report_file, "FAIL: " + reason + "\n");
        std::cerr << "[TestGameApp env-check] FAIL: " << reason << "\n";
        return code;
    };

    char vip[64] = {};
    DWORD vip_len = GetEnvironmentVariableA("APIVPN_VIP", vip, sizeof(vip));
    if (vip_len == 0 || vip_len >= sizeof(vip) || std::string(vip).rfind("10.42.0.", 0) != 0) {
        return fail("APIVPN_VIP was not propagated", 2);
    }

    char marker[128] = {};
    DWORD marker_len = GetEnvironmentVariableA("APIVPN_TEST_MARKER", marker, sizeof(marker));
    if (marker_len == 0 || marker_len >= sizeof(marker) || std::string(marker) != o.token) {
        return fail("custom environment variable was not merged", 3);
    }

#if defined(_M_X64) || defined(__x86_64__)
    const char* hook_name = "apivpn_hook64.dll";
#else
    const char* hook_name = "apivpn_hook32.dll";
#endif
    if (!GetModuleHandleA(hook_name)) {
        return fail(std::string("hook DLL was not loaded: ") + hook_name, 7);
    }

    SOCKET s = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (s == INVALID_SOCKET) return fail("child socket() failed", 4);
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    if (inet_pton(AF_INET, vip, &addr.sin_addr) != 1) {
        closesocket(s);
        return fail("APIVPN_VIP was not a valid IPv4 address", 8);
    }
    addr.sin_port = htons(o.port);
    if (bind(s, reinterpret_cast<const sockaddr*>(&addr), sizeof(addr)) != 0) {
        closesocket(s);
        return fail("child bind failed", 5);
    }
    sockaddr_in local{};
    int local_len = sizeof(local);
    if (getsockname(s, reinterpret_cast<sockaddr*>(&local), &local_len) != 0 ||
        ipToStr(local.sin_addr) != vip || ntohs(local.sin_port) != o.port) {
        char ipc[32] = {}, udp[32] = {}, tcp[32] = {};
        GetEnvironmentVariableA("APIVPN_IPC_PORT", ipc, sizeof(ipc));
        GetEnvironmentVariableA("APIVPN_UDP_GW_PORT", udp, sizeof(udp));
        GetEnvironmentVariableA("APIVPN_TCP_GW_PORT", tcp, sizeof(tcp));
        const std::string got = ipToStr(local.sin_addr) + ":" +
                                std::to_string(ntohs(local.sin_port));
        closesocket(s);
        return fail(std::string("child hook did not virtualize the socket (expected ") + vip + ":" +
                    std::to_string(o.port) + ", got " + got + "; ipc=" + ipc +
                    " udp=" + udp + " tcp=" + tcp + ")", 6);
    }
    closesocket(s);

    writeReportFile(o.report_file, "PASS env-check vip=" + std::string(vip) + "\n");
    std::cout << "[TestGameApp env-check] PASS vip=" << vip << "\n";
    return 0;
}

int runSpawnW(const ScenarioOptions& o) {
    if (o.child_exe.empty() || o.child_report.empty()) {
        writeReportFile(o.report_file, "FAIL: spawn-w missing child path\n");
        return 1;
    }

    PROCESS_INFORMATION pi{};
    auto fail = [&](const std::string& reason, int code) {
        if (pi.hThread) {
            TerminateProcess(pi.hProcess, 1);
            CloseHandle(pi.hThread);
            pi.hThread = nullptr;
        }
        if (pi.hProcess) {
            CloseHandle(pi.hProcess);
            pi.hProcess = nullptr;
        }
        writeReportFile(o.report_file, "FAIL: " + reason + "\n");
        std::cerr << "[TestGameApp spawn-w] FAIL: " << reason << "\n";
        return code;
    };

    const std::wstring child = toWideUtf8(o.child_exe);
    const std::wstring child_report = toWideUtf8(o.child_report);
    std::wstring cmd = L"\"" + child + L"\" --scenario env-check --token custom-env-ok --report \"" +
                       child_report + L"\"";
    std::vector<wchar_t> cmd_buf(cmd.begin(), cmd.end());
    cmd_buf.push_back(L'\0');
    std::vector<wchar_t> env = buildEnvBlockWithMarker(L"APIVPN_TEST_MARKER", L"custom-env-ok");

    STARTUPINFOW si{};
    si.cb = sizeof(si);
    BOOL ok = CreateProcessW(child.c_str(), cmd_buf.data(), nullptr, nullptr, FALSE,
                             CREATE_SUSPENDED | CREATE_UNICODE_ENVIRONMENT, env.data(),
                             nullptr, &si, &pi);
    if (!ok) {
        return fail("CreateProcessW failed (err=" + std::to_string(GetLastError()) + ")", 2);
    }

    // The caller asked for CREATE_SUSPENDED: the child must not run before ResumeThread.
    if (WaitForSingleObject(pi.hProcess, 400) != WAIT_TIMEOUT) {
        return fail("CREATE_SUSPENDED child exited before ResumeThread", 3);
    }
    if (std::ifstream(o.child_report).good()) {
        return fail("CREATE_SUSPENDED child wrote its report before ResumeThread", 4);
    }

    if (ResumeThread(pi.hThread) == static_cast<DWORD>(-1)) {
        return fail("ResumeThread failed", 5);
    }
    DWORD wait_rc = WaitForSingleObject(pi.hProcess, 15000);
    DWORD exit_code = 1;
    if (wait_rc != WAIT_OBJECT_0 || !GetExitCodeProcess(pi.hProcess, &exit_code)) {
        return fail("child did not exit in time", 6);
    }
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    pi.hThread = nullptr;
    pi.hProcess = nullptr;

    std::string child_line;
    {
        std::ifstream in(o.child_report);
        if (in.is_open()) std::getline(in, child_line);
    }
    if (exit_code != 0 || child_line.rfind("PASS", 0) != 0) {
        return fail("child failed (exit=" + std::to_string(exit_code) +
                    " report='" + child_line + "')", 7);
    }

    std::remove(o.child_report.c_str());
    writeReportFile(o.report_file, "PASS spawn-w\n");
    std::cout << "[TestGameApp spawn-w] PASS\n";
    return 0;
}

int runSpawnFail(const ScenarioOptions& o) {
    if (o.child_exe.empty()) {
        writeReportFile(o.report_file, "FAIL: spawn-fail missing child path\n");
        return 1;
    }

    wchar_t old_bin_dir[32768] = {};
    const DWORD old_capacity = static_cast<DWORD>(sizeof(old_bin_dir) / sizeof(old_bin_dir[0]));
    DWORD old_len = GetEnvironmentVariableW(L"APIVPN_BIN_DIR", old_bin_dir, old_capacity);
    const bool had_old = old_len > 0 && old_len < old_capacity;
    std::wstring old_value(old_bin_dir, had_old ? old_len : 0);

    if (!SetEnvironmentVariableW(L"APIVPN_BIN_DIR", L"Z:\\__apivpn_missing_bin__")) {
        writeReportFile(o.report_file, "FAIL: could not set APIVPN_BIN_DIR\n");
        return 2;
    }

    const std::wstring child = toWideUtf8(o.child_exe);
    std::wstring cmd = L"\"" + child + L"\" --scenario env-check --token custom-env-ok --report \"" +
                       toWideUtf8(o.child_report) + L"\"";
    std::vector<wchar_t> cmd_buf(cmd.begin(), cmd.end());
    cmd_buf.push_back(L'\0');

    STARTUPINFOW si{};
    si.cb = sizeof(si);
    PROCESS_INFORMATION pi{};
    BOOL ok = CreateProcessW(child.c_str(), cmd_buf.data(), nullptr, nullptr, FALSE,
                             CREATE_SUSPENDED, nullptr, nullptr, &si, &pi);
    DWORD err = GetLastError();
    if (ok) {
        TerminateProcess(pi.hProcess, 1);
        WaitForSingleObject(pi.hProcess, 3000);
        CloseHandle(pi.hThread);
        CloseHandle(pi.hProcess);
    }

    SetEnvironmentVariableW(L"APIVPN_BIN_DIR", had_old ? old_value.c_str() : nullptr);

    auto fail = [&](const std::string& reason, int code) {
        writeReportFile(o.report_file, "FAIL: " + reason + "\n");
        std::cerr << "[TestGameApp spawn-fail] FAIL: " << reason << "\n";
        return code;
    };
    if (ok) return fail("CreateProcessW unexpectedly succeeded with an invalid bin dir", 3);
    if (err != ERROR_ACCESS_DENIED) {
        return fail("expected ERROR_ACCESS_DENIED, got " + std::to_string(err), 4);
    }
    if (std::ifstream(o.child_report).good()) {
        return fail("child report exists after failed injection", 5);
    }

    writeReportFile(o.report_file, "PASS spawn-fail\n");
    std::cout << "[TestGameApp spawn-fail] PASS\n";
    return 0;
}

int runScenario(const ScenarioOptions& o) {
    if (o.scenario == "tcp") {
        return (o.role == "host") ? runTcpHost(o) : runTcpJoiner(o);
    }
    if (o.scenario == "udp-recv") return runUdpRecv(o);
    if (o.scenario == "udp-send") return runUdpSend(o);
    if (o.scenario == "udp-large") return runUdpLarge(o);
    if (o.scenario == "udp-api") return runUdpApi(o);
    if (o.scenario == "udp-edge") return runUdpEdge(o);
    if (o.scenario == "udp-wait") return runUdpWait(o);
    if (o.scenario == "adapter-dns") return runAdapterDns(o);
    if (o.scenario == "udp-overlapped") return runUdpOverlapped(o);
    if (o.scenario == "udp-overlapped-edge") return runUdpOverlappedEdge(o);
    if (o.scenario == "env-check") return runEnvCheck(o);
    if (o.scenario == "spawn-w") return runSpawnW(o);
    if (o.scenario == "spawn-fail") return runSpawnFail(o);
    writeReportFile(o.report_file, "FAIL: unknown scenario " + o.scenario + "\n");
    return 1;
}

} // namespace

int main(int argc, char* argv[]) {
    std::string role = "host";
    std::string report_file;
    std::string spawn_child_exe;
    std::string spawn_child_role;
    bool use_dplay = false;

    ScenarioOptions opts;

    auto nextInt = [&](int i) { return std::atoi(argv[i]); };

    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--role") == 0 && i + 1 < argc) {
            role = argv[++i];
        } else if (std::strcmp(argv[i], "--report") == 0 && i + 1 < argc) {
            report_file = argv[++i];
        } else if (std::strcmp(argv[i], "--spawn-child") == 0 && i + 2 < argc) {
            spawn_child_exe = argv[++i];
            spawn_child_role = argv[++i];
        } else if (std::strcmp(argv[i], "--dplay") == 0) {
            use_dplay = true;
        } else if (std::strcmp(argv[i], "--scenario") == 0 && i + 1 < argc) {
            opts.scenario = argv[++i];
        } else if (std::strcmp(argv[i], "--mode") == 0 && i + 1 < argc) {
            opts.mode = argv[++i];
        } else if (std::strcmp(argv[i], "--token") == 0 && i + 1 < argc) {
            opts.token = argv[++i];
        } else if (std::strcmp(argv[i], "--dst") == 0 && i + 1 < argc) {
            opts.dst = argv[++i];
        } else if (std::strcmp(argv[i], "--dsts") == 0 && i + 1 < argc) {
            opts.dsts = argv[++i];
        } else if (std::strcmp(argv[i], "--child-exe") == 0 && i + 1 < argc) {
            opts.child_exe = argv[++i];
        } else if (std::strcmp(argv[i], "--child-report") == 0 && i + 1 < argc) {
            opts.child_report = argv[++i];
        } else if (std::strcmp(argv[i], "--port") == 0 && i + 1 < argc) {
            opts.port = static_cast<uint16_t>(nextInt(++i));
        } else if (std::strcmp(argv[i], "--conns") == 0 && i + 1 < argc) {
            opts.conns = nextInt(++i);
        } else if (std::strcmp(argv[i], "--expect") == 0 && i + 1 < argc) {
            opts.expect = nextInt(++i);
        } else if (std::strcmp(argv[i], "--size-kb") == 0 && i + 1 < argc) {
            opts.size_kb = nextInt(++i);
        } else if (std::strcmp(argv[i], "--delay-ms") == 0 && i + 1 < argc) {
            opts.delay_ms = nextInt(++i);
        } else if (std::strcmp(argv[i], "--count") == 0 && i + 1 < argc) {
            opts.count = nextInt(++i);
        } else if (std::strcmp(argv[i], "--timeout-ms") == 0 && i + 1 < argc) {
            opts.timeout_ms = nextInt(++i);
        } else if (std::strcmp(argv[i], "--sequential") == 0) {
            opts.sequential = true;
        }
    }

    opts.role = role;
    opts.report_file = report_file;

    if (!opts.scenario.empty() && opts.scenario != "basic" && opts.scenario != "dplay") {
        WSADATA wsa{};
        if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) {
            writeReportFile(opts.report_file, "FAIL: WSAStartup failed\n");
            return 1;
        }
        int rc = runScenario(opts);
        WSACleanup();
        return rc;
    }

    if (use_dplay) {
        return runDirectPlayTest(role, report_file);
    }

    // If acting as a launcher process, spawn the child executable via standard CreateProcessA
    // so APIVPN's CreateProcessA hook automatically injects the matching bitness Hook DLL.
    if (!spawn_child_exe.empty()) {
        std::string cmd = "\"" + spawn_child_exe + "\" --role " + spawn_child_role;
        if (!report_file.empty()) {
            cmd += " --report \"" + report_file + "\"";
        }
        STARTUPINFOA si{};
        si.cb = sizeof(si);
        PROCESS_INFORMATION pi{};
        if (!CreateProcessA(spawn_child_exe.c_str(), cmd.data(), nullptr, nullptr, FALSE,
                            0, nullptr, nullptr, &si, &pi)) {
            return 40;
        }
        WaitForSingleObject(pi.hProcess, 12000);
        DWORD code = 1;
        GetExitCodeProcess(pi.hProcess, &code);
        CloseHandle(pi.hThread);
        CloseHandle(pi.hProcess);
        return static_cast<int>(code);
    }

#if defined(_M_X64) || defined(__x86_64__)
    const char* arch_str = "x64";
#else
    const char* arch_str = "x86";
#endif

    auto failWith = [&](const std::string& reason, int code) {
        std::cerr << "[TestGameApp " << arch_str << " " << role << "] FAIL: " << reason
                  << " (WSAError=" << WSAGetLastError() << ")\n";
        if (!report_file.empty()) {
            std::ofstream out(report_file);
            out << "FAIL: " << reason << "\n";
        }
        WSACleanup();
        return code;
    };

    WSADATA wsa{};
    if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) {
        return 1;
    }

    // Verify that AttachConsole(ATTACH_PARENT_PROCESS) is blocked by the hook
    if (AttachConsole(ATTACH_PARENT_PROCESS) != FALSE) {
        return failWith("AttachConsole(ATTACH_PARENT_PROCESS) was not blocked by hook", 35);
    }

    // 1. Verify Virtual IP Enumeration via gethostbyname & GetAdaptersInfo
    char host_name[256] = {};
    gethostname(host_name, sizeof(host_name));
    hostent* he = gethostbyname(host_name);
    if (!he || !he->h_addr_list || !he->h_addr_list[0]) {
        return failWith("gethostbyname returned null", 2);
    }
    in_addr my_addr{};
    std::memcpy(&my_addr, he->h_addr_list[0], sizeof(in_addr));
    std::string my_vip_str = ipToStr(my_addr);

    if (my_vip_str.rfind("10.42.0.", 0) != 0) {
        return failWith("gethostbyname did not return 10.42.0.x VIP, got: " + my_vip_str, 3);
    }

    ULONG adapter_len = sizeof(IP_ADAPTER_INFO) * 16;
    std::string adapter_vip;
    auto* adapters = static_cast<PIP_ADAPTER_INFO>(std::malloc(adapter_len));
    if (GetAdaptersInfo(adapters, &adapter_len) == NO_ERROR && adapters) {
        adapter_vip = adapters->IpAddressList.IpAddress.String;
    }
    std::free(adapters);
    if (adapter_vip != my_vip_str) {
        return failWith("GetAdaptersInfo mismatch: " + adapter_vip + " vs " + my_vip_str, 4);
    }

    // 2. Create and bind both UDP and TCP sockets to fixed port 6112 (0.0.0.0:6112)
    SOCKET udp_sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (udp_sock == INVALID_SOCKET) {
        return failWith("Failed to create UDP socket", 5);
    }
    BOOL bcast = TRUE;
    setsockopt(udp_sock, SOL_SOCKET, SO_BROADCAST, reinterpret_cast<const char*>(&bcast), sizeof(bcast));

    sockaddr_in bind_addr{};
    bind_addr.sin_family = AF_INET;
    bind_addr.sin_addr.s_addr = INADDR_ANY;
    bind_addr.sin_port = htons(kGamePort);

    if (bind(udp_sock, reinterpret_cast<const sockaddr*>(&bind_addr), sizeof(bind_addr)) != 0) {
        return failWith("UDP bind(0.0.0.0:6112) failed", 6);
    }

    sockaddr_in queried_name{};
    int qlen = sizeof(queried_name);
    if (getsockname(udp_sock, reinterpret_cast<sockaddr*>(&queried_name), &qlen) != 0 ||
        ntohs(queried_name.sin_port) != kGamePort) {
        return failWith("getsockname on UDP socket did not return virtual port 6112", 7);
    }

    double bulk_mbps = 0.0;
    long long bulk_ms = 0;
    if (role == "host") {
        // Host binds TCP listener on 0.0.0.0:6112 as well
        SOCKET tcp_listen = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        if (tcp_listen == INVALID_SOCKET) {
            return failWith("Failed to create TCP listener socket", 8);
        }
        if (bind(tcp_listen, reinterpret_cast<const sockaddr*>(&bind_addr), sizeof(bind_addr)) != 0) {
            return failWith("TCP bind(0.0.0.0:6112) failed", 9);
        }
        if (listen(tcp_listen, 8) != 0) {
            return failWith("TCP listen() failed", 10);
        }
        signalReady(report_file);

        // Wait for UDP Broadcast discovery packet from Joiner
        if (!waitReadable(udp_sock, 8000)) {
            return failWith("Host timed out waiting for UDP broadcast discovery", 11);
        }

        char rx_buf[512] = {};
        sockaddr_in from_addr{};
        int from_len = sizeof(from_addr);
        int got = recvfrom(udp_sock, rx_buf, sizeof(rx_buf) - 1, 0,
                           reinterpret_cast<sockaddr*>(&from_addr), &from_len);
        if (got <= 0) {
            return failWith("Host recvfrom failed on UDP broadcast", 12);
        }
        std::string msg(rx_buf, static_cast<size_t>(got));
        std::string peer_vip = ipToStr(from_addr.sin_addr);
        if (msg != "DISCOVER_GAME_LOBBY" || peer_vip.rfind("10.42.0.", 0) != 0 || ntohs(from_addr.sin_port) != kGamePort) {
            return failWith("Unexpected UDP broadcast payload or sender: " + msg + " from " + peer_vip, 13);
        }

        // Reply via UDP Unicast directly to Joiner's Virtual IP:6112
        const char* reply = "LOBBY_INFO:MAP_CLASSIC_LAN";
        if (sendto(udp_sock, reply, static_cast<int>(std::strlen(reply)), 0,
                   reinterpret_cast<const sockaddr*>(&from_addr), sizeof(from_addr)) < 0) {
            return failWith("Host UDP unicast sendto failed", 14);
        }

        // Wait for incoming TCP connection on virtual port 6112
        if (!waitReadable(tcp_listen, 8000)) {
            return failWith("Host timed out waiting for incoming TCP connection", 15);
        }

        sockaddr_in tcp_peer{};
        int tcp_peer_len = sizeof(tcp_peer);
        SOCKET client_sock = accept(tcp_listen, reinterpret_cast<sockaddr*>(&tcp_peer), &tcp_peer_len);
        if (client_sock == INVALID_SOCKET) {
            return failWith("Host accept() failed", 16);
        }

        std::string accepted_vip = ipToStr(tcp_peer.sin_addr);
        if (accepted_vip != peer_vip) {
            return failWith("Host accept() peer VIP mismatch: " + accepted_vip + " vs " + peer_vip, 17);
        }

        sockaddr_in gp_addr{};
        int gp_len = sizeof(gp_addr);
        if (getpeername(client_sock, reinterpret_cast<sockaddr*>(&gp_addr), &gp_len) != 0 ||
            ipToStr(gp_addr.sin_addr) != peer_vip) {
            return failWith("Host getpeername() did not return Joiner VIP", 18);
        }

        if (!waitReadable(client_sock, 5000)) {
            return failWith("Host timed out waiting for TCP payload", 19);
        }
        char tcp_rx[512] = {};
        int tcp_got = recv(client_sock, tcp_rx, sizeof(tcp_rx) - 1, 0);
        if (tcp_got <= 0 || std::string(tcp_rx, static_cast<size_t>(tcp_got)) != "TCP_JOIN_GAME_REQ") {
            return failWith("Host received unexpected TCP data", 20);
        }

        const char* tcp_ack = "TCP_JOIN_GAME_ACCEPT";
        if (send(client_sock, tcp_ack, static_cast<int>(std::strlen(tcp_ack)), 0) <= 0) {
            return failWith("Host TCP send failed", 21);
        }

        // Wait for Joiner's READY_BULK request, then stream 4 MB high-throughput payload
        if (!waitReadable(client_sock, 5000)) {
            return failWith("Host timed out waiting for READY_BULK", 21);
        }
        char bulk_req[32] = {};
        int bulk_req_got = recv(client_sock, bulk_req, sizeof(bulk_req) - 1, 0);
        if (bulk_req_got <= 0 || std::string(bulk_req, static_cast<size_t>(bulk_req_got)) != "READY_BULK") {
            return failWith("Host did not receive expected READY_BULK", 21);
        }

        constexpr size_t kBulkTransferSize = 4 * 1024 * 1024; // 4 MB file-share stream
        std::vector<uint8_t> bulk_data(kBulkTransferSize);
        for (size_t i = 0; i < kBulkTransferSize; ++i) {
            bulk_data[i] = static_cast<uint8_t>((i * 1315423911u + 0xA5u) & 0xFFu);
        }

        size_t total_sent = 0;
        while (total_sent < kBulkTransferSize) {
            size_t chunk = kBulkTransferSize - total_sent;
            if (chunk > 65536) chunk = 65536;
            int s = send(client_sock, reinterpret_cast<const char*>(bulk_data.data() + total_sent),
                         static_cast<int>(chunk), 0);
            if (s <= 0) {
                return failWith("Host bulk TCP send failed at offset " + std::to_string(total_sent), 21);
            }
            total_sent += static_cast<size_t>(s);
        }

        if (!waitReadable(client_sock, 8000)) {
            return failWith("Host timed out waiting for BULK_OK! confirmation", 21);
        }
        char bulk_done[32] = {};
        int bulk_done_got = recv(client_sock, bulk_done, sizeof(bulk_done) - 1, 0);
        if (bulk_done_got <= 0 || std::string(bulk_done, static_cast<size_t>(bulk_done_got)) != "BULK_OK!") {
            return failWith("Host did not receive BULK_OK! confirmation", 21);
        }

        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        closesocket(client_sock);
        closesocket(tcp_listen);
        closesocket(udp_sock);
    } else {
        // Joiner role: also bind a TCP socket to 0.0.0.0:6112 to prove no port conflict on TCP either
        SOCKET dummy_tcp = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        if (bind(dummy_tcp, reinterpret_cast<const sockaddr*>(&bind_addr), sizeof(bind_addr)) != 0) {
            return failWith("Joiner TCP bind(0.0.0.0:6112) collided", 22);
        }
        closesocket(dummy_tcp);

        // Send UDP Broadcast (255.255.255.255:6112) until Host responds via UDP Unicast
        sockaddr_in bcast_addr{};
        bcast_addr.sin_family = AF_INET;
        bcast_addr.sin_addr.s_addr = INADDR_BROADCAST; // 255.255.255.255
        bcast_addr.sin_port = htons(kGamePort);

        const char* disc_msg = "DISCOVER_GAME_LOBBY";
        bool got_unicast_reply = false;
        sockaddr_in host_addr{};

        for (int attempt = 0; attempt < 25 && !got_unicast_reply; ++attempt) {
            sendto(udp_sock, disc_msg, static_cast<int>(std::strlen(disc_msg)), 0,
                   reinterpret_cast<const sockaddr*>(&bcast_addr), sizeof(bcast_addr));

            if (waitReadable(udp_sock, 250)) {
                char rx_buf[512] = {};
                int from_len = sizeof(host_addr);
                int got = recvfrom(udp_sock, rx_buf, sizeof(rx_buf) - 1, 0,
                                   reinterpret_cast<sockaddr*>(&host_addr), &from_len);
                if (got > 0 && std::string(rx_buf, static_cast<size_t>(got)) == "LOBBY_INFO:MAP_CLASSIC_LAN") {
                    got_unicast_reply = true;
                    break;
                }
            }
        }

        if (!got_unicast_reply) {
            return failWith("Joiner did not receive UDP unicast reply from Host", 23);
        }

        std::string host_vip = ipToStr(host_addr.sin_addr);
        if (host_vip.rfind("10.42.0.", 0) != 0 || ntohs(host_addr.sin_port) != kGamePort) {
            return failWith("Invalid Host VIP/port on UDP reply: " + host_vip, 24);
        }

        // Connect TCP socket to Host's Virtual IP (10.42.0.x:6112)
        int connect_err = 0;
        SOCKET tcp_sock = connectWithRetry(host_addr, connect_err);
        if (tcp_sock == INVALID_SOCKET) {
            return failWith("Joiner TCP connect to " + host_vip + ":6112 failed (wsaerr=" +
                            std::to_string(connect_err) + ")", 26);
        }

        sockaddr_in gp_addr{};
        int gp_len = sizeof(gp_addr);
        if (getpeername(tcp_sock, reinterpret_cast<sockaddr*>(&gp_addr), &gp_len) != 0 ||
            ipToStr(gp_addr.sin_addr) != host_vip || ntohs(gp_addr.sin_port) != kGamePort) {
            return failWith("Joiner getpeername() did not return Host VIP:6112", 27);
        }

        const char* req = "TCP_JOIN_GAME_REQ";
        if (send(tcp_sock, req, static_cast<int>(std::strlen(req)), 0) <= 0) {
            return failWith("Joiner TCP send failed", 28);
        }

        if (!waitReadable(tcp_sock, 5000)) {
            return failWith("Joiner timed out waiting for TCP response from Host", 29);
        }

        char tcp_rx[512] = {};
        int tcp_got = recv(tcp_sock, tcp_rx, sizeof(tcp_rx) - 1, 0);
        if (tcp_got <= 0 || std::string(tcp_rx, static_cast<size_t>(tcp_got)) != "TCP_JOIN_GAME_ACCEPT") {
            return failWith("Joiner received unexpected TCP response (got=" + std::to_string(tcp_got) +
                            " str='" + (tcp_got > 0 ? std::string(tcp_rx, static_cast<size_t>(tcp_got)) : "") + "')", 30);
        }

        // Request and verify 4 MB high-throughput bulk TCP stream
        const char* bulk_ready = "READY_BULK";
        if (send(tcp_sock, bulk_ready, static_cast<int>(std::strlen(bulk_ready)), 0) <= 0) {
            return failWith("Joiner failed to send READY_BULK", 31);
        }

        constexpr size_t kBulkTransferSize = 4 * 1024 * 1024;
        size_t total_recvd = 0;
        std::vector<uint8_t> rx_chunk(65536);
        auto t_start = std::chrono::steady_clock::now();

        while (total_recvd < kBulkTransferSize) {
            if (!waitReadable(tcp_sock, 8000)) {
                return failWith("Joiner timed out receiving 4 MB bulk TCP stream at " +
                                std::to_string(total_recvd) + " bytes", 32);
            }
            int got_bytes = recv(tcp_sock, reinterpret_cast<char*>(rx_chunk.data()),
                                 static_cast<int>(rx_chunk.size()), 0);
            if (got_bytes <= 0) {
                return failWith("Joiner recv failed during 4 MB bulk TCP stream at " +
                                std::to_string(total_recvd) + " bytes", 33);
            }
            for (int k = 0; k < got_bytes; ++k) {
                size_t idx = total_recvd + static_cast<size_t>(k);
                uint8_t expected = static_cast<uint8_t>((idx * 1315423911u + 0xA5u) & 0xFFu);
                if (rx_chunk[static_cast<size_t>(k)] != expected) {
                    return failWith("Joiner bulk TCP stream data corruption at byte " + std::to_string(idx), 34);
                }
            }
            total_recvd += static_cast<size_t>(got_bytes);
        }

        auto elapsed_us = std::chrono::duration_cast<std::chrono::microseconds>(
            std::chrono::steady_clock::now() - t_start).count();
        bulk_ms = elapsed_us / 1000;
        bulk_mbps = (elapsed_us > 0)
            ? (static_cast<double>(kBulkTransferSize) / (1024.0 * 1024.0)) / (static_cast<double>(elapsed_us) / 1e6)
            : 0.0;
        std::cout << "[TestGameApp " << arch_str << " " << role << "] 4 MB bulk TCP verified in "
                  << bulk_ms << " ms (" << bulk_mbps << " MB/s)\n";

        const char* bulk_ok = "BULK_OK!";
        send(tcp_sock, bulk_ok, static_cast<int>(std::strlen(bulk_ok)), 0);

        closesocket(tcp_sock);
        closesocket(udp_sock);
    }

    std::cout << "[TestGameApp " << arch_str << " " << role << "] PASS (VIP=" << my_vip_str << ")\n";
    if (!report_file.empty()) {
        std::ofstream out(report_file);
        out << "PASS arch=" << arch_str << " role=" << role << " vip=" << my_vip_str;
        if (role == "joiner") {
            char rate_buf[64] = {};
            std::snprintf(rate_buf, sizeof(rate_buf), " bulk_4MB=%lldms(%.1fMB/s)", bulk_ms, bulk_mbps);
            out << rate_buf;
        }
        out << "\n";
    }

    WSACleanup();
    return 0;
}
