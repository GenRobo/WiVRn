# Offline test of the stream health monitor (GRID #2834, design §9 "integration"), without a headset.
# Runs wivrn-server with health warnings and logging on, and the fake headset (wivrn-handshake-probe)
# reporting 6 frames per second per stream as never decoded. Checks that:
# - the server sends a toast_urgent containing "lost" within 4 s of the loss starting,
# - the server writes one wivrn-health-*.jsonl line per second with the link counters,
# - when the probe stops dropping frames... (not covered: the probe drops for the whole session).
#
# Expected to fail until milestone 2 lands: the server has no health monitor yet, so no toast and no JSONL.
# Usage: health-test.ps1 -HelloXr <path\to\hello_xr.exe> [-Seconds 20] [-DropPerSecond 6] [-Encoder nvenc]
#                        [-BuildDir <build dir>] [-Out <dir>]
# hello_xr (OpenXR-SDK-Source, D3D11) renders through WiVRn's runtime so the server has frames to encode;
# without it the server streams nothing and the probe has no frames to drop.
param(
    [string]$HelloXr = $env:WIVRN_HELLO_XR,
    [int]$Seconds = 20,
    [int]$DropPerSecond = 6,
    [string]$Encoder = "nvenc",
    [string]$BuildDir = "",
    [string]$Out = ""
)
if (-not $HelloXr) { Write-Output "SKIP: pass -HelloXr or set WIVRN_HELLO_XR (hello_xr.exe from OpenXR-SDK-Source)"; exit 77 }
$ErrorActionPreference = "Continue"
$tools = Split-Path -Parent $MyInvocation.MyCommand.Path
if (-not $BuildDir) { $BuildDir = Join-Path $tools "..\..\build-win" }
$BuildDir = (Resolve-Path $BuildDir).Path
$serverDir = Join-Path $BuildDir "server"
if (-not $Out) { $Out = Join-Path $BuildDir "health-test" }
New-Item -ItemType Directory -Force $Out | Out-Null
$Out = (Resolve-Path $Out).Path
Remove-Item "$Out\*" -Force -ErrorAction SilentlyContinue

$env:PATH = (Join-Path $BuildDir "vcpkg_installed\x64-windows\bin") + ";" + $env:PATH
$env:XRT_LOG = "info"

$config = Join-Path $Out "config.json"
"{ `"encoder`": `"$Encoder`", `"hid-forwarding`": false, `"health`": { `"warnings`": true, `"log`": true, `"app-data`": false } }" | Set-Content -Encoding ascii $config

$srv = Start-Process -FilePath (Join-Path $serverDir "wivrn-server.exe") -ArgumentList @("--log-dir", $Out, "--pair", "-f", $config) -WorkingDirectory $serverDir `
    -RedirectStandardError "$Out\server-stderr.log" -RedirectStandardOutput "$Out\server-stdout.log" -PassThru -NoNewWindow
$pin = $null
for ($i = 0; $i -lt 30 -and -not $pin; $i++) {
    Start-Sleep -Milliseconds 500
    $m = Select-String -Path "$Out\server-stderr.log" -Pattern "PIN code: (\d+)" -ErrorAction SilentlyContinue
    if ($m) { $pin = $m.Matches[0].Groups[1].Value }
}
if (-not $pin) {
    Stop-Process -Id $srv.Id -Force -ErrorAction SilentlyContinue
    throw "wivrn-server did not print a PIN, see $Out\server-stderr.log"
}

$env:PROBE_DROP_FRAMES_PER_S = "$DropPerSecond"
$env:PROBE_EXPECT_MESSAGE = "lost"
$probe = Start-Process -FilePath (Join-Path $serverDir "wivrn-handshake-probe.exe") -ArgumentList @("127.0.0.1", "9757", $pin, "$Seconds") `
    -RedirectStandardError "$Out\probe.log" -RedirectStandardOutput "$Out\probe.out" -PassThru -NoNewWindow

Start-Sleep 3
$env:XR_RUNTIME_JSON = Join-Path $BuildDir "openxr_wivrn-dev.json"
# hello_xr exits on the first byte or EOF on stdin: feed it a pipe that stays open, and silent, for the test duration
$hx = Start-Process -FilePath "cmd.exe" -ArgumentList @("/c", "(ping -n $Seconds 127.0.0.1 >nul) | `"$HelloXr`" -G D3D11 >`"$Out\hello_xr.log`" 2>`"$Out\hello_xr.err`"") -PassThru -NoNewWindow

$probe.WaitForExit(($Seconds + 15) * 1000) | Out-Null
$srv.WaitForExit(10000) | Out-Null
foreach ($p in @($hx, $probe, $srv)) { if (-not $p.HasExited) { Stop-Process -Id $p.Id -Force } }
Get-Process wivrn-server, wivrn-handshake-probe, hello_xr -ErrorAction SilentlyContinue | Stop-Process -Force

"--- probe"
Get-Content "$Out\probe.log" | Select-String "server_message|dropped=|FAILED" | ForEach-Object { $_.Line }

"--- checks"
$failed = 0
if ($probe.ExitCode -eq 0) { "toast: probe received a server_message containing 'lost': OK" }
else { "toast: probe exit code $($probe.ExitCode), no warning received: FAILED"; $failed++ }

# Toast latency: first 'dropped=' report with a non-zero count vs the server_message line, from the probe's ms timestamps.
$lines = Get-Content "$Out\probe.log"
$firstDrop = $lines | Where-Object { $_ -match "dropped=[1-9]" } | Select-Object -First 1
$toast = $lines | Where-Object { $_ -match "server_message: .*lost" } | Select-Object -First 1
if ($firstDrop -and $toast) {
    $t0 = [int]([regex]::Match($firstDrop, "^\[(\d+)ms\]").Groups[1].Value)
    $t1 = [int]([regex]::Match($toast, "^\[(\d+)ms\]").Groups[1].Value)
    if ($t1 - $t0 -le 4000 + 2000) { "toast latency: $($t1 - $t0) ms (limit 4 s plus the 2 s report interval): OK" }
    else { "toast latency: $($t1 - $t0) ms: FAILED"; $failed++ }
}

$jsonl = Get-ChildItem "$Out\wivrn-health-*.jsonl" -ErrorAction SilentlyContinue | Select-Object -First 1
if (-not $jsonl) { "jsonl: no wivrn-health-*.jsonl in ${Out}: FAILED"; $failed++ }
else {
    $n = (Get-Content $jsonl.FullName | Measure-Object -Line).Lines
    if ($n -ge $Seconds - 3) { "jsonl: $n lines for a $Seconds s session: OK" } else { "jsonl: $n lines for a $Seconds s session: FAILED"; $failed++ }
    $withLoss = (Get-Content $jsonl.FullName | Where-Object { $_ -match '"idr_lost_ref":\s*[1-9]' }).Count
    if ($withLoss -ge 3) { "jsonl: $withLoss lines record lost reference frames: OK" } else { "jsonl: only $withLoss lines record lost reference frames: FAILED"; $failed++ }
    $event = Get-ChildItem "$Out\wivrn-health-event-*.json" -ErrorAction SilentlyContinue
    if ($event) { "event dump: $($event.Count) file(s): OK" } else { "event dump: none written when the warning fired: FAILED"; $failed++ }
}
if ($failed -eq 0) { "health test passed: $Out" } else { "health test FAILED ($failed): $Out"; exit 1 }
