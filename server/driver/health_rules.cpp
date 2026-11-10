/*
 * WiVRn stream health: rules, thresholds, histogram and the flight-recorder line.
 * Design: https://app.notion.com/p/409f908c414681fdbb5fe32145044300 (GRID #2834).
 * Pure functions over health::sample; the monitor thread calls them once per second.
 */
#include "health_rules.h"

#include <algorithm>
#include <cmath>
#include <format>
#include <nlohmann/json.hpp>

namespace wivrn::health
{

// --- histogram ---------------------------------------------------------------------------

namespace
{
constexpr double bucket_base_ms = 0.1;
constexpr double bucket_growth = 1.25;
const double log_growth = std::log(bucket_growth);
} // namespace

size_t log_histogram::bucket(double ms)
{
	if (not(ms >= bucket_base_ms))
		return 0;
	double index = 1 + std::floor(std::log(ms / bucket_base_ms) / log_growth);
	return size_t(std::min<double>(index, buckets - 1));
}

void log_histogram::observe(double ms)
{
	counts[bucket(ms)]++;
}

double log_histogram::percentile(double p) const
{
	uint64_t n = total();
	if (n == 0)
		return 0;
	uint64_t target = uint64_t(std::ceil(p * n));
	uint64_t seen = 0;
	for (size_t i = 0; i < buckets; ++i)
	{
		seen += counts[i];
		if (seen >= target)
			return i == 0 ? 0 : bucket_base_ms * std::pow(bucket_growth, double(i - 1));
	}
	return bucket_base_ms * std::pow(bucket_growth, double(buckets - 2));
}

uint64_t log_histogram::total() const
{
	uint64_t n = 0;
	for (auto c: counts)
		n += c;
	return n;
}

void log_histogram::clear()
{
	counts.fill(0);
}

// --- names and thresholds ----------------------------------------------------------------

namespace
{
constexpr std::array<std::string_view, 10> rule_names{
        "link_loss",
        "link_slow",
        "encoder_busy",
        "app_slow",
        "app_late",
        "headset_decode",
        "headset_thermal",
        "headset_battery",
        "refresh_mismatch",
        "judder",
};

// Defaults from the 2026-10-29/30 measurements (design §4).
const std::map<std::string, float> default_thresholds{
        {"link_loss.frames_per_s", 5},
        {"link_slow.send_ms", 3},
        {"link_slow.spread_ms", 4},
        {"encoder_busy.util_pct", 85},
        {"encoder_busy.period_ratio", 0.8f},
        {"app_slow.ratio", 0.9f},
        {"app_late.pct", 5},
        {"headset_decode.period_ratio", 1.0f},
        {"headset_thermal.status", 3}, // Android PowerManager: 3 = severe
        {"headset_battery.pct", 15},
        {"refresh_mismatch.hz", 1},
        {"judder.pct", 2}, // GRID #3095 design 6.7
};
} // namespace

std::string_view name(rule_id id)
{
	return rule_names[size_t(id)];
}

std::optional<rule_id> rule_from_name(std::string_view n)
{
	for (size_t i = 0; i < rule_names.size(); ++i)
		if (rule_names[i] == n)
			return rule_id(i);
	return std::nullopt;
}

thresholds thresholds::defaults()
{
	return {default_thresholds};
}

thresholds thresholds::with(const std::map<std::string, float> & overrides)
{
	thresholds t = defaults();
	for (const auto & [key, value]: overrides)
		if (t.values.contains(key))
			t.values[key] = value;
	return t;
}

float thresholds::get(std::string_view key) const
{
	auto it = values.find(std::string(key));
	return it == values.end() ? 0 : it->second;
}

// --- rules -------------------------------------------------------------------------------

namespace
{
float display_period_ms(const sample & s)
{
	float hz = s.headset.display_hz > 0 ? s.headset.display_hz : s.headset.requested_hz;
	return hz > 0 ? 1000.f / hz : -1;
}

std::optional<warning> link_loss(std::span<const sample> recent, const thresholds & th)
{
	const sample & s = recent.back();
	// The headset's own count is authoritative when it reports one (milestone 3); until then the
	// IDR requests for lost reference frames are the only loss signal.
	int lost = s.link.frames_incomplete >= 0 ? s.link.frames_incomplete : int(s.link.idr_lost_ref);
	lost += int(s.link.idr_unsent_gap);
	if (lost < th.get("link_loss.frames_per_s"))
		return std::nullopt;
	return warning{rule_id::link_loss,
	               std::format("Video frames lost between PC and headset ({}/s). Check the headset's adapter and cable; power-cycle the headset if the PC is fine.", lost)};
}

std::optional<warning> link_slow(std::span<const sample> recent, const thresholds & th)
{
	const sample & s = recent.back();
	bool send = s.link.send_ms_p95 > th.get("link_slow.send_ms");
	bool spread = s.link.receive_spread_ms_p95 > th.get("link_slow.spread_ms");
	if (not send and not spread)
		return std::nullopt;
	return warning{rule_id::link_slow,
	               std::format("Network adapter too slow for this bitrate (send {:.1f} ms, receive spread {:.1f} ms). Lower the bitrate or use a different adapter.",
	                           s.link.send_ms_p95,
	                           s.link.receive_spread_ms_p95)};
}

std::optional<warning> encoder_busy(std::span<const sample> recent, const thresholds & th)
{
	const sample & s = recent.back();
	float period = display_period_ms(s);
	bool skipped = s.encoder.skipped > 0;
	bool util = s.encoder.util_pct >= 0 and s.encoder.util_pct >= th.get("encoder_busy.util_pct");
	bool slow = period > 0 and s.encoder.encode_ms_p95 > th.get("encoder_busy.period_ratio") * period;
	if (not skipped and not util and not slow)
		return std::nullopt;
	std::string detail = util   ? std::format("{}% busy", s.encoder.util_pct)
	                     : slow ? std::format("{:.1f} ms per frame", s.encoder.encode_ms_p95)
	                            : std::format("{} frames skipped", s.encoder.skipped);
	return warning{rule_id::encoder_busy, std::format("PC encoder overloaded ({}). Lower the resolution or refresh rate.", detail)};
}

std::optional<warning> app_slow(std::span<const sample> recent, const thresholds & th)
{
	const sample & s = recent.back();
	float display = s.headset.display_hz > 0 ? s.headset.display_hz : s.headset.requested_hz;
	if (s.app.hz < 0 or display <= 0 or s.app.hz >= th.get("app_slow.ratio") * display)
		return std::nullopt;
	return warning{rule_id::app_slow,
	               std::format("App rendering at {:.0f} of {:.0f} fps. Close other apps or browsers on the PC.", s.app.hz, display)};
}

std::optional<warning> app_late(std::span<const sample> recent, const thresholds & th)
{
	const sample & s = recent.back();
	float display = s.headset.display_hz > 0 ? s.headset.display_hz : s.headset.requested_hz;
	bool rate_ok = s.app.hz < 0 or display <= 0 or s.app.hz >= th.get("app_slow.ratio") * display;
	if (not rate_ok or s.app.late_pct <= th.get("app_late.pct"))
		return std::nullopt;
	return warning{rule_id::app_late,
	               std::format("App frames arriving late ({:.0f}%). Raise app-margin-ms in the server configuration.", s.app.late_pct)};
}

std::optional<warning> headset_decode(std::span<const sample> recent, const thresholds & th)
{
	const sample & s = recent.back();
	float period = display_period_ms(s);
	if (period <= 0 or s.headset.decode_ms_p95 <= th.get("headset_decode.period_ratio") * period)
		return std::nullopt;
	return warning{rule_id::headset_decode,
	               std::format("Headset decoder slow ({:.1f} ms per frame). Lower the resolution; power-cycle the headset if it persists.", s.headset.decode_ms_p95)};
}

std::optional<warning> headset_thermal(std::span<const sample> recent, const thresholds & th)
{
	const sample & s = recent.back();
	if (s.headset.thermal_status < 0 or s.headset.thermal_status < th.get("headset_thermal.status"))
		return std::nullopt;
	return warning{rule_id::headset_thermal, "Headset is hot and throttling. Let it cool down or lower the resolution."};
}

std::optional<warning> headset_battery(std::span<const sample> recent, const thresholds & th)
{
	const sample & s = recent.back();
	if (s.headset.battery_pct < 0 or s.headset.charging or s.headset.battery_pct >= th.get("headset_battery.pct"))
		return std::nullopt;
	return warning{rule_id::headset_battery, std::format("Headset battery low ({}%); performance is reduced.", s.headset.battery_pct)};
}

std::optional<warning> refresh_mismatch(std::span<const sample> recent, const thresholds & th)
{
	const sample & s = recent.back();
	if (s.headset.display_hz <= 0 or s.headset.requested_hz <= 0 or std::abs(s.headset.display_hz - s.headset.requested_hz) <= th.get("refresh_mismatch.hz"))
		return std::nullopt;
	return warning{rule_id::refresh_mismatch,
	               std::format("Headset running at {:.0f} Hz, not the requested {:.0f} Hz.", s.headset.display_hz, s.headset.requested_hz)};
}

std::optional<warning> judder(std::span<const sample> recent, const thresholds & th)
{
	const sample & s = recent.back();
	if (s.frames.display <= 0)
		return std::nullopt;
	float pct = 100.f * float(s.frames.repeated + s.frames.skipped) / float(s.frames.display);
	if (pct <= th.get("judder.pct"))
		return std::nullopt;
	// The class is read over the seconds the rule needs to fire, so one bad second does not name it.
	sample::frame_stats sum;
	for (size_t i = recent.size() - std::min<size_t>(recent.size(), 3); i < recent.size(); ++i)
	{
		const auto & f = recent[i].frames;
		sum.link += f.link;
		sum.app += f.app;
		sum.encode += f.encode;
		sum.decode += f.decode;
		sum.compositor += f.compositor;
		sum.tracking += f.tracking;
	}
	return warning{rule_id::judder, std::format("Judder: {:.1f} % of frames repeated or skipped, mostly {}", pct, dominant_class(sum))};
}

constexpr std::array<rule, 10> all_rules{
        link_loss,
        link_slow,
        encoder_busy,
        app_slow,
        app_late,
        headset_decode,
        headset_thermal,
        headset_battery,
        refresh_mismatch,
        judder,
};
} // namespace

std::span<const rule> rules()
{
	return all_rules;
}

// --- hysteresis --------------------------------------------------------------------------

std::vector<rule_state::event> rule_state::step(std::span<const sample> recent, const thresholds & th)
{
	std::vector<event> events;
	if (recent.empty())
		return events;
	int64_t now = recent.back().t_unix_s;
	auto all = rules();
	for (size_t i = 0; i < all.size(); ++i)
	{
		per_rule & r = state[i];
		auto w = all[i](recent, th);
		if (w)
		{
			r.hits++;
			r.misses = 0;
			if (not r.fired and r.hits >= persist and (r.last_fired_t == INT64_MIN or now - r.last_fired_t >= cooldown_s))
			{
				r.fired = true;
				r.last_fired_t = now;
				events.push_back({rule_id(i), true, w->message});
			}
		}
		else
		{
			r.hits = 0;
			r.misses++;
			if (r.fired and r.misses >= clear)
			{
				r.fired = false;
				events.push_back({rule_id(i), false, std::string(name(rule_id(i))) + " cleared"});
			}
		}
	}
	return events;
}

std::vector<rule_id> rule_state::active() const
{
	std::vector<rule_id> out;
	for (size_t i = 0; i < state.size(); ++i)
		if (state[i].fired)
			out.push_back(rule_id(i));
	return out;
}

// --- JSON line ---------------------------------------------------------------------------

std::string to_json_line(const sample & s, std::span<const rule_id> active)
{
	nlohmann::json j;
	j["t"] = s.t_unix_s;
	j["link"] = {
	        {"shards_sent", s.link.shards_sent},
	        {"bytes_sent", s.link.bytes_sent},
	        {"send_ms_p95", s.link.send_ms_p95},
	        {"receive_spread_ms_p95", s.link.receive_spread_ms_p95},
	        {"frames_incomplete", s.link.frames_incomplete},
	        {"idr_lost_ref", s.link.idr_lost_ref},
	        {"idr_unsent_gap", s.link.idr_unsent_gap},
	};
	j["headset"] = {
	        {"decode_ms_p95", s.headset.decode_ms_p95},
	        {"display_jitter_ms_p95", s.headset.display_jitter_ms_p95},
	        {"frames_repeated", s.headset.frames_repeated},
	        {"display_hz", s.headset.display_hz},
	        {"requested_hz", s.headset.requested_hz},
	        {"thermal_status", s.headset.thermal_status},
	        {"battery_pct", s.headset.battery_pct},
	        {"charging", s.headset.charging},
	        {"gpu_ms", s.headset.gpu_ms},
	        {"cpu_ms", s.headset.cpu_ms},
	        {"compositor_dropped", s.headset.compositor_dropped},
	        {"compositor_stale", s.headset.compositor_stale},
	        {"compositor_gpu_ms", s.headset.compositor_gpu_ms},
	        {"app_gpu_ms", s.headset.app_gpu_ms},
	        {"runtime_m2p_ms", s.headset.runtime_m2p_ms},
	        {"cpu_util_pct", s.headset.cpu_util_pct},
	        {"gpu_util_pct", s.headset.gpu_util_pct},
	};
	j["encoder"] = {
	        {"presented", s.encoder.presented},
	        {"skipped", s.encoder.skipped},
	        {"encode_ms_p95", s.encoder.encode_ms_p95},
	        {"util_pct", s.encoder.util_pct},
	};
	j["app"] = {
	        {"hz", s.app.hz},
	        {"cpu_ms", s.app.cpu_ms},
	        {"gpu_ms", s.app.gpu_ms},
	        {"late_pct", s.app.late_pct},
	        {"half_rate_probes", s.app.half_rate_probes},
	        {"half_rate_recoveries", s.app.half_rate_recoveries},
	};
	j["motion_to_photon_ms_p95"] = s.motion_to_photon_ms_p95;
	j["frames"] = {
	        {"display", s.frames.display},
	        {"repeated", s.frames.repeated},
	        {"skipped", s.frames.skipped},
	        {"link", s.frames.link},
	        {"app", s.frames.app},
	        {"encode", s.frames.encode},
	        {"decode", s.frames.decode},
	        {"compositor", s.frames.compositor},
	        {"tracking", s.frames.tracking},
	        {"unattributed", s.frames.unattributed},
	};
	auto names = nlohmann::json::array();
	for (auto id: active)
		names.push_back(name(id));
	j["warnings"] = names;
	if (not s.app_metrics.empty())
		j["app_metrics"] = s.app_metrics;
	if (s.app_metrics_dropped)
		j["app_metrics_dropped"] = s.app_metrics_dropped;
	if (not s.app_warnings.empty())
		j["app_warnings"] = s.app_warnings;
	return j.dump();
}

std::optional<sample> from_json_line(std::string_view line)
{
	nlohmann::json j = nlohmann::json::parse(line, nullptr, false);
	if (j.is_discarded() or not j.is_object())
		return std::nullopt;
	sample s;
	auto get = [](const nlohmann::json & o, const char * key, auto & out) {
		if (auto it = o.find(key); it != o.end() and not it->is_null())
			out = it->get<std::remove_reference_t<decltype(out)>>();
	};
	get(j, "t", s.t_unix_s);
	if (auto it = j.find("link"); it != j.end() and it->is_object())
	{
		get(*it, "shards_sent", s.link.shards_sent);
		get(*it, "bytes_sent", s.link.bytes_sent);
		get(*it, "send_ms_p95", s.link.send_ms_p95);
		get(*it, "receive_spread_ms_p95", s.link.receive_spread_ms_p95);
		get(*it, "frames_incomplete", s.link.frames_incomplete);
		get(*it, "idr_lost_ref", s.link.idr_lost_ref);
		get(*it, "idr_unsent_gap", s.link.idr_unsent_gap);
	}
	if (auto it = j.find("headset"); it != j.end() and it->is_object())
	{
		get(*it, "decode_ms_p95", s.headset.decode_ms_p95);
		get(*it, "display_jitter_ms_p95", s.headset.display_jitter_ms_p95);
		get(*it, "frames_repeated", s.headset.frames_repeated);
		get(*it, "display_hz", s.headset.display_hz);
		get(*it, "requested_hz", s.headset.requested_hz);
		get(*it, "thermal_status", s.headset.thermal_status);
		get(*it, "battery_pct", s.headset.battery_pct);
		get(*it, "charging", s.headset.charging);
		get(*it, "gpu_ms", s.headset.gpu_ms);
		get(*it, "cpu_ms", s.headset.cpu_ms);
		get(*it, "compositor_dropped", s.headset.compositor_dropped);
		get(*it, "compositor_stale", s.headset.compositor_stale);
		get(*it, "compositor_gpu_ms", s.headset.compositor_gpu_ms);
		get(*it, "app_gpu_ms", s.headset.app_gpu_ms);
		get(*it, "runtime_m2p_ms", s.headset.runtime_m2p_ms);
		get(*it, "cpu_util_pct", s.headset.cpu_util_pct);
		get(*it, "gpu_util_pct", s.headset.gpu_util_pct);
	}
	if (auto it = j.find("encoder"); it != j.end() and it->is_object())
	{
		get(*it, "presented", s.encoder.presented);
		get(*it, "skipped", s.encoder.skipped);
		get(*it, "encode_ms_p95", s.encoder.encode_ms_p95);
		get(*it, "util_pct", s.encoder.util_pct);
	}
	if (auto it = j.find("app"); it != j.end() and it->is_object())
	{
		get(*it, "hz", s.app.hz);
		get(*it, "cpu_ms", s.app.cpu_ms);
		get(*it, "gpu_ms", s.app.gpu_ms);
		get(*it, "late_pct", s.app.late_pct);
		get(*it, "half_rate_probes", s.app.half_rate_probes);
		get(*it, "half_rate_recoveries", s.app.half_rate_recoveries);
	}
	get(j, "motion_to_photon_ms_p95", s.motion_to_photon_ms_p95);
	if (auto it = j.find("frames"); it != j.end() and it->is_object())
	{
		get(*it, "display", s.frames.display);
		get(*it, "repeated", s.frames.repeated);
		get(*it, "skipped", s.frames.skipped);
		get(*it, "link", s.frames.link);
		get(*it, "app", s.frames.app);
		get(*it, "encode", s.frames.encode);
		get(*it, "decode", s.frames.decode);
		get(*it, "compositor", s.frames.compositor);
		get(*it, "tracking", s.frames.tracking);
		get(*it, "unattributed", s.frames.unattributed);
	}
	if (auto it = j.find("app_metrics"); it != j.end() and it->is_object())
		for (auto & [k, v]: it->items())
			if (v.is_number())
				s.app_metrics[k] = v.get<double>();
	get(j, "app_metrics_dropped", s.app_metrics_dropped);
	if (auto it = j.find("app_warnings"); it != j.end() and it->is_array())
		for (const auto & v: *it)
			if (v.is_string())
				s.app_warnings.push_back(v.get<std::string>());
	return s;
}

// --- judder (GRID #3095 M6c) -------------------------------------------------------------

namespace
{
// Stage durations of a frame row in nanoseconds, -1 when either end is unknown, in the order
// app (wake→submit), gpu, compositor, encoder, transit (last shard sent→received), spread
// (first→last packet received), decode.
constexpr size_t stage_count = 7;
using stage_ns = std::array<int64_t, stage_count>;
constexpr std::array<uint32_t sample::frame_stats::*, stage_count> stage_class{
        &sample::frame_stats::app,
        &sample::frame_stats::app,
        &sample::frame_stats::encode,
        &sample::frame_stats::encode,
        &sample::frame_stats::link,
        &sample::frame_stats::link,
        &sample::frame_stats::decode,
};

int64_t span_ns(int64_t from, int64_t to)
{
	return (from and to and to >= from) ? to - from : -1;
}

stage_ns stages(const frame_row & r)
{
	return {span_ns(r.wake, r.submit), span_ns(r.submit, r.gpu), span_ns(r.comp0, r.comp1), span_ns(r.enc0, r.enc1), span_ns(r.send1, r.rx1), span_ns(r.rx0, r.rx1), span_ns(r.dec0, r.dec1)};
}

int64_t median(std::vector<int64_t> & v)
{
	if (v.empty())
		return -1;
	auto mid = v.begin() + v.size() / 2;
	std::nth_element(v.begin(), mid, v.end());
	return *mid;
}

// One application frame across its streams: the headset shows it when every stream arrived, so
// the latest blit/display and the largest stage duration stand for the frame.
struct frame
{
	uint64_t n;
	int32_t shown; // -1: no feedback
	int64_t pd, blit, disp, pose_t;
	int32_t pose_flags;
	stage_ns st;
};
} // namespace

sample::frame_stats judder_stats(std::span<const frame_row> rows, int64_t period_ns)
{
	sample::frame_stats out;
	if (rows.empty() or period_ns <= 0)
		return out;
	std::vector<frame> frames;
	for (const auto & r: rows)
	{
		if (frames.empty() or frames.back().n != r.n)
		{
			frames.push_back({r.n, r.shown, r.pd, r.blit, r.disp, r.pose_t, r.pose_flags, stages(r)});
			continue;
		}
		frame & f = frames.back();
		f.shown = std::max(f.shown, r.shown);
		f.blit = std::max(f.blit, r.blit);
		f.disp = std::max(f.disp, r.disp);
		stage_ns st = stages(r);
		for (size_t i = 0; i < stage_count; ++i)
			f.st[i] = std::max(f.st[i], st[i]);
	}
	auto hitch = [period_ns](const frame & f) {
		return f.shown != 1 or (f.disp and f.pd and f.disp - f.pd > period_ns / 2);
	};
	// Baselines: the window's own on-time new frames (not a rolling history, see the header).
	std::array<std::vector<int64_t>, stage_count> seen;
	for (const auto & f: frames)
		if (not hitch(f))
			for (size_t i = 0; i < stage_count; ++i)
				if (f.st[i] >= 0)
					seen[i].push_back(f.st[i]);
	stage_ns med;
	for (size_t i = 0; i < stage_count; ++i)
		med[i] = median(seen[i]);

	// The stage a hitch frame used the budget on, as a frame_stats member.
	auto classify = [&](const frame & f) -> uint32_t sample::frame_stats::* {
		constexpr int32_t tracked = 0b1100; // orientation and position tracked
		if ((f.pose_flags >= 0 and (f.pose_flags & tracked) != tracked) or (f.pose_t and f.disp and f.disp - f.pose_t > 2 * period_ns))
			return &sample::frame_stats::tracking;
		if (f.shown < 0)
			return &sample::frame_stats::link; // nothing came back: the shards never reached the headset
		int best = -1;
		int64_t excess = 0;
		for (size_t i = 0; i < stage_count; ++i)
			if (f.st[i] >= 0 and med[i] >= 0 and f.st[i] - med[i] > excess)
			{
				excess = f.st[i] - med[i];
				best = int(i);
			}
		if (best >= 0 and excess > period_ns / 2)
			return stage_class[size_t(best)];
		if (f.blit and f.pd and f.blit <= f.pd)
			return &sample::frame_stats::compositor;
		return &sample::frame_stats::unattributed;
	};

	out.display = 0;
	for (size_t i = 0; i < frames.size(); ++i)
	{
		const frame & f = frames[i];
		if (f.shown > 0)
			out.display += f.shown;
		if (f.shown <= 0)
			out.skipped++;
		else if (f.shown > 1)
			out.repeated += uint32_t(f.shown - 1);
		if (not hitch(f))
			continue;
		// A repeated frame filled the slot of the next one (the late one), so that frame is the
		// cause, as in the GRID reader; the next frame counts again for its own lateness. With the
		// successor outside the window the repeat stays unattributed rather than guessed.
		if (f.shown > 1 and not(f.disp and f.pd and f.disp - f.pd > period_ns / 2))
		{
			if (i + 1 >= frames.size())
				out.unattributed++;
			else
				out.*classify(hitch(frames[i + 1]) ? frames[i + 1] : f) += 1;
			continue;
		}
		out.*classify(f) += 1;
	}
	return out;
}

std::string_view dominant_class(const sample::frame_stats & f)
{
	std::pair<uint32_t, std::string_view> best{0, "unattributed"};
	for (auto [count, name]: {std::pair<uint32_t, std::string_view>{f.link, "link"}, {f.app, "app"}, {f.encode, "encode"}, {f.decode, "decode"}, {f.compositor, "headset-compositor"}, {f.tracking, "tracking"}})
		if (count > best.first)
			best = {count, name};
	return best.second;
}

// --- telemetry connector -----------------------------------------------------------------

namespace
{
bool valid_identifier(std::string_view s)
{
	if (s.empty() or s.size() > 32)
		return false;
	for (char c: s)
		if (not((c >= 'a' and c <= 'z') or (c >= '0' and c <= '9') or c == '_'))
			return false;
	return true;
}

std::string string_or_empty(const nlohmann::json & o, const char * key)
{
	auto it = o.find(key);
	return it != o.end() and it->is_string() ? it->get<std::string>() : std::string{};
}
} // namespace

std::optional<connector_line> parse_connector_line(std::string_view line)
{
	if (line.size() > max_connector_line)
		return std::nullopt;
	nlohmann::json j = nlohmann::json::parse(line, nullptr, false);
	if (j.is_discarded() or not j.is_object())
		return std::nullopt;
	auto type = string_or_empty(j, "type");
	if (type == "metric")
	{
		connector_line out{.kind = connector_line::kind::metric, .name = string_or_empty(j, "name")};
		auto value = j.find("value");
		if (not valid_identifier(out.name) or value == j.end() or not value->is_number())
			return std::nullopt;
		out.value = value->get<double>();
		if (not std::isfinite(out.value))
			return std::nullopt;
		return out;
	}
	if (type == "warning")
	{
		connector_line out{.kind = connector_line::kind::warning, .name = string_or_empty(j, "id"), .message = string_or_empty(j, "message")};
		if (not valid_identifier(out.name) or out.message.empty() or out.message.size() > 200)
			return std::nullopt;
		if (auto ttl = j.find("ttl_s"); ttl != j.end())
		{
			if (not ttl->is_number())
				return std::nullopt;
			out.ttl_s = int(std::clamp(ttl->get<double>(), 1.0, 600.0));
		}
		return out;
	}
	return std::nullopt;
}

bool app_warning_state::fire(const std::string & id, int ttl_s, int64_t t)
{
	auto it = entries.find(id);
	if (it != entries.end() and t < it->second.refire_t)
		return false;
	if (it == entries.end() and entries.size() >= max_entries)
		return false;
	entries[id] = {.until_t = t + ttl_s, .refire_t = t + std::max(ttl_s, cooldown_s), .listed = true};
	return true;
}

std::vector<std::string> app_warning_state::expire(int64_t t)
{
	std::vector<std::string> expired;
	for (auto it = entries.begin(); it != entries.end();)
	{
		auto & e = it->second;
		if (e.listed and t >= e.until_t)
		{
			e.listed = false;
			expired.push_back(it->first);
		}
		if (not e.listed and t >= e.refire_t)
			it = entries.erase(it);
		else
			++it;
	}
	return expired;
}

std::vector<std::string> app_warning_state::active() const
{
	std::vector<std::string> ids;
	for (const auto & [id, e]: entries)
		if (e.listed)
			ids.push_back(id);
	return ids;
}

} // namespace wivrn::health
