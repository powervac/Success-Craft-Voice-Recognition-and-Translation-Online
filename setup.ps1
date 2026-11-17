# One-time setup: creates a Python virtual environment, installs the dependencies and downloads the models.
# Usage (PowerShell):   .\setup.ps1            (add -Small to use the lighter "medium" Whisper model)
param([switch]$Small)
$ErrorActionPreference = 'Stop'
Set-Location $PSScriptRoot

$py = Get-Command python -ErrorAction SilentlyContinue
if (-not $py) { throw 'Python 3.10+ was not found in PATH. Install it from https://www.python.org/downloads/ and re-run.' }

Write-Host '==> Creating virtual environment (venv)'
python -m venv venv
$venvPy = Join-Path $PSScriptRoot 'venv\Scripts\python.exe'

Write-Host '==> Installing dependencies'
& $venvPy -m pip install --upgrade pip
& $venvPy -m pip install -r requirements.txt

Write-Host '==> Downloading models (about 4.5 GB, one time)'
if ($Small) { & $venvPy download_models.py --small } else { & $venvPy download_models.py }

Write-Host ''
Write-Host 'Setup finished. Next: build the app with .\build.bat and run VoiceWindow.exe'
