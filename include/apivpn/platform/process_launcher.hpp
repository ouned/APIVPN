#pragma once

#include <cstdint>
#include <map>
#include <string>
#include <vector>

namespace apivpn::platform {

enum class BinaryArch {
    Unknown = 0,
    X86_32  = 32,
    X86_64  = 64,
};

struct LaunchedProcessInfo {
    uint32_t    pid                = 0;
    uint32_t    parent_pid         = 0;
    BinaryArch  arch               = BinaryArch::Unknown;
    std::string exe_path;
    std::string args;
    std::string working_dir;
    uint32_t    injected_vip       = 0;
    uint64_t    launched_since_us  = 0;
    int64_t     launched_epoch_sec = 0;
    bool        success            = false;
    std::string error_msg;
};

class ProcessLauncher {
public:
    // Inspect executable header (PE on Windows, ELF on Linux) to determine 32-bit vs 64-bit
    static BinaryArch detectExecutableArch(const std::string& exe_path);

    // Inspect a running process by PID to determine 32-bit vs 64-bit
    static BinaryArch detectProcessArch(uint32_t pid);

    // Directory containing the current module/executable
    static std::string getCurrentModuleDirectory();

    // Launch an application suspended, inject the matching Hook library (32-bit or 64-bit),
    // pass the APIVPN environment variables, and resume execution.
    static LaunchedProcessInfo launchHookedProcess(
        const std::string& exe_path,
        const std::string& args,
        const std::string& working_dir,
        const std::map<std::string, std::string>& env_vars,
        const std::string& bin_dir = "");

    // Inject the matching Hook library into an already-created suspended process by PID
    static bool injectIntoProcess(uint32_t pid, const std::string& bin_dir = "");

    // Check if process is still running
    static bool isProcessAlive(uint32_t pid);

    // Check if process is running; if exited, retrieves its exit code when available
    static bool getProcessStatus(uint32_t pid, bool& out_alive, uint32_t& out_exit_code);

    // Query process image path and parent PID for an existing process
    static bool queryProcessDetails(uint32_t pid, std::string& out_exe_path, uint32_t& out_parent_pid);

    // Returns true if the executable is a background OS/hook helper (e.g. dplaysvr.exe, conhost.exe, injector)
    static bool isBackgroundHelperExecutable(const std::string& exe_path);

    // Find alive non-helper child/descendant processes of the given parent PIDs
    static std::vector<uint32_t> findAliveChildProcesses(
        const std::vector<uint32_t>& parent_pids,
        int64_t min_creation_epoch_sec = 0);

    // Terminate process by PID
    static bool terminateProcess(uint32_t pid);
};

} // namespace apivpn::platform
