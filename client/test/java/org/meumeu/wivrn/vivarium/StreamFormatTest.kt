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

import java.io.ByteArrayOutputStream
import java.io.DataInputStream
import java.io.DataOutputStream
import java.io.File
import java.io.IOException
import org.junit.Assert.assertArrayEquals
import org.junit.Assert.assertEquals
import org.junit.Assert.assertNotEquals
import org.junit.Test

/** Pins the encoder to the golden files that the Python receiver tests also read. */
class StreamFormatTest {
    private val goldens = File(System.getProperty("cameraStreamGoldens")!!)

    private fun golden(name: String) = File(goldens, name).readBytes()

    private fun encode(write: (DataOutputStream) -> Unit): ByteArray =
        ByteArrayOutputStream().also { write(DataOutputStream(it)) }.toByteArray()

    @Test
    fun calibrationMatchesGolden() {
        val calibration = Calibration(
            width = 1280,
            height = 960,
            position = 0,
            source = 0,
            timestampSource = 1,
            lensPoseReference = 2,
            codec = StreamFormat.CODEC_JPEG,
            activeArray = intArrayOf(0, 0, 1280, 960),
            intrinsics = floatArrayOf(433.25f, 433.5f, 640.125f, 480.75f, 0f),
            distortion = floatArrayOf(0.0625f, -0.03125f, 0f, 0.001953125f, -0.0009765625f),
            poseTranslation = floatArrayOf(-0.03125f, -0.015625f, -0.0625f),
            poseRotation = floatArrayOf(0.5f, -0.5f, 0.5f, -0.5f),
            cameraId = "50",
        )
        assertArrayEquals(golden("calibration.bin"), encode { StreamFormat.writeCalibration(it, calibration) })
    }

    @Test
    fun frameMatchesGolden() {
        val info = FrameInfo(1234, 1_000_000_123_456_789, 4_000_000, 8_300_000, 1_000_000_160_000_000)
        val payload = golden("payload.jpg") + ByteArray(7) // trailing capacity is not sent
        val bytes = encode { StreamFormat.writeFrame(it, info, payload, payload.size - 7) }
        assertArrayEquals(golden("frame.bin"), bytes)
    }

    @Test
    fun pingParsesAndPongMatchesGolden() {
        val (type, body) = StreamFormat.readMessage(DataInputStream(golden("ping.bin").inputStream()))
        assertEquals(StreamFormat.TYPE_PING, type)
        val ping = StreamFormat.parsePing(body)
        assertEquals(Ping(7, 5_000_000_000), ping)
        assertArrayEquals(golden("pong.bin"), encode { StreamFormat.writePong(it, ping, 1_000_000_200_000_000) })
    }

    @Test
    fun configureGoldenParses() {
        val (type, body) = StreamFormat.readMessage(DataInputStream(golden("configure.bin").inputStream()))
        assertEquals(StreamFormat.TYPE_CONFIGURE, type)
        assertEquals(StreamConfig(maxRateHz = 60, jpegQuality = 90), StreamFormat.parseConfigure(body))
    }

    @Test
    fun configureWithDetectionGoldenParses() {
        val (type, body) = StreamFormat.readMessage(DataInputStream(golden("configure_detect.bin").inputStream()))
        assertEquals(StreamFormat.TYPE_CONFIGURE, type)
        val expected = StreamConfig(15, 80, images = false, detectDictionary = 0, detectRateHz = 30)
        assertEquals(expected, StreamFormat.parseConfigure(body))
    }

    @Test(expected = IOException::class)
    fun configureImagesFlagOutOfRangeIsRejected() {
        val (_, body) = StreamFormat.readMessage(DataInputStream(golden("configure_detect.bin").inputStream()))
        StreamFormat.parseConfigure(body.also { it[4] = 2 })
    }

    @Test
    fun configureWithDepthTailParses() {
        val (type, body) = StreamFormat.readMessage(DataInputStream(golden("configure_depth.bin").inputStream()))
        assertEquals(StreamFormat.TYPE_CONFIGURE, type)
        val expected = StreamConfig(15, 80, images = true, detectDictionary = 0, detectRateHz = 30, depth = true, depthRateHz = 10)
        assertEquals(expected, StreamFormat.parseConfigure(body))
    }

    @Test(expected = IOException::class)
    fun configureDepthFlagOutOfRangeIsRejected() {
        val (_, body) = StreamFormat.readMessage(DataInputStream(golden("configure_detect.bin").inputStream()))
        StreamFormat.parseConfigure(body + byteArrayOf(2, 0, 0))
    }

