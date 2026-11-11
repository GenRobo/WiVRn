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

import kotlin.math.hypot
import kotlin.math.roundToInt

/**
 * The image the marker detector searches: a frame wider than [FULL_WIDTH] is scaled by
 * [scale] (clamped to [MIN_SCALE]..1) to [detectWidth]x[detectHeight] first, so the
 * candidate search costs a quarter of the frame at 0.5; a narrower frame, or any frame
 * at the default scale of 1, is searched as is. [toFrame] maps corners found in the
 * scaled image back to frame pixels, where they are refined to subpixel precision on
 * the full-resolution frame, so the scale costs candidate recall on small markers but
 * not corner precision. Half scale lost five of six 70 mm markers at the operator's
 * distance (about 25 px a side, under the detector's recall floor), hence the default.
 */
class DetectScale(val width: Int, val height: Int, scale: Float) {
    val scale = if (width > FULL_WIDTH) scale.coerceIn(MIN_SCALE, 1f) else 1f
    val detectWidth = (width * this.scale).roundToInt().coerceAtLeast(1)
    val detectHeight = (height * this.scale).roundToInt().coerceAtLeast(1)
    val scaled = detectWidth != width || detectHeight != height
    private val factorX = width.toFloat() / detectWidth
    private val factorY = height.toFloat() / detectHeight

    /**
     * Maps the first [count] x,y pairs of [xy] from detection to frame pixels in place.
     * Pixel centres align (area resampling): detection pixel 0 of a half-scale image
     * covers frame pixels 0 and 1, so it maps to 0.5, not 0.
     */
    fun toFrame(xy: FloatArray, count: Int = xy.size / 2) {
        if (!scaled) return
        for (i in 0 until count) {
            xy[2 * i] = (xy[2 * i] + 0.5f) * factorX - 0.5f
            xy[2 * i + 1] = (xy[2 * i + 1] + 0.5f) * factorY - 0.5f
        }
    }

    companion object {
        /** Frames up to this wide are searched at full resolution. */
        const val FULL_WIDTH = 640
        const val DEFAULT = 1f
        const val MIN_SCALE = 0.25f

        /**
         * OpenCV's ArUco subpixel refinement window for a marker whose four corners are the
         * x,y pairs in [xy] (frame pixels): [relative] of one module (a bit cell, the side
         * over [markerSize] plus the two borders of [borderBits]), at least 1 and at most
         * [max]; the search spans twice the window plus one pixel each way.
         */
        fun refineWindow(xy: FloatArray, markerSize: Int, borderBits: Int = 1, relative: Float = 0.3f, max: Int = 5): Int {
            var perimeter = 0f
            for (i in 0 until 4) {
                val j = (i + 1) % 4
                perimeter += hypot(xy[2 * j] - xy[2 * i], xy[2 * j + 1] - xy[2 * i + 1])
            }
            val module = perimeter / 4 / (markerSize + 2 * borderBits)
            return (relative * module).roundToInt().coerceIn(1, max)
        }
    }
}
