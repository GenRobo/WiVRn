/*
 * WiVRn VR streaming
 * Copyright (C) 2026  WiVRn Contributors
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <https://www.gnu.org/licenses/>.
 */

#include "wivrn_trace.h"

#include "util/u_trace_marker.h"

#ifdef WIVRN_USE_PERFETTO

#include "os/os_time.h"
#include "util/u_debug.h"
#include "util/u_logging.h"
#ifndef _WIN32
#include "utils/xdg_base_directory.h"
#endif

#include <cstdint>
#include <cstdlib>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <memory>
#include <perfetto.h>
#include <string>
#ifdef _WIN32
#include <process.h>
#else
#include <unistd.h>
#endif
#include <vector>

PERFETTO_DEFINE_CATEGORIES(
        perfetto::Category("wivrn_encoder").SetDescription("WiVRn encoder CPU spans"),
        perfetto::Category("wivrn_encoder_gpu").SetDescription("WiVRn encoder GPU slices"),
        perfetto::Category("wivrn_compositor").SetDescription("WiVRn compositor spans"),
        perfetto::Category("wivrn_network").SetDescription("WiVRn network send spans"),
        perfetto::Category("wivrn_feedback").SetDescription("WiVRn dump_time markers"));
PERFETTO_TRACK_EVENT_STATIC_STORAGE();

DEBUG_GET_ONCE_OPTION(wivrn_tracing, "WIVRN_TRACING", NULL)

