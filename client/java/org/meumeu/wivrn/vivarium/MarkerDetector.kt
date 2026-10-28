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

import org.opencv.core.CvType
import org.opencv.core.Mat
import org.opencv.objdetect.ArucoDetector
import org.opencv.objdetect.DetectorParameters
import org.opencv.objdetect.Objdetect

/**
 * Detects markers in the newest camera frame of [frames] on the calling thread and
 * passes one [MarkerReport] per processed frame, empty or not, to [send]. Uses
 * OpenCV's default detector parameters with subpixel corner refinement. The frames,
 * the OpenCV images that wrap them, and the report are allocated once; only
 * OpenCV's corner outputs are allocated per frame.
 */
class MarkerDetector(private val frames: LatestFrame, private val send: (MarkerReport) -> Unit) {
    fun run(running: () -> Boolean) {
        val first = frames.frames[0]
        val images = frames.frames.map { Mat(it.height, it.width, CvType.CV_8UC1, it.pixels) }
        val parameters = DetectorParameters().apply {
            set_cornerRefinementMethod(Objdetect.CORNER_REFINE_SUBPIX)
        }
        val detector = ArucoDetector(Objdetect.getPredefinedDictionary(0), parameters)
        var dictionary = 0
        val corners = ArrayList<Mat>()
        val ids = Mat()
        val id = IntArray(1)
        val xy = FloatArray(8)
        val report = MarkerReport(first.width, first.height)
        try {
            while (running()) {
                val frame = frames.take(200) ?: continue
                if (frame.dictionary != dictionary) {
                    detector.setDictionary(Objdetect.getPredefinedDictionary(frame.dictionary))
                    dictionary = frame.dictionary
                }
                val start = System.nanoTime()
                corners.forEach(Mat::release)
                corners.clear()
                detector.detectMarkers(images[frame.index], corners, ids)
                report.reset(frame, dictionary)
                for (i in corners.indices) {
                    if (ids.rows() >= ids.cols()) ids.get(i, 0, id) else ids.get(0, i, id)
                    corners[i].get(0, 0, xy)
                    report.add(id[0], xy)
                }
                report.detectUs = ((System.nanoTime() - start) / 1000).toInt()
                send(report)
            }
        } finally {
            corners.forEach(Mat::release)
            images.forEach(Mat::release)
            ids.release()
        }
    }
}
