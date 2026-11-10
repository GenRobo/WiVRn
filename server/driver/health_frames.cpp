/*
 * WiVRn stream health: the per-application-frame record (GRID #3095 milestone 6a). See health_frames.h.
 */
#include "health_frames.h"

#include <algorithm>
#include <nlohmann/json.hpp>
#include <system_error>
#include <utility>

namespace wivrn::health
{

namespace
{
constexpr auto relaxed = std::memory_order_relaxed;

nlohmann::json us_or_null(int64_t t_ns, int64_t t0_ns)
{
	if (t_ns == 0)
		return nullptr;
	return (t_ns - t0_ns) / 1000;
}

template <typename T>
nlohmann::json count_or_null(T v)
{
	if (v < 0)
		return nullptr;
	return v;
}

int64_t ns_from(const nlohmann::json & j, const char * key, int64_t t0_ns)
{
	auto it = j.find(key);
	if (it == j.end() or it->is_null())
		return 0;
	return it->get<int64_t>() * 1000 + t0_ns;
}

template <typename T>
T count_from(const nlohmann::json & j, const char * key)
{
	auto it = j.find(key);
	if (it == j.end() or it->is_null())
		return T(-1);
	return it->get<T>();
}
} // namespace

std::string frame_line(const frame_row & r, int64_t t0)
{
	nlohmann::json j{
	        {"type", "frame"},
	        {"n", r.n},
	        {"s", r.s},
	        {"pd", us_or_null(r.pd, t0)},
	        {"wake", us_or_null(r.wake, t0)},
	        {"begin", us_or_null(r.begin, t0)},
	        {"submit", us_or_null(r.submit, t0)},
	        {"gpu", us_or_null(r.gpu, t0)},
	        {"comp0", us_or_null(r.comp0, t0)},
	        {"comp1", us_or_null(r.comp1, t0)},
	        {"enc0", us_or_null(r.enc0, t0)},
	        {"enc1", us_or_null(r.enc1, t0)},
	        {"send0", us_or_null(r.send0, t0)},
	        {"send1", us_or_null(r.send1, t0)},
	        {"rx0", us_or_null(r.rx0, t0)},
	        {"rx1", us_or_null(r.rx1, t0)},
	        {"dec0", us_or_null(r.dec0, t0)},
	        {"dec1", us_or_null(r.dec1, t0)},
	        {"blit", us_or_null(r.blit, t0)},
	        {"disp", us_or_null(r.disp, t0)},
	        {"shown", count_or_null(r.shown)},
	        {"bytes", count_or_null(r.bytes)},
	        {"idr", count_or_null(r.idr)},
	        {"shards", count_or_null(r.shards)},
	        {"lost", count_or_null(r.lost)},
	        {"pose_t", us_or_null(r.pose_t, t0)},
	        {"pose_flags", count_or_null(r.pose_flags)},
	        {"off_us", r.off_ns == frame_row::no_offset ? nlohmann::json(nullptr) : nlohmann::json(double(r.off_ns) / 1000.0)},
	};
	return j.dump();
}

std::optional<frame_row> frame_from_line(std::string_view line, int64_t t0)
{
	auto j = nlohmann::json::parse(line, nullptr, false);
	if (not j.is_object() or j.value("type", "") != "frame" or not j.contains("n") or not j.contains("s"))
		return std::nullopt;
	frame_row r;
	r.n = j.at("n").get<uint64_t>();
	r.s = j.at("s").get<uint8_t>();
	r.pd = ns_from(j, "pd", t0);
	r.wake = ns_from(j, "wake", t0);
	r.begin = ns_from(j, "begin", t0);
	r.submit = ns_from(j, "submit", t0);
	r.gpu = ns_from(j, "gpu", t0);
	r.comp0 = ns_from(j, "comp0", t0);
	r.comp1 = ns_from(j, "comp1", t0);
	r.enc0 = ns_from(j, "enc0", t0);
	r.enc1 = ns_from(j, "enc1", t0);
	r.send0 = ns_from(j, "send0", t0);
	r.send1 = ns_from(j, "send1", t0);
	r.rx0 = ns_from(j, "rx0", t0);
	r.rx1 = ns_from(j, "rx1", t0);
	r.dec0 = ns_from(j, "dec0", t0);
	r.dec1 = ns_from(j, "dec1", t0);
	r.blit = ns_from(j, "blit", t0);
	r.disp = ns_from(j, "disp", t0);
	r.shown = count_from<int32_t>(j, "shown");
	r.bytes = count_from<int64_t>(j, "bytes");
	r.idr = count_from<int32_t>(j, "idr");
	r.shards = count_from<int32_t>(j, "shards");
	r.lost = count_from<int32_t>(j, "lost");
	r.pose_t = ns_from(j, "pose_t", t0);
	r.pose_flags = count_from<int32_t>(j, "pose_flags");
	if (auto it = j.find("off_us"); it != j.end() and not it->is_null())
		r.off_ns = int64_t(it->get<double>() * 1000.0);
	return r;
}

std::string frame_header_line(const frame_header & h)
{
	return nlohmann::json{
	        {"type", "header"},
	        {"version", 1},
	        {"t0_mono_ns", h.t0_mono_ns},
	        {"t0_unix_ms", h.t0_unix_ms},
	        {"host", h.host},
	        {"server", h.server},
	        {"streams", h.streams},
	        {"display_hz", h.display_hz},
	        {"period_ns", h.period_ns},
	}
	        .dump();
}

std::string frame_clock_line(int64_t t_ns, int64_t t0, int64_t off_ns, int64_t jitter_ns)
{
	// `off_ns` is the raw headset − server monotonic offset, hundreds of seconds when the two
	// booted at different times; the reader wants its drift, so the whole value is kept.
	bool known = off_ns != frame_row::no_offset;
	return nlohmann::json{
	        {"type", "clock"},
	        {"t", (t_ns - t0) / 1000},
	        {"off_us", known ? nlohmann::json(double(off_ns) / 1000.0) : nlohmann::json(nullptr)},
	        {"jitter_us", known ? nlohmann::json(double(jitter_ns) / 1000.0) : nlohmann::json(nullptr)},
	}
	        .dump();
}

std::string frame_event_line(int64_t t_ns, int64_t t0, std::string_view name, const nlohmann::json & value)
{
	return nlohmann::json{
	        {"type", "event"},
	        {"t", (t_ns - t0) / 1000},
	        {"name", name},
	        {"value", value},
	}
	        .dump();
}

// --- ring ----------------------------------------------------------------------------------

void frame_ring::reset()
{
	latest_n.store(-1, std::memory_order_release);
	for (auto & d: displayed_n)
		d.store(-1, relaxed);
}

void frame_ring::predicted(uint64_t n, int64_t pd_ns)
{
	slot & s = at(n);
	s.n.store(-1, relaxed);
	for (auto * f: {&s.wake, &s.begin, &s.submit, &s.gpu, &s.comp0, &s.comp1, &s.pose_t})
		f->store(0, relaxed);
	s.pose_flags.store(-1, relaxed);
	for (auto & st: s.st)
	{
		for (auto * f: {&st.enc0, &st.enc1, &st.send0, &st.send1, &st.rx0, &st.rx1, &st.dec0, &st.dec1, &st.blit, &st.disp})
			f->store(0, relaxed);
		st.off_ns.store(frame_row::no_offset, relaxed);
		for (auto * c: {&st.bytes, &st.shards, &st.shown, &st.idr, &st.lost})
			c->store(-1, relaxed);
	}
	s.pd.store(pd_ns, relaxed);
	s.n.store(int64_t(n), std::memory_order_release);
	latest_n.store(int64_t(n), std::memory_order_release);
}

void frame_ring::compositor_begin(uint64_t n, int64_t t_ns)
{
	at(n).comp0.store(t_ns, relaxed);
}

void frame_ring::compositor_end(uint64_t n, int64_t t_ns)
{
	at(n).comp1.store(t_ns, relaxed);
}

void frame_ring::app_latched(uint64_t n, const app_times & a)
{
	slot & s = at(n);
	s.wake.store(a.wake, relaxed);
	s.begin.store(a.begin, relaxed);
	s.submit.store(a.submit, relaxed);
	s.gpu.store(a.gpu, relaxed);
	if (a.predicted_display == 0)
		return;
	const pose_entry & p = pose_table[uint64_t(a.predicted_display / 1'000'000) % poses];
	if (p.at.load(std::memory_order_acquire) != a.predicted_display)
		return;
	int64_t pose_t = p.pose_t.load(relaxed);
	uint32_t flags = p.flags.load(relaxed);
	if (p.at.load(std::memory_order_acquire) != a.predicted_display)
		return;
	s.pose_t.store(pose_t, relaxed);
	s.pose_flags.store(int32_t(flags), relaxed);
}

void frame_ring::view_pose(int64_t at_ns, int64_t pose_t_ns, uint32_t flags)
{
	pose_entry & p = pose_table[uint64_t(at_ns / 1'000'000) % poses];
	p.at.store(0, relaxed);
	p.pose_t.store(pose_t_ns, relaxed);
	p.flags.store(flags, relaxed);
	p.at.store(at_ns, std::memory_order_release);
}

void frame_ring::encode_begin(uint64_t n, uint8_t s, int64_t t_ns)
{
	at(n).st[s % streams].enc0.store(t_ns, relaxed);
}

void frame_ring::encode_end(uint64_t n, uint8_t s, int64_t t_ns)
{
	at(n).st[s % streams].enc1.store(t_ns, relaxed);
}

void frame_ring::send_begin(uint64_t n, uint8_t s, int64_t t_ns)
{
	at(n).st[s % streams].send0.store(t_ns, relaxed);
}

void frame_ring::send_end(uint64_t n, uint8_t s, int64_t t_ns, uint32_t bytes, uint32_t shards, bool idr)
{
	stream_slot & st = at(n).st[s % streams];
	st.send1.store(t_ns, relaxed);
	st.bytes.store(int32_t(std::min<uint32_t>(bytes, INT32_MAX)), relaxed);
	st.shards.store(int32_t(std::min<uint32_t>(shards, INT32_MAX)), relaxed);
	st.idr.store(idr ? 1 : 0, relaxed);
}

void frame_ring::feedback(uint64_t n, uint8_t s, const feedback_times & f)
{
	s %= streams;
	stream_slot & st = at(n).st[s];
	// A frame reports several times (on reception, then once per display); each report is at
	// least as complete as the previous one, so nothing known is ever overwritten by zero.
	if (f.rx0)
		st.rx0.store(f.rx0, relaxed);
	if (f.rx1)
		st.rx1.store(f.rx1, relaxed);
	if (f.dec0)
		st.dec0.store(f.dec0, relaxed);
	if (f.dec1)
		st.dec1.store(f.dec1, relaxed);
	if (f.blit)
		st.blit.store(f.blit, relaxed);
	if (f.disp)
		st.disp.store(f.disp, relaxed);
	st.shown.store(f.shown, relaxed);
	st.lost.store(f.complete ? 0 : -1, relaxed);
	st.off_ns.store(f.off_ns, relaxed);
	if (f.disp)
	{
		int64_t prev = displayed_n[s].load(relaxed);
		while (prev < int64_t(n) and not displayed_n[s].compare_exchange_weak(prev, int64_t(n), relaxed))
		{
		}
	}
}

std::optional<frame_row> frame_ring::read(uint64_t n, uint8_t s) const
{
	const slot & sl = slots[n % size];
	if (sl.n.load(std::memory_order_acquire) != int64_t(n))
		return std::nullopt;
	frame_row r;
	r.n = n;
	r.s = s;
	r.pd = sl.pd.load(relaxed);
	r.wake = sl.wake.load(relaxed);
	r.begin = sl.begin.load(relaxed);
	r.submit = sl.submit.load(relaxed);
	r.gpu = sl.gpu.load(relaxed);
	r.comp0 = sl.comp0.load(relaxed);
	r.comp1 = sl.comp1.load(relaxed);
	r.pose_t = sl.pose_t.load(relaxed);
	r.pose_flags = sl.pose_flags.load(relaxed);
	const stream_slot & st = sl.st[s % streams];
	r.enc0 = st.enc0.load(relaxed);
	r.enc1 = st.enc1.load(relaxed);
	r.send0 = st.send0.load(relaxed);
	r.send1 = st.send1.load(relaxed);
	r.rx0 = st.rx0.load(relaxed);
	r.rx1 = st.rx1.load(relaxed);
	r.dec0 = st.dec0.load(relaxed);
	r.dec1 = st.dec1.load(relaxed);
	r.blit = st.blit.load(relaxed);
	r.disp = st.disp.load(relaxed);
	r.shown = st.shown.load(relaxed);
	r.bytes = st.bytes.load(relaxed);
	r.idr = st.idr.load(relaxed);
	r.shards = st.shards.load(relaxed);
	r.lost = st.lost.load(relaxed);
	r.off_ns = st.off_ns.load(relaxed);
	if (sl.n.load(std::memory_order_acquire) != int64_t(n))
		return std::nullopt;
	return r;
}

int64_t frame_ring::latest() const
{
	return latest_n.load(std::memory_order_acquire);
}

int64_t frame_ring::latest_displayed(uint8_t s) const
{
	return displayed_n[s % streams].load(relaxed);
}

frame_ring & frames()
{
	static frame_ring ring;
	return ring;
}

void frame_events::push(int64_t t_ns, std::string name, std::string value)
{
	std::lock_guard lock(mutex);
	if (pending.size() < 1024)
		pending.push_back({t_ns, std::move(name), std::move(value)});
}

std::vector<frame_events::event> frame_events::take()
{
	std::lock_guard lock(mutex);
	return std::exchange(pending, {});
}

frame_events & events()
{
	static frame_events queue;
	return queue;
}

// --- writer --------------------------------------------------------------------------------

frame_writer::frame_writer(std::filesystem::path dir_, size_t max_bytes_, int keep_, std::function<std::string()> stamp_) :
        dir(std::move(dir_)), max_bytes(max_bytes_), keep(keep_), stamp(std::move(stamp_))
{
}

frame_writer::~frame_writer() = default;

std::filesystem::path frame_writer::current() const
{
	return paths.empty() ? std::filesystem::path{} : paths.back();
}

void frame_writer::open()
{
	out.close();
	std::error_code ec;
	std::filesystem::create_directories(dir, ec);
	while (keep > 0 and paths.size() >= size_t(keep))
	{
		std::filesystem::remove(paths.front(), ec);
		paths.erase(paths.begin());
	}
	paths.push_back(dir / ("wivrn-frames-" + stamp() + ".jsonl"));
	out.open(paths.back(), std::ios::out | std::ios::trunc | std::ios::binary);
	written = 0;
	out << header << '\n';
	written += header.size() + 1;
}

void frame_writer::write(const std::string & line)
{
	if (written + line.size() + 1 > max_bytes)
		open();
	out << line << '\n';
	written += line.size() + 1;
	if (on_line)
		on_line(line);
}

void frame_writer::start(const frame_ring & ring, const frame_header & h)
{
	t0 = h.t0_mono_ns;
	stream_count = std::min<uint8_t>(h.streams, frame_ring::streams);
	header = frame_header_line(h);
	open();
	if (on_line)
		on_line(header);
	// Start at the oldest frame predicted at or after t0 (the ring is older than the session).
	int64_t first = ring.latest() + 1;
	while (first > 0)
	{
		auto row = ring.read(uint64_t(first - 1), 0);
		if (not row or row->pd < t0)
			break;
		--first;
	}
	next_n.fill(first);
}

void frame_writer::tick(const frame_ring & ring, int64_t now_ns, int64_t off_ns, int64_t jitter_ns)
{
	if (not started())
		return;
	settled_rows(ring, next_n, stream_count, now_ns, [this](const frame_row & row) { write(frame_line(row, t0)); });
	write(frame_clock_line(now_ns, t0, off_ns, jitter_ns));
	out.flush();
}

void frame_writer::event(int64_t t_ns, std::string_view name, const nlohmann::json & value)
{
	if (not started())
		return;
	write(frame_event_line(t_ns, t0, name, value));
	out.flush();
}

} // namespace wivrn::health
