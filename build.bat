@echo off
rem Builds VoiceWindow.exe with MinGW-w64 / LLVM-MinGW (g++ must be in PATH).
cd /d %~dp0
g++ -std=c++17 -O2 -municode -mwindows -static main.cpp -o VoiceWindow.exe -lwinmm -lcomctl32 -lgdi32 -luser32 -lcomdlg32 -lole32
if errorlevel 1 (echo Build FAILED) else (echo Build OK: VoiceWindow.exe)
