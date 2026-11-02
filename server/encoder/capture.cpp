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

#include "capture.h"

#include "encoder_settings.h"
#include "os/os_time.h"
#include "util/u_logging.h"

#include <chrono>
#include <cinttypes>
#include <cstdio>
#include <ctime>
#include <nlohmann/json.hpp>

namespace wivrn::capture
{

namespace
{
std::string timestamp_for_dirname()
{
	std::time_t t = std::time(nullptr);
	std::tm tm{};
#ifdef _WIN32
	localtime_s(&tm, &t);
#else
	localtime_r(&t, &tm);
#endif
	char buffer[32];
	std::strftime(buffer, sizeof(buffer), "%Y%m%d-%H%M%S", &tm);
	return buffer;
}

const char * stream_name(size_t i)
{
	switch (i)
	{
		case 0:
			return "left";
		case 1:
			return "right";
		default:
			return "alpha";
	}
}
} // namespace

const char * bitstream_extension(video_codec codec)
{
	switch (codec)
	{
		case h264:
			return "h264";
		case h265:
			return "h265";
		case av1:
			return "av1";
		case raw:
			return "yuv";
	}
	return "bin";
}

std::optional<std::filesystem::path> start_session(
        const std::filesystem::path & root,
        const std::array<encoder_settings, 3> & streams,
        const from_headset::headset_info_packet & info)
{
	auto dir = root / ("wivrn-capture-" + timestamp_for_dirname());
	std::error_code ec;
	std::filesystem::create_directories(dir, ec);
	if (ec)
	{
		U_LOG_W("Capture: cannot create %s: %s, capture disabled for this session", dir.string().c_str(), ec.message().c_str());
		return std::nullopt;
	}

	nlohmann::json manifest{
	        {"version", 1},
	        {"started_unix_ns", std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::system_clock::now().time_since_epoch()).count()},
	        {"started_host_ns", os_monotonic_get_ns()},
	        {"headset", info.system_name},
	        {"render_eye_width", info.render_eye_width},
	        {"render_eye_height", info.render_eye_height},
	        {"fps", streams[0].fps},
	        {"streams", nlohmann::json::array()},
	};
	for (size_t i = 0; i < streams.size(); ++i)
	{
		const auto & s = streams[i];
		manifest["streams"].push_back({
		        {"index", i},
		        {"name", stream_name(i)},
		        {"codec", bitstream_extension(s.codec)},
		        {"encoder", s.encoder_name},
		        {"width", s.width},
		        {"height", s.height},
		        {"bit_depth", s.bit_depth},
		        {"bitstream", "stream-" + std::to_string(i) + "." + bitstream_extension(s.codec)},
		        {"frames", "stream-" + std::to_string(i) + ".jsonl"},
		});
	}
	// The headset's nominal per-eye field of view, for readers that need it before the first frame
	manifest["fov"] = nlohmann::json::array();
	for (const auto & fov: info.fov)
		manifest["fov"].push_back({fov.angleLeft, fov.angleRight, fov.angleUp, fov.angleDown});

	std::ofstream out(dir / "manifest.json");
	out << manifest.dump(1, '\t') << std::endl;
	if (not out)
	{
		U_LOG_W("Capture: cannot write %s, capture disabled for this session", (dir / "manifest.json").string().c_str());
		return std::nullopt;
	}
	U_LOG_I("Capture: recording to %s", dir.string().c_str());
	return dir;
}

bool frame_log::open(const std::filesystem::path & path)
{
	out.open(path, std::ios::binary);
	if (not out)
		U_LOG_W("Capture: cannot open %s", path.string().c_str());
	return out.is_open();
}

void frame_log::stage(const to_headset::video_stream_data_shard::view_info_t & view)
{
	staged.display_time = view.display_time;
	staged.pose = view.pose;
	staged.fov = view.fov;
	staged.alpha = view.alpha;
	for (size_t v = 0; v < 2; ++v)
	{
		if (view.foveation[v].x != foveation[v].x or view.foveation[v].y != foveation[v].y)
		{
			foveation[v] = view.foveation[v];
			foveation_dirty = true;
		}
	}
}

void frame_log::write(const frame & f, const clock_offset & clock)
{
	const auto & view = staged;
	if (foveation_dirty)
	{
		out << "{\"type\":\"foveation\",\"stream\":" << int(f.stream) << ",\"frame\":" << f.index << ",\"views\":[";
		for (size_t v = 0; v < 2; ++v)
		{
			out << (v ? ",{\"x\":[" : "{\"x\":[");
			for (size_t i = 0; i < foveation[v].x.size(); ++i)
				out << (i ? "," : "") << foveation[v].x[i];
			out << "],\"y\":[";
			for (size_t i = 0; i < foveation[v].y.size(); ++i)
				out << (i ? "," : "") << foveation[v].y[i];
			out << "]}";
		}
		out << "]}\n";
		foveation_dirty = false;
	}

	char line[1536];
	int n = snprintf(line, sizeof(line),
	                 "{\"type\":\"frame\",\"stream\":%d,\"frame\":%" PRIu64 ",\"alpha\":%s,\"codec\":\"%s\",\"width\":%u,\"height\":%u"
	                 ",\"display_time_headset_ns\":%" PRId64 ",\"display_time_host_ns\":%" PRId64
	                 ",\"encode_begin_host_ns\":%" PRId64 ",\"encode_begin_headset_ns\":%" PRId64
	                 ",\"send_end_host_ns\":%" PRId64 ",\"clock_offset_ns\":%" PRId64 ",\"clock_stable\":%s"
	                 ",\"byte_offset\":%" PRIu64 ",\"byte_size\":%" PRIu64 ",\"views\":[",
	                 int(f.stream),
	                 f.index,
	                 view.alpha ? "true" : "false",
	                 bitstream_extension(f.codec),
	                 unsigned(f.width),
	                 unsigned(f.height),
	                 int64_t(view.display_time),
	                 int64_t(clock.from_headset(view.display_time)),
	                 f.encode_begin_host_ns,
	                 int64_t(clock.to_headset(f.encode_begin_host_ns)),
	                 f.send_end_host_ns,
	                 clock.b,
	                 clock.stable ? "true" : "false",
	                 f.byte_offset,
	                 f.byte_size);
	for (size_t v = 0; v < 2 and n > 0 and size_t(n) < sizeof(line); ++v)
	{
		const auto & p = view.pose[v];
		const auto & fov = view.fov[v];
		n += snprintf(line + n, sizeof(line) - n,
		              "%s{\"position\":[%.7g,%.7g,%.7g],\"orientation_xyzw\":[%.8g,%.8g,%.8g,%.8g],\"fov\":[%.7g,%.7g,%.7g,%.7g]}",
		              v ? "," : "",
		              p.position.x,
		              p.position.y,
		              p.position.z,
		              p.orientation.x,
		              p.orientation.y,
		              p.orientation.z,
		              p.orientation.w,
		              fov.angleLeft,
		              fov.angleRight,
		              fov.angleUp,
		              fov.angleDown);
	}
	if (n > 0 and size_t(n) + 3 < sizeof(line))
	{
		line[n++] = ']';
		line[n++] = '}';
		line[n++] = '\n';
		out.write(line, n);
	}
}

} // namespace wivrn::capture
