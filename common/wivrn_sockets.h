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

#pragma once

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#endif

#include "crypto.h"
#include "wivrn_serialization.h"

#include <atomic>
#include <cassert>
#include <cerrno>
#include <cstddef>
#include <exception>
#include <memory>
#include <mutex>
#include <span>
#include <system_error>
#include <utility>
#include <vector>

#ifndef _WIN32
#include <fcntl.h>
#include <netinet/ip.h>
#include <poll.h>
#endif

namespace wivrn
{
#ifdef _WIN32
using socket_t = SOCKET;
using pollfd_t = WSAPOLLFD;
inline constexpr socket_t invalid_socket = INVALID_SOCKET;

inline int last_socket_error()
{
	return WSAGetLastError();
}

// Winsock error codes are Win32 error codes
inline const std::error_category & socket_error_category()
{
	return std::system_category();
}
#else
using socket_t = int;
using pollfd_t = ::pollfd;
inline constexpr socket_t invalid_socket = -1;

inline int last_socket_error()
{
	return errno;
}

inline const std::error_category & socket_error_category()
{
	return std::generic_category();
}
#endif

// WSAStartup on Windows, no-op elsewhere. Call once before creating any socket.
void init_sockets();

// ::poll / WSAPoll. Entries whose fd is invalid_socket are ignored (revents = 0) on all platforms.
int poll_sockets(pollfd_t * fds, size_t n, int timeout_ms);

// Connected, unencrypted datagram socket pair for in-process/inter-process control messages.
// Linux: socketpair(AF_UNIX, SOCK_DGRAM); Windows: two UDP sockets on 127.0.0.1 connected to each other.
std::pair<socket_t, socket_t> make_datagram_pair();

class socket_shutdown : public std::exception
{
public:
	const char * what() const noexcept override;
};

class invalid_packet : public std::exception
{
public:
	const char * what() const noexcept override;
};

template <typename T, typename... Ts>
struct index_of_type
{};

template <typename T, typename... Ts>
struct index_of_type<T, T, Ts...> : std::integral_constant<size_t, 0>
{};

template <typename T, typename T2, typename... Ts>
struct index_of_type<T, T2, Ts...> : std::integral_constant<size_t, 1 + index_of_type<T, Ts...>::value>
{};

static_assert(index_of_type<int, int, float>::value == 0);
static_assert(index_of_type<float, int, float>::value == 1);

class fd_base
{
protected:
	socket_t fd = invalid_socket;

	fd_base(const fd_base &) = delete;

public:
	fd_base() = default;
	fd_base(socket_t fd) :
	        fd{fd} {}
	fd_base(fd_base &&);
	fd_base & operator=(fd_base &&);
	~fd_base();

	socket_t get_fd() const
	{
		return fd;
	}

	operator bool() const
	{
		return fd != invalid_socket;
	}

	operator socket_t() const
	{
		return fd;
	}
};

class UDP : public fd_base
{
	std::shared_ptr<uint8_t[]> buffer;
	std::vector<std::span<uint8_t>> messages;

	crypto::decrypt_context decrypter;
	static thread_local crypto::encrypt_context encrypter;
	static std::atomic<uint64_t> iv_counter;
	static_assert(sizeof(iv_counter) == 8);

	bool encrypted = false;
	std::array<uint8_t, 16> key;
	std::array<uint8_t, 16 - sizeof(iv_counter)> recv_iv_header;
	std::array<uint8_t, 16 - sizeof(iv_counter)> send_iv_header;

public:
	UDP();
	explicit UDP(socket_t fd);

	deserialization_packet receive_raw();
	deserialization_packet receive_pending();
	std::pair<wivrn::deserialization_packet, sockaddr_in6> receive_from_raw();
	size_t send_raw(serialization_packet && packet);
	size_t send_many_raw(std::span<serialization_packet> packets);

	void connect(sockaddr_in6 address);
	void connect(sockaddr_in address);
	void bind(sockaddr_in6 address);
	void subscribe_multicast(in6_addr address);
	void unsubscribe_multicast(in6_addr address);
	void set_receive_buffer_size(int size);
	void set_send_buffer_size(int size);
	void set_tos(int type_of_service);

