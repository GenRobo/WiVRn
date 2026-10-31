# Stream health

The server watches the stream once per second, warns the headset when a rule fires, and keeps
a flight record so an intermittent problem can be analysed afterwards. Design and decisions:
GenRobo/GRID#2834.

Frame handling is unchanged: the monitor reads counters the encoder, the IDR handler and the
application pacer already keep, and the per-frame feedback the headset already sends. One
thread wakes once per second to do the arithmetic.

The headset sends `from_headset::telemetry` once per second while streaming: frames received,
incomplete, decoded, dropped before display and repeated; decode time p95; its own CPU and GPU
time; the active refresh rate; the Android thermal status; the requested performance levels;
and the active network (kind, Wi-Fi RSSI, link speed). The server merges it into the sample,
so the headset's own counts are authoritative where it has them. The server answers with
`to_headset::health`, the last sample, which the headset's Statistics tab shows as its first row.
Both packets change the protocol version: a headset app without them is refused at the lobby
with the usual *incompatible* message.

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
		"log-dir": "/var/tmp/wivrn",
		"thresholds": { "link_loss.frames_per_s": 5, "app_slow.ratio": 0.9 }
	}
}
```

| Key | Default | Meaning |
| --- | --- | --- |
| `warnings` | `true` | Send a toast to the headset when a rule fires, and *Stream recovered* when all clear |
| `log` | `true` | Write the flight record (below) |
| `app-data` | `true` | Publish samples and warnings on the `wivrn-health` app-data channel (milestone 4) |
| `log-dir` | `%LOCALAPPDATA%\wivrn\logs` on Windows, `$XDG_STATE_HOME/wivrn` or `~/.local/state/wivrn` elsewhere | Where the record goes |
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
            "requested_hz":120,"thermal_status":-1,"battery_pct":62,"charging":true,"gpu_ms":-1,"cpu_ms":-1},
 "encoder":{"presented":360,"skipped":0,"encode_ms_p95":2.8,"util_pct":29},
 "app":{"hz":120,"cpu_ms":3.3,"gpu_ms":0.15,"late_pct":0.0},
 "motion_to_photon_ms_p95":26.5,
 "warnings":[]}
```

- Times are milliseconds; percentiles come from 32-bucket logarithmic histograms (bucket
  width 25 %), reset every second. `-1` means unknown.
- `encoder.presented` counts all three streams (two eyes and the alpha stream).
- `link.idr_unsent_gap` is reserved: it needs encoder frame-id tracking and is 0 for now.
- `app_metrics` appears when an application posted metrics on the connector (milestone 4).

When a rule fires, `wivrn-health-event-<time>.json` is written with the sample and the last
600 per-frame timelines per stream (`columns` names the fields; timestamps are the headset's
clock in nanoseconds), so the seconds before the event can be inspected frame by frame.

Reading a record with Python:

```python
import json, statistics
rows = [json.loads(l) for l in open("wivrn-health-20261030-154559.jsonl")]
print(statistics.median(r["motion_to_photon_ms_p95"] for r in rows))
```

## Testing

`cmake -DWIVRN_BUILD_TEST=ON` adds `wivrn-health-test` (the rules replayed over recorded
sessions in `server/tests/health_fixtures/`) and, on Windows, `tools/windows/health-test.ps1`
(server, `hello_xr` and the fake headset dropping frames; expects the toast and the record).
Run them with `ctest -R health`.
