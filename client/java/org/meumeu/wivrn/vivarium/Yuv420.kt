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

package org.meumeu.wivrn.vivarium

import java.nio.ByteBuffer

/**
 * Copies an even-sized YUV_420_888 image into a caller-owned, tightly packed NV21
 * buffer: full-resolution Y followed by interleaved VU at half width and height.
 * Each plane starts at its buffer's current position; row padding and interleaved
 * chroma are skipped, including when the last row has no trailing padding.
 * Source positions and limits are preserved. No scratch buffers are allocated.
 */
internal fun copyYuv420ToNv21(
    width: Int,
    height: Int,
    y: ByteBuffer,
    yRowStride: Int,
    yPixelStride: Int,
    u: ByteBuffer,
    uRowStride: Int,
    uPixelStride: Int,
    v: ByteBuffer,
    vRowStride: Int,
    vPixelStride: Int,
    nv21: ByteArray,
) {
    val yStart = y.position()
    try {
        for (row in 0 until height) {
            val source = yStart + row * yRowStride
            val destination = row * width
            if (yPixelStride == 1) {
                y.position(source)
                y.get(nv21, destination, width)
            } else {
                for (column in 0 until width) {
                    nv21[destination + column] = y.get(source + column * yPixelStride)
                }
            }
        }
    } finally {
        y.position(yStart)
    }

    val uStart = u.position()
    val vStart = v.position()
    var destination = width * height
    for (row in 0 until height / 2) {
        val uRow = uStart + row * uRowStride
        val vRow = vStart + row * vRowStride
        for (column in 0 until width / 2) {
            nv21[destination++] = v.get(vRow + column * vPixelStride)
            nv21[destination++] = u.get(uRow + column * uPixelStride)
        }
    }
}
