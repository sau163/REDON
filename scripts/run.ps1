# run.ps1 — build Redon (if needed) and start the server + browser UI on Windows.
#
#   powershell -ExecutionPolicy Bypass -File scripts\run.ps1
#
# Then open http://127.0.0.1:8080 in your browser. Ctrl-C to stop both.
param(
    [int]$WebPort   = 8080,
    [int]$RedonPort = 6380,
    [string]$Wal    = "none",        # "none" = in-memory; or a path like redon.wal
    [switch]$NoBuild
)

$ErrorActionPreference = "Stop"
$root = Split-Path -Parent $PSScriptRoot
$exe  = Join-Path $root "build\Release\redon-server.exe"
$web  = Join-Path $root "build\Release\redon-web.exe"

if (-not $NoBuild -or -not (Test-Path $exe) -or -not (Test-Path $web)) {
    Write-Host "Building Redon (Release)..." -ForegroundColor Cyan
    cmake -S $root -B (Join-Path $root "build") | Out-Null
    cmake --build (Join-Path $root "build") --config Release
    if ($LASTEXITCODE -ne 0) { throw "build failed" }
}

Write-Host "Starting redon-server on 127.0.0.1:$RedonPort (wal=$Wal, metrics :9090)..." -ForegroundColor Cyan
$server = Start-Process -FilePath $exe `
    -ArgumentList "127.0.0.1", "$RedonPort", "8", "$Wal", "0", "0", "--metrics-port", "9090" `
    -PassThru -WindowStyle Hidden

Start-Sleep -Milliseconds 700
Write-Host "Starting redon-web on http://127.0.0.1:$WebPort ..." -ForegroundColor Cyan
$gateway = Start-Process -FilePath $web `
    -ArgumentList "$WebPort", "127.0.0.1", "$RedonPort" `
    -PassThru -WindowStyle Hidden

Start-Sleep -Milliseconds 500
Start-Process "http://127.0.0.1:$WebPort"   # open the browser

Write-Host ""
Write-Host "Redon is running:" -ForegroundColor Green
Write-Host "  Web UI   : http://127.0.0.1:$WebPort"
Write-Host "  Protocol : 127.0.0.1:$RedonPort   (redon-cli $RedonPort)"
Write-Host "  Metrics  : http://127.0.0.1:9090/metrics"
Write-Host ""
Write-Host "Press Ctrl-C (or close this window) to stop." -ForegroundColor Yellow

try {
    while (-not $server.HasExited -and -not $gateway.HasExited) {
        Start-Sleep -Seconds 1
    }
} finally {
    Write-Host "`nStopping..." -ForegroundColor Cyan
    Stop-Process -Id $gateway.Id -Force -ErrorAction SilentlyContinue
    Stop-Process -Id $server.Id  -Force -ErrorAction SilentlyContinue
}
