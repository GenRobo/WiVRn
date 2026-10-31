/*
 * WiVRn VR streaming
 * Copyright (C) 2026  GenRobo
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

// Stream health telemetry (GRID #2834): thermal status and the active network, read once per
// second over JNI. Each call is a handful of Java method calls; failures leave the fields unknown.
// getSystemService returns java.lang.Object in the JNI signature; the wrapper resolves methods on the
// runtime class, so the services are called through Object-typed handles like wifi_lock.cpp does.
#include "device_status.h"

#include "application.h"
#include "jnipp.h"
#include <spdlog/spdlog.h>

namespace
{
jni::object<"android/content/Context"> app_context()
{
	jni::object<""> act(application::native_app()->activity->clazz);
	auto app = act.call<jni::object<"android/app/Application">>("getApplication");
	return app.call<jni::object<"android/content/Context">>("getApplicationContext");
}
} // namespace

uint8_t get_thermal_status()
{
	try
	{
		static int api_level = jni::klass("android/os/Build$VERSION").field<jni::Int>("SDK_INT");
		if (api_level < 29)
			return 255;
		auto ctx = app_context();
		auto power = ctx.call<jni::object<"java/lang/Object">>("getSystemService", jni::string("power"));
		int status = power.call<jni::Int>("getCurrentThermalStatus");
		return status < 0 ? 255 : uint8_t(status);
	}
	catch (std::exception & e)
	{
		spdlog::debug("thermal status unavailable: {}", e.what());
		return 255;
	}
}

network_status get_network_status()
{
	network_status s;
	try
	{
		auto ctx = app_context();
		auto cm = ctx.call<jni::object<"java/lang/Object">>("getSystemService", jni::string("connectivity"));
		auto network = cm.call<jni::object<"android/net/Network">>("getActiveNetwork");
		if (not (jobject)network)
			return s;
		auto caps = cm.call<jni::object<"android/net/NetworkCapabilities">>("getNetworkCapabilities", network);
		if (not (jobject)caps)
			return s;
		// TRANSPORT_WIFI 1, TRANSPORT_ETHERNET 3, TRANSPORT_USB 8
		if (caps.call<jni::Bool>("hasTransport", jni::Int(8)))
			s.kind = 4;
		else if (caps.call<jni::Bool>("hasTransport", jni::Int(3)))
			s.kind = 3;
		else if (caps.call<jni::Bool>("hasTransport", jni::Int(1)))
			s.kind = 1;
		int kbps = caps.call<jni::Int>("getLinkUpstreamBandwidthKbps");
		s.link_mbps = kbps > 0 ? uint32_t(kbps / 1000) : 0;

		if (s.kind == 1)
		{
			auto wifi = ctx.call<jni::object<"java/lang/Object">>("getSystemService", jni::string("wifi"));
			auto info = wifi.call<jni::object<"android/net/wifi/WifiInfo">>("getConnectionInfo");
			if ((jobject)info)
			{
				s.rssi_dbm = int16_t(info.call<jni::Int>("getRssi").value);
				int link = info.call<jni::Int>("getLinkSpeed");
				if (link > 0)
					s.link_mbps = uint32_t(link);
			}
		}
	}
	catch (std::exception & e)
	{
		spdlog::debug("network status unavailable: {}", e.what());
	}
	return s;
}