	void set_aes_key_and_ivs(std::span<std::uint8_t, 16> key, std::span<std::uint8_t, 8> recv_iv_header, std::span<std::uint8_t, 8> send_iv_header);
};

class TCP : public fd_base
{
	std::shared_ptr<uint8_t[]> buffer;
	ssize_t capacity_left = 0;
	std::span<uint8_t> data;
	std::unique_ptr<std::mutex> mutex;

	void init();

	crypto::decrypt_context decrypter;
	crypto::encrypt_context encrypter;

public:
	TCP() = default;
	TCP(sockaddr_in6 address);
	TCP(sockaddr_in address);
	explicit TCP(socket_t fd);

	deserialization_packet receive_raw();
	deserialization_packet receive_pending();
	size_t send_raw(serialization_packet && packet);
	size_t send_many_raw(std::span<serialization_packet> packets);

	void set_aes_key_and_ivs(std::span<std::uint8_t, 16> key, std::span<std::uint8_t, 16> recv_iv, std::span<std::uint8_t, 16> send_iv);
};

using UnixDatagram = UDP;

class TCPListener : public fd_base
{
public:
	TCPListener() = default;
	TCPListener(int port);

	template <typename T = TCP>
	std::pair<T, sockaddr_in6> accept()
	{
		assert(fd != invalid_socket);
		sockaddr_in6 addr{};
		socklen_t addrlen = sizeof(addr);

		socket_t fd2 = ::accept(fd, (sockaddr *)&addr, &addrlen);
#ifndef _WIN32
		fcntl(fd, F_SETFD, FD_CLOEXEC);
#endif
		if (fd2 == invalid_socket)
			throw std::system_error{last_socket_error(), socket_error_category()};

		return {T{fd2}, addr};
	}
};
template <typename Socket, typename ReceivedType, typename SentType>
class typed_socket;

namespace details
{
template <class T, class Tuple>
struct Index;

template <class T, class... Types>
struct Index<T, std::tuple<T, Types...>>
{
	static const std::size_t value = 0;
};

template <class T, class U, class... Types>
struct Index<T, std::tuple<U, Types...>>
{
	static const std::size_t value = 1 + Index<T, std::tuple<Types...>>::value;
};

template <typename T>
concept not_lvalue_reference = !std::is_lvalue_reference_v<T>;

} // namespace details

template <typename Socket, typename ReceivedType, typename... VariantTypes>
class typed_socket<Socket, ReceivedType, std::variant<VariantTypes...>> : public Socket
{
public:
	template <typename... Args>
	typed_socket(Args &&... args) :
	        Socket(std::forward<Args>(args)...)
	{}

	std::optional<ReceivedType> receive_pending(std::atomic<uint64_t> * size = nullptr)
	{
		deserialization_packet packet = ((Socket *)this)->receive_pending();
		if (packet.empty())
			return {};
		if (size)
			size->fetch_add(packet.wire_size());

		return packet.deserialize<ReceivedType>();
	}

	std::optional<ReceivedType> receive(std::atomic<uint64_t> * size = nullptr)
	{
		deserialization_packet packet = this->receive_raw();
		if (packet.empty())
			return {};
		if (size)
			size->fetch_add(packet.wire_size());

		return packet.deserialize<ReceivedType>();
	}

	// WARNING: serialization packet keeps references to data
	template <typename T>
	static void serialize(serialization_packet & p, const T & data)
	{
		p.clear();
		uint8_t index = details::Index<std::decay_t<T>, std::tuple<VariantTypes...>>::value;
		p.serialize(index);
		p.serialize(data);
	}

	template <details::not_lvalue_reference T>
	size_t send(T && data)
	{
		thread_local serialization_packet p;
		serialize(p, data);
		return this->send_raw(std::move(p));
	}

	size_t send(std::span<serialization_packet> packets)
	{
		return this->send_many_raw(packets);
	}
};

} // namespace wivrn
