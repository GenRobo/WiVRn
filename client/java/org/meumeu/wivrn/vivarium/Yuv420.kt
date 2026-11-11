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

import android.media.Image
import java.nio.ByteBuffer

/** An image size; `android.util.Size` is unusable in JVM unit tests. */
data class StreamSize(val width: Int, val height: Int) {
    override fun toString() = "${width}x$height"
}

/**
 * The size the camera stream is encoded at: [configured] when the camera offers it as a
 * YUV_420_888 output, else the [capture] size that marker detection uses anyway. The
 * caller logs the fallback.
 */
internal fun chooseStreamSize(configured: StreamSize, supported: List<StreamSize>, capture: StreamSize): StreamSize =
    if (configured == capture || configured in supported) configured else capture

/**
 * Copies one YUV_420_888 plane of [width] by [height] samples between buffers that each
 * have their own row and pixel stride (a camera image to an encoder input image). Each
 * plane starts at its buffer's current position; positions and limits are preserved.
 * Chroma planes pass their own (halved) size. No scratch buffers are allocated.
 */
internal fun copyPlane(
    width: Int,
    height: Int,
    src: ByteBuffer,
    srcRowStride: Int,
    srcPixelStride: Int,
    dst: ByteBuffer,
    dstRowStride: Int,
    dstPixelStride: Int,
) {
    val srcStart = src.position()
    val srcLimit = src.limit()
    val dstStart = dst.position()
    val dstLimit = dst.limit()
    try {
        if (srcPixelStride == 1 && dstPixelStride == 1) {
            for (row in 0 until height) {
                val from = srcStart + row * srcRowStride
                src.limit(srcLimit)
                src.position(from)
                src.limit(from + width)
                dst.position(dstStart + row * dstRowStride)
                dst.put(src)
            }
        } else {
            for (row in 0 until height) {
                val from = srcStart + row * srcRowStride
                val to = dstStart + row * dstRowStride
                for (column in 0 until width) {
                    dst.put(to + column * dstPixelStride, src.get(from + column * srcPixelStride))
                }
            }
        }
    } finally {
        src.limit(srcLimit)
        src.position(srcStart)
        dst.limit(dstLimit)
        dst.position(dstStart)
    }
}

/** Copies a YUV_420_888 [src] into the same-sized encoder input image [dst], plane by plane. */
internal fun copyYuv420(src: Image, dst: Image) {
    val width = src.width
    val height = src.height
    val from = src.planes
    val to = dst.planes
    for (plane in 0..2) {
        val divisor = if (plane == 0) 1 else 2
        copyPlane(
            width / divisor, height / divisor,
            from[plane].buffer, from[plane].rowStride, from[plane].pixelStride,
            to[plane].buffer, to[plane].rowStride, to[plane].pixelStride,
        )
    }
}

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
