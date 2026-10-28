/*
 * WiVRn VR streaming
 * Copyright (C) 2026  Geordie Moffatt <geordie.moffatt@generalrobotics.company>
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

#include "dnssd_publisher_windows.h"

#include <chrono>
#include <iostream>
#include <system_error>
#include <vector>

namespace
{
std::wstring widen(const std::string & s)
{
	if (s.empty())
		return {};
	int size = MultiByteToWideChar(CP_UTF8, 0, s.data(), int(s.size()), nullptr, 0);
	std::wstring result(size, L'\0');
	MultiByteToWideChar(CP_UTF8, 0, s.data(), int(s.size()), result.data(), size);
	return result;
}

std::string narrow(const wchar_t * s)
{
	if (not s or not *s)
		return {};
	int size = WideCharToMultiByte(CP_UTF8, 0, s, -1, nullptr, 0, nullptr, nullptr);
	std::string result(size, '\0');
	WideCharToMultiByte(CP_UTF8, 0, s, -1, result.data(), size, nullptr, nullptr);
	result.pop_back(); // terminating null
	return result;
}

std::string error_message(DWORD status)
{
	return std::to_string(status) + " (" + std::system_category().message(int(status)) + ")";
}

// Host name for the SRV record: the machine's own .local name. Windows' mDNS responder answers for
// its addresses on each interface, including IPv6 link-local only ones (USB networking).
std::string local_host_name()
{
	wchar_t buffer[256];
	DWORD size = std::size(buffer);
	if (not GetComputerNameExW(ComputerNameDnsHostname, buffer, &size))
		throw std::system_error(int(GetLastError()), std::system_category(), "GetComputerNameExW");
	return narrow(buffer) + ".local";
}
} // namespace

wivrn::dnssd_publisher::dnssd_publisher(const std::string & name, const std::string & type, int port, const std::map<std::string, std::string> & txt)
{
	std::string service_name = name + "." + type + ".local";
	std::string host_name = local_host_name();

	std::vector<std::wstring> keys;
	std::vector<std::wstring> values;
	for (const auto & [key, value]: txt)
	{
		keys.push_back(widen(key));
		values.push_back(widen(value));
	}
	std::vector<PCWSTR> key_ptrs;
	std::vector<PCWSTR> value_ptrs;
	for (size_t i = 0; i < keys.size(); i++)
	{
		key_ptrs.push_back(keys[i].c_str());
		value_ptrs.push_back(values[i].c_str());
	}

	// The instance holds copies of the strings
	instance = DnsServiceConstructInstance(widen(service_name).c_str(), widen(host_name).c_str(), nullptr, nullptr, WORD(port), 0, 0, DWORD(keys.size()), key_ptrs.data(), value_ptrs.data());
	if (not instance)
		throw std::system_error(int(GetLastError()), std::system_category(), "DnsServiceConstructInstance");

	request = {
	        .Version = DNS_QUERY_REQUEST_VERSION1,
	        .InterfaceIndex = 0, // all interfaces
	        .pServiceInstance = instance,
	        .pRegisterCompletionCallback = completion_callback,
	        .pQueryContext = this,
	        .hCredentials = nullptr,
	        .unicastEnabled = FALSE,
	};

	DWORD status = DnsServiceRegister(&request, nullptr);
	if (status != DNS_REQUEST_PENDING)
	{
		DnsServiceFreeInstance(instance);
		throw std::system_error(int(status), std::system_category(), "DnsServiceRegister");
	}

	std::cerr << "Publishing " << service_name << " with mDNS, port " << port << ", host " << host_name << std::endl;
}

void WINAPI wivrn::dnssd_publisher::completion_callback(DWORD status, PVOID context, PDNS_SERVICE_INSTANCE instance)
{
	auto self = static_cast<dnssd_publisher *>(context);
	std::unique_lock lock(self->mutex);

	// The first completion is the registration, the second one the deregistration
	if (self->completions == 0)
	{
		if (status == ERROR_SUCCESS)
			std::cerr << "mDNS service registered: " << narrow(instance ? instance->pszInstanceName : nullptr) << std::endl;
		else
			std::cerr << "mDNS service registration failed: " << error_message(status) << std::endl;
	}

	if (instance)
		DnsServiceFreeInstance(instance);

	self->completions++;
	self->cv.notify_all();
}

wivrn::dnssd_publisher::~dnssd_publisher()
{
	using namespace std::chrono_literals;

	std::unique_lock lock(mutex);
	// Deregistering before the registration completed would race with it
	cv.wait_for(lock, 5s, [this] { return completions >= 1; });
	lock.unlock();

	DWORD status = DnsServiceDeRegister(&request, nullptr);
	lock.lock();
	if (status == DNS_REQUEST_PENDING)
	{
		// The completion callback uses this object: wait for it
		if (not cv.wait_for(lock, 5s, [this] { return completions >= 2; }))
			std::cerr << "mDNS service deregistration did not complete" << std::endl;
	}
	else
		std::cerr << "DnsServiceDeRegister failed: " << error_message(status) << std::endl;

	DnsServiceFreeInstance(instance);
}
