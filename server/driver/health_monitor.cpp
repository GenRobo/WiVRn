/*
 * WiVRn stream health monitor (GRID #2834). See health_monitor.h.
 */
#include "health_monitor.h"

#include "app_pacer.h"
#include "encoder/idr_handler.h"
#include "encoder/video_encoder.h"
#include "hostname.h"
#include "os/os_time.h"
#include "util/u_logging.h"
#include "version.h"

#include <chrono>
#include <cmath>
#include <ctime>
#include <format>
#include <nlohmann/json.hpp>

#ifdef _WIN32
#include <windows.h>
#else
#include <dlfcn.h>
#endif

namespace wivrn
{

namespace
{
constexpr size_t history_size = 60;
constexpr double ns_to_ms = 1e-6;

int64_t unix_now_s()
{
	return std::chrono::duration_cast<std::chrono::seconds>(std::chrono::system_clock::now().time_since_epoch()).count();
}

std::string timestamp_for_filename()
{
	std::time_t t = std::time(nullptr);
	std::tm tm{};
#ifdef _WIN32
	localtime_s(&tm, &t);
#else
	localtime_r(&t, &tm);
#endif
	char buf[32];
	std::strftime(buf, sizeof(buf), "%Y%m%d-%H%M%S", &tm);
	return buf;
}

#if WIVRN_USE_NVML
// NVML is loaded at runtime so builds and machines without the NVIDIA driver are unaffected.
class nvml_encoder_utilization
{
	void * lib = nullptr;
	void * device = nullptr;
	int (*get_utilization)(void *, unsigned int *, unsigned int *) = nullptr;
	void (*shutdown)() = nullptr;

