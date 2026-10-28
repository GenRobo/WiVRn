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

/**
 * Picks the camera frames to send under a frame-rate cap. A frame passes when at
 * least 1/cap minus [TOLERANCE_NS] has passed since the last sent frame's sensor
 * timestamp, so timestamp jitter does not skip an extra frame: from a 60 Hz
 * camera, a 15 Hz cap sends every 4th frame and a 30 Hz cap every 2nd.
 * Called on the camera thread only.
 */
class FrameRateCap {
    private var lastSentNs: Long? = null

    fun accept(sensorTimestampNs: Long, maxRateHz: Int): Boolean {
        val last = lastSentNs
        if (maxRateHz > 0 && last != null && sensorTimestampNs - last < 1_000_000_000L / maxRateHz - TOLERANCE_NS) {
            return false
        }
        lastSentNs = sensorTimestampNs
        return true
    }

    companion object {
        const val TOLERANCE_NS = 4_000_000L
    }
}
