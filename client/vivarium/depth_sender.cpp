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

#include "depth_sender.h"
#include "depth_format.h"
#include "vk/shader.h"

#include "android/permissions.h"
#include "application.h"
#include "utils/named_thread.h"
#include "vk/vk_allocator.h"
#include "xr/check.h"
#include "xr/details/enumerate.h"
#include "xr/instance.h"
#include "xr/session.h"
#include "xr/system.h"
#include "xr/to_string.h"

#include <bit>
#include <cmath>
#include <cstring>
#include <limits>
#include <spdlog/spdlog.h>
#include <time.h>

namespace
{
constexpr const char * channel = "vivarium-camera";
constexpr const char * scene_permission = "com.oculus.permission.USE_SCENE";
constexpr uint32_t view_count = 2;
int64_t now_ns(bool boot_clock)
{
	timespec ts;
	clock_gettime(boot_clock ? CLOCK_BOOTTIME : CLOCK_MONOTONIC, &ts);
	return int64_t(ts.tv_sec) * 1'000'000'000 + ts.tv_nsec;
}

} // namespace

std::mutex vivarium::depth_sender::request_mutex;
vivarium::depth_request vivarium::depth_sender::request;

void vivarium::depth_sender::configure(depth_request r)
{
	std::lock_guard lock(request_mutex);
	request = r;
}

vivarium::depth_sender::depth_sender(xr::instance & inst, xr::system & sys, xr::session & s, vk::raii::Device & dev, uint32_t queue_family_index) :
        instance(inst),
        session(s),
        device(dev),
        xrCreateEnvironmentDepthProviderMETA(inst.get_proc<PFN_xrCreateEnvironmentDepthProviderMETA>("xrCreateEnvironmentDepthProviderMETA")),
        xrDestroyEnvironmentDepthProviderMETA(inst.get_proc<PFN_xrDestroyEnvironmentDepthProviderMETA>("xrDestroyEnvironmentDepthProviderMETA")),
        xrStartEnvironmentDepthProviderMETA(inst.get_proc<PFN_xrStartEnvironmentDepthProviderMETA>("xrStartEnvironmentDepthProviderMETA")),
        xrStopEnvironmentDepthProviderMETA(inst.get_proc<PFN_xrStopEnvironmentDepthProviderMETA>("xrStopEnvironmentDepthProviderMETA")),
        xrCreateEnvironmentDepthSwapchainMETA(inst.get_proc<PFN_xrCreateEnvironmentDepthSwapchainMETA>("xrCreateEnvironmentDepthSwapchainMETA")),
        xrDestroyEnvironmentDepthSwapchainMETA(inst.get_proc<PFN_xrDestroyEnvironmentDepthSwapchainMETA>("xrDestroyEnvironmentDepthSwapchainMETA")),
        xrEnumerateEnvironmentDepthSwapchainImagesMETA(inst.get_proc<PFN_xrEnumerateEnvironmentDepthSwapchainImagesMETA>("xrEnumerateEnvironmentDepthSwapchainImagesMETA")),
        xrGetEnvironmentDepthSwapchainStateMETA(inst.get_proc<PFN_xrGetEnvironmentDepthSwapchainStateMETA>("xrGetEnvironmentDepthSwapchainStateMETA")),
        xrAcquireEnvironmentDepthImageMETA(inst.get_proc<PFN_xrAcquireEnvironmentDepthImageMETA>("xrAcquireEnvironmentDepthImageMETA")),
        command_pool(dev, vk::CommandPoolCreateInfo{
                                  .flags = vk::CommandPoolCreateFlagBits::eResetCommandBuffer,
                                  .queueFamilyIndex = queue_family_index,
                          })
{
	if (not sys.meta_environment_depth_properties().supportsEnvironmentDepth)
		throw std::runtime_error("environment depth is not supported on this system");
	if (not (application::get_physical_device().getQueueFamilyProperties().at(queue_family_index).queueFlags & vk::QueueFlagBits::eCompute))
		throw std::runtime_error("OpenXR's Vulkan queue does not support sampled compute readback");

	command_buffer = std::move(dev.allocateCommandBuffers({
	        .commandPool = *command_pool,
	        .level = vk::CommandBufferLevel::ePrimary,
	        .commandBufferCount = 1,
	})[0]);
	fence = dev.createFence({.flags = vk::FenceCreateFlagBits::eSignaled});
	sampler = dev.createSampler({
	        .magFilter = vk::Filter::eNearest, .minFilter = vk::Filter::eNearest,
	        .mipmapMode = vk::SamplerMipmapMode::eNearest,
	        .addressModeU = vk::SamplerAddressMode::eClampToEdge,
	        .addressModeV = vk::SamplerAddressMode::eClampToEdge,
	        .addressModeW = vk::SamplerAddressMode::eClampToEdge,
	});
	std::array bindings{
	        vk::DescriptorSetLayoutBinding{0, vk::DescriptorType::eCombinedImageSampler, 1, vk::ShaderStageFlagBits::eCompute},
	        vk::DescriptorSetLayoutBinding{1, vk::DescriptorType::eStorageBuffer, 1, vk::ShaderStageFlagBits::eCompute},
	};
	descriptor_layout = dev.createDescriptorSetLayout({.bindingCount = uint32_t(bindings.size()), .pBindings = bindings.data()});
	std::array sizes{
	        vk::DescriptorPoolSize{vk::DescriptorType::eCombinedImageSampler, 1},
	        vk::DescriptorPoolSize{vk::DescriptorType::eStorageBuffer, 1},
	};
	descriptor_pool = dev.createDescriptorPool({.flags = vk::DescriptorPoolCreateFlagBits::eFreeDescriptorSet,
	        .maxSets = 1, .poolSizeCount = uint32_t(sizes.size()), .pPoolSizes = sizes.data()});
	descriptor_set = std::move(dev.allocateDescriptorSets({.descriptorPool = *descriptor_pool,
	        .descriptorSetCount = 1, .pSetLayouts = &*descriptor_layout})[0]);
	pipeline_layout = dev.createPipelineLayout({.setLayoutCount = 1, .pSetLayouts = &*descriptor_layout});
	auto shader = load_shader(dev, "vivarium_depth.comp");
	pipeline = dev.createComputePipeline(application::get_pipeline_cache(), {
	        .stage = {.stage = vk::ShaderStageFlagBits::eCompute, .module = *shader, .pName = "main"},
	        .layout = *pipeline_layout,
	});

	worker = utils::named_thread("vivarium-depth", &depth_sender::run, this);
}

