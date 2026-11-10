/*
 * WiVRn VR streaming
 * Copyright (C) 2024  Guillaume Meunier <guillaume.meunier@centraliens.net>
 * Copyright (C) 2024  Patrick Nicolas <patricknicolas@laposte.net>
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

#include <functional>
#include <pthread.h>
#include <string>
#include <string_view>
#include <thread>
#include <type_traits>
#include <utility>

namespace utils
{
/// The kernel keeps TASK_COMM_LEN - 1 = 15 bytes of a thread name; pthread_setname_np rejects
/// longer names (ERANGE) instead of truncating them, so cut here.
constexpr size_t thread_name_max = 15;

inline std::string thread_name(std::string_view name)
{
	return std::string(name.substr(0, thread_name_max));
}

/// std::thread whose comm is `name` (cut to 15 bytes) before `f` runs. The thread names itself
/// so the name is set before anything in `f` reads it: jni_thread::setup_thread hands it to ART,
/// which would otherwise rename a JNI-attached thread "Thread-N" (GRID #3095).
template <typename F, typename... Args>
std::thread named_thread(std::string_view name, F && f, Args &&... args)
{
	return std::thread(
	        [](std::string name, std::decay_t<F> f, std::decay_t<Args>... args) {
		        pthread_setname_np(pthread_self(), name.c_str());
		        std::invoke(std::move(f), std::move(args)...);
	        },
	        thread_name(name),
	        std::forward<F>(f),
	        std::forward<Args>(args)...);
}
} // namespace utils
