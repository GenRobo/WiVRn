# Profiling WiVRn

Performance analysis for WiVRn: [Perfetto](https://perfetto.dev/) tracing.
For general debugging see [docs/debugging.md](debugging.md).

## Quick start

Build with tracing (`server-tracing` preset = `server` + `WIVRN_USE_PERFETTO=ON`):
```bash
cmake --preset server-tracing
cmake --build build-server-tracing
```

Self-contained **in-process** trace (no daemon, no script):
```bash
WIVRN_TRACING=inprocess build-server-tracing/server/wivrn-server
```
Connect the HMD, stream, disconnect. Each session writes a `wivrn-<timestamp>-<pid>.pftrace` in cwd;
open it in <https://ui.perfetto.dev>.

**System-backend** capture via the daemon + wrapper:
```bash
WIVRN_TRACING=system wivrn-server     # one shell, with a client streaming
tools/perfetto/wivrn_capture.py       # WiVRn-only; --full adds Monado + ftrace
```

`WIVRN_USE_PERFETTO` defaults `OFF`; even built `ON`, tracing is inert until `WIVRN_TRACING` is set.
The Perfetto amalgamated SDK must be installed where CMake looks (`/usr/share/perfetto/sdk`, override
with `-DPERFETTO_SDK_DIR=<dir>`); no automatic fetch.

### Windows

Use a Developer PowerShell with an already configured Windows server build. Extract the
official `perfetto-cpp-sdk-src.zip` into a directory containing both `perfetto.h` and
`perfetto.cc`; SDK v59.1 has been built with MSVC 2022 and GCC 13.

```powershell
cmake -S . -B build-win -DWIVRN_USE_PERFETTO=ON -DPERFETTO_SDK_DIR=C:/deps/perfetto/sdk
cmake --build build-win --target wivrn-server
$env:WIVRN_TRACING = "inprocess"
$env:WIVRN_TRACING_FILE = "$PWD/session.pftrace"
.\build-win\server\wivrn-server.exe
```

Keep the usual server arguments/configuration. Connect the headset, run the application,
then disconnect the headset to export the trace. Ending only the OpenXR application does
not necessarily disconnect the headset. Allow the export to finish before terminating the
server; forced process termination cannot flush its in-memory buffer. Normal shutdown may
also write a small final trace after the disconnect export.

Remove the environment variables when finished; a tracing-enabled build is inert by default:

```powershell
Remove-Item Env:WIVRN_TRACING, Env:WIVRN_TRACING_FILE
```


## HMD profiling

```bash
adb shell setprop persist.traced.enable 1
```
Then use the [Perfetto UI](https://ui.perfetto.dev/) → connect device. See the
[Android tracing quickstart](https://perfetto.dev/docs/quickstart/android-tracing).

## Backends — `WIVRN_TRACING`

`WIVRN_TRACING` enables tracing and selects how it is collected. Unset/`off` = disabled; unknown
values warn and stay off.

| Value              | Behavior |
| ------------------ | -------- |
| `system`           | Connect to an external `traced` daemon; an external consumer owns the session. The only mode that gives a unified system-wide capture. Use `tools/perfetto/wivrn_capture.py`. |
| `inprocess`        | wivrn-server runs the service itself and writes a self-contained `.pftrace`. No daemon, no sudo. WiVRn track events only — no ftrace/system data. |
| `system+inprocess` | Both, independently → two separate traces (not merged). |

The system backend and daemon wrapper are Linux-only in WiVRn. On Windows, `system` warns
and leaves tracing disabled; `system+inprocess` warns and retains only in-process capture.


In-process options:
- `WIVRN_TRACING_FILE` — output path. A trace is written on **each client disconnect** and at server
  shutdown, so one run can produce several files. Unset → timestamped `wivrn-<timestamp>-<pid>.pftrace`
  in cwd. An explicit value expands `%t` (timestamp)/`%p` (pid) and gains a `-2`, `-3`, … suffix
  rather than overwriting. Written from the per-session child, so use an absolute path if unsure;
  parent dirs are created.
- `WIVRN_TRACING_BUFFER_KB` — ring buffer size in KiB (default `65536`).

A headset must be streaming during the capture window. Confirm setup by raising Monado's log level
(default `warn` hides the lines):
```bash
XRT_LOG=info WIVRN_TRACING=system wivrn-server
# → wivrn::trace: producer socket /run/user/<uid>/wivrn-perfetto-producer.sock
# → wivrn::trace: init complete
```

> Perfetto's default socket is root-only, so WiVRn (and the wrapper) use
> `$XDG_RUNTIME_DIR/wivrn-perfetto-{producer,consumer}.sock` (fallback `/tmp`). Override via
> `PERFETTO_PRODUCER_SOCK_NAME`.


## Capturing — `wivrn_capture.py`

A wrapper around `tracebox` (downloaded on first run): starts the tracing services, waits for the
wivrn-server producer, captures, and tears down. If no producer connects within 20s it **aborts**
rather than write an empty trace — start wivrn-server with `WIVRN_TRACING=system`, stream, retry.

```bash
tools/perfetto/wivrn_capture.py                       # default: WiVRn-only, no ftrace, no sudo
tools/perfetto/wivrn_capture.py --full                # + every producer + broad ftrace (needs sudo)
tools/perfetto/wivrn_capture.py --full --cpu-only     # full producers, ftrace stripped (no sudo)
tools/perfetto/wivrn_capture.py --duration-ms 5000 -o my.pftrace
tools/perfetto/wivrn_capture.py -c custom.pbtx        # any Perfetto config
```

`--cpu-only` strips any `linux.ftrace` block from the config. The wrapper auto-detects ftrace in the
config and only then starts `traced_probes` + uses `sudo`.

### Default vs `--full`

| Aspect | default (`wivrn_trace_cfg.pbtx`) | `--full` (`wivrn_full_trace_cfg.pbtx`) |
| ------ | -------------------------------- | -------------------------------------- |
| track_event | WiVRn categories only       | every category, every producer |
| ftrace | none                             | sched + freq + idle + IRQs + vblank |
| Buffers | 64 MiB (track_event)            | 64 MiB track_event **+** 256 MiB system |
| sudo   | no                               | yes (ftrace) |
| Use for | encoder-pipeline questions      | "something outside WiVRn is doing X" |

`--full` isolates `track_event` in its own buffer so the high-rate ftrace stream can't evict the
WiVRn/Monado slices, and defaults to a 5 s window (override with `--duration-ms`). If a trace is too
big to load, drop the `irq/*` events from a copy of the config.

### Manual capture

Drive Perfetto yourself against WiVRn's default socket pair:
```bash
SOCK_DIR="${XDG_RUNTIME_DIR:-/tmp}"
export PERFETTO_PRODUCER_SOCK_NAME="$SOCK_DIR/wivrn-perfetto-producer.sock"
export PERFETTO_CONSUMER_SOCK_NAME="$SOCK_DIR/wivrn-perfetto-consumer.sock"

traced --background                                    # add sudo + traced_probes for ftrace configs
WIVRN_TRACING=system wivrn-server                      # in another shell, streaming
perfetto -c tools/perfetto/wivrn_trace_cfg.pbtx --txt -o trace.pftrace
```
For ftrace configs, run `traced`/`traced_probes` under `sudo -E` and add
`--set-socket-permissions "$USER:0660:$USER:0660"` to `traced`.

### Summaries

`tools/perfetto/pftrace_summary.py` prints per-slice stats, or a side-by-side diff for two traces:
```bash
tools/perfetto/pftrace_summary.py nvenc.pftrace                 # one trace
tools/perfetto/pftrace_summary.py nvenc.pftrace vulkan.pftrace  # diff with Δmean
```

### Frame and pose correlation

The existing headset connection already returns per-stream feedback. No additional network
port, headset build, or per-frame log-file flushing is required. In-process capture uses the
bounded buffer above; the following instants use the existing compositor/feedback categories:

| Events | Meaning |
| --- | --- |
| `app_prediction`, `compositor_prediction` | Frame ID, predicted target, wake time and period. `app_id=0` denotes the compositor; positive IDs identify app-pacer lifetimes. |
| `app_wake`, `app_begin`, `app_submit_begin`, `app_submit_end`, `app_delivered`, `app_gpu_done`, `app_discarded`, `app_retired` | Application lifecycle milestones, when the runtime supplies the corresponding callback. |
| `app_latched` | Exact association from `(app_id, frame)` to `system_frame`, the compositor/video frame ID. The same app frame can be latched more than once. |
| `head_pose_sample` | Incoming head sample's production time, prediction target and validity flags. Production time is not physical sensor acquisition time. |
| `head_pose_query` | Requested target, outer-clamped target and returned relation flags. |
| `head_pose_evaluation` | Actual position/orientation evaluation or fallback targets, with production provenance. This exposes the interpolator's inner clamp separately. |
| `feedback_summary` | Video `frame`, `stream`, `selection_count`, clock validity and raw headset/converted host predicted display targets. |

Join `app_latched.system_frame` to encoder and feedback `frame`, using `stream` to distinguish
video streams. A latch does not guarantee encoded output: existing skip paths still apply.
`app_gpu_done` is a host notification after waiting for GPU completion, not a hardware GPU
timestamp. The notification can precede publication of the ready layer to the scheduled slot.
Pose queries can be correlated by target time, but do **not** carry an exact application-frame
ID. Polynomial production timestamps describe provenance, not necessarily one contributing
sample. Missing evaluation timestamps retain `INT64_MIN`; inspect the validity flags.

Event timestamps explicitly declare the host monotonic clock domain. Payload fields ending
in `_host_ns` remain Monado host-monotonic nanoseconds; `_headset_ns` fields use the headset
OpenXR clock. On Windows, event timestamps account for any difference between Monado's integer
QPC scale and Perfetto's QPC scale. Converted headset times use the existing offset estimator:
`clock_valid` does not imply zero synchronization error.
Perfetto can normalize `slice.ts` to another trace clock, such as BOOTTIME on Linux. Use the
trace's `clock_snapshot` mappings before subtracting raw host payload times from `slice.ts`;
assuming the clocks share an epoch produces incorrect results after system suspend.

`predicted_display_target` replaces the misleading `display` instant: its time is OpenXR
`predictedDisplayTime`, **not measured scanout**. Likewise, `selection_count` is the client's
existing uint8 frame-selection/reuse counter, can wrap, and does not prove physical
presentations. Several feedback packets may describe the same `(frame, stream)`; do not sum
their selection counts as independent displayed frames.

Inspect these events in the Perfetto UI or with the official trace processor. For example,
this query finds repeated/skipped application frames behind consecutive compositor frames.
Exclude startup and shutdown when interpreting a steady-state interval.

```sql
WITH latches AS (
  SELECT ts,
         EXTRACT_ARG(arg_set_id, 'debug.app_id') AS app_id,
         EXTRACT_ARG(arg_set_id, 'debug.frame') AS app_frame,
         EXTRACT_ARG(arg_set_id, 'debug.system_frame') AS video_frame
  FROM slice WHERE name = 'app_latched'
), steps AS (
  SELECT *, app_frame - LAG(app_frame) OVER (
    PARTITION BY app_id ORDER BY video_frame
  ) AS frame_step
  FROM latches
)
SELECT video_frame, app_frame, frame_step
FROM steps WHERE frame_step != 1 ORDER BY video_frame;
```

### Investigating application-ready misses

When a latch reuses an application frame, compare that application's next frame with the
latch. A late `app_gpu_done` notification places the ready-frame delay upstream of encoding
and transport; it does not, by itself, distinguish GPU work from host scheduling delay.
Cross-thread spans on a shared track are not necessarily nested work: use frame identities
and milestone timestamps rather than inferring causality from their visual nesting.

For a controlled timing-headroom comparison, the existing `U_PACING_APP_MIN_MARGIN_MS`
environment variable selects the application pacer's minimum margin (default `1.5` ms).
Set it only on the server process before startup; for example, compare `1.5` and `3` with
the same scene, refresh rate, codec, bitrate and sensor settings. Capture separate traces and
exclude startup/shutdown. A larger margin wakes the application earlier and can reduce
ready-frame misses, but increases render-ahead time and can age application input.
It is a diagnostic control, not a universal jitter fix. Restore the unset/default setting
afterward, and verify moving-headset behavior before retaining any override.

### Compositor timing history

The compositor estimates present-to-decode latency from one continuous rolling history.
Every 100 stream-0 frames, the background worker snapshots the valid durations under the
pacer lock and computes their percentile outside it. A reset invalidates in-flight
calculations so samples from the previous session cannot restore an obsolete estimate.

Do not swap independent histories between the feedback and worker threads: successive
estimates then describe different subsets of recent frames. This can abruptly advance
the compositor wake deadline and reuse an application frame that was ready for the
previous schedule. Compare `compositor_prediction` wake/target fields with `app_latched`;
a whole-frame gap before `begin` is not evidence of network packet loss.

The regression exercises alternating fast/slow feedback blocks and history reset through
the real pacer. It needs a configured server build but no headset or tracing:

```bash
cmake --build build-server --target wivrn-pacer-test
build-server/server/wivrn-pacer-test
```

For an existing Windows build, use `build-win` and run
`build-win/server/wivrn-pacer-test.exe`. This test does not replace a live workload trace
or moving-headset visual acceptance.


## Tracing Monado too

Monado's own `u_trace` (built on percetto) can join the same session. Build with
`WIVRN_TRACE_MONADO=ON` (requires `WIVRN_USE_PERFETTO=ON`):
```bash
cmake --preset server-tracing-monado
cmake --build build-server-tracing-monado

WIVRN_TRACING=system wivrn-server     # also sets Monado's XRT_TRACING for you
tools/perfetto/wivrn_capture.py --full
```

- **System backend + `--full` only.** Monado's percetto can't write in-process, and the default
  config allowlists only `wivrn_*` categories.
- **percetto must be available** at configure time (pkg-config, or `-DPercetto_ROOT_DIR=<path>`);
  Monado does not fetch it. Missing → `XRT_HAVE_PERCETTO specified but not available`.
- Links a second Perfetto SDK copy (percetto's); both feed one `traced`. Leave off unless you need
  Monado spans.

## Adding instrumentation

Include `utils/wivrn_trace.h`; no `#ifdef` at call sites.

```cpp
// CPU span (RAII; ends at scope exit)
wivrn::trace::scope s(wivrn::trace::cpu_track::encoder, stream_idx, frame_index, "my_span");

// CPU instant
wivrn::trace::cpu_instant(wivrn::trace::cpu_track::compositor, "my_event", 0, 0);

// CPU begin/end pair (lifetime spans multiple calls)
wivrn::trace::cpu_begin(wivrn::trace::cpu_track::network, stream_idx, frame_idx, "Send");
wivrn::trace::cpu_end  (wivrn::trace::cpu_track::network, stream_idx, frame_idx, "Send");

// GPU span: bracket work with a gpu_timestamp_pool's cmd_begin/cmd_end, emit after the fence
if (auto s = pool.collect(slot))
        wivrn::trace::gpu_slice(wivrn::trace::gpu_track::nvenc_copy,
                                "my_gpu_span", s->begin_ns, s->end_ns, s->frame_index, stream);
```

GPU begin/end come from the pool already in host-monotonic ns (via `VK_EXT_calibrated_timestamps`).
A disabled pool (no timestamp bits, or `WIVRN_TRACING` unset) returns `nullopt`. `name` must outlive
the emit (a literal is fine). Place the span at the operation's semantic start, not the function top.

| `cpu_track` | category | track |
| ----------- | -------- | ----- |
| `encoder` | `wivrn_encoder` | WiVRn encoder |
| `compositor` | `wivrn_compositor` | WiVRn compositor |
| `network` | `wivrn_network` | WiVRn network |
