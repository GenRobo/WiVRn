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

import org.junit.Assert.assertArrayEquals
import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertTrue
import org.junit.Test

class DetectScaleTest {
    @Test
    fun wideFramesAreSearchedAtHalfSize() {
        val scale = DetectScale(1280, 960, DetectScale.DEFAULT)
        assertTrue(scale.scaled)
        assertEquals(640, scale.detectWidth)
        assertEquals(480, scale.detectHeight)
    }

    @Test
    fun framesUpToSixHundredFortyWideAreSearchedAsIs() {
        val scale = DetectScale(640, 480, DetectScale.DEFAULT)
        assertFalse(scale.scaled)
        assertEquals(640, scale.detectWidth)
        assertEquals(480, scale.detectHeight)
        val xy = floatArrayOf(10f, 20f, 30f, 40f)
        scale.toFrame(xy)
        assertArrayEquals(floatArrayOf(10f, 20f, 30f, 40f), xy, 0f)
    }

    @Test
    fun scaleIsClampedAndOneMeansFullSize() {
        assertEquals(0.25f, DetectScale(1280, 960, 0.01f).scale, 0f)
        assertEquals(1f, DetectScale(1280, 960, 7f).scale, 0f)
        assertFalse(DetectScale(1280, 960, 1f).scaled)
        assertEquals(320, DetectScale(1280, 960, 0.25f).detectWidth)
    }

    @Test
    fun cornersMapBackToFramePixelCentres() {
        val scale = DetectScale(1280, 960, 0.5f)
        // Detection pixel 0 covers frame pixels 0 and 1; pixel 639 covers 1278 and 1279.
        val xy = floatArrayOf(0f, 0f, 639f, 479f, 100.25f, 50.5f, 7f, 7f)
        scale.toFrame(xy, 3)
        assertArrayEquals(floatArrayOf(0.5f, 0.5f, 1278.5f, 958.5f, 201f, 101.5f, 7f, 7f), xy, 1e-4f)
    }

    @Test
    fun oddSizesUseTheirActualRatio() {
        val scale = DetectScale(1001, 751, 0.5f)
        assertEquals(501, scale.detectWidth)
        assertEquals(376, scale.detectHeight)
        val xy = floatArrayOf(500f, 375f)
        scale.toFrame(xy)
        assertEquals((500.5f * 1001 / 501) - 0.5f, xy[0], 1e-3f)
        assertEquals((375.5f * 751 / 376) - 0.5f, xy[1], 1e-3f)
    }

    /** A square marker of [side] pixels. */
    private fun square(side: Float) = floatArrayOf(100f, 100f, 100f + side, 100f, 100f + side, 100f + side, 100f, 100f + side)

    @Test
    fun refinementWindowFollowsOpenCvsRule() {
        // 4x4 dictionary: 6 modules a side. A 40 px marker has 6.7 px modules; 0.3 of one is 2 px.
        assertEquals(2, DetectScale.refineWindow(square(40f), 4))
        // Large markers are capped at OpenCV's 5 px window.
        assertEquals(5, DetectScale.refineWindow(square(200f), 4))
        // Tiny markers still get a 1 px window.
        assertEquals(1, DetectScale.refineWindow(square(6f), 4))
        // A 6x6 dictionary has more modules per side, so a smaller window for the same size.
        assertEquals(2, DetectScale.refineWindow(square(60f), 6))
        assertEquals(3, DetectScale.refineWindow(square(60f), 4))
    }
}
