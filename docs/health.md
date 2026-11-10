# Stream health

The server watches the stream once per second, warns the headset when a rule fires, and keeps
a flight record so an intermittent problem can be analysed afterwards. Design and decisions:
GenRobo/GRID#2834.

Frame handling is unchanged: the monitor reads counters the encoder, the IDR handler and the
application pacer already keep, and the per-frame feedback the headset already sends. One
thread wakes once per second to do the arithmetic. The encoders' own five-second log summary
(`encoder stream N (5 s): presented …`) is gone: the `encoder` object of each sample and the
connector below carry the same counts once per second.

The headset sends `from_headset::telemetry` once per second while streaming: frames received,
incomplete, decoded, dropped before display and repeated; decode time p95; its own CPU and GPU
time; the active refresh rate; the Android thermal status; the requested performance levels;
the active network (kind, Wi-Fi RSSI, link speed); and, when the runtime offers
`XR_META_performance_metrics`, its [runtime counters](#headset-runtime-counters). The server
merges it into the sample, so the headset's own counts are authoritative where it has them. The
server answers with `to_headset::health`, the last sample, which the headset's Statistics tab
shows as its first row. Both packets change the protocol version: a headset app without them is
refused at the lobby with the usual *incompatible* message.

Warnings reach the headset as `server_message::kind::warning`, drawn by the headset app on its
own composition layer in the VIEW reference space (locked to the view, so it does not swim with
tracking latency and stays readable while video frames are lost). The amber panel sits below the
centre of the view for 10 s. Servers from milestone 2 sent `toast_urgent` instead; the client
still shows those as before.

## Configuration

```json
{
	"health": {
		"warnings": true,
		"log": true,
		"app-data": true,
		"frames": false,
		"frames-max-mb": 50,
		"frames-keep": 3,
		"log-dir": "/var/tmp/wivrn",
		"thresholds": { "link_loss.frames_per_s": 5, "app_slow.ratio": 0.9 }
	}
}
```

| Key | Default | Meaning |
| --- | --- | --- |
| `warnings` | `true` | Send a toast to the headset when a rule fires, and *Stream recovered* when all clear |
| `log` | `true` | Write the flight record (below) |
| `app-data` | `true` | Speak the [connector](#connector) protocol on the `wivrn-health` app-data channel, when one is configured |
| `frames` | `false` | Write the [frame record](#frame-record): one line per application frame per stream (`vivarium-teleop xr up --trace-light`, GRID#3095) |
| `frames-max-mb` | `50` | Rotate the frame record at this size |
| `frames-keep` | `3` | Frame record files kept per server run, the oldest deleted first; `0` keeps every file (`vivarium-teleop xr up --trace` sets this, so the file it names outlives a long session) |
| `log-dir` | `%LOCALAPPDATA%\wivrn\logs` on Windows, `$XDG_STATE_HOME/wivrn` or `~/.local/state/wivrn` elsewhere | Where the records go |
| `thresholds` | see below | `"<rule>.<param>": number` overrides; unknown keys are ignored |

## Rules

A rule fires after its condition holds for 3 consecutive seconds, clears after 5 seconds
without it, and does not fire again within 30 s of its last firing. Headset-only fields are
unknown (`-1`) until the headset reports them (milestone 3) and never fire a rule while unknown.

| Rule | Condition (default thresholds) | Headset message |
| --- | --- | --- |
| `link_loss` | lost frames per second ≥ `link_loss.frames_per_s` (5); lost = headset's incomplete frames when known, else IDR requests for lost reference frames | Video frames lost between PC and headset (N/s). Check the headset's adapter and cable; power-cycle the headset if the PC is fine. |
| `link_slow` | send p95 > `link_slow.send_ms` (3) or receive spread p95 > `link_slow.spread_ms` (4) | Network adapter too slow for this bitrate… |
| `encoder_busy` | frames skipped > 0, or NVENC ≥ `encoder_busy.util_pct` (85), or encode p95 > `encoder_busy.period_ratio` (0.8) × period | PC encoder overloaded… |
| `app_slow` | app frames < `app_slow.ratio` (0.9) × display rate, while an application is connected | App rendering at X of Y fps. Close other apps or browsers on the PC. |
| `app_late` | frames delivered after the compositor deadline > `app_late.pct` (5) with the rate normal | App frames arriving late. Raise app-margin-ms… |
| `headset_decode` | decode p95 > `headset_decode.period_ratio` (1.0) × period | Headset decoder slow… |
| `headset_thermal` | thermal status ≥ `headset_thermal.status` (3, Android "severe") | Headset is hot and throttling. |
| `headset_battery` | battery < `headset_battery.pct` (15) and not charging | Headset battery low… |
| `refresh_mismatch` | active refresh differs from requested by > `refresh_mismatch.hz` (1) | Headset running at X Hz, not the requested Y Hz. |
| `judder` | repeated + skipped frames > `judder.pct` (2) % of the display frames settled that second, from the [frame record](#frame-record)'s ring (`frames` below); the 3 s persistence of every rule makes it "for 3 s" | Judder: X % of frames repeated or skipped, mostly *class* |

`judder` names the class that explains most hitches over the three seconds it took to fire,
attributed per frame like `vivarium-teleop xr trace` (GRID#3095 design 6.6, calibrated on the
2026-11-10 laptop record: frames land in their slot, the pacer wakes the application 3.6 periods
ahead, decode ends 36 ms before the slot, the pose is 2.6 ms old at wake and 53 ms old at
photon) but simplified so it runs on the ring's fields once per second: `tracking` (pose not
tracked, or older than two periods **when the application woke** — its age at photon is the
pipeline latency, not a tracking fault); otherwise the stage whose duration exceeds its stream's
rolling median of the last 300 on-time frames by more than half a period — `app` (wake to
submit, GPU), `encode` (compositor, encoder), `link` (last shard sent to received, first to last
packet received; also a frame with no feedback at all), `decode`; otherwise `compositor` (the
headset's) when the frame was decoded at least half a period before its predicted display time
and still repeated, skipped or late; else `unattributed`, in which case the message adds the
largest excess seen (`mostly unattributed (largest excess decode 4.0 ms)`). The headset runtime's
own `compositor_dropped` increase over those seconds counts for `compositor` as well. A repeated
frame is blamed on the frame after it (the late one), as the reader does. Not here: the
`session`, `app-half-rate` and `content-cadence` classes (no event or viewer input), and a
repeat whose successor settles in the next second is `unattributed`.

Thresholds come from measurements on 2026-10-29/30 (Quest 3, RTX 3080 Ti laptop, RTX desktop,
HEVC 10-bit, 50.8 Mb/s per eye): clean sessions showed 0–12 IDR requests per session and
0 encoder skips; a lossy USB adapter 225 per 30 s; a saturated NVENC 464 per 30 s at 88 %;
a competing browser dropped the application from 120 Hz to 66–82 Hz.

## Flight record

`wivrn-health-<time>.jsonl` holds one JSON object per second:

```json
{"t":1761866000,
 "link":{"shards_sent":4576,"bytes_sent":6105057,"send_ms_p95":0.0,"receive_spread_ms_p95":0.38,
         "frames_incomplete":-1,"idr_lost_ref":0,"idr_unsent_gap":0},
 "headset":{"decode_ms_p95":4.4,"display_jitter_ms_p95":0.1,"frames_repeated":0,"display_hz":120,
            "requested_hz":120,"thermal_status":-1,"battery_pct":62,"charging":true,"gpu_ms":-1,"cpu_ms":-1,
            "compositor_dropped":0,"compositor_stale":-1,"compositor_gpu_ms":2.1,"app_gpu_ms":3.4,
            "runtime_m2p_ms":38.2,"cpu_util_pct":41,"gpu_util_pct":57},
 "encoder":{"presented":360,"skipped":0,"encode_ms_p95":2.8,"util_pct":29},
 "app":{"hz":120,"cpu_ms":3.3,"gpu_ms":0.15,"late_pct":0.0,"half_rate_probes":0,"half_rate_recoveries":0},
 "motion_to_photon_ms_p95":26.5,
 "frames":{"display":120,"repeated":0,"skipped":0,"link":0,"app":0,"encode":0,"decode":0,"compositor":0,"tracking":0,"unattributed":0},
 "warnings":[]}
```

- Times are milliseconds; percentiles come from 32-bucket logarithmic histograms (bucket
  width 25 %), reset every second. `-1` means unknown.
- `encoder.presented` counts all three streams (two eyes and the alpha stream).
- `link.idr_unsent_gap` is reserved: it needs encoder frame-id tracking and is 0 for now.
- `app.half_rate_probes` counts the frames the application pacer predicted one period out to
  test a half-rate lock-in (an application delivering at a steady 2x or 3x the display period,
  see [app-margin-ms](configuration.md#app-margin-ms)), `app.half_rate_recoveries` the ones the
  application answered in time, after which it is paced at one period again. A probe per second
  with no recovery is an application that really needs two periods.
- `app_metrics` appears when an application posted metrics on the [connector](#connector)
  during that second, `app_metrics_dropped` when it posted lines the server rejected, and
  `app_warnings` lists the application warnings still within their ttl.
- `frames` is the judder rule's input: the application frames that settled this second (the
  [frame record](#frame-record)'s rule, every stream), `display` the display frames they
  account for (the sum of `shown`; `-1` while none settled), `repeated` and `skipped` the
  hitches, and the class counts the hitches by cause as the `judder` rule attributes them
  (repeats and late frames both count, so the classes can add up to more than `repeated +
  skipped`); `excess_stage` / `excess_ms` the largest stage excess among the unattributed ones
  (`""` / `-1` when none).
- Event lines (`"type":"event"`, `"type":"warning"`) are interleaved with the samples in the
  connector stream (below); the file holds them too, so a reader must look at `type`.

When a rule fires, `wivrn-health-event-<time>.json` is written with the sample and the last
600 frames per stream from the [frame record](#frame-record)'s ring (`streams[s]` is a list of
`frame` objects, times relative to `t0_mono_ns`), so the seconds before the event can be
inspected frame by frame whether or not the frame record is on.

### Headset runtime counters

The headset app asks for `XR_META_performance_metrics` and, when the runtime offers it (the
client logs `XR_META_performance_metrics: available|absent` and then the runtime's counter list
as `XR_META_performance_metrics counters (N): …`), enables it with
`xrSetPerformanceMetricsStateMETA` and reads these counters with
`xrQueryPerformanceMetricsCounterMETA` once per second, with the rest of the telemetry:

| Sample field (`headset.*`) | Counter path | Unit |
| --- | --- | --- |
| `compositor_dropped` | `/perfmetrics_meta/compositor/dropped_frame_count` | frames, over the runtime's own interval |
| `compositor_stale` | `/perfmetrics_meta/compositor/stale_frame_count` when the runtime lists it; else the refresh rate minus `/perfmetrics_meta/compositor/frames_per_second` when it lists that | frames |
| `compositor_gpu_ms` | `/perfmetrics_meta/compositor/gpu_frametime` | ms |
| `app_gpu_ms` | `/perfmetrics_meta/app/gpu_frametime` | ms |
| `runtime_m2p_ms` | `/perfmetrics_meta/app/motion_to_photon_latency` | ms |
| `cpu_util_pct` | `/perfmetrics_meta/device/cpu_utilization_average` | % |
| `gpu_util_pct` | `/perfmetrics_meta/device/gpu_utilization` | % |

A path the runtime does not list (the extension's specification only says which paths *may*
exist) is not queried and its field stays unknown (`-1`; `0xFFFF` / `255` on the wire). The
two `compositor_stale` sources are not in the specification's list at all: the field is unknown
until a runtime is seen listing one of them. The counters are the runtime's view of the headset
compositor, so a repeated frame with `compositor_dropped` rising and the pipeline on time is the
headset, not the stream.

Reading a record with Python:

```python
import json, statistics
rows = [json.loads(l) for l in open("wivrn-health-20261030-154559.jsonl")]
print(statistics.median(r["motion_to_photon_ms_p95"] for r in rows))
```

## Frame record

With `health.frames` on, every application frame leaves one line per video stream in
`wivrn-frames-<time>.jsonl` (same directory as the flight record), the input of
`vivarium-teleop xr trace` (GRID#3095). The file opens when a headset session starts and
closes when it ends; it rotates at `frames-max-mb` (a new `wivrn-frames-<time>.jsonl`, the
oldest beyond `frames-keep` deleted; `frames-keep: 0` deletes nothing). At 72 Hz with two
streams 50 MB is about nine minutes. UTF-8, one JSON object per line, LF.

The first line is the header:

```json
{"type":"header","version":1,"t0_mono_ns":123456789012,"t0_unix_ms":1794270000123,"host":"GRGeordieUbuntuLaptop","server":"26.9-45-gd09ec1f9","streams":2,"display_hz":72.0,"period_ns":13888888}
```

Then one `frame` line per application frame per stream (`s` < `streams`), written once the
headset has displayed a frame three beyond it on that stream (its three-deep frame buffer has
released it, so the repeat count is final) or 2 s after its predicted display time without
that, whichever comes first; a row is never written twice, feedback arriving after it is dropped:

```json
{"type":"frame","n":18221,"s":0,"pd":2217133,"wake":2203280,"begin":2203310,"submit":2205100,"gpu":2205900,"comp0":2206000,"comp1":2207200,"enc0":2207250,"enc1":2210800,"send0":2210850,"send1":2211400,"rx0":2212100,"rx1":2213300,"dec0":2213350,"dec1":2217000,"blit":2217050,"disp":2217140,"shown":1,"bytes":41230,"idr":0,"shards":31,"lost":0,"pose_t":2202900,"pose_flags":15,"off_us":1170.5}
```

- Every time is **server monotonic microseconds since `t0_mono_ns`**, a whole number.
  Headset-side times (`rx0`, `rx1`, `dec0`, `dec1`, `blit`, `disp`) are converted with the
  session's clock offset before the subtraction. A value that is not known is `null`, never
  `-1`, never omitted.
- `n` is the compositor's frame index (the one the encoder and the headset's feedback use),
  `s` the stream, `pd` the predicted display time.
- `wake`, `begin`, `submit`: the application woke from `xrWaitFrame`, called `xrBeginFrame`,
  called `xrEndFrame`; `gpu`: its GPU work finished. These are the application frame the
  compositor latched into frame `n`; when the application has not delivered a new one, the
  previous frame is latched again and the row repeats its application timings.
- `comp0`, `comp1`: the compositor started and its GPU work (layer squash, foveation) finished.
- `enc0`, `enc1`: encode begin and end; `send0`, `send1`: first and last shard sent; `bytes`
  the encoded size, `shards` the shards sent, `idr` 1 when the frame went on the reliable
  socket (every encoder's IDR frames).
- `rx0`, `rx1`: the headset received the first and last packet; `dec0`, `dec1`: sent to and
  received from its decoder; `blit`, `disp`: composed and displayed; `shown` the times it was
  displayed (0 = never, a frame dropped before display); `lost` 0 when the headset decoded the
  frame, `null` when it reported it incomplete (the feedback packet does not carry a count).
- `pose_t`: the tracking sample the application's view pose for this frame came from;
  `pose_flags`: the `XrViewStateFlags` of that pose (bit 0 orientation valid, bit 1 position
  valid, bit 2 orientation tracked, bit 3 position tracked; 0 = no pose). `null` when the
  application did not locate its views for this frame's display time.
- `off_us`: the clock-offset estimate (headset monotonic time minus server monotonic time) in
  microseconds when the feedback arrived. It is the whole offset, hundreds of seconds when the
  two booted at different times (824 394 750.7 on the 2026-11-10 laptop run); what matters is
  its change over the file, the drift between the two clocks (32 µs/s there).

Once per second a `clock` line records the same offset (`off_us`, `null` before the first
feedback) and `jitter_us`, the spread (max − min) of the estimate over that second; session
events are `event` lines; `value` is a string, a number, a boolean or `null`:

```json
{"type":"clock","t":5000000,"off_us":1170.5,"jitter_us":42.0}
{"type":"event","t":1500,"name":"session_state","value":"FOCUSED"}
```

| `name` | `value` |
| --- | --- |
| `session_state` | the `XrSessionState` without its prefix (`VISIBLE`, `FOCUSED`, `STOPPING`, …) |
| `idr_request` | why the encoder was asked for an IDR: `lost_reference`, `dropped_idr`, `timeout` |
| `bitrate` | the new bitrate in bit/s |
| `recenter` | `null` |
| `warning` | `"<rule> fired"` or `"<rule> cleared"` |
| `interaction_profile` | the left and right profiles, space separated |
| `user_presence` | `true` / `false` |
| `display_hz` | the new refresh rate (the header carries the initial one) |
| `pause`, `resume` | `null` (the headset went away and came back) |

Frame paths only store into a fixed ring of 4096 frames (relaxed atomic stores, no allocation,
no lock); the monitor thread serialises finished rows once per second. The same lines go out on
the [connector](#connector) when `health.app-data` is on.

```python
import json
rows = [json.loads(l) for l in open("wivrn-frames-20261110-154559.jsonl")]
header, frames = rows[0], [r for r in rows if r["type"] == "frame" and r["s"] == 0]
late = [f for f in frames if f["disp"] and f["disp"] - f["pd"] > header["period_ns"] / 2000]
print(len(late), "frames displayed more than half a period late")
```

## Connector

An external process (the GRID link monitor, a simulation backend) reads the stream health and
posts its own warnings and metrics over the `wivrn-health` [app-data](configuration.md#app-data)
channel. Configure a sink and keep `health.app-data` on:

```json
{ "app-data": [{ "channel": "wivrn-health", "connect": "127.0.0.1:9761" }] }
```

Port convention on a GRID PC: the Vivarium backend's camera sink listens on 9760
(`vivarium-camera`) and its health sink on 9761 (`wivrn-health`, `vivarium-teleop launch
--health-port`); the GRID link monitor serves `/link-stats` on 9762 and reads the health sink
through the backend (`GET /sessions/<id>/health`) rather than connecting here itself.

The server connects to the sink as a TCP client while a headset session is active (retrying
every 2 s) and both sides write UTF-8 JSON lines, one object per line, `\n`-terminated. Unlike
other channels, nothing on `wivrn-health` reaches the headset app: the server itself is the
peer. The sink must tolerate reconnects and reset its line buffer when the connection drops.

### Server to sink

Each line has a `type`:

| `type` | When | Content |
| --- | --- | --- |
| `sample` | once per second | the [flight record](#flight-record) object, plus `"session":"paused"` while the headset is away |
| `warning` | a rule or an application warning changes state | `t`, `id` (rule name or application id), `state` (`fired`/`cleared`), `message` (when fired), `source: "app"` for application warnings |
| `event` | session lifecycle | `t` (unix seconds), `event` (`session_paused`, `session_resumed`) |
| `header`, `frame`, `clock`, `event` | with `health.frames` on | the [frame record](#frame-record) lines, unchanged: a frame-record `event` has `name` and `value` and its `t` is microseconds since the header's `t0_mono_ns`; the lifecycle `event` above has `event` and a unix `t` |

```json
{"t":1761866000,"type":"sample","link":{...},"headset":{...},"encoder":{...},"app":{...},"motion_to_photon_ms_p95":26.5,"warnings":["link_loss"],"app_metrics":{"sim_realtime_ratio":0.6}}
{"t":1761866003,"type":"warning","id":"link_loss","state":"fired","message":"Video frames lost between PC and headset (8/s). ..."}
{"t":1761866010,"type":"warning","id":"sim_slow","state":"fired","message":"Simulation at 0.6x realtime","source":"app"}
{"t":1761866020,"type":"warning","id":"sim_slow","state":"cleared","source":"app"}
```

### Sink to server

| `type` | Fields | Effect |
| --- | --- | --- |
| `warning` | `id` `[a-z0-9_]{1,32}`, `message` 1–200 characters, `ttl_s` number 1–600 (default 10) | Shown in the headset like a rule warning (`server_message::kind::warning`, amber view-locked panel) and written as a `warning` line with `"source":"app"`. The id is listed in `app_warnings` for `ttl_s` seconds and, like the rules, cannot fire again within 30 s of its last firing (or within its ttl if longer); posts in that window are silently merged. At most 256 distinct ids are tracked at once. |
| `metric` | `name` `[a-z0-9_]{1,32}`, `value` finite number; other keys ignored | Merged into the next `sample` under `app_metrics`; the last value posted for a name during the second wins. Metrics are not evaluated by any rule. |

```json
{"type":"warning","id":"sim_slow","message":"Simulation at 0.6x realtime","ttl_s":10}
{"type":"metric","name":"sim_realtime_ratio","value":0.6}
```

Validation: a line is at most 4096 bytes without the newline, a JSON object, with a known
`type` and the fields above. Anything else (a longer line, which is skipped up to its newline;
a non-object; an unknown type such as `frame`; a bad id, value or message) is dropped and
counted in the next sample's `app_metrics_dropped`; it never disconnects the sink or affects
the stream. The server does not answer individual lines. With `health.warnings` off, posted
warnings are recorded but not shown in the headset.

Posting from Python:

```python
import json, socket
srv = socket.create_server(("127.0.0.1", 9761))
conn, _ = srv.accept()  # the WiVRn server connects when a headset session starts
conn.sendall((json.dumps({"type": "metric", "name": "sim_realtime_ratio", "value": 0.6}) + "\n").encode())
for line in conn.makefile():
    row = json.loads(line)
    if row["type"] == "sample":
        print(row["motion_to_photon_ms_p95"], row.get("app_metrics"))
```

## Testing

`cmake -DWIVRN_BUILD_TEST=ON` adds `wivrn-health-test` (the rules replayed over recorded
sessions in `server/tests/health_fixtures/`), `wivrn-health-frames-test` (the frame record:
ring to JSON line round trip, rotation, the 2 s settle, nothing before the record starts) and,
on Windows, `tools/windows/health-test.ps1` (server, `hello_xr` and the fake headset dropping
frames; expects the toast and the record). Run them with `ctest -R health`.
