#include "correction.h"

#include <globals.h>
#include <memory>
#include <logger/logger.h>
#include <game/game.h>
#include <cache/cache.h>
#include <memory/memory.h>
#include <Offsets/Offsets.hpp>
#include <render/assets/settings.h>
#include <render/keybind/keybind.h>
#include <sdk/sdk.h>
#include <sdk/math/math.h>
#include <visibility/visibility.h>
#include <algorithm>
#include <cmath>
#include <mutex>
#include <random>
#include <string>
#include <thread>
#include <chrono>
#include <utility>
#include <vector>
#include <windows.h>

struct random_pick_t
{

	std::uint64_t player = 0;
	std::string part;
	std::chrono::steady_clock::time_point drawn_at{};
};

static random_pick_t random_pick;

static const char* const body_parts[] = {
	"Head",
	"UpperTorso", "LowerTorso",
	"LeftUpperArm", "LeftLowerArm", "LeftHand", "RightUpperArm", "RightLowerArm", "RightHand",
	"LeftUpperLeg", "LeftLowerLeg", "LeftFoot", "RightUpperLeg", "RightLowerLeg", "RightFoot",
	"Torso", "Left Arm", "Right Arm", "Left Leg", "Right Leg" };

struct sight_t
{
	math::matrix4 view{};
	math::vector2 dims{};

	math::vector2 aim{};

	math::vector3 camera{};
	bool has_camera = false;

	math::vector3 own_head{};
	bool has_own_head = false;
};

// true if the vector is a sane world position
static bool is_position(const math::vector3& position)
{
	if (!std::isfinite(position.x) || !std::isfinite(position.y) || !std::isfinite(position.z))
		return false;

	return position.x != 0.0f || position.y != 0.0f || position.z != 0.0f;
}

// where on screen the shot is aimed
static math::vector2 read_aim(const math::vector2& dims)
{
	POINT cursor{};
	if (GetCursorPos(&cursor))
	{
		return { static_cast<float>(cursor.x), static_cast<float>(cursor.y) };
	}

	return { dims.x * 0.5f + game::window_offset_x, dims.y * 0.5f + game::window_offset_y };
}

// snapshot of the camera and view this frame
static sight_t read_sight()
{
	sight_t sight;
	sight.view = game::visengine.get_viewmatrix();
	sight.dims = game::visengine.get_dimensions();
	sight.aim = read_aim(sight.dims);

	if (game::camera != 0)
	{
		sight.camera = memory->read<math::vector3>(game::camera + Offsets::Camera::Position);
		sight.has_camera = is_position(sight.camera);
	}

	rbx::part_t head{};
	std::uint64_t humanoid = 0;
	{
		std::lock_guard<std::mutex> lock(cache::mtx);
		const auto found = cache::cached_local_player.parts.find("Head");
		if (found != cache::cached_local_player.parts.end())
		{
			head = found->second;
		}
		humanoid = cache::cached_local_player.humanoid.address;
	}

	const bool seated = humanoid != 0 && memory->read<std::uint64_t>(humanoid + Offsets::Humanoid::SeatPart) != 0;
	if (head.address != 0 && !seated)
	{
		rbx::primitive_t primitive = head.get_primitive();
		if (primitive.address != 0)
		{
			sight.own_head = primitive.get_position();
			sight.has_own_head = is_position(sight.own_head);
		}
	}

	return sight;
}

// true if a bullet could reach that point
static bool bullet_reaches(const sight_t& sight, const math::vector3& point)
{
	return !sight.has_own_head || visibility->is_visible(sight.own_head, point);
}

// true if nothing blocks the shot to the point
static bool has_clear_shot(const sight_t& sight, const math::vector3& point)
{
	if (!settings::universal::wall_check)
		return true;

	if (sight.has_camera && !visibility->is_visible(sight.camera, point))
		return false;

	return bullet_reaches(sight, point);
}

// true if a body part is visible and hittable
static bool is_part_visible(const cache::entity_t& entity, const char* name, const sight_t& sight)
{
	auto it = entity.parts.find(name);
	if (it == entity.parts.end() || it->second.address == 0)
		return false;

	rbx::primitive_t primitive = it->second.get_primitive();
	if (primitive.address == 0)
		return false;

	const math::vector3 position = primitive.get_position();
	if (!is_position(position))
		return false;

	math::vector2 screen{};
	if (!game::visengine.world_to_screen(position, screen, sight.dims, sight.view))
		return false;
	if (screen.x < game::window_offset_x || screen.x > game::window_offset_x + sight.dims.x ||
		screen.y < game::window_offset_y || screen.y > game::window_offset_y + sight.dims.y)
		return false;

	if (!visibility->is_visible(sight.camera, position))
		return false;

	return !settings::universal::wall_check || bullet_reaches(sight, position);
}

