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
#include <set>
#include <string>
#include <vector>

using namespace wivrn::health;

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

	for (auto id: {rule_id::link_loss, rule_id::app_slow, rule_id::refresh_mismatch})
		CHECK(rule_from_name(name(id)) == id);
	CHECK(!rule_from_name("bogus"));
	CHECK(rules().size() == 9);
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
	}
	CHECK(!from_json_line("not json"));
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
	if (failures)
	{
		std::cerr << failures << " check(s) failed" << std::endl;
		return 1;
	}
	std::cout << "health rules: all checks passed" << std::endl;
	return 0;
}
