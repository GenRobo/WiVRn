// Functional tests for the stream health rules (GRID #2834, design §9).
// Usage: wivrn-health-test <path to server/tests/health_fixtures>
// Fixtures are one JSON line per second in the flight-recorder format (docs/health.md):
//   clean_session.jsonl      2026-10-30 Windows desktop, no other browser running: 120 Hz, no loss
//   contending_browser.jsonl 2026-10-30 Windows desktop: 8 s at 120 Hz then 66 Hz while another
//                            Chrome ran; server counters unchanged
//   lossy_adapter.jsonl      derived from the 2026-10-29 Linux 1 Gb cdc_ncm adapter run: 225 IDR
//                            requests in 30 s (per-second spread is synthetic), 0-2 skips per 5 s
#include "driver/health_rules.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <map>
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

std::vector<sample> load(const std::string & path)
{
	std::ifstream in(path);
	std::vector<sample> out;
	for (std::string line; std::getline(in, line);)
	{
		if (line.empty())
			continue;
		auto s = from_json_line(line);
		if (!s)
		{
			std::cerr << path << ": unparsable line: " << line.substr(0, 80) << std::endl;
			++failures;
			continue;
		}
		out.push_back(*s);
	}
	return out;
}

/// Replay a session and return every rule that fired at least once.
std::set<rule_id> replay(const std::vector<sample> & session, const thresholds & th)
{
	rule_state st;
	std::set<rule_id> fired;
	for (size_t i = 0; i < session.size(); ++i)
	{
		std::span<const sample> recent(session.data(), i + 1);
		for (const auto & e: st.step(recent, th))
			if (e.fired)
				fired.insert(e.id);
	}
	return fired;
}

sample clean_at(int64_t t)
{
	sample s;
	s.t_unix_s = t;
	s.link.send_ms_p95 = 1.2f;
	s.link.receive_spread_ms_p95 = 1.8f;
	s.link.frames_incomplete = 0;
	s.encoder.presented = 600;
	s.encoder.encode_ms_p95 = 2.9f;
	s.app.hz = 120;
	s.app.late_pct = 0.3f;
	s.headset.display_hz = 120;
	s.headset.requested_hz = 120;
	s.headset.decode_ms_p95 = 4.1f;
	s.headset.battery_pct = 62;
	s.headset.charging = true;
	s.headset.thermal_status = 1;
	return s;
}

sample lossy_at(int64_t t)
{
	sample s = clean_at(t);
	s.link.frames_incomplete = 8;
	s.link.idr_lost_ref = 8;
	return s;
}

void test_recorded_sessions(const std::string & dir)
{
	const auto th = thresholds::defaults();

	auto clean = load(dir + "/clean_session.jsonl");
	CHECK(clean.size() == 30);
	CHECK(replay(clean, th).empty());

	auto browser = load(dir + "/contending_browser.jsonl");
	CHECK(browser.size() == 30);
	CHECK(replay(browser, th) == std::set<rule_id>{rule_id::app_slow});

	auto lossy = load(dir + "/lossy_adapter.jsonl");
	CHECK(lossy.size() == 30);
	CHECK(replay(lossy, th) == std::set<rule_id>{rule_id::link_loss});
}

void test_persistence_and_cooldown()
{
	const auto th = thresholds::defaults();
	rule_state st;
	std::vector<sample> session;
	auto step = [&](sample s) {
		session.push_back(s);
		return st.step(session, th);
	};
	int64_t t = 1000;

	// Warm up clean.
	for (int i = 0; i < 5; ++i)
		CHECK(step(clean_at(t++)).empty());

	// One bad second does not fire.
	CHECK(step(lossy_at(t++)).empty());
	CHECK(step(clean_at(t++)).empty());

	// Three consecutive bad seconds fire exactly once, on the third.
	CHECK(step(lossy_at(t++)).empty());
	CHECK(step(lossy_at(t++)).empty());
	auto events = step(lossy_at(t++));
	CHECK(events.size() == 1 && events[0].fired && events[0].id == rule_id::link_loss);
	CHECK(!events.empty() && events[0].message.find("lost") != std::string::npos);
	CHECK(st.active() == std::vector<rule_id>{rule_id::link_loss});

	// Still bad: no repeated event.
	CHECK(step(lossy_at(t++)).empty());

	// Clears only after five clean seconds.
	for (int i = 0; i < 4; ++i)
		CHECK(step(clean_at(t++)).empty());
	events = step(clean_at(t++));
	CHECK(events.size() == 1 && !events[0].fired && events[0].id == rule_id::link_loss);
	CHECK(st.active().empty());

	// Re-firing within the 30 s cooldown is suppressed, even after three bad seconds...
	for (int i = 0; i < 3; ++i)
		CHECK(step(lossy_at(t++)).empty());
	// ...and allowed once the cooldown has passed.
	for (int i = 0; i < 5; ++i)
		step(clean_at(t++));
	t += 30;
	step(lossy_at(t++));
	step(lossy_at(t++));
	events = step(lossy_at(t++));
	CHECK(events.size() == 1 && events[0].fired);
}

