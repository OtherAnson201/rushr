#include "targeting.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <random>
#include <thread>
#include <globals.h>
#include <game/game.h>
#include <render/assets/settings.h>
#include <sdk/math/math.h>
#include <sdk/sdk.h>
#include <memory/memory.h>
#include <Offsets/Offsets.hpp>
#include <render/keybind/keybind.h>
#include <render/render.h>
#include <cache/cache.h>
#include <visibility/visibility.h>
#include <mutex>

static float get_world_distance_to_entity(cache::entity_t& entity, cache::entity_t& local_player);

// true if the entity is mid-jump
static bool is_jumping(const cache::entity_t& entity)
{
	if (entity.humanoid.address == 0)
		return false;

	rbx::humanoid_t humanoid(entity.humanoid.address);
	int humanoid_state = static_cast<int>(humanoid.get_state());

	if (humanoid_state == 3 || humanoid_state == 5)
		return true;

	auto root_it = entity.parts.find("HumanoidRootPart");
	if (root_it == entity.parts.end() || !root_it->second.address)
		return false;

	try
	{
		std::uint64_t prim_address = memory->read<std::uint64_t>(root_it->second.address + Offsets::BasePart::Primitive);
		if (!prim_address)
			return false;

		math::vector3 velocity = memory->read<math::vector3>(prim_address + Offsets::Primitive::AssemblyLinearVelocity);
		return velocity.y > 5.0f;
	}
	catch (...)
	{
		return false;
	}
}

// the entitys velocity
static math::vector3 get_velocity(cache::entity_t& entity)
{
	auto root_it = entity.parts.find("HumanoidRootPart");
	if (root_it == entity.parts.end() || !root_it->second.address)
		root_it = entity.parts.find("Torso");
	if (root_it == entity.parts.end() || !root_it->second.address)
		root_it = entity.parts.find("Head");

	if (root_it == entity.parts.end() || !root_it->second.address)
	{
		return math::vector3(0.0f, 0.0f, 0.0f);
	}

	rbx::part_t part_copy = root_it->second;
	rbx::primitive_t prim = part_copy.get_primitive();

	if (!prim.address)
	{
		auto head_it = entity.parts.find("Head");
		if (head_it != entity.parts.end() && head_it->second.address)
		{
			part_copy = head_it->second;
			prim = part_copy.get_primitive();
		}

		if (!prim.address)
			return math::vector3(0.0f, 0.0f, 0.0f);
	}

	try
	{
		return memory->read<math::vector3>(prim.address + Offsets::Primitive::AssemblyLinearVelocity);
	}
	catch (...)
	{
		return math::vector3(0.0f, 0.0f, 0.0f);
	}
}

// lead the aim for the targets movement
static math::vector3 apply_prediction(cache::entity_t& entity, const math::vector3& position)
{
	math::vector3 velocity = get_velocity(entity);

	float prediction_factor_x;
	float prediction_factor_y;

	if (settings::targeting::air_prediction_enabled && is_jumping(entity))
	{
		prediction_factor_x = settings::targeting::air_prediction_x * 0.02f;
		prediction_factor_y = settings::targeting::air_prediction_y * 0.02f;
	}
	else
	{
		prediction_factor_x = settings::targeting::prediction_x * 0.02f;
		prediction_factor_y = settings::targeting::prediction_y * 0.02f;
	}

	math::vector3 predicted_offset;
	predicted_offset.x = velocity.x * prediction_factor_x;
	predicted_offset.y = velocity.y * prediction_factor_y;
	predicted_offset.z = velocity.z * prediction_factor_x;

	return position + predicted_offset;
}

// pixels from the crosshair to a point
static float get_distance_from_center(const math::vector2& point, const math::vector2& position)
{
	float dx{ position.x - point.x };
	float dy{ position.y - point.y };
	return std::sqrt(dx * dx + dy * dy);
}

