#pragma once

// UTF-8 <-> UTF-16 helpers for Win32 code.
//
// Convention: every std::string that holds a path, command line, or other OS text is UTF-8.
// Win32 calls always go through the W APIs, converting at the boundary with these helpers.
// Never pass such a string to an A API (those interpret bytes in the ANSI code page) and
// never decode an A API result as UTF-8.

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <winsock2.h> // must precede windows.h in any TU that also uses Winsock
#include <windows.h>

#include <cwchar>
#include <string>

namespace apivpn::platform {

inline std::wstring utf8ToWide(const std::string& s) {
    if (s.empty()) return std::wstring();
    int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0);
    if (n <= 0) return std::wstring();
    std::wstring out(static_cast<size_t>(n), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), out.data(), n);
    return out;
}

inline std::string wideToUtf8(const wchar_t* s, size_t len) {
    if (!s || len == 0) return std::string();
    int n = WideCharToMultiByte(CP_UTF8, 0, s, static_cast<int>(len), nullptr, 0, nullptr, nullptr);
    if (n <= 0) return std::string();
    std::string out(static_cast<size_t>(n), '\0');
    WideCharToMultiByte(CP_UTF8, 0, s, static_cast<int>(len), out.data(), n, nullptr, nullptr);
    return out;
}

inline std::string wideToUtf8(const std::wstring& s) {
    return wideToUtf8(s.data(), s.size());
}

inline std::string wideToUtf8(const wchar_t* s) {
    return s ? wideToUtf8(s, wcslen(s)) : std::string();
}

// GetModuleFileNameW without the MAX_PATH limit. Returns UTF-8, empty on failure.
inline std::string moduleFileNameUtf8(HMODULE module) {
    std::wstring buf(MAX_PATH, L'\0');
    for (;;) {
        DWORD n = GetModuleFileNameW(module, buf.data(), static_cast<DWORD>(buf.size()));
        if (n == 0) return std::string();
        if (n < buf.size()) {
            buf.resize(n);
            return wideToUtf8(buf);
        }
        if (buf.size() >= 32768) return std::string();
        buf.resize(buf.size() * 2);
    }
}

// GetEnvironmentVariableW as UTF-8. Returns false if the variable is not set.
inline bool getEnvUtf8(const char* name_ascii, std::string& out) {
    std::wstring wname = utf8ToWide(name_ascii);
    DWORD n = GetEnvironmentVariableW(wname.c_str(), nullptr, 0);
    if (n == 0) return false;
    std::wstring buf(n, L'\0');
    DWORD got = GetEnvironmentVariableW(wname.c_str(), buf.data(), n);
    if (got == 0 || got >= n) return false;
    buf.resize(got);
    out = wideToUtf8(buf);
    return true;
}

} // namespace apivpn::platform
