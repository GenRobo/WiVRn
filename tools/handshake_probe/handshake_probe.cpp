// Test tool: replays the WiVRn headset-side TCP handshake (client/wivrn_client.cpp) against a server,
// so the Windows server's handshake can be tested without a headset.
// Usage: wivrn-handshake-probe <host-ipv4> <port> <pin|known|hangup> [session-seconds]
// With session-seconds > 0 it then acts as a minimal headset (headset_info, FOCUSED, timesync replies)
// and reports what the server streams (video shards etc.), to exercise the encoder without a headset.
#include "crypto.h"
#include "protocol_version.h"
#include "secrets.h"
#include "smp.h"
#include "wivrn_packets.h"
#include "wivrn_sockets.h"

#include <array>
#include <chrono>
#include <cmath>
#include <deque>
#include <fstream>
#include <iostream>
#include <map>
#include <ranges>
#include <string_view>
#include <vector>
#include <thread>
#include <typeinfo>

using namespace wivrn;
using namespace std::chrono_literals;

int main(int argc, char ** argv)
{
	if (argc < 4)
	{
		std::cerr << "usage: " << argv[0] << " host port pin|known" << std::endl;
		return 2;
	}
	init_sockets();
	sockaddr_in addr{};
	addr.sin_family = AF_INET;
	addr.sin_port = htons(std::stoi(argv[2]));
	inet_pton(AF_INET, argv[1], &addr.sin_addr);

	typed_socket<TCP, to_headset::packets, from_headset::packets> control(addr);
	auto t0 = std::chrono::steady_clock::now();
	auto log = [&](auto &&... args) {
		std::cerr << "[" << std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - t0).count() << "ms] ";
		(std::cerr << ... << args) << std::endl;
	};

	auto receive = [&](std::chrono::seconds timeout) -> to_headset::packets {
		auto end = std::chrono::steady_clock::now() + timeout;
		while (std::chrono::steady_clock::now() < end)
		{
			pollfd_t fd{};
			fd.fd = control.get_fd();
			fd.events = POLLIN;
			int r = poll_sockets(&fd, 1, 100);
			if (r < 0)
				throw std::system_error(last_socket_error(), std::system_category(), "poll");
			if (fd.revents & (POLLHUP | POLLERR))
				throw std::runtime_error("server closed/errored the control socket, revents=" + std::to_string(fd.revents));
			if (fd.revents & POLLIN)
				if (auto p = control.receive())
					return std::move(*p);
		}
		throw std::runtime_error("timeout");
	};

	try
	{
		crypto::key keypair = crypto::key::generate_x448_keypair();
		control.send(from_headset::crypto_handshake{
		        .protocol_version = wivrn::protocol_version,
		        .public_key = keypair.public_key(),
		        .name = "handshake-probe",
		});
		log("sent crypto_handshake");

		auto ch = std::get<to_headset::crypto_handshake>(receive(10s));
		log("got crypto_handshake state=", int(ch.state));
		if (std::string_view(argv[3]) == "hangup")
		{
			log("hanging up like a cancelled PIN prompt");
			return 0;
		}
		std::string pin = "000000";
		using cs = to_headset::crypto_handshake::crypto_state;
		if (ch.state == cs::pin_needed)
		{
			pin = argv[3];
			crypto::smp smp;
			control.send(from_headset::pin_check_1{smp.step1(pin)});
			auto m2 = std::get<to_headset::pin_check_2>(receive(10s)).message;
			log("got pin_check_2");
			control.send(from_headset::pin_check_3{smp.step3(m2)});
			auto m4 = std::get<to_headset::pin_check_4>(receive(10s)).message;
			log("got pin_check_4, match=", smp.step5(m4));
		}
		if (ch.state == cs::pin_needed or ch.state == cs::client_already_paired)
		{
			crypto::key server_key = crypto::key::from_public_key(ch.public_key);
			secrets s{keypair, server_key, pin};
			control.set_aes_key_and_ivs(s.control_key, s.control_iv_to_headset, s.control_iv_from_headset);
		}
		control.send(from_headset::crypto_handshake{});
		auto h = std::get<to_headset::handshake>(receive(10s));
		log("got handshake stream_port=", h.stream_port);

		// TCP-only (like wivrn+tcp://): second handshake on the control socket
		control.send(from_headset::handshake{});
		auto h2 = std::get<to_headset::handshake>(receive(10s));
		log("got second handshake stream_port=", h2.stream_port, " -> handshake OK (server now waits for headset_info_packet)");

		// Optional: act as a minimal headset for <seconds> (argv[4]) and count what the server streams.
		int session_seconds = argc > 4 ? std::stoi(argv[4]) : 0;
		if (session_seconds <= 0)
		{
			std::this_thread::sleep_for(1s);
			return 0;
		}

		const XrFovf fov{-0.9f, 0.9f, 0.9f, -0.9f};
		// PROBE_CODECS=h265,h264 (preferred first) and PROBE_BIT_DEPTH=8|10, like the headset app's video settings
		std::vector<video_codec> codecs;
		std::string codec_env = std::getenv("PROBE_CODECS") ? std::getenv("PROBE_CODECS") : "h265,h264";
		for (auto part: std::views::split(codec_env, ','))
		{
			std::string_view c(part.begin(), part.end());
			codecs.push_back(c == "h264" ? video_codec::h264 : c == "av1" ? video_codec::av1 : video_codec::h265);
		}
		uint8_t bit_depth = std::getenv("PROBE_BIT_DEPTH") ? uint8_t(std::stoi(std::getenv("PROBE_BIT_DEPTH"))) : 8;
		log("advertising codecs=", codec_env, " bit_depth=", int(bit_depth));
		control.send(from_headset::headset_info_packet{
		        .render_eye_width = 1280,
		        .render_eye_height = 1344,
		        .stream_eye_width = 1280,
		        .stream_eye_height = 1344,
		        .available_refresh_rates = {72.f, 90.f},
		        .settings = {.preferred_refresh_rate = 72.f, .minimum_refresh_rate = 72.f, .bitrate_bps = 25'000'000},
		        .fov = {fov, fov},
		        .supported_codecs = codecs,
		        .bit_depth = bit_depth,
		        .system_name = "handshake-probe",
		        .language = "en",
		        .country = "US",
		});
		control.send(from_headset::session_state_changed{.state = XR_SESSION_STATE_FOCUSED});
		log("sent headset_info + FOCUSED; acting as a headset for ", session_seconds, " s");

		std::map<std::string, size_t> counts;
		size_t video_bytes = 0;
		std::array<size_t, 3> frames{}, stream_bytes{};
		std::array<std::ofstream, 3> dumps;
		const char * dump_prefix = std::getenv("PROBE_DUMP"); // e.g. D:\x\probe -> probe-0.bin ...
		if (dump_prefix)
			for (int i = 0; i < 3; ++i)
				dumps[i].open(std::string(dump_prefix) + "-" + std::to_string(i) + ".bin", std::ios::binary);
		auto now_ns = [] { return (int64_t)std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch()).count(); };
		auto end = std::chrono::steady_clock::now() + std::chrono::seconds(session_seconds);
		auto next_report = std::chrono::steady_clock::now() + 2s;
		auto next_tracking = std::chrono::steady_clock::now();
		std::deque<std::pair<std::chrono::steady_clock::time_point, from_headset::feedback>> pending_acks;
		const auto ack_delay = std::chrono::milliseconds(std::getenv("PROBE_ACK_DELAY_MS") ? std::stoi(std::getenv("PROBE_ACK_DELAY_MS")) : 0);
		log("ack delay ", ack_delay.count(), " ms");
		while (std::chrono::steady_clock::now() < end)
		{
			// Send acks once their simulated decode latency has elapsed (PROBE_ACK_DELAY_MS, default 0)
			while (not pending_acks.empty() and pending_acks.front().first <= std::chrono::steady_clock::now())
			{
				control.send(std::move(pending_acks.front().second));
				pending_acks.pop_front();
			}
			if (std::chrono::steady_clock::now() >= next_tracking)
			{
				// Head pose + eye views, like the real client, so apps render a valid (non-black) scene.
				// The head slowly turns so frames differ (and P-frames are non-trivial).
				next_tracking += 10ms;
				auto t = now_ns();
				float yaw = 0.3f * std::sin(t * 1e-9f);
				XrQuaternionf q{0, std::sin(yaw / 2), 0, std::cos(yaw / 2)};
				const uint8_t all = from_headset::pose_flags::orientation_valid | from_headset::pose_flags::position_valid | from_headset::pose_flags::orientation_tracked | from_headset::pose_flags::position_tracked;
				from_headset::tracking tr{};
				tr.production_timestamp = t;
				tr.timestamp = t;
				tr.view_flags = XR_VIEW_STATE_ORIENTATION_VALID_BIT | XR_VIEW_STATE_POSITION_VALID_BIT | XR_VIEW_STATE_ORIENTATION_TRACKED_BIT | XR_VIEW_STATE_POSITION_TRACKED_BIT;
				tr.views[0] = {.pose = {{0, 0, 0, 1}, {-0.032f, 0, 0}}, .fov = fov};
				tr.views[1] = {.pose = {{0, 0, 0, 1}, {0.032f, 0, 0}}, .fov = fov};
				tr.device_poses.push_back({.pose = {q, {0, 1.6f, 0}}, .device = device_id::HEAD, .flags = all});
				control.send(std::move(tr));
			}

			pollfd_t fd{};
			fd.fd = control.get_fd();
			fd.events = POLLIN;
			if (poll_sockets(&fd, 1, 5) > 0)
			{
				if (fd.revents & (POLLHUP | POLLERR))
					throw std::runtime_error("server closed the control socket during the session");
				auto handle = [&](to_headset::packets && packet) {
					std::visit([&](auto && pkt) {
						using T = std::remove_cvref_t<decltype(pkt)>;
						counts[typeid(T).name()]++;
						if constexpr (std::is_same_v<T, to_headset::timesync_query>)
						{
							control.send(from_headset::timesync_response{
							        .query = pkt.query,
							        .response = std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch()).count(),
							});
						}
						else if constexpr (std::is_same_v<T, to_headset::video_stream_data_shard>)
						{
							video_bytes += pkt.payload.size();
							auto s = pkt.stream_item_idx % 3;
							stream_bytes[s] += pkt.payload.size();
							if (dumps[s])
								dumps[s].write((const char *)pkt.payload.data(), pkt.payload.size());
							if (pkt.timing_info)
							{
								// Last shard of a frame: ack it like the real client does after decoding,
								// otherwise the encoder's reference handler stops encoding (should_skip).
								frames[s]++;
								auto t = now_ns();
								pending_acks.push_back({std::chrono::steady_clock::now() + ack_delay, from_headset::feedback{
								        .frame_index = pkt.frame_idx,
								        .stream_index = pkt.stream_item_idx,
								        .received_first_packet = t,
								        .received_last_packet = t,
								        .sent_to_decoder = t,
								        .received_from_decoder = t,
								        .blitted = t,
								        .displayed = t,
								        .times_displayed = 1,
								}});
							}
						}
						else if constexpr (std::is_same_v<T, to_headset::video_stream_description>)
							log("video_stream_description ", pkt.width, "x", pkt.height, " @", pkt.frame_rate);
						else if constexpr (std::is_same_v<T, to_headset::server_message>)
							log("server_message: ", pkt.msg);
					},
					           std::move(packet));
				};
				if (auto p = control.receive())
					handle(std::move(*p));
				while (auto p = control.receive_pending())
					handle(std::move(*p));
			}
			if (std::chrono::steady_clock::now() > next_report)
			{
				next_report += 2s;
				std::string s;
				for (auto & [k, v]: counts)
					s += std::string(k).substr(std::string(k).rfind(':') + 1) + "=" + std::to_string(v) + " ";
				log("received: ", s, "video_bytes=", video_bytes, " frames per stream=", frames[0], "/", frames[1], "/", frames[2],
				    " bytes per stream=", stream_bytes[0], "/", stream_bytes[1], "/", stream_bytes[2]);
			}
		}
		log("session done, video_bytes=", video_bytes);
		return 0;
	}
	catch (std::exception & e)
	{
		log("FAILED: ", e.what());
		return 1;
	}
}