namespace wivrn::trace
{
namespace
{
bool initialized = false;
bool enabled = false;
::vk_bundle * calibration = nullptr;

perfetto::Track g_encoder_track(1);
perfetto::Track g_compositor_track(2);
perfetto::Track g_network_track(3);
perfetto::Track g_feedback_track(4);

// WIVRN_TRACING is the single switch: it both enables tracing and selects which Perfetto
// backend(s) init() arms.
//   unset/off/0/false/no - disabled
//   system / 1/true/yes/on - connect to an external traced as a producer (the consumer
//                            owns the session); legacy boolean values map here
//   inprocess             - run the tracing service in this process and write the file
//   system+inprocess      - arm both independently
// Unknown values emit a warning and leave tracing disabled.
enum class tracing_mode
{
	off,
	system,
	inprocess,
	both,
};

// In-process session state (only used when the mode includes inprocess). The session
// must outlive every emit and be stopped/flushed in shutdown() to produce a file.
std::unique_ptr<perfetto::TracingSession> g_inproc_session;

tracing_mode parse_tracing_mode()
{
	const char * v = debug_get_option_wivrn_tracing();
	if (!v or !*v)
		return tracing_mode::off;
	const std::string s = v;
	if (s == "off" or s == "0" or s == "false" or s == "no")
		return tracing_mode::off;
	if (s == "system" or s == "1" or s == "true" or s == "yes" or s == "on")
		return tracing_mode::system;
	if (s == "inprocess")
		return tracing_mode::inprocess;
	if (s == "system+inprocess" or s == "both")
		return tracing_mode::both;
	U_LOG_W("wivrn::trace: unknown WIVRN_TRACING '%s' (expected off|system|inprocess|system+inprocess), tracing disabled", v);
	return tracing_mode::off;
}

#ifndef _WIN32
// Default socket is root-only; use a per-user XDG_RUNTIME_DIR path so a user-launched traced can bind.
std::string default_producer_sock_path()
{
	return (xdg_runtime_dir() / "wivrn-perfetto-producer.sock").string();
}
#endif

int process_id()
{
#ifdef _WIN32
	return _getpid();
#else
	return getpid();
#endif
}

// Callers supply os_monotonic_get_ns() values, not the SDK's default BOOTTIME
// clock. Declare their domain explicitly so Perfetto can use its clock snapshots.
perfetto::TraceTimestamp host_timestamp(int64_t time_ns)
{
#ifdef _WIN32
	// Monado scales QPC by an integer ns/tick. Perfetto instead uses QPC/frequency;
	// these agree for the usual 10 MHz clock, but not for every QPC frequency.
	static const double scale = [] {
		LARGE_INTEGER frequency;
		QueryPerformanceFrequency(&frequency);
		return 1e9 / (static_cast<double>(frequency.QuadPart) * os_ns_per_qpc_tick_get());
	}();
	const uint64_t value = scale == 1.0 ? static_cast<uint64_t>(time_ns) : static_cast<uint64_t>(time_ns * scale);
#else
	const uint64_t value = static_cast<uint64_t>(time_ns);
#endif
	return {perfetto::protos::pbzero::BUILTIN_CLOCK_MONOTONIC, value};
}

// Local-time stamp for trace filenames, evaluated per flush.
std::string now_timestamp()
{
	const std::time_t now = std::time(nullptr);
	std::tm tm{};
#ifdef _WIN32
	localtime_s(&tm, &now);
#else
	localtime_r(&now, &tm);
#endif
	char buf[16];
	std::strftime(buf, sizeof(buf), "%Y%m%d-%H%M%S", &tm);
	return buf;
}

std::string expand_file_placeholders(std::string s)
{
	const std::string ts = now_timestamp();
	const std::string pid = std::to_string(process_id());
	for (size_t pos = 0; (pos = s.find('%', pos)) != std::string::npos;)
	{
		if (pos + 1 >= s.size())
			break;
		const char c = s[pos + 1];
		const std::string * rep = nullptr;
		if (c == 't')
			rep = &ts;
		else if (c == 'p')
			rep = &pid;
		if (rep)
		{
			s.replace(pos, 2, *rep);
			pos += rep->size();
		}
		else
			pos += 2; // leave any other %x untouched
	}
	return s;
}

// Path from WIVRN_TRACING_FILE; unset ⇒ wivrn-<ts>-<pid>.pftrace in cwd. %t/%p expanded.
std::string resolve_inproc_file_path()
{
	const char * file = std::getenv("WIVRN_TRACING_FILE");
	if (!file or !*file)
		return "wivrn-" + now_timestamp() + "-" + std::to_string(process_id()) + ".pftrace";
	return expand_file_placeholders(file);
}

// Don't clobber an existing trace; suffix -2, -3, ... until free.
std::filesystem::path unique_file_path(const std::string & candidate)
{
	std::filesystem::path p = candidate;
	std::error_code ec;
	if (not std::filesystem::exists(p, ec))
		return p;
	const auto dir = p.parent_path();
	const auto stem = p.stem().string();
	const auto ext = p.extension().string();
	for (unsigned n = 2;; ++n)
	{
		std::filesystem::path q = dir / (stem + "-" + std::to_string(n) + ext);
		if (not std::filesystem::exists(q, ec))
			return q;
	}
}

// Flush + stop the session and write its buffer; caller re-arms or resets.
void write_inproc_session()
{
	perfetto::TrackEvent::Flush();
	g_inproc_session->StopBlocking();
	const std::vector<char> data = g_inproc_session->ReadTraceBlocking();

	const std::filesystem::path path = unique_file_path(resolve_inproc_file_path());
	std::error_code ec;
	if (const auto parent = path.parent_path(); not parent.empty())
		std::filesystem::create_directories(parent, ec);

	std::ofstream out(path, std::ios::binary | std::ios::trunc);
	if (out)
	{
		out.write(data.data(), static_cast<std::streamsize>(data.size()));
		out.close();
		if (out)
			U_LOG_I("wivrn::trace: wrote %zu bytes to %s", data.size(), path.string().c_str());
		else
			U_LOG_E("wivrn::trace: failed to write %s", path.string().c_str());
	}
	else
		U_LOG_E("wivrn::trace: failed to open %s for writing", path.string().c_str());
}

// Arm an in-process tracing session. The output path is resolved when the buffer is written
// (on flush_session/shutdown), so each rotated trace gets its own timestamped file.
void start_inprocess_session()
{
	uint32_t buffer_kb = 65536;
	if (const char * b = std::getenv("WIVRN_TRACING_BUFFER_KB"); b and *b)
	{
		if (const long parsed = std::strtol(b, nullptr, 10); parsed > 0)
			buffer_kb = static_cast<uint32_t>(parsed);
	}

	// Enable only the WiVRn categories, mirroring the system-backend .pbtx allowlist.
	// Do NOT add disabled_categories("*"): per wivrn_trace_cfg.pbtx that silences the
	// allowlist too. wivrn-server defines only these categories, so this fully scopes it.
	perfetto::protos::gen::TrackEventConfig te;
	te.add_enabled_categories("wivrn_encoder");
	te.add_enabled_categories("wivrn_encoder_gpu");
	te.add_enabled_categories("wivrn_compositor");
	te.add_enabled_categories("wivrn_network");
	te.add_enabled_categories("wivrn_feedback");

	perfetto::TraceConfig cfg;
	cfg.add_buffers()->set_size_kb(buffer_kb);
	auto * ds = cfg.add_data_sources()->mutable_config();
	ds->set_name("track_event");
	ds->set_track_event_config_raw(te.SerializeAsString());

	g_inproc_session = perfetto::Tracing::NewTrace(perfetto::kInProcessBackend);
	g_inproc_session->Setup(cfg);
	g_inproc_session->StartBlocking();
	U_LOG_I("wivrn::trace: in-process session started, %u KiB buffer", buffer_kb);
}

perfetto::Track & cpu_track_obj(cpu_track d)
{
	switch (d)
	{
		case cpu_track::encoder:
			return g_encoder_track;
		case cpu_track::compositor:
			return g_compositor_track;
		case cpu_track::network:
			return g_network_track;
	}
	return g_encoder_track;
}
} // namespace

// Bridge the WIVRN_TRACING switch onto Monado's tracing before u_trace_marker_init() caches
// XRT_TRACING. Only the system backend reaches a traced consumer, so only enable Monado then;
// setenv(..., 0) preserves an explicit user-set XRT_TRACING. No-op without WIVRN_TRACE_MONADO.
static void propagate_to_monado()
{
#if defined(WIVRN_TRACE_MONADO) && !defined(_WIN32)
	const tracing_mode mode = parse_tracing_mode();
	if (mode == tracing_mode::system or mode == tracing_mode::both)
		setenv("XRT_TRACING", "true", 0);
#endif
}

void init()
{
	if (initialized)
		return;
	initialized = true;

	// Monado's marker init caches XRT_TRACING, so propagate_to_monado() must run first.
	// Both run regardless of WIVRN_TRACING so Monado's tracing behaves as it would normally.
	propagate_to_monado();
	u_trace_marker_init();

	const tracing_mode mode = parse_tracing_mode();
	if (mode == tracing_mode::off)
	{
		U_LOG_D("wivrn::trace: WIVRN_TRACING not set, perfetto tracing disabled");
		return;
	}

	bool use_system = mode == tracing_mode::system or mode == tracing_mode::both;
	const bool use_inprocess = mode == tracing_mode::inprocess or mode == tracing_mode::both;
#ifdef _WIN32
	if (use_system)
	{
		U_LOG_W("wivrn::trace: system backend is unsupported on Windows; use WIVRN_TRACING=inprocess");
		use_system = false;
	}
#endif
	if (!use_system and !use_inprocess)
		return;
	enabled = true;

	perfetto::TracingInitArgs args;
	if (use_system)
	{
#ifndef _WIN32
		const auto sock = default_producer_sock_path();
		if (setenv("PERFETTO_PRODUCER_SOCK_NAME", sock.c_str(), 0) == 0)
			U_LOG_I("wivrn::trace: producer socket %s", std::getenv("PERFETTO_PRODUCER_SOCK_NAME"));
		args.backends |= perfetto::kSystemBackend;
#endif
	}
	if (use_inprocess)
		args.backends |= perfetto::kInProcessBackend;
	perfetto::Tracing::Initialize(args);
	perfetto::TrackEvent::Register();

	// Name the custom tracks; descriptors must be emitted after Register().
	auto set_track_name = [](perfetto::Track & trk, const char * name) {
		auto desc = trk.Serialize();
		desc.set_name(name);
		perfetto::TrackEvent::SetTrackDescriptor(trk, desc);
	};
	set_track_name(g_encoder_track, "WiVRn encoder");
	set_track_name(g_compositor_track, "WiVRn compositor");
	set_track_name(g_network_track, "WiVRn network");
	set_track_name(g_feedback_track, "WiVRn feedback");

	// The in-process session must be started after Register() so its data source is known.
	if (use_inprocess)
		start_inprocess_session();

	U_LOG_I("wivrn::trace: init complete");
}

void flush_session()
{
	if (!initialized or !g_inproc_session)
		return;
	// Client gone but process lives; flush and re-arm for the next one.
	write_inproc_session();
	g_inproc_session.reset();
	start_inprocess_session();
}

void shutdown()
{
	if (!initialized)
		return;
	if (g_inproc_session)
	{
		write_inproc_session();
		g_inproc_session.reset();
	}
	enabled = false;
}

bool is_enabled()
{
	return enabled;
}

void set_calibration_source(::vk_bundle * monado_vk)
{
	calibration = monado_vk;
}

::vk_bundle * calibration_source()
{
	return calibration;
}

void instant_feedback(const char * name, int64_t time_ns, uint64_t frame, uint8_t stream)
{
	if (!enabled)
		return;
	TRACE_EVENT_INSTANT("wivrn_feedback", perfetto::DynamicString(name), g_feedback_track, host_timestamp(time_ns), "frame", frame, "stream", static_cast<uint32_t>(stream));
}

void frame_prediction(uint64_t app_id, int64_t frame_id, int64_t when_host_ns,
                      int64_t wake_host_ns, int64_t target_host_ns, int64_t period_ns)
{
	if (!enabled or !TRACE_EVENT_CATEGORY_ENABLED("wivrn_compositor"))
		return;
	TRACE_EVENT_INSTANT("wivrn_compositor", perfetto::DynamicString(app_id ? "app_prediction" : "compositor_prediction"),
	                   g_compositor_track, host_timestamp(when_host_ns),
	                   "app_id", app_id, "frame", frame_id, "wake_host_ns", wake_host_ns,
	                   "target_host_ns", target_host_ns, "period_ns", period_ns);
}

void app_frame(app_stage stage, uint64_t app_id, int64_t frame_id, int64_t when_host_ns,
               int64_t display_target_host_ns, int64_t system_frame_id)
{
	if (!enabled or !TRACE_EVENT_CATEGORY_ENABLED("wivrn_compositor"))
		return;
	const char * name = nullptr;
	switch (stage)
	{
		case app_stage::wake: name = "app_wake"; break;
		case app_stage::begin: name = "app_begin"; break;
		case app_stage::submit_begin: name = "app_submit_begin"; break;
		case app_stage::submit_end: name = "app_submit_end"; break;
		case app_stage::delivered: name = "app_delivered"; break;
		case app_stage::gpu_done: name = "app_gpu_done"; break;
		case app_stage::discarded: name = "app_discarded"; break;
		case app_stage::latched: name = "app_latched"; break;
		case app_stage::retired: name = "app_retired"; break;
	}
	TRACE_EVENT_INSTANT("wivrn_compositor", perfetto::DynamicString(name),
	                   g_compositor_track, host_timestamp(when_host_ns),
	                   "app_id", app_id, "frame", frame_id, "system_frame", system_frame_id,
	                   "display_target_host_ns", display_target_host_ns);
}

void head_pose_sample(int64_t production_host_ns, int64_t sample_target_host_ns, uint64_t pose_flags)
{
	if (!enabled or !TRACE_EVENT_CATEGORY_ENABLED("wivrn_compositor"))
		return;
	TRACE_EVENT_INSTANT("wivrn_compositor", "head_pose_sample", g_compositor_track,
	                   host_timestamp(os_monotonic_get_ns()),
	                   "production_host_ns", production_host_ns, "sample_target_host_ns", sample_target_host_ns,
	                   "pose_flags", pose_flags);
}

void head_pose_query(bool view_query, int64_t when_host_ns, int64_t requested_target_host_ns,
                     int64_t effective_target_host_ns, int64_t position_production_host_ns, uint64_t relation_flags)
{
	if (!enabled or !TRACE_EVENT_CATEGORY_ENABLED("wivrn_compositor"))
		return;
	TRACE_EVENT_INSTANT("wivrn_compositor", "head_pose_query", g_compositor_track, host_timestamp(when_host_ns),
	                   "view_query", view_query, "requested_target_host_ns", requested_target_host_ns,
	                   "effective_target_host_ns", effective_target_host_ns,
	                   "position_production_host_ns", position_production_host_ns, "relation_flags", relation_flags);
}

void head_pose_evaluation(int64_t requested_target_host_ns, int64_t position_target_host_ns,
                          int64_t orientation_target_host_ns, int64_t position_production_host_ns,
                          int64_t orientation_production_host_ns, uint64_t relation_flags)
{
	if (!enabled or !TRACE_EVENT_CATEGORY_ENABLED("wivrn_compositor"))
		return;
	TRACE_EVENT_INSTANT("wivrn_compositor", "head_pose_evaluation", g_compositor_track,
	                   host_timestamp(os_monotonic_get_ns()), "requested_target_host_ns", requested_target_host_ns,
	                   "position_target_host_ns", position_target_host_ns,
	                   "orientation_target_host_ns", orientation_target_host_ns,
	                   "position_production_host_ns", position_production_host_ns,
	                   "orientation_production_host_ns", orientation_production_host_ns, "relation_flags", relation_flags);
}

void feedback_summary(uint64_t frame_id, uint8_t stream, uint8_t times_displayed, bool clock_valid,
                      int64_t predicted_display_headset_ns, int64_t predicted_display_host_ns)
{
	if (!enabled or !TRACE_EVENT_CATEGORY_ENABLED("wivrn_feedback"))
		return;
	TRACE_EVENT_INSTANT("wivrn_feedback", "feedback_summary", g_feedback_track,
	                   host_timestamp(os_monotonic_get_ns()), "frame", frame_id, "stream", static_cast<uint32_t>(stream),
	                   "selection_count", static_cast<uint32_t>(times_displayed), "clock_valid", clock_valid,
	                   "predicted_display_headset_ns", predicted_display_headset_ns,
	                   "predicted_display_host_ns", predicted_display_host_ns);
}

void gpu_slice(gpu_track, const char * slice_name, int64_t begin_ns, int64_t end_ns, uint64_t frame, uint8_t stream)
{
	if (!enabled)
		return;
	// gpu_track is unused today (all slices share the encoder track); kept for future GPU lanes.
	TRACE_EVENT_BEGIN("wivrn_encoder_gpu", perfetto::DynamicString(slice_name), g_encoder_track, host_timestamp(begin_ns), "frame", frame, "stream", static_cast<uint32_t>(stream));
	TRACE_EVENT_END("wivrn_encoder_gpu", g_encoder_track, host_timestamp(end_ns));
}

#define WIVRN_CPU_BEGIN(CAT, trk, name, ts, frame, stream) \
	TRACE_EVENT_BEGIN(CAT, perfetto::DynamicString(name), (trk), host_timestamp(ts), "frame", (frame), "stream", static_cast<uint32_t>(stream))

scope::scope(cpu_track which, uint8_t stream, uint64_t frame, const char * name) :
        which(which), stream(stream), frame(frame), name(name), begin_ns(0), active(false)
{
	if (!wivrn::trace::enabled)
		return;
	bool enabled = false;
	switch (which)
	{
		case cpu_track::encoder:
			enabled = TRACE_EVENT_CATEGORY_ENABLED("wivrn_encoder");
			break;
		case cpu_track::compositor:
			enabled = TRACE_EVENT_CATEGORY_ENABLED("wivrn_compositor");
			break;
		case cpu_track::network:
			enabled = TRACE_EVENT_CATEGORY_ENABLED("wivrn_network");
			break;
	}
	if (!enabled)
		return;
	active = true;
	begin_ns = os_monotonic_get_ns();
}

scope::~scope()
{
	if (!active)
		return;
	const int64_t end_ns = os_monotonic_get_ns();
	perfetto::Track & trk = cpu_track_obj(which);
	switch (which)
	{
		case cpu_track::encoder:
			WIVRN_CPU_BEGIN("wivrn_encoder", trk, name, begin_ns, frame, stream);
			TRACE_EVENT_END("wivrn_encoder", trk, host_timestamp(end_ns));
			break;
		case cpu_track::compositor:
			WIVRN_CPU_BEGIN("wivrn_compositor", trk, name, begin_ns, frame, stream);
			TRACE_EVENT_END("wivrn_compositor", trk, host_timestamp(end_ns));
			break;
		case cpu_track::network:
			WIVRN_CPU_BEGIN("wivrn_network", trk, name, begin_ns, frame, stream);
			TRACE_EVENT_END("wivrn_network", trk, host_timestamp(end_ns));
			break;
	}
}

void cpu_instant(cpu_track which, const char * name, uint64_t frame, uint8_t stream)
{
	if (!enabled)
		return;
	const int64_t ts = os_monotonic_get_ns();
	perfetto::Track & trk = cpu_track_obj(which);
	switch (which)
	{
		case cpu_track::encoder:
			TRACE_EVENT_INSTANT("wivrn_encoder", perfetto::DynamicString(name), trk, host_timestamp(ts), "frame", frame, "stream", static_cast<uint32_t>(stream));
			break;
		case cpu_track::compositor:
			TRACE_EVENT_INSTANT("wivrn_compositor", perfetto::DynamicString(name), trk, host_timestamp(ts), "frame", frame, "stream", static_cast<uint32_t>(stream));
			break;
		case cpu_track::network:
			TRACE_EVENT_INSTANT("wivrn_network", perfetto::DynamicString(name), trk, host_timestamp(ts), "frame", frame, "stream", static_cast<uint32_t>(stream));
			break;
	}
}

void cpu_begin(cpu_track which, uint8_t stream, uint64_t frame, const char * name)
{
	if (!enabled)
		return;
	const int64_t ts = os_monotonic_get_ns();
	perfetto::Track & trk = cpu_track_obj(which);
	switch (which)
	{
		case cpu_track::encoder:
			WIVRN_CPU_BEGIN("wivrn_encoder", trk, name, ts, frame, stream);
			break;
		case cpu_track::compositor:
			WIVRN_CPU_BEGIN("wivrn_compositor", trk, name, ts, frame, stream);
			break;
		case cpu_track::network:
			WIVRN_CPU_BEGIN("wivrn_network", trk, name, ts, frame, stream);
			break;
	}
}

void cpu_end(cpu_track which, uint8_t, uint64_t, const char *)
{
	if (!enabled)
		return;
	const int64_t ts = os_monotonic_get_ns();
	perfetto::Track & trk = cpu_track_obj(which);
	switch (which)
	{
		case cpu_track::encoder:
			TRACE_EVENT_END("wivrn_encoder", trk, host_timestamp(ts));
			break;
		case cpu_track::compositor:
			TRACE_EVENT_END("wivrn_compositor", trk, host_timestamp(ts));
			break;
		case cpu_track::network:
			TRACE_EVENT_END("wivrn_network", trk, host_timestamp(ts));
			break;
	}
}
} // namespace wivrn::trace

