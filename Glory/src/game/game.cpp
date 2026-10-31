#include "game.h"
#include <dwmapi.h>
#include <chrono>
#include <climits>
#include <cstring>
#include <mutex>
#include <logger/logger.h>
#include <memory/memory.h>
#include <Offsets/Offsets.hpp>

// find robloxs main window
HWND game::get_roblox_window()
{
	if (game::roblox_window && IsWindow(game::roblox_window))
	{
		return game::roblox_window;
	}

	struct search_t
	{
		DWORD process_id = 0;

		HWND titled = nullptr;
		HWND any = nullptr;
	};

	search_t search{};
	search.process_id = memory->get_process_id();

	if (search.process_id != 0)
	{
		EnumWindows([](HWND window, LPARAM lParam) -> BOOL
			{
				search_t& search = *reinterpret_cast<search_t*>(lParam);

				DWORD process_id = 0;
				GetWindowThreadProcessId(window, &process_id);

				if (process_id != search.process_id || !IsWindowVisible(window) || GetWindow(window, GW_OWNER) != nullptr)
					return TRUE;

				if (search.any == nullptr)
				{
					search.any = window;
				}

				char title[64]{};
				GetWindowTextA(window, title, sizeof(title));
				if (std::strcmp(title, "Roblox") == 0)
				{
					search.titled = window;
					return FALSE;
				}

				return TRUE;
			}, reinterpret_cast<LPARAM>(&search));
	}

	HWND hwnd = search.titled != nullptr ? search.titled : search.any;
	if (hwnd == nullptr)
	{
		hwnd = FindWindowA(nullptr, "Roblox");
	}

	if (hwnd)
	{
		game::roblox_window = hwnd;
	}

	return hwnd;
}

// true if the local player is typing
static bool read_typing()
{
	static std::uint32_t known_session = 0;
	static std::uint64_t input_service = 0;
	static std::chrono::steady_clock::time_point last_lookup{};

	if (game::datamodel.address == 0)
		return false;

	const auto now = std::chrono::steady_clock::now();
	if (game::session != known_session || (input_service == 0 && now - last_lookup >= std::chrono::seconds(2)))
	{
		known_session = game::session;
		last_lookup = now;
		input_service = game::datamodel.find_first_child_by_class("UserInputService").address;
	}

	if (input_service == 0)
		return false;

	auto looks_like_pointer = [](std::uint64_t value) { return value >= 0x10000 && value < 0x7FFFFFFFFFFFull; };

	const std::uint64_t input_state = memory->read<std::uint64_t>(input_service + Offsets::UserInputService::WindowInputState);
	if (!looks_like_pointer(input_state))
		return false;

	const std::uint64_t text_box = memory->read<std::uint64_t>(input_state + Offsets::WindowInputState::CurrentTextBox);
	if (!looks_like_pointer(text_box))
		return false;

	return rbx::instance_t{ text_box }.get_class_name() == "TextBox";
}

// true if the chat box has focus
bool game::typing_in_chat()
{
	static std::mutex mutex;
	static std::chrono::steady_clock::time_point checked{};
	static bool typing = false;

	std::lock_guard<std::mutex> lock(mutex);

	const auto now = std::chrono::steady_clock::now();
	if (now - checked < std::chrono::milliseconds(10))
		return typing;
	checked = now;

	const bool was = typing;
	typing = read_typing();

	if (typing != was)
	{
		LOG_INFO("{}", typing ? "typing in the game (chat or a text box): hotkeys are paused" : "done typing in the game: hotkeys work again");
	}
	return typing;
}

