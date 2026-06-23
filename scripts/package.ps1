# package.ps1 — build Redon (Release) and produce a shareable Windows zip.
#
#   powershell -ExecutionPolicy Bypass -File scripts\package.ps1
#
# Output: dist\redon-win64.zip  — unzip anywhere and double-click start.bat.
$ErrorActionPreference = "Stop"
$root  = Split-Path -Parent $PSScriptRoot
$build = Join-Path $root "build"
$dist  = Join-Path $root "dist"
$stage = Join-Path $dist "redon-win64"

Write-Host "Building Redon (Release)..." -ForegroundColor Cyan
cmake -S $root -B $build | Out-Null
cmake --build $build --config Release
if ($LASTEXITCODE -ne 0) { throw "build failed" }

Write-Host "Staging files..." -ForegroundColor Cyan
if (Test-Path $stage) { Remove-Item -Recurse -Force $stage }
New-Item -ItemType Directory -Force -Path (Join-Path $stage "bin")  | Out-Null
New-Item -ItemType Directory -Force -Path (Join-Path $stage "docs") | Out-Null

$bin = Join-Path $build "Release"
foreach ($exe in "redon-server.exe","redon-cli.exe","redon-web.exe","redon-bench.exe") {
    Copy-Item (Join-Path $bin $exe) (Join-Path $stage "bin") -Force
}
Copy-Item (Join-Path $root "README.md") $stage -Force
Copy-Item (Join-Path $root "docs\*.md") (Join-Path $stage "docs") -Force

# A double-click launcher: starts the server + web UI and opens the browser.
@'
@echo off
cd /d "%~dp0"
echo Starting Redon...
start "" /b bin\redon-server.exe 127.0.0.1 6380 8 none 0 0 --metrics-port 9090
timeout /t 1 /nobreak >nul
start "" /b bin\redon-web.exe 8080 127.0.0.1 6380
timeout /t 1 /nobreak >nul
start "" http://127.0.0.1:8080
echo.
echo Redon is running:
echo   Web UI  : http://127.0.0.1:8080
echo   Metrics : http://127.0.0.1:9090/metrics
echo   CLI     : bin\redon-cli.exe 6380
echo.
echo Closing this window stops nothing; to stop Redon, end redon-server.exe and
echo redon-web.exe in Task Manager.
pause
'@ | Set-Content -Path (Join-Path $stage "start.bat") -Encoding ASCII

@'
Redon — a distributed key-value store, built from scratch in C++.

QUICK START
  1. Double-click  start.bat
  2. Your browser opens http://127.0.0.1:8080 — type commands and press Run.

COMMAND LINE (optional)
  bin\redon-cli.exe 6380      then type:  SET name Saurabh   /   GET name

WHAT IS RUNNING
  redon-server  the database (TCP on 127.0.0.1:6380, metrics on :9090)
  redon-web     a web UI that bridges your browser to the server (:8080)

See README.md and the docs\ folder for how each of the nine phases works.
'@ | Set-Content -Path (Join-Path $stage "START-HERE.txt") -Encoding ASCII

$zip = Join-Path $dist "redon-win64.zip"
if (Test-Path $zip) { Remove-Item -Force $zip }
Write-Host "Zipping -> $zip" -ForegroundColor Cyan
Compress-Archive -Path (Join-Path $stage "*") -DestinationPath $zip -Force

Write-Host ""
Write-Host "Done. Shareable bundle:" -ForegroundColor Green
Write-Host "  $zip"
Write-Host "  (unzip anywhere, then double-click start.bat)"