#else // !WIVRN_USE_PERFETTO

namespace wivrn::trace
{
void init()
{
	u_trace_marker_init();
}
void flush_session() {}
void shutdown() {}
bool is_enabled()
{
	return false;
}
void set_calibration_source(::vk_bundle *) {}
::vk_bundle * calibration_source()
{
	return nullptr;
}
void instant_feedback(const char *, int64_t, uint64_t, uint8_t) {}
void frame_prediction(uint64_t, int64_t, int64_t, int64_t, int64_t, int64_t) {}
void app_frame(app_stage, uint64_t, int64_t, int64_t, int64_t, int64_t) {}
void head_pose_sample(int64_t, int64_t, uint64_t) {}
void head_pose_query(bool, int64_t, int64_t, int64_t, int64_t, uint64_t) {}
void head_pose_evaluation(int64_t, int64_t, int64_t, int64_t, int64_t, uint64_t) {}
void feedback_summary(uint64_t, uint8_t, uint8_t, bool, int64_t, int64_t) {}
void gpu_slice(gpu_track, const char *, int64_t, int64_t, uint64_t, uint8_t) {}
scope::scope(cpu_track which, uint8_t stream, uint64_t frame, const char * name) :
        which(which), stream(stream), frame(frame), name(name), begin_ns(0), active(false) {}
scope::~scope() {}
void cpu_instant(cpu_track, const char *, uint64_t, uint8_t) {}
void cpu_begin(cpu_track, uint8_t, uint64_t, const char *) {}
void cpu_end(cpu_track, uint8_t, uint64_t, const char *) {}
} // namespace wivrn::trace

#endif // WIVRN_USE_PERFETTO
