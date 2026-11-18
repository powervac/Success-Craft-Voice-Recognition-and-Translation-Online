@echo off
rem Installs everything VoiceWindow needs and builds VoiceWindow.exe from main.cpp.
rem   1) C++ compiler: uses g++ from PATH, otherwise downloads LLVM-MinGW (UCRT) into .\tools
rem   2) builds VoiceWindow.exe with build.bat
rem   3) Python 3.10+ (installs Python 3.12 via winget if missing), venv, packages from requirements.txt
rem   4) models: downloaded now only when an NVIDIA GPU is present (on CPU the app picks a smaller model itself)
setlocal EnableExtensions
cd /d "%~dp0"
set "ROOT=%~dp0"

echo.
echo ===== 1/4  C++ compiler =====
where g++ >nul 2>&1 && goto :build
call :find_gpp
if defined GPPDIR goto :gpp_found
set "LLVM_URL=https://github.com/mstorsjo/llvm-mingw/releases/download/20261020/llvm-mingw-20261020-ucrt-x86_64.zip"
if not exist "%ROOT%tools" mkdir "%ROOT%tools"
echo Downloading LLVM-MinGW (181 MB)...
curl.exe -L --fail -o "%ROOT%tools\llvm-mingw.zip" "%LLVM_URL%"
if errorlevel 1 (echo Download failed. & goto :fail)
echo Extracting into %ROOT%tools ... this takes a few minutes
tar -xf "%ROOT%tools\llvm-mingw.zip" -C "%ROOT%tools"
if errorlevel 1 (echo Extraction failed. & goto :fail)
del /q "%ROOT%tools\llvm-mingw.zip"
call :find_gpp
if not defined GPPDIR (echo g++ was not found after extraction. & goto :fail)
:gpp_found
echo Using g++ from %GPPDIR%
set "PATH=%GPPDIR%;%PATH%"

:build
echo.
echo ===== 2/4  Building VoiceWindow.exe =====
if exist VoiceWindow.exe del /q VoiceWindow.exe
call build.bat
if not exist VoiceWindow.exe (echo Compilation failed, see the messages above. & goto :fail)

echo.
echo ===== 3/4  Python and packages =====
call :find_py
if defined PY goto :py_found
where winget >nul 2>&1 || (echo Python 3.10+ not found and winget is missing. Install Python from python.org. & goto :fail)
echo Installing Python 3.12 via winget...
winget install -e --id Python.Python.3.12 --scope user --accept-source-agreements --accept-package-agreements
call :find_py
if not defined PY (echo Python was not found after installation. Open a new console and run this file again. & goto :fail)
:py_found
echo Using Python: %PY%
if not exist venv\Scripts\python.exe "%PY%" -m venv venv
if not exist venv\Scripts\python.exe (echo Could not create venv. & goto :fail)
venv\Scripts\python.exe -m pip install --upgrade pip
set "GPU=0"
where nvidia-smi >nul 2>&1 && set "GPU=1"
if "%GPU%"=="1" (
  echo NVIDIA GPU found: installing all packages including CUDA libraries.
  venv\Scripts\python.exe -m pip install -r requirements.txt
) else (
  echo No NVIDIA GPU found: installing packages without CUDA libraries.
  findstr /v /i "nvidia" requirements.txt > "%TEMP%\voicerecogn_req.txt"
  venv\Scripts\python.exe -m pip install -r "%TEMP%\voicerecogn_req.txt"
)
if errorlevel 1 (echo pip install failed. & goto :fail)

echo.
echo ===== 4/4  Models =====
if "%GPU%"=="1" (
  echo Downloading models, about 4.5 GB...
  venv\Scripts\python.exe download_models.py
) else (
  echo Skipped: without an NVIDIA GPU the app downloads the lighter model itself on first start.
)

echo.
echo ===== Done. Start VoiceWindow.exe =====
pause
exit /b 0

:fail
echo.
echo ===== FAILED =====
pause
exit /b 1

:find_gpp
set "GPPDIR="
for /d %%Q in ("%ROOT%tools\llvm-mingw*") do if exist "%%~Q\bin\g++.exe" set "GPPDIR=%%~Q\bin"
if not defined GPPDIR for /d %%P in ("%LOCALAPPDATA%\Microsoft\WinGet\Packages\MartinStorsjo.LLVM-MinGW.UCRT*" "%ProgramFiles%\WinGet\Packages\MartinStorsjo.LLVM-MinGW.UCRT*") do (
  for /d %%Q in ("%%~P\llvm-mingw*") do if exist "%%~Q\bin\g++.exe" set "GPPDIR=%%~Q\bin"
)
if not defined GPPDIR if exist "C:\msys64\ucrt64\bin\g++.exe" set "GPPDIR=C:\msys64\ucrt64\bin"
if not defined GPPDIR if exist "C:\msys64\mingw64\bin\g++.exe" set "GPPDIR=C:\msys64\mingw64\bin"
exit /b 0

:find_py
set "PY="
python -c "import sys; sys.exit(0 if sys.version_info >= (3, 10) else 1)" >nul 2>&1 && set "PY=python"
if defined PY exit /b 0
for %%V in (314 313 312 311 310) do if not defined PY if exist "%LOCALAPPDATA%\Programs\Python\Python%%V\python.exe" set "PY=%LOCALAPPDATA%\Programs\Python\Python%%V\python.exe"
exit /b 0
