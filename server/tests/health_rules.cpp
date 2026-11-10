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
	CHECK(to_json_line(clean_at(0), {}).find("app_") == std::string::npos); // nothing posted: keys absent
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
