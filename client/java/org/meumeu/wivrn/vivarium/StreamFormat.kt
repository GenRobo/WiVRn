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

import java.io.DataInputStream
import java.io.DataOutputStream
import java.io.IOException

/**
 * Vivarium camera stream format, version 1, carried on the WiVRn application data
 * channel [CHANNEL]. Every message is a 12-byte header (magic, version, type, body
 * length), big-endian. The layout is documented in the Vivarium teleop package
 * (packages/vivarium-teleop/README.md); golden files in client/test/fixtures pin it
 * for both sides. DEPTH messages are written by the C++ side (client/vivarium).
 */
object StreamFormat {
    const val CHANNEL = "vivarium-camera"
    const val VERSION = 1
    const val MAX_BODY_BYTES = 8 * 1024 * 1024
    const val TYPE_CALIBRATION = 1
    const val TYPE_FRAME = 2
    const val TYPE_PING = 3
    const val TYPE_PONG = 4
    const val TYPE_CONFIGURE = 5
    const val TYPE_MARKERS = 6
    const val TYPE_LINK_STATUS = 7
    const val TYPE_DEPTH = 8
    const val CODEC_JPEG = 1
    /** Dictionary byte of the configure message that turns detection off. */
    const val DETECTION_OFF = 255
    const val TRANSPORT_NONE = 0
    const val TRANSPORT_WIFI = 1
    const val TRANSPORT_ETHERNET = 2
    const val TRANSPORT_USB = 3
    const val TRANSPORT_OTHER = 4
    const val ROUTE_LOOPBACK = 0
    const val ROUTE_NETWORK = 1
    /** The stream rides on WiVRn's own connection to the PC. */
    const val ROUTE_WIVRN = 2
    const val HEADER_BYTES = 12
    private const val RSSI_ABSENT = -0x8000
    private val MAGIC = "VCAM".toByteArray(Charsets.US_ASCII)
    private const val CALIBRATION_FIXED_BYTES = 106
    private const val FRAME_FIXED_BYTES = 40
    private const val MARKERS_FIXED_BYTES = 44
    private const val MARKER_BYTES = 36
    private const val LINK_STATUS_FIXED_BYTES = 18

    private fun writeHeader(out: DataOutputStream, type: Int, length: Int) {
        out.write(MAGIC)
        out.writeShort(VERSION)
        out.writeShort(type)
        out.writeInt(length)
    }

    private fun writeFloats(out: DataOutputStream, values: FloatArray, count: Int) {
        require(values.size == count) { "expected $count values, got ${values.size}" }
        values.forEach(out::writeFloat)
    }

    fun writeCalibration(out: DataOutputStream, c: Calibration) {
        val id = c.cameraId.toByteArray(Charsets.UTF_8)
        writeHeader(out, TYPE_CALIBRATION, CALIBRATION_FIXED_BYTES + id.size)
        out.writeInt(c.width)
        out.writeInt(c.height)
        out.writeInt(c.position)
        out.writeInt(c.source)
        out.writeByte(c.timestampSource)
        out.writeByte(c.lensPoseReference)
        out.writeByte(c.codec)
        out.writeByte(0)
        c.activeArray.forEach(out::writeInt)
        writeFloats(out, c.intrinsics, 5)
        writeFloats(out, c.distortion, 5)
        writeFloats(out, c.poseTranslation, 3)
        writeFloats(out, c.poseRotation, 4)
        out.writeShort(id.size)
        out.write(id)
    }

    fun writeFrame(out: DataOutputStream, f: FrameInfo, payload: ByteArray, length: Int) {
        writeHeader(out, TYPE_FRAME, FRAME_FIXED_BYTES + length)
        out.writeLong(f.frameNumber)
        out.writeLong(f.sensorTimestampNs)
        out.writeLong(f.exposureTimeNs)
        out.writeLong(f.rollingShutterSkewNs)
        out.writeLong(f.sendTimeNs)
        out.write(payload, 0, length)
    }

    fun writeMarkers(out: DataOutputStream, r: MarkerReport) {
        writeHeader(out, TYPE_MARKERS, MARKERS_FIXED_BYTES + MARKER_BYTES * r.count)
        out.writeLong(r.frameNumber)
        out.writeLong(r.sensorTimestampNs)
        out.writeLong(r.exposureTimeNs)
        out.writeLong(r.rollingShutterSkewNs)
        out.writeShort(r.width)
        out.writeShort(r.height)
        out.writeByte(r.dictionary)
        out.writeByte(0)
        out.writeShort(r.count)
        out.writeInt(r.detectUs)
        for (i in 0 until r.count) {
            out.writeInt(r.ids[i])
            for (j in 0 until 8) out.writeFloat(r.corners[8 * i + j])
        }
    }

