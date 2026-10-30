/*
 * WiVRn stream health monitor (GRID #2834): aggregates what both ends already measure into one
 * sample per second, runs the rules, warns the headset, and keeps a flight record.
 * Design: https://app.notion.app/p/409f908c414681fdbb5fe32145044300
 *
 * Hot-path cost: on_feedback does a handful of subtractions, histogram increments and a ring
 * write under a short mutex; every other input is an existing atomic counter read once per
 * second by the monitor thread.
 */
#pragma once

#include "configuration.h"
#include "health_rules.h"
#include "wivrn_packets.h"

#include <array>
#include <filesystem>
#include <fstream>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace wivrn
{

class health_monitor
{
public:
	/// Sends a message to the headset; `urgent` selects server_message::kind::toast_urgent.
	using send_fn = std::function<void(const std::string & message, bool urgent)>;
	/// Called on the monitor thread each second to fill the fields only the session knows
	/// (battery, requested and active refresh rate).
	using fill_fn = std::function<void(health::sample &)>;

	health_monitor(configuration::health_options options, std::filesystem::path default_log_dir, send_fn send, fill_fn fill);
	~health_monitor();

	health_monitor(const health_monitor &) = delete;
	health_monitor & operator=(const health_monitor &) = delete;

	/// Where the flight record goes when `health.log-dir` is unset: `%LOCALAPPDATA%\wivrn\logs`
	/// on Windows (the server log directory), `$XDG_STATE_HOME/wivrn` or `~/.local/state/wivrn`
	/// elsewhere.
	static std::filesystem::path default_log_dir();

	/// Per-frame feedback from the headset, timestamps in the headset clock.
	void on_feedback(const from_headset::feedback &);

	/// The headset's display period, for display jitter; set whenever the refresh rate changes.
	void set_display_hz(float hz);

	/// Session lifecycle from the connection: "session_paused" (headset gone, rules suspended)
	/// and "session_resumed". Recorded as event lines in the flight record.
	void on_session_event(std::string_view event);

	/// A line received on the telemetry connector (milestone 4): warnings and metrics from
	/// applications. Invalid lines are counted and ignored.
	void on_connector_line(std::string_view line);

	/// The most recent sample, for the connector and the stats tab.
	health::sample last_sample() const;

	/// Called with every JSON line the monitor produces (samples and warning events), for the
	/// telemetry connector. Set before the first tick.
	std::function<void(const std::string &)> on_line;

private:
	void run(std::stop_token);
	health::sample collect(int64_t now_unix_s);
	void write_event_dump(const health::rule_state::event &, const health::sample &);
	std::filesystem::path log_path(const char * prefix, const char * extension) const;

	configuration::health_options options;
	std::filesystem::path log_dir;
	send_fn send;
	fill_fn fill;
	health::thresholds thresholds;

	// Written by on_feedback (net thread), drained by the monitor thread.
	struct feedback_record
	{
		uint64_t frame_index;
		uint8_t stream;
		int64_t encode_begin, encode_end, send_begin, send_end;
		int64_t received_first, received_last, sent_to_decoder, received_from_decoder, blitted, displayed;
		uint8_t times_displayed;
	};
	static constexpr size_t ring_size = 600;
	struct feedback_state
	{
		health::log_histogram encode, send, spread, decode, display_jitter, motion_to_photon;
		uint32_t incomplete = 0;
		uint32_t repeated = 0;
		std::atomic<int64_t> period_ns{0};
		std::array<int64_t, 3> last_displayed{};
		std::array<std::array<feedback_record, ring_size>, 3> ring{};
		std::array<size_t, 3> ring_next{};
		std::array<size_t, 3> ring_count{};
	};
	mutable std::mutex mutex;
	feedback_state fb;
	health::sample last;
	std::vector<health::sample> history; // bounded, newest last
	health::rule_state state;

	std::ofstream jsonl;
	int64_t started_unix_s = 0;
	uint32_t invalid_connector_lines = 0;
	std::map<std::string, double> connector_metrics;
	std::atomic<bool> paused{false};
	void write_line(const std::string &);

	std::jthread thread;
};

} // namespace wivrn
