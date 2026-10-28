# Server on Windows

The server can be built for Windows as a headless console program, `wivrn-server.exe`, together with the OpenXR runtime DLL that applications load.

Compared to Linux:
- One headset session per process: the server exits when the session ends, start it again for the next one.
- No dashboard, `wivrnctl`, D-Bus interface, application launcher or audio.
- The service is published with mDNS through Windows' own DNS-SD service instead of Avahi.
- No input forwarding (uinput): OpenXR controllers and gamepads work, keyboard and mouse forwarding does not.
- Encoders: NVENC (NVIDIA), Vulkan Video and x264. VA-API is Linux only.
- The OpenXR runtime is not registered as the system's active runtime: applications select it with `XR_RUNTIME_JSON`.

## Prerequisites

- Visual Studio 2022 with the "Desktop development with C++" workload, MSVC 14.44 (VS 17.14). It includes CMake and Ninja.
- [Git for Windows](https://git-scm.com/download/win). Its `sh.exe` applies the Monado patches during configuration.
- Python 3, for Monado's code generators.
- [vcpkg](https://github.com/microsoft/vcpkg):
  ```bat
  git clone https://github.com/microsoft/vcpkg.git C:\vcpkg
  C:\vcpkg\bootstrap-vcpkg.bat -disableMetrics
  ```
- For NVENC, the NVIDIA driver. It provides `nvcuda.dll` and `nvEncodeAPI64.dll`; the CUDA toolkit is not needed. The Vulkan SDK is not needed either: the Vulkan loader and headers come from vcpkg.

## Compile

From a "x64 Native Tools Command Prompt for VS 2022", or after `call "C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars64.bat" -vcvars_ver=14.44`, in your checkout directory:
```bat
cmake -S . -B build-win -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo ^
  -DCMAKE_TOOLCHAIN_FILE=C:\vcpkg\scripts\buildsystems\vcpkg.cmake ^
  -DVCPKG_MANIFEST_DIR=tools/windows -DVCPKG_TARGET_TRIPLET=x64-windows ^
  -DWIVRN_BUILD_SERVER=ON -DWIVRN_BUILD_SERVER_LIBRARY=ON -DWIVRN_BUILD_WIVRNCTL=OFF -DWIVRN_BUILD_DASHBOARD=OFF ^
  -DWIVRN_USE_PIPEWIRE=OFF -DWIVRN_USE_VAAPI=OFF -DWIVRN_USE_NVENC=ON -DWIVRN_USE_VULKAN_ENCODE=ON -DWIVRN_USE_X264=ON ^
  -DWIVRN_FEATURE_STEAMVR_LIGHTHOUSE=OFF -DWIVRN_FEATURE_SOLARXR=OFF ^
  -DWIVRN_OPENXR_MANIFEST_TYPE=absolute -DCMAKE_CXX_SCAN_FOR_MODULES=OFF
cmake --build build-win --target wivrn-server openxr_wivrn
```

The first configuration installs the dependencies listed in `tools/windows/vcpkg.json` into `build-win\vcpkg_installed` (x264 is built from source, this takes a while).

Outputs:
- `build-win\server\wivrn-server.exe`
- `build-win\openxr_wivrn-dev.json`, the OpenXR runtime manifest pointing at the runtime DLL in the build tree.

## Run

```bat
cd build-win\server
wivrn-server.exe --pair
```

`--pair` prints a PIN code to pair a new headset, even when headsets are already known. Other options:
- `-f <file>`: use another [configuration file](configuration.md). The default one is `%APPDATA%\wivrn\config.json`, and known headsets are stored next to it. For example, to force NVENC:
  ```json
  { "encoder": "nvenc", "hid-forwarding": false }
  ```
- `--log-dir <dir>`: the log is also written to `<dir>\wivrn-server-<time>.log`, by default in `%LOCALAPPDATA%\wivrn\logs`.
- `--no-encrypt`: disable encryption (debug).
- `--no-publish-service`: do not publish the service with mDNS.

When the session ends the server exits; run it again for the next session.

### Connecting the headset over USB with adb

With the WiVRn app installed on the headset and the headset connected over USB, forward the port and start the app with a `wivrn+tcp://` URL:
```bat
adb reverse tcp:9757 tcp:9757
adb shell am start -a android.intent.action.VIEW -d "wivrn+tcp://localhost" org.meumeu.wivrn
```
Replace `org.meumeu.wivrn` with your package name, see [the README](../README.md). When pairing, the headset shows a PIN pad, or the PIN can be passed in the URL: `wivrn+tcp://:<PIN>@localhost`.

### Native USB networking

Headsets that support it (Horizon OS 2.6 or later) can connect over USB without adb or developer mode: enable "USB networking" in the WiVRn app's settings. `adb reverse` is not used in this mode.

On the PC this adds a network adapter, "UsbNcm Host Device", with only an IPv6 link-local address (`fe80::…`; the `169.254.x.x` IPv4 address it also gets is not reachable from the headset). Windows classifies it as a Public network.

The headset finds the server with mDNS, like on a local network:
- `wivrn-server.exe` registers `_wivrn._tcp` with Windows' DNS-SD service; Windows' mDNS responder (the DNS Client service, in `svchost.exe`) answers on every interface, including the link-local one. Its queries on UDP port 5353 are allowed by the built-in "mDNS (UDP-In)" firewall rule, which is enabled for Public networks: no rule is needed for port 5353.
- Select the server in the headset's server list, or connect to the PC's link-local address with the interface as zone id, e.g. `wivrn://:<PIN>@[fe80::…%25usb0]:9757` (needs a headset app built from this branch; the 26.9 release cannot resolve such URLs).
- The connection itself (TCP and UDP port 9757) needs an inbound rule for `wivrn-server.exe` that includes the Public profile. The prompt shown the first time the server listens only selects Private networks by default; to create the rule from an elevated PowerShell:
  ```powershell
  New-NetFirewallRule -DisplayName "WiVRn server" -Direction Inbound -Action Allow -Profile Any -Program "C:\path\to\WiVRn\build-win\server\wivrn-server.exe"
  ```

### Connecting over the network

Allow inbound TCP and UDP port 9757 for `wivrn-server.exe` in the Windows firewall (see the rule above). The server appears in the headset's server list, or connect with the PC's address in a `wivrn://` URL.

### Headset app

The headset app is built the upstream way, on Linux, as described in [building](building.md#client-headset); a WSL distribution works. The Android SDK packages used are:
```bash
sdkmanager "ndk;29.0.14206865" "cmake;3.31.5" "platforms;android-34" "build-tools;34.0.0" "platform-tools"
```
together with JDK 17, `rsvg-convert`, `msgfmt` (gettext), `glslangValidator`, `spirv-opt` and the `ktx` tool from [KTX-Software](https://github.com/KhronosGroup/KTX-Software/releases) on `PATH`. `./gradlew assembleDebug` builds `build/outputs/apk/debug/WiVRn-debug.apk`, with the application id `org.meumeu.wivrn.local`, which installs next to the store and GitHub releases.

### Running an application

Point the application at the runtime with `XR_RUNTIME_JSON`, for instance:
```bat
set XR_RUNTIME_JSON=C:\path\to\WiVRn\build-win\openxr_wivrn-dev.json
hello_xr.exe -G D3D11
```

#### Chrome (WebXR)

Chrome uses the runtime from `XR_RUNTIME_JSON`; OpenXR is enabled by default on Windows. With a separate profile:
```bat
set XR_RUNTIME_JSON=C:\path\to\WiVRn\build-win\openxr_wivrn-dev.json
"C:\Program Files\Google\Chrome\Application\chrome.exe" --user-data-dir=%LOCALAPPDATA%\wivrn\chrome-profile ^
  --no-first-run --no-default-browser-check --no-sandbox
```
Add `--remote-debugging-port=9555` to drive Chrome through the DevTools protocol (e.g. to automate WebXR tests).

`--no-sandbox` is the simple path. To keep Chrome's sandbox, Chrome's XR process runs in an AppContainer, which needs:
- the same `XDG_RUNTIME_DIR` for the server and Chrome, e.g. `set XDG_RUNTIME_DIR=C:\monado-rt` in both consoles (Monado names its pipe after this directory, which otherwise differs inside the AppContainer; the directory does not need to exist);
- `set IPC_RELAXED_CONNECTION_SECURITY=1` for the server, so AppContainers can open the pipe;
- the runtime files readable by AppContainers: `icacls build-win /grant *S-1-15-2-1:(OI)(CI)(RX) /T`.
