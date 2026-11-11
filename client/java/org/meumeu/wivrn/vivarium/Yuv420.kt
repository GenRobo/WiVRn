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
/**
 * Which chroma plane (1 = U, 2 = V) starts the interleaved block, or 0 when the chroma is
 * planar. Android maps both planes of an interleaved block onto one buffer, one byte apart,
 * so the plane that starts first has the longer buffer.
 */
internal fun firstChromaPlane(uRemaining: Int, uPixelStride: Int, vRemaining: Int, vPixelStride: Int): Int = when {
    uPixelStride != 2 || vPixelStride != 2 -> 0
    uRemaining > vRemaining -> 1
    vRemaining > uRemaining -> 2
    else -> 0
}

internal fun copyYuv420(src: Image, dst: Image) {
    val width = src.width
    val height = src.height
    val from = src.planes
    val to = dst.planes
    copyPlane(width, height, from[0].buffer, from[0].rowStride, from[0].pixelStride, to[0].buffer, to[0].rowStride, to[0].pixelStride)
    val srcFirst = firstChromaPlane(from[1].buffer.remaining(), from[1].pixelStride, from[2].buffer.remaining(), from[2].pixelStride)
    val dstFirst = firstChromaPlane(to[1].buffer.remaining(), to[1].pixelStride, to[2].buffer.remaining(), to[2].pixelStride)
    if (srcFirst != 0 && srcFirst == dstFirst) {
        // Both sides interleave their chroma in the same order (NV12 or NV21): the block is
        // one packed row of `width` bytes per chroma line from the first plane, moved in bulk
        // (the per-sample path below costs the Quest ~25 % CPU at 30 Hz, live 2026-11-11).
        copyPlane(width, height / 2, from[srcFirst].buffer, from[srcFirst].rowStride, 1, to[dstFirst].buffer, to[dstFirst].rowStride, 1)
        return
    }
    for (plane in 1..2) {
        copyPlane(
            width / 2, height / 2,
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
