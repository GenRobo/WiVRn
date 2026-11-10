/*
 * WiVRn VR streaming
 * Copyright (C) 2022  Guillaume Meunier <guillaume.meunier@centraliens.net>
 * Copyright (C) 2022  Patrick Nicolas <patricknicolas@laposte.net>
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

#include "stream.h"

#include "application.h"
#include "utils/i18n.h"
#include "utils/named_thread.h"

#include <algorithm>
#include <cmath>
#include <spdlog/spdlog.h>
#include <uni_algo/case.h>

void scenes::stream::process_packets()
{
#ifdef __ANDROID__
	application::instance().setup_jni();
#endif
	while (state_ != state::shutdown)
	{
		try
		{
			network_session->poll(*this, std::chrono::milliseconds(500));
		}
		catch (std::exception & e)
		{
			spdlog::info("Exception in network thread, exiting: {}", e.what());
			exit();
		}
	}
}

void scenes::stream::operator()(to_headset::server_message && message)
{
	switch (message.kind)
	{
		case to_headset::server_message::kind::toast:
		case to_headset::server_message::kind::toast_urgent: {
			auto toast = gui_toast.lock();
			toast->emplace(message.msg, message.kind == to_headset::server_message::kind::toast_urgent);

			gui_status_last_change = instance.now();
			break;
		}
		case to_headset::server_message::kind::warning: {
			gui_warning.lock()->emplace(message.msg, instance.now());
			break;
		}

		case to_headset::server_message::kind::error: {
			auto queue = stream_error_queue.lock();
			queue->emplace(std::move(message.msg));

			break;
		}
	}
}

void scenes::stream::operator()(to_headset::video_stream_data_shard && shard)
{
	std::shared_lock lock(decoder_mutex);
	uint8_t idx = shard.stream_item_idx;
	if (idx >= decoders.size())
	{
		// We don't know (yet?) about this stream, ignore packet
		return;
	}
	decoders[idx].decoder->push_shard(std::move(shard));
}

void scenes::stream::operator()(to_headset::feature_control && control)
{
	switch (control.f)
	{
		case wivrn::to_headset::feature_control::hid_input:
			hid_forwarding = control.state;
			if (not control.state and (application::get_config().forward_keyboard or application::get_config().forward_mouse or application::get_config().forward_gamepad))
			{
				auto toast = gui_toast.lock();
				toast->emplace(_("The server does not allow forwarded input devices"), true);
				gui_status_last_change = instance.now();
			}
			return;
		case wivrn::to_headset::feature_control::microphone:
			if (audio_handle)
				audio_handle->set_mic_state(control.state);
			return;
	}
}

void scenes::stream::operator()(to_headset::audio_stream_description && desc)
{
	audio_handle.emplace(desc, *network_session, instance);
}

void scenes::stream::operator()(to_headset::video_stream_description && desc)
{
	setup(desc);

	if (not tracking_thread)
	{
		tracking_thread = utils::named_thread("tracking_thread", &stream::tracking, this);
	}
}

void scenes::stream::operator()(to_headset::refresh_rate_change && rate)
{
	spdlog::info("refresh rate change request: {}", rate.hz);
	session.set_refresh_rate(rate.hz);
	std::shared_lock lock(decoder_mutex);
	if (video_stream_description)
		video_stream_description->refresh_rate = rate.hz;
}

void scenes::stream::operator()(to_headset::stream_tab_change && tab)
{
	next_gui_status = tab.tab;
}

void scenes::stream::operator()(to_headset::timesync_query && query)
{
	network_session->send_stream(from_headset::timesync_response{
	        .query = query.query,
	        .response = instance.now(),
	});
}

void scenes::stream::operator()(audio_data && data)
{
	if (audio_handle)
		(*audio_handle)(std::move(data));
}

void scenes::stream::send_feedback(const wivrn::from_headset::feedback & feedback)
{
	if (not feedback.sent_to_decoder)
		health_counters.incomplete.fetch_add(1, std::memory_order_relaxed);
	try
	{
		network_session->send_control(wivrn::from_headset::feedback{feedback});
	}
	catch (std::exception & e)
	{
		spdlog::warn("Exception while sending feedback packet: {}", e.what());
	}
}

void scenes::stream::observe_decode_ms(float ms)
{
	// Same 32 logarithmic buckets as the server's health::log_histogram: 0.1 ms, growing 1.25x.
	size_t bucket = 0;
	if (ms >= 0.1f)
		bucket = std::min<size_t>(31, size_t(1 + std::floor(std::log(ms / 0.1f) / std::log(1.25f))));
	health_counters.decode_hist[bucket].fetch_add(1, std::memory_order_relaxed);
}

void scenes::stream::setup_runtime_counters()
{
	runtime_counters = {};
	if (not instance.has_extension(XR_META_PERFORMANCE_METRICS_EXTENSION_NAME))
		return;
	// The runtime's own list decides what is read: a path it does not list makes
	// xrQueryPerformanceMetricsCounterMETA fail, so the field stays unknown instead. The list is
	// logged once (greppable) because the extension's specification only says which paths "may"
	// exist on Meta devices.
	auto available = session.performance_metrics_counters();
	std::string names;
	for (const auto & p: available)
		names += (names.empty() ? "" : " ") + p;
	spdlog::info("XR_META_performance_metrics counters ({}): {}", available.size(), names);
	auto resolve = [&](const char * path) {
		return std::ranges::find(available, path) != available.end() ? instance.string_to_path(path) : XR_NULL_PATH;
	};
	runtime_counters.compositor_dropped = resolve("/perfmetrics_meta/compositor/dropped_frame_count");
	// No stale-frame counter is in the specification's list (revision 2): the first name is a
	// guess checked against the runtime's list; without it the frames-per-second counter, also
	// unlisted, gives stale = refresh rate - compositor rate; without either the field is unknown.
	runtime_counters.compositor_stale = resolve("/perfmetrics_meta/compositor/stale_frame_count");
	runtime_counters.compositor_fps = resolve("/perfmetrics_meta/compositor/frames_per_second");
	runtime_counters.compositor_gpu = resolve("/perfmetrics_meta/compositor/gpu_frametime");
	runtime_counters.app_gpu = resolve("/perfmetrics_meta/app/gpu_frametime");
	runtime_counters.app_m2p = resolve("/perfmetrics_meta/app/motion_to_photon_latency");
	runtime_counters.cpu_util = resolve("/perfmetrics_meta/device/cpu_utilization_average");
	runtime_counters.gpu_util = resolve("/perfmetrics_meta/device/gpu_utilization");
	runtime_counters.enabled = session.enable_performance_metrics();
	spdlog::info("XR_META_performance_metrics: {}", runtime_counters.enabled ? "enabled" : "not enabled");
}

void scenes::stream::send_telemetry(XrTime now)
{
	uint32_t total = 0;
	std::array<uint16_t, 32> hist;
	for (size_t i = 0; i < hist.size(); ++i)
	{
		hist[i] = health_counters.decode_hist[i].exchange(0, std::memory_order_relaxed);
		total += hist[i];
	}
	float decode_p95 = 0;
	if (total)
	{
		uint32_t target = (total * 95 + 99) / 100, seen = 0;
		for (size_t i = 0; i < hist.size(); ++i)
		{
			seen += hist[i];
			if (seen >= target)
			{
				decode_p95 = i == 0 ? 0.f : 0.1f * std::pow(1.25f, float(i - 1));
				break;
			}
		}
	}
	auto clamp16 = [](float v) { return uint16_t(std::clamp(v, 0.f, 65535.f)); };
	from_headset::telemetry t{
	        .frames_incomplete = health_counters.incomplete.exchange(0, std::memory_order_relaxed),
	        .frames_decoded = health_counters.decoded.exchange(0, std::memory_order_relaxed),
	        .frames_dropped_before_display = health_counters.dropped.exchange(0, std::memory_order_relaxed),
	        .frames_repeated = health_counters.repeated.exchange(0, std::memory_order_relaxed),
	        .decode_ms_p95_x10 = clamp16(decode_p95 * 10),
	        .gpu_ms_x10 = clamp16(compact_gpu_time * 1e4f),
	        .cpu_ms_x10 = clamp16(compact_cpu_time * 1e4f),
	        .display_hz_active_x10 = 0,
	        .thermal_status = 255,
	        .cpu_level = perf_level_cpu,
	        .gpu_level = perf_level_gpu,
	        .network_kind = 0,
	        .rssi_dbm = INT16_MIN,
	        .link_mbps = 0,
	        .compositor_dropped = 0xFFFF,
	        .compositor_stale = 0xFFFF,
	        .compositor_gpu_ms_x10 = 0xFFFF,
	        .app_gpu_ms_x10 = 0xFFFF,
	        .runtime_m2p_ms_x10 = 0xFFFF,
	        .cpu_util_pct = 255,
	        .gpu_util_pct = 255,
	};
	t.frames_received = t.frames_incomplete + t.frames_decoded;
	if (instance.has_extension(XR_FB_DISPLAY_REFRESH_RATE_EXTENSION_NAME))
		t.display_hz_active_x10 = clamp16(session.get_current_refresh_rate() * 10);
	t.thermal_status = device_thermal_status.load();
	t.network_kind = device_network_kind.load();
	t.rssi_dbm = device_rssi_dbm.load();
	t.link_mbps = device_link_mbps.load();
	if (runtime_counters.enabled)
	{
		// A counter is a float or an integer at the runtime's choice; 0xFFFF / 255 stay reserved.
		auto value = [&](XrPath p) -> std::optional<float> {
			auto c = session.query_performance_metrics(p);
			if (not c)
				return std::nullopt;
			return (c->counterFlags & XR_PERFORMANCE_METRICS_COUNTER_FLOAT_VALUE_VALID_BIT_META) ? c->floatValue : float(c->uintValue);
		};
		auto u16 = [&](XrPath p, float scale) -> uint16_t {
			auto v = value(p);
			return v ? uint16_t(std::clamp(std::round(*v * scale), 0.f, 65534.f)) : 0xFFFF;
		};
		auto pct = [&](XrPath p) -> uint8_t {
			auto v = value(p);
			return v ? uint8_t(std::clamp(std::round(*v), 0.f, 254.f)) : 255;
		};
		t.compositor_dropped = u16(runtime_counters.compositor_dropped, 1);
		t.compositor_gpu_ms_x10 = u16(runtime_counters.compositor_gpu, 10);
		t.app_gpu_ms_x10 = u16(runtime_counters.app_gpu, 10);
		t.runtime_m2p_ms_x10 = u16(runtime_counters.app_m2p, 10);
		t.cpu_util_pct = pct(runtime_counters.cpu_util);
		t.gpu_util_pct = pct(runtime_counters.gpu_util);
		if (runtime_counters.compositor_stale != XR_NULL_PATH)
			t.compositor_stale = u16(runtime_counters.compositor_stale, 1);
		else if (auto fps = value(runtime_counters.compositor_fps); fps and t.display_hz_active_x10)
			t.compositor_stale = uint16_t(std::clamp(std::round(float(t.display_hz_active_x10) / 10.f - *fps), 0.f, 65534.f));
	}
	try
	{
		network_session->send_control(std::move(t));
	}
	catch (std::exception & e)
	{
		spdlog::warn("Exception while sending telemetry: {}", e.what());
	}
}

void scenes::stream::operator()(to_headset::health && h)
{
	*last_health.lock() = std::move(h);
}

void scenes::stream::operator()(to_headset::application_list && l)
{
	apps(std::move(l));
}

void scenes::stream::operator()(to_headset::application_icon && icon)
{
	apps(std::move(icon));
}

void scenes::stream::operator()(to_headset::running_applications && apps)
{
	*running_applications.lock() = std::move(apps);
}

void scenes::stream::operator()(to_headset::app_data && packet)
{
	application::dispatch_app_data(std::move(packet));
}

void scenes::stream::start_application(std::string appid)
{
	network_session->send_control(wivrn::from_headset::start_app{
	        .app_id = std::move(appid),
	});
}
