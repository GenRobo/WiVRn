/*
 * WiVRn stream health: the per-application-frame record (GRID #3095 milestone 6a).
 *
 * Every frame path (compositor, application pacer, HMD, encoders, headset feedback) writes its
 * timestamps into one fixed ring of 4096 frames with relaxed atomic stores: no allocation, no
 * lock, nothing else on the frame path. The health monitor thread reads the ring once per
 * second and, when `health.frames` is on, serialises finished rows to
 * `wivrn-frames-<time>.jsonl` (schema in docs/health.md, "Frame record"). This unit has no
 * Monado or clock dependency so server/tests/health_frames.cpp drives it directly.
 */
#pragma once

#include <array>
#include <atomic>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <functional>
#include <mutex>
#include <nlohmann/json_fwd.hpp>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace wivrn::health
{

/// One application frame on one video stream: a snapshot of a ring slot. Times are server
/// monotonic nanoseconds, 0 = unknown; counts and flags -1 = unknown.
struct frame_row
{
	uint64_t n = 0;
	uint8_t s = 0;
	int64_t pd = 0;   // predicted display time (compositor)
	int64_t wake = 0; // application pacer: wake-up sent, application began, application submitted
	int64_t begin = 0;
	int64_t submit = 0;
	int64_t gpu = 0;   // application GPU work done
	int64_t comp0 = 0; // compositor begin / GPU work done
	int64_t comp1 = 0;
	int64_t enc0 = 0; // encoder begin / end
	int64_t enc1 = 0;
	int64_t send0 = 0; // first / last shard sent
	int64_t send1 = 0;
	int64_t rx0 = 0; // headset: first / last packet received
	int64_t rx1 = 0;
	int64_t dec0 = 0; // headset: sent to / received from the decoder
	int64_t dec1 = 0;
	int64_t blit = 0;
	int64_t disp = 0;
	int32_t shown = -1;  // times displayed, 0 = never
	int64_t bytes = -1;  // encoded size
	int32_t idr = -1;    // 1 when the frame was an IDR
	int32_t shards = -1; // shards sent
	int32_t lost = -1;   // shards the headset missed: 0 when complete, unknown otherwise
	int64_t pose_t = 0;  // tracking sample the application's view pose came from
	int32_t pose_flags = -1;
	static constexpr int64_t no_offset = INT64_MIN;
	int64_t off_ns = no_offset; // clock offset (headset - server) at feedback time
};

struct frame_header
{
	int64_t t0_mono_ns = 0;
	int64_t t0_unix_ms = 0;
	std::string host;
	std::string server;
	uint8_t streams = 0;
	float display_hz = 0;
	int64_t period_ns = 0;
};

/// The JSON lines of the record; every time becomes whole microseconds since t0, unknown is null.
std::string frame_line(const frame_row &, int64_t t0_mono_ns);
std::string frame_header_line(const frame_header &);
std::string frame_clock_line(int64_t t_ns, int64_t t0_mono_ns, int64_t off_ns, int64_t jitter_ns);
/// `value` is any JSON value (string, number, bool) or null when the event carries none.
std::string frame_event_line(int64_t t_ns, int64_t t0_mono_ns, std::string_view name, const nlohmann::json & value);
/// Inverse of frame_line, for the tests and the event dump; nullopt for a line of another type.
std::optional<frame_row> frame_from_line(std::string_view line, int64_t t0_mono_ns);

/// The ring the frame paths write. Slot n % size holds frame n from the compositor's
/// prediction (which resets it) until frame n + size takes it over. Each field is stored by
/// one thread; readers load the frame index before and after the fields to detect a takeover.
class frame_ring
{
public:
	static constexpr size_t size = 4096;
	static constexpr size_t streams = 3;

	/// Session start: forgets the previous session's frame indices (the compositor restarts at 0).
	void reset();
	void predicted(uint64_t n, int64_t pd_ns);
	void compositor_begin(uint64_t n, int64_t t_ns);
	void compositor_end(uint64_t n, int64_t t_ns);

	// Application pacer, when the compositor latches application frame into system frame n.
	struct app_times
	{
		int64_t wake, begin, submit, gpu;
		int64_t predicted_display; // the display time the application located its views for
	};
	void app_latched(uint64_t n, const app_times &);

	// HMD, the application's thread: the view pose handed out for display time `at_ns`.
	void view_pose(int64_t at_ns, int64_t pose_t_ns, uint32_t flags);

	// Encoder thread of stream s.
	void encode_begin(uint64_t n, uint8_t s, int64_t t_ns);
	void encode_end(uint64_t n, uint8_t s, int64_t t_ns);
	void send_begin(uint64_t n, uint8_t s, int64_t t_ns);
	void send_end(uint64_t n, uint8_t s, int64_t t_ns, uint32_t bytes, uint32_t shards, bool idr);

	// Network thread: headset feedback in server time.
	struct feedback_times
	{
		int64_t rx0, rx1, dec0, dec1, blit, disp;
		uint8_t shown;
		bool complete; // the headset decoded it (no shard missing)
		int64_t off_ns;
	};
	void feedback(uint64_t n, uint8_t s, const feedback_times &);

	// Monitor thread.
	/// nullopt when the slot holds another frame (not yet predicted, or taken over).
	std::optional<frame_row> read(uint64_t n, uint8_t s) const;
	/// Highest frame index predicted, -1 before the first.
	int64_t latest() const;
	/// Highest frame index the headset reported displayed on stream s, -1 before the first.
	int64_t latest_displayed(uint8_t s) const;

private:
	struct stream_slot
	{
		std::atomic<int64_t> enc0, enc1, send0, send1, rx0, rx1, dec0, dec1, blit, disp, off_ns;
		std::atomic<int32_t> bytes, shards, shown, idr, lost;
	};
	struct slot
	{
		std::atomic<int64_t> n{-1};
		std::atomic<int64_t> pd, wake, begin, submit, gpu, comp0, comp1, pose_t;
		std::atomic<int32_t> pose_flags;
		std::array<stream_slot, streams> st;
	};
	struct pose_entry
	{
		std::atomic<int64_t> at, pose_t;
		std::atomic<uint32_t> flags;
	};
	// Keyed by the display time's millisecond: a frame is latched within a few periods of the
	// application locating its views, far less than this 256 ms window.
	static constexpr size_t poses = 256;

	std::array<slot, size> slots;
	std::array<pose_entry, poses> pose_table;
	std::atomic<int64_t> latest_n{-1};
	std::array<std::atomic<int64_t>, streams> displayed_n{-1, -1, -1};

	static slot & at(std::array<slot, size> & s, uint64_t n)
	{
		return s[n % size];
	}
	slot & at(uint64_t n)
	{
		return at(slots, n);
	}
};

/// The process-wide ring; the compositor, pacer, HMD and encoders have no session handle.
frame_ring & frames();

/// Timestamped events from code without a session handle (the IDR handler's requests), drained
/// by the monitor once per second. Mutex-protected: nothing per frame goes through here.
class frame_events
{
public:
	struct event
	{
		int64_t t_ns;
		std::string name;
		std::string value;
	};
	void push(int64_t t_ns, std::string name, std::string value);
	std::vector<event> take();

private:
	std::mutex mutex;
	std::vector<event> pending;
};
frame_events & events();

} // namespace wivrn::health
