#include "macros.h"

#include <globals.h>
#include <game/game.h>
#include <logger/logger.h>
#include <render/assets/settings.h>
#include <render/keybind/keybind.h>
#include <timing/precise_sleep.h>
#include <algorithm>
#include <chrono>
#include <cmath>
#include <thread>
#include <windows.h>

using namespace std::chrono_literals;

//       SendInput, c

//       Loop

//               break

// build a keyboard INPUT for a key up or down
static INPUT key_event(WORD key, bool down)
{
	INPUT input{};
	input.type = INPUT_KEYBOARD;
	input.ki.wVk = key;
	input.ki.wScan = static_cast<WORD>(MapVirtualKeyW(key, MAPVK_VK_TO_VSC));
	input.ki.dwFlags = down ? 0 : KEYEVENTF_KEYUP;
	return input;
}

// send one key up or down
static void send_key(WORD key, bool down)
{
	INPUT input = key_event(key, down);
	SendInput(1, &input, sizeof(INPUT));
}

// press then release a key
static void tap_key(WORD key)
{
	INPUT inputs[2] = { key_event(key, true), key_event(key, false) };
	SendInput(2, inputs, sizeof(INPUT));
}

// nudge the mouse sideways
static void move_mouse(LONG dx)
{
	INPUT input{};
	input.type = INPUT_MOUSE;
	input.mi.dx = dx;
	input.mi.dwFlags = MOUSEEVENTF_MOVE;
	SendInput(1, &input, sizeof(INPUT));
}

// how far to spin the camera this step
static LONG spin_step()
{

	constexpr double base_dpi = 800.0;
	constexpr double base_sensitivity = 0.36;

	const double dpi = std::clamp(settings::macros::dpi, settings::macros::dpi_min, settings::macros::dpi_max);
	const double sensitivity = std::clamp(settings::macros::sensitivity, settings::macros::sensitivity_min, settings::macros::sensitivity_max);

	const double step = settings::macros::spin * base_dpi * base_sensitivity / (dpi * sensitivity);
	return static_cast<LONG>(std::llround((std::min)(step, 1000000000.0)));
}

// run the glitch macro sequence once
static void run_glitch(precise_sleeper& sleeper)
{
	const LONG step = spin_step();
	macros::last_run = GetTickCount64();

	// SendInput, c
	tap_key('C');
	sleeper.sleep_ms(6.0);

	send_key(VK_SPACE, true);
	sleeper.sleep_ms(50.0);
	send_key(VK_SPACE, false);
	sleeper.sleep_ms(4.0);

	int moves = 0;
	const auto start = std::chrono::steady_clock::now();
	while (g_running && std::chrono::steady_clock::now() - start <= 200ms)
	{
		move_mouse(step);
		moves++;
		sleeper.sleep_ms(4.0);
	}

	LOG_DEBUG("glitch macro ran: {} mouse moves of {} counts", moves, step);
}

// background loop: fire the glitch macro while toggled
void macros::run()
{

	precise_sleeper sleeper;

	int watched_key = 0;
	bool was_down = false;

	while (g_running)
	{
		const int key = settings::macros::keybind;

		if (!settings::macros::enabled || key == 0)
		{
			watched_key = 0;
			std::this_thread::sleep_for(50ms);
			continue;
		}

		const bool down = keybind::key_down(key);

		if (key != watched_key)
		{
			watched_key = key;
			was_down = down;
		}

		const bool pressed = down && !was_down;
		was_down = down;

		if (pressed && GetForegroundWindow() == game::get_roblox_window())
		{
			run_glitch(sleeper);
		}

		sleeper.sleep_ms(1.0);
	}
}