void test_rules_do_not_cross_fire()
{
	const auto th = thresholds::defaults();
	auto only = [&](sample (*make)(int64_t)) {
		std::vector<sample> session;
		for (int i = 0; i < 10; ++i)
			session.push_back(make(1000 + i));
		return replay(session, th);
	};
	CHECK(only([](int64_t t) { auto s = clean_at(t); s.encoder.skipped = 3; return s; }) == std::set<rule_id>{rule_id::encoder_busy});
	CHECK(only([](int64_t t) { auto s = clean_at(t); s.encoder.util_pct = 92; return s; }) == std::set<rule_id>{rule_id::encoder_busy});
	CHECK(only([](int64_t t) { auto s = clean_at(t); s.app.hz = 82; return s; }) == std::set<rule_id>{rule_id::app_slow});
	CHECK(only([](int64_t t) { auto s = clean_at(t); s.app.late_pct = 8; return s; }) == std::set<rule_id>{rule_id::app_late});
	CHECK(only([](int64_t t) { auto s = clean_at(t); s.headset.decode_ms_p95 = 12; return s; }) == std::set<rule_id>{rule_id::headset_decode});
	CHECK(only([](int64_t t) { auto s = clean_at(t); s.headset.thermal_status = 4; return s; }) == std::set<rule_id>{rule_id::headset_thermal});
	CHECK(only([](int64_t t) { auto s = clean_at(t); s.headset.battery_pct = 9; s.headset.charging = false; return s; }) == std::set<rule_id>{rule_id::headset_battery});
	CHECK(only([](int64_t t) { auto s = clean_at(t); s.headset.display_hz = 90; return s; }) == std::set<rule_id>{rule_id::refresh_mismatch});
	CHECK(only([](int64_t t) { auto s = clean_at(t); s.link.send_ms_p95 = 4.5f; return s; }) == std::set<rule_id>{rule_id::link_slow});
	// Judder needs the frame ring's counts: a clean second with 72 display frames is silent, 3 % is not.
	CHECK(only([](int64_t t) { auto s = clean_at(t); s.frames.display = 72; return s; }).empty());
	CHECK(only([](int64_t t) { auto s = clean_at(t); s.frames.display = 100; s.frames.repeated = 2; s.frames.skipped = 1; s.frames.link = 3; return s; }) == std::set<rule_id>{rule_id::judder});
	// Unknown headset fields (milestone 1 server, no telemetry) never fire headset rules.
	CHECK(only([](int64_t t) { auto s = clean_at(t); s.headset = {}; s.link.frames_incomplete = -1; return s; }).empty());
}

void test_thresholds()
{
	auto d = thresholds::defaults();
	CHECK(d.get("link_loss.frames_per_s") == 5);
	CHECK(d.get("app_slow.ratio") == 0.9f);
	CHECK(d.get("encoder_busy.util_pct") == 85);
	auto o = thresholds::with({{"link_loss.frames_per_s", 20}, {"no_such_rule.param", 1}});
	CHECK(o.get("link_loss.frames_per_s") == 20);
	CHECK(o.get("app_slow.ratio") == 0.9f);
	CHECK(o.get("no_such_rule.param") == 0);

	// A raised threshold stops the lossy fixture from firing.
	std::vector<sample> session;
	for (int i = 0; i < 10; ++i)
		session.push_back(lossy_at(1000 + i));
	CHECK(replay(session, o).empty());
	CHECK(replay(session, d) == std::set<rule_id>{rule_id::link_loss});

	for (auto id: {rule_id::link_loss, rule_id::app_slow, rule_id::refresh_mismatch, rule_id::judder})
		CHECK(rule_from_name(name(id)) == id);
	CHECK(!rule_from_name("bogus"));
	CHECK(rules().size() == 10);
	CHECK(d.get("judder.pct") == 2);
}

