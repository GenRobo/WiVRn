/*
 * WiVRn VR streaming
 * Copyright (C) 2022-2026 Guillaume Meunier <guillaume.meunier@centraliens.net>
 * Copyright (C) 2022-2026  Patrick Nicolas <patricknicolas@laposte.net>
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

/*!
 * @file
 * @brief  Headless Windows supervisor for the WiVRn Monado service.
 *
 * Replaces the Linux main.cpp (GLib main loop, D-Bus, Avahi, libnotify, systemd/fork app launcher,
 * active-runtime management). One headset session per process: listen on TCP <port>, authenticate,
 * run Monado's IPC server in-process until the session ends, then exit (restart the process to
 * accept the next session).
 */

#include "driver/configuration.h"
#include "driver/wivrn_connection.h"
#include "exit_codes.h"
#include "ipc_server_cb.h"
#include "utils/overloaded.h"
#include "version.h"
#include "wivrn_config.h"
#include "wivrn_ipc.h"
#include "wivrn_packets.h"
#include "wivrn_sockets.h"

#include "util/u_trace_marker.h"
#include <server/ipc_server_interface.h>

#include <CLI/CLI.hpp>
#include <atomic>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <mutex>
#include <random>
#include <streambuf>
#include <thread>

#include "util/u_logging.h"

#include <windows.h>

U_TRACE_TARGET_SETUP(U_TRACE_WHICH_SERVICE)

using namespace wivrn;

std::optional<wivrn::typed_socket<wivrn::UnixDatagram, from_monado::packets, to_monado::packets>> wivrn_ipc_socket_main_loop;
std::optional<wivrn::typed_socket<wivrn::UnixDatagram, to_monado::packets, from_monado::packets>> wivrn_ipc_socket_monado;