// pick a random visible part to aim at
static bool choose_random_part(const cache::entity_t& player, const sight_t& sight)
{
	using namespace std::chrono_literals;

	constexpr auto keep_for = 250ms;

	if (!sight.has_camera)
		return false;

	const auto now = std::chrono::steady_clock::now();

	if (random_pick.player == player.instance.address && !random_pick.part.empty() && now - random_pick.drawn_at < keep_for &&
		is_part_visible(player, random_pick.part.c_str(), sight))
	{
		return true;
	}

	std::vector<const char*> visible;
	for (const char* name : body_parts)
	{
		if (is_part_visible(player, name, sight))
		{
			visible.push_back(name);
		}
	}

	if (visible.empty())
	{
		random_pick = {};
		return false;
	}

	static std::mt19937 generator{ std::random_device{}() };
	std::uniform_int_distribution<std::size_t> draw(0, visible.size() - 1);

	random_pick.player = player.instance.address;
	random_pick.part = visible[draw(generator)];
	random_pick.drawn_at = now;
	return true;
}

// nothing read them.
static math::vector3 apply_prediction(const cache::entity_t& entity, const math::vector3& position)
{
	auto root = entity.parts.find("HumanoidRootPart");
	if (root == entity.parts.end() || root->second.address == 0)
		return position;

	const rbx::primitive_t primitive = root->second.get_primitive();
	if (primitive.address == 0)
		return position;

	const math::vector3 velocity = memory->read<math::vector3>(primitive.address + Offsets::Primitive::AssemblyLinearVelocity);
	if (!std::isfinite(velocity.x) || !std::isfinite(velocity.y) || !std::isfinite(velocity.z))
		return position;

	const float horizontal = settings::correction::prediction_x * 0.02f;
	const float vertical = settings::correction::prediction_y * 0.02f;

	return position + math::vector3{ velocity.x * horizontal, velocity.y * vertical, velocity.z * horizontal };
}

// pixels from the crosshair to a point
static float get_distance_from_center(const math::vector2& point, const math::vector2& position)
{
	float dx = position.x - point.x;
	float dy = position.y - point.y;
	return std::sqrt(dx * dx + dy * dy);
}

static bool get_target_point_for_entity
(
	const cache::entity_t& entity,
	const sight_t& sight,
	float fov,
	bool aiming,
	float& out_distance,
	math::vector2& out_screen,
	math::vector3& out_world_pos
)
{
	float best_distance = FLT_MAX;
	bool found = false;

	auto try_part = [&](const char* name)
	{
		auto it = entity.parts.find(name);
		if (it == entity.parts.end())
			return;

		rbx::part_t part{ it->second };
		if (!part.address)
			return;

		rbx::primitive_t primitive = part.get_primitive();
		if (!primitive.address)
			return;

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

		if (settings::correction::enable_prediction)
		{
			pos_3d = apply_prediction(entity, pos_3d);
		}

		math::vector2 pos_2d{};
		if (!game::visengine.world_to_screen(pos_3d, pos_2d, sight.dims, sight.view))
			return;

		const float distance = get_distance_from_center(sight.aim, pos_2d);

		if (distance > fov)
			return;

		if (distance < best_distance)
		{
			best_distance = distance;
			out_screen = pos_2d;
			out_world_pos = pos_3d;
			found = true;
		}
	};

	switch (settings::correction::target_part)
	{
	case 1:
		try_part("Head");
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
	case settings::correction::random_part:

		if (aiming && random_pick.player == entity.instance.address && !random_pick.part.empty())
		{
			try_part(random_pick.part.c_str());
		}
		else
		{
			try_part("Head");
			try_part("HumanoidRootPart");
		}
		break;
	default:

		try_part("Head");
		try_part("HumanoidRootPart");
		break;
	}

	if (!found)
		return false;

	out_distance = best_distance;
	return true;
}

// true if the target is close enough
static bool is_within_reach(const cache::entity_t& entity, const sight_t& sight, const math::vector3& point)
{
	if (settings::correction::target_part == settings::correction::random_part)
		return choose_random_part(entity, sight);

	return has_clear_shot(sight, point);
}

// nearest valid player to the crosshair
static cache::entity_t get_closest_player(const sight_t& sight)
{
	struct candidate_t
	{
		float distance;
		const cache::entity_t* entity;
		math::vector3 point;
	};

	std::vector<candidate_t> candidates;

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
			continue;

		if (entity.instance.address == local_player_snapshot.instance.address)
			continue;

		if (!entity.target_allowed)
			continue;

		if (entity.humanoid.address != 0 && entity.health <= 0.0f)
			continue;

		float distance{};
		math::vector2 target_screen{};
		math::vector3 target_world{};
		if (!get_target_point_for_entity(entity, sight, settings::correction::fov, false, distance, target_screen, target_world))
			continue;

		candidates.push_back({ distance, &entity, target_world });
	}

	std::sort(candidates.begin(), candidates.end(), [](const candidate_t& a, const candidate_t& b) { return a.distance < b.distance; });

	for (const candidate_t& candidate : candidates)
	{
		if (is_within_reach(*candidate.entity, sight, candidate.point))
		{
			return *candidate.entity;
		}
	}

	return {};
}