    fun writePong(out: DataOutputStream, ping: Ping, headsetNs: Long) {
        writeHeader(out, TYPE_PONG, 20)
        out.writeInt(ping.sequence)
        out.writeLong(ping.pcSendNs)
        out.writeLong(headsetNs)
    }

    fun writeLinkStatus(out: DataOutputStream, s: LinkStatus) {
        // Each text is a u8 length and UTF-8 bytes; addresses and interface names are short.
        val address = s.localAddress.toByteArray(Charsets.UTF_8).let { it.copyOf(minOf(it.size, 255)) }
        val name = s.interfaceName.toByteArray(Charsets.UTF_8).let { it.copyOf(minOf(it.size, 255)) }
        writeHeader(out, TYPE_LINK_STATUS, LINK_STATUS_FIXED_BYTES + 2 + address.size + name.size)
        out.writeLong(s.sendTimeNs)
        out.writeByte(s.transport)
        out.writeByte(s.route)
        out.writeShort(s.rssiDbm ?: RSSI_ABSENT)
        out.writeShort(s.txMbps ?: 0)
        out.writeShort(s.rxMbps ?: 0)
        out.writeShort(s.frequencyMhz ?: 0)
        out.writeByte(address.size)
        out.write(address)
        out.writeByte(name.size)
        out.write(name)
    }

    /** Reads a 12-byte header and returns the message type and body length. */
    fun readHeader(input: DataInputStream): Pair<Int, Int> {
        val magic = ByteArray(4).also(input::readFully)
        if (!magic.contentEquals(MAGIC)) throw IOException("bad magic")
        val version = input.readUnsignedShort()
        if (version != VERSION) throw IOException("stream version $version, expected $VERSION")
        val type = input.readUnsignedShort()
        val length = input.readInt()
        if (length < 0 || length > MAX_BODY_BYTES) throw IOException("bad body length $length")
        return type to length
    }

    /** Reads one message and returns its type and body; throws IOException on bad input. */
    fun readMessage(input: DataInputStream): Pair<Int, ByteArray> {
        val (type, length) = readHeader(input)
        return type to ByteArray(length).also(input::readFully)
    }

    fun parsePing(body: ByteArray): Ping {
        if (body.size != 12) throw IOException("ping body has ${body.size} bytes")
        val input = DataInputStream(body.inputStream())
        return Ping(input.readInt(), input.readLong())
    }

    /**
     * A 4-byte body sets the image stream only; 8 bytes add the images switch and
     * detection; 11 bytes add the depth switch and rate.
     */
    fun parseConfigure(body: ByteArray): StreamConfig {
        if (body.size != 4 && body.size != 8 && body.size != 11) throw IOException("configure body has ${body.size} bytes")
        val input = DataInputStream(body.inputStream())
        val maxRateHz = input.readUnsignedShort()
        val jpegQuality = input.readUnsignedByte()
        if (jpegQuality !in 1..100) throw IOException("JPEG quality $jpegQuality outside 1 to 100")
        if (body.size == 4) return StreamConfig(maxRateHz, jpegQuality)
        input.readUnsignedByte() // reserved
        val images = input.readUnsignedByte()
        if (images > 1) throw IOException("images flag $images is not 0 or 1")
        val dictionary = input.readUnsignedByte()
        val detectRateHz = input.readUnsignedShort()
        var depth = false
        var depthRateHz = 0
        if (body.size == 11) {
            val flag = input.readUnsignedByte()
            if (flag > 1) throw IOException("depth flag $flag is not 0 or 1")
            depth = flag == 1
            depthRateHz = input.readUnsignedShort()
        }
        return StreamConfig(
            maxRateHz,
            jpegQuality,
            images = images == 1,
            detectDictionary = if (dictionary == DETECTION_OFF) null else dictionary,
            detectRateHz = detectRateHz,
            depth = depth,
            depthRateHz = depthRateHz,
        )
    }
}

/**
 * Reassembles messages from the chunks the application data channel delivers: a chunk
 * may hold part of a message or several messages. [push] calls [handle] with the type
 * and body of each complete message, in order; it throws IOException on bad input, after
 * which the reader must be discarded.
 */
class MessageReader {
    private var buffer = ByteArray(4096)
    private var size = 0

