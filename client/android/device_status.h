/*
 * WiVRn VR streaming
 * Copyright (C) 2026  GenRobo
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

#pragma once

#include <cstdint>

/// Android PowerManager thermal status 0..6, 255 when unavailable (API < 29 or failure).
uint8_t get_thermal_status();

/// The active network as from_headset::telemetry reports it.
struct network_status
{
	uint8_t kind = 0;       // 0 unknown, 1 wifi, 3 ethernet, 4 usb
	int16_t rssi_dbm = -32768; // wifi only
	uint32_t link_mbps = 0; // 0 unknown
};
network_status get_network_status();