	template <typename T>
	T symbol(const char * name)
	{
#ifdef _WIN32
		return reinterpret_cast<T>(GetProcAddress(static_cast<HMODULE>(lib), name));
#else
		return reinterpret_cast<T>(dlsym(lib, name));
#endif
	}

public:
	nvml_encoder_utilization()
	{
#ifdef _WIN32
		lib = LoadLibraryA("nvml.dll");
#else
		lib = dlopen("libnvidia-ml.so.1", RTLD_NOW);
#endif
		if (not lib)
			return;
		auto init = symbol<int (*)()>("nvmlInit_v2");
		auto by_index = symbol<int (*)(unsigned int, void **)>("nvmlDeviceGetHandleByIndex_v2");
		get_utilization = symbol<int (*)(void *, unsigned int *, unsigned int *)>("nvmlDeviceGetEncoderUtilization");
		shutdown = symbol<void (*)()>("nvmlShutdown");
		if (not(init and by_index and get_utilization) or init() != 0 or by_index(0, &device) != 0)
		{
			device = nullptr;
			get_utilization = nullptr;
		}
	}
	~nvml_encoder_utilization()
	{
		if (lib and shutdown and device)
			shutdown();
	}
	/// Percent, or -1 when unavailable.
	int read()
	{
		if (not device)
			return -1;
		unsigned int util = 0, period_us = 0;
		return get_utilization(device, &util, &period_us) == 0 ? int(util) : -1;
	}
};
#else
struct nvml_encoder_utilization
{
	int read()
	{
		return -1;
	}
};
#endif
} // namespace

health_monitor::health_monitor(configuration::health_options options_, std::filesystem::path default_log_dir, uint8_t video_streams_, send_fn send_, fill_fn fill_) :
        options(std::move(options_)),
        log_dir(options.log_dir.value_or(std::move(default_log_dir))),
        send(std::move(send_)),
        fill(std::move(fill_)),
        thresholds(health::thresholds::with(options.thresholds)),
        video_streams(video_streams_)
{
	history.reserve(history_size);
	if (options.log)
	{
		std::error_code ec;
		std::filesystem::create_directories(log_dir, ec);
		auto path = log_path("wivrn-health-", ".jsonl");
		jsonl.open(path, std::ios::app);
		if (jsonl)
			U_LOG_I("Stream health log: %s", path.string().c_str());
		else
			U_LOG_W("Stream health log: cannot open %s", path.string().c_str());
	}
	// Frame record (GRID #3095): the ring forgets the previous session; t0 is the session start,
	// the file opens on the first tick, when the display rate for the header is known.
	health::frames().reset();
	frames_t0_ns = os_monotonic_get_ns();
	if (options.frames)
	{
		frames_writer = std::make_unique<health::frame_writer>(log_dir, size_t(options.frames_max_mb) << 20, int(options.frames_keep), timestamp_for_filename);
		frames_writer->on_line = [this](const std::string & line) {
			if (on_line)
				on_line(line);
		};
	}
	started_unix_s = unix_now_s();
	thread = std::jthread([this](std::stop_token stop) { run(stop); });
}

std::filesystem::path health_monitor::default_log_dir()
{
#ifdef _WIN32
	const char * base = std::getenv("LOCALAPPDATA");
	return std::filesystem::path(base ? base : ".") / "wivrn" / "logs";
#else
	if (const char * state = std::getenv("XDG_STATE_HOME"); state and *state)
		return std::filesystem::path(state) / "wivrn";
	const char * home = std::getenv("HOME");
	return std::filesystem::path(home ? home : ".") / ".local" / "state" / "wivrn";
#endif
}

health_monitor::~health_monitor()
{
	thread.request_stop();
	thread.join();
}

std::filesystem::path health_monitor::log_path(const char * prefix, const char * extension) const
{
	return log_dir / (std::string(prefix) + timestamp_for_filename() + extension);
}

void health_monitor::on_feedback(const from_headset::feedback & f, const clock_offset & o)
{
	// All timestamps are in the headset clock; only differences are used.
	auto ms = [](int64_t a, int64_t b) { return (a and b) ? double(b - a) * ns_to_ms : -1.0; };
	double encode = ms(f.encode_begin, f.encode_end);
	double send = ms(f.send_begin, f.send_end);
	double spread = ms(f.received_first_packet, f.received_last_packet);
	double decode = ms(f.sent_to_decoder, f.received_from_decoder);
	double m2p = ms(f.encode_begin, f.displayed);
	uint8_t stream = f.stream_index % 3;

	// Frame record: the headset's side of the row, in server time. Relaxed stores, no lock.
	auto server_time = [&o](int64_t t) { return t ? o.from_headset(t) : 0; };
	health::frame_ring::feedback_times times{
	        .rx0 = server_time(f.received_first_packet),
	        .rx1 = server_time(f.received_last_packet),
	        .dec0 = server_time(f.sent_to_decoder),
	        .dec1 = server_time(f.received_from_decoder),
	        .blit = server_time(f.blitted),
	        .disp = server_time(f.displayed),
	        .shown = f.times_displayed,
	        .complete = f.sent_to_decoder != 0,
	        .off_ns = o.b,
	};
	health::frames().feedback(f.frame_index, f.stream_index, times);

	std::lock_guard lock(mutex);
	if (encode >= 0)
		fb.encode.observe(encode);
	if (send >= 0)
		fb.send.observe(send);
	if (spread >= 0)
		fb.spread.observe(spread);
	if (decode >= 0)
		fb.decode.observe(decode);
	if (m2p >= 0)
		fb.motion_to_photon.observe(m2p);
	if (not f.sent_to_decoder)
		fb.incomplete++;
	if (f.times_displayed > 1)
		fb.repeated += f.times_displayed - 1;
	if (f.displayed)
	{
		// Jitter: distance of each display interval from the nearest whole number of periods,
		// so a perfect stream reads 0 and a repeated frame still reads 0 (it is counted above).
		int64_t & last = fb.last_displayed[stream];
		int64_t period = fb.period_ns.load(std::memory_order_relaxed);
		if (last and f.displayed > last and period > 0)
		{
			int64_t delta = f.displayed - last;
			int64_t periods = std::max<int64_t>(1, (delta + period / 2) / period);
			fb.display_jitter.observe(double(std::abs(delta - periods * period)) * ns_to_ms);
		}
		last = f.displayed;
	}
	fb.off_ns = o.b;
	fb.off_min_ns = std::min(fb.off_min_ns, o.b);
	fb.off_max_ns = std::max(fb.off_max_ns, o.b);
}

void health_monitor::set_display_hz(float hz)
{
	int64_t period = hz > 0 ? int64_t(1e9 / hz) : 0;
	if (fb.period_ns.exchange(period, std::memory_order_relaxed) != period and period)
		on_event("display_hz", hz);
}

void health_monitor::on_telemetry(const from_headset::telemetry & t)
{
	std::lock_guard lock(mutex);
	last_telemetry = t;
	telemetry_fresh = true;
}

void health_monitor::on_session_event(std::string_view event)
{
	paused = event == "session_paused";
	write_line(nlohmann::json{{"t", unix_now_s()}, {"type", "event"}, {"event", event}}.dump());
	U_LOG_I("Stream health: %s", std::string(event).c_str());
	on_event(event == "session_paused" ? "pause" : "resume", nullptr);
}

void health_monitor::on_event(std::string_view name, nlohmann::json value)
{
	std::lock_guard lock(frames_mutex);
	if (frames_writer)
		frames_writer->event(os_monotonic_get_ns(), name, value);
}

void health_monitor::write_line(const std::string & line)
{
	{
		std::lock_guard lock(mutex);
		if (jsonl)
		{
			jsonl << line << '\n';
			jsonl.flush();
		}
	}
	if (on_line)
		on_line(line);
}

void health_monitor::on_connector_data(std::string_view data)
{
	connector_buffer.feed(data, [this](std::string_view line) { on_connector_line(line); });
}

void health_monitor::on_connector_line(std::string_view line)
{
	auto parsed = health::parse_connector_line(line);
	if (not parsed)
	{
		std::lock_guard lock(mutex);
		connector_dropped++;
		return;
	}
	if (parsed->kind == health::connector_line::kind::metric)
	{
		std::lock_guard lock(mutex);
		connector_metrics[parsed->name] = parsed->value;
		return;
	}
	int64_t t = unix_now_s();
	{
		std::lock_guard lock(mutex);
		if (not app_warnings.fire(parsed->name, parsed->ttl_s, t))
			return; // within its ttl or the cooldown: the headset already shows it
	}
	U_LOG_W("Stream health: app warning %s: %s", parsed->name.c_str(), parsed->message.c_str());
	if (options.warnings and send)
		send(parsed->message, true);
	write_line(nlohmann::json{{"t", t}, {"type", "warning"}, {"id", parsed->name}, {"state", "fired"}, {"message", parsed->message}, {"source", "app"}}.dump());
}

health::sample health_monitor::last_sample() const
{
	std::lock_guard lock(mutex);
	return last;
}

health::sample health_monitor::collect(int64_t now_unix_s)
{
	health::sample s;
	s.t_unix_s = now_unix_s;

	auto enc = video_encoder::take_health_counters();
	s.encoder.presented = uint32_t(enc.presented);
	s.encoder.skipped = uint32_t(enc.skipped);
	s.link.shards_sent = uint32_t(enc.shards);
	s.link.bytes_sent = enc.bytes;

	auto idr = idr_handler::take_request_counts();
	s.link.idr_lost_ref = idr.lost_reference + idr.dropped_idr + idr.timeout;
	s.link.idr_unsent_gap = 0; // needs encoder frame-id tracking; see docs/health.md

	{
		std::lock_guard lock(mutex);
		s.encoder.encode_ms_p95 = fb.encode.total() ? float(fb.encode.percentile(0.95)) : -1;
		s.link.send_ms_p95 = fb.send.total() ? float(fb.send.percentile(0.95)) : -1;
		s.link.receive_spread_ms_p95 = fb.spread.total() ? float(fb.spread.percentile(0.95)) : -1;
		s.headset.decode_ms_p95 = fb.decode.total() ? float(fb.decode.percentile(0.95)) : -1;
		s.headset.display_jitter_ms_p95 = fb.display_jitter.total() ? float(fb.display_jitter.percentile(0.95)) : -1;
		s.motion_to_photon_ms_p95 = fb.motion_to_photon.total() ? float(fb.motion_to_photon.percentile(0.95)) : -1;
		s.headset.frames_repeated = fb.repeated;
		// The headset's own counts (milestone 3) are authoritative when fresh this second; the
		// server's view of incomplete frames otherwise stays unknown, because the server only
		// sees the frames the headset acknowledged.
		if (last_telemetry and telemetry_fresh)
		{
			const auto & t = *last_telemetry;
			s.link.frames_incomplete = int32_t(t.frames_incomplete);
			s.headset.frames_repeated = t.frames_repeated;
			if (t.frames_decoded)
				s.headset.decode_ms_p95 = float(t.decode_ms_p95_x10) / 10.f;
			s.headset.gpu_ms = float(t.gpu_ms_x10) / 10.f;
			s.headset.cpu_ms = float(t.cpu_ms_x10) / 10.f;
			if (t.thermal_status != 255)
				s.headset.thermal_status = t.thermal_status;
			if (t.display_hz_active_x10)
				s.headset.display_hz = float(t.display_hz_active_x10) / 10.f;
			telemetry_fresh = false;
		}
		fb.encode.clear();
		fb.send.clear();
		fb.spread.clear();
		fb.decode.clear();
		fb.display_jitter.clear();
		fb.motion_to_photon.clear();
		fb.incomplete = 0;
		fb.repeated = 0;
		s.app_metrics = std::move(connector_metrics);
		connector_metrics.clear();
		s.app_metrics_dropped = connector_dropped;
		connector_dropped = 0;
		s.app_warnings = app_warnings.active();
	}

	if (fill)
		fill(s);
	return s;
}

namespace
{
to_headset::health to_packet(const health::sample & s, std::span<const health::rule_id> active)
{
	to_headset::health h{
	        .t_unix_s = s.t_unix_s,
	        .link = {s.link.idr_lost_ref, s.link.frames_incomplete, s.link.send_ms_p95, s.link.receive_spread_ms_p95},
	        .encoder = {s.encoder.presented, s.encoder.skipped, s.encoder.encode_ms_p95, s.encoder.util_pct},
	        .app = {s.app.hz, s.app.late_pct, s.app.cpu_ms, s.app.gpu_ms},
	        .headset = {s.headset.decode_ms_p95, s.headset.display_jitter_ms_p95, s.headset.display_hz, s.headset.requested_hz, s.headset.thermal_status, s.headset.battery_pct},
	        .motion_to_photon_ms_p95 = s.motion_to_photon_ms_p95,
	        .warnings = 0,
	};
	for (auto id: active)
		h.warnings |= uint16_t(1u << uint8_t(id));
	return h;
}
} // namespace

void health_monitor::write_event_dump(const health::rule_state::event & e, const health::sample & s)
{
	nlohmann::json j;
	j["t"] = s.t_unix_s;
	j["warning"] = health::name(e.id);
	j["message"] = e.message;
	j["sample"] = nlohmann::json::parse(health::to_json_line(s, state.active()));
	// The last 600 frames of each stream, as frame-record rows relative to the session's t0
	// (docs/health.md "Frame record").
	const auto & ring = health::frames();
	int64_t t0 = frames_t0_ns;
	int64_t latest = ring.latest();
	auto streams = nlohmann::json::array();
	for (uint8_t stream = 0; stream < video_streams and stream < health::frame_ring::streams; ++stream)
	{
		auto frames = nlohmann::json::array();
		for (int64_t n = std::max<int64_t>(0, latest - 599); n <= latest; ++n)
			if (auto row = ring.read(uint64_t(n), stream))
				frames.push_back(nlohmann::json::parse(health::frame_line(*row, t0)));
		streams.push_back(std::move(frames));
	}
	j["t0_mono_ns"] = t0;
	j["streams"] = streams;
	auto path = log_path("wivrn-health-event-", ".json");
	std::ofstream out(path);
	out << j.dump();
	U_LOG_I("Stream health: %s fired, timelines written to %s", std::string(health::name(e.id)).c_str(), path.string().c_str());
}

void health_monitor::run(std::stop_token stop)
{
	nvml_encoder_utilization nvml;
	auto next = std::chrono::steady_clock::now() + std::chrono::seconds(1);
	// The first second after start has partial counters: collect it to prime, do not evaluate.
	bool primed = false;
	while (not stop.stop_requested())
	{
		std::this_thread::sleep_until(next);
		next += std::chrono::seconds(1);
		if (stop.stop_requested())
			break;

		int64_t now = unix_now_s();
		std::vector<std::string> expired;
		{
			std::lock_guard lock(mutex);
			expired = app_warnings.expire(now);
		}
		for (const auto & id: expired)
			write_line(nlohmann::json{{"t", now}, {"type", "warning"}, {"id", id}, {"state", "cleared"}, {"source", "app"}}.dump());
		health::sample s = collect(now);
		s.encoder.util_pct = nvml.read();
		frames_tick(s);
		if (not primed)
		{
			primed = true;
			continue;
		}

		// While the headset is away, record the second (counters are zero) but keep the rules and
		// their history untouched so a reconnect does not look like a loss or a slow app.
		bool is_paused = paused.load();
		if (not is_paused)
		{
			if (history.size() >= history_size)
				history.erase(history.begin());
			history.push_back(s);
		}
		auto events = is_paused ? std::vector<health::rule_state::event>{} : state.step(history, thresholds);
		auto active = state.active();
		auto j = nlohmann::json::parse(health::to_json_line(s, active));
		j["type"] = "sample";
		if (is_paused)
			j["session"] = "paused";
		{
			std::lock_guard lock(mutex);
			last = s;
		}
		write_line(j.dump());
		if (send_health and not is_paused)
			send_health(to_packet(s, active));

		for (const auto & e: events)
		{
			if (e.fired)
			{
				U_LOG_W("Stream health: %s", e.message.c_str());
				if (options.warnings and send)
					send(e.message, true);
				if (options.log)
					write_event_dump(e, s);
			}
			else
			{
				U_LOG_I("Stream health: %s", e.message.c_str());
				if (options.warnings and send and state.active().empty())
					send("Stream recovered", false);
			}
			write_line(nlohmann::json{{"t", s.t_unix_s}, {"type", "warning"}, {"id", health::name(e.id)}, {"state", e.fired ? "fired" : "cleared"}, {"message", e.message}}.dump());
			on_event("warning", std::string(health::name(e.id)) + (e.fired ? " fired" : " cleared"));
		}
	}
}

void health_monitor::frames_tick(const health::sample & s)
{
	if (not frames_writer)
		return;
	int64_t off, lo, hi;
	{
		std::lock_guard lock(mutex);
		off = fb.off_ns;
		lo = fb.off_min_ns;
		hi = fb.off_max_ns;
		fb.off_min_ns = INT64_MAX;
		fb.off_max_ns = INT64_MIN;
	}
	int64_t now_ns = os_monotonic_get_ns();
	std::lock_guard lock(frames_mutex);
	if (not frames_writer->started())
	{
		if (s.headset.display_hz <= 0)
			return; // the header needs the display rate; the ring keeps the frames meanwhile
		int64_t unix_ms = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count();
		health::frame_header header{
		        .t0_mono_ns = frames_t0_ns,
		        .t0_unix_ms = unix_ms - (now_ns - frames_t0_ns) / 1'000'000,
		        .host = hostname(),
		        .server = display_version(),
		        .streams = video_streams,
		        .display_hz = s.headset.display_hz,
		        .period_ns = int64_t(1e9 / s.headset.display_hz),
		};
		frames_writer->start(health::frames(), header);
		U_LOG_I("Stream health: frame record %s", frames_writer->current().string().c_str());
	}
	for (const auto & e: health::events().take())
		frames_writer->event(e.t_ns, e.name, e.value);
	frames_writer->tick(health::frames(), now_ns, off, lo <= hi ? hi - lo : 0);
}

} // namespace wivrn
