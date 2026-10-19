@echo off
setlocal enabledelayedexpansion

echo ============================================================
echo Building APIVPN 32-bit (x86) Targets...
echo ============================================================
cmake -B build32 -G "Visual Studio 17 2022" -A Win32
if errorlevel 1 exit /b 1
cmake --build build32 --config Release
if errorlevel 1 exit /b 1

echo ============================================================
echo Building APIVPN 64-bit (x64) Targets...
echo ============================================================
cmake -B build64 -G "Visual Studio 17 2022" -A x64
if errorlevel 1 exit /b 1
cmake --build build64 --config Release
if errorlevel 1 exit /b 1

echo ============================================================
echo All x86 and x64 binaries built successfully in .\bin\
echo ============================================================
dir /b bin\*.exe bin\*.dll
