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
import org.junit.Test

class StreamSizeTest {
    private val capture = StreamSize(1280, 960)
    private val supported = listOf(StreamSize(320, 240), StreamSize(640, 480), StreamSize(800, 600), capture)

    @Test
    fun anOfferedSizeIsStreamed() {
        assertEquals(StreamSize(640, 480), chooseStreamSize(StreamSize(640, 480), supported, capture))
    }

    @Test
    fun theCaptureSizeNeedsNoSecondOutput() {
        assertEquals(capture, chooseStreamSize(capture, emptyList(), capture))
    }

    @Test
    fun anUnofferedSizeFallsBackToTheCapture() {
        assertEquals(capture, chooseStreamSize(StreamSize(1920, 1080), supported, capture))
        assertEquals(capture, chooseStreamSize(StreamSize(480, 640), supported, capture))
        assertEquals(capture, chooseStreamSize(StreamSize(0, 0), supported, capture))
    }
}
