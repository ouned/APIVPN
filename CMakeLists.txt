cmake_minimum_required(VERSION 3.20)
project(APIVPN LANGUAGES CXX)

# The Windows client embeds assets/apivpn.rc, so the resource compiler must be
# an enabled language.  RC is unavailable off Windows, hence the guard.
if(WIN32)
    enable_language(RC)
endif()

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Emit compile_commands.json for clangd / editor tooling.
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# Opt-in: treat compiler warnings as errors (useful in CI).
option(APIVPN_WERROR "Treat compiler warnings as errors" OFF)

# Place all build artifacts in ${CMAKE_SOURCE_DIR}/bin so 32-bit and 64-bit
# binaries sit side-by-side for seamless cross-bitness injection.
set(APIVPN_BIN_DIR "${CMAKE_SOURCE_DIR}/bin")
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY "${APIVPN_BIN_DIR}")
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY "${APIVPN_BIN_DIR}")
set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/lib")

foreach(CFG DEBUG RELEASE RELWITHDEBINFO MINSIZEREL)
    set(CMAKE_RUNTIME_OUTPUT_DIRECTORY_${CFG} "${APIVPN_BIN_DIR}")
    set(CMAKE_LIBRARY_OUTPUT_DIRECTORY_${CFG} "${APIVPN_BIN_DIR}")
    set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY_${CFG} "${CMAKE_BINARY_DIR}/lib")
endforeach()

if(MSVC)
    # Statically link the MSVC C/C++ runtime (/MT and /MTd) so binaries and
    # hook DLLs have zero dependency on VC++ Redistributable DLLs.
    set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
    add_compile_options(/W4 /wd4100 /MP /EHsc /D_CRT_SECURE_NO_WARNINGS /D_WINSOCK_DEPRECATED_NO_WARNINGS)
else()
    add_compile_options(-Wall -Wextra -Wno-unused-parameter -Wshadow -Wnon-virtual-dtor)
    if(MINGW)
        # Do not prefix DLLs/import libs with "lib" so output matches apivpn_hook32.dll / apivpn_hook64.dll
        set(CMAKE_SHARED_LIBRARY_PREFIX "")
        set(CMAKE_STATIC_LIBRARY_PREFIX "")
        # Statically link libgcc, libstdc++, and winpthreads so executables and
        # injected hook DLLs have zero external MinGW runtime DLL dependencies.
        add_link_options(-static -static-libgcc -static-libstdc++)
    endif()
endif()

if(APIVPN_WERROR)
    if(MSVC)
        add_compile_options(/WX)
    else()
        add_compile_options(-Werror)
    endif()
endif()

include_directories("${CMAKE_SOURCE_DIR}/include")

if(CMAKE_SIZEOF_VOID_P EQUAL 8)
    set(ARCH_SUFFIX "64")
else()
    set(ARCH_SUFFIX "32")
endif()

if(WIN32)
    # The code uses Vista+ and Windows 10 APIs (CompareStringOrdinal, GetTickCount64,
    # WM_DPICHANGED, ChangeWindowMessageFilterEx). MSVC's SDK defaults to the latest
    # version, but MinGW-w64's default _WIN32_WINNT varies by release, so pin it to the
    # supported minimum (Windows 10) for every target.
    add_compile_definitions(_WIN32_WINNT=0x0A00 WINVER=0x0A00)

    # ============================================================================
    # 1. Platform & Core Static Library (Windows)
    # ============================================================================
    add_library(apivpn_platform STATIC
        src/platform/win32/net_socket_win32.cpp
        src/platform/win32/process_launcher_win32.cpp
        src/platform/win32/inline_detour_win32.cpp
    )
    target_link_libraries(apivpn_platform PUBLIC ws2_32 iphlpapi bcrypt)

    # ============================================================================
    # 2. Hook DLL (Built for both 32-bit x86 and 64-bit x64)
    # ============================================================================
    add_library(apivpn_hook${ARCH_SUFFIX} SHARED
        src/hook/socket_shim_core.cpp
        src/hook/win32/dllmain_win32.cpp
    )
    target_link_libraries(apivpn_hook${ARCH_SUFFIX} PRIVATE apivpn_platform ws2_32 iphlpapi)

    # ============================================================================
    # 3. Injector Helper Executable (Built for both 32-bit x86 and 64-bit x64)
    # ============================================================================
    add_executable(apivpn_injector${ARCH_SUFFIX}
        src/injector/injector_main.cpp
    )

    # ============================================================================
    # 4. Test Game Application (Built for both 32-bit x86 and 64-bit x64)
    # ============================================================================
    add_executable(test_game_app${ARCH_SUFFIX}
        tests/test_game_app.cpp
    )
    target_link_libraries(test_game_app${ARCH_SUFFIX} PRIVATE ws2_32 iphlpapi ole32 user32)

    # ============================================================================
    # 5. Primary 64-bit Targets: VPN Server, Client GUI/CLI, and Integration Runner
    # ============================================================================
    if(CMAKE_SIZEOF_VOID_P EQUAL 8)
        add_library(apivpn_core STATIC
            src/server/vpn_server.cpp
            src/client/vpn_client.cpp
            src/client/vpn_client_process.cpp
            src/client/vpn_client_ipc.cpp
            src/client/vpn_client_network.cpp
            src/client/vpn_client_holepunch.cpp
            src/client/vpn_client_gateway.cpp
            src/client/vpn_client_tcp.cpp
        )
        target_link_libraries(apivpn_core PUBLIC apivpn_platform)

        add_executable(apivpn_server
            src/server/server_main.cpp
        )
        target_link_libraries(apivpn_server PRIVATE apivpn_core)

        add_executable(apivpn_client WIN32
            src/client/client_main.cpp
            src/client/gui_win32.cpp
            src/client/gui_theme.cpp
            src/client/gui_panels.cpp
            src/client/gui_settings.cpp
            src/client/gui_tray.cpp
            assets/apivpn.rc
            third_party/imgui/imgui.cpp
            third_party/imgui/imgui_draw.cpp
            third_party/imgui/imgui_tables.cpp
            third_party/imgui/imgui_widgets.cpp
            third_party/imgui/imgui_impl_win32.cpp
            third_party/imgui/imgui_impl_dx11.cpp
        )
        target_include_directories(apivpn_client PRIVATE
            "${CMAKE_SOURCE_DIR}/third_party/imgui"
            "${CMAKE_SOURCE_DIR}/src/client"
        )
        target_link_libraries(apivpn_client PRIVATE apivpn_core comctl32 comdlg32 user32 gdi32 shell32 d3d11 d3dcompiler dxgi dwmapi)

        add_executable(integration_test_runner
            tests/integration_test_runner.cpp
        )
        target_link_libraries(integration_test_runner PRIVATE apivpn_core)
    endif()
else()
    # ============================================================================
    # Linux / POSIX Targets (VPN Server)
    # ============================================================================
    find_package(Threads REQUIRED)

    add_library(apivpn_platform STATIC
        src/platform/linux/net_socket_linux.cpp
    )
    target_link_libraries(apivpn_platform PUBLIC Threads::Threads)

    add_library(apivpn_core STATIC
        src/server/vpn_server.cpp
    )
    target_link_libraries(apivpn_core PUBLIC apivpn_platform)

    add_executable(apivpn_server
        src/server/server_main.cpp
    )
    target_link_libraries(apivpn_server PRIVATE apivpn_core)
endif()
