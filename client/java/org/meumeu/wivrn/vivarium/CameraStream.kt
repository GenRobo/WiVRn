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

import android.Manifest
import android.annotation.SuppressLint
import android.content.Context
import android.content.pm.PackageManager
import android.graphics.ImageFormat
import android.graphics.Rect
import android.graphics.YuvImage
import android.hardware.camera2.CameraCaptureSession
import android.hardware.camera2.CameraCharacteristics
import android.hardware.camera2.CameraDevice
import android.hardware.camera2.CameraManager
import android.hardware.camera2.CaptureRequest
import android.hardware.camera2.CaptureResult
import android.hardware.camera2.TotalCaptureResult
import android.hardware.camera2.params.OutputConfiguration
import android.hardware.camera2.params.SessionConfiguration
import android.media.ImageReader
import android.os.Handler
import android.os.HandlerThread
import android.os.SystemClock
import android.util.Log
import java.io.ByteArrayOutputStream
import java.io.DataOutputStream
import java.io.IOException
import java.util.concurrent.ArrayBlockingQueue
import java.util.concurrent.Executor
import java.util.concurrent.TimeUnit
import kotlin.concurrent.thread
import org.opencv.android.OpenCVLoader

private const val TAG = "VivariumCamera"
private const val WIDTH = 1280
private const val HEIGHT = 960
private const val FPS = 60
private const val POSITION_LEFT = 0
private const val SOURCE_PASSTHROUGH = 0
private const val STALE_NS = 500_000_000L
private const val STATUS_PERIOD_MS = 2000L
/** The PC pings every 50 ms; after this long without a message it is gone. */
private const val LINK_TIMEOUT_MS = 500L
private val KEY_SOURCE = CameraCharacteristics.Key("com.meta.extra_metadata.camera_source", Int::class.javaObjectType)
private val KEY_POSITION = CameraCharacteristics.Key("com.meta.extra_metadata.position", Int::class.javaObjectType)

/** Camera permissions; the headset one is granted by Horizon OS v74+ only. */
val CAMERA_PERMISSIONS = arrayOf(Manifest.permission.CAMERA, "horizonos.permission.HEADSET_CAMERA")

/** Whether the app may open the headset cameras. */
fun hasCameraPermissions(context: Context): Boolean =
    CAMERA_PERMISSIONS.all { context.checkSelfPermission(it) == PackageManager.PERMISSION_GRANTED }

/** One outgoing message, built on the sending thread and handed to the channel whole. */
private class Message(capacity: Int) : ByteArrayOutputStream(capacity) {
    val out = DataOutputStream(this)

    /** The backing array; only the first [size] bytes are valid. */
    fun bytes(): ByteArray = buf
}

/**
 * Streams the left passthrough camera and the markers detected in it to the PC over the
 * `vivarium-camera` application data channel of the WiVRn session.
 *
 * The PC receiver on the other end of the channel expects CALIBRATION before anything
 * else and sends CONFIGURE as soon as it connects, then PING every 50 ms. The headset
 * cannot see the PC connect, so a CONFIGURE while unlinked marks the PC as present and
 * is answered with CALIBRATION first; [LINK_TIMEOUT_MS] without a PING marks it gone,
 * and nothing but PONG and CALIBRATION is sent while unlinked. [send] hands one message
 * to the channel; [onDepthConfig] passes the PC's depth request to the C++ depth sender
 * with the clock PONG uses (true: `CLOCK_BOOTTIME`, false: `CLOCK_MONOTONIC`).
 */
