/* WiVRn VR streaming
 * Copyright (C) 2026 General Robotics
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#pragma once

#include <boost/url/url_view_base.hpp>
#include <string>

namespace wivrn
{
inline std::string uri_hostname(const boost::urls::url_view_base & uri)
{
	// Boost.URL 1.89's host_address() asserts on encoded IPv6 zone IDs.
	// Decode first, preserving "%usb0" for getaddrinfo, then remove literal brackets.
	auto host = uri.host();
	if (uri.host_type() == boost::urls::host_type::ipv6 or uri.host_type() == boost::urls::host_type::ipvfuture)
	{
		host.pop_back();
		host.erase(0, 1);
	}
	return host;
}
}
