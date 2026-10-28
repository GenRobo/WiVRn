/* WiVRn VR streaming
 * Copyright (C) 2026 General Robotics
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#pragma once

#include <array>
#include <bit>
#include <cmath>
#include <cstdint>
#include <span>
#include <vector>

namespace vivarium
{
// Bit tests remain valid under WiVRn's -ffast-math build.
inline bool finite_positive(float v)
{
    const auto bits = std::bit_cast<uint32_t>(v);
    return bits > 0 and bits < 0x7f800000;
}

inline uint16_t depth_mm(uint32_t sample, float near_z, float far_z)
{
    if (sample >= 65535 or not finite_positive(near_z)) return 0;
    const auto far_bits = std::bit_cast<uint32_t>(far_z);
    // META: farZ < nearZ denotes an infinite projection, not reversed Z.
    const bool infinite = far_bits == 0x7f800000 or ((far_bits & 0x7fffffff) < 0x7f800000 and far_z < near_z);
    if (not infinite and (not finite_positive(far_z) or far_z <= near_z)) return 0;
    const double d = double(sample) / 65535.0;
    const double metres = infinite ? double(near_z) / (1.0 - d)
                                  : double(near_z) * far_z / (far_z - d * (far_z - double(near_z)));
    const double mm = std::round(metres * 1000.0);
    return mm <= 0 ? 0 : mm >= 65535 ? 65535 : uint16_t(mm);
}

inline void depth_u16(std::vector<uint8_t> & out, uint16_t v)
{
    out.push_back(v >> 8);
    out.push_back(v & 255);
}
inline void depth_u32(std::vector<uint8_t> & out, uint32_t v)
{
    depth_u16(out, v >> 16);
    depth_u16(out, v & 65535);
}
inline void depth_i64(std::vector<uint8_t> & out, int64_t v)
{
    depth_u32(out, uint64_t(v) >> 32);
    depth_u32(out, uint64_t(v) & 0xffffffff);
}
// View = fov left/right/up/down, pose xyz/xyzw in STAGE, near/far metres.
using depth_view = std::array<float, 13>;
inline void depth_header(std::vector<uint8_t> & out, int64_t capture_ns, int64_t send_ns,
                         uint16_t width, uint16_t height, std::span<const depth_view> views)
{
    out.clear();
    out.reserve(12 + 24 + views.size() * 52 + size_t(width) * height * views.size() * 2);
    out.insert(out.end(), {'V', 'C', 'A', 'M'});
    depth_u16(out, 1);
    depth_u16(out, 8);
    depth_u32(out, 24 + views.size() * 52 + size_t(width) * height * views.size() * 2);
    depth_i64(out, capture_ns);
    depth_i64(out, send_ns);
    depth_u16(out, width);
    depth_u16(out, height);
    out.push_back(views.size());
    out.insert(out.end(), {0, 0, 0}); // raw, reserved
    for (const auto & view : views)
        for (float value : view) depth_u32(out, std::bit_cast<uint32_t>(value));
}
inline void depth_pixel(std::vector<uint8_t> & out, uint16_t mm)
{
    out.push_back(mm & 255);
    out.push_back(mm >> 8);
}
} // namespace vivarium
