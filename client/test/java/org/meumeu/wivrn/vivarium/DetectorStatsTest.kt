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

import org.junit.Assert.assertEquals
import org.junit.Assert.assertNull
import org.junit.Test

class DetectorStatsTest {
    @Test
    fun summarizesOncePerPeriodAndRestarts() {
        val stats = DetectorStats(periodNs = 1_000_000_000L)
        var threadCounts = 0
        val threads = { threadCounts++; 1 }
        // 14 frames of 11.2 ms CPU over the first 0.95 s: nothing yet.
        for (n in 0 until 14) assertNull(stats.add(n * 67_500_000L, 11_200_000L, if (n == 7) 6 else 3, threads))
        assertEquals(0, threadCounts)
        // The 15th frame lands at 1.0125 s: 15 frames over that is 14.8 Hz.
        assertEquals("14.8 Hz, 11.2 ms/frame, 1 thread, 6 markers max", stats.add(14 * 67_500_000L + 67_500_000L, 11_200_000L, 0, threads))
        assertEquals(1, threadCounts)
        // A new period starts from that frame's time: two frames over the next second.
        assertNull(stats.add(1_500_000_000L, 5_000_000L, 0, threads))
        assertEquals("2.0 Hz, 5.0 ms/frame, 6 threads, 1 marker max", stats.add(2_012_500_000L, 5_000_000L, 1) { 6 })
    }
}