class CameraStream(
    private val context: Context,
    private val send: (ByteArray) -> Boolean,
    private val onDepthConfig: (enabled: Boolean, rateHz: Int, bootClock: Boolean) -> Unit,
) {
    @Volatile private var running = false
    private lateinit var calibration: Calibration
    private val cameraThread by lazy { HandlerThread("vivarium-camera").apply { start() } }
    private val handler by lazy { Handler(cameraThread.looper) }
    private var camera: CameraDevice? = null
    private var reader: ImageReader? = null
    // NV21 buffers whose chroma stays neutral (128): only luma is copied, so the JPEG is grayscale.
    private val freeBuffers = ArrayBlockingQueue<ByteArray>(4)
    private val encodeQueue = ArrayBlockingQueue<Pair<FrameInfo, ByteArray>>(2)
    private val pendingImages = HashMap<Long, ByteArray>()
    private val pendingResults = HashMap<Long, TotalCaptureResult>()
    companion object {
        // Capture can pause while the relay TCP session stays connected. Keep the
        // PC's last request across capture instances; only CONFIGURE replaces it.
        @Volatile private var config = StreamConfig.DEFAULT
    }
    @Volatile private var linked = false
    @Volatile private var lastPcMessageMs = 0L
    private val rateCap = FrameRateCap()
    // Detection: the camera thread copies luma into `latest.back()` and publishes it once
    // the frame's capture result is known; the detector thread takes the newest frame.
    private val latest = LatestFrame(WIDTH, HEIGHT)
    private val detectCap = FrameRateCap()
    private var detectorReady = false
    private var awaitingDetectionNs = -1L
    // Incoming bytes arrive on the WiVRn network thread; one reader and one reply buffer for it.
    private var messages = MessageReader()
    private val reply = Message(64)
    // Link status: sent every STATUS_PERIOD_MS, and at once on link or a network change.
    private val statusWake = ArrayBlockingQueue<Unit>(1)
    private lateinit var network: NetworkStatus
    private var encoder: Thread? = null
    private var detector: Thread? = null
    private var status: Thread? = null

    /** Opens the left passthrough camera; false when access is missing or there is no such camera. */
    fun start(): Boolean {
        if (running) return true
        if (!hasCameraPermissions(context)) {
            Log.w(TAG, "camera access not granted")
            return false
        }
        val manager = context.getSystemService(CameraManager::class.java)
        val id = manager.cameraIdList.firstOrNull { id ->
            val ch = manager.getCameraCharacteristics(id)
            ch.get(KEY_SOURCE) == SOURCE_PASSTHROUGH && ch.get(KEY_POSITION) == POSITION_LEFT
        }
        if (id == null) {
            Log.e(TAG, "no left passthrough camera; is passthrough enabled on Horizon OS v74+?")
            return false
        }
        val characteristics = manager.getCameraCharacteristics(id)
        calibration = calibrationOf(id, characteristics)
        repeat(4) { freeBuffers.add(ByteArray(WIDTH * HEIGHT * 3 / 2).apply { fill(128.toByte(), WIDTH * HEIGHT) }) }
        running = true
        encoder = thread(name = "vivarium-encoder") { encodeLoop() }
        detectorReady = OpenCVLoader.initLocal()
        if (detectorReady) {
            detector = thread(name = "vivarium-detector") { MarkerDetector(latest, ::sendMarkers).run { running } }
        } else {
            Log.e(TAG, "OpenCV failed to load; marker detection is off")
        }
        network = NetworkStatus(context) { statusWake.offer(Unit) }.also { it.start() }
        status = thread(name = "vivarium-status") { statusLoop() }
        openCamera(manager, id, characteristics)
        return true
    }

    fun stop() {
        if (!running) return
        running = false
        handler.post {
            camera?.close()
            reader?.close()
            cameraThread.quitSafely()
        }
        network.stop()
        statusWake.offer(Unit)
        setLinked(false)
        encoder?.join()
        detector?.join()
        status?.join()
        cameraThread.join()
    }

    private fun calibrationOf(id: String, ch: CameraCharacteristics): Calibration {
        fun floats(values: FloatArray?, count: Int) = FloatArray(count) { values?.getOrNull(it) ?: Float.NaN }
        val array = ch.get(CameraCharacteristics.SENSOR_INFO_PRE_CORRECTION_ACTIVE_ARRAY_SIZE) ?: Rect(0, 0, WIDTH, HEIGHT)
        return Calibration(
            width = WIDTH,
            height = HEIGHT,
            position = ch.get(KEY_POSITION) ?: -1,
            source = ch.get(KEY_SOURCE) ?: -1,
            timestampSource = ch.get(CameraCharacteristics.SENSOR_INFO_TIMESTAMP_SOURCE) ?: 0,
            lensPoseReference = ch.get(CameraCharacteristics.LENS_POSE_REFERENCE) ?: 255,
            codec = StreamFormat.CODEC_JPEG,
            activeArray = intArrayOf(array.left, array.top, array.width(), array.height()),
            intrinsics = floats(ch.get(CameraCharacteristics.LENS_INTRINSIC_CALIBRATION), 5),
            distortion = floats(ch.get(CameraCharacteristics.LENS_DISTORTION), 5),
            poseTranslation = floats(ch.get(CameraCharacteristics.LENS_POSE_TRANSLATION), 3),
            poseRotation = floats(ch.get(CameraCharacteristics.LENS_POSE_ROTATION), 4),
            cameraId = id,
        )
    }

    private fun bootClock() = calibration.timestampSource == CameraCharacteristics.SENSOR_INFO_TIMESTAMP_SOURCE_REALTIME

    /** Headset clock that SENSOR_TIMESTAMP uses, for clock-sync replies. */
    private fun clockNs(): Long = if (bootClock()) SystemClock.elapsedRealtimeNanos() else System.nanoTime()

    @SuppressLint("MissingPermission") // start() checks the permissions first.
    private fun openCamera(manager: CameraManager, id: String, ch: CameraCharacteristics) {
        val fpsRange = ch.get(CameraCharacteristics.CONTROL_AE_AVAILABLE_TARGET_FPS_RANGES)
            ?.firstOrNull { it.lower == FPS && it.upper == FPS }
        manager.openCamera(id, object : CameraDevice.StateCallback() {
            override fun onOpened(device: CameraDevice) {
                if (!running) {
                    device.close()
                    return
                }
                camera = device
                val imageReader = ImageReader.newInstance(WIDTH, HEIGHT, ImageFormat.YUV_420_888, 4)
                imageReader.setOnImageAvailableListener(::onImage, handler)
                reader = imageReader
                val request = device.createCaptureRequest(CameraDevice.TEMPLATE_PREVIEW).apply {
                    addTarget(imageReader.surface)
                    fpsRange?.let { set(CaptureRequest.CONTROL_AE_TARGET_FPS_RANGE, it) }
                }.build()
                val config = SessionConfiguration(
                    SessionConfiguration.SESSION_REGULAR,
                    listOf(OutputConfiguration(imageReader.surface)),
                    Executor { handler.post(it) },
                    object : CameraCaptureSession.StateCallback() {
                        override fun onConfigured(session: CameraCaptureSession) {
                            if (!running) return
                            session.setRepeatingRequest(request, captureCallback, handler)
                            Log.i(TAG, "streaming camera $id ${WIDTH}x$HEIGHT, fps range $fpsRange")
                        }

                        override fun onConfigureFailed(session: CameraCaptureSession) {
                            Log.e(TAG, "capture session configuration failed")
                        }
                    },
                )
                device.createCaptureSession(config)
            }

            override fun onDisconnected(device: CameraDevice) {
                Log.w(TAG, "camera disconnected")
                device.close()
            }

            override fun onError(device: CameraDevice, error: Int) {
                Log.e(TAG, "camera error $error")
                device.close()
            }
        }, handler)
    }

    private val captureCallback = object : CameraCaptureSession.CaptureCallback() {
        override fun onCaptureCompleted(s: CameraCaptureSession, r: CaptureRequest, result: TotalCaptureResult) {
            val timestamp = result.get(CaptureResult.SENSOR_TIMESTAMP) ?: return
            pendingResults[timestamp] = result
            if (timestamp == awaitingDetectionNs) publishDetection(result)
            pendingResults.keys.removeAll { it < timestamp - STALE_NS }
            pendingImages.entries.removeAll { (key, stale) -> (key < timestamp - STALE_NS).also { if (it) freeBuffers.offer(stale) } }
            pair(timestamp)
        }
    }

    private fun onImage(imageReader: ImageReader) {
        val image = imageReader.acquireNextImage() ?: return
        image.use {
            if (!linked) return
            val current = config
            val dictionary = current.detectDictionary
            if (detectorReady && dictionary != null && detectCap.accept(it.timestamp, current.detectRateHz)) {
                val plane = it.planes[0]
                latest.back().apply {
                    copyLuma(plane.buffer, plane.rowStride)
                    sensorTimestampNs = it.timestamp
                    this.dictionary = dictionary
                }
                awaitingDetectionNs = it.timestamp
                pendingResults[it.timestamp]?.let { result -> publishDetection(result) }
            }
            // Drop frames over the PC's rate cap before copying and encoding them.
            if (!current.images || !rateCap.accept(it.timestamp, current.maxRateHz)) return
            val buffer = freeBuffers.poll()
            if (buffer == null) {
                Log.w(TAG, "encoder behind; frame dropped")
                return
            }
            val plane = it.planes[0]
            val luma = plane.buffer
            for (row in 0 until HEIGHT) {
                luma.position(row * plane.rowStride)
                luma.get(buffer, row * WIDTH, WIDTH)
            }
            pendingImages[it.timestamp] = buffer
            pair(it.timestamp)
        }
    }

    /** Joins an image with its capture result; both arrive on the camera thread in either order. */
    private fun pair(timestamp: Long) {
        val result = pendingResults[timestamp] ?: return
        val buffer = pendingImages.remove(timestamp) ?: return
        pendingResults.remove(timestamp)
        val info = FrameInfo(
            result.frameNumber,
            timestamp,
            result.get(CaptureResult.SENSOR_EXPOSURE_TIME) ?: -1,
            result.get(CaptureResult.SENSOR_ROLLING_SHUTTER_SKEW) ?: -1,
            0,
        )
        if (!encodeQueue.offer(info to buffer)) freeBuffers.offer(buffer)
    }

    /** Hands the copied luma in `latest.back()` to the detector with its frame's capture result. */
    private fun publishDetection(result: TotalCaptureResult) {
        awaitingDetectionNs = -1L
        latest.back().setCapture(
            result.frameNumber,
            result.get(CaptureResult.SENSOR_EXPOSURE_TIME) ?: -1,
            result.get(CaptureResult.SENSOR_ROLLING_SHUTTER_SKEW) ?: -1,
        )
        latest.publish()
    }

    private val markersMessage = Message(4096)

    /** Detector thread: sends one frame's markers; dropped while the PC is not there. */
    private fun sendMarkers(report: MarkerReport) {
        if (!linked) return
        markersMessage.reset()
        StreamFormat.writeMarkers(markersMessage.out, report)
        send(markersMessage.toByteArray())
    }

    private fun encodeLoop() {
        val jpeg = Message(512 * 1024)
        val frame = Message(512 * 1024)
        val area = Rect(0, 0, WIDTH, HEIGHT)
        while (running) {
            val (info, nv21) = encodeQueue.poll(200, TimeUnit.MILLISECONDS) ?: continue
            jpeg.reset()
            try {
                YuvImage(nv21, ImageFormat.NV21, WIDTH, HEIGHT, null).compressToJpeg(area, config.jpegQuality, jpeg)
            } finally {
                freeBuffers.offer(nv21)
            }
            if (!linked) continue
            frame.reset()
            StreamFormat.writeFrame(frame.out, info.copy(sendTimeNs = clockNs()), jpeg.bytes(), jpeg.size())
            send(frame.toByteArray())
        }
    }

    /**
     * Network thread: bytes from the PC. Answers pings, applies configure messages and
     * sends the calibration when the PC appears.
     */
    fun receive(chunk: ByteArray) {
        if (!running) return
        try {
            messages.push(chunk, ::handle)
        } catch (e: IOException) {
            Log.e(TAG, "bad bytes from the PC: ${e.message}")
            messages = MessageReader()
            setLinked(false)
        }
    }

    private fun handle(type: Int, body: ByteArray) {
        lastPcMessageMs = SystemClock.elapsedRealtime()
        when (type) {
            StreamFormat.TYPE_PING -> {
                if (!linked) {
                    reply.reset()
                    StreamFormat.writeCalibration(reply.out, calibration)
                    send(reply.toByteArray())
                    setLinked(true)
                }
                val ping = StreamFormat.parsePing(body)
                reply.reset()
                StreamFormat.writePong(reply.out, ping, clockNs())
                send(reply.toByteArray())
            }
            StreamFormat.TYPE_CONFIGURE -> {
                val parsed = StreamFormat.parseConfigure(body)
                // Relay sinks may reconnect while the headset link remains alive.
                // Refresh metadata for every configure; the receiver only answers
                // its first calibration with CONFIGURE, so there is no echo loop.
                reply.reset()
                StreamFormat.writeCalibration(reply.out, calibration)
                send(reply.toByteArray())
                setLinked(true)
                config = parsed
                onDepthConfig(parsed.depth, parsed.depthRateHz, bootClock())
                Log.i(TAG, "configured: $parsed")
            }
            else -> throw IOException("unexpected message type $type")
        }
    }

    private fun setLinked(value: Boolean) {
        if (linked == value) return
        linked = value
        if (value) {
            Log.i(TAG, "PC receiver present")
            onDepthConfig(config.depth, config.depthRateHz, bootClock())
        } else {
            Log.i(TAG, "PC receiver gone")
            onDepthConfig(false, 0, false)
        }
        statusWake.offer(Unit)
    }

    /** Sends the headset's network status to the PC while linked; logs each change. */
    private fun statusLoop() {
        val message = Message(64)
        var lastKey: List<Any?>? = null
        var nextStatusMs = 0L
        while (running) {
            val woken = statusWake.poll(LINK_TIMEOUT_MS / 2, TimeUnit.MILLISECONDS) != null
            if (!linked) continue
            val now = SystemClock.elapsedRealtime()
            if (now - lastPcMessageMs > LINK_TIMEOUT_MS) {
                setLinked(false)
                continue
            }
            if (now < nextStatusMs && !woken) continue
            nextStatusMs = now + STATUS_PERIOD_MS
            val status = network.read(clockNs())
            if (status.key() != lastKey) {
                lastKey = status.key()
                Log.i(TAG, "link: $status")
            }
            message.reset()
            StreamFormat.writeLinkStatus(message.out, status)
            send(message.toByteArray())
        }
    }
}
