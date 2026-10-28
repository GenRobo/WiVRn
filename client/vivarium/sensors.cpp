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

vivarium::sensors::sensors(xr::instance &, xr::system &, xr::session &, vk::raii::Device &, uint32_t) :
        camera(make_camera())
{

}

vivarium::sensors::~sensors()
{
	try
	{
		camera.call<void>("stop");
	}
	catch (std::exception & e)
	{
		spdlog::error("Vivarium camera: stop failed: {}", e.what());
	}
}

void vivarium::sensors::frame(XrTime)
{
	if (not camera_started and not camera_failed)
	{
		if (camera.call<jni::Bool>("hasPermissions"))
		{
			camera_started = camera.call<jni::Bool>("start");
			if (camera_started)
				spdlog::info("Vivarium camera: started");
			else
			{
				spdlog::error("Vivarium camera: cannot start, see the VivariumCamera log");
				camera_failed = true;
			}
		}
		else if (not permission_requested)
		{
			permission_requested = true;
			request_permissions();
		}
	}

}

