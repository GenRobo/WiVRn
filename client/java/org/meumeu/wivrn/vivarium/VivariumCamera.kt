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
 * Sensor streaming as the C++ client drives it (client/vivarium/sensors.cpp):
 * created once per session with any sensor enabled, [start]ed with the local settings
 * and [stop]ped when the session ends. Only image capture needs camera permissions.
 * Messages go out through [MainActivity.sendAppData]; the PC's bytes come back through
 * the channel listener; the PC's depth request goes to the C++ depth sender.
 */
class VivariumCamera(private val activity: MainActivity) {
    @Volatile private var stream: CameraStream? = null
    private val listener = MainActivity.AppDataListener { _, data -> stream?.receive(data) }

    /** Whether the camera permissions are granted; C++ requests them when not. */
    fun hasPermissions(): Boolean = hasCameraPermissions(activity)

    /** Starts the enabled sensors; false when required camera metadata or capture is unavailable. */
    fun start(markerTracking: Boolean, rgbCameraStreaming: Boolean, depthCameraStreaming: Boolean): Boolean {
        if (stream != null) return true
        val s = CameraStream(
            activity,
            { bytes -> activity.sendAppData(StreamFormat.CHANNEL, bytes) },
            ::configureDepth,
            markerTracking,
            rgbCameraStreaming,
            depthCameraStreaming,
        )
        try {
            if (!s.start()) return false
        } catch (error: Exception) {
            android.util.Log.e("SensorStreaming", "sensor startup failed", error)
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
        configureDepth(false, 0, false)
    }

    /** The PC's depth request, applied by the C++ depth sender on the render thread. */
    private external fun configureDepth(enabled: Boolean, rateHz: Int, bootClock: Boolean)
}
