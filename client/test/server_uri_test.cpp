/* WiVRn VR streaming
 * Copyright (C) 2026 General Robotics
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include "../server_uri.h"
#include <boost/url/parse.hpp>
#include <boost/version.hpp>
#include <iostream>
#include <stdexcept>
#include <string_view>

int main()
{
	const struct
	{
		std::string_view uri;
		std::string_view hostname;
	} cases[] = {
	        {"wivrn://:123456@[fe80::1234%25usb0]:9757", "fe80::1234%usb0"},
	        {"wivrn+tcp://[fe80::1234%2518]:9757", "fe80::1234%18"},
	        {"wivrn://[2001:db8::1]:9757", "2001:db8::1"},
	        {"wivrn://192.0.2.1:9757", "192.0.2.1"},
	        {"wivrn://workstation.local:9757", "workstation.local"},
	        {"wivrn://%77orkstation.local:9757", "workstation.local"},
	        {"wivrn://[v1.abc]:9757", "v1.abc"},
	};
	std::cout << "Boost " << BOOST_LIB_VERSION << "; assertions "
#ifdef NDEBUG
	          << "disabled"
#else
	          << "enabled"
#endif
	          << std::endl;
	for (const auto & test : cases)
	{
		auto parsed = boost::urls::parse_uri(test.uri);
		if (not parsed)
			throw std::runtime_error("could not parse " + std::string(test.uri));
		auto actual = wivrn::uri_hostname(*parsed);
		if (actual != test.hostname)
			throw std::runtime_error("wrong resolver hostname for " + std::string(test.uri) + ": " + actual);
	}
	std::cout << "7 server URI hostname cases passed\n";
}
