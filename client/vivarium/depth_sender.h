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

#include "vk/allocation.h"

#include <condition_variable>
#include <mutex>
#include <thread>
#include <vector>
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

// What the PC asked for in its CONFIGURE message, parsed by the Kotlin camera module
// (StreamFormat.parseConfigure) and handed over through VivariumCamera.configureDepth.
struct depth_request
{
	bool enabled = false;
	int rate_hz = 0; // 0: every frame
	bool boot_clock = false; // send_time_ns on CLOCK_BOOTTIME (as PONG) instead of CLOCK_MONOTONIC
};

// Sends XR_META_environment_depth frames to the PC as DEPTH messages (camera stream
// format v1 type 8) on the "vivarium-camera" channel while the PC asks for them.
//
// The render thread acquires the depth image between xrBeginFrame and xrEndFrame and
// copies it to a host-visible buffer; a worker thread waits for the copy, converts the
// normalized D16 depth to metric millimetres with the frame's near and far planes, and
// sends the message, so the render thread never waits on the GPU or the network.
class depth_sender
{
	static std::mutex request_mutex;
	static depth_request request;

	xr::instance & instance;
	xr::session & session;
	vk::raii::Device & device;

	PFN_xrCreateEnvironmentDepthProviderMETA xrCreateEnvironmentDepthProviderMETA{};
	PFN_xrDestroyEnvironmentDepthProviderMETA xrDestroyEnvironmentDepthProviderMETA{};
	PFN_xrStartEnvironmentDepthProviderMETA xrStartEnvironmentDepthProviderMETA{};
	PFN_xrStopEnvironmentDepthProviderMETA xrStopEnvironmentDepthProviderMETA{};
	PFN_xrCreateEnvironmentDepthSwapchainMETA xrCreateEnvironmentDepthSwapchainMETA{};
	PFN_xrDestroyEnvironmentDepthSwapchainMETA xrDestroyEnvironmentDepthSwapchainMETA{};
	PFN_xrEnumerateEnvironmentDepthSwapchainImagesMETA xrEnumerateEnvironmentDepthSwapchainImagesMETA{};
	PFN_xrGetEnvironmentDepthSwapchainStateMETA xrGetEnvironmentDepthSwapchainStateMETA{};
	PFN_xrAcquireEnvironmentDepthImageMETA xrAcquireEnvironmentDepthImageMETA{};

	XrEnvironmentDepthProviderMETA provider = XR_NULL_HANDLE;
	XrEnvironmentDepthSwapchainMETA swapchain = XR_NULL_HANDLE;
	std::vector<vk::Image> images;
	std::vector<vk::raii::ImageView> image_views;
	uint32_t width = 0;
	uint32_t height = 0;
	bool failed = false; // provider creation failed for this request; retried when the request changes
	bool permission_requested = false;
	depth_request active;
	XrTime last_sent = 0;

	vk::raii::CommandPool command_pool;
	vk::raii::CommandBuffer command_buffer = nullptr;
	vk::raii::Fence fence = nullptr;
	buffer_allocation staging;
	vk::raii::Sampler sampler = nullptr;
	vk::raii::DescriptorSetLayout descriptor_layout = nullptr;
	vk::raii::DescriptorPool descriptor_pool = nullptr;
	vk::raii::DescriptorSet descriptor_set = nullptr;
	vk::raii::PipelineLayout pipeline_layout = nullptr;
	vk::raii::Pipeline pipeline = nullptr;

	// Worker: one frame in flight; the render thread only records a new copy once the
	// previous message has been sent.
	std::mutex mutex;
	std::condition_variable cv;
	bool pending = false;
	bool quit = false;
	XrEnvironmentDepthImageMETA pending_image{};
	XrTime pending_time = 0;
	std::thread worker;
	std::vector<uint8_t> message;
	// Normalized D16 to millimetres for the current near and far planes
	std::vector<uint16_t> lut;
	float lut_near = -1;
	float lut_far = -1;

	void start_provider();
	void stop_provider();
	void run();
	void convert(const XrEnvironmentDepthImageMETA & image, XrTime xr_time);

public:
	// From the Kotlin module, any thread; applied on the next frame()
	static void configure(depth_request);

	depth_sender(xr::instance &, xr::system &, xr::session &, vk::raii::Device &, uint32_t queue_family_index);
	~depth_sender();

	// Render thread, between xrBeginFrame and xrEndFrame
	void frame(XrTime display_time);
};

} // namespace vivarium