    fun push(chunk: ByteArray, handle: (type: Int, body: ByteArray) -> Unit) {
        if (size + chunk.size > buffer.size) buffer = buffer.copyOf(maxOf(buffer.size * 2, size + chunk.size))
        chunk.copyInto(buffer, size)
        size += chunk.size
        var start = 0
        while (size - start >= StreamFormat.HEADER_BYTES) {
            val (type, length) = StreamFormat.readHeader(DataInputStream(buffer.inputStream(start, StreamFormat.HEADER_BYTES)))
            val end = start + StreamFormat.HEADER_BYTES + length
            if (end > size) break
            handle(type, buffer.copyOfRange(start + StreamFormat.HEADER_BYTES, end))
            start = end
        }
        if (start > 0) {
            buffer.copyInto(buffer, 0, start, size)
            size -= start
        }
    }
}

class Calibration(
    val width: Int,
    val height: Int,
    val position: Int,
    val source: Int,
    val timestampSource: Int,
    val lensPoseReference: Int,
    val codec: Int,
    val activeArray: IntArray,
    val intrinsics: FloatArray,
    val distortion: FloatArray,
    val poseTranslation: FloatArray,
    val poseRotation: FloatArray,
    val cameraId: String,
)

data class FrameInfo(
    val frameNumber: Long,
    val sensorTimestampNs: Long,
    val exposureTimeNs: Long,
    val rollingShutterSkewNs: Long,
    val sendTimeNs: Long,
)

/**
 * The headset's active network ([transport], `TRANSPORT_*`) and Wi-Fi signal, link speeds
 * (Mbps) and frequency (MHz), null where Android reports none; and how the stream reaches
 * the PC: [route] (`ROUTE_*`), [localAddress] and [interfaceName] of the active network.
 */
data class LinkStatus(
    val sendTimeNs: Long,
    val transport: Int,
    val route: Int,
    val rssiDbm: Int?,
    val txMbps: Int?,
    val rxMbps: Int?,
    val frequencyMhz: Int?,
    val localAddress: String,
    val interfaceName: String,
) {
    /** What the app logs as a change; signal and link speeds vary too often. */
    fun key() = listOf(transport, route, localAddress, interfaceName, frequencyMhz)
}

/** Sequence is an unsigned 32-bit counter carried as its bit pattern. */
data class Ping(val sequence: Int, val pcSendNs: Long)

/**
 * Set by the PC: image frame-rate cap (0: every camera frame), JPEG quality and the
 * images switch; the marker dictionary (null: detection off) and detection rate cap
 * (0: as fast as the detector keeps up); the depth switch and rate cap (0: every frame).
 */
data class StreamConfig(
    val maxRateHz: Int,
    val jpegQuality: Int,
    val images: Boolean = true,
    val detectDictionary: Int? = null,
    val detectRateHz: Int = 0,
    val depth: Boolean = false,
    val depthRateHz: Int = 0,
) {
    companion object {
        /** Used until the PC configures the stream. */
        val DEFAULT = StreamConfig(maxRateHz = 15, jpegQuality = 80)
    }
}

/**
 * Markers found in one camera frame, reused from frame to frame: [ids] and the four
 * corners of each marker in [corners] (x, y pairs clockwise from the top-left) hold
 * [count] markers.
 */
class MarkerReport(val width: Int, val height: Int) {
    var frameNumber = 0L
    var sensorTimestampNs = 0L
    var exposureTimeNs = -1L
    var rollingShutterSkewNs = -1L
    var dictionary = 0
    var detectUs = 0
    var count = 0
        private set
    var ids = IntArray(16)
        private set
    var corners = FloatArray(16 * 8)
        private set

    /** Starts a report for [frame]; clears the markers. */
    fun reset(frame: LumaFrame, detectorDictionary: Int) {
        frameNumber = frame.frameNumber
        sensorTimestampNs = frame.sensorTimestampNs
        exposureTimeNs = frame.exposureTimeNs
        rollingShutterSkewNs = frame.rollingShutterSkewNs
        dictionary = detectorDictionary
        detectUs = 0
        count = 0
    }

    /** Appends a marker; [xy] holds its four corners as x, y pairs. */
    fun add(id: Int, xy: FloatArray) {
        if (count == ids.size) {
            ids = ids.copyOf(count * 2)
            corners = corners.copyOf(count * 16)
        }
        ids[count] = id
        xy.copyInto(corners, 8 * count, 0, 8)
        count++
    }
}
