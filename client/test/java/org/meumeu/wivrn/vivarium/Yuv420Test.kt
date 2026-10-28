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
import org.junit.Test

class Yuv420Test {
    @Test
    fun planarChromaUsesVuOrderAndSkipsIndependentRowPadding() {
        val y = ByteBuffer.wrap(byteArrayOf(
            99, 99, 1, 2, 3, 4, 99, 99, 5, 6, 7, 8,
            99, 99, 9, 10, 11, 12, 99, 99, 13, 14, 15, 16, 99,
        )).apply { position(2); limit(24) }
        val u = ByteBuffer.wrap(byteArrayOf(99, 21, 22, 99, 23, 24, 99))
            .apply { position(1); limit(6) }
        val v = ByteBuffer.wrap(byteArrayOf(99, 99, -31, -32, 99, 99, -33, -34, 99))
            .apply { position(2); limit(8) }
        val nv21 = ByteArray(24) { 128.toByte() }

        copyYuv420ToNv21(4, 4, y, 6, 1, u, 3, 1, v, 4, 1, nv21)

        assertArrayEquals(byteArrayOf(
            1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16,
            -31, 21, -32, 22, -33, 23, -34, 24,
        ), nv21)
        assertEquals(2, y.position())
        assertEquals(24, y.limit())
        assertEquals(1, u.position())
        assertEquals(6, u.limit())
        assertEquals(2, v.position())
        assertEquals(8, v.limit())
    }

    @Test
    fun sharedInterleavedChromaRespectsOffsetsAndTightLastRowLimits() {
        val y = ByteBuffer.wrap(ByteArray(16) { (it + 1).toByte() })
        val chroma = ByteBuffer.allocateDirect(12).apply {
            put(byteArrayOf(99, -31, 21, -32, 22, 99, 99, -33, 23, -34, 24, 99))
            flip()
        }
        // U and V alias the same storage, but each view ends at its last sample.
        val u = chroma.asReadOnlyBuffer().apply { position(2); limit(11) }
        val v = chroma.asReadOnlyBuffer().apply { position(1); limit(10) }
        val nv21 = ByteArray(24)

        copyYuv420ToNv21(4, 4, y, 4, 1, u, 6, 2, v, 6, 2, nv21)

        assertArrayEquals(byteArrayOf(
            1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16,
            -31, 21, -32, 22, -33, 23, -34, 24,
        ), nv21)
        assertEquals(0, y.position())
        assertEquals(16, y.limit())
        assertEquals(2, u.position())
        assertEquals(11, u.limit())
        assertEquals(1, v.position())
        assertEquals(10, v.limit())
    }

    @Test
    fun pixelStridesDoNotLeakPaddingIntoTheImage() {
        val y = ByteBuffer.wrap(byteArrayOf(
            99, 1, 99, 2, 99, 3, 99, 4, 99,
            99, 5, 99, 6, 99, 7, 99, 8, 99,
        )).apply { position(1); limit(17) }
        val u = ByteBuffer.wrap(byteArrayOf(51, 99, 52))
        val v = ByteBuffer.wrap(byteArrayOf(61, 99, 62))
        val nv21 = ByteArray(12)

        copyYuv420ToNv21(4, 2, y, 9, 2, u, 4, 2, v, 4, 2, nv21)

        assertArrayEquals(byteArrayOf(1, 2, 3, 4, 5, 6, 7, 8, 61, 51, 62, 52), nv21)
        assertEquals(1, y.position())
        assertEquals(17, y.limit())
    }

    @Test
    fun reusingPooledOutputReplacesChromaFromThePreviousFrame() {
        val y = ByteBuffer.wrap(byteArrayOf(1, 2, 3, 4))
        val u = ByteBuffer.wrap(byteArrayOf(41))
        val v = ByteBuffer.wrap(byteArrayOf(91))
        val nv21 = ByteArray(6) { 128.toByte() }
        copyYuv420ToNv21(2, 2, y, 2, 1, u, 1, 1, v, 1, 1, nv21)
        assertArrayEquals(byteArrayOf(1, 2, 3, 4, 91, 41), nv21)

        u.put(0, 73)
        v.put(0, 23)
        copyYuv420ToNv21(2, 2, y, 2, 1, u, 1, 1, v, 1, 1, nv21)
        assertArrayEquals(byteArrayOf(1, 2, 3, 4, 23, 73), nv21)
    }
}
