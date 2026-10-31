#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <thread>
#include <chrono>
#include <cstring>
#include <string>
#include <globals.h>
#include <logger/logger.h>
#include <game/game.h>
#include <render/render.h>
#include <render/assets/settings.h>
#include <memory/memory.h>
#include <cache/cache.h>
#include <features/targeting/targeting.h>

std::thread targeting_thread;
std::thread cache_thread;

std::atomic<bool> g_running{ true };

namespace {

	// start a named feature thread
	std::thread start_named_thread(const char* name, void (*entry)())
	{
		return std::thread([name, entry]
			{
				logger::name_thread(name);
				LOG_DEBUG("thread started");
				entry();
				LOG_DEBUG("thread exited");
			});
	}

	// log the periodic status line
	void log_status()
	{
		static std::atomic<std::uint64_t> last_status{ 0 };
		if (!logger::should_log(logger::level::info) || !logger::throttle(last_status, 10000))
			return;

		std::size_t player_count = 0;
		bool has_local_player = false;
		{
			std::lock_guard<std::mutex> lock(cache::mtx);
			player_count = cache::cached_players.size();
			has_local_player = cache::cached_local_player.instance.address != 0;
		}

		LOG_INFO("status: {} players cached, local player {}, players service 0x{:X}, workspace 0x{:X}, camera 0x{:X}, {:.0f} fps",
			player_count, has_local_player ? "found" : "missing", game::players.address, game::workspace.address, game::camera, ImGui::GetIO().Framerate);
	}

	// log when a menu setting changes
	void log_setting_changes()
	{
		struct watched_t
		{
			const char* name;
			bool* value;
			bool last;
		};

		static watched_t watched[] = {
			{ "menu", &render->running },
			{ "aimbot", &settings::targeting::enabled },
			{ "visuals", &settings::visuals::enabled },
		};

		static bool initialized = false;

		for (watched_t& entry : watched)
		{
			if (initialized && *entry.value != entry.last)
			{
				LOG_INFO("{} {}", entry.name, *entry.value ? "on" : "off");
			}
			entry.last = *entry.value;
		}

		initialized = true;
	}
}

namespace
{

	bool attached = false;

	const char* attached_name = "RobloxPlayerBeta.exe";
}

// attach to roblox and resolve pointers
bool initialize_memory(bool retrying)
{
	const logger::level problem = retrying ? logger::level::debug : logger::level::error;

	const char* process_name = "RobloxPlayerBeta.exe";
	if (!memory->attach_to_process(process_name))
	{
		process_name = "RobloxStudioBeta.exe";
		if (!memory->attach_to_process(process_name))
		{
			LOG_AT(problem, "could not attach to RobloxPlayerBeta.exe or RobloxStudioBeta.exe");
			return false;
		}
	}

	const std::uint64_t module_base = memory->find_module_address(process_name);
	if (module_base == 0)
	{
		LOG_AT(problem, "module {} not found in pid {}", process_name, memory->get_process_id());
		return false;
	}

	if (!game::reinitialize_game_state())
	{
		LOG_AT(problem, "{} is running (pid {}) but has no game loaded yet", process_name, memory->get_process_id());
		return false;
	}

	attached_name = process_name;
	game::update_window_offset();

	LOG_INFO("attached to {} (pid {}), module base 0x{:X}, size 0x{:X}", process_name, memory->get_process_id(), module_base, memory->get_module_size());
	return true;
}

// true if roblox has closed
static bool roblox_is_gone()
{
	using namespace std::chrono_literals;

	if (memory->process_ended())
		return true;

	static std::chrono::steady_clock::time_point last_look{};
	const auto now = std::chrono::steady_clock::now();
	if (now - last_look < 3s)
		return false;
	last_look = now;

	const std::uint32_t current = memory->get_process_id();
	if (memory_t::has_visible_window(current))
		return false;

	const std::uint32_t other = memory->find_process_id(attached_name);
	return other != 0 && other != current && memory_t::has_visible_window(other);
}

