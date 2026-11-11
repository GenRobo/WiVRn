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

import android.os.Debug
import android.util.Log
import org.opencv.core.Core
import org.opencv.core.CvType
import org.opencv.core.Mat
import org.opencv.core.Size
import org.opencv.core.TermCriteria
import org.opencv.imgproc.Imgproc
import org.opencv.objdetect.ArucoDetector
import org.opencv.objdetect.DetectorParameters
import org.opencv.objdetect.Objdetect
import java.io.File

private const val TAG = "VivariumMarkers"

/**
 * Detects markers in the newest camera frame of [frames] on the calling thread and
 * passes one [MarkerReport] per processed frame, empty or not, to [send]. The thread
 * is the only one: OpenCV's worker pool is disabled before the detector is built, so
 * the cost is one core at most and scales with the rate the camera thread admits.
 * Candidates are searched on the [DetectScale] copy of the frame and their corners
 * refined to subpixel precision on the full frame, with OpenCV's own refinement
 * window and criteria. The frames, the OpenCV images that wrap them, the scaled copy
 * and the report are allocated once; only OpenCV's corner outputs are allocated per
 * frame. Once a minute the measured rate, CPU per frame and thread count are logged.
 */
class MarkerDetector(
    private val frames: LatestFrame,
    private val scale: DetectScale,
    private val send: (MarkerReport) -> Unit,
) {
    fun run(running: () -> Boolean) {
        Core.setNumThreads(1)
        val first = frames.frames[0]
        val images = frames.frames.map { Mat(it.height, it.width, CvType.CV_8UC1, it.pixels) }
        val small = Mat(scale.detectHeight, scale.detectWidth, CvType.CV_8UC1)
        val smallSize = Size(scale.detectWidth.toDouble(), scale.detectHeight.toDouble())
        val parameters = DetectorParameters().apply {
            set_cornerRefinementMethod(Objdetect.CORNER_REFINE_NONE)
        }
        val borderBits = parameters.get_markerBorderBits()
        val maxWindow = parameters.get_cornerRefinementWinSize()
        val relativeWindow = parameters.get_relativeCornerRefinmentWinSize().toFloat()
        val criteria = TermCriteria(
            TermCriteria.MAX_ITER + TermCriteria.EPS,
            parameters.get_cornerRefinementMaxIterations(),
            parameters.get_cornerRefinementMinAccuracy(),
        )
        val window = Size()
        val zeroZone = Size(-1.0, -1.0)
        val detector = ArucoDetector(Objdetect.getPredefinedDictionary(0), parameters)
        var dictionary = 0
        var markerSize = detector.dictionary.get_markerSize()
        val corners = ArrayList<Mat>()
        val ids = Mat()
        val id = IntArray(1)
        val xy = FloatArray(8)
        val report = MarkerReport(first.width, first.height)
        val stats = DetectorStats()
        val threadName = Thread.currentThread().name
        Log.i(TAG, "detecting on ${scale.detectWidth}x${scale.detectHeight} of ${first.width}x${first.height}, OpenCV threads ${Core.getNumThreads()}")
        try {
            while (running()) {
                val frame = frames.take(200) ?: continue
                if (frame.dictionary != dictionary) {
                    detector.dictionary = Objdetect.getPredefinedDictionary(frame.dictionary)
                    dictionary = frame.dictionary
                    markerSize = detector.dictionary.get_markerSize()
                }
                val start = System.nanoTime()
                val cpuStart = Debug.threadCpuTimeNanos()
                corners.forEach(Mat::release)
                corners.clear()
                val image = images[frame.index]
                val input = if (scale.scaled) {
                    Imgproc.resize(image, small, smallSize, 0.0, 0.0, Imgproc.INTER_AREA)
                    small
                } else {
                    image
                }
                detector.detectMarkers(input, corners, ids)
                report.reset(frame, dictionary)
                for (i in corners.indices) {
                    if (ids.rows() >= ids.cols()) ids.get(i, 0, id) else ids.get(0, i, id)
                    val quad = corners[i]
                    quad.get(0, 0, xy)
                    scale.toFrame(xy)
                    val side = DetectScale.refineWindow(xy, markerSize, borderBits, relativeWindow, maxWindow).toDouble()
                    window.width = side
                    window.height = side
                    quad.put(0, 0, xy)
                    Imgproc.cornerSubPix(image, quad, window, zeroZone, criteria)
                    quad.get(0, 0, xy)
                    report.add(id[0], xy)
                }
                val end = System.nanoTime()
                report.detectUs = ((end - start) / 1000).toInt()
                send(report)
                stats.add(end, Debug.threadCpuTimeNanos() - cpuStart, report.count) { threadsNamed(threadName) }?.let { Log.i(TAG, it) }
            }
        } finally {
            corners.forEach(Mat::release)
            images.forEach(Mat::release)
            small.release()
            ids.release()
        }
    }

    /** Threads of this process named [name]: the detector and any OpenCV workers it spawned. */
    private fun threadsNamed(name: String): Int = try {
        File("/proc/self/task").listFiles()?.count { File(it, "comm").readText().trim() == name } ?: Core.getNumThreads()
    } catch (e: Exception) {
        Core.getNumThreads()
    }
}
