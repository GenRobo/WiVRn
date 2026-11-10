// Unit tests for the per-frame record (GRID #3095 milestone 6a, docs/health.md "Frame record"):
// ring to JSON line round trip including nulls, the header, rotation at the size limit with the
// three-file cap, nothing written before the record is started, and the 2 s settle without
// feedback. Usage: wivrn-health-frames-test
#include "driver/health_frames.h"

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <nlohmann/json.hpp>
#include <set>
#include <string>
#include <vector>

using namespace wivrn::health;

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

constexpr int64_t t0 = 1'000'000'000'000; // server monotonic ns at session start
constexpr int64_t period = 13'888'888;    // 72 Hz

std::vector<nlohmann::json> read_lines(const std::filesystem::path & path)
{
	std::vector<nlohmann::json> out;
	std::ifstream in(path, std::ios::binary);
	for (std::string line; std::getline(in, line);)
	{
		CHECK(not line.empty() and line.back() != '\r');
		out.push_back(nlohmann::json::parse(line));
	}
	return out;
}

frame_row full_row()
{
	frame_row r;
	r.n = 18221;
	r.s = 1;
	r.pd = t0 + 2'217'133'000;
	r.wake = t0 + 2'203'280'000;
	r.begin = t0 + 2'203'310'000;
	r.submit = t0 + 2'205'100'000;
	r.gpu = t0 + 2'205'900'000;
	r.comp0 = t0 + 2'206'000'000;
	r.comp1 = t0 + 2'207'200'000;
	r.enc0 = t0 + 2'207'250'000;
	r.enc1 = t0 + 2'210'800'000;
	r.send0 = t0 + 2'210'850'000;
	r.send1 = t0 + 2'211'400'000;
	r.rx0 = t0 + 2'212'100'000;
	r.rx1 = t0 + 2'213'300'000;
	r.dec0 = t0 + 2'213'350'000;
	r.dec1 = t0 + 2'217'000'000;
	r.blit = t0 + 2'217'050'000;
	r.disp = t0 + 2'217'140'000;
	r.shown = 1;
	r.bytes = 41230;
	r.idr = 0;
	r.shards = 31;
	r.lost = 0;
	r.pose_t = t0 + 2'202'900'000;
	r.pose_flags = 15;
	r.off_ns = 1'170'500;
	return r;
}

const std::set<std::string> frame_keys = {
        "type",
        "n",
        "s",
        "pd",
        "wake",
        "begin",
        "submit",
        "gpu",
        "comp0",
        "comp1",
        "enc0",
        "enc1",
        "send0",
        "send1",
        "rx0",
        "rx1",
        "dec0",
        "dec1",
        "blit",
        "disp",
        "shown",
        "bytes",
        "idr",
        "shards",
        "lost",
        "pose_t",
        "pose_flags",
        "off_us"};