// refresh robloxs client area offset
void game::update_window_offset()
{
	HWND roblox_window = game::get_roblox_window();
	if (roblox_window)
	{
		RECT client_rect{};
		POINT client_pos{};
		if (GetClientRect(roblox_window, &client_rect))
		{
			client_pos.x = client_rect.left;
			client_pos.y = client_rect.top;
			ClientToScreen(roblox_window, &client_pos);
			game::window_offset_x = (float)client_pos.x;
			game::window_offset_y = (float)client_pos.y;

			const bool minimized = IsIconic(roblox_window) != FALSE;
			game::window_width = minimized ? 0.0f : (float)(client_rect.right - client_rect.left);
			game::window_height = minimized ? 0.0f : (float)(client_rect.bottom - client_rect.top);
		}
	}
	else
	{
		game::window_offset_x = 0.0f;
		game::window_offset_y = 0.0f;
		game::window_width = 0.0f;
		game::window_height = 0.0f;
	}
}

// true if a window covers the game
static bool covers_the_game(HWND window, HWND game_window)
{
	if (!IsWindowVisible(window) || IsIconic(window))
		return false;

	if (GetWindow(window, GW_OWNER) == game_window)
		return false;

	BOOL cloaked = FALSE;
	if (SUCCEEDED(DwmGetWindowAttribute(window, DWMWA_CLOAKED, &cloaked, sizeof(cloaked))) && cloaked)
		return false;

	return true;
}

// keep the overlay stacked above roblox
game::overlay_place game::order_overlay(HWND overlay)
{
	const UINT flags = SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE;

	const HWND roblox = get_roblox_window();

	if (roblox == nullptr || (GetWindowLongPtr(roblox, GWL_EXSTYLE) & WS_EX_TOPMOST))
	{
		SetWindowPos(overlay, HWND_TOPMOST, 0, 0, 0, 0, flags);
		return overlay_position = overlay_place::above_everything;
	}

	if (IsIconic(roblox) || !IsWindowVisible(roblox))
	{
		return overlay_position = overlay_place::hidden;
	}

	for (HWND above = GetWindow(roblox, GW_HWNDPREV); above != nullptr; above = GetWindow(above, GW_HWNDPREV))
	{
		if (above == overlay)
		{
			return overlay_position = overlay_place::above_game;
		}

		if (covers_the_game(above, roblox))
			break;
	}

	if (GetWindowLongPtr(overlay, GWL_EXSTYLE) & WS_EX_TOPMOST)
	{
		SetWindowPos(overlay, HWND_NOTOPMOST, 0, 0, 0, 0, flags);
	}

	const HWND above = GetWindow(roblox, GW_HWNDPREV);
	if (above != overlay)
	{
		if (above == nullptr || (GetWindowLongPtr(above, GWL_EXSTYLE) & WS_EX_TOPMOST))
		{
			SetWindowPos(overlay, HWND_TOP, 0, 0, 0, 0, flags);
		}
		else
		{
			SetWindowPos(overlay, above, 0, 0, 0, 0, flags);
		}
	}

	return overlay_position = overlay_place::above_game;
}

// find windows sitting over the game
void game::update_covering_windows(HWND overlay)
{
	covering_windows.clear();

	HWND roblox = get_roblox_window();
	if (roblox == nullptr)
		return;

	if (IsIconic(roblox))
	{
		covering_windows.push_back({ LONG_MIN / 2, LONG_MIN / 2, LONG_MAX / 2, LONG_MAX / 2 });
		return;
	}

	if (overlay_position == overlay_place::above_game)
		return;

	for (HWND window = GetTopWindow(nullptr); window != nullptr && window != roblox; window = GetWindow(window, GW_HWNDNEXT))
	{
		if (window == overlay || !IsWindowVisible(window) || IsIconic(window))
			continue;

		const LONG_PTR ex_style = GetWindowLongPtr(window, GWL_EXSTYLE);
		if (ex_style & WS_EX_TRANSPARENT)
			continue;

		BOOL cloaked = FALSE;
		if (SUCCEEDED(DwmGetWindowAttribute(window, DWMWA_CLOAKED, &cloaked, sizeof(cloaked))) && cloaked)
			continue;

		RECT rect{};
		if (!GetWindowRect(window, &rect) || rect.right - rect.left < 8 || rect.bottom - rect.top < 8)
			continue;

		covering_windows.push_back(rect);
	}
}

