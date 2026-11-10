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

import java.util.Locale

/**
 * The marker detector's measured rate and CPU cost, summarized once per [periodNs] as
 * `14.8 Hz, 11.2 ms/frame, 1 thread`: frames processed over the period's wall time, the
 * detector thread's CPU time per frame, and how many threads carry the detector's name
 * (OpenCV's worker pool inherits it, so more than one means the pool is still in use).
 */
class DetectorStats(private val periodNs: Long = 60_000_000_000L) {
    private var periodStartNs = Long.MIN_VALUE
    private var frames = 0
    private var cpuNs = 0L

    /**
     * Records a frame that took [frameCpuNs] of CPU, finished at [nowNs]; returns the
     * summary line when a period has elapsed, counting [threads] at that moment, else null.
     */
    fun add(nowNs: Long, frameCpuNs: Long, threads: () -> Int): String? {
        if (periodStartNs == Long.MIN_VALUE) periodStartNs = nowNs
        frames++
        cpuNs += frameCpuNs
        val elapsedNs = nowNs - periodStartNs
        if (elapsedNs < periodNs) return null
        val count = threads()
        val line = String.format(
            Locale.US, "%.1f Hz, %.1f ms/frame, %d thread%s",
            frames * 1e9 / elapsedNs, cpuNs / 1e6 / frames, count, if (count == 1) "" else "s",
        )
        periodStartNs = nowNs
        frames = 0
        cpuNs = 0
        return line
    }
}
