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
import org.junit.Assert.assertArrayEquals
import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertNull
import org.junit.Assert.assertSame
import org.junit.Assert.assertTrue
import org.junit.Test

class LatestFrameTest {
    private fun fill(latest: LatestFrame, frameNumber: Long): Boolean {
        latest.back().frameNumber = frameNumber
        return latest.publish()
    }

    @Test
    fun detectorGetsTheNewestFrame() {
        val latest = LatestFrame(4, 2)
        assertFalse(fill(latest, 1))
        assertTrue(fill(latest, 2)) // frame 1 was not taken: replaced
        assertEquals(2L, latest.take(0)?.frameNumber)
        assertNull(latest.take(0))
    }

    @Test
    fun cameraNeverWritesTheFrameTheDetectorHolds() {
        val latest = LatestFrame(4, 2)
        fill(latest, 1)
        val held = latest.take(0)!!
        repeat(5) {
            assertTrue(latest.back() !== held)
            fill(latest, 10L + it)
        }
        assertEquals(1L, held.frameNumber)
        assertEquals(14L, latest.take(0)?.frameNumber)
        assertEquals(setOf(0, 1, 2), latest.frames.map { it.index }.toSet())
    }

    @Test
    fun takeWaitsForAPublish() {
        val latest = LatestFrame(4, 2)
        val publisher = Thread {
            Thread.sleep(20)
            fill(latest, 7)
        }.apply { start() }
        val frame = latest.take(5_000)
        publisher.join()
        assertEquals(7L, frame?.frameNumber)
        assertSame(frame, latest.frames[frame!!.index])
    }

    @Test
    fun copyLumaSkipsRowPadding() {
        val frame = LumaFrame(0, 3, 2)
        // Row stride 5; the last row ends at the image width, as in Android image planes.
        val plane = ByteBuffer.wrap(byteArrayOf(1, 2, 3, 0, 0, 4, 5, 6))
        frame.copyLuma(plane, rowStride = 5)
        val copied = ByteArray(6).also { frame.pixels.rewind(); frame.pixels.get(it) }
        assertArrayEquals(byteArrayOf(1, 2, 3, 4, 5, 6), copied)
        assertEquals(0, plane.position())
    }
}
