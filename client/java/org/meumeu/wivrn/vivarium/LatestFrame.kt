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
import java.util.concurrent.TimeUnit
import java.util.concurrent.locks.ReentrantLock
import kotlin.concurrent.withLock

/** A luma image in a direct buffer (OpenCV wraps it without a copy) and its capture metadata. */
class LumaFrame(val index: Int, val width: Int, val height: Int) {
    val pixels: ByteBuffer = ByteBuffer.allocateDirect(width * height)
    var frameNumber = 0L
    var sensorTimestampNs = 0L
    var exposureTimeNs = -1L
    var rollingShutterSkewNs = -1L
    var dictionary = 0

    /** Copies YUV_420_888 luma (pixel stride 1), preserving the plane's position and limit. */
    fun copyLuma(plane: ByteBuffer, rowStride: Int) {
        val position = plane.position()
        val limit = plane.limit()
        require(plane.remaining() >= (height - 1) * rowStride + width)
        pixels.clear()
        try {
            for (row in 0 until height) {
                val start = position + row * rowStride
                plane.position(start)
                plane.limit(start + width)
                pixels.put(plane)
                plane.limit(limit)
            }
        } finally {
            plane.limit(limit)
            plane.position(position)
        }
    }

    fun setCapture(frameNumber: Long, exposureTimeNs: Long, rollingShutterSkewNs: Long) {
        this.frameNumber = frameNumber
        this.exposureTimeNs = exposureTimeNs
        this.rollingShutterSkewNs = rollingShutterSkewNs
    }
}

/**
 * Hands the newest camera frame from the camera thread to the detector thread with
 * three preallocated frames: the camera thread fills [back], [publish] swaps it with
 * the waiting frame, and [take] swaps the waiting frame to the detector. Neither side
 * waits for the other's work, and a frame the detector has not taken yet is replaced
 * by a newer one.
 */
class LatestFrame(width: Int, height: Int) {
    val frames = List(3) { LumaFrame(it, width, height) }
    private val lock = ReentrantLock()
    private val published = lock.newCondition()
    private var back = frames[0]
    private var ready = frames[1]
    private var front = frames[2]
    private var fresh = false

    /** The frame the camera thread fills; the detector never sees it before [publish]. */
    fun back(): LumaFrame = back

    /** Camera thread: offers the filled frame; returns true when it replaced one not yet taken. */
    fun publish(): Boolean = lock.withLock {
        val replaced = fresh
        back = ready.also { ready = back }
        fresh = true
        published.signal()
        replaced
    }

    /** Detector thread: the newest frame, or null after [timeoutMs]; it stays the detector's until the next take. */
    fun take(timeoutMs: Long): LumaFrame? = lock.withLock {
        var waitNs = TimeUnit.MILLISECONDS.toNanos(timeoutMs)
        while (!fresh && waitNs > 0) waitNs = published.awaitNanos(waitNs)
        if (!fresh) return null
        front = ready.also { ready = front }
        fresh = false
        front
    }
}
