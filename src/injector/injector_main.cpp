#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif

#include "apivpn/platform/win32_utf8.hpp" // includes winsock2.h then windows.h

#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

namespace {

// Minimal command-line tokenizer following the CommandLineToArgvW backslash/quote
// rules, so we do not have to link shell32 (the injector links nothing extra).
// argv[0] (the program name) uses the simpler "quote-delimited, no escaping" rule.
std::vector<std::wstring> parseCommandLineW(const wchar_t* cmdline) {
    std::vector<std::wstring> argv;
    const wchar_t* p = cmdline;

    // argv[0]: delimited by whitespace, or by quotes with no backslash escaping.
    std::wstring arg0;
    if (*p == L'"') {
        ++p;
        while (*p && *p != L'"') arg0 += *p++;
        if (*p == L'"') ++p;
    } else {
        while (*p && *p != L' ' && *p != L'\t') arg0 += *p++;
    }
    argv.push_back(arg0);

    for (;;) {
        while (*p == L' ' || *p == L'\t') ++p;
        if (*p == L'\0') break;

        std::wstring arg;
        bool in_quotes = false;
        for (;;) {
            if (*p == L'\0') break;
            if (!in_quotes && (*p == L' ' || *p == L'\t')) break;

            if (*p == L'\\') {
                size_t backslashes = 0;
                while (*p == L'\\') { ++backslashes; ++p; }
                if (*p == L'"') {
                    arg.append(backslashes / 2, L'\\');
                    if (backslashes & 1) {
                        arg += L'"'; // escaped literal quote
                        ++p;
                    } else {
                        in_quotes = !in_quotes;
                        ++p;
                    }
                } else {
                    arg.append(backslashes, L'\\');
                }
            } else if (*p == L'"') {
                if (in_quotes && p[1] == L'"') {
                    arg += L'"'; // "" inside a quoted run -> literal quote
                    p += 2;
                } else {
                    in_quotes = !in_quotes;
                    ++p;
                }
            } else {
                arg += *p++;
            }
        }
        argv.push_back(arg);
    }

    return argv;
}

} // namespace

int main() {
    // Parse our own command line as UTF-16 and convert to UTF-8, so paths with
    // non-ANSI characters survive (the C-runtime argv would be ANSI-decoded).
    std::vector<std::wstring> wargv = parseCommandLineW(GetCommandLineW());
    std::vector<std::string> args;
    args.reserve(wargv.size());
    for (const auto& w : wargv) {
        args.push_back(apivpn::platform::wideToUtf8(w));
    }
    const int argc = static_cast<int>(args.size());

    DWORD pid = 0;
    std::string dll_path;

    for (int i = 1; i < argc; ++i) {
        if (args[i] == "--inject-pid" && i + 1 < argc) {
            pid = static_cast<DWORD>(std::strtoul(args[++i].c_str(), nullptr, 10));
        } else if (args[i] == "--dll" && i + 1 < argc) {
            dll_path = args[++i];
        }
    }

    if (pid == 0 || dll_path.empty()) {
        std::fprintf(stderr, "Usage: %s --inject-pid <pid> --dll <dll_path>\n",
                     argc > 0 ? args[0].c_str() : "apivpn_injector");
        return 1;
    }

    HANDLE hProc = OpenProcess(
        PROCESS_CREATE_THREAD | PROCESS_QUERY_INFORMATION |
        PROCESS_VM_OPERATION | PROCESS_VM_WRITE | PROCESS_VM_READ,
        FALSE, pid);
    if (!hProc) {
        std::fprintf(stderr, "OpenProcess(%lu) failed: %lu\n", pid, GetLastError());
        return 2;
    }

    std::wstring wpath = apivpn::platform::utf8ToWide(dll_path);
    if (wpath.empty()) {
        CloseHandle(hProc);
        return 3;
    }
    size_t alloc_bytes = (wpath.size() + 1) * sizeof(wchar_t);

    void* remote_mem = VirtualAllocEx(hProc, nullptr, alloc_bytes, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    if (!remote_mem) {
        CloseHandle(hProc);
        return 4;
    }

    SIZE_T written = 0;
    if (!WriteProcessMemory(hProc, remote_mem, wpath.c_str(), alloc_bytes, &written) || written != alloc_bytes) {
        VirtualFreeEx(hProc, remote_mem, 0, MEM_RELEASE);
        CloseHandle(hProc);
        return 5;
    }

    HMODULE hKernel32 = GetModuleHandleW(L"kernel32.dll");
    auto load_lib = reinterpret_cast<LPTHREAD_START_ROUTINE>(
        reinterpret_cast<void*>(GetProcAddress(hKernel32, "LoadLibraryW")));
    if (!load_lib) {
        VirtualFreeEx(hProc, remote_mem, 0, MEM_RELEASE);
        CloseHandle(hProc);
        return 6;
    }

    HANDLE hThread = CreateRemoteThread(hProc, nullptr, 0, load_lib, remote_mem, 0, nullptr);
    if (!hThread) {
        std::fprintf(stderr, "CreateRemoteThread failed: %lu\n", GetLastError());
        VirtualFreeEx(hProc, remote_mem, 0, MEM_RELEASE);
        CloseHandle(hProc);
        return 7;
    }

    DWORD wait_rc = WaitForSingleObject(hThread, 10000);
    if (wait_rc != WAIT_OBJECT_0) {
        // Timed out (or wait failed): the remote thread may still be running and
        // LoadLibraryW may still read the path buffer, so do NOT free remote_mem
        // (deliberate leak) and report failure.
        std::fprintf(stderr, "Remote LoadLibraryW did not complete (wait rc %lu)\n", wait_rc);
        CloseHandle(hThread);
        CloseHandle(hProc);
        return 8;
    }

    DWORD exit_code = 0;
    BOOL got_code = GetExitCodeThread(hThread, &exit_code);

    CloseHandle(hThread);
    VirtualFreeEx(hProc, remote_mem, 0, MEM_RELEASE);
    CloseHandle(hProc);

    // LoadLibraryW returns the module HMODULE (truncated to 32 bits in the thread
    // exit code). Zero means the library failed to load. STILL_ACTIVE would mean
    // the thread never finished, which we already excluded above.
    if (!got_code || exit_code == 0 || exit_code == STILL_ACTIVE) {
        return 9;
    }
    return 0;
}
