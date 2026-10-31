#include "clicker.h"

#include <globals.h>
#include <game/game.h>
#include <logger/logger.h>
#include <render/assets/settings.h>
#include <render/keybind/keybind.h>
#include <timing/precise_sleep.h>
#include <algorithm>
#include <chrono>
#include <cmath>
#include <string>
#include <thread>
#include <windows.h>

using namespace std::chrono_literals;

// going away.

namespace
{
	using steady = std::chrono::steady_clock;

	constexpr double idle_wait = 5.0;

	struct hotkey_t
	{

		bool were_down = true;

		int key = 0;
		bool key_was_down = true;

		// true while the clicker key is held
		bool pressed()
		{

			const bool down = keybind::key_down(VK_SHIFT) && keybind::key_down('G');
			const bool went_down = down && !were_down;
			were_down = down;

			const int bound = settings::clicker::keybind;
			if (bound != key)
			{
				key = bound;
				key_was_down = true;
			}

			const bool key_down = keybind::key_down(key);
			const bool key_went_down = key_down && !key_was_down;
			key_was_down = key_down;

			return went_down || key_went_down;
		}
	};

	// true if the window belongs to this program
	bool is_own(HWND window)
	{
		if (window == nullptr)
			return false;

		if (window == GetConsoleWindow())
			return true;

		DWORD process = 0;
		GetWindowThreadProcessId(window, &process);
		return process == GetCurrentProcessId();
	}

	// true if roblox is the window in front
	bool at_the_game(HWND roblox)
	{
		const HWND front = GetForegroundWindow();
		return front != nullptr && (front == roblox || is_own(front));
	}

	// readable label for the log line
	std::string describe(HWND window)
	{
		if (window == nullptr)
			return "no window";

		if (is_own(window))
			return "this program's own window";

		char title[96]{};
		char class_name[96]{};
		GetWindowTextA(window, title, sizeof(title));
		GetClassNameA(window, class_name, sizeof(class_name));
		return std::string("'") + title + "' (" + class_name + ")";
	}

	enum class hold_t
	{

		none,
		no_game,

		not_in_front,

		covered,

		off_view
	};

	// what is holding the clicker key down, if anything
	hold_t held_by(HWND roblox)
	{
		if (roblox == nullptr)
			return hold_t::no_game;

		if (!at_the_game(roblox))
			return hold_t::not_in_front;

		POINT cursor{};
		if (!GetCursorPos(&cursor))
			return hold_t::covered;

		const HWND under = WindowFromPoint(cursor);
		if (under == nullptr || GetAncestor(under, GA_ROOT) != roblox)
			return hold_t::covered;

		RECT view{};
		if (!ScreenToClient(roblox, &cursor) || !GetClientRect(roblox, &view) || !PtInRect(&view, cursor))
			return hold_t::off_view;

		return hold_t::none;
	}

	// readable label for the log line
	std::string describe(hold_t hold)
	{
		POINT cursor{};

		switch (hold)
		{
		case hold_t::no_game:
			return "there is no game window";
		case hold_t::not_in_front:
			return "the window in front is " + describe(GetForegroundWindow()) + ", not the game";
		case hold_t::covered:
			return GetCursorPos(&cursor) ? "under the cursor is " + describe(GetAncestor(WindowFromPoint(cursor), GA_ROOT)) + ", not the game" : "the cursor cannot be found";
		case hold_t::off_view:
			return "the cursor is on the game's title bar or border";
		default:
			return "nothing";
		}
	}

	// send one left click
	bool send_click()
	{
		INPUT inputs[2]{};
		inputs[0].type = INPUT_MOUSE;
		inputs[0].mi.dwFlags = MOUSEEVENTF_LEFTDOWN;
		inputs[0].mi.dwExtraInfo = clicker::click_tag;
		inputs[1] = inputs[0];
		inputs[1].mi.dwFlags = MOUSEEVENTF_LEFTUP;

		return SendInput(2, inputs, sizeof(INPUT)) == 2;
	}

