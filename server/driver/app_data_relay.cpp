/*
 * WiVRn VR streaming
 * Copyright (C) 2026  General Robotics
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

#include "app_data_relay.h"

#include "wivrn_session.h"

#include "util/u_logging.h"

#include <algorithm>
#include <cassert>
#include <chrono>
#include <cinttypes>
#include <system_error>

#ifdef _WIN32
#include <ws2tcpip.h>
#else
#include <fcntl.h>
#include <netdb.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

using namespace std::chrono_literals;

namespace
{
#ifdef _WIN32
constexpr int send_flags = 0;
#else
// A sink that went away must not kill the server with SIGPIPE
constexpr int send_flags = MSG_NOSIGNAL;
#endif

std::system_error socket_error()
{
	return std::system_error{wivrn::last_socket_error(), wivrn::socket_error_category()};
}

std::string socket_error_message()
{
	return socket_error().code().message();
}

void set_nonblocking(wivrn::socket_t fd, bool nonblocking)
{
#ifdef _WIN32
	u_long mode = nonblocking;
	if (ioctlsocket(fd, FIONBIO, &mode) != 0)
		throw socket_error();
#else
	int flags = fcntl(fd, F_GETFL);
	if (flags < 0 or fcntl(fd, F_SETFL, nonblocking ? (flags | O_NONBLOCK) : (flags & ~O_NONBLOCK)) < 0)
		throw socket_error();
#endif
}

bool would_block(int error)
{
#ifdef _WIN32
	return error == WSAEWOULDBLOCK;
#else
	return error == EAGAIN or error == EWOULDBLOCK or error == EINPROGRESS;
#endif
}

// Receive and discard pending datagrams on a non-blocking socket
void drain(wivrn::socket_t fd)
{
	char buffer[64];
	while (::recv(fd, buffer, sizeof(buffer), 0) > 0)
	{
	}
}

std::string describe(const addrinfo & ai)
{
	char host[NI_MAXHOST];
	char port[NI_MAXSERV];
	if (getnameinfo(ai.ai_addr, (socklen_t)ai.ai_addrlen, host, sizeof(host), port, sizeof(port), NI_NUMERICHOST | NI_NUMERICSERV) != 0)
		return "?";
	return (ai.ai_family == AF_INET6 ? "[" + std::string(host) + "]" : std::string(host)) + ":" + port;
}
} // namespace

namespace wivrn
{

app_data_relay::app_data_relay(wivrn_session & session, configuration::app_data_sink sink) :
        session(session), sink(std::move(sink))
{
	auto [a, b] = make_datagram_pair();
	wake_sender = fd_base(a);
	wake_receiver = fd_base(b);
	// A wake-up is a hint: never block the session's network thread on it, never block the drain
	set_nonblocking(wake_sender.get_fd(), true);
	set_nonblocking(wake_receiver.get_fd(), true);
}

app_data_relay::~app_data_relay()
{
	pause();
}

void app_data_relay::resume()
{
	if (thread.joinable())
		return;
	bytes_to_headset = 0;
	bytes_to_sink = 0;
	bytes_dropped = 0;
	thread = std::jthread([this](std::stop_token stop) { run(stop); });
}

void app_data_relay::pause()
{
	if (not thread.joinable())
		return;
	thread.request_stop();
	::send(wake_sender.get_fd(), "", 1, 0);
	thread.join();
	drop_queue();
}

void app_data_relay::from_headset(std::vector<uint8_t> && data)
{
	std::lock_guard lock(mutex);
	if (not connected)
	{
		bytes_dropped += data.size();
		return;
	}
	if (queued_bytes + data.size() > max_queued_bytes)
	{
		// A missing chunk corrupts application framing: end this stream, never resume it.
		bytes_dropped += data.size();
		connected = false;
		overflowed = true;
		::send(wake_sender.get_fd(), "", 1, 0);
		return;
	}
	queued_bytes += data.size();
	queue.push_back(std::move(data));
	// EAGAIN means a wake-up is already pending
	::send(wake_sender.get_fd(), "", 1, 0);
}

void app_data_relay::drop_queue()
{
	std::lock_guard lock(mutex);
	connected = false;
	for (const auto & data: queue)
		bytes_dropped += data.size();
	queue.clear();
	queued_bytes = 0;
}

void app_data_relay::log_counters()
{
	U_LOG_I("app-data %s: %" PRIu64 " B sink->headset, %" PRIu64 " B headset->sink, %" PRIu64 " B dropped",
	        sink.channel.c_str(),
	        bytes_to_headset.load(),
	        bytes_to_sink.load(),
	        bytes_dropped.load());
}

void app_data_relay::run(std::stop_token stop)
{
	U_LOG_I("app-data %s: relaying to %s:%s", sink.channel.c_str(), sink.host.c_str(), sink.port.c_str());
	bool connect_failed = false;
	uint64_t logged_dropped = 0;
	auto next_log = std::chrono::steady_clock::now() + 5s;
	while (not stop.stop_requested())
	{
		TCP tcp;
		try
		{
			tcp = connect(stop);
		}
		catch (const std::exception & e)
		{
			if (not connect_failed)
				U_LOG_W("app-data %s: cannot connect to %s:%s: %s, retrying every 2 s", sink.channel.c_str(), sink.host.c_str(), sink.port.c_str(), e.what());
			connect_failed = true;
		}

		if (tcp)
		{
			connect_failed = false;
			U_LOG_I("app-data %s: connected to %s:%s", sink.channel.c_str(), sink.host.c_str(), sink.port.c_str());
			overflowed = false;
			connected = true;
			std::string reason;
			try
			{
				serve(stop, tcp);
				reason = stop.stop_requested() ? "session paused" : "sink closed the connection";
			}
			catch (const std::exception & e)
			{
				reason = e.what();
			}
			if (overflowed)
				reason = "queue overflow; resetting byte stream";
			tcp = TCP{};
			drop_queue();
			U_LOG_I("app-data %s: disconnected from %s:%s (%s)", sink.channel.c_str(), sink.host.c_str(), sink.port.c_str(), reason.c_str());
			log_counters();
			next_log = std::chrono::steady_clock::now() + 5s;
			logged_dropped = bytes_dropped;
		}

		// Retry in 2 s (less if the stats log comes first); the wake-up socket also ends the wait on pause()
		auto until = std::chrono::steady_clock::now() + 2s;
		while (not stop.stop_requested() and std::chrono::steady_clock::now() < until)
		{
			auto now = std::chrono::steady_clock::now();
			if (now >= next_log)
			{
				next_log += 5s;
				// Nothing flows while disconnected: only report when headset data was dropped
				if (bytes_dropped != logged_dropped)
				{
					log_counters();
					logged_dropped = bytes_dropped;
				}
			}
			pollfd_t fd{};
			fd.fd = wake_receiver.get_fd();
			fd.events = POLLIN;
			int timeout = (int)std::chrono::duration_cast<std::chrono::milliseconds>(std::min(until, next_log) - now).count();
			if (poll_sockets(&fd, 1, std::max(timeout, 1)) > 0)
				drain(wake_receiver.get_fd());
		}
	}
}

TCP app_data_relay::connect(std::stop_token stop)
{
	addrinfo hints{};
	hints.ai_family = AF_UNSPEC;
	hints.ai_socktype = SOCK_STREAM;
	addrinfo * results = nullptr;
	if (int r = getaddrinfo(sink.host.c_str(), sink.port.c_str(), &hints, &results); r != 0)
	{
#ifdef _WIN32
		throw std::runtime_error("cannot resolve: " + std::system_category().message(r));
#else
		throw std::runtime_error(std::string("cannot resolve: ") + gai_strerror(r));
#endif
	}
	std::unique_ptr<addrinfo, decltype(&freeaddrinfo)> results_owner(results, &freeaddrinfo);

	std::string last_error = "no address";
	for (addrinfo * ai = results; ai and not stop.stop_requested(); ai = ai->ai_next)
	{
		socket_t fd = ::socket(ai->ai_family, ai->ai_socktype, ai->ai_protocol);
		if (fd == invalid_socket)
		{
			last_error = socket_error_message();
			continue;
		}
		TCP tcp(fd); // owns fd from now on
		try
		{
			// Non-blocking connect, so that pause() is not delayed by an unreachable sink
			set_nonblocking(fd, true);
			bool done = ::connect(fd, ai->ai_addr, (socklen_t)ai->ai_addrlen) == 0;
			if (not done and not would_block(last_socket_error()))
				throw socket_error();

			auto until = std::chrono::steady_clock::now() + 2s;
			while (not done and not stop.stop_requested() and std::chrono::steady_clock::now() < until)
			{
				pollfd_t pfd{};
				pfd.fd = fd;
				pfd.events = POLLOUT;
				int r = poll_sockets(&pfd, 1, 100);
				if (r < 0)
					throw socket_error();
				if (r == 0)
					continue;
				int error = 0;
				socklen_t len = sizeof(error);
				if (getsockopt(fd, SOL_SOCKET, SO_ERROR, (char *)&error, &len) < 0)
					throw socket_error();
				if (error != 0)
					throw std::system_error(error, socket_error_category());
				done = true;
			}
			if (not done)
				throw std::runtime_error("timeout");
			return tcp;
		}
		catch (const std::exception & e)
		{
			last_error = describe(*ai) + ": " + e.what();
		}
	}
	throw std::runtime_error(last_error);
}

void app_data_relay::serve(std::stop_token stop, TCP & tcp)
{
	std::vector<uint8_t> buffer(app_data_max_size);
	auto next_log = std::chrono::steady_clock::now() + 5s;
	while (not stop.stop_requested())
	{
		pollfd_t fds[2] = {};
		fds[0].fd = tcp.get_fd();
		fds[0].events = POLLIN;
		fds[1].fd = wake_receiver.get_fd();
		fds[1].events = POLLIN;

		auto now = std::chrono::steady_clock::now();
		if (now >= next_log)
		{
			next_log += 5s;
			log_counters();
		}
		int timeout = (int)std::chrono::duration_cast<std::chrono::milliseconds>(next_log - now).count();
		int r = poll_sockets(fds, 2, std::max(timeout, 1));
		if (r < 0)
			throw socket_error();
		if (r == 0)
			continue;

		if (fds[0].revents & POLLIN)
		{
			auto n = ::recv(tcp.get_fd(), (char *)buffer.data(), (int)buffer.size(), 0);
			if (n == 0)
				return;
			if (n < 0)
			{
				if (would_block(last_socket_error()))
					continue;
				throw socket_error();
			}
			try
			{
				session.send_control(to_headset::app_data{
				        .channel = sink.channel,
				        .data = std::vector<uint8_t>(buffer.begin(), buffer.begin() + n),
				});
				bytes_to_headset += n;
			}
			catch (const std::exception &)
			{
				// The session's network thread reports the headset failure and pauses the relay
				bytes_dropped += n;
			}
		}
		else if (fds[0].revents & (POLLERR | POLLHUP))
			return;

		if (fds[1].revents & POLLIN)
		{
			drain(wake_receiver.get_fd());
			if (stop.stop_requested())
				return;
			if (not flush_queue(stop, tcp))
				throw std::runtime_error("cannot write to sink: " + socket_error_message());
		}
	}
}

bool app_data_relay::flush_queue(std::stop_token stop, TCP & tcp)
{
	std::deque<std::vector<uint8_t>> pending;
	{
		std::lock_guard lock(mutex);
		pending.swap(queue);
		queued_bytes = 0;
	}
	size_t remaining = 0;
	for (const auto & data: pending)
		remaining += data.size();
	for (const auto & data: pending)
	{
		size_t offset = 0;
		while (offset < data.size())
		{
			if (stop.stop_requested())
			{
				bytes_dropped += remaining;
				return true;
			}
			if (overflowed)
			{
				bytes_dropped += remaining;
				return false;
			}
			auto n = ::send(tcp.get_fd(), (const char *)data.data() + offset, (int)(data.size() - offset), send_flags);
			if (n < 0)
			{
				if (would_block(last_socket_error()))
				{
					pollfd_t fd{};
					fd.fd = tcp.get_fd();
					fd.events = POLLOUT;
					if (poll_sockets(&fd, 1, 100) >= 0)
						continue;
				}
				bytes_dropped += remaining;
				return false;
			}
			if (n == 0)
			{
				bytes_dropped += remaining;
				return false;
			}
			offset += n;
			remaining -= n;
			bytes_to_sink += n;
		}
	}
	return true;
}

} // namespace wivrn