void test_histogram()
{
	log_histogram h;
	CHECK(h.total() == 0);
	CHECK(h.percentile(0.95) == 0);
	// Uniform 0.5 .. 100 ms in 0.5 ms steps: exact p95 is 95 ms.
	for (int i = 1; i <= 200; ++i)
		h.observe(i * 0.5);
	CHECK(h.total() == 200);
	double p95 = h.percentile(0.95);
	// Within one logarithmic bucket (buckets grow 1.25x) of the exact value.
	CHECK(p95 >= 95.0 / 1.25 && p95 <= 95.0);
	CHECK(h.percentile(0.5) >= 50.0 / 1.25 && h.percentile(0.5) <= 50.0);
	CHECK(log_histogram::bucket(0) == 0);
	CHECK(log_histogram::bucket(0.05) == 0);
	CHECK(log_histogram::bucket(1000) == log_histogram::buckets - 1);
	CHECK(log_histogram::bucket(1.0) < log_histogram::bucket(2.0));
	h.clear();
	CHECK(h.total() == 0);
}

void test_json_round_trip()
{
	sample s = lossy_at(1761866000);
	s.encoder.util_pct = 31;
	s.app_metrics["sim_realtime_ratio"] = 0.6;
	auto line = to_json_line(s, std::vector<rule_id>{rule_id::link_loss});
	CHECK(line.find('\n') == std::string::npos);
	CHECK(line.find("\"link_loss\"") != std::string::npos);
	CHECK(line.find("sim_realtime_ratio") != std::string::npos);
	auto back = from_json_line(line);
	CHECK(back.has_value());
	if (back)
	{
		CHECK(back->t_unix_s == s.t_unix_s);
		CHECK(back->link.idr_lost_ref == 8);
		CHECK(back->link.frames_incomplete == 8);
		CHECK(back->encoder.util_pct == 31);
		CHECK(std::abs(back->app.hz - 120) < 1e-3);
		CHECK(back->headset.charging);
		CHECK(back->app_metrics.at("sim_realtime_ratio") == 0.6);
		CHECK(back->app_metrics_dropped == 0);
		CHECK(back->app_warnings.empty());
	}
	CHECK(!from_json_line("not json"));

	s.app_metrics_dropped = 3;
	s.app_warnings = {"sim_slow"};
	s.app.half_rate_probes = 2;
	s.app.half_rate_recoveries = 1;
	back = from_json_line(to_json_line(s, {}));
	CHECK(back and back->app_metrics_dropped == 3);
	CHECK(back and back->app_warnings == std::vector<std::string>{"sim_slow"});
	CHECK(back and back->app.half_rate_probes == 2 and back->app.half_rate_recoveries == 1);
	// Records written before the counters existed read back as zero.
	auto old = from_json_line(R"({"t":1,"app":{"hz":66,"cpu_ms":15.0,"gpu_ms":0.14,"late_pct":3.0}})");
	CHECK(old and old->app.half_rate_probes == 0 and old->app.half_rate_recoveries == 0 and old->app.hz == 66);
	CHECK(old and old->headset.compositor_dropped == -1 and old->headset.runtime_m2p_ms == -1 and old->frames.display == -1);
	CHECK(to_json_line(clean_at(0), {}).find("app_metrics") == std::string::npos); // nothing posted: keys absent
	CHECK(to_json_line(clean_at(0), {}).find("app_warnings") == std::string::npos);

	// Headset runtime counters and the frame stats (GRID #3095 M6c).
	s.headset.compositor_dropped = 2;
	s.headset.compositor_stale = -1;
	s.headset.compositor_gpu_ms = 3.1f;
	s.headset.app_gpu_ms = 4.7f;
	s.headset.runtime_m2p_ms = 41.2f;
	s.headset.cpu_util_pct = 63;
	s.headset.gpu_util_pct = -1;
	s.frames = {.display = 79, .repeated = 7, .skipped = 0, .link = 14, .compositor = 1};
	back = from_json_line(to_json_line(s, {}));
	CHECK(back and back->headset.compositor_dropped == 2 and back->headset.compositor_stale == -1);
	CHECK(back and std::abs(back->headset.compositor_gpu_ms - 3.1f) < 1e-3 and std::abs(back->headset.app_gpu_ms - 4.7f) < 1e-3);
	CHECK(back and std::abs(back->headset.runtime_m2p_ms - 41.2f) < 1e-3 and back->headset.cpu_util_pct == 63 and back->headset.gpu_util_pct == -1);
	CHECK(back and back->frames.display == 79 and back->frames.repeated == 7 and back->frames.link == 14 and back->frames.compositor == 1 and back->frames.app == 0);
}