    @Test
    fun readerReassemblesMessagesFromChunks() {
        val bytes = golden("configure.bin") + golden("ping.bin") + golden("configure_detect.bin")
        val seen = ArrayList<Pair<Int, Int>>()
        val reader = MessageReader()
        // Chunks cut inside headers and bodies, and one chunk holding several messages.
        for (chunk in listOf(bytes.copyOfRange(0, 5), bytes.copyOfRange(5, 20), bytes.copyOfRange(20, bytes.size))) {
            reader.push(chunk) { type, body -> seen.add(type to body.size) }
        }
        assertEquals(listOf(StreamFormat.TYPE_CONFIGURE to 4, StreamFormat.TYPE_PING to 12, StreamFormat.TYPE_CONFIGURE to 8), seen)
        reader.push(golden("ping.bin")) { type, body -> seen.add(type to body.size) }
        assertEquals(4, seen.size)
    }

    @Test(expected = IOException::class)
    fun readerRejectsBadMagic() {
        MessageReader().push(golden("ping.bin").also { it[1] = 0 }) { _, _ -> }
    }

    private fun markerReport(frameNumber: Long, detectUs: Int) = MarkerReport(1280, 960).apply {
        val frame = LumaFrame(0, 1, 1).apply {
            this.frameNumber = frameNumber
            sensorTimestampNs = 1_000_000_123_456_789
            setCapture(frameNumber, 4_000_000, 8_300_000)
        }
        reset(frame, detectorDictionary = 0)
        this.detectUs = detectUs
    }

    @Test
    fun markersMatchGolden() {
        val report = markerReport(1234, 8125).apply {
            add(3, floatArrayOf(100.25f, 200.5f, 180.75f, 201f, 179.5f, 280.25f, 99f, 279.75f))
            add(17, floatArrayOf(640.125f, 480f, 700f, 470.5f, 710.5f, 530f, 650f, 540.25f))
        }
        assertArrayEquals(golden("markers.bin"), encode { StreamFormat.writeMarkers(it, report) })
    }

    @Test
    fun emptyMarkersMatchGolden() {
        val report = markerReport(1235, 6250)
        assertArrayEquals(golden("markers_empty.bin"), encode { StreamFormat.writeMarkers(it, report) })
    }

    private val wifiStatus = LinkStatus(
        sendTimeNs = 1_000_000_210_000_000,
        transport = StreamFormat.TRANSPORT_WIFI,
        route = StreamFormat.ROUTE_NETWORK,
        rssiDbm = -30,
        txMbps = 1200,
        rxMbps = 1080,
        frequencyMhz = 5180,
        localAddress = "192.168.0.69",
        interfaceName = "wlan0",
    )

    @Test
    fun linkStatusMatchesGolden() {
        assertArrayEquals(golden("link_status.bin"), encode { StreamFormat.writeLinkStatus(it, wifiStatus) })
    }

    @Test
    fun adbLinkStatusWithoutWifiMatchesGolden() {
        val status = LinkStatus(
            sendTimeNs = 1_000_000_220_000_000,
            transport = StreamFormat.TRANSPORT_NONE,
            route = StreamFormat.ROUTE_LOOPBACK,
            rssiDbm = null,
            txMbps = null,
            rxMbps = null,
            frequencyMhz = null,
            localAddress = "127.0.0.1",
            interfaceName = "lo",
        )
        assertArrayEquals(golden("link_status_adb.bin"), encode { StreamFormat.writeLinkStatus(it, status) })
    }

    @Test
    fun linkStatusKeyIgnoresSignalAndSpeed() {
        assertEquals(wifiStatus.key(), wifiStatus.copy(sendTimeNs = 0, rssiDbm = -70, txMbps = 300).key())
        assertNotEquals(wifiStatus.key(), wifiStatus.copy(frequencyMhz = 2437).key())
    }

    @Test
    fun markerReportGrowsAndResets() {
        val report = markerReport(1, 1)
        val xy = FloatArray(8) { it.toFloat() }
        repeat(40) { report.add(it, xy) }
        assertEquals(40, report.count)
        assertEquals(39, report.ids[39])
        assertEquals(7f, report.corners[39 * 8 + 7])
        report.reset(LumaFrame(0, 1, 1), detectorDictionary = 20)
        assertEquals(0, report.count)
        assertEquals(20, report.dictionary)
    }

    @Test(expected = IOException::class)
    fun configureQualityOutOfRangeIsRejected() {
        val (_, body) = StreamFormat.readMessage(DataInputStream(golden("configure.bin").inputStream()))
        StreamFormat.parseConfigure(body.also { it[2] = 0 })
    }

    @Test(expected = IOException::class)
    fun badMagicIsRejected() {
        val bytes = golden("ping.bin").also { it[0] = 'X'.code.toByte() }
        StreamFormat.readMessage(DataInputStream(bytes.inputStream()))
    }

    @Test(expected = IOException::class)
    fun otherVersionIsRejected() {
        val bytes = golden("ping.bin").also { it[5] = 2 }
        StreamFormat.readMessage(DataInputStream(bytes.inputStream()))
    }
}