static bool get_target_point_for_entity
(
	const cache::entity_t& entity,
	const math::matrix4& view,
	const math::vector2& dims,
	float fov,
	float& out_distance,
	math::vector2& out_screen
)
{

	math::vector2 cursor_pos{ dims.x * 0.5f + game::window_offset_x, dims.y * 0.5f + game::window_offset_y };
	{
		POINT cursor{};
		if (!GetCursorPos(&cursor))
		{
			return false;
		}

		cursor_pos = { static_cast<float>(cursor.x), static_cast<float>(cursor.y) };
	}

	float best_distance = FLT_MAX;
	bool found = false;

	auto try_part = [&](const char* name)
		{
			auto it = entity.parts.find(name);
			if (it == entity.parts.end())
			{
				return;
			}

			rbx::part_t part{ it->second };
			if (!part.address)
			{
				return;
			}

			rbx::primitive_t primitive = part.get_primitive();
			if (!primitive.address)
			{
				return;
			}

			math::vector3 pos_3d;
			try
			{
				pos_3d = primitive.get_position();
				if (std::isnan(pos_3d.x) || std::isnan(pos_3d.y) || std::isnan(pos_3d.z))
					return;
				if (pos_3d.x == 0.0f && pos_3d.y == 0.0f && pos_3d.z == 0.0f)
					return;
			}
			catch (...)
			{
				return;
			}

			if (settings::targeting::enable_prediction)
			{
				try
				{
					pos_3d = apply_prediction(const_cast<cache::entity_t&>(entity), pos_3d);
				}
				catch (...)
				{
				}
			}

			math::vector2 pos_2d{};

			if (!game::visengine.world_to_screen(pos_3d, pos_2d, dims, view))
			{
				return;
			}

			const float distance = get_distance_from_center(cursor_pos, pos_2d);

			if (distance > fov)
			{
				return;
			}

			if (distance < best_distance)
			{
				best_distance = distance;
				out_screen = pos_2d;
				found = true;
			}
		};

	int target_part_to_use = settings::targeting::target_part;
	if (settings::targeting::air_part_enabled && entity.humanoid.address != 0 && is_jumping(entity))
	{
		target_part_to_use = settings::targeting::air_part;
	}

	switch (target_part_to_use)
	{
	case 1:
		try_part("Head");
		if (!found) try_part("head");
		break;
	case 2:
		try_part("HumanoidRootPart");
		if (!found) try_part("Torso");
		if (!found) try_part("UpperTorso");
		if (!found) try_part("LowerTorso");
		break;

	case 3:
		try_part("LeftUpperArm");
		if (!found) try_part("LeftLowerArm");
		if (!found) try_part("Left Arm");
		break;
	case 4:
		try_part("RightUpperArm");
		if (!found) try_part("RightLowerArm");
		if (!found) try_part("Right Arm");
		break;
	case 5:
		try_part("LeftUpperLeg");
		if (!found) try_part("LeftLowerLeg");
		if (!found) try_part("Left Leg");
		break;
	case 6:
		try_part("RightUpperLeg");
		if (!found) try_part("RightLowerLeg");
		if (!found) try_part("Right Leg");
		break;
	default:
	{
		const char* common_parts[] = { "Head", "HumanoidRootPart" };
		for (const char* name : common_parts)
		{
			try_part(name);
		}
		break;
	}
	}

	if (!found)
	{
		return false;
	}

	out_distance = best_distance;
	return true;
}

// how much of the aim step to apply this frame
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
	POINT cursor{};
	if (!GetCursorPos(&cursor))
	{
		return;
	}

	float delta_x = target.x - static_cast<float>(cursor.x);
	float delta_y = target.y - static_cast<float>(cursor.y);
	const float distance = std::sqrt(delta_x * delta_x + delta_y * delta_y);

	static thread_local float remainder_x = 0.0f;
	static thread_local float remainder_y = 0.0f;

	if (distance < 3.f)
	{
		remainder_x = 0.0f;
		remainder_y = 0.0f;
		return;
	}

	static thread_local std::chrono::steady_clock::time_point last_pass{};
	auto now = std::chrono::steady_clock::now();
	float dt = std::chrono::duration<float>(now - last_pass).count();
	last_pass = now;

	if (dt > 0.1f)
	{
		dt = 0.015f;
	}

	remainder_x += delta_x * (1.0f - remaining_share(settings::targeting::sensitivity_x, dt));
	remainder_y += delta_y * (1.0f - remaining_share(settings::targeting::sensitivity_y, dt));

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

// run one mouse aim step at the target
static void mouse_targeting()
{
	if (targeting::player.instance.address == 0)
	{
		return;
	}

	math::matrix4 view = game::visengine.get_viewmatrix();
	math::vector2 dims = game::visengine.get_dimensions();

	cache::entity_t local_player_snapshot;
	{
		std::lock_guard<std::mutex> lock(cache::mtx);
		local_player_snapshot = cache::cached_local_player;
	}

	float world_distance = get_world_distance_to_entity(targeting::player, local_player_snapshot);
	if (world_distance == FLT_MAX)
	{
		return;
	}

	float distance{};
	math::vector2 target_screen{};

	if (get_target_point_for_entity(targeting::player, view, dims, FLT_MAX, distance, target_screen))
	{
		if (settings::targeting::offset_enabled)
		{
			target_screen.x += settings::targeting::offset_x;
			target_screen.y += settings::targeting::offset_y;
		}
		mouse_aim(target_screen);
	}
}

