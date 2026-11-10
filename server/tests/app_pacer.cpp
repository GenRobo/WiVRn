// Fake-clock harness over the application pacer (GRID #2834): the half-rate lock-in seen with a
// browser on 2026-10-30 and the probe that recovers from it.
// Usage: wivrn-app-pacer-test
//
// The application model is the one the flight record supports: the browser delivers a frame a
// fixed lead before the display time it was handed (5 ms here, the pipeline runs one slot behind)
// and never sooner than its own cost after wake-up. Handed display times one period apart it
// measures ~1.5 ms; handed two periods it measures ~16 ms, which the pacer's `cpu_time > period`
// branch turns into two periods again: the bistable state.
#include "driver/app_pacer.h"

#include <algorithm>
#include <cstdint>
#include <cstdlib>
#include <functional>
#include <iostream>
#include <vector>

using namespace wivrn;

namespace
{
int failures = 0;

#define CHECK(cond)                                                                               \
	do                                                                                        \
	{                                                                                         \
		if (!(cond))                                                                      \
		{                                                                                 \
			std::cerr << __FILE__ << ":" << __LINE__ << ": FAIL " #cond << std::endl; \
			++failures;                                                               \
		}                                                                                 \
	} while (0)

constexpr int64_t ms = 1'000'000;
constexpr int64_t period = 8'333'333; // 120 Hz
constexpr int64_t compositor_time = 3 * ms;
constexpr int64_t gpu_cost = ms / 7;     // 0.14 ms, the browser's measured GPU time
constexpr int64_t app_overhead = ms / 5; // xrEndFrame return to the next xrWaitFrame

struct delivered_frame
{
	int64_t wake_up;
	int64_t predicted_display;
	int64_t delivered;
	int64_t displayed; // first slot the compositor can show it in
};

/// One pacer, one application, a fake monotonic clock. `deliver(wake, predicted_display)` is the
/// application model; the harness runs the OpenXR frame loop the way Monado's multi-compositor
/// drives the pacer: info, predict, wake-up, deliver, GPU done.
struct harness
{
	pacing_app_factory factory{1.5f};
	u_pacing_app * upa = nullptr;
	int64_t now = 1'000 * ms;
	std::vector<delivered_frame> frames;

	harness()
	{
		factory.create(&upa);
	}
	~harness()
	{
		upa->destroy(upa);
	}

	static int64_t slot_after(int64_t t)
	{
		return (t / period + 1) * period;
	}

	void run_until(int64_t end, const std::function<int64_t(int64_t wake, int64_t display)> & deliver)
	{
		while (now < end)
		{
			// The compositor's own frame, as its main loop reports it.
			upa->info(upa, slot_after(now + compositor_time), period, compositor_time);
			int64_t id, wake, display, display_period;
			upa->predict(upa, now, &id, &wake, &display, &display_period);
			now = std::max(now, wake);
			upa->mark_point(upa, id, U_TIMING_POINT_WAKE_UP, now);
			upa->mark_point(upa, id, U_TIMING_POINT_BEGIN, now);
			int64_t d = std::max(now, deliver(now, display));
			upa->mark_delivered(upa, id, d, display);
			upa->mark_gpu_done(upa, id, d + gpu_cost);
			frames.push_back({now, display, d, slot_after(d + compositor_time)});
			now = d + app_overhead;
		}
	}

