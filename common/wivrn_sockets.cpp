/*
 * WiVRn VR streaming
 * Copyright (C) 2022  Guillaume Meunier <guillaume.meunier@centraliens.net>
 * Copyright (C) 2022  Patrick Nicolas <patricknicolas@laposte.net>
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

#include "wivrn_sockets.h"

#include "crypto.h"
#include <algorithm>
#include <array>
#include <cassert>
#include <cstdio>
#include <memory>
#include <string.h>
#include <string>
#include <system_error>

#ifdef _WIN32
#include <mstcpip.h> // SIO_UDP_CONNRESET
#ifndef SIO_UDP_CONNRESET
#define SIO_UDP_CONNRESET _WSAIOW(IOC_VENDOR, 12)
#endif
#else
#include <arpa/inet.h>
#include <netdb.h>
#include <netinet/ip.h>
#include <netinet/tcp.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <sys/uio.h>
#include <unistd.h>
#endif

namespace
{
std::system_error socket_error()
{
	return std::system_error{wivrn::last_socket_error(), wivrn::socket_error_category()};
}

int close_socket(wivrn::socket_t fd)
{
#ifdef _WIN32
	return ::closesocket(fd);
#else
	return ::close(fd);
#endif
}

// Close a socket owned by a fd_base after a failed call and throw the error of that call.
// fd is invalidated so that ~fd_base does not close it a second time.
[[noreturn]] void close_and_throw(wivrn::socket_t & fd)
{
	auto error = socket_error();
	close_socket(fd);
	fd = wivrn::invalid_socket;
	throw error;
}

void set_cloexec(wivrn::socket_t fd)
{
#ifdef _WIN32
	// Nothing to do: there is no fork/exec, sockets are only inherited by CreateProcess(bInheritHandles = TRUE)
	(void)fd;
#else
	fcntl(fd, F_SETFD, FD_CLOEXEC);
#endif
}

#ifdef _WIN32
using iobuf = WSABUF;

iobuf make_iobuf(const void * data, size_t size)
{
	iobuf buf;
	buf.len = static_cast<ULONG>(size);
	buf.buf = static_cast<CHAR *>(const_cast<void *>(data));
	return buf;
}

// IPv6 sockets are IPv6-only by default on Windows (not on Linux): enable dual stack so that
// IPv4 peers (v4-mapped addresses) can use the same socket
void set_dual_stack(SOCKET fd)
{
	DWORD v6only = 0;
	if (setsockopt(fd, IPPROTO_IPV6, IPV6_V6ONLY, (const char *)&v6only, sizeof(v6only)) < 0)
		throw socket_error();
}

// By default an ICMP "port unreachable" caused by a previous send makes the next recv fail with WSAECONNRESET
void disable_udp_connreset(SOCKET fd)
{
	BOOL report = FALSE;
	DWORD bytes = 0;
	WSAIoctl(fd, SIO_UDP_CONNRESET, &report, sizeof(report), nullptr, 0, &bytes, nullptr, nullptr);
}

// Replacement for MSG_DONTWAIT (Winsock has no per-call nonblocking flag and toggling FIONBIO
// would race with blocking sends from other threads): true if a recv on fd will not block.
bool readable_now(SOCKET fd)
{
	WSAPOLLFD pfd{};
	pfd.fd = fd;
	pfd.events = POLLRDNORM;
	int r = WSAPoll(&pfd, 1, 0);
	if (r == SOCKET_ERROR)
		throw socket_error();
	// POLLRDNORM, or POLLHUP/POLLERR: recv returns immediately with data, 0 or an error
	return r > 0;
}

// sendmsg replacement: send all buffers, handling partial sends
size_t send_all(SOCKET fd, std::vector<WSABUF> & bufs)
{
	WSABUF * iov = bufs.data();
	size_t iovlen = bufs.size();
	size_t total_sent = 0;
	while (true)
	{
		DWORD sent = 0;
		if (WSASend(fd, iov, static_cast<DWORD>(iovlen), &sent, 0, nullptr, nullptr) != 0)
			throw socket_error();

		if (sent == 0)
			throw wivrn::socket_shutdown{};

		total_sent += sent;

		// buffers fully consumed
		while (iovlen > 0 and sent >= iov[0].len)
		{
			sent -= iov[0].len;
			++iov;
			--iovlen;
		}
		if (iovlen == 0)
			return total_sent;
		iov[0].buf += sent;
		iov[0].len -= sent;
	}
}
#else
using iobuf = iovec;

iobuf make_iobuf(const void * data, size_t size)
{
	return iovec{.iov_base = const_cast<void *>(data), .iov_len = size};
}
#endif
} // namespace

void wivrn::init_sockets()
{
#ifdef _WIN32
	WSADATA wsa_data;
	if (int err = WSAStartup(MAKEWORD(2, 2), &wsa_data); err != 0)
		throw std::system_error{err, std::system_category(), "WSAStartup"};
#endif
}

int wivrn::poll_sockets(pollfd_t * fds, size_t n, int timeout_ms)
{
#ifdef _WIN32
	// poll() ignores negative fds, WSAPoll may fail on INVALID_SOCKET entries: only pass valid sockets
	if (std::ranges::none_of(std::span(fds, n), [](const pollfd_t & pfd) { return pfd.fd == INVALID_SOCKET; }))
		return WSAPoll(fds, static_cast<ULONG>(n), timeout_ms);

	std::array<WSAPOLLFD, 8> valid;
	std::array<size_t, 8> index;
	size_t count = 0;
	for (size_t i = 0; i < n; ++i)
	{
		fds[i].revents = 0;
		if (fds[i].fd == INVALID_SOCKET)
			continue;
		if (count == valid.size())
		{
			WSASetLastError(WSAEINVAL);
			return SOCKET_ERROR;
		}
		index[count] = i;
		valid[count++] = fds[i];
	}

	if (count == 0)
	{
		// Same as poll() with only ignored entries: wait for the timeout (-1 = INFINITE)
		Sleep(static_cast<DWORD>(timeout_ms));
		return 0;
	}

	int r = WSAPoll(valid.data(), static_cast<ULONG>(count), timeout_ms);
	if (r > 0)
	{
		for (size_t i = 0; i < count; ++i)
			fds[index[i]].revents = valid[i].revents;
	}
	return r;
#else
	return ::poll(fds, n, timeout_ms);
#endif
}

std::pair<wivrn::socket_t, wivrn::socket_t> wivrn::make_datagram_pair()
{
#ifdef _WIN32
	// No AF_UNIX datagram sockets on Windows: two loopback UDP sockets connected to each other.
	// Connected UDP sockets only accept datagrams from their peer.
	std::array<SOCKET, 2> fds{INVALID_SOCKET, INVALID_SOCKET};
	std::array<sockaddr_in, 2> addrs{};
	auto fail = [&]() {
		auto error = socket_error();
		for (SOCKET s: fds)
			if (s != INVALID_SOCKET)
				closesocket(s);
		throw error;
	};

	for (size_t i = 0; i < 2; ++i)
	{
		fds[i] = ::socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
		if (fds[i] == INVALID_SOCKET)
			fail();

		addrs[i].sin_family = AF_INET;
		addrs[i].sin_addr.s_addr = htonl(INADDR_LOOPBACK);
		addrs[i].sin_port = 0;
		if (::bind(fds[i], (sockaddr *)&addrs[i], sizeof(addrs[i])) < 0)
			fail();

		socklen_t len = sizeof(addrs[i]);
		if (getsockname(fds[i], (sockaddr *)&addrs[i], &len) < 0)
			fail();

		disable_udp_connreset(fds[i]);
	}

	if (::connect(fds[0], (sockaddr *)&addrs[1], sizeof(addrs[1])) < 0)
		fail();
	if (::connect(fds[1], (sockaddr *)&addrs[0], sizeof(addrs[0])) < 0)
		fail();

	return {fds[0], fds[1]};
#else
	int fds[2];
	if (socketpair(AF_UNIX, SOCK_DGRAM, 0, fds) < 0)
		throw socket_error();
	set_cloexec(fds[0]);
	set_cloexec(fds[1]);
	return {fds[0], fds[1]};
#endif
}

thread_local crypto::encrypt_context wivrn::UDP::encrypter{EVP_aes_128_ctr()};
std::atomic<uint64_t> wivrn::UDP::iv_counter;

const char * wivrn::invalid_packet::what() const noexcept
{
	return "Invalid packet";
}

const char * wivrn::socket_shutdown::what() const noexcept
{
	return "Socket shutdown";
}

wivrn::fd_base::fd_base(wivrn::fd_base && other) :
        fd(other.fd)
{
	other.fd = -1;
}

wivrn::fd_base & wivrn::fd_base::operator=(wivrn::fd_base && other)
{
	std::swap(fd, other.fd);
	return *this;
}

wivrn::fd_base::~fd_base()
{
	if (fd != invalid_socket)
		close_socket(fd);
}

wivrn::UDP::UDP()
{
	fd = socket(AF_INET6, SOCK_DGRAM, 0);
	if (fd == invalid_socket)
		throw socket_error();
	set_cloexec(fd);
#ifdef _WIN32
	set_dual_stack(fd);
	disable_udp_connreset(fd);
#endif
}

wivrn::UDP::UDP(socket_t fd)
{
	this->fd = fd;
}

void wivrn::UDP::bind(sockaddr_in6 address)
{
	if (::bind(fd, (sockaddr *)&address, sizeof(address)) < 0)
		throw socket_error();
}

void wivrn::UDP::connect(sockaddr_in6 sa)
{
	if (::connect(fd, (sockaddr *)&sa, sizeof(sa)) < 0)
		throw socket_error();
}

void wivrn::UDP::connect(sockaddr_in sa)
{
	if (::connect(fd, (sockaddr *)&sa, sizeof(sa)) < 0)
		throw socket_error();
}

void wivrn::UDP::subscribe_multicast(in6_addr address)
{
	assert(IN6_IS_ADDR_MULTICAST(&address));

	ipv6_mreq subscribe{};
	subscribe.ipv6mr_multiaddr = address;

	if (setsockopt(fd, IPPROTO_IPV6, IPV6_ADD_MEMBERSHIP, (const char *)&subscribe, sizeof(subscribe)) < 0)
		close_and_throw(fd);
}

void wivrn::UDP::unsubscribe_multicast(in6_addr address)
{
	assert(IN6_IS_ADDR_MULTICAST(&address));

	ipv6_mreq subscribe{};
	subscribe.ipv6mr_multiaddr = address;

	if (setsockopt(fd, IPPROTO_IPV6, IPV6_DROP_MEMBERSHIP, (const char *)&subscribe, sizeof(subscribe)) < 0)
		close_and_throw(fd);
}

void wivrn::UDP::set_receive_buffer_size(int size)
{
	setsockopt(fd, SOL_SOCKET, SO_RCVBUF, (const char *)&size, sizeof(size));
}

void wivrn::UDP::set_send_buffer_size(int size)
{
	setsockopt(fd, SOL_SOCKET, SO_SNDBUF, (const char *)&size, sizeof(size));
}

void wivrn::UDP::set_tos(int tos)
{
	int err = setsockopt(fd, IPPROTO_IP, IP_TOS, (const char *)&tos, sizeof(tos));
	if (err == -1)
	{
#ifdef _WIN32
		// Windows only honours IP_TOS through the QoS API: ignore
		std::fprintf(stderr, "wivrn: cannot set IP_TOS %d on socket (error %d), ignored\n", tos, last_socket_error());
		return;
#else
		throw socket_error();
#endif
	}
}

void wivrn::TCP::init()
{
	int nodelay = 1;
	if (setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, (const char *)&nodelay, sizeof(nodelay)) < 0)
		close_and_throw(fd);

	mutex = std::make_unique<std::mutex>();
}

wivrn::TCP::TCP(socket_t fd)
{
	this->fd = fd;

	init();
}

wivrn::TCP::TCP(sockaddr_in6 sa)
{
	fd = socket(AF_INET6, SOCK_STREAM, 0);
	if (fd == invalid_socket)
		throw socket_error();
	set_cloexec(fd);

	if (connect(fd, (sockaddr *)&sa, sizeof(sa)) < 0)
		close_and_throw(fd);

	init();
}

wivrn::TCP::TCP(sockaddr_in sa)
{
	fd = socket(AF_INET, SOCK_STREAM, 0);
	if (fd == invalid_socket)
		throw socket_error();
	set_cloexec(fd);

	if (connect(fd, (sockaddr *)&sa, sizeof(sa)) < 0)
		close_and_throw(fd);

	init();
}

wivrn::TCPListener::TCPListener(int port)
{
	fd = socket(AF_INET6, SOCK_STREAM, 0);

	if (fd == invalid_socket)
	{
		throw socket_error();
	}

#ifdef _WIN32
	// Windows' SO_REUSEADDR allows binding a port that another socket is actively listening on, unlike Linux.
	// Rebinding while old connections are in TIME_WAIT (the reason for SO_REUSEADDR on Linux) works without it.
	// On exception, ~fd_base closes the socket
	set_dual_stack(fd);
#else
	int reuse_addr = 1;
	if (setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, (const char *)&reuse_addr, sizeof(reuse_addr)) < 0)
		close_and_throw(fd);
#endif

	sockaddr_in6 addr{};
	addr.sin6_family = AF_INET6;
	addr.sin6_port = htons(port);
	addr.sin6_addr = in6addr_any;

	if (bind(fd, (sockaddr *)&addr, sizeof(addr)) < 0)
		close_and_throw(fd);

	int backlog = 1;
	if (listen(fd, backlog) < 0)
		close_and_throw(fd);
}

std::pair<wivrn::deserialization_packet, sockaddr_in6> wivrn::UDP::receive_from_raw()
{
	sockaddr_in6 addr;
	socklen_t addrlen = sizeof(addr);

#ifdef _WIN32
	// No MSG_TRUNC size peeking on Winsock: receive into a buffer large enough for any UDP datagram
	static constexpr int max_datagram_size = 65536;
	auto buffer = std::make_shared_for_overwrite<uint8_t[]>(max_datagram_size);
	ssize_t received = recvfrom(fd, (char *)buffer.get(), max_datagram_size, 0, (sockaddr *)&addr, &addrlen);
	if (received < 0)
	{
		// ICMP port unreachable from a previous send: no data
		if (WSAGetLastError() == WSAECONNRESET)
			return {};
		throw socket_error();
	}
#else
	ssize_t peeked = recvfrom(fd, nullptr, 0, MSG_PEEK | MSG_TRUNC, (sockaddr *)&addr, &addrlen);
	if (peeked < 0)
		throw socket_error();

	auto buffer = std::make_shared_for_overwrite<uint8_t[]>(peeked);
	ssize_t received = recvfrom(fd, buffer.get(), peeked, 0, (sockaddr *)&addr, &addrlen);
	if (received < 0)
		throw socket_error();
#endif

	std::span message{buffer.get(), (size_t)received};

	if (encrypted)
	{
		// Not big enough for the IV: drop the packet
		if (received < sizeof(uint64_t))
			return {};

		std::array<uint8_t, 16> full_iv;
		memcpy(full_iv.data(), buffer.get(), sizeof(uint64_t)); // TODO: endianness?
		memcpy(full_iv.data() + sizeof(uint64_t), recv_iv_header.data(), recv_iv_header.size());

		message = message.subspan(sizeof(uint64_t));

		decrypter.set_iv(full_iv);
		decrypter.decrypt_in_place(message);
	}

	return {deserialization_packet{std::move(buffer), message}, addr};
}

wivrn::deserialization_packet wivrn::UDP::receive_pending()
{
	if (messages.empty())
		return {};

	auto span = messages.back();
	messages.pop_back();
	return deserialization_packet{buffer, span};
}

wivrn::deserialization_packet wivrn::UDP::receive_raw()
{
	if (not messages.empty())
	{
		auto span = messages.back();
		messages.pop_back();
		return deserialization_packet{buffer, span};
	}

	static const size_t message_size = 2048;
	static const size_t num_messages = 20;
	if ((not buffer) or buffer.use_count() > 1)
	{
		buffer = std::make_shared_for_overwrite<uint8_t[]>(message_size * num_messages);
	}
#ifdef _WIN32
	// No recvmmsg on Winsock: receive the datagrams that are already queued (up to num_messages),
	// one recv per datagram so that datagram boundaries are preserved
	std::array<size_t, num_messages> lengths;
	int received = 0;
	for (size_t attempt = 0; attempt < num_messages; ++attempt)
	{
		if (not readable_now(fd))
		{
			if (attempt == 0)
				throw std::system_error{WSAEWOULDBLOCK, std::system_category()}; // EAGAIN with MSG_DONTWAIT
			break;
		}

		int size = ::recv(fd, (char *)buffer.get() + message_size * received, (int)message_size, 0);
		if (size == SOCKET_ERROR)
		{
			int err = WSAGetLastError();
			if (err == WSAEMSGSIZE)
				continue; // datagram larger than message_size, truncated: drop it
			if (err == WSAECONNRESET)
				break; // ICMP port unreachable from a previous send: no data
			if (received > 0)
				break; // like recvmmsg: the error is reported by the next call
			if (err == WSAESHUTDOWN)
				throw socket_shutdown();
			throw std::system_error{err, std::system_category()};
		}
		lengths[received++] = size;
	}

	if (received == 0)
		return {};
#else
	std::array<iovec, num_messages> iovecs;
	std::array<mmsghdr, num_messages> mmsgs;
	for (size_t i = 0; i < num_messages; ++i)
	{
		iovecs[i] = {
		        .iov_base = buffer.get() + message_size * i,
		        .iov_len = message_size,
		};

		mmsgs[i] = {
		        .msg_hdr = {
		                .msg_iov = &iovecs[i],
		                .msg_iovlen = 1,
		        },
		};
	}

	int received = recvmmsg(fd, mmsgs.data(), num_messages, MSG_DONTWAIT, nullptr);

	if (received < 0)
		throw socket_error();
	if (received == 0)
		throw socket_shutdown();
#endif

	messages.reserve(received);

	for (int i = received - 1; i >= 0; --i)
	{
#ifdef _WIN32
		std::span<uint8_t> message{buffer.get() + message_size * i, lengths[i]};
#else
		std::span<uint8_t> message{(uint8_t *)iovecs[i].iov_base, mmsgs[i].msg_len};
#endif
		assert(message.data() != nullptr);

		if (encrypted)
		{
			// Not big enough for the IV: drop the packet
			if (message.size() < sizeof(uint64_t))
				throw std::runtime_error("Packet too small: " + std::to_string(message.size()));

			std::array<uint8_t, 16> full_iv;
			memcpy(full_iv.data(), message.data(), sizeof(uint64_t)); // TODO: endianness?
			memcpy(full_iv.data() + sizeof(uint64_t), recv_iv_header.data(), recv_iv_header.size());

			message = message.subspan(sizeof(uint64_t));

			decrypter.set_iv(full_iv);
			decrypter.decrypt_in_place(message);
		}

		if (i == 0)
			return deserialization_packet{buffer, message};

		messages.push_back(message);
	}

#ifdef _MSC_VER
	__assume(false);
#else
	__builtin_unreachable();
#endif
}

size_t wivrn::UDP::send_raw(serialization_packet && packet)
{
	thread_local std::vector<iobuf> iovecs;
	iovecs.clear();

	std::vector<std::span<uint8_t>> & data = packet;

	uint64_t counter;
	if (encrypted)
	{
		counter = iv_counter.fetch_add(1);

		std::array<uint8_t, 16> full_iv;
		memcpy(full_iv.data(), &counter, sizeof(uint64_t)); // TODO: endianness?
		memcpy(full_iv.data() + sizeof(uint64_t), send_iv_header.data(), send_iv_header.size());

		iovecs.push_back(make_iobuf(&counter, sizeof(uint64_t)));

		encrypter.set_key_and_iv(key, full_iv);
		encrypter.encrypt_in_place(data);
	}

	for (const auto & span: data)
		iovecs.push_back(make_iobuf(span.data(), span.size()));

#ifdef _WIN32
	if (DWORD sent = 0; WSASend(fd, iovecs.data(), static_cast<DWORD>(iovecs.size()), &sent, 0, nullptr, nullptr) == 0)
		return sent;
#else
	if (ssize_t sent = ::writev(fd, iovecs.data(), iovecs.size()); sent >= 0)
		return sent;
#endif
	throw socket_error();
}

size_t wivrn::UDP::send_many_raw(std::span<serialization_packet> packets)
{
	thread_local std::vector<iobuf> iovecs;
#ifdef _WIN32
	thread_local std::vector<size_t> iovec_counts;
#else
	thread_local std::vector<mmsghdr> mmsgs;
#endif
	thread_local std::vector<uint64_t> iv_counters;

	if (packets.empty())
		return 0;

	iovecs.clear();
#ifdef _WIN32
	iovec_counts.clear();
#else
	mmsgs.clear();
#endif
	iv_counters.clear();

	iv_counters.reserve(packets.size());

	size_t sent = 0;
	for (serialization_packet & packet: packets)
	{
		std::vector<std::span<uint8_t>> & data = packet;

		if (encrypted)
		{
			iv_counters.push_back(iv_counter.fetch_add(1));

			std::array<uint8_t, 16> full_iv;
			memcpy(full_iv.data(), &iv_counters.back(), sizeof(uint64_t)); // TODO: endianness?
			memcpy(full_iv.data() + sizeof(uint64_t), send_iv_header.data(), send_iv_header.size());

			iovecs.push_back(make_iobuf(&iv_counters.back(), sizeof(uint64_t)));

			encrypter.set_key_and_iv(key, full_iv);
			encrypter.encrypt_in_place(data);
		}

		for (const auto & span: data)
		{
			iovecs.push_back(make_iobuf(span.data(), span.size_bytes()));
			sent += span.size();
		}

#ifdef _WIN32
		iovec_counts.push_back(encrypted ? data.size() + 1 : data.size());
#else
		if (encrypted)
			mmsgs.push_back({.msg_hdr = {.msg_iovlen = data.size() + 1}});
		else
			mmsgs.push_back({.msg_hdr = {.msg_iovlen = data.size()}});
#endif
	}

#ifdef _WIN32
	// No sendmmsg on Winsock: one WSASend per datagram. As with sendmmsg, only an error on the
	// first datagram is reported, later failed datagrams are considered lost
	for (size_t i = 0, j = 0; i < packets.size(); j += iovec_counts[i++])
	{
		DWORD bytes = 0;
		if (WSASend(fd, iovecs.data() + j, static_cast<DWORD>(iovec_counts[i]), &bytes, 0, nullptr, nullptr) != 0 and i == 0)
			throw socket_error();
	}
#else
	for (size_t i = 0, j = 0; i < packets.size(); ++i)
	{
		mmsgs[i].msg_hdr.msg_iov = &iovecs[j];
		j += mmsgs[i].msg_hdr.msg_iovlen;
	}

	// sendmmsg may not send all messages, just consider them as lost for UDP
	if (sendmmsg(fd, mmsgs.data(), mmsgs.size(), 0) < 0)
		throw socket_error();
#endif
	return sent;
}

wivrn::deserialization_packet wivrn::TCP::receive_raw()
{
	static constexpr size_t max_payload = 16 * 1024 * 1024;

	ssize_t expected_size;

	if (data.size_bytes() < sizeof(uint32_t))
	{
		expected_size = sizeof(uint32_t) - data.size_bytes();
	}
	else
	{
		uint32_t payload_size = *reinterpret_cast<uint32_t *>(data.data());
		if (payload_size > max_payload)
			throw std::runtime_error("Invalid packet: size " + std::to_string(payload_size));
		expected_size = payload_size + sizeof(uint32_t) - data.size_bytes();
	}

	if (expected_size > capacity_left)
	{
		size_t new_size = std::max<size_t>(data.size_bytes() + expected_size,
		                                   4096);
		auto old = std::move(buffer);
		buffer = std::make_shared_for_overwrite<uint8_t[]>(new_size);
		memcpy(buffer.get(), data.data(), data.size_bytes());
		data = std::span(buffer.get(), data.size());
		capacity_left = new_size - data.size_bytes();
	}

	if (capacity_left > 0)
	{
		// data.data() + data.size() instead of &*data.end(): MSVC debug iterators reject dereferencing end()
		uint8_t * receive_ptr = data.data() + data.size();
#ifdef _WIN32
		// No MSG_DONTWAIT: fail with WSAEWOULDBLOCK (like EAGAIN) if recv would block
		if (not readable_now(fd))
			throw std::system_error{WSAEWOULDBLOCK, std::system_category()};
		ssize_t received_size = recv(fd, (char *)receive_ptr, static_cast<int>(capacity_left), 0);
#else
		ssize_t received_size = recv(fd, receive_ptr, capacity_left, MSG_DONTWAIT);
#endif

		if (received_size < 0)
			throw socket_error();

		if (received_size == 0)
			throw socket_shutdown{};

		if (decrypter)
		{
			std::span<uint8_t> received_data{receive_ptr, (size_t)received_size};
			decrypter.decrypt_in_place(received_data);
		}

		data = std::span(data.data(), data.size() + received_size);
		capacity_left -= received_size;
	}

	if (data.size_bytes() < sizeof(uint32_t))
		return {};

	uint32_t payload_size = *reinterpret_cast<uint32_t *>(data.data());
	if (payload_size == 0)
		throw std::runtime_error("Invalid packet: 0 size");

	if (data.size_bytes() < sizeof(uint32_t) + payload_size)
		return {};

	auto span = data.subspan(sizeof(uint32_t), payload_size);
	data = data.subspan(sizeof(uint32_t) + payload_size);
	return deserialization_packet{buffer, span};
}

wivrn::deserialization_packet wivrn::TCP::receive_pending()
{
	if (data.size_bytes() < sizeof(uint32_t))
		return {};

	uint32_t payload_size = *reinterpret_cast<uint32_t *>(data.data());
	if (payload_size == 0)
		throw std::runtime_error("Invalid packet: 0 size");

	if (data.size_bytes() < sizeof(uint32_t) + payload_size)
		return {};

	auto span = data.subspan(sizeof(uint32_t), payload_size);
	data = data.subspan(sizeof(uint32_t) + payload_size);
	return deserialization_packet{buffer, span};
}

size_t wivrn::TCP::send_raw(serialization_packet && packet)
{
	thread_local std::vector<iobuf> iovecs;
	iovecs.clear();

	std::vector<std::span<uint8_t>> & data = packet;

	uint32_t size = 0;
	iovecs.push_back(make_iobuf(&size, sizeof(size)));
	for (const auto & span: data)
	{
		size += span.size_bytes();
		iovecs.push_back(make_iobuf(span.data(), span.size_bytes()));
	}

#ifndef _WIN32
	msghdr hdr{
	        .msg_name = nullptr,
	        .msg_namelen = 0,
	        .msg_iov = iovecs.data(),
	        .msg_iovlen = iovecs.size(),
	        .msg_control = nullptr,
	        .msg_controllen = 0,
	        .msg_flags = 0,
	};
#endif

	std::lock_guard lock(*mutex);
	if (encrypter)
	{
		data.insert(data.begin(), {(uint8_t *)&size, sizeof(size)});
		encrypter.encrypt_in_place(data);
	}

#ifdef _WIN32
	return send_all(fd, iovecs);
#else
	size_t total_sent = 0;
	while (true)
	{
		ssize_t sent = ::sendmsg(fd, &hdr, MSG_NOSIGNAL);

		if (sent == 0)
			throw socket_shutdown{};

		if (sent < 0)
			throw socket_error();

		total_sent += sent;

		// iov fully consumed
		while (hdr.msg_iovlen > 0 and sent >= hdr.msg_iov[0].iov_len)
		{
			sent -= hdr.msg_iov[0].iov_len;
			++hdr.msg_iov;
			--hdr.msg_iovlen;
		}
		if (hdr.msg_iovlen == 0)
			return total_sent;
		hdr.msg_iov[0].iov_base = (void *)((uintptr_t)hdr.msg_iov[0].iov_base + sent);
		hdr.msg_iov[0].iov_len -= sent;
	}
#endif
}

size_t wivrn::TCP::send_many_raw(std::span<serialization_packet> packets)
{
	thread_local std::vector<iobuf> iovecs;
	thread_local std::vector<uint32_t> sizes;
	thread_local std::vector<std::span<uint8_t>> spans;

	if (packets.empty())
		return 0;

	iovecs.clear();
	sizes.clear();
	spans.clear();

	sizes.reserve(packets.size());

	for (serialization_packet & packet: packets)
	{
		std::vector<std::span<uint8_t>> & data = packet;

		auto & size = sizes.emplace_back(0);
		iovecs.push_back(make_iobuf(&size, sizeof(size)));
		spans.emplace_back((uint8_t *)&size, sizeof(size));

		for (const auto & span: data)
		{
			size += span.size_bytes();
			iovecs.push_back(make_iobuf(span.data(), span.size_bytes()));
			spans.emplace_back(span.data(), span.size_bytes());
		}
	}

#ifndef _WIN32
	msghdr hdr{
	        .msg_name = nullptr,
	        .msg_namelen = 0,
	        .msg_iov = iovecs.data(),
	        .msg_iovlen = iovecs.size(),
	        .msg_control = nullptr,
	        .msg_controllen = 0,
	        .msg_flags = 0,
	};
#endif

	std::lock_guard lock(*mutex);
	if (encrypter)
	{
		encrypter.encrypt_in_place(spans);
	}

#ifdef _WIN32
	return send_all(fd, iovecs);
#else
	size_t total_sent = 0;
	while (true)
	{
		ssize_t sent = ::sendmsg(fd, &hdr, MSG_NOSIGNAL);

		if (sent == 0)
			throw socket_shutdown{};

		if (sent < 0)
			throw socket_error();

		total_sent += sent;

		// iov fully consumed
		while (hdr.msg_iovlen > 0 and sent >= hdr.msg_iov[0].iov_len)
		{
			sent -= hdr.msg_iov[0].iov_len;
			++hdr.msg_iov;
			--hdr.msg_iovlen;
		}
		if (hdr.msg_iovlen == 0)
			return total_sent;
		hdr.msg_iov[0].iov_base = (void *)((uintptr_t)hdr.msg_iov[0].iov_base + sent);
		hdr.msg_iov[0].iov_len -= sent;
	}
#endif
}

void wivrn::UDP::set_aes_key_and_ivs(std::span<std::uint8_t, 16> key_, std::span<std::uint8_t, 8> recv_iv_header_, std::span<std::uint8_t, 8> send_iv_header_)
{
	decrypter = crypto::decrypt_context{EVP_aes_128_ctr()};
	decrypter.set_key(key_);

	std::ranges::copy(key_, key.begin());
	std::ranges::copy(recv_iv_header_, recv_iv_header.begin());
	std::ranges::copy(send_iv_header_, send_iv_header.begin());
	encrypted = true;
}

void wivrn::TCP::set_aes_key_and_ivs(std::span<std::uint8_t, 16> key, std::span<std::uint8_t, 16> recv_iv, std::span<std::uint8_t, 16> send_iv)
{
	encrypter = crypto::encrypt_context{EVP_aes_128_ctr()};
	encrypter.set_key(key);
	encrypter.set_iv(send_iv);

	decrypter = crypto::decrypt_context{EVP_aes_128_ctr()};
	decrypter.set_key(key);
	decrypter.set_iv(recv_iv);
}