// resolve an entity from a locked address
static cache::entity_t find_valid_target(std::uint64_t address)
{
	std::lock_guard<std::mutex> lock(cache::mtx);
	for (const cache::entity_t& entity : cache::cached_players)
	{
		if (entity.instance.address != address)
			continue;

		if (entity.target_allowed && (entity.humanoid.address == 0 || entity.health > 0.0f))
			return entity;

		break;
	}

	return {};
}

// to cross it.
static cache::entity_t pick_target(std::uint64_t locked, const sight_t& sight)
{
	if (settings::correction::sticky_aim && locked != 0)
	{
		const cache::entity_t player = find_valid_target(locked);

		float distance{};
		math::vector2 screen{};
		math::vector3 world{};
		if (player.instance.address != 0 &&
			get_target_point_for_entity(player, sight, settings::correction::fov, false, distance, screen, world) &&
			is_within_reach(player, sight, world))
		{
			return player;
		}
	}

	return get_closest_player(sight);
}

// redirect the shot onto this player
static bool aim_at(const cache::entity_t& player)
{
	sight_t sight;
	sight.view = game::visengine.get_viewmatrix();
	sight.dims = game::visengine.get_dimensions();

	sight.aim = read_aim(sight.dims);

	math::vector2 target_screen{};
	math::vector3 target_world{};
	float distance{};

	if (!get_target_point_for_entity(player, sight, FLT_MAX, true, distance, target_screen, target_world))
		return false;

	static std::uint32_t known_session = 0;
	static std::uint64_t mouse_service = 0;
	if (game::session != known_session || mouse_service == 0)
	{
		known_session = game::session;
		mouse_service = game::datamodel.find_first_child_by_class("MouseService").address;
	}

	if (mouse_service == 0)
		return false;

	const float game_x = target_screen.x - game::window_offset_x;
	const float game_y = target_screen.y - game::window_offset_y;

	if (settings::correction::spoof_mouse)
	{
		rbx::c_silent_help silent_helper{ mouse_service };
		silent_helper.write_mouse_position(mouse_service, game_x, game_y);
	}
	else if (game::camera != 0)
	{
		rbx::camera_t camera_obj{ game::camera };
		math::vector2 center_pos{ sight.dims.x * 0.5f, sight.dims.y * 0.5f };
		math::vector2int16 viewport = camera_obj.CalculateViewport({ game_x, game_y }, sight.dims, center_pos);
		camera_obj.set_viewport(viewport);
	}

	return true;
}

// background loop: bend shots onto a target
void correction::run()
{
	using namespace std::chrono_literals;

	keybind::keybind_t silent_kb{};
	silent_kb.key = settings::correction::keybind;
	silent_kb.mode = static_cast<keybind::activation_mode>(settings::correction::activation_mode);

	std::uint64_t locked_target = 0;

	std::uint64_t last_target = 0;
	std::chrono::steady_clock::time_point target_lost_at{};

	while (g_running)
	{
		if (!settings::correction::enabled)
		{
			locked_target = 0;
			std::this_thread::sleep_for(10ms);
			continue;
		}

		silent_kb.key = settings::correction::keybind;
		silent_kb.mode = static_cast<keybind::activation_mode>(settings::correction::activation_mode);

		if (!keybind::is_active(silent_kb))
		{
			locked_target = 0;
			last_target = 0;
			target_lost_at = {};
			std::this_thread::sleep_for(10ms);
			continue;
		}

		cache::entity_t player = pick_target(locked_target, read_sight());

		if (last_target != 0 && player.instance.address != last_target)
		{
			if (find_valid_target(last_target).instance.address != 0)
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

				if (std::chrono::duration<float, std::milli>(now - target_lost_at).count() < settings::correction::switch_delay)
				{
					player = cache::entity_t{};
				}
			}
		}

		locked_target = player.instance.address;

		if (player.instance.address == 0)
		{
			std::this_thread::sleep_for(1ms);
			continue;
		}

		last_target = player.instance.address;
		target_lost_at = {};

		// picked again.
		const auto until = std::chrono::steady_clock::now() + 8ms;
		do
		{
			if (!aim_at(player))
			{

				std::this_thread::sleep_for(1ms);
				break;
			}
		} while (g_running && std::chrono::steady_clock::now() < until);
	}
}