	// the configured click rate
	int clicks_a_second()
	{
		return std::clamp(settings::clicker::clicks_per_second, settings::clicker::clicks_per_second_min, settings::clicker::clicks_per_second_max);
	}
}

// background loop: auto click while held and on the game
void clicker::run()
{
	precise_sleeper sleeper;
	hotkey_t hotkey;

	bool running = false;
	steady::time_point since{};

	HWND window = nullptr;

	steady::time_point due{};
	int told_rate = 0;

	std::uint64_t clicks = 0;
	std::uint64_t counted = 0;
	steady::time_point counted_at{};

	int told = -1;
	steady::time_point told_at{};

	auto stop = [&](const char* why)
	{
		on = false;
		running = false;
		clicking = false;
		clicks_per_second = 0;

		LOG_INFO("auto clicker off after {:.1f} s and {} clicks: {}", std::chrono::duration<double>(steady::now() - since).count(), clicks, why);
	};

	while (g_running)
	{
		bool by_key = false;

		if (hotkey.pressed())
		{
			if (at_the_game(game::get_roblox_window()))
			{
				on = !on;
				by_key = true;
			}
			else
			{
				LOG_INFO("auto clicker: its key was pressed while the window in front was {}, neither the game nor this program, so it was left alone", describe(GetForegroundWindow()));
			}
		}

		const int rate = clicks_a_second();

		if (on != running)
		{
			const char* by = by_key ? "its key" : "the box in the menu";

			if (running)
			{
				stop(by);
			}
			else
			{
				running = true;
				since = steady::now();
				window = nullptr;
				clicks = 0;
				told = -1;
				told_rate = rate;

				LOG_INFO("auto clicker on, by {}: {} left clicks a second while the cursor is on the game", by, rate);
			}
		}

		if (!running)
		{
			sleeper.sleep_ms(idle_wait);
			continue;
		}

		if (rate != told_rate)
		{
			told_rate = rate;
			LOG_EVERY(1000, logger::level::info, "auto clicker: the speed is now {} clicks a second", rate);
		}

		const HWND roblox = game::get_roblox_window();
		if (window == nullptr)
		{
			window = roblox;
		}
		else if (roblox != window)
		{
			stop("the game window it was clicking in is gone");
			continue;
		}

		const steady::time_point now = steady::now();
		const hold_t hold = held_by(window);

		if (static_cast<int>(hold) != told && (told == -1 || now - told_at >= 1s))
		{
			told = static_cast<int>(hold);
			told_at = now;

			if (hold == hold_t::none)
			{
				LOG_INFO("auto clicker clicking");
			}
			else
			{
				LOG_INFO("auto clicker waiting: {}", describe(hold));
			}
		}

		if (hold != hold_t::none)
		{
			clicking = false;
			clicks_per_second = 0;
			sleeper.sleep_ms(idle_wait);
			continue;
		}

		if (!clicking)
		{

			clicking = true;
			due = now;
			counted = clicks;
			counted_at = now;
		}
		else if (now - counted_at >= 1s)
		{
			clicks_per_second = static_cast<int>(std::llround((clicks - counted) / std::chrono::duration<double>(now - counted_at).count()));
			counted = clicks;
			counted_at = now;
		}

		if (now >= due)
		{
			if (send_click())
			{
				clicks++;
			}
			else
			{
				LOG_EVERY(5000, logger::level::warn, "auto clicker: Windows did not take the click (error {})", GetLastError());
			}

			due = (std::max)(due + steady::duration{ 1s } / rate, steady::now());
		}

		const double left = std::chrono::duration<double, std::milli>(due - steady::now()).count();
		if (left > 0.0)
		{
			sleeper.sleep_ms((std::min)(left, idle_wait));
		}
	}

	clicking = false;
}