// launch every feature thread
void start_feature_threads()
{
	cache_thread = start_named_thread("cache", cache::run);
	targeting_thread = start_named_thread("aimbot", targeting::run);

	LOG_INFO("feature threads started");
}

// signal all feature threads to stop
void stop_feature_threads()
{

	static std::atomic<bool> stopping{ false };
	if (stopping.exchange(true))
		return;

	g_running = false;

	if (targeting_thread.joinable()) targeting_thread.join();
	if (cache_thread.joinable()) cache_thread.join();
}

// entry point: attach, start features, run the menu
std::int32_t main()
{
	using namespace std::chrono_literals;

	SetConsoleTitleA("Rushr");

	logger::initialize();

	attached = initialize_memory(false);
	if (!attached)
	{
		game::detach();
		LOG_WARN("no game to attach to yet, waiting for Roblox");
	}

	start_feature_threads();

	DisableProcessWindowsGhosting();

	{ /* setup overlay */
		if (!render->create_window())
		{
			LOG_FATAL("create_window failed (error {})", GetLastError());
			logger::hold_console();
			stop_feature_threads();
			std::exit(0);
		}

		if (!render->create_device())
		{
			LOG_FATAL("create_device failed");
			logger::hold_console();
			stop_feature_threads();
			std::exit(0);
		}
		if (!render->create_imgui())
		{
			LOG_FATAL("create_imgui failed");
			logger::hold_console();
			stop_feature_threads();
			std::exit(0);
		}
	}

	LOG_INFO("overlay ready ({}x{})", GetSystemMetrics(SM_CXSCREEN), GetSystemMetrics(SM_CYSCREEN));

	static bool ins_was_pressed = false;

	while (true)
	{
		render->start_render();

		log_status();
		log_setting_changes();

		{
			bool ins_down = (GetAsyncKeyState(VK_INSERT) & 0x8000) != 0;
			if (ins_down && !ins_was_pressed)
				render->running = !render->running;
			ins_was_pressed = ins_down;
		}

		static auto last_game_check = std::chrono::steady_clock::now();
		auto now = std::chrono::steady_clock::now();
		if (now - last_game_check >= (attached ? 500ms : 2000ms))
		{
			last_game_check = now;

			if (!attached)
			{
				attached = initialize_memory(true);
			}
			else if (roblox_is_gone())
			{
				LOG_INFO("Roblox is gone, waiting for it to come back");
				attached = false;
				game::detach();
				memory->detach();
			}
			else
			{

				const bool was_changed = game::game_changed;
				if (game::check_game_changed() && !was_changed)
				{
					LOG_INFO("the game changed, looking for the new one");
				}

				if (game::game_changed && !game::reinitialize_game_state())
				{
					LOG_EVERY(30000, logger::level::info, "the new game has not loaded yet, still looking");
				}
			}
		}

		const game::overlay_place place = game::order_overlay(render->detail->window);

		static game::overlay_place logged_place = game::overlay_place::hidden;
		static bool place_was_logged = false;
		if (!place_was_logged || place != logged_place)
		{
			place_was_logged = true;
			logged_place = place;
			LOG_INFO("overlay is {}",
				place == game::overlay_place::above_game ? "just above the game window, under whatever covers the game" :
				place == game::overlay_place::hidden ? "not drawn, the game is minimized" :
				"above every window, there is no ordinary game window to sit on");
		}

		const bool menu_drawn = place != game::overlay_place::hidden && render->running;

		if (place == game::overlay_place::hidden)
		{

			game::update_window_offset();
		}
		else
		{
			render->render_visuals();

			if (menu_drawn)
			{
				render->render_menu();
			}
		}

		render->update_input_passthrough(menu_drawn);

		render->end_render();
	}

	stop_feature_threads();
	return 0;
}
