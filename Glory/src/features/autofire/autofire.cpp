#include "autofire.h"

#include <globals.h>
#include <logger/logger.h>
#include <game/game.h>
#include <cache/cache.h>
#include <memory/memory.h>
#include <Offsets/Offsets.hpp>
#include <render/assets/settings.h>
#include <render/keybind/keybind.h>
#include <render/render.h>
#include <sdk/sdk.h>
#include <sdk/math/math.h>
#include <visibility/visibility.h>
#include <cmath>
#include <mutex>
#include <thread>
#include <chrono>
#include <windows.h>
#include <random>

using namespace std::chrono_literals;

// true if an entity sits under the crosshair
static bool is_entity_in_crosshair(const cache::entity_t& entity, const math::matrix4& view, const math::vector2& dims, math::vector3* out_pos = nullptr)
{

	math::vector2 center_pos{ dims.x * 0.5f + game::window_offset_x, dims.y * 0.5f + game::window_offset_y };
	POINT cursor{};
	if (GetCursorPos(&cursor))
	{
		center_pos = { static_cast<float>(cursor.x), static_cast<float>(cursor.y) };
	}

	static const std::vector<const char*> head_parts = { "Head" };
	static const std::vector<const char*> torso_parts = { "HumanoidRootPart", "Torso", "UpperTorso", "LowerTorso" };
	static const std::vector<const char*> limb_parts = {
		"LeftUpperArm", "RightUpperArm", "LeftLowerArm", "RightLowerArm", "LeftHand", "RightHand",
		"LeftUpperLeg", "RightUpperLeg", "LeftLowerLeg", "RightLowerLeg", "LeftFoot", "RightFoot",
		"Left Arm", "Right Arm", "Left Leg", "Right Leg" };

	std::vector<const char*> parts_to_check;

	switch (settings::autofire::target_part)
	{
	case 1: // Head only
		parts_to_check = head_parts;
		break;
	case 2: // Torso only
		parts_to_check = torso_parts;
		break;
	case 3: // Limbs only
		parts_to_check = limb_parts;
		break;
	default: // All parts
		parts_to_check = head_parts;
		parts_to_check.insert(parts_to_check.end(), torso_parts.begin(), torso_parts.end());
		parts_to_check.insert(parts_to_check.end(), limb_parts.begin(), limb_parts.end());
		break;
	}

	for (const char* name : parts_to_check)
	{
		auto it = entity.parts.find(name);
		if (it == entity.parts.end())
			continue;

		rbx::part_t part{ it->second };
		if (!part.address)
			continue;

		rbx::primitive_t primitive = part.get_primitive();
		if (!primitive.address)
			continue;

		math::vector3 pos_3d;
		try
		{
			pos_3d = primitive.get_position();
			if (std::isnan(pos_3d.x) || std::isnan(pos_3d.y) || std::isnan(pos_3d.z))
				continue;
			if (pos_3d.x == 0.0f && pos_3d.y == 0.0f && pos_3d.z == 0.0f)
				continue;
		}
		catch (...)
		{
			continue;
		}

		math::vector2 pos_2d{};
		if (!game::visengine.world_to_screen(pos_3d, pos_2d, dims, view))
			continue;

		float dx = pos_2d.x - center_pos.x;
		float dy = pos_2d.y - center_pos.y;
		float distance = std::sqrt(dx * dx + dy * dy);

		bool inside_outline = false;
		{
			const math::vector3 size = primitive.get_size();
			const math::matrix3 rotation = primitive.get_rotation();

			float left = FLT_MAX, top = FLT_MAX, right = -FLT_MAX, bottom = -FLT_MAX;
			bool all_in_front = true;

			for (int i = 0; i < 8 && all_in_front; i++)
			{
				const math::vector3 corner{
					(i & 1 ? 0.5f : -0.5f) * size.x,
					(i & 2 ? 0.5f : -0.5f) * size.y,
					(i & 4 ? 0.5f : -0.5f) * size.z };

				math::vector2 screen{};
				if (!game::visengine.world_to_screen(pos_3d + rotation * corner, screen, dims, view))
				{
					all_in_front = false;
					break;
				}

				if (screen.x < left) left = screen.x;
				if (screen.x > right) right = screen.x;
				if (screen.y < top) top = screen.y;
				if (screen.y > bottom) bottom = screen.y;
			}

			inside_outline = all_in_front && center_pos.x >= left && center_pos.x <= right && center_pos.y >= top && center_pos.y <= bottom;
		}

		if (inside_outline || distance < settings::autofire::hit_radius)
		{
			if (out_pos)
				*out_pos = pos_3d;
			return true;
		}
	}

	return false;
}

