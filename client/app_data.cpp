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

#include "application.h"
#include "wivrn_client.h"
#include "wivrn_packets.h"

#include <algorithm>
#include <cassert>
#include <spdlog/spdlog.h>

#ifdef __ANDROID__
#include "android/jnipp.h"
#include <jni.h>
#endif

bool application::send_app_data(std::string_view channel, std::span<const uint8_t> data)
{
	auto & self = instance();
	// Held for the whole call: the chunks of one message are not interleaved with another sender's
	std::lock_guard lock(self.app_data_session_mutex);
	if (not self.app_data_session)
		return false;

	try
	{
		for (size_t offset = 0; offset < data.size(); offset += app_data_max_size)
		{
			auto chunk = data.subspan(offset, std::min(app_data_max_size, data.size() - offset));
			self.app_data_session->send_control(from_headset::app_data{
			        .channel = std::string(channel),
			        .data = std::vector<uint8_t>(chunk.begin(), chunk.end()),
			});
		}
		return true;
	}
	catch (std::exception & e)
	{
		spdlog::warn("Cannot send app data on channel {}: {}", channel, e.what());
		return false;
	}
}

void application::set_app_data_handler(std::string channel, std::function<void(std::span<const uint8_t>)> handler)
{
	auto & self = instance();
	std::lock_guard lock(self.app_data_handlers_mutex);
	if (handler)
		self.app_data_handlers[std::move(channel)] = std::make_shared<const std::function<void(std::span<const uint8_t>)>>(std::move(handler));
	else
		self.app_data_handlers.erase(channel);
}

void application::set_app_data_session(wivrn_session * session)
{
	auto & self = instance();
	std::lock_guard lock(self.app_data_session_mutex);
	self.app_data_session = session;
}

void application::clear_app_data_session(wivrn_session * session)
{
	auto & self = instance();
	std::lock_guard lock(self.app_data_session_mutex);
	if (self.app_data_session == session)
		self.app_data_session = nullptr;
}

#ifdef __ANDROID__
// MainActivity.onAppData(String channel, byte[] data), called on the stream's network thread which is JNI-attached
static void call_java_on_app_data(const std::string & channel, std::span<const uint8_t> data)
{
	auto & env = jni::jni_thread::env();
	jobject activity = application::native_app()->activity->clazz;
	static jmethodID method = env.GetMethodID(env.GetObjectClass(activity), "onAppData", "(Ljava/lang/String;[B)V");
	assert(method);

	jstring jchannel = env.NewStringUTF(channel.c_str());
	jbyteArray jdata = env.NewByteArray(data.size());
	env.SetByteArrayRegion(jdata, 0, data.size(), reinterpret_cast<const jbyte *>(data.data()));
	env.CallVoidMethod(activity, method, jchannel, jdata);
	if (env.ExceptionCheck())
	{
		env.ExceptionDescribe();
		env.ExceptionClear();
	}
	env.DeleteLocalRef(jdata);
	env.DeleteLocalRef(jchannel);
}
#endif

void application::dispatch_app_data(wivrn::to_headset::app_data && packet)
{
	auto & self = instance();
	std::shared_ptr<const std::function<void(std::span<const uint8_t>)>> handler;
	{
		std::lock_guard lock(self.app_data_handlers_mutex);
		if (auto it = self.app_data_handlers.find(packet.channel); it != self.app_data_handlers.end())
			handler = it->second;
	}
	if (handler)
	{
		(*handler)(packet.data);
		return;
	}
#ifdef __ANDROID__
	call_java_on_app_data(packet.channel, packet.data);
#else
	spdlog::debug("No handler for app data on channel {}, {} bytes dropped", packet.channel, packet.data.size());
#endif
}

#ifdef __ANDROID__
extern "C" jboolean Java_org_meumeu_wivrn_MainActivity_sendAppData(JNIEnv * env, jobject instance, jstring jchannel, jbyteArray jdata)
{
	if (not jchannel or not jdata)
		return false;

	const char * channel = env->GetStringUTFChars(jchannel, nullptr);
	jsize size = env->GetArrayLength(jdata);
	// Copy out instead of pinning: the send may block and must not hold the array
	thread_local std::vector<uint8_t> buffer;
	buffer.resize(size);
	env->GetByteArrayRegion(jdata, 0, size, reinterpret_cast<jbyte *>(buffer.data()));

	bool sent = application::send_app_data(channel, buffer);

	env->ReleaseStringUTFChars(jchannel, channel);
	return sent;
}
#endif
