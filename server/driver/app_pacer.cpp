/*
 * WiVRn VR streaming
 * Copyright (C) 2025  Patrick Nicolas <patricknicolas@laposte.net>
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

#include "app_pacer.h"
#include "configuration.h"

#include "util/u_debug.h"
#include "util/u_time.h"
#include "util/u_var.h"
#include "utils/method.h"
#include <algorithm>
#include <cassert>
#include <cmath>
#include <ranges>

// Default value is half the compositor margin, overridden by "app-margin-ms" in the configuration file
DEBUG_GET_ONCE_FLOAT_OPTION(min_margin_ms, "U_PACING_APP_MIN_MARGIN_MS", 1.5f)

namespace wivrn
{

class app_pacer : public u_pacing_app
{
	pacing_app_factory & parent;
	int64_t frame_id = 0;
	int64_t compositor_display_time = 0;
	int64_t last_display_time = 0;
	int64_t period = 10'000'000;

	std::mutex mutex; // locks cpu/draw/gpu time
	int64_t cpu_time = 0;
	int64_t gpu_time = 0;
	int64_t compositor_time = 0;

	u_var_draggable_f32 min_margin_ms;

	struct frame
	{
		int64_t frame_id;
		int64_t wake_up;
		int64_t delivered;
		int64_t predicted_display;
	};
	std::array<frame, 16> frames{};

	frame & get_frame(int64_t id)
	{
		assert(id >= 0);
		return frames[id % frames.size()];
	}

	// Half-rate lock-in (GRID #2834). A pipelined application (a browser) that is handed display
	// times two periods out delivers two periods after its wake-up, so the measured CPU time keeps
	// it on two periods long after the link hiccup that put it there; the application itself costs
	// a fraction of a period. Once it has delivered at a steady 2x or 3x the period for a second, one
	// frame per second is predicted one period out with no allowance for the CPU estimate. If that
	// frame arrives in time the CPU estimate is reset to what it measured and pacing continues at one
	// period; if it misses, the frame is shown where the two-period cadence would have shown it
	// anyway and the next probe waits another second.
	struct
	{
		int multiple = 0;         // 2 or 3 while a steady cadence window is open, 0 otherwise
		int64_t window_start = 0; // the delivery that opened the window
		uint32_t window_deliveries = 0;
		int64_t last_delivered = 0;
		int64_t last_probe = 0;
		int64_t probe_frame_id = 0; // probe in flight, 0 when none
	} half_rate;
	static constexpr int64_t half_rate_window_ns = U_TIME_1S_IN_NS;
	static constexpr int64_t half_rate_probe_interval_ns = U_TIME_1S_IN_NS;
	static constexpr double half_rate_tolerance = 0.05;

	int64_t margin_ns() const
	{
		return int64_t(U_TIME_1MS_IN_NS * min_margin_ms.val);
	}
	bool half_rate_probe_due(int64_t now_ns) const;
	void half_rate_track(int64_t when_ns);

public:
	using base_t = u_pacing_app;

	app_pacer(pacing_app_factory & parent) :
	        u_pacing_app{
	                .predict = method_pointer<&app_pacer::predict>,
	                .mark_point = method_pointer<&app_pacer::mark_point>,
	                .mark_discarded = method_pointer<&app_pacer::mark_discarded>,
	                .mark_delivered = method_pointer<&app_pacer::mark_delivered>,
	                .mark_gpu_done = method_pointer<&app_pacer::mark_gpu_done>,
	                .latched = method_pointer<&app_pacer::latched>,
	                .retired = method_pointer<&app_pacer::retired>,
	                .info = method_pointer<&app_pacer::info>,
	                .destroy = method_pointer<&app_pacer::destroy>,
	        },
	        parent(parent),
	        min_margin_ms{
	                .val = parent.min_margin_ms,
	                .step = 0.1,
	                .min = 0.5,
	                .max = 5.0,
	        }
	{
		// U variable tracking.
		u_var_add_root(this, "App timing info", true);
		u_var_add_draggable_f32(this, &min_margin_ms, "Minimum margin(ms)");
	}

	~app_pacer()
	{
		u_var_remove_root(this);
	}

	void predict(int64_t now_ns,
	             int64_t * out_frame_id,
	             int64_t * out_wake_up_time,
	             int64_t * out_predicted_display_time,
	             int64_t * out_predicted_display_period);
	void mark_point(int64_t frame_id, enum u_timing_point point, int64_t when_ns);
	void mark_discarded(int64_t frame_id, int64_t when_ns);
	void mark_delivered(int64_t frame_id, int64_t when_ns, int64_t display_time_ns);
	void mark_gpu_done(int64_t frame_id, int64_t when_ns);
	void latched(int64_t frame_id, int64_t when_ns, int64_t system_frame_id) {}
	void retired(int64_t frame_id, int64_t when_ns) {}
	void info(int64_t predicted_display_time_ns,
	          int64_t predicted_display_period_ns,
	          int64_t extra_ns);

	int64_t get_app_time()
	{
		std::lock_guard lock(mutex);
		return std::max(cpu_time, gpu_time);
	}

	std::pair<int64_t, int64_t> get_times()
	{
		std::lock_guard lock(mutex);
		return {cpu_time, gpu_time};
	}

	void destroy()
	{
		parent.remove_app(this);
		delete this;
	}
};

void app_pacer::predict(int64_t now_ns,
                        int64_t * out_frame_id,
                        int64_t * out_wake_up_time,
                        int64_t * out_predicted_display_time,
                        int64_t * out_predicted_display_period)
{
	*out_frame_id = ++frame_id;
	get_frame(frame_id) = {.frame_id = frame_id};

	// no need to lock, called in same thread as writes
	auto min_ready = now_ns + cpu_time + gpu_time + compositor_time;
	// The ideal display time: one frame after the last
	last_display_time += period;
	// Sync phase with compositor
	last_display_time = compositor_display_time + period * ((period / 2 + last_display_time - compositor_display_time) / period);

	// Half-rate probe: the next display slot, no allowance for the CPU estimate under test.
	if (half_rate_probe_due(now_ns) and last_display_time - compositor_time - margin_ns() > now_ns)
	{
		*out_wake_up_time = now_ns;
		*out_predicted_display_period = period;
		*out_predicted_display_time = last_display_time;
		half_rate.probe_frame_id = frame_id;
		half_rate.last_probe = now_ns;
		parent.half_rate_probe_count.fetch_add(1, std::memory_order_relaxed);
	}
	else if (cpu_time > period or gpu_time > period or (min_ready > last_display_time and min_ready < last_display_time + period))
	{
		// We are limited by app time, don't wait
		*out_wake_up_time = now_ns;
		*out_predicted_display_period = period; // FIXME: may be more than one frame
		while (last_display_time < min_ready)
			last_display_time += period;
		*out_predicted_display_time = last_display_time;
	}
	else
	{
		// Ensure display time is achievable
		while (last_display_time < min_ready)
			last_display_time += period;

		*out_predicted_display_time = last_display_time;
		*out_predicted_display_period = period;
		*out_wake_up_time = last_display_time - (cpu_time + gpu_time + compositor_time + margin_ns());
	}
	get_frame(frame_id).predicted_display = *out_predicted_display_time;
}

void app_pacer::mark_point(int64_t frame_id, enum u_timing_point point, int64_t when_ns)
{
	auto & frame = get_frame(frame_id);
	if (frame.frame_id != frame_id)
		return;
	switch (point)
	{
		case U_TIMING_POINT_WAKE_UP:
			frame.wake_up = when_ns;
			break;
		case U_TIMING_POINT_BEGIN:
		case U_TIMING_POINT_SUBMIT_BEGIN:
		case U_TIMING_POINT_SUBMIT_END:
			break;
	}
}

bool app_pacer::half_rate_probe_due(int64_t now_ns) const
{
	if (half_rate.multiple == 0 or half_rate.probe_frame_id or half_rate.window_deliveries < 2)
		return false;
	int64_t span = half_rate.last_delivered - half_rate.window_start;
	if (span < half_rate_window_ns or now_ns - half_rate.last_probe < half_rate_probe_interval_ns)
		return false;
	double expected = double(half_rate.multiple * period);
	if (std::abs(double(span) / (half_rate.window_deliveries - 1) - expected) > half_rate_tolerance * expected)
		return false;
	// The cost the compositor measures directly has to fit in one period with the margin; the
	// CPU estimate is the thing in doubt, the probe is what tests it.
	return gpu_time + compositor_time + margin_ns() <= period;
}

void app_pacer::half_rate_track(int64_t when_ns)
{
	if (half_rate.last_delivered)
	{
		int64_t interval = when_ns - half_rate.last_delivered;
		int multiple = int((interval + period / 2) / period);
		// Late frames stay in the window: in the lock-in state a few percent of the frames miss
		// the deadline by a fraction of a millisecond without changing the cadence.
		if ((multiple != 2 and multiple != 3) or std::abs(interval - multiple * period) > period / 4)
			half_rate.multiple = 0;
		else if (multiple == half_rate.multiple)
			++half_rate.window_deliveries;
		else
		{
			half_rate.multiple = multiple;
			half_rate.window_start = half_rate.last_delivered;
			half_rate.window_deliveries = 2;
		}
	}
	half_rate.last_delivered = when_ns;
}

void app_pacer::mark_delivered(int64_t frame_id, int64_t when_ns, int64_t display_time_ns)
{
	auto & frame = get_frame(frame_id);
	if (frame.frame_id != frame_id)
		return;
	frame.delivered = when_ns;
	bool late = frame.predicted_display and when_ns > frame.predicted_display - compositor_time;
	// Health counters: one relaxed increment per frame; the monitor thread reads them once per second.
	parent.delivered_count.fetch_add(1, std::memory_order_relaxed);
	if (late)
		parent.late_count.fetch_add(1, std::memory_order_relaxed);
	if (frame_id == half_rate.probe_frame_id)
	{
		half_rate.probe_frame_id = 0;
		if (not late and frame.wake_up)
		{
			// Answered in time: the two-period CPU estimate was pipeline latency, not work.
			std::lock_guard lock(mutex);
			cpu_time = when_ns - frame.wake_up;
			half_rate.multiple = 0;
			parent.half_rate_recovery_count.fetch_add(1, std::memory_order_relaxed);
		}
	}
	half_rate_track(when_ns);
}

void app_pacer::mark_discarded(int64_t frame_id, int64_t when_ns)
{
	auto & frame = get_frame(frame_id);
	if (frame.frame_id != frame_id)
		return;
	frame.delivered = when_ns;
	// A discarded probe is a missed one; the cadence window carries on.
	if (frame_id == half_rate.probe_frame_id)
		half_rate.probe_frame_id = 0;
}

void app_pacer::info(int64_t predicted_display_time_ns,
                     int64_t predicted_display_period_ns,
                     int64_t extra_ns)
{
	compositor_display_time = predicted_display_time_ns;
	period = predicted_display_period_ns;
	compositor_time = std::max<int64_t>(0, extra_ns);
}

int64_t lerp0(int64_t a, int64_t b, double t)
{
	if (a == 0)
		return b;
	return std::lerp(a, b, t);
}

void app_pacer::mark_gpu_done(int64_t frame_id, int64_t when_ns)
{
	auto & frame = get_frame(frame_id);
	if (frame.frame_id != frame_id or not(frame.wake_up and frame.delivered))
		return;

	std::lock_guard lock(mutex);
	cpu_time = lerp0(cpu_time, std::min(3 * period, frame.delivered - frame.wake_up), 0.1);
	gpu_time = lerp0(gpu_time, std::min(3 * period, when_ns - frame.delivered), 0.1);
}

pacing_app_factory::pacing_app_factory() :
        pacing_app_factory(configuration().app_margin_ms.value_or(debug_get_float_option_min_margin_ms()))
{
}

pacing_app_factory::pacing_app_factory(float min_margin_ms) :
        u_pacing_app_factory{
                .create = method_pointer<&pacing_app_factory::create>,
                .destroy = method_pointer<&pacing_app_factory::destroy>,
        },
        min_margin_ms(min_margin_ms)
{
}

void pacing_app_factory::remove_app(app_pacer * app)
{
	std::lock_guard lock(mutex);
	std::erase(app_pacers, app);
}

xrt_result_t pacing_app_factory::create(struct u_pacing_app ** out_upa)
{
	std::lock_guard lock(mutex);
	*out_upa = app_pacers.emplace_back(new app_pacer(*this));
	return XRT_SUCCESS;
}

void pacing_app_factory::destroy()
{
}

int64_t pacing_app_factory::get_frame_time()
{
	std::lock_guard lock(mutex);
	if (app_pacers.empty())
		return 1;
	return std::ranges::max(std::ranges::transform_view(app_pacers, [](auto * pacer) { return pacer->get_app_time(); }));
}

pacing_app_factory::health_counters pacing_app_factory::take_health_counters()
{
	health_counters c{
	        .apps = 0,
	        .delivered = delivered_count.exchange(0, std::memory_order_relaxed),
	        .late = late_count.exchange(0, std::memory_order_relaxed),
	        .cpu_ns = 0,
	        .gpu_ns = 0,
	        .half_rate_probes = half_rate_probe_count.exchange(0, std::memory_order_relaxed),
	        .half_rate_recoveries = half_rate_recovery_count.exchange(0, std::memory_order_relaxed),
	};
	std::lock_guard lock(mutex);
	c.apps = uint32_t(app_pacers.size());
	for (auto * pacer: app_pacers)
	{
		auto [cpu, gpu] = pacer->get_times();
		c.cpu_ns = std::max(c.cpu_ns, cpu);
		c.gpu_ns = std::max(c.gpu_ns, gpu);
	}
	return c;
}

} // namespace wivrn