// background loop: fire when an enemy is on the crosshair
void autofire::run()
{
	using namespace std::chrono_literals;

	// Pre-initialize random generator
	static std::random_device rd;
	static std::mt19937 gen(rd());
	static std::uniform_real_distribution<> dis(0.0, 1.0);

	auto last_shot_time = std::chrono::steady_clock::now();

	keybind::keybind_t trigger_kb{};

	float random_share = static_cast<float>(dis(gen));

	while (g_running)
	{
		if (!settings::autofire::enabled)
		{
			std::this_thread::sleep_for(100ms);
			continue;
		}

		trigger_kb.key = settings::autofire::keybind;
		trigger_kb.mode = static_cast<keybind::activation_mode>(settings::autofire::activation_mode);

		if (!keybind::is_active(trigger_kb))
		{
			std::this_thread::sleep_for(10ms);
			continue;
		}

		if (GetForegroundWindow() != game::get_roblox_window() || render->menu_has_mouse)
		{
			std::this_thread::sleep_for(10ms);
			continue;
		}

		// Safety checks
		if (game::visengine.address == 0 || game::datamodel.address == 0)
		{
			std::this_thread::sleep_for(50ms);
			continue;
		}

		std::uint64_t local_address = 0;
		{
			std::lock_guard<std::mutex> lock(cache::mtx);
			local_address = cache::cached_local_player.instance.address;
		}

		if (local_address == 0)
		{
			std::this_thread::sleep_for(10ms);
			continue;
		}

		// Check delay
		auto current_time = std::chrono::steady_clock::now();
		auto time_since_last = std::chrono::duration_cast<std::chrono::milliseconds>(current_time - last_shot_time).count();

		float delay = settings::autofire::delay;
		if (settings::autofire::randomize_delay)
		{
			delay = settings::autofire::delay_min + random_share * (settings::autofire::delay_max - settings::autofire::delay_min);
		}

		if (time_since_last < delay)
		{
			std::this_thread::sleep_for(1ms);
			continue;
		}

		// Get view data
		math::matrix4 view{};
		math::vector2 dims{};
		try
		{
			view = game::visengine.get_viewmatrix();
			dims = game::visengine.get_dimensions();
			if (dims.x == 0 || dims.y == 0)
			{
				std::this_thread::sleep_for(10ms);
				continue;
			}
		}
		catch (...)
		{
			std::this_thread::sleep_for(10ms);
			continue;
		}

		std::vector<cache::entity_t> entities_snapshot;
		{
			std::lock_guard<std::mutex> lock(cache::mtx);
			entities_snapshot = cache::cached_players;
		}

		if (entities_snapshot.empty())
		{
			std::this_thread::sleep_for(10ms);
			continue;
		}

		math::vector3 camera_pos{};
		bool has_camera_pos = false;
		if (settings::universal::wall_check && game::camera != 0)
		{
			camera_pos = memory->read<math::vector3>(game::camera + Offsets::Camera::Position);
			has_camera_pos = true;
		}

		// Find target
		bool target_found = false;
		for (const auto& entity : entities_snapshot)
		{
			if (entity.instance.address == 0)
				continue;
			if (entity.instance.address == local_address)
				continue;

			if (!entity.target_allowed)
				continue;
			if (entity.humanoid.address != 0 && entity.health <= 0.0f)
				continue;

			math::vector3 hit_pos{};
			if (is_entity_in_crosshair(entity, view, dims, &hit_pos))
			{
				if (has_camera_pos && !visibility->is_visible(camera_pos, hit_pos))
					continue;

				target_found = true;
				break;
			}
		}

		if (target_found)
		{
			INPUT input{};
			input.type = INPUT_MOUSE;
			input.mi.dwFlags = MOUSEEVENTF_LEFTDOWN;
			SendInput(1, &input, sizeof(INPUT));

			std::this_thread::sleep_for(std::chrono::milliseconds(20));

			input.mi.dwFlags = MOUSEEVENTF_LEFTUP;
			SendInput(1, &input, sizeof(INPUT));

			last_shot_time = std::chrono::steady_clock::now();
			random_share = static_cast<float>(dis(gen));
		}

		std::this_thread::sleep_for(1ms);
	}
}
