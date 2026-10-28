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

#include "sensors.h"

#include "android/permissions.h"
#include "application.h"
#include "xr/instance.h"
#include "xr/system.h"

#include <iterator>
#include <spdlog/spdlog.h>
#include <stdexcept>

namespace
{
// Camera access (the headset camera permission exists on Horizon OS v74+ only).
constexpr const char * permissions[] = {
        "android.permission.CAMERA",
        "horizonos.permission.HEADSET_CAMERA",
};

void request_from(size_t index)
{
	if (index >= std::size(permissions))
		return;
	// Ask for both camera permissions; depth requests its own spatial permission.
	request_permission(permissions[index], [index](bool) { request_from(index + 1); });
}

// Loads the Kotlin class through the activity's class loader: FindClass from a native
// thread only sees system classes
jni::object<"org/meumeu/wivrn/vivarium/VivariumCamera"> make_camera()
{
	auto & env = jni::jni_thread::env();
	jni::object<"org/meumeu/wivrn/MainActivity"> activity(application::native_app()->activity->clazz);
	auto loader = activity.call<jni::object<"java/lang/ClassLoader">>("getClassLoader");
	jni::string name("org.meumeu.wivrn.vivarium.VivariumCamera");
	auto klass = loader.call<jni::object<"java/lang/Class">>("loadClass", name);

	jmethodID constructor = env.GetMethodID((jclass)klass.handle(), "<init>", "(Lorg/meumeu/wivrn/MainActivity;)V");
	if (not constructor)
		throw std::runtime_error("VivariumCamera constructor not found");
	jobject local = env.NewObject((jclass)klass.handle(), constructor, activity.handle());
	jni::details::handle_java_exception();
	jni::object<"org/meumeu/wivrn/vivarium/VivariumCamera"> camera(local);
	env.DeleteLocalRef(local);
	return camera;
}
} // namespace

void vivarium::request_permissions()
{
	request_from(0);
}

vivarium::sensors::sensors(xr::instance & instance, xr::system & system, xr::session & session, vk::raii::Device & device, uint32_t queue_family_index,
                           bool marker_tracking, bool rgb_camera_streaming, bool depth_camera_streaming) :
        camera(make_camera()),
        marker_tracking(marker_tracking),
        rgb_camera_streaming(rgb_camera_streaming),
        depth_camera_streaming(depth_camera_streaming)
{
	if (not depth_camera_streaming)
		return;
	if (instance.has_extension(XR_META_ENVIRONMENT_DEPTH_EXTENSION_NAME))
	{
		try
		{
			depth.emplace(instance, system, session, device, queue_family_index);
		}
		catch (std::exception & e)
		{
			spdlog::warn("Vivarium depth: unavailable: {}", e.what());
		}
	}
	else
		spdlog::info("Vivarium depth: {} is not supported by the runtime", XR_META_ENVIRONMENT_DEPTH_EXTENSION_NAME);
}

vivarium::sensors::~sensors()
{
	try
	{
		camera.call<void>("stop");
	}
	catch (std::exception & e)
	{
		spdlog::error("Sensor streaming: stop failed: {}", e.what());
	}
	depth.reset();
}

void vivarium::sensors::frame(XrTime display_time)
{
	if (not camera_started and not camera_failed)
	{
		if (not (marker_tracking or rgb_camera_streaming) or camera.call<jni::Bool>("hasPermissions"))
		{
			camera_started = camera.call<jni::Bool>("start", jni::Bool(marker_tracking), jni::Bool(rgb_camera_streaming), jni::Bool(depth_camera_streaming));
			if (camera_started)
				spdlog::info("Sensor streaming: started");
			else
			{
				spdlog::error("Sensor streaming: cannot start, see the SensorStreaming log");
				camera_failed = true;
			}
		}
		else if (not permission_requested)
		{
			permission_requested = true;
			request_permissions();
		}
	}

	if (depth)
		depth->frame(display_time);
}

extern "C" JNIEXPORT void JNICALL Java_org_meumeu_wivrn_vivarium_VivariumCamera_configureDepth(JNIEnv *, jobject, jboolean enabled, jint rate_hz, jboolean boot_clock)
{
	vivarium::depth_sender::configure({
	        .enabled = enabled == JNI_TRUE,
	        .rate_hz = rate_hz,
	        .boot_clock = boot_clock == JNI_TRUE,
	});
}
