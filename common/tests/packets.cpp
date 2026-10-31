// Contract test for the stream health packets (GRID #2834, design §6.4 and §9):
// from_headset::telemetry and to_headset::health serialise and deserialise unchanged, and
// adding them changes the protocol version so an old headset app is refused at the lobby.
//
// Skipped (exit 77) until milestone 3 defines the packets: WIVRN_HEALTH_PACKETS is set by
// common/CMakeLists.txt once wivrn_packets.h declares them.
#include "protocol_version.h"
#include "wivrn_packets.h"
#include "wivrn_serialization.h"

#include <algorithm>
#include <cstring>
#include <iostream>
#include <memory>
#include <span>
#include <vector>

#ifndef WIVRN_HEALTH_PACKETS
int main()
{
	std::cout << "SKIP: health packets not defined yet (GRID #2834 milestone 3)" << std::endl;
	return 77;
}
#else
using namespace wivrn;

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
template <typename Packets, typename T>
T round_trip(const T & value)
{
	serialization_packet out;
	out.serialize(Packets{value});
	std::vector<uint8_t> bytes;
	for (auto span: static_cast<std::vector<std::span<uint8_t>> &>(out))
		bytes.insert(bytes.end(), span.begin(), span.end());
	auto memory = std::shared_ptr<uint8_t[]>(new uint8_t[bytes.size()]);
	std::copy(bytes.begin(), bytes.end(), memory.get());
	deserialization_packet in(memory, std::span<uint8_t>(memory.get(), bytes.size()));
	return std::get<T>(in.deserialize<Packets>());
}
} // namespace

int main()
{
	from_headset::telemetry t{
	        .frames_received = 600,
	        .frames_incomplete = 7,
	        .frames_decoded = 593,
	        .frames_dropped_before_display = 2,
	        .frames_repeated = 3,
	        .decode_ms_p95_x10 = 41,
	        .gpu_ms_x10 = 23,
	        .cpu_ms_x10 = 12,
	        .display_hz_active_x10 = 1200,
	        .thermal_status = 2,
	        .cpu_level = 3,
	        .gpu_level = 4,
	        .network_kind = 3,
	        .rssi_dbm = -61,
	        .link_mbps = 1000,
	};
	auto t2 = round_trip<from_headset::packets>(t);
	CHECK(std::memcmp(&t, &t2, sizeof(t)) == 0);

	to_headset::health h{};
	h.link.idr_lost_ref = 8;
	h.app.hz = 119.8f;
	h.headset.thermal_status = 2;
	auto h2 = round_trip<to_headset::packets>(h);
	CHECK(h2.link.idr_lost_ref == 8);
	CHECK(h2.app.hz == h.app.hz);
	CHECK(h2.headset.thermal_status == 2);

	// The protocol version of a build with the packets differs from the last release without them.
	CHECK(protocol_version != WIVRN_PROTOCOL_VERSION_BEFORE_HEALTH);

	if (failures)
		return 1;
	std::cout << "health packets: all checks passed" << std::endl;
	return 0;
}
#endif