	/// Frames delivered in [from, to).
	auto window(int64_t from, int64_t to) const
	{
		std::vector<delivered_frame> out;
		for (auto & f: frames)
			if (f.delivered >= from and f.delivered < to)
				out.push_back(f);
		return out;
	}
};

/// The browser: a frame arrives `lead` before the display time it was handed, never sooner than
/// `cost` after wake-up.
auto browser(int64_t cost, int64_t lead = 5 * ms)
{
	return [=](int64_t wake, int64_t display) { return std::max(wake + cost, display - lead); };
}

int displayed_multiple(const delivered_frame & a, const delivered_frame & b)
{
	return int((b.displayed - a.displayed + period / 2) / period);
}

void test_lock_in_recovers()
{
	harness h;
	// One second at 120 Hz: a frame per period, nothing late.
	h.run_until(h.now + 1000 * ms, browser(ms * 3 / 2));
	auto steady = h.window(h.now - 500 * ms, h.now);
	CHECK(steady.size() >= 58);
	for (size_t i = 1; i < steady.size(); ++i)
		CHECK(displayed_multiple(steady[i - 1], steady[i]) == 1);
	auto c = h.factory.take_health_counters();
	CHECK(c.delivered >= 118);
	CHECK(c.late == 0);
	CHECK(c.half_rate_probes == 0);
	// The link hiccup: the browser stalls for five frames.
	int64_t hiccup = h.now;
	int stalled = 0;
	h.run_until(h.now + 600 * ms, [&](int64_t wake, int64_t display) {
		if (stalled < 5)
		{
			++stalled;
			return wake + 25 * ms;
		}
		return browser(ms * 3 / 2)(wake, display);
	});
	// Locked in: every frame is handed, and shown at, two periods, and the pacer measures more
	// than a period of CPU time (the flight record showed 15 ms at 120 Hz).
	auto locked = h.window(hiccup + 200 * ms, h.now);
	CHECK(locked.size() >= 20);
	for (size_t i = 1; i < locked.size(); ++i)
		CHECK(displayed_multiple(locked[i - 1], locked[i]) == 2);
	for (auto & f: locked)
		CHECK(f.delivered - f.wake_up > period); // what the pacer measures as CPU time

	// Two seconds later the probe has put it back to one period.
	h.run_until(hiccup + 2000 * ms, browser(ms * 3 / 2));
	c = h.factory.take_health_counters();
	CHECK(c.half_rate_probes >= 1);
	CHECK(c.half_rate_probes <= 2);
	CHECK(c.half_rate_recoveries == 1);
	int64_t recovered_at = 0;
	for (size_t i = 1; i < h.frames.size(); ++i)
		if (h.frames[i].delivered > hiccup + 200 * ms and recovered_at == 0 and displayed_multiple(h.frames[i - 1], h.frames[i]) == 1)
			recovered_at = h.frames[i].delivered;
	CHECK(recovered_at != 0);
	CHECK(recovered_at - hiccup < 2000 * ms);
	auto after = h.window(recovered_at, h.now);
	CHECK(after.size() >= 50);
	for (size_t i = 1; i < after.size(); ++i)
		CHECK(displayed_multiple(after[i - 1], after[i]) == 1);
	// And stays there.
	h.run_until(h.now + 3000 * ms, browser(ms * 3 / 2));
	c = h.factory.take_health_counters();
	CHECK(c.delivered >= 355);
	CHECK(c.half_rate_probes == 0);
	CHECK(c.late == 0);
}

void test_slow_app_stays_at_two_periods()
{
	harness h;
	// A display-locked application whose frame costs 1.3 periods: it belongs at two periods.
	auto slow = browser(period * 13 / 10);
	h.run_until(h.now + 1000 * ms, slow);
	int64_t start = h.now;
	(void)h.factory.take_health_counters();
	h.run_until(h.now + 5000 * ms, slow);
	auto c = h.factory.take_health_counters();
	CHECK(c.half_rate_probes >= 3);      // the cadence looks like the lock-in, so it is probed...
	CHECK(c.half_rate_probes <= 5);      // ...at most once per second...
	CHECK(c.half_rate_recoveries == 0);  // ...and never answered in time.
	CHECK(c.late == c.half_rate_probes); // the only late frames are the probes
	// No oscillation: shown at two periods throughout, probes included.
	auto shown = h.window(start, h.now);
	CHECK(shown.size() >= 295);
	CHECK(shown.size() <= 305);
	for (size_t i = 1; i < shown.size(); ++i)
		CHECK(displayed_multiple(shown[i - 1], shown[i]) == 2);
	// Every non-probe frame was handed two periods.
	int one_period_predictions = 0;
	for (size_t i = 1; i < shown.size(); ++i)
		if (shown[i].predicted_display - shown[i - 1].predicted_display < period * 3 / 2)
			++one_period_predictions;
	CHECK(one_period_predictions == int(c.half_rate_probes));
}

void test_genuinely_slow_cpu_app_is_not_probed()
{
	harness h;
	// Flat out at 1.3 periods per frame: not a sub-multiple of the display rate, nothing to probe.
	h.run_until(h.now + 3000 * ms, [](int64_t wake, int64_t) { return wake + period * 13 / 10; });
	auto c = h.factory.take_health_counters();
	CHECK(c.delivered >= 250);
	CHECK(c.half_rate_probes == 0);
	CHECK(c.half_rate_recoveries == 0);
}

void test_gpu_bound_app_is_not_probed()
{
	harness h;
	// Display-locked, cheap on the CPU, but the compositor measures 1.3 periods of GPU time:
	// the measured cost does not fit in one period, no probe.
	int64_t id_gpu = period * 13 / 10;
	while (h.now < 1000 * ms + 4000 * ms)
	{
		h.upa->info(h.upa, harness::slot_after(h.now + compositor_time), period, compositor_time);
		int64_t id, wake, display, display_period;
		h.upa->predict(h.upa, h.now, &id, &wake, &display, &display_period);
		h.now = std::max(h.now, wake);
		h.upa->mark_point(h.upa, id, U_TIMING_POINT_WAKE_UP, h.now);
		int64_t d = std::max(h.now + ms, display - 5 * ms);
		h.upa->mark_delivered(h.upa, id, d, display);
		h.upa->mark_gpu_done(h.upa, id, d + id_gpu);
		h.frames.push_back({h.now, display, d, harness::slot_after(d + compositor_time)});
		h.now = d + app_overhead;
	}
	auto c = h.factory.take_health_counters();
	CHECK(c.delivered >= 100);
	CHECK(c.gpu_ns > period);
	CHECK(c.half_rate_probes == 0);
}
} // namespace

int main()
{
	test_lock_in_recovers();
	test_slow_app_stays_at_two_periods();
	test_genuinely_slow_cpu_app_is_not_probed();
	test_gpu_bound_app_is_not_probed();
	if (failures)
	{
		std::cerr << failures << " check(s) failed" << std::endl;
		return EXIT_FAILURE;
	}
	std::cout << "app_pacer: all checks passed" << std::endl;
	return EXIT_SUCCESS;
}