// world distance to an entity
static float get_world_distance_to_entity(cache::entity_t& entity, cache::entity_t& local_player)
{
	auto target_it = entity.parts.find("HumanoidRootPart");
	if (target_it == entity.parts.end() || !target_it->second.address)
		target_it = entity.parts.find("Torso");
	if (target_it == entity.parts.end() || !target_it->second.address)
		target_it = entity.parts.find("Head");

	if (target_it == entity.parts.end() || !target_it->second.address)
	{
		return FLT_MAX;
	}

	rbx::part_t target_part_copy = target_it->second;
	rbx::primitive_t target_primitive = target_part_copy.get_primitive();

	if (!target_primitive.address)
	{
		auto head_it = entity.parts.find("Head");
		if (head_it != entity.parts.end() && head_it->second.address)
		{
			target_part_copy = head_it->second;
			target_primitive = target_part_copy.get_primitive();
		}

		if (!target_primitive.address)
			return FLT_MAX;
	}

	math::vector3 target_pos;
	try
	{
		target_pos = target_primitive.get_position();
	}
	catch (...)
	{
		return FLT_MAX;
	}

	math::vector3 local_pos{};
	bool local_valid = false;

	auto local_it = local_player.parts.find("HumanoidRootPart");
	if (local_it != local_player.parts.end() && local_it->second.address)
	{
		rbx::part_t local_part_copy = local_it->second;
		rbx::primitive_t local_primitive = local_part_copy.get_primitive();
		if (local_primitive.address)
		{
			try
			{
				local_pos = local_primitive.get_position();
				local_valid = true;
			}
			catch (...) {}
		}
	}

	if (!local_valid)
	{
		std::uint64_t workspace = memory->read<std::uint64_t>(game::datamodel.address + Offsets::DataModel::Workspace);
		if (workspace != 0)
		{
			std::uint64_t camera = memory->read<std::uint64_t>(workspace + Offsets::Workspace::CurrentCamera);
			if (camera != 0)
			{
				try
				{
					local_pos = memory->read<math::vector3>(camera + Offsets::Camera::Position);
					local_valid = true;
				}
				catch (...) {}
			}
		}
	}

	if (!local_valid)
	{
		return FLT_MAX;
	}

	if (std::isnan(local_pos.x) || std::isnan(local_pos.y) || std::isnan(local_pos.z) ||
		std::isnan(target_pos.x) || std::isnan(target_pos.y) || std::isnan(target_pos.z))
		return FLT_MAX;

	if ((local_pos.x == 0.0f && local_pos.y == 0.0f && local_pos.z == 0.0f) ||
		(target_pos.x == 0.0f && target_pos.y == 0.0f && target_pos.z == 0.0f))
		return FLT_MAX;

	return local_pos.distance(target_pos);
}

// nearest player to the crosshair in view
static cache::entity_t get_closest_player(const math::matrix4& view, const math::vector2& dims)
{
	cache::entity_t best_player{};
	float closest = FLT_MAX;

	std::vector<cache::entity_t> entities_snapshot;
	cache::entity_t local_player_snapshot;
	{
		std::lock_guard<std::mutex> lock(cache::mtx);
		entities_snapshot = cache::cached_players;
		local_player_snapshot = cache::cached_local_player;
	}

	for (cache::entity_t& entity : entities_snapshot)
	{
		if (!entity.instance.address)
		{
			continue;
		}

		if (entity.instance.address == local_player_snapshot.instance.address)
		{
			continue;
		}

		if (!entity.target_allowed)
		{
			continue;
		}

		if (entity.humanoid.address != 0 && entity.health <= 0.0f)
		{
			continue;
		}

		if (local_player_snapshot.instance.address != 0)
		{
			float world_distance = get_world_distance_to_entity(entity, local_player_snapshot);
			if (world_distance == FLT_MAX)
			{
				continue;
			}
		}

		float distance{};
		math::vector2 target_screen{};
		if (!get_target_point_for_entity(entity, view, dims, settings::targeting::fov, distance, target_screen))
		{
			continue;
		}

		if (distance < closest)
		{
			closest = distance;
			best_player = entity;
		}
	}

	return best_player;
}