// --- judder (GRID #3095 M6c): settled rows classified by stage, the rule over the samples ---

constexpr int64_t period = 13'888'888; // 72 Hz
constexpr int frames_per_s = 72;

/// A clean row: every stage at a typical cost (10.85 ms from wake to blit), the pose located
/// just after the wake, blitted 1.15 ms early and displayed on time.
frame_row clean_row(uint64_t n, uint8_t s)
{
	frame_row r;
	r.n = n;
	r.s = s;
	r.pd = 1'000'000'000 + int64_t(n) * period;
	r.wake = r.pd - 12'000'000;
	r.begin = r.wake + 100'000;
	r.submit = r.wake + 2'000'000;
	r.gpu = r.submit + 500'000;
	r.comp0 = r.gpu + 100'000;
	r.comp1 = r.comp0 + 1'000'000;
	r.enc0 = r.comp1 + 50'000;
	r.enc1 = r.enc0 + 2'500'000;
	r.send0 = r.enc1 + 50'000;
	r.send1 = r.send0 + 600'000;
	r.rx0 = r.send0 + 700'000;
	r.rx1 = r.send1 + 800'000;
	r.dec0 = r.rx1 + 50'000;
	r.dec1 = r.dec0 + 3'000'000;
	r.blit = r.dec1 + 100'000;
	r.disp = r.pd + 7'000;
	r.shown = 1;
	r.bytes = 41230;
	r.idr = 0;
	r.shards = 31;
	r.lost = 0;
	r.pose_t = r.wake + 300'000;
	r.pose_flags = 15;
	r.off_ns = 1'170'500;
	return r;
}

/// One second of a two-stream ring (sorted by n, s), `fault` applied to every row of frames
/// where `n % 10 == 5`, whose predecessor is then shown twice.
std::vector<frame_row> second_of_rows(int64_t first_n, void (*fault)(frame_row &))
{
	std::vector<frame_row> rows;
	for (int64_t n = first_n; n < first_n + frames_per_s; ++n)
		for (uint8_t s = 0; s < 2; ++s)
		{
			frame_row r = clean_row(uint64_t(n), s);
			if (fault and n % 10 == 5)
				fault(r);
			if (fault and n % 10 == 4)
				r.shown = 2;
			rows.push_back(r);
		}
	return rows;
}

void link_fault(frame_row & r)
{
	// The last shard took 12 ms longer to arrive; everything after it shifts and the frame
	// lands one period late.
	constexpr int64_t extra = 12'000'000;
	r.rx1 += extra;
	r.dec0 += extra;
	r.dec1 += extra;
	r.blit += extra;
	r.disp += period;
}

void compositor_fault(frame_row & r)
{
	// Delivered as always, the headset still showed it a period late.
	r.disp += period;
}

