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
 * Picks the camera frames the marker detector sees. The PC's detection rate cap applies
 * once a marker is in view; while none has been seen for [SEEN_HOLD_NS] the detector is
 * searching an empty view and [SEARCH_HZ] is enough to notice a marker appear, whatever
 * the PC asks for (it commands its full rate until every target is anchored). The PC
 * can still cap lower. [accept] runs on the camera thread, [seen] on the detector
 * thread; both use the camera's sensor timestamps.
 */
class DetectGate {
    private val cap = FrameRateCap()
    @Volatile private var lastSeenNs = NEVER

    /** Detector thread: a frame with this sensor timestamp held at least one marker. */
    fun seen(sensorTimestampNs: Long) {
        lastSeenNs = sensorTimestampNs
    }

    /** Whether the search ceiling applies to a frame at this sensor timestamp. */
    fun searching(sensorTimestampNs: Long): Boolean {
        val seen = lastSeenNs
        return seen == NEVER || sensorTimestampNs - seen > SEEN_HOLD_NS
    }

    /** The rate cap in force for a frame: the PC's (0: every frame), or [SEARCH_HZ] if lower while searching. */
    fun rateHz(sensorTimestampNs: Long, commandedHz: Int): Int =
        if (!searching(sensorTimestampNs)) commandedHz
        else if (commandedHz in 1..SEARCH_HZ) commandedHz
        else SEARCH_HZ

    /** Camera thread: whether to copy this frame for the detector. */
    fun accept(sensorTimestampNs: Long, commandedHz: Int): Boolean =
        cap.accept(sensorTimestampNs, rateHz(sensorTimestampNs, commandedHz))

    companion object {
        /** Client-side detection ceiling while no marker is in view. */
        const val SEARCH_HZ = 15
        /** How long after the last sighting the PC's rate still applies, so a missed frame does not drop the rate. */
        const val SEEN_HOLD_NS = 1_000_000_000L
        private const val NEVER = Long.MIN_VALUE
    }
}