namespace
{
std::atomic<bool> quitting = false;
std::atomic<bool> session_running = false;

/*
 * Log file: everything written to std::cerr and every Monado/WiVRn U_LOG message is also appended
 * to <log-dir>/wivrn-server-<time>.log, so a console/log-buffer flood can't erase the history.
 */
std::mutex log_mutex;
FILE * log_file = nullptr;

void log_write(const char * data, size_t size)
{
	std::lock_guard lock(log_mutex);
	if (log_file)
	{
		fwrite(data, 1, size, log_file);
		fflush(log_file);
	}
}

class tee_buf : public std::streambuf
{
	std::streambuf * console;

protected:
	int overflow(int c) override
	{
		if (c == EOF)
			return 0;
		char ch = char(c);
		log_write(&ch, 1);
		return console->sputc(ch);
	}
	std::streamsize xsputn(const char * s, std::streamsize n) override
	{
		log_write(s, size_t(n));
		return console->sputn(s, n);
	}
	int sync() override
	{
		return console->pubsync();
	}

public:
	explicit tee_buf(std::streambuf * console) :
	        console(console) {}
};

void log_sink(const char * file, int line, const char * func, enum u_logging_level level, const char * format, va_list args, void *)
{
	static const char * names[] = {"TRACE", "DEBUG", "INFO", "WARN", "ERROR", "RAW"};
	char msg[4096];
	vsnprintf(msg, sizeof(msg), format, args);
	SYSTEMTIME t;
	GetLocalTime(&t);
	char line_buf[4352];
	int n = snprintf(line_buf, sizeof(line_buf), "%02d:%02d:%02d.%03d %5s [%s] %s\n", t.wHour, t.wMinute, t.wSecond, t.wMilliseconds, names[std::min<int>(level, 5)], func, msg);
	if (n > 0)
		log_write(line_buf, std::min<size_t>(n, sizeof(line_buf) - 1));
}

void open_log_file(std::filesystem::path dir)
{
	std::error_code ec;
	std::filesystem::create_directories(dir, ec);
	SYSTEMTIME t;
	GetLocalTime(&t);
	char name[64];
	snprintf(name, sizeof(name), "wivrn-server-%04d%02d%02d-%02d%02d%02d.log", t.wYear, t.wMonth, t.wDay, t.wHour, t.wMinute, t.wSecond);
	auto path = dir / name;
	log_file = _wfopen(path.c_str(), L"ab");
	if (not log_file)
	{
		std::cerr << "Cannot open log file " << path.string() << std::endl;
		return;
	}
	static tee_buf cerr_tee(std::cerr.rdbuf());
	std::cerr.rdbuf(&cerr_tee);
	u_log_set_sink(log_sink, nullptr);
	std::cerr << "Logging to " << path.string() << std::endl;
}

BOOL WINAPI on_console_ctrl(DWORD type)
{
	std::cerr << "Shutting down (console event " << type << ")" << std::endl;
	quitting = true;
	if (session_running)
		wivrn_ipc_socket_main_loop->send(to_monado::stop{});
	return TRUE;
}

std::string make_pin()
{
	std::random_device rd;
	std::mt19937 gen(rd());
	std::uniform_int_distribution<> distrib(0, 999999);
	char buffer[7];
	snprintf(buffer, sizeof(buffer), "%06d", distrib(gen));
	return buffer;
}

// Drain supervisor-side control packets (what D-Bus/Avahi/notifications consumed on Linux); log only.
void drain_control_channel()
{
	while (true)
	{
		pollfd_t fd{};
		fd.fd = wivrn_ipc_socket_main_loop->get_fd();
		fd.events = POLLIN;
		if (poll_sockets(&fd, 1, 0) <= 0 or not(fd.revents & POLLIN))
			return;

		auto packet = wivrn_ipc_socket_main_loop->receive();
		if (not packet)
			return;

		std::visit(utils::overloaded{
		                   [](const from_headset::headset_info_packet & info) {
			                   static const char * names[] = {"h264", "h265", "av1", "raw"};
			                   std::string codecs;
			                   for (auto c: info.supported_codecs)
				                   codecs += std::string(codecs.empty() ? "" : ", ") + (unsigned(c) < 4 ? names[c] : "?");
			                   std::cerr << "Headset info: " << info.system_name
			                             << "; decoders (preferred first): " << codecs
			                             << "; bit depth: " << (info.bit_depth ? std::to_string(*info.bit_depth) : std::string("default"))
			                             << "; stream eye " << info.stream_eye_width << "x" << info.stream_eye_height
			                             << " (render " << info.render_eye_width << "x" << info.render_eye_height << ")"
			                             << "; preferred " << info.settings.preferred_refresh_rate << " Hz, bitrate " << info.settings.bitrate_bps
			                             << "; passthrough " << info.passthrough << std::endl;
		                   },
		                   [](const from_headset::settings_changed & s) {
			                   std::cerr << "Headset settings: bitrate " << s.bitrate_bps << " bps, refresh " << s.preferred_refresh_rate << std::endl;
		                   },
		                   [](const from_headset::start_app &) {
			                   std::cerr << "Headset requested an application launch: not supported on Windows" << std::endl;
		                   },
		                   [](const from_headset::stream_tab_changed &) {},
		                   [](const from_monado::headset_connected &) { std::cerr << "Headset reconnected" << std::endl; },
		                   [](const from_monado::headset_disconnected &) { std::cerr << "Headset disconnected" << std::endl; },
		                   [](const from_monado::server_error & e) {
			                   std::cerr << "Server error in " << e.where << ": " << e.message << std::endl;
		                   },
		           },
		           *packet);
	}
}

int run_session()
{
	wivrn::ipc_server_cb server_cb;
	ipc_server_main_info server_info{
	        .udgci = {
	                .window_title = "WiVRn",
	                .open = U_DEBUG_GUI_OPEN_NEVER,
	        },
	        .no_stdin = true,
	};

	session_running = true;
	int result = EXIT_FAILURE;
	std::jthread monado([&] {
		try
		{
			result = ipc_server_main_common(&server_info, &server_cb, nullptr);
		}
		catch (std::exception & e)
		{
			std::cerr << e.what() << std::endl;
		}
		session_running = false;
	});

	while (session_running)
	{
		drain_control_channel();
		std::this_thread::sleep_for(std::chrono::milliseconds(50));
	}
	monado.join();
	drain_control_channel();
	return result;
}
} // namespace

