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

#pragma once

#include "util/u_pacing.h"

#include <atomic>
#include <cstdint>
#include <mutex>
#include <vector>

namespace wivrn
{

class wivrn_session;
class app_pacer;

class pacing_app_factory : public u_pacing_app_factory
{
	friend class app_pacer;
	std::mutex mutex;
	std::vector<app_pacer *> app_pacers;
	const float min_margin_ms;
	void remove_app(app_pacer *);

public:
	using base_t = u_pacing_app_factory;

	pacing_app_factory();
	xrt_result_t create(struct u_pacing_app ** out_upa);
	void destroy();

	int64_t get_frame_time();

	/// Counters for the stream health monitor (GRID #2834), reset by each call: frames the
	/// applications delivered since the last call, how many were delivered after the compositor
	/// deadline, and the slowest application's smoothed CPU and GPU time in nanoseconds.
	struct health_counters
	{
		uint32_t apps;
		uint32_t delivered;
		uint32_t late;
		int64_t cpu_ns;
		int64_t gpu_ns;
	};
	health_counters take_health_counters();

private:
	std::atomic<uint32_t> delivered_count{0};
	std::atomic<uint32_t> late_count{0};
};

} // namespace wivrn
