/*
 * WiVRn VR streaming
 * Copyright (C) 2026  GenRobo
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

#include "driver/clock_offset.h"
#include "wivrn_packets.h"

#include <array>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <optional>

// Mixed-reality capture, GRID #2837: record what the server encodes so an offline
// compositor can replay it over the headset's passthrough camera.
//
// One directory per session, `<capture.dir>/wivrn-capture-<time>/`, holding
//   manifest.json      the stream layout (eye size, codecs, frame rate, headset)
//   stream-<i>.<codec> the encoded bitstream of stream i (0 left, 1 right, 2 alpha),
//                      exactly the bytes sent to the headset
//   stream-<i>.jsonl   one line per encoded frame: index, timestamps on both clocks,
//                      the view poses and fields of view the frame was rendered with,
//                      and the byte range of the frame in the bitstream; a `foveation`
//                      line precedes a frame whenever the foveation tables change.
// See docs/configuration.md#capture for the line format.
namespace wivrn
{
struct encoder_settings;

namespace capture
{

// Create the session directory under `root` and write manifest.json. Returns the directory,
// or nothing (logged) when it cannot be created: capture is then off for the session.
std::optional<std::filesystem::path> start_session(
        const std::filesystem::path & root,
        const std::array<encoder_settings, 3> & streams,
        const from_headset::headset_info_packet & info);

// File extension of a bitstream dump for a codec, without the dot.
const char * bitstream_extension(video_codec);

// The per-stream frame sidecar, written next to the bitstream dump. stage() runs on the
// encode thread and copies the plain view data; write() runs where the dump is written, at
// the end of the frame. The foveation tables are only copied when they change.
class frame_log
{
	std::ofstream out;
	struct
	{
		XrTime display_time;
		std::array<XrPosef, 2> pose;
		std::array<XrFovf, 2> fov;
		bool alpha;
	} staged{};
	std::array<to_headset::foveation_parameter, 2> foveation;
	bool foveation_dirty = false;

public:
	explicit operator bool() const
	{
		return out.is_open();
	}

	bool open(const std::filesystem::path & path);

	struct frame
	{
		uint8_t stream;
		uint64_t index;
		video_codec codec;
		uint16_t width;
		uint16_t height;
		int64_t encode_begin_host_ns; // os_monotonic_get_ns() when the encode started
		int64_t send_end_host_ns;     // os_monotonic_get_ns() when the last byte was queued
		uint64_t byte_offset;         // first byte of the frame in the bitstream dump
		uint64_t byte_size;
	};

	void stage(const to_headset::video_stream_data_shard::view_info_t &);
	void write(const frame &, const clock_offset &);
};

} // namespace capture
} // namespace wivrn