int main(int argc, char * argv[])
{
	CLI::App app;

	std::string config_file;
	app.add_option("-f", config_file, "configuration file")->option_text("FILE")->check(CLI::ExistingFile);
	auto version_flag = app.add_flag("--version")->description("print version and exit");
	auto no_encrypt = app.add_flag("--no-encrypt")->description("disable encryption")->group("Debug");
	auto pairing = app.add_flag("--pair")->description("enable pairing (a PIN is printed) even if headsets are already known");
	std::string log_dir;
	app.add_option("--log-dir", log_dir, "also write the log to <dir>/wivrn-server-<time>.log (default %LOCALAPPDATA%\\wivrn\\logs)")->option_text("DIR");
	// Accepted for command-line compatibility with the Linux server; these features don't exist here.
	app.add_flag("--no-publish-service,--no-manage-active-runtime,--no-fork,--no-instructions")->group("");

	CLI11_PARSE(app, argc, argv);

	if (*version_flag)
	{
		std::cout << "WiVRn version " << wivrn::display_version() << std::endl;
		return 0;
	}

	if (log_dir.empty())
		log_dir = (std::filesystem::path(std::getenv("LOCALAPPDATA") ? std::getenv("LOCALAPPDATA") : ".") / "wivrn" / "logs").string();
	open_log_file(std::filesystem::path(log_dir));

	if (not config_file.empty())
		configuration::set_config_file(config_file);

	std::cerr << "WiVRn " << wivrn::display_version() << " starting (headless Windows build)" << std::endl;

	_putenv_s("XRT_LOG", std::getenv("XRT_LOG") ? std::getenv("XRT_LOG") : "info");

	try
	{
		init_sockets();

		auto [main_loop_end, monado_end] = make_datagram_pair();
		wivrn_ipc_socket_main_loop.emplace(main_loop_end);
		wivrn_ipc_socket_monado.emplace(monado_end);

		SetConsoleCtrlHandler(on_console_ctrl, TRUE);

		auto enc_state = wivrn_connection::encryption_state::enabled;
		std::string pin;
		if (*no_encrypt)
		{
			enc_state = wivrn_connection::encryption_state::disabled;
			std::cerr << "Encryption is disabled" << std::endl;
		}
		else if (*pairing or known_keys().empty())
		{
			enc_state = wivrn_connection::encryption_state::pairing;
			pin = make_pin();
			std::cerr << "To pair a new headset use PIN code: " << pin << std::endl;
			std::cerr << "  (the headset shows a PIN pad after connecting; or pass it in the URI: wivrn+tcp://:" << pin << "@localhost)" << std::endl;
		}

		const int port = configuration().port;
		auto delay_next_try = std::chrono::milliseconds(10);

		while (not quitting)
		{
			std::optional<TCP> tcp;
			{
				// The listener must be closed before the session starts: the session's reconnect path
				// (accept_connection) binds the same port again, and Windows refuses a second listener
				// (WSAEADDRINUSE) while this one is open.
				TCPListener listener(port);
				std::cerr << "Listening on TCP port " << port << " (UDP on the same port once a headset connects); no mDNS, connect manually" << std::endl;

				while (not quitting and not tcp)
				{
					pollfd_t fd{};
					fd.fd = listener.get_fd();
					fd.events = POLLIN;
					if (poll_sockets(&fd, 1, 200) > 0 and (fd.revents & POLLIN))
						tcp.emplace(listener.accept().first);
					drain_control_channel();
				}
			}
			if (quitting)
				break;

			try
			{
				std::stop_source stop;
				connection = std::make_unique<wivrn_connection>(stop.get_token(), enc_state, pin, std::move(*tcp));
			}
			catch (wivrn::incorrect_pin &)
			{
				delay_next_try *= 2;
				std::cerr << "Incorrect PIN, waiting " << delay_next_try.count() << "ms" << std::endl;
				std::this_thread::sleep_for(delay_next_try);
				continue;
			}
			catch (std::exception & e)
			{
				std::cerr << "Client connection failed: " << e.what() << std::endl;
				std::this_thread::sleep_for(std::chrono::seconds(1));
				continue;
			}

			std::cerr << "Client connected" << std::endl;
			if (enc_state == wivrn_connection::encryption_state::pairing)
				enc_state = wivrn_connection::encryption_state::enabled;

			// Monado's IPC server (named pipe) and the WiVRn session run in this process; the session
			// takes ownership of `connection` in instance::create_system.
			int ret = run_session();
			std::cerr << "Session ended (" << ret << "). Restart wivrn-server to accept another session." << std::endl;
			return ret;
		}
	}
	catch (std::exception & e)
	{
		std::cerr << e.what() << std::endl;
		return wivrn_exit_code::unknown_error;
	}

	return wivrn_exit_code::success;
}