vivarium::depth_sender::~depth_sender()
{
	{
		std::lock_guard lock(mutex);
		quit = true;
	}
	cv.notify_all();
	if (worker.joinable())
		worker.join();

	stop_provider();
}

void vivarium::depth_sender::start_provider()
{
	XrEnvironmentDepthProviderCreateInfoMETA provider_info{
	        .type = XR_TYPE_ENVIRONMENT_DEPTH_PROVIDER_CREATE_INFO_META,
	};
	CHECK_XR(xrCreateEnvironmentDepthProviderMETA(session, &provider_info, &provider));

	XrEnvironmentDepthSwapchainCreateInfoMETA swapchain_info{
	        .type = XR_TYPE_ENVIRONMENT_DEPTH_SWAPCHAIN_CREATE_INFO_META,
	};
	CHECK_XR(xrCreateEnvironmentDepthSwapchainMETA(provider, &swapchain_info, &swapchain));

	XrEnvironmentDepthSwapchainStateMETA state{
	        .type = XR_TYPE_ENVIRONMENT_DEPTH_SWAPCHAIN_STATE_META,
	};
	CHECK_XR(xrGetEnvironmentDepthSwapchainStateMETA(swapchain, &state));
	width = state.width;
	height = state.height;
	if (width == 0 or height == 0 or width > 65535 or height > 65535 or uint64_t(width) * height * 4 + 128 > 8 * 1024 * 1024)
		throw std::runtime_error("environment depth dimensions exceed the VCAM wire limits");

	auto swapchain_images = xr::details::enumerate<XrSwapchainImageVulkanKHR>(xrEnumerateEnvironmentDepthSwapchainImagesMETA, swapchain);
	images.clear();
	for (auto & image: swapchain_images)
		images.push_back(image.image);
	if (images.empty())
		throw std::runtime_error("environment depth returned no swapchain images");
	for (auto image : images)
		image_views.push_back(device.createImageView({
		        .image = image, .viewType = vk::ImageViewType::e2DArray,
		        .format = vk::Format::eD16Unorm,
		        .subresourceRange = {.aspectMask = vk::ImageAspectFlagBits::eDepth, .levelCount = 1, .layerCount = view_count},
		}));

	// The pinned META spec guarantees sampled D16, not TRANSFER_SRC usage.
	// Sample into a storage buffer without changing runtime-owned image layouts.
	staging = buffer_allocation(
	        device,
	        vk::BufferCreateInfo{
	                .size = vk::DeviceSize(width) * height * view_count * sizeof(uint32_t),
	                .usage = vk::BufferUsageFlagBits::eStorageBuffer,
	        },
	        VmaAllocationCreateInfo{
	                .flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_RANDOM_BIT,
	                .usage = VMA_MEMORY_USAGE_AUTO,
	        },
	        "vivarium depth staging");

	CHECK_XR(xrStartEnvironmentDepthProviderMETA(provider));
	last_sent = 0;
	spdlog::info("Vivarium depth: provider started, {}x{} x{} images, {} Hz", width, height, images.size(), active.rate_hz);
}

