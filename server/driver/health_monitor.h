/*
 * WiVRn stream health monitor (GRID #2834): aggregates what both ends already measure into one
 * sample per second, runs the rules, warns the headset, and keeps a flight record. With
 * `health.frames` (GRID #3095) it also serialises the per-frame ring to wivrn-frames-*.jsonl.
 * Design: https://app.notion.app/p/409f908c414681fdbb5fe32145044300
 *
 * Hot-path cost: on_feedback does a handful of subtractions, histogram increments and a ring
 * write under a short mutex, plus relaxed stores into the frame ring; every other input is an
 * existing atomic counter read once per second by the monitor thread.
 */
#pragma once

#include "clock_offset.h"
#include "configuration.h"
#include "health_frames.h"
#include "health_rules.h"
#include "wivrn_packets.h"

#include <array>
#include <filesystem>
#include <fstream>
#include <functional>
#include <memory>
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

	/// `video_streams` is the number of encoded streams (two eyes, three with alpha), for the
	/// frame record's header.
	health_monitor(configuration::health_options options, std::filesystem::path default_log_dir, uint8_t video_streams, send_fn send, fill_fn fill);
	~health_monitor();

	health_monitor(const health_monitor &) = delete;
	health_monitor & operator=(const health_monitor &) = delete;

	/// Where the flight record goes when `health.log-dir` is unset: `%LOCALAPPDATA%\wivrn\logs`
	/// on Windows (the server log directory), `$XDG_STATE_HOME/wivrn` or `~/.local/state/wivrn`
	/// elsewhere.
	static std::filesystem::path default_log_dir();

	/// Per-frame feedback from the headset, timestamps in the headset clock; `offset` converts
	/// them for the frame record.
	void on_feedback(const from_headset::feedback &, const clock_offset & offset);

	/// The headset's display period, for display jitter; set whenever the refresh rate changes.
	void set_display_hz(float hz);

	/// Session lifecycle from the connection: "session_paused" (headset gone, rules suspended)
	/// and "session_resumed". Recorded as event lines in the flight record and the frame record.
	void on_session_event(std::string_view event);

	/// Session events for the frame record (GRID #3095): `session_state`, `bitrate`,
	/// `recenter`, `interaction_profile`, `user_presence`, `display_hz`; any thread.
	void on_event(std::string_view name, nlohmann::json value);

	/// The headset's once-per-second telemetry (milestone 3): fills the headset-side fields.
	void on_telemetry(const from_headset::telemetry &);

	/// Sends the last sample to the headset for its statistics tab; set by the session.
	std::function<void(const to_headset::health &)> send_health;

	/// The app-data channel the telemetry connector uses (milestone 4).
	static constexpr std::string_view connector_channel = "wivrn-health";

	/// Bytes received from the connector sink, on the relay thread: split into lines, each a
	/// warning or a metric from an application (health::parse_connector_line). Invalid lines
	/// are counted in the sample (`app_metrics_dropped`) and dropped.
	void on_connector_data(std::string_view data);

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

	struct feedback_state
	{
		health::log_histogram encode, send, spread, decode, display_jitter, motion_to_photon;
		uint32_t incomplete = 0;
		uint32_t repeated = 0;
		std::atomic<int64_t> period_ns{0};
		std::array<int64_t, 3> last_displayed{};
		// Clock offset seen by on_feedback this second: last, lowest, highest.
		int64_t off_ns = 0, off_min_ns = INT64_MAX, off_max_ns = INT64_MIN;
	};
	mutable std::mutex mutex;
	feedback_state fb;
	health::sample last;
	std::vector<health::sample> history; // bounded, newest last
	health::rule_state state;

	std::ofstream jsonl;
	int64_t started_unix_s = 0;
	// Connector state: written on the relay thread, read by collect() under mutex.
	health::connector_line_buffer connector_buffer; // relay thread only
	uint32_t connector_dropped = 0;
	std::map<std::string, double> connector_metrics;
	health::app_warning_state app_warnings;
	void on_connector_line(std::string_view line);
	std::atomic<bool> paused{false};
	std::optional<from_headset::telemetry> last_telemetry; // under mutex
	bool telemetry_fresh = false;                          // set by on_telemetry, cleared by collect
	void write_line(const std::string &);

	// Frame record (GRID #3095): the writer runs on the monitor thread, events come from the
	// session's threads; both under frames_mutex, never on a frame path.
	std::mutex frames_mutex;
	std::unique_ptr<health::frame_writer> frames_writer;
	int64_t frames_t0_ns = 0;
	uint8_t video_streams;
	void frames_tick(const health::sample &);

	std::jthread thread;
};

} // namespace wivrn
