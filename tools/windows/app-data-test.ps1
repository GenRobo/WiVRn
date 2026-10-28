# Offline test of the server's "app-data" relay (see docs/windows.md), without a headset.
# Runs wivrn-server with an app-data channel pointing at a test TCP sink (app_data_sink.py) and the fake
# headset (wivrn-handshake-probe) replaying a file on that channel while recording what comes back.
# The sink sends its own payload to the headset, records what the headset sends and restarts once,
# so both directions and the reconnection are checked:
# - what the probe received must be the sink's payload once per sink connection,
# - what each sink connection received must be a contiguous slice of the probe's stream.
#
# Usage: app-data-test.ps1 [-Seconds 25] [-RestartAfter 8] [-Port 9760] [-BuildDir <build dir>] [-Out <dir>]
param(
    [int]$Seconds = 25,
    [int]$RestartAfter = 8,
    [int]$Port = 9760,
    [int]$StallFirst = 0,
    [string]$BuildDir = "",
    [string]$Out = ""
)
$ErrorActionPreference = "Continue"
$tools = Split-Path -Parent $MyInvocation.MyCommand.Path
if (-not $BuildDir) { $BuildDir = Join-Path $tools "..\..\build-win" }
$BuildDir = (Resolve-Path $BuildDir).Path
$serverDir = Join-Path $BuildDir "server"
if (-not $Out) { $Out = Join-Path $BuildDir "app-data-test" }
New-Item -ItemType Directory -Force $Out | Out-Null
$Out = (Resolve-Path $Out).Path
Remove-Item "$Out\*" -Force -ErrorAction SilentlyContinue

$env:PATH = (Join-Path $BuildDir "vcpkg_installed\x64-windows\bin") + ";" + $env:PATH
$env:XRT_LOG = "info"

# Random payloads: 48 x 64 KiB from the headset (a multiple of the probe's packet size, see app_data_sink.py check), 2 MiB from the sink
$chunk = 65536
$random = [System.Random]::new(1)
foreach ($f in @(@("headset.bin", (48 * $chunk)), @("sink.bin", (32 * $chunk)))) {
    $bytes = [byte[]]::new($f[1]); $random.NextBytes($bytes)
    [IO.File]::WriteAllBytes((Join-Path $Out $f[0]), $bytes)
}

$config = Join-Path $Out "config.json"
"{ `"encoder`": `"x264`", `"hid-forwarding`": false, `"app-data`": [ { `"channel`": `"test`", `"connect`": `"127.0.0.1:$Port`" } ] }" | Set-Content -Encoding ascii $config

$sinkMode = @("--restart-after", "$RestartAfter")
if ($StallFirst -gt 0) { $sinkMode = @("--stall-first", "$StallFirst") }
$sink = Start-Process -FilePath "python" -ArgumentList (@((Join-Path $tools "app_data_sink.py"), "serve", "$Port", "$Out\sink.bin", "$Out\sink-received") + $sinkMode) `
    -RedirectStandardOutput "$Out\sink.log" -RedirectStandardError "$Out\sink.err" -PassThru -NoNewWindow
Start-Sleep 1

$srv = Start-Process -FilePath (Join-Path $serverDir "wivrn-server.exe") -ArgumentList @("--log-dir", $Out, "--pair", "-f", $config) -WorkingDirectory $serverDir `
    -RedirectStandardError "$Out\server-stderr.log" -RedirectStandardOutput "$Out\server-stdout.log" -PassThru -NoNewWindow
$pin = $null
for ($i = 0; $i -lt 30 -and -not $pin; $i++) {
    Start-Sleep -Milliseconds 500
    $m = Select-String -Path "$Out\server-stderr.log" -Pattern "PIN code: (\d+)" -ErrorAction SilentlyContinue
    if ($m) { $pin = $m.Matches[0].Groups[1].Value }
}
if (-not $pin) {
    foreach ($p in @($sink, $srv)) { Stop-Process -Id $p.Id -Force -ErrorAction SilentlyContinue }
    throw "wivrn-server did not print a PIN, see $Out\server-stderr.log"
}

$env:PROBE_APP_DATA_CHANNEL = "test"
$env:PROBE_APP_DATA_SEND = Join-Path $Out "headset.bin"
$env:PROBE_APP_DATA_REPEAT = "1"
$env:PROBE_APP_DATA_CHUNK = "$chunk"
$env:PROBE_APP_DATA_RATE = "50"
$env:PROBE_APP_DATA_RECV = Join-Path $Out "probe-received.bin"
$probe = Start-Process -FilePath (Join-Path $serverDir "wivrn-handshake-probe.exe") -ArgumentList @("127.0.0.1", "9757", $pin, "$Seconds") `
    -RedirectStandardError "$Out\probe.log" -RedirectStandardOutput "$Out\probe.out" -PassThru -NoNewWindow

$probe.WaitForExit(($Seconds + 15) * 1000) | Out-Null
$srv.WaitForExit(10000) | Out-Null
$sink.WaitForExit(5000) | Out-Null
foreach ($p in @($probe, $srv, $sink)) { if (-not $p.HasExited) { Stop-Process -Id $p.Id -Force } }
Get-Process wivrn-server, wivrn-handshake-probe -ErrorAction SilentlyContinue | Stop-Process -Force

"--- server app-data log"
Select-String -Path "$Out\server-stderr.log" -Pattern "app-data" | ForEach-Object { $_.Line }
"--- sink"
Get-Content "$Out\sink.log"
"--- probe"
Get-Content "$Out\probe.log" | Select-String "app_data" | ForEach-Object { $_.Line }

"--- checks"
$failed = 0
$sinkPayload = [IO.File]::ReadAllBytes("$Out\sink.bin")
$probeReceived = [IO.File]::ReadAllBytes("$Out\probe-received.bin")
$connections = (Get-ChildItem "$Out\sink-received-*.bin").Count
$payloadCopies = $connections
# The stalled first connection only receives; sending there could block the test sink as well.
if ($StallFirst -gt 0) { $payloadCopies-- }
$expected = $sinkPayload * $payloadCopies
if ([System.Linq.Enumerable]::SequenceEqual([byte[]]$probeReceived, [byte[]]$expected)) {
    "sink -> headset: probe received the sink payload $payloadCopies times ($($probeReceived.Length) bytes): OK"
} else {
    "sink -> headset: probe received $($probeReceived.Length) bytes, expected the sink payload $payloadCopies times ($($expected.Length) bytes): FAILED"
    $failed++
}
& python (Join-Path $tools "app_data_sink.py") check "$Out\headset.bin" $chunk (Get-ChildItem "$Out\sink-received-*.bin" | ForEach-Object { $_.FullName })
if ($LASTEXITCODE -ne 0) { $failed++ }
if ($connections -lt 2) { "reconnection: only $connections sink connection(s): FAILED"; $failed++ } else { "reconnection: $connections sink connections: OK" }
if ($StallFirst -gt 0) {
    if (Select-String -Path "$Out\server-stderr.log" -Pattern "queue overflow; resetting byte stream") {
        "queue overflow: local byte stream reset: OK"
    } else { "queue overflow: no stream reset observed: FAILED"; $failed++ }
}
if ($failed -eq 0) { "app-data test passed: $Out" } else { "app-data test FAILED ($failed): $Out"; exit 1 }
