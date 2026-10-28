# Offline encoder test for the Windows server, without a headset (see docs/windows.md).
# Runs wivrn-server, the fake headset (wivrn-handshake-probe) and an OpenXR application (hello_xr)
# through WiVRn's runtime, dumps the bitstream the fake headset receives and, if ffmpeg is on PATH,
# decodes it and reports the frame count and decode errors.
#
# Usage: encoder-test.ps1 -HelloXr <path\to\hello_xr.exe> [-Encoder auto|nvenc|vulkan|x264|<config.json>]
#                         [-Codecs h265,h264] [-BitDepth 8|10] [-Seconds 30] [-BuildDir <build dir>] [-Out <dir>]
# Set PROBE_ACK_DELAY_MS to delay the fake headset's frame acknowledgements (simulated latency).
param(
    [Parameter(Mandatory = $true)][string]$HelloXr,
    [string]$Encoder = "auto",
    [string]$Codecs = "h265,h264",
    [int]$BitDepth = 8,
    [int]$Seconds = 30,
    [string]$BuildDir = "",
    [string]$Out = ""
)
$ErrorActionPreference = "Continue"
if (-not $BuildDir) { $BuildDir = Join-Path (Split-Path -Parent $MyInvocation.MyCommand.Path) "..\..\build-win" }
$BuildDir = (Resolve-Path $BuildDir).Path
$serverDir = Join-Path $BuildDir "server"
if (-not $Out) { $Out = Join-Path $BuildDir "encoder-test-$([IO.Path]::GetFileNameWithoutExtension($Encoder))" }
New-Item -ItemType Directory -Force $Out | Out-Null
$Out = (Resolve-Path $Out).Path

$env:PATH = (Join-Path $BuildDir "vcpkg_installed\x64-windows\bin") + ";" + $env:PATH
$env:XRT_LOG = "info"
$env:WIVRN_DUMP_VIDEO = Join-Path $Out "server"

$srvArgs = @("--log-dir", $Out, "--pair")
if ($Encoder -like "*.json") {
    $srvArgs += @("-f", (Resolve-Path $Encoder).Path)
} elseif ($Encoder -ne "auto") {
    $config = Join-Path $Out "config.json"
    "{ `"encoder`": `"$Encoder`", `"hid-forwarding`": false }" | Set-Content -Encoding ascii $config
    $srvArgs += @("-f", $config)
}

$srv = Start-Process -FilePath (Join-Path $serverDir "wivrn-server.exe") -ArgumentList $srvArgs -WorkingDirectory $serverDir `
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

$env:PROBE_CODECS = $Codecs
$env:PROBE_BIT_DEPTH = "$BitDepth"
$env:PROBE_DUMP = Join-Path $Out "probe"
if (-not $env:PROBE_ACK_DELAY_MS) { $env:PROBE_ACK_DELAY_MS = "0" }
$probe = Start-Process -FilePath (Join-Path $serverDir "wivrn-handshake-probe.exe") -ArgumentList @("127.0.0.1", "9757", $pin, "$Seconds") `
    -RedirectStandardError "$Out\probe.log" -RedirectStandardOutput "$Out\probe.out" -PassThru -NoNewWindow
Start-Sleep 3

$env:XR_RUNTIME_JSON = Join-Path $BuildDir "openxr_wivrn-dev.json"
# hello_xr exits on the first byte or EOF on stdin: feed it a pipe that stays open, and silent, for the test duration
$hx = Start-Process -FilePath "cmd.exe" -ArgumentList @("/c", "(ping -n $Seconds 127.0.0.1 >nul) | `"$HelloXr`" -G D3D11 >`"$Out\hello_xr.log`" 2>`"$Out\hello_xr.err`"") -PassThru -NoNewWindow

$probe.WaitForExit(($Seconds + 15) * 1000) | Out-Null
foreach ($p in @($hx, $probe, $srv)) { if (-not $p.HasExited) { Stop-Process -Id $p.Id -Force } }
Get-Process wivrn-server, wivrn-handshake-probe, hello_xr -ErrorAction SilentlyContinue | Stop-Process -Force

Get-Content "$Out\probe.log" -Tail 2
if (Get-Command ffmpeg -ErrorAction SilentlyContinue) {
    foreach ($f in Get-ChildItem "$Out\probe-*.bin" | Where-Object Length -gt 0) {
        $errors = (& ffmpeg -hide_banner -v error -i $f.FullName -f null - 2>&1 | Measure-Object -Line).Lines
        $frames = & ffprobe -v error -count_frames -select_streams v:0 -show_entries stream=codec_name,width,height,nb_read_frames -of csv=p=0 $f.FullName
        "$($f.Name): $frames (codec,width,height,frames); decode errors: $errors"
    }
}
"done: $Out"