void test_round_trip()
{
	auto r = full_row();
	auto line = frame_line(r, t0);
	auto j = nlohmann::json::parse(line);
	std::set<std::string> keys;
	for (auto & [k, v]: j.items())
		keys.insert(k);
	CHECK(keys == frame_keys);
	CHECK(j["type"] == "frame");
	CHECK(j["n"] == 18221);
	CHECK(j["s"] == 1);
	CHECK(j["pd"] == 2'217'133);
	CHECK(j["wake"] == 2'203'280);
	CHECK(j["disp"] == 2'217'140);
	CHECK(j["shown"] == 1);
	CHECK(j["bytes"] == 41230);
	CHECK(j["shards"] == 31);
	CHECK(j["pose_flags"] == 15);
	CHECK(j["off_us"] == 1170.5);
	for (auto & [k, v]: j.items())
		CHECK(not v.is_null());

	auto back = frame_from_line(line, t0);
	CHECK(back);
	if (back)
	{
		CHECK(back->n == r.n and back->s == r.s and back->pd == r.pd and back->wake == r.wake and back->begin == r.begin);
		CHECK(back->submit == r.submit and back->gpu == r.gpu and back->comp0 == r.comp0 and back->comp1 == r.comp1);
		CHECK(back->enc0 == r.enc0 and back->enc1 == r.enc1 and back->send0 == r.send0 and back->send1 == r.send1);
		CHECK(back->rx0 == r.rx0 and back->rx1 == r.rx1 and back->dec0 == r.dec0 and back->dec1 == r.dec1);
		CHECK(back->blit == r.blit and back->disp == r.disp and back->shown == r.shown and back->bytes == r.bytes);
		CHECK(back->idr == r.idr and back->shards == r.shards and back->lost == r.lost);
		CHECK(back->pose_t == r.pose_t and back->pose_flags == r.pose_flags and back->off_ns == r.off_ns);
	}

	// Unknown everywhere: nulls, never -1, never omitted.
	frame_row empty;
	empty.n = 7;
	empty.s = 0;
	empty.pd = t0 + period;
	auto e = nlohmann::json::parse(frame_line(empty, t0));
	keys.clear();
	for (auto & [k, v]: e.items())
		keys.insert(k);
	CHECK(keys == frame_keys);
	CHECK(e["pd"] == period / 1000);
	for (const char * k: {"wake", "begin", "submit", "gpu", "comp0", "comp1", "enc0", "enc1", "send0", "send1", "rx0", "rx1", "dec0", "dec1", "blit", "disp", "shown", "bytes", "idr", "shards", "lost", "pose_t", "pose_flags", "off_us"})
		CHECK(e[k].is_null());
	auto back_empty = frame_from_line(e.dump(), t0);
	CHECK(back_empty and back_empty->wake == 0 and back_empty->shown == -1 and back_empty->off_ns == frame_row::no_offset);

	CHECK(not frame_from_line(frame_header_line({}), t0));
	CHECK(not frame_from_line("not json", t0));
}

void test_header_clock_event()
{
	frame_header h{
	        .t0_mono_ns = t0,
	        .t0_unix_ms = 1794270000123,
	        .host = "GRGeordieUbuntuLaptop",
	        .server = "26.9-45-gd09ec1f9",
	        .streams = 2,
	        .display_hz = 72.f,
	        .period_ns = period,
	};
	auto j = nlohmann::json::parse(frame_header_line(h));
	CHECK(j["type"] == "header");
	CHECK(j["version"] == 1);
	CHECK(j["t0_mono_ns"] == t0);
	CHECK(j["t0_unix_ms"] == 1794270000123);
	CHECK(j["host"] == "GRGeordieUbuntuLaptop");
	CHECK(j["server"] == "26.9-45-gd09ec1f9");
	CHECK(j["streams"] == 2);
	CHECK(j["display_hz"] == 72.0);
	CHECK(j["period_ns"] == period);

	auto c = nlohmann::json::parse(frame_clock_line(t0 + 5'000'000'000, t0, 1'170'500, 42'000));
	CHECK(c["type"] == "clock");
	CHECK(c["t"] == 5'000'000);
	CHECK(c["off_us"] == 1170.5);
	CHECK(c["jitter_us"] == 42.0);

	auto e = nlohmann::json::parse(frame_event_line(t0 + 1'500'000, t0, "session_state", "FOCUSED"));
	CHECK(e["type"] == "event");
	CHECK(e["t"] == 1500);
	CHECK(e["name"] == "session_state");
	CHECK(e["value"] == "FOCUSED");
	auto n = nlohmann::json::parse(frame_event_line(t0 + 1'500'000, t0, "recenter", nullptr));
	CHECK(n["value"].is_null());
	auto b = nlohmann::json::parse(frame_event_line(t0, t0, "user_presence", true));
	CHECK(b["value"] == true);
}

