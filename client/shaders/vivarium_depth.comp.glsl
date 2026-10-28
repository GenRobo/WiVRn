// WiVRn VR streaming
// Copyright (C) 2026 General Robotics
// SPDX-License-Identifier: GPL-3.0-or-later
#version 450
layout(local_size_x = 8, local_size_y = 8, local_size_z = 1) in;
layout(set = 0, binding = 0) uniform sampler2DArray depthImage;
layout(std430, set = 0, binding = 1) writeonly buffer Readback { uint samples[]; };
void main()
{
    ivec3 size = textureSize(depthImage, 0);
    ivec3 pixel = ivec3(gl_GlobalInvocationID);
    if (any(greaterThanEqual(pixel, size))) return;
    uint offset = uint((pixel.z * size.y + pixel.y) * size.x + pixel.x);
    samples[offset] = uint(round(texelFetch(depthImage, pixel, 0).r * 65535.0));
}
