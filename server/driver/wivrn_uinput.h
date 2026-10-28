#include "wivrn_packets.h"
#include "wivrn_sockets.h"

#include <cstdint>
#include <map>
#include <vector>

#ifdef _WIN32
#include <stdexcept>
// No uinput on Windows: construction fails, so the session reports hid_input=false and keeps
// the OpenXR controllers/gamepad (wivrn_session catches the exception).
class wivrn_uinput
{
public:
	wivrn_uinput()
	{
		throw std::runtime_error("input forwarding is not supported on Windows");
	}
	void handle_input(wivrn::from_headset::hid::input &) {}
	void handle_gamepad(const wivrn::from_headset::inputs &) {}
	void destroy_gamepad() {}
	std::vector<wivrn::to_headset::haptics> read_rumble()
	{
		return {};
	}
};
#else
#include <linux/uinput.h>


class wivrn_uinput
{
public:
	// Devices are created lazily on first use, so only forwarded classes are exposed.
	wivrn_uinput() = default;

	void handle_input(wivrn::from_headset::hid::input &);

	// The gamepad device is created on the first packet with gamepad inputs
	void handle_gamepad(const wivrn::from_headset::inputs &);
	// Remove the virtual gamepad, when the headset stops mirroring or the gamepad disconnects
	void destroy_gamepad();

	// Drains pending force-feedback requests from the virtual gamepad and returns the
	// resulting haptics packets to forward to the headset, if any changed.
	std::vector<wivrn::to_headset::haptics> read_rumble();

private:
	void init_keyboard();
	void init_mouse();
	void init_gamepad();

	void send_key(uint16_t key, bool down);
	void send_button(uint16_t mouse_button, bool down);
	void mouse_move_relative(int16_t x, int16_t y);
	void mouse_scroll(int16_t vertical, int16_t horizontal);

	static constexpr int ff_effects_max = 16;

	wivrn::fd_base kbd_fd;
	wivrn::fd_base mouse_fd;
	wivrn::fd_base gamepad_fd;
	std::map<int16_t, ff_effect> ff_effects;
};
#endif