/// Drives frame n through every frame path, with feedback for stream 0 and 1.
void produce(frame_ring & ring, uint64_t n, bool with_feedback, int64_t base)
{
	int64_t pd = base + int64_t(n) * period;
	ring.predicted(n, pd);
	ring.view_pose(pd, pd - 15'000'000, 15);
	ring.app_latched(n, {.wake = pd - 14'000'000, .begin = pd - 13'900'000, .submit = pd - 12'000'000, .gpu = pd - 11'500'000, .predicted_display = pd});
	ring.compositor_begin(n, pd - 11'000'000);
	ring.compositor_end(n, pd - 10'000'000);
	for (uint8_t s = 0; s < 2; ++s)
	{
		ring.encode_begin(n, s, pd - 9'900'000);
		ring.encode_end(n, s, pd - 7'000'000);
		ring.send_begin(n, s, pd - 6'900'000);
		ring.send_end(n, s, pd - 6'000'000, 41230 + s, 31, n == 0);
		if (with_feedback)
		{
			// First report: received and queued for decoding; second: displayed.
			ring.feedback(n, s, {.rx0 = pd - 5'000'000, .rx1 = pd - 4'000'000, .dec0 = pd - 3'900'000, .dec1 = 0, .blit = 0, .disp = 0, .shown = 0, .complete = true, .off_ns = 1'170'500});
			ring.feedback(n, s, {.rx0 = pd - 5'000'000, .rx1 = pd - 4'000'000, .dec0 = pd - 3'900'000, .dec1 = pd - 500'000, .blit = pd - 400'000, .disp = pd + 7'000, .shown = 1, .complete = true, .off_ns = 1'170'500});
		}
	}
}

void test_ring()
{
	auto & ring = frames();
	ring.reset();
	CHECK(ring.latest() == -1);
	CHECK(ring.latest_displayed(0) == -1);
	CHECK(not ring.read(0, 0));

	produce(ring, 0, true, t0 + 100'000'000);
	CHECK(ring.latest() == 0);
	CHECK(ring.latest_displayed(0) == 0 and ring.latest_displayed(1) == 0);
	auto r = ring.read(0, 1);
	CHECK(r);
	if (r)
	{
		int64_t pd = t0 + 100'000'000;
		CHECK(r->n == 0 and r->s == 1 and r->pd == pd);
		CHECK(r->wake == pd - 14'000'000 and r->begin == pd - 13'900'000 and r->submit == pd - 12'000'000 and r->gpu == pd - 11'500'000);
		CHECK(r->pose_t == pd - 15'000'000 and r->pose_flags == 15);
		CHECK(r->comp0 == pd - 11'000'000 and r->comp1 == pd - 10'000'000);
		CHECK(r->enc0 == pd - 9'900'000 and r->enc1 == pd - 7'000'000 and r->send0 == pd - 6'900'000 and r->send1 == pd - 6'000'000);
		CHECK(r->bytes == 41231 and r->shards == 31 and r->idr == 1);
		// The second report completed the row without losing the first one's fields.
		CHECK(r->rx0 == pd - 5'000'000 and r->rx1 == pd - 4'000'000 and r->dec0 == pd - 3'900'000 and r->dec1 == pd - 500'000);
		CHECK(r->blit == pd - 400'000 and r->disp == pd + 7'000 and r->shown == 1 and r->lost == 0 and r->off_ns == 1'170'500);
	}
	// No view pose located for that display time: pose fields stay unknown.
	ring.predicted(1, t0 + 200'000'000);
	ring.app_latched(1, {.wake = 1, .begin = 2, .submit = 3, .gpu = 4, .predicted_display = t0 + 999'000'000});
	auto r1 = ring.read(1, 0);
	CHECK(r1 and r1->pose_t == 0 and r1->pose_flags == -1 and r1->wake == 1 and r1->enc0 == 0 and r1->shown == -1 and r1->idr == -1);
	// Incomplete frame: lost is unknown, not 0.
	ring.feedback(1, 0, {.rx0 = 5, .rx1 = 0, .dec0 = 0, .dec1 = 0, .blit = 0, .disp = 0, .shown = 0, .complete = false, .off_ns = 0});
	r1 = ring.read(1, 0);
	CHECK(r1 and r1->lost == -1 and r1->shown == 0 and r1->rx0 == 5);
	// A slot taken over by frame n + size is no longer frame n.
	ring.predicted(1 + frame_ring::size, t0 + 300'000'000);
	CHECK(not ring.read(1, 0));
	CHECK(ring.read(1 + frame_ring::size, 0));
	ring.reset();
}

struct temp_dir
{
	std::filesystem::path path;
	temp_dir()
	{
		path = std::filesystem::temp_directory_path() / ("wivrn-health-frames-test-" + std::to_string(std::rand()));
		std::filesystem::remove_all(path);
	}
	~temp_dir()
	{
		std::filesystem::remove_all(path);
	}
};

frame_header header()
{
	return {.t0_mono_ns = t0, .t0_unix_ms = 1794270000123, .host = "test", .server = "test", .streams = 2, .display_hz = 72.f, .period_ns = period};
}

void test_nothing_before_start()
{
	temp_dir dir;
	auto & ring = frames();
	ring.reset();
	frame_writer w(dir.path, 50 << 20, [] { return std::string("20261110-120000"); });
	int lines = 0;
	w.on_line = [&](const std::string &) { ++lines; };
	produce(ring, 0, true, t0 + 100'000'000);
	w.tick(ring, t0 + 5'000'000'000, 0, 0);
	w.event(t0 + 1'000'000, "pause", nullptr);
	CHECK(not w.started());
	CHECK(w.files().empty());
	CHECK(not std::filesystem::exists(dir.path));
	CHECK(lines == 0);
	ring.reset();
}

void test_settle_and_rows()
{
	temp_dir dir;
	auto & ring = frames();
	ring.reset();
	int stamp = 0;
	frame_writer w(dir.path, 50 << 20, [&] { return "stamp" + std::to_string(++stamp); });
	std::vector<std::string> sent;
	w.on_line = [&](const std::string & l) { sent.push_back(l); };

	int64_t base = t0 + 100'000'000;
	// Frames 0..5 with feedback, frame 6 without; the headset has displayed up to frame 5.
	for (uint64_t n = 0; n < 6; ++n)
		produce(ring, n, true, base);
	produce(ring, 6, false, base);
	w.start(ring, header());
	CHECK(w.started());
	CHECK(w.files().size() == 1);
	CHECK(w.current().filename().string() == "wivrn-frames-stamp1.jsonl");

	int64_t now = base + 6 * period + 10'000'000; // right after frame 6's display slot
	w.tick(ring, now, 1'170'500, 42'000);
	auto lines = read_lines(w.current());
	// Header, frames 0..2 on both streams (frame 5 displayed = three beyond frame 2), clock.
	CHECK(lines.size() == 1 + 3 * 2 + 1);
	CHECK(lines.front()["type"] == "header");
	CHECK(lines.front()["streams"] == 2);
	CHECK(lines.back()["type"] == "clock");
	CHECK(lines.back()["off_us"] == 1170.5 and lines.back()["jitter_us"] == 42.0);
	std::vector<std::pair<int, int>> order;
	for (size_t i = 1; i + 1 < lines.size(); ++i)
	{
		CHECK(lines[i]["type"] == "frame");
		order.emplace_back(lines[i]["n"].get<int>(), lines[i]["s"].get<int>());
		CHECK(lines[i]["disp"] == (base + lines[i]["n"].get<int64_t>() * period + 7'000 - t0) / 1000);
		CHECK(lines[i]["shown"] == 1);
	}
	CHECK((order == std::vector<std::pair<int, int>>{{0, 0}, {1, 0}, {2, 0}, {0, 1}, {1, 1}, {2, 1}}));
	CHECK(sent.size() == lines.size()); // the connector got the same lines

	// One second later nothing new is final: no rows, one clock line.
	w.tick(ring, now + 1'000'000'000, 1'170'500, 0);
	lines = read_lines(w.current());
	CHECK(lines.size() == 1 + 6 + 2);

	// Frame 6 never gets feedback: 2 s after its predicted display it is written with nulls,
	// and frames 3..5 before it (in order, once).
	w.event(now + 1'500'000'000, "idr_request", "lost_reference");
	w.tick(ring, base + 6 * period + frame_writer::settle_ns, 1'170'500, 0);
	lines = read_lines(w.current());
	CHECK(lines.size() == 1 + 6 + 2 + 1 + 4 * 2 + 1);
	std::vector<nlohmann::json> frames6;
	for (auto & l: lines)
		if (l["type"] == "frame" and l["n"] == 6)
			frames6.push_back(l);
	CHECK(frames6.size() == 2);
	for (auto & f: frames6)
	{
		CHECK(f["enc0"].is_number() and f["send1"].is_number() and f["bytes"] == 41230 + f["s"].get<int>());
		CHECK(f["rx0"].is_null() and f["disp"].is_null() and f["shown"].is_null() and f["lost"].is_null() and f["off_us"].is_null());
		CHECK(f["pose_flags"] == 15);
	}
	bool saw_event = false;
	for (auto & l: lines)
		if (l["type"] == "event")
		{
			saw_event = true;
			CHECK(l["name"] == "idr_request" and l["value"] == "lost_reference");
		}
	CHECK(saw_event);

	// Late feedback for frame 6 does not write it again.
	ring.feedback(6, 0, {.rx0 = base + 7 * period, .rx1 = 0, .dec0 = 0, .dec1 = 0, .blit = 0, .disp = base + 8 * period, .shown = 1, .complete = true, .off_ns = 0});
	w.tick(ring, base + 20 * period + frame_writer::settle_ns, 0, 0);
	lines = read_lines(w.current());
	int rows6 = 0, frames_total = 0;
	for (auto & l: lines)
		if (l["type"] == "frame")
		{
			++frames_total;
			if (l["n"] == 6)
				++rows6;
		}
	CHECK(rows6 == 2);
	CHECK(frames_total == 7 * 2);
	ring.reset();
}

void test_rotation()
{
	temp_dir dir;
	auto & ring = frames();
	ring.reset();
	int stamp = 0;
	// Each frame line is ~330 bytes; a 3000-byte limit rotates every few rows.
	frame_writer w(dir.path, 3000, [&] { return "r" + std::to_string(++stamp); });
	int64_t base = t0 + 100'000'000;
	w.start(ring, header());
	for (uint64_t n = 0; n < 60; ++n)
		produce(ring, n, true, base);
	w.tick(ring, base + 60 * period + frame_writer::settle_ns, 0, 0);
	CHECK(stamp > 3);
	CHECK(w.files().size() == frame_writer::keep);
	size_t on_disk = 0;
	for (auto & entry: std::filesystem::directory_iterator(dir.path))
		if (entry.path().filename().string().starts_with("wivrn-frames-"))
			++on_disk;
	CHECK(on_disk == frame_writer::keep);
	for (auto & path: w.files())
	{
		CHECK(std::filesystem::exists(path));
		CHECK(std::filesystem::file_size(path) <= 3000);
		auto lines = read_lines(path);
		CHECK(not lines.empty() and lines.front()["type"] == "header");
	}
	CHECK(not std::filesystem::exists(dir.path / "wivrn-frames-r1.jsonl"));
	CHECK(w.current().filename().string() == "wivrn-frames-r" + std::to_string(stamp) + ".jsonl");
	ring.reset();
}

} // namespace

int main()
{
	test_round_trip();
	test_header_clock_event();
	test_ring();
	test_nothing_before_start();
	test_settle_and_rows();
	test_rotation();
	if (failures)
	{
		std::cerr << failures << " failure(s)" << std::endl;
		return 1;
	}
	std::cout << "health_frames: all checks passed" << std::endl;
	return 0;
}
