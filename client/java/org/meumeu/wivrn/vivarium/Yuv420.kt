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
 * Which chroma plane (1 = U, 2 = V) starts an interleaved block, read from the samples: the
 * two planes are views of one buffer one byte apart, so with U first `U[k + 1]` is `V[k]`,
 * with V first `V[k + 1]` is `U[k]`. 0 when the planes are not interleaved or neither
 * relation holds over [samples] pairs; a uniform region satisfies both and either order
 * copies it right. Android gives both planes the same length, so lengths tell nothing.
 */
internal fun interleavedOrder(u: ByteBuffer, uPixelStride: Int, v: ByteBuffer, vPixelStride: Int, samples: Int = 16): Int {
    if (uPixelStride != 2 || vPixelStride != 2) return 0
    val pairs = minOf(samples, (minOf(u.remaining(), v.remaining()) - 1) / 2)
    if (pairs <= 0) return 0
    var uFirst = true
    var vFirst = true
    for (i in 0 until pairs) {
        val k = 2 * i
        if (u.get(u.position() + k + 1) != v.get(v.position() + k)) uFirst = false
        if (v.get(v.position() + k + 1) != u.get(u.position() + k)) vFirst = false
    }
    return if (uFirst) 1 else if (vFirst) 2 else 0
}

/**
 * [interleavedOrder] for a writable block whose content is unknown (a codec input image):
 * a byte written through one plane is read back through the other, then restored.
 */
internal fun probeInterleavedOrder(u: ByteBuffer, uPixelStride: Int, v: ByteBuffer, vPixelStride: Int): Int {
    if (uPixelStride != 2 || vPixelStride != 2 || u.remaining() < 2 || v.remaining() < 2) return 0
    val mark = u.get(u.position() + 1)
    val probe = (mark + 1).toByte()
    u.put(u.position() + 1, probe)
    val uFirst = v.get(v.position()) == probe
    u.put(u.position() + 1, mark)
    if (uFirst) return 1
    val markV = v.get(v.position() + 1)
    val probeV = (markV + 1).toByte()
    v.put(v.position() + 1, probeV)
    val vFirst = u.get(u.position()) == probeV
    v.put(v.position() + 1, markV)
    return if (vFirst) 2 else 0
}

private var layoutLogged = false

internal fun copyYuv420(src: Image, dst: Image) {
    val width = src.width
    val height = src.height
    val from = src.planes
    val to = dst.planes
    copyPlane(width, height, from[0].buffer, from[0].rowStride, from[0].pixelStride, to[0].buffer, to[0].rowStride, to[0].pixelStride)
    val srcFirst = interleavedOrder(from[1].buffer, from[1].pixelStride, from[2].buffer, from[2].pixelStride)
    val dstFirst = probeInterleavedOrder(to[1].buffer, to[1].pixelStride, to[2].buffer, to[2].pixelStride)
    if (!layoutLogged) {
        layoutLogged = true
        android.util.Log.i("SensorStreaming", "yuv copy: src strides ${from.map { "${it.rowStride}/${it.pixelStride}" }} order $srcFirst, dst strides ${to.map { "${it.rowStride}/${it.pixelStride}" }} order $dstFirst")
    }
    if (srcFirst != 0 && srcFirst == dstFirst) {
        // Both sides interleave their chroma in the same order (NV12 or NV21): each plane's
        // row is one run of `width - 1` bytes from its first sample to its last, with the other
        // plane's samples in between at the same offsets, so a bulk put per row moves it; the
        // two runs overlap, which is cheaper than the per-sample path below (a whole core on
        // the Quest at 30 Hz, live 2026-11-11). A plane's buffer ends at its own last sample.
        for (plane in 1..2) {
            copyPlane(width - 1, height / 2, from[plane].buffer, from[plane].rowStride, 1, to[plane].buffer, to[plane].rowStride, 1)
        }
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
