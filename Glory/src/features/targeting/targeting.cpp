#include "targeting.h"

#include <algorithm>
#include <cfloat>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <mutex>
#include <thread>
#include <vector>
#include <globals.h>
#include <game/game.h>
#include <render/assets/settings.h>
#include <sdk/math/math.h>
#include <sdk/sdk.h>
#include <render/keybind/keybind.h>
#include <render/render.h>
#include <cache/cache.h>

// pixels between two screen points
static float distance_between(const math::vector2& a, const math::vector2& b)
{
	const float dx = b.x - a.x;
	const float dy = b.y - a.y;
	return std::sqrt(dx * dx + dy * dy);
}

// screen position of one body part, false if its missing or off screen
static bool part_on_screen(const cache::entity_t& entity, const char* name, const math::matrix4& view, const math::vector2& dims, math::vector2& out)
{
	const auto it = entity.parts.find(name);
	if (it == entity.parts.end() || !it->second.address)
	{
		return false;
	}

	rbx::primitive_t primitive = it->second.get_primitive();
	if (!primitive.address)
	{
		return false;
	}

	const math::vector3 position = primitive.get_position();
	if (std::isnan(position.x) || std::isnan(position.y) || std::isnan(position.z))
	{
		return false;
	}
	if (position.x == 0.0f && position.y == 0.0f && position.z == 0.0f)
	{
		return false;
	}

	return game::visengine.world_to_screen(position, out, dims, view);
}

// where to aim on this player: 0 closest to the cursor, 1 head, 2 torso
static bool get_aim_point(const cache::entity_t& entity, const math::matrix4& view, const math::vector2& dims, const math::vector2& cursor, math::vector2& out)
{
	switch (settings::targeting::target_part)
	{
	case 1:
		return part_on_screen(entity, "Head", view, dims, out);
	case 2:
		return part_on_screen(entity, "HumanoidRootPart", view, dims, out)
			|| part_on_screen(entity, "Torso", view, dims, out)
			|| part_on_screen(entity, "UpperTorso", view, dims, out);
	default:
	{
		math::vector2 head{};
		math::vector2 root{};
		const bool has_head = part_on_screen(entity, "Head", view, dims, head);
		const bool has_root = part_on_screen(entity, "HumanoidRootPart", view, dims, root);
		if (!has_head && !has_root)
		{
			return false;
		}

		out = has_head && (!has_root || distance_between(cursor, head) <= distance_between(cursor, root)) ? head : root;
		return true;
	}
	}
}

// current cursor position
static bool get_cursor(math::vector2& out)
{
	POINT cursor{};
	if (!GetCursorPos(&cursor))
	{
		return false;
	}

	out = { static_cast<float>(cursor.x), static_cast<float>(cursor.y) };
	return true;
}

// how much of the aim step to leave for later this frame
static float remaining_share(float sensitivity, float dt)
{
	if (sensitivity >= 100.0f)
	{
		return 0.0f;
	}

	const float slack = (100.0f - (std::max)(sensitivity, 1.0f)) / 100.0f;
	const float time_constant = slack * slack * 0.5f;
	return std::exp(-dt / time_constant);
}

// move the mouse toward a screen point
static void mouse_aim(const math::vector2& target)
{
	math::vector2 cursor{};
	if (!get_cursor(cursor))
	{
		return;
	}

	const float delta_x = target.x - cursor.x;
	const float delta_y = target.y - cursor.y;

	static thread_local float remainder_x = 0.0f;
	static thread_local float remainder_y = 0.0f;

	if (std::sqrt(delta_x * delta_x + delta_y * delta_y) < 3.f)
	{
		remainder_x = 0.0f;
		remainder_y = 0.0f;
		return;
	}

	static thread_local std::chrono::steady_clock::time_point last_pass{};
	const auto now = std::chrono::steady_clock::now();
	float dt = std::chrono::duration<float>(now - last_pass).count();
	last_pass = now;

	if (dt > 0.1f)
	{
		dt = 0.015f;
	}

	const float share = 1.0f - remaining_share(settings::targeting::sensitivity, dt);
	remainder_x += delta_x * share;
	remainder_y += delta_y * share;

	const int move_x = static_cast<int>(std::round(remainder_x));
	const int move_y = static_cast<int>(std::round(remainder_y));

	if (move_x != 0 || move_y != 0)
	{
		INPUT input{};
		input.type = INPUT_MOUSE;
		input.mi.dx = move_x;
		input.mi.dy = move_y;
		input.mi.dwFlags = MOUSEEVENTF_MOVE;
		SendInput(1, &input, sizeof(INPUT));

		remainder_x -= static_cast<float>(move_x);
		remainder_y -= static_cast<float>(move_y);
	}
}

// player closest to the cursor inside the fov, anyone but the local player
static std::uint64_t pick_target(const math::matrix4& view, const math::vector2& dims, const math::vector2& cursor)
{
	std::vector<cache::entity_t> players;
	std::uint64_t local_player = 0;
	{
		std::lock_guard<std::mutex> lock(cache::mtx);
		players = cache::cached_players;
		local_player = cache::cached_local_player.instance.address;
	}

	std::uint64_t best = 0;
	float closest = settings::targeting::fov;

	for (const cache::entity_t& entity : players)
	{
		if (!entity.instance.address || entity.instance.address == local_player)
		{
			continue;
		}

		math::vector2 point{};
		if (!get_aim_point(entity, view, dims, cursor, point))
		{
			continue;
		}

		const float distance = distance_between(cursor, point);
		if (distance <= closest)
		{
			closest = distance;
			best = entity.instance.address;
		}
	}

	return best;
}

// latest copy of the locked player, false once they left the server
static bool find_player(std::uint64_t address, cache::entity_t& out)
{
	std::lock_guard<std::mutex> lock(cache::mtx);
	for (const cache::entity_t& entity : cache::cached_players)
	{
		if (entity.instance.address == address)
		{
			out = entity;
			return true;
		}
	}
	return false;
}

// main aim loop: lock onto a player and stay on them until the key is let go
void targeting::run()
{
	keybind::keybind_t targeting_kb{};
	std::uint64_t locked = 0;

	while (g_running)
	{
		targeting_kb.key = settings::targeting::keybind;
		targeting_kb.mode = static_cast<keybind::activation_mode>(settings::targeting::activation_mode);

		// with the menu open only a held key moves the mouse, or you couldnt reach the menu to turn it off
		const bool held_down = targeting_kb.mode == keybind::activation_mode::hold && targeting_kb.key != 0;

		if (!settings::targeting::enabled || !keybind::is_active(targeting_kb) || (render->running && !held_down))
		{
			locked = 0;
			Sleep(1);
			continue;
		}

		const math::matrix4 view = game::visengine.get_viewmatrix();
		const math::vector2 dims = game::visengine.get_dimensions();

		math::vector2 cursor{};
		if (!get_cursor(cursor))
		{
			Sleep(1);
			continue;
		}

		cache::entity_t target{};
		if (locked == 0 || !find_player(locked, target))
		{
			locked = pick_target(view, dims, cursor);
			if (locked == 0 || !find_player(locked, target))
			{
				locked = 0;
				Sleep(1);
				continue;
			}
		}

		math::vector2 point{};
		if (get_aim_point(target, view, dims, cursor, point))
		{
			mouse_aim(point);
		}

		std::this_thread::sleep_for(std::chrono::milliseconds(1));
	}
}
