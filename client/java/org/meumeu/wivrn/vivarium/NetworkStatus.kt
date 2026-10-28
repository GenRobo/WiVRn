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

import android.content.Context
import android.net.ConnectivityManager
import android.net.Network
import android.net.NetworkCapabilities
import android.net.wifi.WifiInfo
import java.net.Inet4Address

/**
 * Reads the headset's active network (transport, Wi-Fi signal, link speeds, frequency,
 * interface and address) and wakes [onChange] when the default network changes. The
 * stream itself rides on WiVRn's connection, so the route is always [StreamFormat.ROUTE_WIVRN].
 */
class NetworkStatus(context: Context, private val onChange: () -> Unit) {
    private val connectivity = context.getSystemService(ConnectivityManager::class.java)
    private val callback = object : ConnectivityManager.NetworkCallback() {
        override fun onAvailable(network: Network) = onChange()

        override fun onLost(network: Network) = onChange()

        override fun onCapabilitiesChanged(network: Network, capabilities: NetworkCapabilities) = onChange()
    }

    fun start() = connectivity.registerDefaultNetworkCallback(callback)

    fun stop() = connectivity.unregisterNetworkCallback(callback)

    /** The status now. */
    fun read(sendTimeNs: Long): LinkStatus {
        val network = connectivity.activeNetwork
        val capabilities = network?.let(connectivity::getNetworkCapabilities)
        val transport = when {
            capabilities == null -> StreamFormat.TRANSPORT_NONE
            capabilities.hasTransport(NetworkCapabilities.TRANSPORT_WIFI) -> StreamFormat.TRANSPORT_WIFI
            capabilities.hasTransport(NetworkCapabilities.TRANSPORT_ETHERNET) -> StreamFormat.TRANSPORT_ETHERNET
            capabilities.hasTransport(NetworkCapabilities.TRANSPORT_USB) -> StreamFormat.TRANSPORT_USB
            else -> StreamFormat.TRANSPORT_OTHER
        }
        // Without location permission Android redacts the SSID and BSSID, not these values.
        val wifi = capabilities?.transportInfo as? WifiInfo
        val link = network?.let(connectivity::getLinkProperties)
        val addresses = link?.linkAddresses?.map { it.address }.orEmpty()
        val local = addresses.firstOrNull { it is Inet4Address } ?: addresses.firstOrNull()
        return LinkStatus(
            sendTimeNs = sendTimeNs,
            transport = transport,
            route = StreamFormat.ROUTE_WIVRN,
            rssiDbm = wifi?.rssi?.takeIf { it in -126..0 },
            txMbps = wifi?.txLinkSpeedMbps?.takeIf { it > 0 },
            rxMbps = wifi?.rxLinkSpeedMbps?.takeIf { it > 0 },
            frequencyMhz = wifi?.frequency?.takeIf { it > 0 },
            localAddress = local?.hostAddress ?: "",
            interfaceName = link?.interfaceName ?: "",
        )
    }
}
