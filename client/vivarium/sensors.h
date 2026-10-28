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

#pragma once

#include "android/jnipp.h"
#include "depth_sender.h"

#include <optional>
#include <vulkan/vulkan_raii.hpp>
#include <openxr/openxr.h>

namespace xr
{
class instance;
class session;
class system;
} // namespace xr

namespace vivarium
{

// Asks for camera permissions, one dialog after the other; called when the
// "Vivarium camera" setting is turned on so the prompts show in the lobby.
void request_permissions();

// The headset sensors streamed to the PC on the "vivarium-camera" channel while a
// stream session is active: the Kotlin camera module (client/java/org/meumeu/wivrn/
// vivarium, passthrough camera frames and marker detections) and the C++ depth sender.
// Owned by the stream scene; frame() runs between xrBeginFrame and xrEndFrame.
class sensors
{
	jni::object<"org/meumeu/wivrn/vivarium/VivariumCamera"> camera;
	bool camera_started = false;
	bool camera_failed = false;
	bool permission_requested = false;
	std::optional<depth_sender> depth;

public:
	sensors(xr::instance &, xr::system &, xr::session &, vk::raii::Device &, uint32_t queue_family_index);
	~sensors();

	sensors(const sensors &) = delete;
	sensors & operator=(const sensors &) = delete;

	void frame(XrTime display_time);
};

} // namespace vivarium