// true if the entity can still be targeted
static bool is_target_valid(const cache::entity_t& entity)
{
	if (entity.instance.address == 0)
	{
		return false;
	}

	cache::entity_t local_player_snapshot;
	{
		std::lock_guard<std::mutex> lock(cache::mtx);
		local_player_snapshot = cache::cached_local_player;
	}

	if (entity.instance.address == local_player_snapshot.instance.address)
	{
		return false;
	}

	return entity.target_allowed;
}

// background loop: aim the mouse at the closest player
void targeting::run()
{
	using namespace std::chrono_literals;

	keybind::keybind_t targeting_kb{};
	targeting_kb.key = settings::targeting::keybind;
	targeting_kb.mode = static_cast<keybind::activation_mode>(settings::targeting::activation_mode);

	std::uint64_t last_target = 0;
	std::chrono::steady_clock::time_point target_lost_at{};

	while (g_running)
	{
		if (!settings::targeting::enabled)
		{
			targeting::sticky_target = cache::entity_t{};
			Sleep(1);
			continue;
		}

		targeting_kb.key = settings::targeting::keybind;
		targeting_kb.mode = static_cast<keybind::activation_mode>(settings::targeting::activation_mode);

		const bool active = keybind::is_active(targeting_kb);
		const bool held_down = targeting_kb.mode == keybind::activation_mode::hold && targeting_kb.key != 0;

		if (!active || (render->running && !held_down))
		{
			targeting::sticky_target = cache::entity_t{};
			targeting::player = cache::entity_t{};
			last_target = 0;
			target_lost_at = {};
			Sleep(1);
			continue;
		}

		math::matrix4 view = game::visengine.get_viewmatrix();
		math::vector2 dims = game::visengine.get_dimensions();

		cache::entity_t player{};

		if (settings::targeting::sticky_aim && targeting::sticky_target.instance.address != 0)
		{
			cache::entity_t refreshed_target{};
			bool target_found = false;
			{
				std::lock_guard<std::mutex> lock(cache::mtx);
				for (const auto& entity : cache::cached_players)
				{
					if (entity.instance.address == targeting::sticky_target.instance.address)
					{
						refreshed_target = entity;
						target_found = true;
						break;
					}
				}
			}

			if (target_found)
			{
				if (refreshed_target.humanoid.address == 0 || is_target_valid(refreshed_target))
				{
					player = refreshed_target;
					targeting::sticky_target = refreshed_target;
				}
				else
				{
					targeting::sticky_target = refreshed_target;
					targeting::player = cache::entity_t{};
					Sleep(1);
					continue;
				}
			}
			else
			{
				targeting::sticky_target = cache::entity_t{};
				targeting::player = cache::entity_t{};
				Sleep(1);
				continue;
			}
		}
		else
		{
			player = get_closest_player(view, dims);

			if (player.instance.address == 0 && last_target != 0)
			{
				std::lock_guard<std::mutex> lock(cache::mtx);
				for (const auto& entity : cache::cached_players)
				{
					if (entity.instance.address == last_target)
					{
						if (entity.target_allowed && (entity.humanoid.address == 0 || entity.health > 0.0f))
						{
							player = entity;
						}
						break;
					}
				}
			}
			if (settings::targeting::sticky_aim && player.instance.address != 0)
			{
				targeting::sticky_target = player;
			}
		}

		if (last_target != 0 && player.instance.address != last_target)
		{
			bool previous_still_fine = false;
			{
				std::lock_guard<std::mutex> lock(cache::mtx);
				for (const auto& entity : cache::cached_players)
				{
					if (entity.instance.address == last_target)
					{
						previous_still_fine = entity.target_allowed && (entity.humanoid.address == 0 || entity.health > 0.0f);
						break;
					}
				}
			}

			if (previous_still_fine)
			{
				target_lost_at = {};
			}
			else
			{
				auto now = std::chrono::steady_clock::now();
				if (target_lost_at == std::chrono::steady_clock::time_point{})
				{
					target_lost_at = now;
				}

				const auto waited = std::chrono::duration<float, std::milli>(now - target_lost_at).count();
				if (waited < settings::targeting::switch_delay)
				{
					player = cache::entity_t{};
				}
			}
		}

		if (player.instance.address == 0)
		{
			targeting::player = cache::entity_t{};
			targeting::sticky_target = cache::entity_t{};
			Sleep(1);
			continue;
		}

		last_target = player.instance.address;
		target_lost_at = {};

		targeting::player = player;

		mouse_targeting();
		std::this_thread::sleep_for(std::chrono::milliseconds(1));
	}
}
