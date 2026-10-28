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

import org.meumeu.wivrn.MainActivity

/**
 * The Vivarium camera module as the C++ client drives it (client/vivarium/sensors.cpp):
 * created once per stream session when the "Vivarium camera" setting is on, [start]ed
 * once the camera permissions are granted and [stop]ped when the session ends. Camera
 * messages go out through [MainActivity.sendAppData]; the PC's bytes come back through
 * the channel listener; the PC's depth request goes to the C++ depth sender.
 */
class VivariumCamera(private val activity: MainActivity) {
    @Volatile private var stream: CameraStream? = null
    private val listener = MainActivity.AppDataListener { _, data -> stream?.receive(data) }

    /** Whether the camera permissions are granted; C++ requests them when not. */
    fun hasPermissions(): Boolean = hasCameraPermissions(activity)

    /** Starts streaming; false when the camera cannot be opened (see the log). */
    fun start(): Boolean {
        if (stream != null) return true
        val s = CameraStream(activity, { bytes -> activity.sendAppData(StreamFormat.CHANNEL, bytes) })
        try {
            if (!s.start()) return false
        } catch (error: Exception) {
            android.util.Log.e("VivariumCamera", "capture startup failed", error)
            s.stop()
            return false
        }
        stream = s
        activity.setAppDataListener(StreamFormat.CHANNEL, listener)
        return true
    }

    fun stop() {
        activity.setAppDataListener(StreamFormat.CHANNEL, null)
        stream?.stop()
        stream = null
    }

}