void test_judder_stats()
{
	auto clean = judder_stats(second_of_rows(0, nullptr), period);
	CHECK(clean.display == frames_per_s and clean.repeated == 0 and clean.skipped == 0);
	CHECK(clean.link == 0 and clean.compositor == 0 and clean.unattributed == 0);
	CHECK(judder_stats({}, period).display == -1);

	// 7 faulty frames per second: each repeats its predecessor (blamed on the late frame, as the
	// GRID reader does) and is late itself.
	auto link = judder_stats(second_of_rows(0, link_fault), period);
	CHECK(link.display == frames_per_s + 7 and link.repeated == 7 and link.skipped == 0);
	CHECK(link.link == 14 and link.compositor == 0 and link.app == 0 and link.decode == 0 and link.unattributed == 0);
	CHECK(dominant_class(link) == "link");

	auto compositor = judder_stats(second_of_rows(0, compositor_fault), period);
	CHECK(compositor.display == frames_per_s + 7 and compositor.repeated == 7);
	CHECK(compositor.compositor == 14 and compositor.link == 0 and compositor.unattributed == 0);
	CHECK(dominant_class(compositor) == "headset-compositor");

	// No feedback at all is a skip blamed on the link; an untracked pose wins over everything.
	auto lost = judder_stats(second_of_rows(0, [](frame_row & r) { r.rx0 = r.rx1 = r.dec0 = r.dec1 = r.blit = r.disp = 0; r.shown = -1; r.lost = -1; }), period);
	CHECK(lost.skipped == 7 and lost.display == frames_per_s and lost.link == 14);
	auto untracked = judder_stats(second_of_rows(0, [](frame_row & r) { link_fault(r); r.pose_flags = 3; }), period);
	CHECK(untracked.tracking == 14 and untracked.link == 0);
	CHECK(dominant_class({}) == "unattributed");
}

void test_judder_rule()
{
	const auto th = thresholds::defaults();
	auto session_with = [](void (*fault)(frame_row &), int seconds) {
		std::vector<sample> session;
		for (int i = 0; i < seconds; ++i)
		{
			sample s = clean_at(1000 + i);
			s.headset.display_hz = s.headset.requested_hz = 72;
			s.frames = judder_stats(second_of_rows(int64_t(i) * frames_per_s, fault), period);
			session.push_back(s);
		}
		return session;
	};
	CHECK(replay(session_with(nullptr, 10), th).empty());
	CHECK(replay(session_with(link_fault, 10), th) == std::set<rule_id>{rule_id::judder});

	// Fires on the third faulty second and names the class; a raised threshold keeps it quiet.
	for (auto [fault, expected]: {std::pair{link_fault, "mostly link"}, {compositor_fault, "mostly headset-compositor"}})
	{
		rule_state st;
		auto session = session_with(fault, 5);
		std::vector<rule_state::event> events;
		for (size_t i = 0; i < session.size(); ++i)
		{
			auto e = st.step(std::span<const sample>(session.data(), i + 1), th);
			if (!e.empty())
				CHECK(i == 2);
			events.insert(events.end(), e.begin(), e.end());
		}
		CHECK(events.size() == 1 and events[0].id == rule_id::judder and events[0].fired);
		// 8 of 80 display frames in the third second (frames 144..215 hold eight n % 10 == 5).
		CHECK(events.size() == 1 and events[0].message.starts_with("Judder: 10.0 % of frames repeated or skipped, ") and events[0].message.ends_with(expected));
	}
	CHECK(replay(session_with(link_fault, 10), thresholds::with({{"judder.pct", 12}})).empty());
}

// --- telemetry connector (milestone 4): inbound lines are validated, counted and never crash ---

