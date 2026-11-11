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

    @Test
    fun copyPlaneBulkPathSkipsRowPaddingOnBothSidesAndRestoresBuffers() {
        val src = ByteBuffer.wrap(byteArrayOf(
            99, 99, 1, 2, 3, 4, 99, 99, 5, 6, 7, 8, 99, 99, 9, 10, 11, 12,
        )).apply { position(2); limit(18) }
        val dst = ByteBuffer.wrap(ByteArray(19) { 77 }).apply { position(1); limit(18) }

        copyPlane(4, 3, src, 6, 1, dst, 6, 1)

        assertArrayEquals(byteArrayOf(
            77, 1, 2, 3, 4, 77, 77, 5, 6, 7, 8, 77, 77, 9, 10, 11, 12, 77, 77,
        ), dst.array())
        assertEquals(2, src.position())
        assertEquals(18, src.limit())
        assertEquals(1, dst.position())
        assertEquals(18, dst.limit())
    }

    @Test
    fun copyPlanePlanarToInterleavedWritesEveryOtherByte() {
        val u = ByteBuffer.wrap(byteArrayOf(21, 22, 99, 23, 24, 99)).apply { limit(5) }
        val v = ByteBuffer.wrap(byteArrayOf(31, 32, 99, 33, 34))
        // Semi-planar destination: U at even and V at odd offsets of one buffer, padded rows.
        val chroma = ByteBuffer.wrap(ByteArray(12) { 77 })
        val dstU = chroma.duplicate().apply { position(0) }
        val dstV = chroma.duplicate().apply { position(1) }

        copyPlane(2, 2, u, 3, 1, dstU, 6, 2)
        copyPlane(2, 2, v, 3, 1, dstV, 6, 2)

        assertArrayEquals(byteArrayOf(21, 31, 22, 32, 77, 77, 23, 33, 24, 34, 77, 77), chroma.array())
        assertEquals(0, dstU.position())
        assertEquals(1, dstV.position())
        assertEquals(0, u.position())
        assertEquals(5, u.limit())
    }

    @Test
    fun interleavedToInterleavedOfAnotherOrderGoesSampleBySample() {
        // Source NV12 rows (U V U V, row stride 5), destination NV21 (V U V U, row stride 4).
        val src = ByteBuffer.wrap(byteArrayOf(1, -1, 2, -2, 99, 3, -3, 4, -4, 99))
        val srcU = src.duplicate().apply { position(0); limit(10) }
        val srcV = src.duplicate().apply { position(1); limit(10) }
        val dst = ByteBuffer.wrap(ByteArray(8) { 77 })
        val dstV = dst.duplicate().apply { position(0) }
        val dstU = dst.duplicate().apply { position(1) }
        assertEquals(1, interleavedOrder(srcU, 2, srcV, 2))
        assertEquals(2, probeInterleavedOrder(dstU, 2, dstV, 2))
        assertArrayEquals(ByteArray(8) { 77 }, dst.array()) // the probe restores what it touched
        assertEquals(0, interleavedOrder(srcU, 1, srcV, 1))
        // A uniform region satisfies both relations: U first is reported, either order copies it.
        val flat = ByteBuffer.wrap(ByteArray(10) { 5 })
        assertEquals(1, interleavedOrder(flat.duplicate(), 2, flat.duplicate().apply { position(1) }, 2))
        // Planes that do not alias each other are not interleaved, whatever their strides say.
        val apart = ByteBuffer.wrap(byteArrayOf(1, 2, 3, 4, 5, 6, 7, 8))
        assertEquals(0, interleavedOrder(apart.duplicate(), 2, apart.duplicate().apply { position(4) }, 2))

        copyPlane(2, 2, srcU, 5, 2, dstU, 4, 2)
        copyPlane(2, 2, srcV, 5, 2, dstV, 4, 2)

        assertArrayEquals(byteArrayOf(-1, 1, -2, 2, -3, 3, -4, 4), dst.array())
    }

    @Test
    fun sameOrderInterleavedChromaMovesAsOverlappingRunsWithinEachPlane() {
        // NV12 on both sides, two chroma rows of two pairs, row stride 5 (source) and 4
        // (destination); Android ends each plane's buffer at that plane's last sample, so the
        // runs are `width - 1` bytes and the last of each row comes from the other plane.
        val src = ByteBuffer.wrap(byteArrayOf(1, -1, 2, -2, 99, 3, -3, 4, -4))
        val srcU = src.duplicate().apply { position(0); limit(8) }
        val srcV = src.duplicate().apply { position(1); limit(9) }
        val dst = ByteBuffer.wrap(ByteArray(8) { 77 })
        val dstU = dst.duplicate().apply { position(0); limit(7) }
        val dstV = dst.duplicate().apply { position(1); limit(8) }
        assertEquals(1, interleavedOrder(srcU, 2, srcV, 2))
        assertEquals(1, probeInterleavedOrder(dstU, 2, dstV, 2))

        copyPlane(4 - 1, 2, srcU, 5, 1, dstU, 4, 1)
        copyPlane(4 - 1, 2, srcV, 5, 1, dstV, 4, 1)

        assertArrayEquals(byteArrayOf(1, -1, 2, -2, 3, -3, 4, -4), dst.array())
    }

    @Test
    fun copyPlaneInterleavedToPlanarDropsTheOtherChannel() {
        val src = ByteBuffer.wrap(byteArrayOf(99, 1, 88, 2, 88, 0, 3, 88, 4)).apply { position(1); limit(9) }
        val dst = ByteBuffer.wrap(ByteArray(6) { 77 }).apply { position(1) }

        copyPlane(2, 2, src, 5, 2, dst, 2, 1)

        assertArrayEquals(byteArrayOf(77, 1, 2, 3, 4, 77), dst.array())
        assertEquals(1, src.position())
        assertEquals(1, dst.position())
    }
}
