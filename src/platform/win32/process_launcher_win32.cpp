#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif

#include "apivpn/platform/win32_utf8.hpp" // includes winsock2.h then windows.h
#include <tlhelp32.h>
#include "apivpn/platform/process_launcher.hpp"
#include "apivpn/common/protocol.hpp"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <map>
#include <string>
#include <unordered_set>
#include <vector>

namespace apivpn::platform {

namespace {

// Wait for a remote thread and return true only if it finished with a non-zero
// exit code. On WAIT_TIMEOUT (or wait failure) the remote thread may still be
// running and LoadLibraryW may still read the path buffer, so the caller must
// deliberately leak the remote buffer; this helper only reports success/failure.
bool remoteThreadLoadedOk(HANDLE hThread, bool& out_timed_out) {
    out_timed_out = false;
    DWORD wait_rc = WaitForSingleObject(hThread, 10000);
    if (wait_rc != WAIT_OBJECT_0) {
        out_timed_out = true;
        return false;
    }
    DWORD exit_code = 0;
    if (!GetExitCodeThread(hThread, &exit_code)) return false;
    // LoadLibraryW returns the module handle (truncated to 32 bits in the thread
    // exit code); zero means the DLL failed to load.
    return exit_code != 0 && exit_code != STILL_ACTIVE;
}

bool injectSameBitnessDll(DWORD pid, const std::string& dll_path) {
    HANDLE hProc = OpenProcess(
        PROCESS_CREATE_THREAD | PROCESS_QUERY_INFORMATION |
        PROCESS_VM_OPERATION | PROCESS_VM_WRITE | PROCESS_VM_READ,
        FALSE, pid);
    if (!hProc) return false;

    std::wstring wpath = utf8ToWide(dll_path);
    if (wpath.empty()) {
        CloseHandle(hProc);
        return false;
    }
    size_t alloc_bytes = (wpath.size() + 1) * sizeof(wchar_t);

    void* remote_mem = VirtualAllocEx(hProc, nullptr, alloc_bytes, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    if (!remote_mem) {
        CloseHandle(hProc);
        return false;
    }

    SIZE_T written = 0;
    if (!WriteProcessMemory(hProc, remote_mem, wpath.c_str(), alloc_bytes, &written) || written != alloc_bytes) {
        VirtualFreeEx(hProc, remote_mem, 0, MEM_RELEASE);
        CloseHandle(hProc);
        return false;
    }

    HMODULE hKernel32 = GetModuleHandleW(L"kernel32.dll");
    auto load_lib_addr = reinterpret_cast<LPTHREAD_START_ROUTINE>(
        reinterpret_cast<void*>(GetProcAddress(hKernel32, "LoadLibraryW")));
    if (!load_lib_addr) {
        VirtualFreeEx(hProc, remote_mem, 0, MEM_RELEASE);
        CloseHandle(hProc);
        return false;
    }

    HANDLE hThread = CreateRemoteThread(hProc, nullptr, 0, load_lib_addr, remote_mem, 0, nullptr);
    if (!hThread) {
        VirtualFreeEx(hProc, remote_mem, 0, MEM_RELEASE);
        CloseHandle(hProc);
        return false;
    }

    bool timed_out = false;
    bool ok = remoteThreadLoadedOk(hThread, timed_out);
    CloseHandle(hThread);
    if (!timed_out) {
        VirtualFreeEx(hProc, remote_mem, 0, MEM_RELEASE);
    }
    // else: leak remote_mem on purpose; the thread may still read it.
    CloseHandle(hProc);
    return ok;
}

bool fileExists(const std::string& path) {
    DWORD attr = GetFileAttributesW(utf8ToWide(path).c_str());
    return (attr != INVALID_FILE_ATTRIBUTES && !(attr & FILE_ATTRIBUTE_DIRECTORY));
}

} // namespace

BinaryArch ProcessLauncher::detectExecutableArch(const std::string& exe_path) {
    HANDLE hFile = CreateFileW(utf8ToWide(exe_path).c_str(), GENERIC_READ,
                               FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                               nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (hFile == INVALID_HANDLE_VALUE) return BinaryArch::Unknown;

    auto readAt = [&](LONG offset, void* buf, DWORD size) -> bool {
        LARGE_INTEGER li{};
        li.QuadPart = offset;
        if (!SetFilePointerEx(hFile, li, nullptr, FILE_BEGIN)) return false;
        DWORD got = 0;
        return ReadFile(hFile, buf, size, &got, nullptr) && got == size;
    };

    BinaryArch result = BinaryArch::Unknown;
    IMAGE_DOS_HEADER dos_hdr{};
    if (readAt(0, &dos_hdr, sizeof(dos_hdr)) && dos_hdr.e_magic == IMAGE_DOS_SIGNATURE) {
        DWORD nt_sig = 0;
        if (readAt(dos_hdr.e_lfanew, &nt_sig, sizeof(nt_sig)) && nt_sig == IMAGE_NT_SIGNATURE) {
            IMAGE_FILE_HEADER file_hdr{};
            DWORD got = 0;
            if (ReadFile(hFile, &file_hdr, sizeof(file_hdr), &got, nullptr) && got == sizeof(file_hdr)) {
                if (file_hdr.Machine == IMAGE_FILE_MACHINE_I386) {
                    result = BinaryArch::X86_32;
                } else if (file_hdr.Machine == IMAGE_FILE_MACHINE_AMD64) {
                    result = BinaryArch::X86_64;
                }
            }
        }
    }

    CloseHandle(hFile);
    return result;
}

BinaryArch ProcessLauncher::detectProcessArch(uint32_t pid) {
    HANDLE hProc = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (!hProc) return BinaryArch::Unknown;

    BOOL is_wow64 = FALSE;
    if (!IsWow64Process(hProc, &is_wow64)) {
        CloseHandle(hProc);
        return BinaryArch::Unknown;
    }
    CloseHandle(hProc);

    SYSTEM_INFO si{};
    GetNativeSystemInfo(&si);
    if (si.wProcessorArchitecture == PROCESSOR_ARCHITECTURE_AMD64) {
        return is_wow64 ? BinaryArch::X86_32 : BinaryArch::X86_64;
    }
    return BinaryArch::X86_32;
}

std::string ProcessLauncher::getCurrentModuleDirectory() {
    HMODULE hMod = nullptr;
    GetModuleHandleExW(
        GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
        reinterpret_cast<LPCWSTR>(&ProcessLauncher::getCurrentModuleDirectory),
        &hMod);
    std::string s = moduleFileNameUtf8(hMod);
    if (s.empty()) return ".";
    size_t pos = s.find_last_of("\\/");
    return (pos != std::string::npos) ? s.substr(0, pos) : ".";
}

bool ProcessLauncher::injectIntoProcess(uint32_t pid, const std::string& bin_dir_arg) {
    BinaryArch target_arch = detectProcessArch(pid);
    if (target_arch == BinaryArch::Unknown) return false;

    std::string bin_dir = bin_dir_arg;
    if (bin_dir.empty()) {
        std::string env_val;
        if (getEnvUtf8(kEnvBinDir, env_val) && !env_val.empty()) {
            bin_dir = env_val;
        } else {
            bin_dir = getCurrentModuleDirectory();
        }
    }

    const bool target_is_64 = (target_arch == BinaryArch::X86_64);
    std::string dll_name = target_is_64 ? "apivpn_hook64.dll" : "apivpn_hook32.dll";
    std::string dll_path = bin_dir + "\\" + dll_name;
    if (!fileExists(dll_path)) {
        return false;
    }

#if defined(_M_X64) || defined(__x86_64__)
    constexpr bool self_is_64 = true;
#else
    constexpr bool self_is_64 = false;
#endif

    // If current process has the exact same bitness as the target process, inject directly
    if (self_is_64 == target_is_64) {
        if (injectSameBitnessDll(pid, dll_path)) {
            return true;
        }
    }

    // Otherwise delegate to the matching bitness injector helper executable
    std::string injector_exe = bin_dir + "\\" + (target_is_64 ? "apivpn_injector64.exe" : "apivpn_injector32.exe");
    if (!fileExists(injector_exe)) {
        return false;
    }

    std::string cmd = "\"" + injector_exe + "\" --inject-pid " + std::to_string(pid) + " --dll \"" + dll_path + "\"";
    STARTUPINFOW si{};
    si.cb = sizeof(si);
    PROCESS_INFORMATION pi{};
    std::wstring wcmd = utf8ToWide(cmd);
    std::vector<wchar_t> cmd_buf(wcmd.begin(), wcmd.end());
    cmd_buf.push_back(L'\0');
    std::wstring wbin_dir = utf8ToWide(bin_dir);

    if (!CreateProcessW(nullptr, cmd_buf.data(), nullptr, nullptr, FALSE,
                        CREATE_NO_WINDOW, nullptr,
                        wbin_dir.empty() ? nullptr : wbin_dir.c_str(), &si, &pi)) {
        return false;
    }

    DWORD wait_rc = WaitForSingleObject(pi.hProcess, 10000);
    bool ok = false;
    if (wait_rc == WAIT_OBJECT_0) {
        DWORD exit_code = 1;
        if (GetExitCodeProcess(pi.hProcess, &exit_code)) {
            ok = (exit_code == 0);
        }
    }
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    return ok;
}

// Build a UTF-16 environment block for CreateProcessW: the current process
// environment with the APIVPN_* overrides added/replaced, sorted case-insensitively
// by variable name as Windows requires. Leaves the caller's own environment intact.
static std::vector<wchar_t> buildChildEnvironmentBlock(
    const std::map<std::string, std::string>& overrides) {

    struct CiLessW {
        bool operator()(const std::wstring& a, const std::wstring& b) const {
            return CompareStringOrdinal(a.c_str(), static_cast<int>(a.size()),
                                        b.c_str(), static_cast<int>(b.size()),
                                        TRUE) == CSTR_LESS_THAN;
        }
    };
    // Map variable name -> full "NAME=VALUE" entry (preserving existing casing/content).
    std::map<std::wstring, std::wstring, CiLessW> entries;

    LPWCH env = GetEnvironmentStringsW();
    if (env) {
        for (const wchar_t* p = env; *p; ) {
            std::wstring entry(p);
            p += entry.size() + 1;
            // Names may legitimately start with '=' (e.g. "=C:"); find the name/value
            // split at the first '=' at or after index 1.
            size_t eq = entry.find(L'=', 1);
            std::wstring name = (eq == std::wstring::npos) ? entry : entry.substr(0, eq);
            entries[name] = entry;
        }
        FreeEnvironmentStringsW(env);
    }

    for (const auto& kv : overrides) {
        std::wstring wname = utf8ToWide(kv.first);
        entries[wname] = wname + L"=" + utf8ToWide(kv.second);
    }

    std::vector<wchar_t> block;
    for (const auto& e : entries) {
        block.insert(block.end(), e.second.begin(), e.second.end());
        block.push_back(L'\0');
    }
    block.push_back(L'\0'); // double-null terminator (also covers an empty block)
    return block;
}

LaunchedProcessInfo ProcessLauncher::launchHookedProcess(
    const std::string& exe_path,
    const std::string& args,
    const std::string& working_dir,
    const std::map<std::string, std::string>& env_vars,
    const std::string& bin_dir_arg) {

    LaunchedProcessInfo info;
    info.exe_path = exe_path;
    info.args = args;
    info.arch = detectExecutableArch(exe_path);

    if (info.arch == BinaryArch::Unknown) {
        info.error_msg = "Target is not a valid 32-bit or 64-bit Windows PE executable: " + exe_path;
        return info;
    }

    std::string bin_dir = bin_dir_arg.empty() ? getCurrentModuleDirectory() : bin_dir_arg;

    // Build an explicit child environment block (current env + APIVPN_* overrides).
    // The caller's own process environment is never modified.
    std::map<std::string, std::string> overrides = env_vars;
    overrides[kEnvBinDir] = bin_dir;
    std::vector<wchar_t> env_block = buildChildEnvironmentBlock(overrides);

    // Command line: quoted exe path followed by the raw args string.
    std::string cmd_line = "\"" + exe_path + "\"";
    if (!args.empty()) {
        cmd_line += " " + args;
    }
    std::wstring wcmd = utf8ToWide(cmd_line);
    std::vector<wchar_t> cmd_buf(wcmd.begin(), wcmd.end());
    cmd_buf.push_back(L'\0');

    STARTUPINFOW si{};
    si.cb = sizeof(si);
    PROCESS_INFORMATION pi{};

    std::string effective_work_dir = working_dir;
    if (effective_work_dir.empty()) {
        size_t pos = exe_path.find_last_of("\\/");
        if (pos != std::string::npos) {
            effective_work_dir = exe_path.substr(0, pos);
        }
    }
    info.working_dir = effective_work_dir;

    std::wstring wexe = utf8ToWide(exe_path);
    std::wstring wwork = utf8ToWide(effective_work_dir);

    BOOL created = CreateProcessW(
        wexe.c_str(),
        cmd_buf.data(),
        nullptr,
        nullptr,
        FALSE,
        CREATE_SUSPENDED | CREATE_NO_WINDOW | CREATE_NEW_PROCESS_GROUP | CREATE_UNICODE_ENVIRONMENT,
        env_block.data(),
        effective_work_dir.empty() ? nullptr : wwork.c_str(),
        &si,
        &pi);

    if (!created) {
        info.error_msg = "CreateProcessW failed with error " + std::to_string(GetLastError());
        return info;
    }

    info.pid = static_cast<uint32_t>(pi.dwProcessId);

    if (!injectIntoProcess(info.pid, bin_dir)) {
        TerminateProcess(pi.hProcess, 1);
        CloseHandle(pi.hThread);
        CloseHandle(pi.hProcess);
        info.error_msg = "Failed to inject APIVPN hook DLL into PID " + std::to_string(info.pid);
        return info;
    }

    if (ResumeThread(pi.hThread) == static_cast<DWORD>(-1)) {
        TerminateProcess(pi.hProcess, 1);
        CloseHandle(pi.hThread);
        CloseHandle(pi.hProcess);
        info.error_msg = "ResumeThread failed with error " + std::to_string(GetLastError());
        return info;
    }
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);

    info.success = true;
    return info;
}

bool ProcessLauncher::isProcessAlive(uint32_t pid) {
    HANDLE hProc = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (!hProc) return false;
    DWORD code = 0;
    BOOL ok = GetExitCodeProcess(hProc, &code);
    CloseHandle(hProc);
    return ok && (code == STILL_ACTIVE);
}

bool ProcessLauncher::getProcessStatus(uint32_t pid, bool& out_alive, uint32_t& out_exit_code) {
    out_alive = false;
    out_exit_code = 0;
    HANDLE hProc = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (!hProc) return false;
    DWORD code = 0;
    BOOL ok = GetExitCodeProcess(hProc, &code);
    CloseHandle(hProc);
    if (!ok) return false;
    if (code == STILL_ACTIVE) {
        out_alive = true;
    } else {
        out_alive = false;
        out_exit_code = static_cast<uint32_t>(code);
    }
    return true;
}

bool ProcessLauncher::queryProcessDetails(uint32_t pid, std::string& out_exe_path, uint32_t& out_parent_pid) {
    out_exe_path.clear();
    out_parent_pid = 0;
    if (pid == 0) return false;

    HANDLE hProc = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (hProc) {
        std::vector<wchar_t> full_path(32768); // Windows max path length (\\?\ extended)
        DWORD sz = static_cast<DWORD>(full_path.size());
        if (QueryFullProcessImageNameW(hProc, 0, full_path.data(), &sz)) {
            out_exe_path = wideToUtf8(full_path.data(), sz);
        }
        CloseHandle(hProc);
    }

    HANDLE hSnap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (hSnap != INVALID_HANDLE_VALUE) {
        PROCESSENTRY32W pe{};
        pe.dwSize = sizeof(pe);
        if (Process32FirstW(hSnap, &pe)) {
            do {
                if (pe.th32ProcessID == pid) {
                    out_parent_pid = pe.th32ParentProcessID;
                    if (out_exe_path.empty()) {
                        out_exe_path = wideToUtf8(pe.szExeFile);
                    }
                    break;
                }
            } while (Process32NextW(hSnap, &pe));
        }
        CloseHandle(hSnap);
    }

    return !out_exe_path.empty();
}

bool ProcessLauncher::isBackgroundHelperExecutable(const std::string& exe_path) {
    if (exe_path.empty()) return false;
    size_t slash = exe_path.find_last_of("\\/");
    const char* base = (slash != std::string::npos) ? (exe_path.c_str() + slash + 1) : exe_path.c_str();
    return _stricmp(base, "dplaysvr.exe") == 0 ||
           _stricmp(base, "conhost.exe") == 0 ||
           _stricmp(base, "apivpn_injector32.exe") == 0 ||
           _stricmp(base, "apivpn_injector64.exe") == 0 ||
           _stricmp(base, "werfault.exe") == 0 ||
           _stricmp(base, "rundll32.exe") == 0;
}

std::vector<uint32_t> ProcessLauncher::findAliveChildProcesses(
    const std::vector<uint32_t>& parent_pids,
    int64_t min_creation_epoch_sec) {

    std::vector<uint32_t> found_children;
    if (parent_pids.empty()) return found_children;

    HANDLE hSnap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (hSnap == INVALID_HANDLE_VALUE) return found_children;

    struct ProcNode {
        uint32_t    pid = 0;
        uint32_t    ppid = 0;
        std::string exe_name;
    };
    std::vector<ProcNode> all_procs;

    PROCESSENTRY32W pe{};
    pe.dwSize = sizeof(pe);
    DWORD self_pid = GetCurrentProcessId();
    if (Process32FirstW(hSnap, &pe)) {
        do {
            if (pe.th32ProcessID <= 4 || pe.th32ProcessID == self_pid) continue;
            all_procs.push_back({pe.th32ProcessID, pe.th32ParentProcessID, wideToUtf8(pe.szExeFile)});
        } while (Process32NextW(hSnap, &pe));
    }
    CloseHandle(hSnap);

    std::unordered_set<uint32_t> known_tree(parent_pids.begin(), parent_pids.end());
    std::unordered_set<uint32_t> initial_set(parent_pids.begin(), parent_pids.end());

    bool added = true;
    while (added) {
        added = false;
        for (const auto& pn : all_procs) {
            if (known_tree.count(pn.pid) != 0) continue;
            if (known_tree.count(pn.ppid) == 0) continue;

            // Verify the candidate process is still alive and check creation time against PID reuse
            HANDLE hProc = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pn.pid);
            if (!hProc) continue;

            DWORD exit_code = 0;
            bool alive = GetExitCodeProcess(hProc, &exit_code) && (exit_code == STILL_ACTIVE);
            bool valid_time = true;
            if (alive && min_creation_epoch_sec > 0) {
                FILETIME ftCreate{}, ftExit{}, ftKernel{}, ftUser{};
                if (GetProcessTimes(hProc, &ftCreate, &ftExit, &ftKernel, &ftUser)) {
                    ULARGE_INTEGER uli{};
                    uli.LowPart = ftCreate.dwLowDateTime;
                    uli.HighPart = ftCreate.dwHighDateTime;
                    constexpr uint64_t kEpochDiff100ns = 116444736000000000ULL;
                    if (uli.QuadPart >= kEpochDiff100ns) {
                        int64_t create_epoch = static_cast<int64_t>((uli.QuadPart - kEpochDiff100ns) / 10'000'000ULL);
                        if (create_epoch + 2 < min_creation_epoch_sec) {
                            valid_time = false;
                        }
                    }
                }
            }
            CloseHandle(hProc);

            if (!valid_time) continue;

            known_tree.insert(pn.pid);
            added = true;
            if (alive && initial_set.count(pn.pid) == 0 && !isBackgroundHelperExecutable(pn.exe_name)) {
                found_children.push_back(pn.pid);
            }
        }
    }

    return found_children;
}

bool ProcessLauncher::terminateProcess(uint32_t pid) {
    HANDLE hProc = OpenProcess(PROCESS_TERMINATE, FALSE, pid);
    if (!hProc) return false;
    BOOL ok = TerminateProcess(hProc, 0);
    CloseHandle(hProc);
    return ok != FALSE;
}

} // namespace apivpn::platform