void test_connector_parse()
{
	auto w = parse_connector_line(R"({"type":"warning","id":"sim_slow","message":"Simulation at 0.6x realtime","ttl_s":10})");
	CHECK(w and w->kind == connector_line::kind::warning);
	CHECK(w and w->name == "sim_slow");
	CHECK(w and w->message == "Simulation at 0.6x realtime");
	CHECK(w and w->ttl_s == 10);
	// ttl is optional (10 s) and clamped to 1..600
	w = parse_connector_line(R"({"type":"warning","id":"a","message":"m"})");
	CHECK(w and w->ttl_s == 10);
	w = parse_connector_line(R"({"type":"warning","id":"a","message":"m","ttl_s":5000})");
	CHECK(w and w->ttl_s == 600);

	auto m = parse_connector_line(R"({"type":"metric","name":"sim_realtime_ratio","value":0.6,"unit":"x"})");
	CHECK(m and m->kind == connector_line::kind::metric);
	CHECK(m and m->name == "sim_realtime_ratio");
	CHECK(m and m->value == 0.6);
	CHECK(parse_connector_line(R"({"type":"metric","name":"n","value":42})").has_value()); // integers are numbers

	// Dropped: oversize, not JSON, not an object, unknown type, bad id/name, bad value, bad message
	std::string oversize = R"({"type":"metric","name":"n","value":1,"pad":")" + std::string(max_connector_line, 'x') + "\"}";
	CHECK(!parse_connector_line(oversize));
	CHECK(!parse_connector_line(""));
	CHECK(!parse_connector_line("not json"));
	CHECK(!parse_connector_line("[1,2,3]"));
	CHECK(!parse_connector_line(R"("metric")"));
	CHECK(!parse_connector_line(R"({"type":"frame","name":"n","value":1})"));
	CHECK(!parse_connector_line(R"({"name":"n","value":1})"));
	CHECK(!parse_connector_line(R"({"type":"metric","name":"Bad-Name","value":1})"));
	CHECK(!parse_connector_line(R"({"type":"metric","name":"","value":1})"));
	CHECK(!parse_connector_line(R"({"type":"metric","name":")" + std::string(33, 'a') + R"(","value":1})"));
	CHECK(parse_connector_line(R"({"type":"metric","name":")" + std::string(32, 'a') + R"(","value":1})").has_value());
	CHECK(!parse_connector_line(R"({"type":"metric","name":"n"})"));
	CHECK(!parse_connector_line(R"({"type":"metric","name":"n","value":"1"})"));
	CHECK(!parse_connector_line(R"({"type":"metric","name":7,"value":1})"));
	CHECK(!parse_connector_line(R"({"type":"warning","id":"sim slow","message":"m"})"));
	CHECK(!parse_connector_line(R"({"type":"warning","id":"a","message":""})"));
	CHECK(!parse_connector_line(R"({"type":"warning","id":"a","message":")" + std::string(201, 'm') + R"("})"));
	CHECK(parse_connector_line(R"({"type":"warning","id":"a","message":")" + std::string(200, 'm') + R"("})").has_value());
	CHECK(!parse_connector_line(R"({"type":"warning","id":"a","message":"m","ttl_s":"10"})"));
	CHECK(!parse_connector_line(R"({"type":"warning","id":"a"})"));
}

void test_connector_line_buffer()
{
	connector_line_buffer buf;
	std::vector<std::string> lines;
	auto collect = [&](std::string_view l) { lines.emplace_back(l); };

	// Lines split across arbitrary chunks, CRLF tolerated by the parser, empty lines delivered as-is
	buf.feed("{\"a\":1}\n{\"b\"", collect);
	CHECK(lines.size() == 1);
	buf.feed(":2}\r\n\n{\"c\":3}", collect);
	CHECK(lines.size() == 3);
	CHECK(lines[0] == "{\"a\":1}");
	CHECK(lines[1] == "{\"b\":2}\r");
	CHECK(lines[2].empty());
	buf.feed("\n", collect);
	CHECK(lines.size() == 4 and lines[3] == "{\"c\":3}");

	// An oversize line is delivered once, truncated to max + 1 so the parser rejects it, and the
	// rest of it is skipped; the following line is intact.
	lines.clear();
	std::string huge(3 * max_connector_line, 'x');
	buf.feed(huge.substr(0, 1000), collect);
	buf.feed(huge.substr(1000, 5000), collect);
	CHECK(lines.size() == 1 and lines[0].size() == max_connector_line + 1);
	buf.feed(huge.substr(6000) + "\n{\"ok\":true}\n", collect);
	CHECK(lines.size() == 2 and lines[1] == "{\"ok\":true}");
	CHECK(!parse_connector_line(lines[0]));
	CHECK(buf.pending.empty() and not buf.skipping);

	// Exactly the limit passes
	lines.clear();
	buf.feed(std::string(max_connector_line, 'y') + "\n", collect);
	CHECK(lines.size() == 1 and lines[0].size() == max_connector_line);
}

