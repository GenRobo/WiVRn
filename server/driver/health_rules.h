/*
 * WiVRn stream health: per-second samples, rules and warnings.
 * Design: https://app.notion.com/p/409f908c414681fdbb5fe32145044300 (GRID #2834).
 *
 * This header is the contract between the health monitor (server/driver/health_monitor.cpp,
 * milestone 2) and its tests (server/tests/health_rules.cpp). Everything here is plain data
 * and pure functions: no threads, no clocks, no I/O, so the tests replay recorded sessions.
 */
#pragma once

#include "health_frames.h"

#include <array>
#include <cstdint>
#include <map>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace wivrn::health
{

/// Fixed 32-bucket logarithmic histogram of durations in milliseconds, 0.1 ms to ~200 ms.
/// One array increment per observation; percentiles are read on the monitor thread.
struct log_histogram
{
	static constexpr size_t buckets = 32;
	std::array<uint32_t, buckets> counts{};

	/// Bucket for a duration: 0 holds [0, 0.1 ms), each next bucket is 1.25x wider.
	static size_t bucket(double ms);
	void observe(double ms);
	/// Lower bound of the bucket holding the requested percentile (0..1), or 0 when empty.
	double percentile(double p) const;
	uint64_t total() const;
	void clear();
};

/// One second of the stream, as the monitor sees both ends. Absent values are negative
/// (counts) or NaN-free sentinels (-1) so the JSON line stays compact and self-describing.
struct sample
{
	int64_t t_unix_s = 0;

	struct
	{
		uint32_t shards_sent = 0;
		uint64_t bytes_sent = 0;
		float send_ms_p95 = -1;
		float receive_spread_ms_p95 = -1;
		int32_t frames_incomplete = -1; // from headset telemetry (milestone 2), -1 until then
		uint32_t idr_lost_ref = 0;      // headset reported a reference frame it never decoded
		uint32_t idr_unsent_gap = 0;    // headset asked for a frame the compositor never encoded
	} link;

	struct
	{
		float decode_ms_p95 = -1;
		float display_jitter_ms_p95 = -1;
		uint32_t frames_repeated = 0;
		float display_hz = -1;
		float requested_hz = -1;
		int32_t thermal_status = -1; // Android thermal status 0..6, -1 unknown
		int32_t battery_pct = -1;
		bool charging = false;
		float gpu_ms = -1;
		float cpu_ms = -1;
		// Runtime counters (GRID #3095 M6c, XR_META_performance_metrics); -1 unknown.
		int32_t compositor_dropped = -1;
		int32_t compositor_stale = -1;
		float compositor_gpu_ms = -1;
		float app_gpu_ms = -1;
		float runtime_m2p_ms = -1;
		int32_t cpu_util_pct = -1;
		int32_t gpu_util_pct = -1;
	} headset;

	struct
	{
		uint32_t presented = 0;
		uint32_t skipped = 0;
		float encode_ms_p95 = -1;
		int32_t util_pct = -1; // NVML, -1 when unavailable
	} encoder;

	struct
	{
		float hz = -1;
		float cpu_ms = -1;
		float gpu_ms = -1;
		float late_pct = -1;
		uint32_t half_rate_probes = 0;     // one-period probes the app pacer issued (GRID #2834 lock-in)
		uint32_t half_rate_recoveries = 0; // probes the application answered in time
	} app;

	float motion_to_photon_ms_p95 = -1;

	/// Display frames settled this second, from the frame ring (GRID #3095 M6c, judder_stats);
	/// `display` is -1 while no row settled. The per-class counts are the hitches (repeats,
	/// skips and frames displayed more than half a period late) by the stage that used the budget.
	struct frame_stats
	{
		int32_t display = -1;
		uint32_t repeated = 0;
		uint32_t skipped = 0;
		uint32_t link = 0;
		uint32_t app = 0;
		uint32_t encode = 0;
		uint32_t decode = 0;
		uint32_t compositor = 0; // headset compositor: delivered in time, still repeated or late
		uint32_t tracking = 0;
		uint32_t unattributed = 0;
	} frames;

	/// Metrics posted by applications on the telemetry connector (milestone 4): the last value
	/// posted for each name during this second.
	std::map<std::string, double> app_metrics;
	/// Connector lines dropped this second: oversize, not JSON, unknown type, bad id or value.
	uint32_t app_metrics_dropped = 0;
	/// Application warnings (connector) still within their ttl.
	std::vector<std::string> app_warnings;
};

enum class rule_id : uint8_t
{
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

std::string_view name(rule_id);
std::optional<rule_id> rule_from_name(std::string_view);

/// Thresholds keyed "<rule>.<param>", with defaults from the 2026-10-29/30 measurements.
/// Unknown keys are ignored so a config file can carry keys for newer builds.
struct thresholds
{
	std::map<std::string, float> values;

	static thresholds defaults();
	/// Overlay `overrides` on the defaults.
	static thresholds with(const std::map<std::string, float> & overrides);
	float get(std::string_view key) const;
};

struct warning
{
	rule_id id;
	std::string message;
};

/// A rule evaluates the most recent samples (newest last) and returns a warning
/// while its condition holds for the current sample. Rules never look at state.
using rule = std::optional<warning> (*)(std::span<const sample> recent, const thresholds &);

/// Every rule, in rule_id order.
std::span<const rule> rules();

/// Hysteresis and cooldown over the raw rule output: a condition must hold for
/// `persist` consecutive samples to fire, clears after `clear` consecutive samples
/// without it, and cannot re-fire within `cooldown_s` of its last firing.
struct rule_state
{
	int persist = 3;
	int clear = 5;
	int cooldown_s = 30;

	struct event
	{
		rule_id id;
		bool fired; // false = cleared
		std::string message;
	};

	/// Feed one sample (one second). Returns the events that changed state.
	std::vector<event> step(std::span<const sample> recent, const thresholds &);
	/// Rules currently in the fired state.
	std::vector<rule_id> active() const;

private:
	struct per_rule
	{
		int hits = 0;
		int misses = 0;
		bool fired = false;
		int64_t last_fired_t = INT64_MIN;
	};
	std::array<per_rule, 10> state{};
};

/// One JSON line per sample, the flight-recorder format documented in docs/health.md.
std::string to_json_line(const sample &, std::span<const rule_id> active);
/// Parse a line written by to_json_line (used by tests and the connector).
std::optional<sample> from_json_line(std::string_view);

/// The judder rule's input: one second of settled frame rows, every stream, sorted by (n, s), with
/// the display period in effect. Kind per frame: `shown` 0 (or no feedback) is a skip, `shown`
/// above 1 is `shown - 1` repeats, and a new frame displayed more than half a period after `pd`
/// is late; each is attributed like `vivarium-teleop xr trace` (design 6.6) with these
/// simplifications: tracking (pose not tracked or older than two periods); the stage with the
/// largest excess over the median of the window's own on-time frames when above half a period
/// (app = wake→submit and gpu, encode = compositor and encoder, link = transit and spread,
/// decode); a frame without feedback is link; headset compositor when the frame was blitted
/// before `pd` and still repeated, skipped or late; else unattributed. No session, half-rate or
/// content-cadence classes, and the medians are the window's, not a rolling 300-frame history.
sample::frame_stats judder_stats(std::span<const frame_row> rows, int64_t period_ns);
/// The dominant class name of a stats sum: the largest count among the attributed classes,
/// "unattributed" when none.
std::string_view dominant_class(const sample::frame_stats &);

// --- telemetry connector (milestone 4) ---------------------------------------------------

/// Longest inbound connector line accepted, in bytes, newline excluded.
constexpr size_t max_connector_line = 4096;

/// A validated line posted by an application on the connector.
struct connector_line
{
	enum class kind
	{
		metric,
		warning,
	} kind;
	std::string name;    // metric name or warning id, [a-z0-9_]{1,32}
	double value = 0;    // metric
	std::string message; // warning, 1..200 characters
	int ttl_s = 10;      // warning, 1..600
};

/// Parse and validate one inbound line; nullopt for anything that must be dropped.
std::optional<connector_line> parse_connector_line(std::string_view);

/// Splits a byte stream into lines for parse_connector_line. A line longer than
/// max_connector_line is delivered truncated to max_connector_line + 1 bytes (so the
/// receiver can count it as oversize) and the rest of it is skipped.
struct connector_line_buffer
{
	std::string pending;
	bool skipping = false;

	template <typename F>
	void feed(std::string_view data, F && on_line)
	{
		while (not data.empty())
		{
			size_t nl = data.find('\n');
			std::string_view chunk = data.substr(0, nl);
			data.remove_prefix(nl == std::string_view::npos ? data.size() : nl + 1);
			if (skipping)
			{
				skipping = nl == std::string_view::npos;
				continue;
			}
			if (pending.size() + chunk.size() > max_connector_line)
			{
				pending.append(chunk.substr(0, max_connector_line + 1 - pending.size()));
				skipping = nl == std::string_view::npos;
				on_line(std::string_view(pending));
				pending.clear();
				continue;
			}
			if (nl == std::string_view::npos)
			{
				pending.append(chunk);
				return;
			}
			if (pending.empty())
				on_line(chunk);
			else
			{
				pending.append(chunk);
				on_line(std::string_view(pending));
				pending.clear();
			}
		}
	}
};

/// Application warnings follow the rules' cooldown: an id posted again within
/// max(ttl_s, cooldown_s) of its last firing is not sent again; it stays listed in the sample
/// until its ttl elapses. At most max_entries distinct ids are tracked at once; more are dropped.
struct app_warning_state
{
	int cooldown_s = 30;
	static constexpr size_t max_entries = 256;

	/// True when the warning should be sent to the headset now.
	bool fire(const std::string & id, int ttl_s, int64_t t);
	/// Ids whose ttl elapsed at `t`, removed from the active list.
	std::vector<std::string> expire(int64_t t);
	std::vector<std::string> active() const;

private:
	struct entry
	{
		int64_t until_t;  // listed until
		int64_t refire_t; // may fire again from
		bool listed;
	};
	std::map<std::string, entry> entries;
};

} // namespace wivrn::health
