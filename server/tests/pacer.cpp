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

#include "compositor/pacer.h"
#include "driver/clock_offset.h"

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <iostream>
#include <stdexcept>
#include <thread>

namespace
{
constexpr int64_t ms = 1'000'000;
constexpr int64_t slow = 20 * ms;
constexpr int64_t fast = 5 * ms;

struct prediction
{
	int64_t frame;
	int64_t wake;
	int64_t present;
	int64_t slop;
	int64_t display;
};

prediction predict(wivrn::pacer & pacer)
{
	prediction result;
	pacer.predict(result.frame, result.wake, result.present, result.slop, result.display);
	return result;
}

void feedback(wivrn::pacer & pacer, const prediction & prediction, int64_t latency)
{
	wivrn::from_headset::feedback feedback{};
	feedback.frame_index = prediction.frame;
	feedback.received_from_decoder = prediction.present + latency;
	feedback.blitted = std::max(prediction.display, feedback.received_from_decoder + ms);
	feedback.times_displayed = 1;
	pacer.on_feedback(feedback, wivrn::clock_offset{.stable = true});

	// Let the asynchronous estimator consume each completed block. No assertion
	// depends on its exact completion time; later predictions observe its output.
	if (prediction.frame % 100 == 0)
		std::this_thread::sleep_for(std::chrono::milliseconds(20));
}

void require(bool condition, const char * message)
{
	if (not condition)
		throw std::runtime_error(message);
}

void continuous_history(wivrn::pacer & pacer)
{
	bool saw_slow_estimate = false;
	for (int i = 0; i < 1200; ++i)
	{
		auto p = predict(pacer);
		// With no displayed timestamp and a zero client margin, this difference
		// is the estimated present-to-decode latency, independent of clock phase.
		auto estimate = p.display - p.present;
		if (estimate == slow)
			saw_slow_estimate = true;
		if (saw_slow_estimate and estimate != slow)
		{
			std::cerr << "frame " << p.frame << ": retained latency fell from "
			          << slow / ms << " to " << estimate / ms << " ms\n";
			throw std::runtime_error("recent slow frames vanished from pacing history");
		}

		// Alternate slow/fast blocks at estimator boundaries. Both belong to the
		// same recent history: half the samples remain slow, so p99.5 stays slow.
		auto latency = p.frame > 0 and ((p.frame - 1) / 100) % 2 == 0 ? slow : fast;
		feedback(pacer, p, latency);
	}
	require(saw_slow_estimate, "estimator did not consume the slow feedback");
	std::cout << "PASS: alternating feedback blocks retain one continuous history\n";
}

void reset_history(wivrn::pacer & pacer)
{
	pacer.reset();
	bool saw_fresh_estimate = false;
	for (int i = 0; i < 700; ++i)
	{
		auto p = predict(pacer);
		auto estimate = p.display - p.present;
		if (estimate == fast)
			saw_fresh_estimate = true;
		if (saw_fresh_estimate)
			require(estimate == fast, "pre-reset latency resurfaced in pacing history");
		feedback(pacer, p, fast);
	}
	require(saw_fresh_estimate, "estimator did not consume post-reset feedback");
	std::cout << "PASS: reset cannot resurrect a previous session's timing samples\n";
}
} // namespace

int main()
{
#ifdef _WIN32
	_putenv_s("WIVRN_CLIENT_MARGIN_MS", "0");
#else
	setenv("WIVRN_CLIENT_MARGIN_MS", "0", 1);
#endif
	try
	{
		wivrn::pacer pacer(8'333'333);
		continuous_history(pacer);
		reset_history(pacer);
	}
	catch (const std::exception & error)
	{
		std::cerr << "FAIL: " << error.what() << '\n';
		return 1;
	}
}