void test_app_warning_cooldown()
{
	app_warning_state st;
	CHECK(st.fire("sim_slow", 10, 1000));
	CHECK(!st.fire("sim_slow", 10, 1005)); // within ttl: not sent again
	CHECK(!st.fire("sim_slow", 10, 1020)); // ttl over, but within the 30 s rule cooldown
	CHECK(st.fire("other", 10, 1020));     // another id is independent
	CHECK((st.active() == std::vector<std::string>{"other", "sim_slow"}));
	CHECK(st.expire(1009).empty());
	CHECK(st.expire(1010) == std::vector<std::string>{"sim_slow"});
	CHECK(st.active() == std::vector<std::string>{"other"});
	CHECK(st.fire("sim_slow", 10, 1030)); // cooldown elapsed
	CHECK(st.fire("long", 120, 1030));
	CHECK(!st.fire("long", 120, 1100)); // a ttl longer than the cooldown holds the id for the ttl
	CHECK(st.fire("long", 120, 1150));

	// Distinct ids are bounded
	app_warning_state many;
	for (size_t i = 0; i < app_warning_state::max_entries; ++i)
		CHECK(many.fire("id_" + std::to_string(i), 10, 0));
	CHECK(!many.fire("one_too_many", 10, 0));
	many.expire(100); // every entry listed and refire-able: forgotten
	CHECK(many.active().empty());
	CHECK(many.fire("one_too_many", 10, 100));
}

/// What the relay thread does with the sink's bytes: split, parse, drop and count.
void test_connector_end_to_end()
{
	connector_line_buffer buf;
	std::map<std::string, double> metrics;
	std::vector<std::string> sent;
	uint32_t dropped = 0;
	app_warning_state st;
	auto on_line = [&](std::string_view line) {
		auto p = parse_connector_line(line);
		if (not p)
			dropped++;
		else if (p->kind == connector_line::kind::metric)
			metrics[p->name] = p->value;
		else if (st.fire(p->name, p->ttl_s, 0))
			sent.push_back(p->message);
	};
	std::string stream =
	        R"({"type":"metric","name":"sim_realtime_ratio","value":0.6})"
	        "\n"
	        R"({"type":"warning","id":"sim_slow","message":"Simulation slow","ttl_s":10})"
	        "\n"
	        R"({"type":"warning","id":"sim_slow","message":"Simulation slow","ttl_s":10})"
	        "\n"
	        "garbage\n"
	        "[]\n"
	        R"({"type":"frame","t":1})"
	        "\n"
	        R"({"type":"metric","name":"Bad","value":1})"
	        "\n" +
	        std::string(max_connector_line + 1, 'z') + "\n" +
	        R"({"type":"metric","name":"sim_realtime_ratio","value":0.7})"
	        "\n";
	for (size_t i = 0; i < stream.size(); i += 7)
		buf.feed(std::string_view(stream).substr(i, 7), on_line);
	CHECK(sent == std::vector<std::string>{"Simulation slow"});
	CHECK(metrics.size() == 1 and metrics.at("sim_realtime_ratio") == 0.7); // last value wins
	CHECK(dropped == 5);

	sample s;
	s.app_metrics = metrics;
	s.app_metrics_dropped = dropped;
	s.app_warnings = st.active();
	auto line = to_json_line(s, {});
	CHECK(line.find(R"("app_metrics":{"sim_realtime_ratio":0.7})") != std::string::npos);
	CHECK(line.find(R"("app_metrics_dropped":5)") != std::string::npos);
	CHECK(line.find(R"("app_warnings":["sim_slow"])") != std::string::npos);
}
} // namespace

int main(int argc, char ** argv)
{
	if (argc < 2)
	{
		std::cerr << "usage: " << argv[0] << " <health_fixtures dir>" << std::endl;
		return 2;
	}
	test_recorded_sessions(argv[1]);
	test_persistence_and_cooldown();
	test_rules_do_not_cross_fire();
	test_thresholds();
	test_histogram();
	test_json_round_trip();
	test_judder_stats();
	test_judder_rule();
	test_connector_parse();
	test_connector_line_buffer();
	test_app_warning_cooldown();
	test_connector_end_to_end();
	if (failures)
	{
		std::cerr << failures << " check(s) failed" << std::endl;
		return 1;
	}
	std::cout << "health rules: all checks passed" << std::endl;
	return 0;
}
