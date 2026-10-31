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
constexpr std::array<std::string_view, 9> rule_names{
        "link_loss",
        "link_slow",
        "encoder_busy",
        "app_slow",
        "app_late",
        "headset_decode",
        "headset_thermal",
        "headset_battery",
        "refresh_mismatch",
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
	                           s.link.send_ms_p95, s.link.receive_spread_ms_p95)};
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
	std::string detail = util ? std::format("{}% busy", s.encoder.util_pct)
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

constexpr std::array<rule, 9> all_rules{
        link_loss,
        link_slow,
        encoder_busy,
        app_slow,
        app_late,
        headset_decode,
        headset_thermal,
        headset_battery,
        refresh_mismatch,
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
	};
	j["motion_to_photon_ms_p95"] = s.motion_to_photon_ms_p95;
	auto names = nlohmann::json::array();
	for (auto id: active)
		names.push_back(name(id));
	j["warnings"] = names;
	if (not s.app_metrics.empty())
		j["app_metrics"] = s.app_metrics;
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
	}
	get(j, "motion_to_photon_ms_p95", s.motion_to_photon_ms_p95);
	if (auto it = j.find("app_metrics"); it != j.end() and it->is_object())
		for (auto & [k, v]: it->items())
			if (v.is_number())
				s.app_metrics[k] = v.get<double>();
	return s;
}

} // namespace wivrn::health
