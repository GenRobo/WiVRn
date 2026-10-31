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

// Include first because of incompatibility between Eigen and X includes
#include "driver/wivrn_session.h"

#include "video_encoder.h"

#include "encoder_settings.h"
#include "os/os_time.h"
#include "utils/wivrn_trace.h"
#include "wivrn_config.h"

#include <atomic>
#include <cinttypes>
#include <string>

#include "util/u_logging.h"

#if WIVRN_USE_NVENC
#include "video_encoder_nvenc.h"
#endif
#if WIVRN_USE_VAAPI
#include "ffmpeg/video_encoder_va.h"
#endif
#if WIVRN_USE_X264
#include "video_encoder_x264.h"
#endif
#if WIVRN_USE_VULKAN_ENCODE
#include "video_encoder_vulkan_h264.h"
#include "video_encoder_vulkan_h265.h"
#endif
#include "video_encoder_raw.h"

namespace wivrn
{

namespace
{
// Per-stream pipeline counters, logged every 5 s.
struct stream_stats
{
	std::atomic<uint64_t> presented, skipped, encode_calls, encoded, empty, bytes, shards;
};
stream_stats stats[3];
std::atomic<int64_t> next_stats_log_ns = 0;
// Health monitor view of the same counters; separate so the five-second log keeps its own windows.
struct health_stats
{
	std::atomic<uint64_t> presented, skipped, bytes, shards;
} health_totals;

void maybe_log_stats()
{
	int64_t now = os_monotonic_get_ns();
	int64_t next = next_stats_log_ns.load();
	if (now < next or not next_stats_log_ns.compare_exchange_strong(next, now + 5'000'000'000))
		return;
	if (next == 0)
		return; // first call only arms the timer
	for (int i = 0; i < 3; ++i)
	{
		auto & s = stats[i];
		uint64_t presented = s.presented.exchange(0), skipped = s.skipped.exchange(0), calls = s.encode_calls.exchange(0),
		         encoded = s.encoded.exchange(0), empty = s.empty.exchange(0), bytes = s.bytes.exchange(0), shards = s.shards.exchange(0);
		if (presented or calls or bytes)
			U_LOG_I("encoder stream %d (5 s): presented %" PRIu64 ", skipped (waiting for headset ack) %" PRIu64 ", encode calls %" PRIu64
			        ", frames produced %" PRIu64 ", empty %" PRIu64 ", sent %" PRIu64 " shards / %.2f Mbit/s",
			        i, presented, skipped, calls, encoded, empty, shards, bytes * 8 / 5e6);
	}
}
} // namespace

video_encoder::health_counters video_encoder::take_health_counters()
{
	return {
	        .presented = health_totals.presented.exchange(0, std::memory_order_relaxed),
	        .skipped = health_totals.skipped.exchange(0, std::memory_order_relaxed),
	        .bytes = health_totals.bytes.exchange(0, std::memory_order_relaxed),
	        .shards = health_totals.shards.exchange(0, std::memory_order_relaxed),
	};
}

video_encoder::sender::sender() :
        thread([this](std::stop_token t) {
	        while (not t.stop_requested())
	        {
		        data * d = nullptr;
		        {
			        std::unique_lock lock(mutex);
			        if (pending.empty())
				        cv.wait_for(lock, std::chrono::milliseconds(100));
			        else
				        d = &pending.front();
		        }
		        if (d and not d->span.empty())
		        {
			        d->encoder->SendData(d->span, true, d->prefer_control);
			        std::unique_lock lock(mutex);
			        pending.pop_front();
			        cv.notify_all();
		        }
	        }
	        std::unique_lock lock(mutex);
	        pending.clear();
	        cv.notify_all();
        })
{
}

void video_encoder::sender::push(data && d)
{
	std::unique_lock lock(mutex);
	pending.push_back(std::move(d));
	cv.notify_all();
}

void video_encoder::sender::wait_idle(video_encoder * encoder)
{
	std::unique_lock lock(mutex);
	while (std::ranges::any_of(pending, [=](auto & data) { return data.encoder == encoder; }))
		cv.wait_for(lock, std::chrono::milliseconds(100));
}

std::shared_ptr<video_encoder::sender> video_encoder::sender::get()
{
	static std::weak_ptr<video_encoder::sender> instance;
	static std::mutex m;
	std::unique_lock lock(m);
	auto s = instance.lock();
	if (s)
		return s;
	s.reset(new video_encoder::sender());
	instance = s;
	return s;
}

std::unique_ptr<video_encoder> video_encoder::create(
        wivrn::vk_bundle & wivrn_vk,
        const encoder_settings & settings,
        uint8_t stream_idx)
{
	using namespace std::string_literals;
	std::unique_ptr<video_encoder> res;
	if (settings.encoder_name == encoder_vulkan)
	{
#if WIVRN_USE_VULKAN_ENCODE
		switch (settings.codec)
		{
			case video_codec::h264:
				res = video_encoder_vulkan_h264::create(wivrn_vk, settings, stream_idx);
				break;
			case video_codec::h265:
				res = video_encoder_vulkan_h265::create(wivrn_vk, settings, stream_idx);
				break;
			case video_codec::av1:
				throw std::runtime_error("av1 not supported for vulkan video encode");
			case video_codec::raw:
				throw std::runtime_error("raw codec only supported on raw encoder");
		}
#else
		throw std::runtime_error("Vulkan video encode not enabled");
#endif
	}
	if (settings.encoder_name == encoder_x264)
	{
#if WIVRN_USE_X264
		res = std::make_unique<video_encoder_x264>(wivrn_vk, settings, stream_idx);
#else
		throw std::runtime_error("x264 encoder not enabled");
#endif
	}
	if (settings.encoder_name == encoder_nvenc)
	{
#if WIVRN_USE_NVENC
		res = std::make_unique<video_encoder_nvenc>(wivrn_vk, settings, stream_idx);
#else
		throw std::runtime_error("nvenc support not enabled");
#endif
	}
	if (settings.encoder_name == encoder_vaapi)
	{
#if WIVRN_USE_VAAPI
		res = std::make_unique<video_encoder_va>(wivrn_vk, settings, stream_idx);
#else
		throw std::runtime_error("vaapi support not enabled");
#endif
	}

	if (settings.encoder_name == encoder_raw)
	{
		res = std::make_unique<video_encoder_raw>(wivrn_vk, settings, stream_idx);
	}

	if (not res)
		throw std::runtime_error("Failed to create encoder " + settings.encoder_name);

	auto wivrn_dump_video = std::getenv("WIVRN_DUMP_VIDEO");
	if (wivrn_dump_video)
	{
		std::string file(wivrn_dump_video);
		file += "-" + std::to_string(stream_idx);
		switch (settings.codec)
		{
			case h264:
				file += ".h264";
				break;
			case h265:
				file += ".h265";
				break;
			case av1:
				file += ".av1";
				break;
			case raw:
				file += ".yuv";
				break;
		}
		res->video_dump.open(file);
	}
	return res;
}

video_encoder::video_encoder(vk_bundle & vk,
                             uint8_t stream_idx,
                             uint32_t target_queue,
                             const encoder_settings & settings,
                             std::unique_ptr<idr_handler> idr,
                             bool async_send) :
        stream_idx(stream_idx),
        target_queue(target_queue),
        need_transfer(not vk.optimal_transfer(vk.queue.family_index, target_queue)),
        bitrate_multiplier(settings.bitrate_multiplier),
        shared_sender(async_send ? sender::get() : nullptr),
        idr(std::move(idr)),
        extent{
                .width = settings.width,
                .height = settings.height,
        }
{
	assert(this->idr);
}

video_encoder::~video_encoder()
{
	if (shared_sender)
		shared_sender->wait_idle(this);
}

void video_encoder::on_feedback(const from_headset::feedback & feedback)
{
	assert(feedback.stream_index == stream_idx);
	static std::atomic<int> logged[3];
	if (logged[stream_idx % 3].fetch_add(1) < 5)
		U_LOG_I("feedback stream %d frame %" PRIu64 ": received_last_packet %s, sent_to_decoder %s, displayed %s",
		        int(stream_idx), feedback.frame_index,
		        feedback.received_last_packet ? "yes" : "no", feedback.sent_to_decoder ? "yes" : "no", feedback.displayed ? "yes" : "no");
	idr->on_feedback(feedback);
}

void video_encoder::reset()
{
	idr->reset();
}

void video_encoder::set_bitrate(uint32_t bitrate_bps)
{
	pending_bitrate = bitrate_bps * bitrate_multiplier;
}

void video_encoder::set_framerate(float framerate)
{
	pending_framerate = framerate;
}

void video_encoder::present_image(vk::Image y_cbcr, vk::SemaphoreSubmitInfo info, uint64_t frame_index)
{
	wivrn::trace::scope trace_present(wivrn::trace::cpu_track::encoder, stream_idx, frame_index, "present_image");
	// Wait for encoder to be done
	present_slot = (present_slot + 1) % num_slots;
	state[present_slot].wait(busy);
	stats[stream_idx % 3].presented++;
	health_totals.presented.fetch_add(1, std::memory_order_relaxed);
	if (idr->should_skip(frame_index))
	{
		stats[stream_idx % 3].skipped++;
		health_totals.skipped.fetch_add(1, std::memory_order_relaxed);
		// The reference-tracking encoders (Vulkan Video) stop encoding until the headset acknowledges a
		// frame (feedback.sent_to_decoder); make a stall visible instead of silent.
		if (++consecutive_skips == 72)
			U_LOG_W("encoder stream %d: no headset acknowledgement for 72 frames, encoding paused until feedback arrives (frame %" PRIu64 ")",
			        int(stream_idx), frame_index);
		state[present_slot] = skip;
		return;
	}
	if (consecutive_skips >= 72)
		U_LOG_I("encoder stream %d: resumed after %d skipped frames", int(stream_idx), consecutive_skips);
	consecutive_skips = 0;
	state[present_slot] = busy;
	return present_image(y_cbcr, info, present_slot, frame_index);
}

void video_encoder::encode(wivrn_session & cnx,
                           const to_headset::video_stream_data_shard::view_info_t & view_info,
                           uint64_t frame_index)
{
	encode_slot = (encode_slot + 1) % num_slots;

	struct idle_setter
	{
		std::atomic_unsigned_lock_free & state;
		~idle_setter()
		{
			state = idle;
			state.notify_all();
		}
	};
	idle_setter i{state[encode_slot]};

	if (state[encode_slot] == skip)
		return;

	if (shared_sender)
		shared_sender->wait_idle(this);
	this->cnx = &cnx;
	clock = cnx.get_offset();

	wivrn::trace::scope trace_encode(wivrn::trace::cpu_track::encoder, stream_idx, frame_index, "encode");
	auto encode_begin = os_monotonic_get_ns();
	timing_info = {
	        .encode_begin = clock.to_headset(encode_begin),
	};

	// Prepare the video shard template
	shard.stream_item_idx = stream_idx;
	shard.frame_idx = frame_index;
	shard.shard_idx = 0;
	shard.view_info = view_info;
	shard.timing_info.reset();

	stats[stream_idx % 3].encode_calls++;
	auto data = encode(encode_slot, frame_index);
	if (data)
	{
		stats[stream_idx % 3].encoded++;
		timing_info.encode_end = clock.to_headset(os_monotonic_get_ns());
		assert(shared_sender);
		shared_sender->push(std::move(*data));
	}
	else
		stats[stream_idx % 3].empty++;
	maybe_log_stats();
}

void video_encoder::SendData(std::span<uint8_t> data, bool end_of_frame, bool control)
{
	std::lock_guard lock(mutex);
	if (shard.shard_idx == 0)
	{
		// One SendData call per NAL; span the whole frame, not each call.
		wivrn::trace::cpu_begin(wivrn::trace::cpu_track::network, stream_idx, shard.frame_idx, "SendData");
		timing_info.send_begin = clock.to_headset(os_monotonic_get_ns());
	}
	if (end_of_frame)
	{
		timing_info.send_end = clock.to_headset(os_monotonic_get_ns());
		if (not timing_info.encode_end)
			timing_info.encode_end = timing_info.send_end;
	}
	if (video_dump)
		video_dump.write((char *)data.data(), data.size());
	stats[stream_idx % 3].bytes += data.size();
	health_totals.bytes.fetch_add(data.size(), std::memory_order_relaxed);

	ssize_t max_payload_size = (cnx->has_stream() and not control) ? to_headset::video_stream_data_shard::max_payload_size : std::numeric_limits<uint32_t>::max();

	auto begin = data.begin();
	auto end = data.end();
	while (begin != end)
	{
		const size_t payload_size = std::max(0z, max_payload_size - ssize_t(serialized_size(shard.view_info)));
		auto next = std::min(end, begin + payload_size);
		if (next == end)
		{
			if (end_of_frame)
				shard.timing_info = timing_info;
		}
		shard.payload = {begin, next};
		try
		{
			if (control)
				cnx->send_control(to_headset::video_stream_data_shard{shard});
			else
				cnx->send_stream(to_headset::video_stream_data_shard{shard});
		}
		catch (...)
		{
			// Ignore network errors
		}
		++shard.shard_idx;
		stats[stream_idx % 3].shards++;
		health_totals.shards.fetch_add(1, std::memory_order_relaxed);
		shard.view_info.reset();
		begin = next;
	}
	if (end_of_frame)
		wivrn::trace::cpu_end(wivrn::trace::cpu_track::network, stream_idx, shard.frame_idx, "SendData");
}

} // namespace wivrn
