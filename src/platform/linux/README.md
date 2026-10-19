# Linux Platform Backend

APIVPN separates core virtualization and VPN routing from OS-specific primitives via `include/apivpn/platform/`.

- **`net_socket_linux.cpp`** *(Implemented)*: Implements `apivpn::platform::NetSocket` and `pollSockets()` using POSIX `socket()`, `bind()`, `connect()`, `sendto()`, `recvfrom()`, `fcntl(O_NONBLOCK)`, `getifaddrs()`, and `poll()`, enabling `apivpn_server` to build and run natively on Linux.

To extend Linux support to the client/hook side:
1. **`process_launcher_linux.cpp`**: Implement `ProcessLauncher::launchHookedProcess()` using `fork()` + `execve()` with `LD_PRELOAD=/path/$LIB/libapivpn_hook.so` and the `APIVPN_*` environment variables.
2. **`preload_linux.cpp`**: Intercept `bind`, `sendto`, `recvfrom`, `connect`, `listen`, `accept`, `getsockname`, `getpeername`, and `getifaddrs` via ELF symbol interposition (`dlsym(RTLD_NEXT, ...)`), forwarding directly into the portable `apivpn::hook::SocketShimCore`.