// true if a rect is hidden by another window
bool game::is_covered(float left, float top, float right, float bottom)
{
	for (const RECT& rect : covering_windows)
	{
		if (right >= rect.left && left <= rect.right && bottom >= rect.top && top <= rect.bottom)
			return true;
	}

	return false;
}

namespace
{

	// true if a value looks like a valid pointer
	bool is_pointer(std::uint64_t value)
	{
		return value >= 0x10000 && value < 0x7FFFFFFFFFFFull;
	}

	struct game_pointers_t
	{
		std::uint64_t visual_engine = 0;
		std::uint64_t datamodel = 0;
	};

	// walk the offset chain to the games objects
	game_pointers_t resolve_pointers()
	{
		game_pointers_t found;

		const std::uint64_t module_base = memory->get_module_address();
		if (module_base == 0)
			return found;

		const std::uint64_t visual_engine = memory->read<std::uint64_t>(module_base + Offsets::VisualEngine::Pointer);
		if (!is_pointer(visual_engine))
			return found;

		const std::uint64_t fake_datamodel = memory->read<std::uint64_t>(visual_engine + Offsets::VisualEngine::FakeDataModel);
		if (!is_pointer(fake_datamodel))
			return found;

		const std::uint64_t datamodel = memory->read<std::uint64_t>(fake_datamodel + Offsets::FakeDataModel::RealDataModel);
		if (!is_pointer(datamodel))
			return found;

		found.visual_engine = visual_engine;
		found.datamodel = datamodel;
		return found;
	}
}

// true if the player joined a different game
bool game::check_game_changed()
{

	const game_pointers_t now = resolve_pointers();

	bool changed = now.datamodel != game::datamodel.address || now.visual_engine != game::visengine.address;

	if (!changed && now.datamodel != 0)
	{
		const std::uint64_t job_id = memory->read<std::uint64_t>(now.datamodel + Offsets::DataModel::JobId);
		if (game::current_job_id == 0)
		{
			game::current_job_id = job_id;
		}
		else if (job_id != game::current_job_id)
		{
			changed = true;
		}
	}

	if (changed)
	{
		game::game_changed = true;
	}

	return changed;
}

// re-resolve everything after a game change
bool game::reinitialize_game_state()
{
	const game_pointers_t found = resolve_pointers();
	if (found.datamodel == 0)
	{
		LOG_DEBUG("no datamodel to be found yet (module base 0x{:X})", memory->get_module_address());
		return false;
	}

	const rbx::instance_t players_service = rbx::instance_t{ found.datamodel }.find_first_child_by_class("Players");
	if (players_service.address == 0)
	{
		LOG_DEBUG("datamodel 0x{:X} has no Players service yet", found.datamodel);
		return false;
	}

	game::visengine = rbx::visualengine_t{ found.visual_engine };
	game::datamodel = rbx::instance_t{ found.datamodel };
	game::players = players_service;

	game::workspace = rbx::instance_t{};
	game::local_player = rbx::instance_t{};
	game::camera = 0;

	game::current_job_id = 0;

	game::session++;
	game::game_changed = false;

	LOG_INFO("game found: visual engine 0x{:X}, datamodel 0x{:X}, players 0x{:X}", found.visual_engine, found.datamodel, players_service.address);
	return true;
}

// drop all game state
void game::detach()
{
	game::game_changed = true;

	game::visengine = rbx::visualengine_t{};
	game::datamodel = rbx::instance_t{};
	game::players = rbx::instance_t{};
	game::workspace = rbx::instance_t{};
	game::local_player = rbx::instance_t{};
	game::camera = 0;
	game::current_job_id = 0;

	game::roblox_window = nullptr;
}
