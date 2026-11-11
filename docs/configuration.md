# Configurable items

Configuration is done on server side.
Files are read from
- `/usr/share/wivrn/config.json` (where `/usr` is selected at configure time with `CMAKE_INSTALL_PREFIX`)
- `/etc/wivrn/config.json`
- `$XDG_CONFIG_HOME/wivrn/config.json` or if `$XDG_CONFIG_HOME` is not set, `$HOME/.config/wivrn/config.json`.

Files later in the list replace top-level values from previous ones.

If you installed WiVRn from a flatpack, the config is in `$HOME/.var/app/io.github.wivrn.wivrn/config/wivrn/config.json`.

All elements are optional and have default values.

## `bit-depth`
Default value: `8` (bits)

Bit depth of the video. 8-bit is supported by all encoders. 10-bit is supported by `vaapi` and `nvenc` encoders using `h265` or `av1`.

## `encoder`
The encoder to use, either a single string or object applied to all streams, or a list of string or objects with values for left, right and alpha.
When a string it is used, it is equivalent to the `encoder` item of the object.

WiVRn encodes each eye separately, and the alpha channel as one for both eyes. Each stream is processed independently, this may use resources more effectively and reduce latency.

### `encoder`
Default value: `nvenc` if Nvidia GPU and compiled with nvenc, `vaapi` for all other GPU when compiled with ffmpeg, else `x264`.

Identifier of the encoder, one of
* `x264`: software encoding
* `nvenc`: Nvidia hardware encoding
* `vaapi`: AMD/Intel hardware encoding
* `vulkan`: experimental, for any GPU that supports vulkan video encode

### `codec`
Default value: best supported by both headset and encoder of `av1`, `h264`, `h265`.

One of `h264`, `h265`, `av1`, `raw`.

Not all encoders support every codec:
- `x264` encoder only supports `h264` codec
- `vulkan` encoder supports `h264` and `h265` codecs
- `raw` encoder only supports `raw` codec
- `nvenc` and `vaapi` support all codecs, except `raw`

