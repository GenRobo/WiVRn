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
import org.junit.Assert.assertFalse
import org.junit.Assert.assertTrue
import org.junit.Test

class DetectGateTest {
    private val period = 16_666_667L

    private fun at(n: Int) = 5_000_000_000L + n * period

    /** Frame numbers admitted from 60 Hz timestamps; [seenAt] frames report a marker when admitted. */
    private fun admitted(commandedHz: Int, frames: Int = 120, seenAt: Set<Int> = emptySet()): List<Int> {
        val gate = DetectGate()
        return (0 until frames).filter { n ->
            gate.accept(at(n), commandedHz).also { if (it && n in seenAt) gate.seen(at(n)) }
        }
    }

    @Test
    fun searchingCapsTheCommandedRateAtFifteenHz() {
        assertEquals((0 until 120 step 4).toList(), admitted(60))
        assertEquals((0 until 120 step 4).toList(), admitted(0))
        assertEquals((0 until 120 step 4).toList(), admitted(30))
    }

    @Test
    fun lowerCommandedRatesApplyWhileSearching() {
        assertEquals((0 until 120 step 12).toList(), admitted(5))
        assertEquals((0 until 120 step 4).toList(), admitted(15))
    }

    @Test
    fun sightingReleasesTheCommandedRateUntilAHoldAfterTheLastOne() {
        // Seen on frame 4 only: every frame while within one second of it (frame 63 is the
        // last; 60 periods exceed it), then every 4th from the last admitted frame again.
        val expected = listOf(0) + (4..63).toList() + (67 until 120 step 4).toList()
        assertEquals(expected, admitted(60, seenAt = setOf(4)))
    }

    @Test
    fun repeatedSightingsKeepTheCommandedRate() {
        assertEquals((0 until 120 step 2).toList().let { listOf(0) + it.drop(2) }, admitted(30, seenAt = (4 until 120).toSet()))
    }

    @Test
    fun searchingOnlyWhileNoMarkerWasSeenRecently() {
        val gate = DetectGate()
        assertTrue(gate.searching(at(0)))
        assertEquals(DetectGate.SEARCH_HZ, gate.rateHz(at(0), 0))
        assertEquals(DetectGate.SEARCH_HZ, gate.rateHz(at(0), 60))
        assertEquals(5, gate.rateHz(at(0), 5))
        gate.seen(at(10))
        assertFalse(gate.searching(at(10)))
        assertFalse(gate.searching(at(10) + DetectGate.SEEN_HOLD_NS))
        assertTrue(gate.searching(at(10) + DetectGate.SEEN_HOLD_NS + 1))
        assertEquals(60, gate.rateHz(at(10), 60))
        assertEquals(0, gate.rateHz(at(10), 0))
    }
}
