/*
 * WiVRn VR streaming
 * Copyright (C) 2026  General Robotics
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

#include "configuration.h"
#include "wivrn_sockets.h"

#include <atomic>
#include <cstdint>
#include <deque>
#include <mutex>
#include <stop_token>
#include <string>
#include <thread>
#include <vector>

namespace wivrn
{
class wivrn_session;

// Relays one application data channel between the headset and a TCP sink ("app-data" in the
// configuration). While resumed, a thread connects to the sink, retrying every 2 s, forwards what
// the sink writes to the headset as to_headset::app_data and what the headset sends to the sink.
// Without a connected sink, headset data is dropped and counted.
class app_data_relay
{
	wivrn_session & session;
	const configuration::app_data_sink sink;

	// Headset to sink queue, drained by the relay thread. Bounded: a stalled sink must not
	// stall the headset connection.
	static constexpr size_t max_queued_bytes = 16 * 1024 * 1024;
	std::mutex mutex;
	std::deque<std::vector<uint8_t>> queue;
	size_t queued_bytes = 0;
	std::atomic<bool> connected = false;

	// Wakes the relay thread when the queue is filled (both ends of a datagram pair)
	fd_base wake_sender;
	fd_base wake_receiver;

	std::atomic<uint64_t> bytes_to_headset = 0;
	std::atomic<uint64_t> bytes_to_sink = 0;
	std::atomic<uint64_t> bytes_dropped = 0;

	std::jthread thread;

	void run(std::stop_token stop);
	TCP connect(std::stop_token stop);
	void serve(std::stop_token stop, TCP & tcp);
	bool flush_queue(TCP & tcp);
	void drop_queue();
	void log_counters();

public:
	app_data_relay(wivrn_session & session, configuration::app_data_sink sink);
	app_data_relay(const app_data_relay &) = delete;
	app_data_relay & operator=(const app_data_relay &) = delete;
	~app_data_relay();

	const std::string & channel() const
	{
		return sink.channel;
	}

	// Connect to the sink (and keep reconnecting) until pause() is called
	void resume();
	// Disconnect from the sink, headset data is dropped until resume()
	void pause();

	// Called by the session's network thread with data from the headset
	void from_headset(std::vector<uint8_t> && data);
};
} // namespace wivrn