If `nvenc` encoder is in use, you can refer to [nvidia website](https://developer.nvidia.com/video-encode-decode-support-matrix) to make sure that your GPU supports encoding with the desired codec.

### Examples
1. Simple configuration
```json
{
	"encoder": {
		"encoder": "vaapi",
		"codec": "h265"
	}
}
```
Use vaapi hardware encoding, h265 video codec (HEVC).

2. Hardware + software encoder
```json
{
	"encoder": [
		{
			"encoder": "vaapi",
			"codec": "h265",
		},
		{
			"encoder": "x264",
			"codec": "h264",
		},
		{
			"encoder": "vaapi",
			"codec": "h265",
		},
	]
}
```
Creates a hardware encoder for left eye and transparency, and a software encoder for right eye.

### `device`, only for vaapi
Default value: unset

Manually specify the device for encoding, can be used to offload encode to an iGPU. Device shall be in the form "/dev/dri/renderD128".


### `options` (very advanced), only for vaapi
Default value: unset

Json object of additional options to pass directly to ffmpeg `avcodec_open2`'s `option` parameter.

## `application`
Default value: unset

An application to start when connection with the headset is established, can be a string or an array of strings if parameters need to be provided.

### Example
```json
{
	"application": ["steam", "steam://launch/275850/VR"]
}
```
Launch No Man's Sky in VR mode on Steam when connection with headset is established.

## `app-data`
Default value: unset

Application data channels: byte streams between the headset app and TCP services on the server side, carried on the headset connection. A list of objects with
* `channel`: the channel name, matched with what the headset app sends on and listens to (`MainActivity.sendAppData` / `setAppDataListener`, or `application::send_app_data` / `set_app_data_handler` in the C++ client);
* `connect`: the sink's `host:port`, an IPv6 address in brackets.

While a headset session is active, the server connects to each sink as a TCP client, retrying every 2 seconds, and relays the bytes in both directions: what the sink writes is delivered to the headset app on the channel, what the headset app sends on the channel is written to the sink. A channel is a plain byte stream, message framing belongs to the application; a packet carries at most 1 MiB, larger writes are split. While no sink is connected, data from the headset is dropped and counted. The server logs connections, disconnections and the byte counts per direction every 5 seconds.

Headset-to-sink data uses a bounded 16 MiB queue rather than blocking the tracking/network thread. Overflow closes the local TCP connection and discards pending data before reconnecting: missing chunks are never followed by later chunks on the same connection, which would corrupt application framing. Sink writes are non-blocking and stop-aware, so a stalled receiver cannot prevent session shutdown. Consumers must reset their framing state on reconnect.

The channel `wivrn-health` is spoken by the server itself, not the headset app: the sink receives the stream health samples and warnings, and what it writes goes to the health monitor. See [health.md](health.md#connector).

### Example
```json
{
	"app-data": [
		{ "channel": "vivarium-camera", "connect": "127.0.0.1:9760" }
	]
}
```

## `app-margin-ms`
Default value: `1.5`, or `U_PACING_APP_MIN_MARGIN_MS` when that environment variable is set

Minimum margin, in milliseconds, between the predicted end of an application's frame and
the compositor deadline. Range 0.5 to 5. A larger margin wakes the application earlier,
trading a little latency for fewer late frames. Applications with variable frame times,
such as WebXR in a browser, may need 3 to pace smoothly at 120 Hz.

A pipelined application such as a browser can lock into half rate after a hiccup: handed display
times two periods out it delivers two periods after its wake-up, which the pacer measures as two
periods of CPU time and answers with two-period display times again, although each frame costs it
a fraction of a period (GRID #2834, 66 Hz at a 120 Hz display with 15 ms measured against 1.5 ms
of work). The margin makes this rarer, not impossible. Once an application has delivered at a
steady 2x or 3x the display period (within 5 %) for a second while its measured GPU time fits in
one period with the margin, the pacer predicts one frame per second one period out regardless of
the CPU estimate. If that frame arrives in time the estimate is reset to what it measured and
pacing continues at one period; if it misses, it is shown where the slower cadence would have
shown it anyway and the next probe waits another second. The [health record](health.md) counts
the probes and the recoveries under `app`.

### Example
```json
{
	"app-margin-ms": 3
}
```

## `health`
Default value: all on, frame record off

Stream health monitor: in-headset warnings, a per-second flight record and, on request, a
per-frame record. See [health.md](health.md) for the keys (`warnings`, `log`, `app-data`,
`frames`, `frames-max-mb`, `frames-keep`, `log-dir`, `thresholds`), the rules and the record formats.

### Example
```json
{
	"health": { "warnings": true, "log": true, "thresholds": { "link_loss.frames_per_s": 8 } }
}
```

## `mirror`
Default value: unset (off)

Live mirror (GRID #2463): show what the headset is being sent, on the PC, without writing a
file. `{"connect": "127.0.0.1:9763", "stream": 0}`: while something listens on `connect`, the
encoded bitstream of `stream` (0 left eye, 1 right eye, 2 alpha; Annex-B `h264`/`h265`, `av1`
OBUs) is copied to it byte for byte, exactly as sent to the headset, and an IDR is forced each
time the sink connects so it decodes from its first bytes. Nothing is copied while no sink
listens; the server retries the connection every 2 s. The picture is the encoded, foveated
image (the periphery compressed), not the headset's final view. A slow sink is dropped (16 MiB
of backlog) and reconnected. For example: `ffplay -fflags nobuffer -flags low_delay
-framerate 72 -f hevc "tcp://127.0.0.1:9763?listen=1"`.

## `capture`
Default value: unset (off)

Mixed-reality capture (GRID #2837): while a headset session streams, record what the server
encodes so an offline compositor can replay it over the headset's passthrough camera. Keys:
- `dir`: the root directory. Each session gets its own `wivrn-capture-<YYYYmmdd-HHMMSS>/`
  under it. The configuration is read when the session starts, so a change applies to the
  next headset connection. On Windows, `wivrn-server.exe --capture DIR` overrides this key.

Off, nothing runs. On, the server writes, per stream `i` (0 left eye, 1 right eye, 2 alpha):
- `stream-<i>.<codec>`: the encoded bitstream, exactly the bytes sent to the headset
  (Annex-B `h264`/`h265`, `av1` OBUs or `yuv`). This replaces `WIVRN_DUMP_VIDEO`, which keeps
  working when capture is off.
- `stream-<i>.jsonl`: one `frame` line per encoded frame, in bitstream order, so the n-th
  decoded frame is the n-th `frame` line (frames the encoder skipped while waiting for the
  headset are absent from both). Fields: `frame` (compositor frame index, shared by the three
  streams), `stream`, `alpha` (the frame carries an alpha channel; the alpha stream exists only
  then), `codec`, `width`/`height` (the encoded eye size, alpha is half height),
  `display_time_headset_ns`/`display_time_host_ns` (the predicted display time on the headset's
  clock and on the server's monotonic clock), `encode_begin_host_ns`/`encode_begin_headset_ns`,
  `send_end_host_ns`, `clock_offset_ns`/`clock_stable` (the headset-minus-host offset in use),
  `byte_offset`/`byte_size` (the frame's bytes in the bitstream, parameter sets included) and
  `views`: for the left and right eye, the `position` (metres) and `orientation_xyzw` of the
  view in the application's stage space, and the `fov` (`[left, right, up, down]`, radians,
  signed as in `XrFovf`) the frame was rendered with.
- A `foveation` line precedes the first `frame` line, and any frame whose foveation tables
  differ from the previous one: `views[v].x`/`y` are the per-axis source pixel counts of the
  encoded image (see `foveation_parameter` in `common/wivrn_packets.h`), needed to undo the
  foveation before reprojecting.
- `manifest.json`: the session layout (eye size, codecs, frame rate, headset name, nominal
  fields of view, file names), written when the session starts.

Cost: one buffered file write per encoded frame on the stream's send thread, plus the sidecar
line (about 600 bytes). Nothing is allocated per frame beyond the dump itself.

### Example
```json
{
	"capture": { "dir": "D:/captures" }
}
```

## `tcp-only`
Default value: `false`

Only use TCP for communications with the client, this may have increased latency.
If `false` or unset, WiVRn will use both TCP and UDP.

### Example
```json
{
	"tcp-only": true
}
```

## `publish-service`
Default value: `avahi`

How to publish the service over the network, `avahi` or null.

If set to null, service will not be published and address has to be entered manually on the headset.

## `openvr-compat-path`
Default value: unset

Provides the path to the directory of an OpenVR compatibility tool (such as OpenComposite).

If unset, WiVRn will autodetect the path of such a tool as usual (see [the SteamVR guide](./steamvr.md)).

If set to an null, WiVRn will not manage the OpenVR configuration.

## `hid-forwarding`
Default value: `false`

Only available when the `uinput` kernel module is loaded and the user has write access.

Mirrors input devices forwarded from the headset (keyboard, mouse, gamepad) to `uinput`
devices, for applications that do not read them through OpenXR. Which devices are forwarded is
chosen on the headset.

A forwarded gamepad is also exposed as a native OpenXR gamepad at `/user/gamepad`, which needs
no permission and is always available. Only the digested controller state is forwarded
(buttons, two sticks, analog triggers and a d-pad), so device specific features such as gyro,
adaptive triggers or the touchpad are not available.

## `debug-gui`
Default value: `false`

Only available when built with `WIVRN_FEATURE_DEBUG_GUI`.

Enables the Monado debug gui.

## `use-steamvr-lh`
Default value: `false`

Only available when built with `WIVRN_FEATURE_STEAMVR_LIGHTHOUSE`.

Enables the driver to load SteamVR Lighthouse devices.

## `lh-max-extrapolation`
Default value: unset

Only available when built with `WIVRN_FEATURE_STEAMVR_LIGHTHOUSE`.

Maximum time in milliseconds that poses may be extrapolated ahead for SteamVR Lighthouse devices.

## `lh-stick-deadzone`
Default value: `0`

Only available when built with `WIVRN_FEATURE_STEAMVR_LIGHTHOUSE`

Applies a deadzone to joysticks on SteamVR controllers (e.g. Index).

## `port`
Default value: `9757`

Change the TCP/UDP port used for the connection.

## `hostname`
Default value: unset

If set, overrides the name displayed in the server list.

## Headset sensor streaming (Android fork)

The headset's **Tracking** page has three independent switches, all off by
default and saved in the client configuration:

- **Marker tracking** (`marker_tracking`): detect ArUco markers and send their
  corner coordinates without requiring image streaming.
- **RGB camera streaming** (`rgb_camera_streaming`): send color images (H.264
  by default) without requiring marker detection.
- **Depth camera streaming** (`depth_camera_streaming`): send environment depth
  without opening Camera2 capture.

Enable only the required switches in the lobby. A PC request cannot override a
local off switch. Marker tracking and RGB capture need Camera and Headset Camera
access; depth needs Spatial Data access. Replacing the former combined setting
leaves the new switches off until explicitly enabled.

The RGB stream has four settings without a lobby switch, in the client
configuration file only:

| Key | Default | Meaning |
|---|---|---|
| `camera_stream_codec` | `"h264"` | `"h264"`: the hardware encoder; `"jpeg"`: CPU JPEG at CONFIGURE's quality. |
| `camera_stream_width` | `1280` | Streamed image width; with the height, one of the YUV_420_888 output sizes the camera reports (a second capture output next to the 1280x960 one detection uses). |
| `camera_stream_height` | `960` | Streamed image height; a size the camera does not offer is logged and the 1280x960 capture is streamed. |
| `camera_stream_bitrate_mbps` | `4` | H.264 target bitrate, variable-bitrate mode, in Mbit/s. |

CALIBRATION carries the codec (`1` JPEG, `2` H264) and the streamed size;
marker reports keep the 1280x960 capture size. With H.264 each FRAME payload is
one access unit in Annex-B form (`00 00 00 01` start codes); SPS and PPS are
in front of every IDR, one IDR per second and one on request when the PC
enables images, so the PC feeds the payloads to an H.264 decoder in order and
starts at the first IDR. With JPEG each payload is one JPEG image, decoded on its
own. `maxRateHz` in CONFIGURE caps both; `jpegQuality` only matters for JPEG.

**Camera and depth streaming are not recommended over Wi-Fi.** Prefer native
USB networking or Ethernet for simultaneous rendered video and sensor streams.
Capture runs only in a visible WiVRn streaming session; ending the session or
hiding the app closes the camera and releases the depth provider.
The PC receiver uses the `vivarium-camera` application data channel, for example
`"app-data": [{"channel": "vivarium-camera", "connect": "127.0.0.1:9760"}]`.
No camera app, separate camera TCP connection, or ADB reverse is needed.

The left passthrough camera produces color H.264 or JPEG frames and on-headset
OpenCV 4.14 ArUco detections in VCAM version 1. Detection still uses luma; the
encoders include the YUV source's chroma. CONFIGURE controls image and detection rates
within the locally enabled streams. Camera modes use their actual timestamp
source for PING/PONG; depth-only uses CLOCK_BOOTTIME and does not query Camera2.
LINK_STATUS reports route `2` (WiVRn) with Android's active-network transport.
A receiver accepts real CALIBRATION or LINK_STATUS as startup readiness and
resends its current CONFIGURE once per TCP connection. Camera frames and marker
reports still require calibration; depth does not. CONFIGURE refreshes calibration
for camera modes or real network status for depth-only, without an echo loop.
Sensor messages are serialized before transport
chunking, so concurrent images, markers, status and depth cannot interleave bytes.

The marker detector is bounded so it cannot starve frame submission (GRID #3095):
it runs on one background-priority thread with OpenCV's worker pool disabled, so
its cost is at most one core and proportional to the frames it is given. Frames
are dropped on the camera thread, before any copy, down to CONFIGURE's detection
rate; while no marker has been seen for a second the client caps that at 15 Hz
regardless of the commanded rate (the PC asks for its full rate until every
target is anchored, and 15 Hz is enough to notice a marker appear). The PC can
still ask for less. Candidates can be searched on a copy of the frame scaled by
`marker_detect_scale` (client configuration, default `1.0`, clamped to 0.25..1;
frames up to 640 px wide are always searched as is), after which each marker's
corners are refined to subpixel precision on the full 1280×960 frame with
OpenCV's own ArUco refinement window, so a scale costs candidate recall but not
corner precision. Half scale is opt-in because it lost recall at the operator's
distance: 70 mm markers at about 1 m are ~50 px a side on the 1280×960 camera,
~25 px at half scale, below the detector's recall floor (one of six markers found,
so the sheet never anchored). Once a minute the client logs
`VivariumMarkers: 14.8 Hz, 11.2 ms/frame, 1 thread, 6 markers max`: the measured
report rate, the detector thread's CPU time per frame, how many threads carry the
`marker-detector` name (OpenCV workers inherit it; more than one means the pool
is back), and the most markers one frame held over the minute, so a scale that
loses recall shows up next to the CPU it saves.

Depth is opt-in through both the local switch and CONFIGURE's optional `>BH`
depth flag/rate tail. The client requests Spatial Data permission only when
depth is requested and requires `XR_META_environment_depth`; unsupported
runtimes keep independently enabled camera and marker streams.
The implementation follows the
[pinned OpenXR 1.1.58 META specification](https://github.com/KhronosGroup/OpenXR-Docs/blob/release-1.1.58/specification/sources/chapters/extensions/meta/meta_environment_depth.adoc):
two D16_UNORM array layers are sampled with a compute shader into a host-visible
storage buffer (no assumed image TRANSFER_SRC usage), retaining the runtime's
SHADER_READ_ONLY_OPTIMAL layout and queue ownership. GPU reads are queued between
xrBeginFrame/xrEndFrame. A newly created provider first acquires on the next
frame, so it observes xrBeginFrame before acquisition; immediate acquisition
in the creation frame returned XR_ERROR_CALL_ORDER_INVALID on Quest 3.
A single in-flight worker converts and sends images without waiting in the
render loop. Near/far planes unproject normalized depth, including
META's far-less-than-near infinite projection; unavailable/max-depth samples
become zero, positive depths round to millimetres and saturate at 65535.

DEPTH type `8` uses `>qqHHBBxx`, then one `>4f7f2f` per view (FOV, STAGE xyz/xyzw,
near/far), then view-major little-endian uint16 millimetres. Compression is raw
(`0`); receiver LZ4 support does not require this sender to compress. Extension
revision 2 supplies capture XrTime; revision 1 falls back to predicted display
XrTime, not a fabricated capture timestamp. The send timestamp follows the PONG
clock. Dimensions and complete raw body are bounded to the 8 MiB VCAM limit.

Offline checks: `./gradlew testDebugUnitTest` exercises the actual Kotlin
serializers against shared receiver goldens. Build and run
`c++ -std=c++20 -O2 -ffast-math client/test/depth_format_test.cpp -o /tmp/depth-format-test`
then `/tmp/depth-format-test client/test/fixtures/camera_stream` for actual C++
DEPTH byte parity and conversion boundaries. Remaining hardware gates include
runtime Vulkan validation, calibrated pose/timestamp alignment, visibility
pause/resume and repeated start/stop checks for camera/provider resource leaks.

Live native-USB verification on Quest 3 (2026-10-28) received two 320×320 depth
maps at 10 Hz, with finite stereo poses and increasing capture timestamps.
Disabling depth stopped frames; re-enabling resumed at 9.93 Hz without
restarting AR.

The independent-controls build `ec884f0d` also passed four 15-second AR runs.
The PC requested all streams in every run; locally disabled streams stayed off.

| Local switches | RGB Hz | Marker reports/s | Depth Hz |
| --- | ---: | ---: | ---: |
| Depth only | 0 | 0 | 10.0 |
| RGB only | 5.0 | 0 | 0 |
| Markers only | 0 | 15.0 | 0 |
| All three | 5.0 | 14.8 | 10.0 |

Depth-only passed with both camera permissions denied and no camera calibration.
RGB-only delivered actual color 1280×960 JPEGs. Marker-only saw all six printed
IDs 0–5; ID 17 also appeared, so this does not establish detection precision.
All-three delivered color images, markers and both 320×320 depth views together.
These short checks do not establish sustained frame quality, calibrated depth
accuracy or visual occlusion; fast-motion jitter and black flashes remain open.
