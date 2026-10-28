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

class FrameRateCapTest {
    /** Frame numbers sent from 60 Hz timestamps with ±1 ms jitter. */
    private fun sent(maxRateHz: Int, frames: Int = 24): List<Int> {
        val cap = FrameRateCap()
        return (0 until frames).filter { n ->
            val jitter = if (n % 2 == 0) 1_000_000L else -1_000_000L
            cap.accept(5_000_000_000L + n * 16_666_667L + jitter, maxRateHz)
        }
    }

    @Test
    fun lowRateSendsEveryFourthFrame() {
        assertEquals((0 until 24 step 4).toList(), sent(15))
    }

    @Test
    fun halfRateSendsEverySecondFrame() {
        assertEquals((0 until 24 step 2).toList(), sent(30))
    }

    @Test
    fun fullRateAndNoCapSendEveryFrame() {
        assertEquals((0 until 24).toList(), sent(60))
        assertEquals((0 until 24).toList(), sent(0))
    }
}
