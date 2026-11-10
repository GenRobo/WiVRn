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

// Mixed-reality capture (GRID #2837): the session manifest and the per-frame sidecar that
// `vivarium-teleop capture-report` reads back. Usage: wivrn-capture-test (writes to a
// temporary directory).
#include "encoder/capture.h"
#include "encoder/encoder_settings.h"

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <nlohmann/json.hpp>
#include <string>
#include <vector>

namespace wivrn
{
// clock_offset.cpp pulls in the connection; the conversions are y = x + b.
XrTime clock_offset::from_headset(XrTime ts) const
{
	return ts - b;
}
XrTime clock_offset::to_headset(XrTime ts) const
{
	return ts + b;
}
} // namespace wivrn

namespace
{
int failures = 0;

#define CHECK(cond)                                                                   \
	do                                                                                \
	{                                                                                 \
		if (!(cond))                                                                  \
		{                                                                             \
			std::cerr << __FILE__ << ":" << __LINE__ << ": FAIL " #cond << std::endl; \
			++failures;                                                               \
		}                                                                             \
	} while (0)

std::vector<nlohmann::json> read_lines(const std::filesystem::path & path)
{
	std::ifstream in(path);
	std::vector<nlohmann::json> out;
	for (std::string line; std::getline(in, line);)
		if (not line.empty())
			out.push_back(nlohmann::json::parse(line));
	return out;
}

wivrn::to_headset::video_stream_data_shard::view_info_t view(int64_t display_time, float x, int fov_cols)
{
	wivrn::to_headset::video_stream_data_shard::view_info_t v{
	        .display_time = display_time,
	        .alpha = true,
	};
	for (size_t i = 0; i < 2; ++i)
	{
		v.pose[i] = {.orientation = {0, 0.7071068f, 0, 0.7071068f}, .position = {x + float(i) * 0.064f, 1.6f, -0.25f}};
		v.fov[i] = {.angleLeft = -0.9f, .angleRight = 0.8f, .angleUp = 0.85f, .angleDown = -0.95f};
		v.foveation[i].x = std::vector<uint16_t>(fov_cols, 1);
		v.foveation[i].y = {2, 3, 2};
	}
	return v;
}
} // namespace

int main()
{
	using namespace wivrn;
	auto root = std::filesystem::temp_directory_path() / "wivrn-capture-test";
	std::filesystem::remove_all(root);

	std::array<encoder_settings, 3> streams;
	for (size_t i = 0; i < 3; ++i)
		streams[i] = {.width = 1920, .height = uint16_t(i == 2 ? 960 : 1920), .codec = h264, .fps = 72, .encoder_name = "nvenc", .bit_depth = 8};
	from_headset::headset_info_packet info{.render_eye_width = 2064, .render_eye_height = 2208, .system_name = "Quest 3"};
	info.fov[0] = info.fov[1] = {-0.9f, 0.8f, 0.85f, -0.95f};

	auto dir = capture::start_session(root, streams, info);
	CHECK(dir.has_value());
	if (not dir)
		return 1;
	auto manifest = nlohmann::json::parse(std::ifstream(*dir / "manifest.json"));
	CHECK(manifest["version"] == 1);
	CHECK(manifest["headset"] == "Quest 3");
	CHECK(manifest["streams"].size() == 3);
	CHECK(manifest["streams"][2]["bitstream"] == "stream-2.h264");
	CHECK(manifest["streams"][2]["height"] == 960);
	CHECK(manifest["fov"][0][0].get<float>() == -0.9f);
	CHECK(manifest.contains("started_unix_ns") and manifest.contains("started_host_ns"));

	capture::frame_log log;
	CHECK(not log);
	CHECK(log.open(*dir / "stream-0.jsonl"));
	CHECK(bool(log));

	clock_offset clock{.b = 5'000'000'000, .stable = true};
	capture::frame_log::frame f{.stream = 0, .index = 10, .codec = h264, .width = 1920, .height = 1920};

	// Two frames with the same foveation, one with another: two foveation lines in all.
	log.stage(view(1'000'000'000'000, 0.f, 4));
	f.encode_begin_host_ns = 999'990'000'000;
	f.send_end_host_ns = 999'995'000'000;
	f.byte_offset = 0;
	f.byte_size = 1000;
	log.write(f, clock);
	log.stage(view(1'000'013'888'888, 0.01f, 4));
	f.index = 11;
	f.byte_offset = 1000;
	f.byte_size = 500;
	log.write(f, clock);
	log.stage(view(1'000'027'777'777, 0.02f, 5));
	f.index = 12;
	f.byte_offset = 1500;
	f.byte_size = 600;
	log.write(f, clock);
	log = capture::frame_log(); // close

	auto lines = read_lines(*dir / "stream-0.jsonl");
	CHECK(lines.size() == 5);
	if (lines.size() != 5)
		return 1;
	CHECK(lines[0]["type"] == "foveation");
	CHECK(lines[0]["frame"] == 10);
	CHECK(lines[0]["views"][0]["x"].size() == 4);
	CHECK(lines[0]["views"][1]["y"] == nlohmann::json({2, 3, 2}));
	CHECK(lines[1]["type"] == "frame");
	CHECK(lines[2]["type"] == "frame");
	CHECK(lines[3]["type"] == "foveation");
	CHECK(lines[3]["views"][0]["x"].size() == 5);
	CHECK(lines[4]["type"] == "frame");

	const auto & frame = lines[1];
	CHECK(frame["stream"] == 0);
	CHECK(frame["frame"] == 10);
	CHECK(frame["alpha"] == true);
	CHECK(frame["codec"] == "h264");
	CHECK(frame["width"] == 1920 and frame["height"] == 1920);
	CHECK(frame["display_time_headset_ns"] == 1'000'000'000'000);
	CHECK(frame["display_time_host_ns"] == 995'000'000'000);
	CHECK(frame["encode_begin_host_ns"] == 999'990'000'000);
	CHECK(frame["encode_begin_headset_ns"] == 1'004'990'000'000);
	CHECK(frame["send_end_host_ns"] == 999'995'000'000);
	CHECK(frame["clock_offset_ns"] == 5'000'000'000);
	CHECK(frame["clock_stable"] == true);
	CHECK(frame["byte_offset"] == 0 and frame["byte_size"] == 1000);
	CHECK(frame["views"].size() == 2);
	CHECK(frame["views"][1]["position"][0].get<float>() == 0.064f);
	CHECK(frame["views"][0]["position"][1].get<float>() == 1.6f);
	CHECK(frame["views"][0]["orientation_xyzw"][3].get<float>() == 0.7071068f);
	const float fov[] = {-0.9f, 0.8f, 0.85f, -0.95f};
	for (size_t i = 0; i < 4; ++i)
		CHECK(frame["views"][0]["fov"][i].get<float>() == fov[i]);
	CHECK(lines[4]["byte_offset"] == 1500 and lines[4]["views"][0]["position"][0].get<float>() == 0.02f);

	std::filesystem::remove_all(root);
	if (failures)
		std::cerr << failures << " failure(s)" << std::endl;
	else
		std::cout << "capture_log: ok" << std::endl;
	return failures ? 1 : 0;
}