void vivarium::depth_sender::stop_provider()
{
	if (not provider)
		return;

	// The worker may still be reading the staging buffer of a frame in flight
	{
		std::unique_lock lock(mutex);
		cv.wait(lock, [this] { return not pending; });
	}
	if (device.waitForFences(*fence, VK_TRUE, UINT64_MAX) != vk::Result::eSuccess)
		spdlog::warn("Vivarium depth: fence wait failed");

	xrStopEnvironmentDepthProviderMETA(provider);
	image_views.clear();
	if (swapchain)
		xrDestroyEnvironmentDepthSwapchainMETA(swapchain);
	xrDestroyEnvironmentDepthProviderMETA(provider);
	swapchain = XR_NULL_HANDLE;
	provider = XR_NULL_HANDLE;
	images.clear();
	staging = {};
	spdlog::info("Vivarium depth: provider stopped");
}

void vivarium::depth_sender::frame(XrTime display_time)
{
	depth_request wanted;
	{
		std::lock_guard lock(request_mutex);
		wanted = request;
	}

	if (wanted.enabled != active.enabled or wanted.rate_hz != active.rate_hz or wanted.boot_clock != active.boot_clock)
	{
		{
			std::lock_guard lock(mutex);
			if (pending)
				return; // Defer configuration/stop until the worker finishes; no render wait.
		}
		if (not wanted.enabled)
			stop_provider();
		active = wanted;
		failed = false;
		spdlog::info("Vivarium depth: {}, {} Hz", active.enabled ? "on" : "off", active.rate_hz);
	}

	if (not active.enabled)
		return;
	if (failed)
	{
		stop_provider();
		return;
	}

	if (not provider)
	{
		if (not check_permission(scene_permission))
		{
			if (not permission_requested)
			{
				permission_requested = true;
				request_permission(scene_permission, [](bool granted) {
					spdlog::info("Vivarium depth: spatial data permission {}", granted ? "granted" : "denied");
				});
			}
			return;
		}

		try
		{
			start_provider();
		}
		catch (std::exception & e)
		{
			spdlog::error("Vivarium depth: cannot start the depth provider: {}", e.what());
			stop_provider();
			failed = true;
			return;
		}
	}

	if (active.rate_hz > 0 and last_sent and display_time - last_sent < 1'000'000'000 / active.rate_hz)
		return;

	{
		std::lock_guard lock(mutex);
		if (pending)
			return;
	}

	XrEnvironmentDepthImageTimestampMETA timestamp{
	        .type = XR_TYPE_ENVIRONMENT_DEPTH_IMAGE_TIMESTAMP_META,
	};
	XrEnvironmentDepthImageMETA image{
	        .type = XR_TYPE_ENVIRONMENT_DEPTH_IMAGE_META,
	        .next = &timestamp,
	        .views = {{.type = XR_TYPE_ENVIRONMENT_DEPTH_IMAGE_VIEW_META}, {.type = XR_TYPE_ENVIRONMENT_DEPTH_IMAGE_VIEW_META}},
	};
	XrEnvironmentDepthImageAcquireInfoMETA acquire_info{
	        .type = XR_TYPE_ENVIRONMENT_DEPTH_IMAGE_ACQUIRE_INFO_META,
	        .space = application::space(xr::spaces::world),
	        .displayTime = display_time,
	};
	XrResult res;
	{
		auto queue_lock = application::get_queue().lock();
		res = xrAcquireEnvironmentDepthImageMETA(provider, &acquire_info, &image);
	}
	if (res == XR_ENVIRONMENT_DEPTH_NOT_AVAILABLE_META)
		return;
	if (not XR_SUCCEEDED(res))
	{
		spdlog::error("Vivarium depth: xrAcquireEnvironmentDepthImageMETA: {}", xr::to_string(res));
		failed = true;
		return;
	}
	if (image.swapchainIndex >= images.size())
	{
		spdlog::error("Vivarium depth: swapchain index {} out of range", image.swapchainIndex);
		failed = true;
		return;
	}

	// Acquire guarantees SHADER_READ_ONLY_OPTIMAL and the bound queue's ownership.
	// Queue all image reads before xrEndFrame, retaining that layout and ownership.
	vk::DescriptorImageInfo texture{*sampler, *image_views[image.swapchainIndex], vk::ImageLayout::eShaderReadOnlyOptimal};
	vk::DescriptorBufferInfo buffer{staging, 0, VK_WHOLE_SIZE};
	std::array writes{
	        vk::WriteDescriptorSet{.dstSet = *descriptor_set, .dstBinding = 0, .descriptorCount = 1,
	                .descriptorType = vk::DescriptorType::eCombinedImageSampler, .pImageInfo = &texture},
	        vk::WriteDescriptorSet{.dstSet = *descriptor_set, .dstBinding = 1, .descriptorCount = 1,
	                .descriptorType = vk::DescriptorType::eStorageBuffer, .pBufferInfo = &buffer},
	};
	device.updateDescriptorSets(writes, {});
	command_buffer.reset();
	command_buffer.begin({.flags = vk::CommandBufferUsageFlagBits::eOneTimeSubmit});
	command_buffer.bindPipeline(vk::PipelineBindPoint::eCompute, *pipeline);
	command_buffer.bindDescriptorSets(vk::PipelineBindPoint::eCompute, *pipeline_layout, 0, *descriptor_set, {});
	command_buffer.dispatch((width + 7) / 8, (height + 7) / 8, view_count);
	command_buffer.pipelineBarrier(
	        vk::PipelineStageFlagBits::eComputeShader, vk::PipelineStageFlagBits::eHost, {},
	        vk::MemoryBarrier{.srcAccessMask = vk::AccessFlagBits::eShaderWrite, .dstAccessMask = vk::AccessFlagBits::eHostRead}, {}, {});
	command_buffer.end();

	device.resetFences(*fence);
	vk::SubmitInfo submit_info{
	        .commandBufferCount = 1,
	        .pCommandBuffers = &*command_buffer,
	};
	application::get_queue().lock()->submit(submit_info, *fence);

	{
		std::lock_guard lock(mutex);
		pending = true;
		pending_image = image;
		pending_image.next = nullptr;
		pending_time = timestamp.captureTime ? timestamp.captureTime : display_time;
	}
	cv.notify_one();
	last_sent = display_time;
}

void vivarium::depth_sender::run()
{
	while (true)
	{
		XrEnvironmentDepthImageMETA image;
		XrTime xr_time;
		{
			std::unique_lock lock(mutex);
			cv.wait(lock, [this] { return pending or quit; });
			if (quit and not pending)
				return;
			image = pending_image;
			xr_time = pending_time;
		}

		try
		{
			if (device.waitForFences(*fence, VK_TRUE, UINT64_MAX) == vk::Result::eSuccess)
			{
				convert(image, xr_time);
				if (not application::send_app_data(channel, message))
					spdlog::debug("Vivarium depth: no stream session, frame dropped");
			}
		}
		catch (const std::exception & e)
		{
			spdlog::error("Vivarium depth: frame dropped: {}", e.what());
		}

		{
			std::lock_guard lock(mutex);
			pending = false;
		}
		cv.notify_all();
	}
}

void vivarium::depth_sender::convert(const XrEnvironmentDepthImageMETA & image, XrTime xr_time)
{
	const float near_z = image.nearZ;
	const float far_z = image.farZ;
	if (std::bit_cast<uint32_t>(near_z) != std::bit_cast<uint32_t>(lut_near) or std::bit_cast<uint32_t>(far_z) != std::bit_cast<uint32_t>(lut_far))
	{
		lut.resize(65536);
		for (size_t i = 0; i < lut.size(); ++i)
			lut[i] = depth_mm(i, near_z, far_z);
		lut_near = near_z;
		lut_far = far_z;
	}
	std::array<depth_view, view_count> views;
	for (size_t i = 0; i < views.size(); ++i)
	{
		const auto & v = image.views[i];
		views[i] = {v.fov.angleLeft, v.fov.angleRight, v.fov.angleUp, v.fov.angleDown,
		        v.pose.position.x, v.pose.position.y, v.pose.position.z,
		        v.pose.orientation.x, v.pose.orientation.y, v.pose.orientation.z, v.pose.orientation.w,
		        near_z, far_z};
	}
	depth_header(message, xr_time, now_ns(active.boot_clock), width, height, views);
	const auto * in = staging.data<uint32_t>();
	vmaInvalidateAllocation(vk_allocator::instance(), staging, 0, VK_WHOLE_SIZE);
	const size_t pixels = size_t(width) * height * view_count;
	for (size_t i = 0; i < pixels; ++i)
		depth_pixel(message, lut[std::min(in[i], 65535u)]);
	staging.unmap();
}
