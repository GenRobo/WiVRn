/*
 * WiVRn stream health monitor (GRID #2834). See health_monitor.h.
 */
#include "health_monitor.h"

#include "app_pacer.h"
#include "encoder/idr_handler.h"
#include "encoder/video_encoder.h"
#include "util/u_logging.h"

#include <chrono>
#include <cmath>
#include <ctime>
#include <format>
#include <nlohmann/json.hpp>
#include <regex>

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

health_monitor::health_monitor(configuration::health_options options_, std::filesystem::path default_log_dir, send_fn send_, fill_fn fill_) :
        options(std::move(options_)),
        log_dir(options.log_dir.value_or(std::move(default_log_dir))),
        send(std::move(send_)),
        fill(std::move(fill_)),
        thresholds(health::thresholds::with(options.thresholds))
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

void health_monitor::on_feedback(const from_headset::feedback & f)
{
	// All timestamps are in the headset clock; only differences are used.
	auto ms = [](int64_t a, int64_t b) { return (a and b) ? double(b - a) * ns_to_ms : -1.0; };
	double encode = ms(f.encode_begin, f.encode_end);
	double send = ms(f.send_begin, f.send_end);
	double spread = ms(f.received_first_packet, f.received_last_packet);
	double decode = ms(f.sent_to_decoder, f.received_from_decoder);
	double m2p = ms(f.encode_begin, f.displayed);
	uint8_t stream = f.stream_index % 3;

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
	auto & ring = fb.ring[stream];
	ring[fb.ring_next[stream]] = {
	        .frame_index = f.frame_index,
	        .stream = f.stream_index,
	        .encode_begin = f.encode_begin,
	        .encode_end = f.encode_end,
	        .send_begin = f.send_begin,
	        .send_end = f.send_end,
	        .received_first = f.received_first_packet,
	        .received_last = f.received_last_packet,
	        .sent_to_decoder = f.sent_to_decoder,
	        .received_from_decoder = f.received_from_decoder,
	        .blitted = f.blitted,
	        .displayed = f.displayed,
	        .times_displayed = f.times_displayed,
	};
	fb.ring_next[stream] = (fb.ring_next[stream] + 1) % ring_size;
	fb.ring_count[stream] = std::min(fb.ring_count[stream] + 1, ring_size);
}

void health_monitor::set_display_hz(float hz)
{
	fb.period_ns.store(hz > 0 ? int64_t(1e9 / hz) : 0, std::memory_order_relaxed);
}

void health_monitor::on_session_event(std::string_view event)
{
	paused = event == "session_paused";
	write_line(nlohmann::json{{"t", unix_now_s()}, {"type", "event"}, {"event", event}}.dump());
	U_LOG_I("Stream health: %s", std::string(event).c_str());
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

void health_monitor::on_connector_line(std::string_view line)
{
	static const std::regex ident("^[a-z0-9_]{1,32}$");
	nlohmann::json j = nlohmann::json::parse(line, nullptr, false);
	auto type = j.is_object() ? j.value("type", "") : "";
	if (type == "metric")
	{
		auto name = j.value("name", "");
		if (std::regex_match(name, ident) and j.contains("value") and j["value"].is_number())
		{
			std::lock_guard lock(mutex);
			connector_metrics[name] = j["value"].get<double>();
			return;
		}
	}
	else if (type == "warning")
	{
		auto id = j.value("id", "");
		auto message = j.value("message", "");
		if (std::regex_match(id, ident) and not message.empty() and message.size() <= 200)
		{
			if (options.warnings and send)
				send(message, true);
			if (on_line)
				on_line(nlohmann::json{{"type", "warning"}, {"id", id}, {"state", "fired"}, {"message", message}, {"source", "app"}}.dump());
			return;
		}
	}
	std::lock_guard lock(mutex);
	invalid_connector_lines++;
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
		// frames_incomplete stays -1 until the headset reports it (milestone 3): the server only
		// sees the frames the headset acknowledged, complete or not.
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
	}

	if (fill)
		fill(s);
	return s;
}

void health_monitor::write_event_dump(const health::rule_state::event & e, const health::sample & s)
{
	nlohmann::json j;
	j["t"] = s.t_unix_s;
	j["warning"] = health::name(e.id);
	j["message"] = e.message;
	j["sample"] = nlohmann::json::parse(health::to_json_line(s, state.active()));
	auto streams = nlohmann::json::array();
	{
		std::lock_guard lock(mutex);
		for (size_t stream = 0; stream < 3; ++stream)
		{
			auto frames = nlohmann::json::array();
			size_t count = fb.ring_count[stream];
			size_t start = (fb.ring_next[stream] + ring_size - count) % ring_size;
			for (size_t i = 0; i < count; ++i)
			{
				const auto & r = fb.ring[stream][(start + i) % ring_size];
				frames.push_back({
				        r.frame_index,
				        r.encode_begin,
				        r.encode_end,
				        r.send_begin,
				        r.send_end,
				        r.received_first,
				        r.received_last,
				        r.sent_to_decoder,
				        r.received_from_decoder,
				        r.blitted,
				        r.displayed,
				        r.times_displayed,
				});
			}
			streams.push_back(frames);
		}
	}
	j["columns"] = {"frame_index", "encode_begin", "encode_end", "send_begin", "send_end", "received_first", "received_last", "sent_to_decoder", "received_from_decoder", "blitted", "displayed", "times_displayed"};
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

		health::sample s = collect(unix_now_s());
		s.encoder.util_pct = nvml.read();
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
		}
	}
}

} // namespace wivrn
