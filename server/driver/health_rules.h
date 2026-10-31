/*
 * WiVRn stream health: per-second samples, rules and warnings.
 * Design: https://app.notion.com/p/409f908c414681fdbb5fe32145044300 (GRID #2834).
 *
 * This header is the contract between the health monitor (server/driver/health_monitor.cpp,
 * milestone 2) and its tests (server/tests/health_rules.cpp). Everything here is plain data
 * and pure functions: no threads, no clocks, no I/O, so the tests replay recorded sessions.
 */
#pragma once

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
	} app;

	float motion_to_photon_ms_p95 = -1;

	/// Metrics posted by applications on the telemetry connector (milestone 4).
	std::map<std::string, double> app_metrics;
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
	std::array<per_rule, 9> state{};
};

/// One JSON line per sample, the flight-recorder format documented in docs/health.md.
std::string to_json_line(const sample &, std::span<const rule_id> active);
/// Parse a line written by to_json_line (used by tests and the connector).
std::optional<sample> from_json_line(std::string_view);

} // namespace wivrn::health
