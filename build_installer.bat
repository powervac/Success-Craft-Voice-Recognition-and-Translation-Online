@echo off
rem Builds VoiceRecognitionSetup.exe: one file that installs everything on any Windows 10/11 x64 PC.
setlocal
cd /d "%~dp0"
set "GPPDIR="
for /d %%Q in ("%~dp0tools\llvm-mingw*") do if exist "%%~Q\bin\g++.exe" set "GPPDIR=%%~Q\bin"
if not defined GPPDIR (echo Compiler not found in the tools folder. & goto :fail)
set "PATH=%GPPDIR%;%PATH%"
if not exist VoiceWindow.exe call build.bat
if not exist VoiceWindow.exe goto :fail
cd installer
windres installer.rc -O coff -o installer_res.o || goto :fail
g++ -std=c++17 -O2 -municode -static installer.cpp installer_res.o -o ..\VoiceRecognitionSetup.exe -lole32 -luuid -lshell32 -lurlmon || goto :fail
del installer_res.o
cd ..
echo.
echo Built: %~dp0VoiceRecognitionSetup.exe
pause
exit /b 0
:fail
echo.
echo ===== FAILED =====
pause
exit /b 1
