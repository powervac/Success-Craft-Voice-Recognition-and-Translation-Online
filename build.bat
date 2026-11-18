@echo off
rem Builds VoiceWindow.exe with MinGW-w64 / LLVM-MinGW (g++ must be in PATH).
cd /d %~dp0
windres VoiceWindow.rc -O coff -o VoiceWindow_res.o
if errorlevel 1 goto :done
g++ -std=c++17 -O2 -s -municode -mwindows -static main.cpp VoiceWindow_res.o -o VoiceWindow.exe -lwinmm -lcomctl32 -lgdi32 -luser32 -lcomdlg32 -lole32 -lshell32
if errorlevel 1 (echo Build FAILED) else (echo Build OK: VoiceWindow.exe)
:done
