#include "mover.h"

#include <globals.h>
#include <game/game.h>
#include <logger/logger.h>
#include <cache/cache.h>
#include <memory/memory.h>
#include <Offsets/Offsets.hpp>
#include <render/assets/settings.h>
#include <render/render.h>
#include <render/keybind/keybind.h>
#include <features/stabilizer/stabilizer.h>
#include <features/netblock/netblock.h>
#include <sdk/sdk.h>
#include <sdk/math/math.h>
#include <timing/precise_sleep.h>
#include <algorithm>
#include <atomic>
#include <cmath>
#include <mutex>
#include <string>
#include <thread>
#include <chrono>
#include <vector>
#include <unordered_set>
#include <windows.h>

using namespace std::chrono_literals;

static struct {
	keybind::keybind_t speedhack;
	keybind::keybind_t noclip;
	keybind::keybind_t jumppower;
	keybind::keybind_t teleport;
	keybind::keybind_t freecam;
} mover_keybinds;

// set a keybinds key and mode
static void set_bind(keybind::keybind_t& bind, int key, int mode)
{
	if (bind.key != key)
	{
		bind.key = key;

		bind.was_pressed = true;
	}

	bind.mode = static_cast<keybind::activation_mode>(mode);
}

// sync keybinds from the menu settings
static void update_keybind_settings()
{
	set_bind(mover_keybinds.speedhack, settings::mover::speedhack::keybind, settings::mover::speedhack::activation_mode);
	set_bind(mover_keybinds.noclip, settings::mover::noclip::keybind, settings::mover::noclip::activation_mode);
	set_bind(mover_keybinds.jumppower, settings::mover::jumppower::keybind, settings::mover::jumppower::activation_mode);
	set_bind(mover_keybinds.teleport, settings::mover::teleport::keybind, settings::mover::teleport::activation_mode);
	set_bind(mover_keybinds.freecam, settings::mover::freecam::keybind, settings::mover::freecam::activation_mode);
}

// true if roblox is the focused window
static bool is_roblox_focused()
{
	HWND foreground = GetForegroundWindow();
	if (foreground == render->detail->window) return true;
	return foreground == game::get_roblox_window();
}

// true if a keybind is firing right now
static bool is_keybind_active(keybind::keybind_t& kb)
{

	if (kb.mode == keybind::activation_mode::always || kb.key == 0)
		return is_roblox_focused();

	bool pressed = keybind::key_down(kb.key);

	if (kb.mode == keybind::activation_mode::hold)
		return pressed && is_roblox_focused();

	if (kb.mode == keybind::activation_mode::toggle)
	{
		if (pressed && !kb.was_pressed)
		{
			kb.state = !kb.state;
			kb.was_pressed = true;
		}
		else if (!pressed)
		{
			kb.was_pressed = false;
		}
		return kb.state && is_roblox_focused();
	}

	return false;
}

// true if the vector has no nan or inf
static bool is_finite(const math::vector3& v)
{
	return std::isfinite(v.x) && std::isfinite(v.y) && std::isfinite(v.z);
}

struct local_character_t
{
	std::uint64_t model = 0;
	std::uint64_t root = 0;
	std::uint64_t humanoid = 0;
};

// grab the local players character parts
static local_character_t get_local_character()
{
	static local_character_t character;
	static std::chrono::steady_clock::time_point last_lookup{};

	std::uint64_t model = 0;
	if (game::local_player.address != 0)
	{
		model = memory->read<std::uint64_t>(game::local_player.address + Offsets::Player::ModelInstance);
	}

	if (model == 0)
	{
		character = {};
		return character;
	}

	const bool complete = character.root != 0 && character.humanoid != 0;
	auto now = std::chrono::steady_clock::now();

	if (model != character.model || (!complete && now - last_lookup >= 250ms))
	{
		last_lookup = now;
		character = {};
		character.model = model;

		for (const rbx::instance_t& child : rbx::model_instance_t{ model }.get_children())
		{
			if (character.humanoid == 0 && child.get_class_name() == "Humanoid")
			{
				character.humanoid = child.address;
			}
			else if (character.root == 0 && child.get_name() == "HumanoidRootPart")
			{
				character.root = child.address;
			}
		}
	}

	return character;
}

enum class held_back_t
{
	speedhack,
	jump_power,
	fly
};

// clamp a speed to the configured cap
static float within_top_speed(float speed, held_back_t what)
{
	if (!settings::mover::stabilizer::enabled)
		return speed;

	const float top = stabilizer::top_speed();
	if (!(speed > top))
		return speed;

	struct told_t
	{
		float speed = 0.0f;
		float top = 0.0f;
		std::chrono::steady_clock::time_point at{};
	};

	static told_t told[3];
	static const char* const names[3] = { "speedhack", "jump power", "fly speed" };

	told_t& last = told[static_cast<int>(what)];
	const auto now = std::chrono::steady_clock::now();

	if (speed != last.speed || top != last.top || now - last.at >= 60s)
	{
		last = { speed, top, now };
		LOG_INFO("anti fling keeps the {} within its top speed: {:.0f} is set in the menu, {:.0f} is what it gets", names[static_cast<int>(what)], speed, top);
	}

	return top;
}

// apply the speed hack this tick
static void speedhack_tick(const local_character_t& character)
{

	static std::uint64_t applied_humanoid = 0;
	static float original_walkspeed = 16.0f;

	const std::uint64_t humanoid = character.humanoid;
	const bool active = settings::mover::speedhack::enabled && humanoid != 0 && is_keybind_active(mover_keybinds.speedhack);
	const bool walkspeed_mode = settings::mover::speedhack::mode == 0;

	if (applied_humanoid != 0 && !(active && walkspeed_mode))
	{
		if (applied_humanoid == humanoid)
		{
			memory->write<float>(humanoid + Offsets::Humanoid::Walkspeed, original_walkspeed);
			memory->write<float>(humanoid + Offsets::Humanoid::WalkspeedCheck, original_walkspeed);
		}
		applied_humanoid = 0;
	}

	if (!active)
	{
		stabilizer::own_ground_speed = 0.0f;
		return;
	}

	const float speed = within_top_speed(settings::mover::speedhack::speed, held_back_t::speedhack);
	stabilizer::own_ground_speed = speed;

	if (netblock::gliding)
		return;

	if (walkspeed_mode)
	{
		float current = memory->read<float>(humanoid + Offsets::Humanoid::Walkspeed);

		if (applied_humanoid != humanoid)
		{
			original_walkspeed = current;
			applied_humanoid = humanoid;
		}

		if (std::abs(current - speed) > 0.1f)
		{
			memory->write<float>(humanoid + Offsets::Humanoid::Walkspeed, speed);
		}

		float check = memory->read<float>(humanoid + Offsets::Humanoid::WalkspeedCheck);
		if (std::abs(check - speed) > 0.1f)
		{
			memory->write<float>(humanoid + Offsets::Humanoid::WalkspeedCheck, speed);
		}

		return;
	}

	if (character.root == 0)
		return;

	rbx::primitive_t prim = rbx::part_t{ character.root }.get_primitive();
	if (prim.address == 0)
		return;

	math::vector3 move_direction = memory->read<math::vector3>(humanoid + Offsets::Humanoid::MoveDirection);
	if (!is_finite(move_direction))
		return;

	move_direction.y = 0.0f;
	const float length = move_direction.length();
	if (length < 0.1f || length > 1.5f)
		return;

	math::vector3 velocity = memory->read<math::vector3>(prim.address + Offsets::Primitive::AssemblyLinearVelocity);
	if (!is_finite(velocity))
		return;

	velocity.x = move_direction.x / length * speed;
	velocity.z = move_direction.z / length * speed;
	memory->write<math::vector3>(prim.address + Offsets::Primitive::AssemblyLinearVelocity, velocity);
}

// apply the jump power tweak this tick
static void jumppower_tick(const local_character_t& character)
{

	static std::uint64_t applied_humanoid = 0;
	static float original_power = 50.0f;
	static float original_height = 7.2f;

	const std::uint64_t humanoid = character.humanoid;
	const bool active = settings::mover::jumppower::enabled && humanoid != 0 && is_keybind_active(mover_keybinds.jumppower);

	if (!active)
	{
		if (applied_humanoid != 0)
		{
			if (applied_humanoid == humanoid)
			{
				memory->write<float>(humanoid + Offsets::Humanoid::JumpPower, original_power);
				memory->write<float>(humanoid + Offsets::Humanoid::JumpHeight, original_height);
			}
			applied_humanoid = 0;
		}
		return;
	}

	float current_power = memory->read<float>(humanoid + Offsets::Humanoid::JumpPower);
	float current_height = memory->read<float>(humanoid + Offsets::Humanoid::JumpHeight);

	if (applied_humanoid != humanoid)
	{
		original_power = current_power;
		original_height = current_height;
		applied_humanoid = humanoid;
	}

	const float power = within_top_speed(settings::mover::jumppower::value, held_back_t::jump_power);
	const float height = power * power / (2.0f * 196.2f);

	if (std::abs(current_power - power) > 0.1f)
	{
		memory->write<float>(humanoid + Offsets::Humanoid::JumpPower, power);
	}

	if (std::abs(current_height - height) > 0.05f)
	{
		memory->write<float>(humanoid + Offsets::Humanoid::JumpHeight, height);
	}
}

// toggle noclip this tick
static void noclip_tick(const local_character_t& character)
{
	static std::uint64_t parts_model = 0;

	static std::vector<std::uint64_t> primitives;

	static std::unordered_set<std::uint64_t> cleared;
	static std::chrono::steady_clock::time_point last_scan{};

	const std::uint8_t collide_bit = static_cast<std::uint8_t>(Offsets::PrimitiveFlags::CanCollide);

	const bool active = settings::mover::noclip::enabled && character.model != 0 && is_keybind_active(mover_keybinds.noclip);

	if (!active)
	{

		if (!cleared.empty() && character.model != 0 && character.model == parts_model)
		{
			for (std::uint64_t primitive : primitives)
			{
				if (!cleared.contains(primitive))
					continue;

				std::uint8_t flags = memory->read<std::uint8_t>(primitive + Offsets::Primitive::Flags);
				memory->write<std::uint8_t>(primitive + Offsets::Primitive::Flags, static_cast<std::uint8_t>(flags | collide_bit));
			}
		}

		cleared.clear();
		return;
	}

	auto now = std::chrono::steady_clock::now();
	if (character.model != parts_model || now - last_scan >= 250ms)
	{
		if (character.model != parts_model)
		{
			cleared.clear();
		}

		parts_model = character.model;
		last_scan = now;
		primitives.clear();

		auto add_part = [](std::uint64_t part)
			{
				std::uint64_t primitive = memory->read<std::uint64_t>(part + Offsets::BasePart::Primitive);
				if (primitive != 0)
				{
					primitives.push_back(primitive);
				}
			};

		for (const rbx::instance_t& child : rbx::model_instance_t{ character.model }.get_children())
		{
			const std::string class_name = child.get_class_name();

			if (class_name.find("Part") != std::string::npos)
			{
				add_part(child.address);
			}
			else if (class_name == "Tool" || class_name == "Accessory" || class_name == "Hat")
			{

				for (const rbx::instance_t& inner : child.get_children())
				{
					if (inner.get_class_name().find("Part") != std::string::npos)
					{
						add_part(inner.address);
					}
				}
			}
		}
	}

	for (std::uint64_t primitive : primitives)
	{
		std::uint8_t flags = memory->read<std::uint8_t>(primitive + Offsets::Primitive::Flags);
		if (flags & collide_bit)
		{
			memory->write<std::uint8_t>(primitive + Offsets::Primitive::Flags, static_cast<std::uint8_t>(flags & ~collide_bit));
			cleared.insert(primitive);
		}
	}
}

struct teleport_writes_t
{
	std::uint64_t primitive = 0;
	math::vector3 position{};
};

static teleport_writes_t teleport_writes;

// flush the queued teleport writes
static void apply_teleport_writes()
{
	memory->write<math::vector3>(teleport_writes.primitive + Offsets::Primitive::Position, teleport_writes.position);

	memory->write<math::vector3>(teleport_writes.primitive + Offsets::Primitive::AssemblyLinearVelocity, math::vector3{});
}

struct teleport_target_t
{

	std::string name;

	std::uint64_t root = 0;
};

// pick where to teleport to
static teleport_target_t find_teleport_target()
{
	std::lock_guard<std::mutex> lock(cache::mtx);

	teleport_target_t target;
	target.name = cache::teleport_target;

	if (target.name.empty())
		return target;

	for (const cache::entity_t& entity : cache::cached_players)
	{
		if (entity.name != target.name)
			continue;

		auto root = entity.parts.find("HumanoidRootPart");
		if (root != entity.parts.end())
		{
			target.root = root->second.address;
		}
		break;
	}

	return target;
}

static std::atomic<mover::teleport_status> teleport_state{ mover::teleport_status::off };

// current teleport state for the menu
mover::teleport_status mover::get_teleport_status()
{
	return teleport_state;
}

// distance from a part to a point
static float distance_to(std::uint64_t primitive, const math::vector3& point)
{
	if (primitive == 0)
		return -1.0f;

	const math::vector3 position = rbx::primitive_t{ primitive }.get_position();
	return is_finite(position) && is_finite(point) ? position.distance(point) : -1.0f;
}

// step the teleport this tick
static void teleport_tick(const local_character_t& character)
{
	constexpr auto interval = 250ms;

	constexpr auto hold = 30ms;

	constexpr auto check_after = hold + 60ms;

	static std::chrono::steady_clock::time_point last_jump{};
	static teleport_target_t target;
	static bool was_active = false;

	static bool first_lookup = true;
	static int jump_number = 0;
	static float distance_before = -1.0f;
	static bool checked = true;

	teleport_writes = {};

	if (!settings::mover::teleport::enabled)
	{

		mover_keybinds.teleport.state = false;
	}

	const bool active = settings::mover::teleport::enabled && character.root != 0 && is_keybind_active(mover_keybinds.teleport);

	if (active != was_active)
	{
		was_active = active;
		first_lookup = true;
		jump_number = 0;

		const keybind::keybind_t& bind = mover_keybinds.teleport;
		LOG_INFO("teleport loop {} (key {}, {})", active ? "on" : "off", bind.key,
			bind.key == 0 ? "no key, so it follows Enable" :
			bind.mode == keybind::activation_mode::toggle ? "toggle" :
			bind.mode == keybind::activation_mode::hold ? "hold" : "always");
	}

	if (!active)
	{

		last_jump = {};
		target = {};
		teleport_state = mover::teleport_status::off;
		return;
	}

	const rbx::primitive_t own = rbx::part_t{ character.root }.get_primitive();

	const auto now = std::chrono::steady_clock::now();
	if (now - last_jump >= interval)
	{
		const std::string previous_name = target.name;
		const bool previous_found = target.root != 0;

		last_jump = now;
		target = find_teleport_target();

		const bool can_jump = target.root != 0 && target.root != character.root;
		teleport_state = target.name.empty() ? mover::teleport_status::no_target :
			can_jump ? mover::teleport_status::looping : mover::teleport_status::waiting;

		if (can_jump)
		{
			jump_number++;
			distance_before = distance_to(own.address, rbx::part_t{ target.root }.get_primitive().get_position());
			checked = false;
		}
		else if (first_lookup || target.name != previous_name || previous_found)
		{

			LOG_INFO("teleport: {}", target.name.empty() ? "no player is picked" : target.name + " has no character to land on right now");
		}

		first_lookup = false;
	}

	if (target.root == 0 || target.root == character.root || own.address == 0)
		return;

	const auto since_jump = now - last_jump;
	const bool holding = since_jump <= hold;
	const bool checking = !checked && since_jump >= check_after;
	if (!holding && !checking)
		return;

	rbx::primitive_t target_primitive = rbx::part_t{ target.root }.get_primitive();
	if (target_primitive.address == 0)
		return;

	const math::vector3 position = target_primitive.get_position();
	if (!is_finite(position))
		return;

	if (checking)
	{

		checked = true;
		if (jump_number <= 12 || jump_number % 20 == 0)
		{
			LOG_INFO("teleport jump {} onto {}: {:.1f} studs away before, {:.1f} studs away {}ms after letting go",
				jump_number, target.name, distance_before, distance_to(own.address, position),
				std::chrono::duration_cast<std::chrono::milliseconds>(check_after - hold).count());
		}
		return;
	}

	teleport_writes.primitive = own.address;
	teleport_writes.position = position;
}

// run the anti-fling guard this tick
static void stabilizer_tick(const local_character_t& character)
{
	std::uint64_t primitive = 0;

	std::uint64_t seated_root = 0;

	stabilizer::standing_t standing = settings::mover::stabilizer::enabled ? stabilizer::standing_t::no_character : stabilizer::standing_t::off;

	if (settings::mover::stabilizer::enabled && character.root != 0 && character.humanoid != 0)
	{

		const bool seated = memory->read<std::uint64_t>(character.humanoid + Offsets::Humanoid::SeatPart) != 0;
		const float health = memory->read<float>(character.humanoid + Offsets::Humanoid::Health);

		if (health > 0.0f)
		{
			const std::uint64_t root = rbx::part_t{ character.root }.get_primitive().address;

			if (!seated)
			{
				primitive = root;
				standing = root != 0 ? stabilizer::standing_t::guarded : stabilizer::standing_t::no_character;
			}
			else if (settings::mover::stabilizer::leave_vehicles)
			{
				seated_root = root;
				standing = root != 0 ? stabilizer::standing_t::sitting_watched : stabilizer::standing_t::no_character;
			}
			else
			{
				standing = stabilizer::standing_t::sitting;
			}
		}
		else
		{
			standing = stabilizer::standing_t::dead;
		}
	}

	stabilizer::standing = standing;

	const bool dead = standing == stabilizer::standing_t::dead;
	stabilizer::character_model = primitive != 0 || seated_root != 0 || dead ? character.model : 0;
	stabilizer::humanoid = primitive != 0 || seated_root != 0 ? character.humanoid : 0;
	stabilizer::seated_root = seated_root;
	stabilizer::root_primitive = primitive;
}

struct fly_writes_t
{
	std::uint64_t primitive = 0;

	bool position = false;
	bool height = false;
	bool velocity = false;
	math::vector3 position_value{};
	math::vector3 velocity_value{};
};

static fly_writes_t fly_writes;

// flush the queued fly writes
static void apply_fly_writes()
{
	const fly_writes_t& writes = fly_writes;

	if (writes.position)
	{
		memory->write<math::vector3>(writes.primitive + Offsets::Primitive::Position, writes.position_value);
	}
	else if (writes.height)
	{
		memory->write<float>(writes.primitive + Offsets::Primitive::Position + sizeof(float), writes.position_value.y);
	}

	if (writes.velocity)
	{
		memory->write<math::vector3>(writes.primitive + Offsets::Primitive::AssemblyLinearVelocity, writes.velocity_value);
	}
}

// apply the fly hack this tick
static void flyhack_tick(const local_character_t& character, float dt)
{
	static bool key_was_pressed = false;
	static bool toggle_state = false;
	static bool was_flying = false;

	static math::vector3 hold_position{};

	fly_writes = {};

	bool flying = false;

	if (!settings::expl::fly_enabled)
	{
		toggle_state = false;
	}
	else
	{

		const keybind::activation_mode fly_key_mode = static_cast<keybind::activation_mode>(settings::expl::fly_keybind_mode);

		if (settings::expl::fly_keybind == 0 || fly_key_mode == keybind::activation_mode::always)
		{
			flying = true;
		}
		else if (fly_key_mode == keybind::activation_mode::hold)
		{
			flying = keybind::key_down(settings::expl::fly_keybind);
		}
		else
		{
			bool current_key_state = keybind::key_down(settings::expl::fly_keybind);
			if (current_key_state && !key_was_pressed)
			{
				toggle_state = !toggle_state;
				key_was_pressed = true;
			}
			else if (!current_key_state)
			{
				key_was_pressed = false;
			}
			flying = toggle_state;
		}
	}

	rbx::primitive_t prim{};
	if (flying && character.root != 0)
	{
		prim = rbx::part_t{ character.root }.get_primitive();
	}

	static std::uint64_t anchored_primitive = 0;
	const std::uint8_t anchor_bit = static_cast<std::uint8_t>(Offsets::PrimitiveFlags::Anchored);
	const bool want_anchor = prim.address != 0 && settings::expl::fly_mode == 2;

	if (anchored_primitive != 0 && (!want_anchor || anchored_primitive != prim.address))
	{

		std::uint64_t current = character.root != 0 ? memory->read<std::uint64_t>(character.root + Offsets::BasePart::Primitive) : 0;
		if (current == anchored_primitive)
		{
			std::uint8_t flags = memory->read<std::uint8_t>(anchored_primitive + Offsets::Primitive::Flags);
			memory->write<std::uint8_t>(anchored_primitive + Offsets::Primitive::Flags, static_cast<std::uint8_t>(flags & ~anchor_bit));
		}
		anchored_primitive = 0;
	}

	if (prim.address == 0)
	{
		was_flying = false;
		return;
	}

	if (want_anchor)
	{
		std::uint8_t flags = memory->read<std::uint8_t>(prim.address + Offsets::Primitive::Flags);
		if ((flags & anchor_bit) == 0)
		{
			memory->write<std::uint8_t>(prim.address + Offsets::Primitive::Flags, static_cast<std::uint8_t>(flags | anchor_bit));

			anchored_primitive = prim.address;
		}
	}

	const math::vector3 position = prim.get_position();
	if (!is_finite(position))
		return;

	if (!was_flying)
	{
		hold_position = position;
		was_flying = true;
	}

	if (teleport_writes.primitive != 0)
	{
		hold_position = teleport_writes.position;
	}

	float forward = 0.0f, side = 0.0f, vertical = 0.0f;
	if (is_roblox_focused())
	{
		if (GetAsyncKeyState('W') & 0x8000) forward += 1.0f;
		if (GetAsyncKeyState('S') & 0x8000) forward -= 1.0f;
		if (GetAsyncKeyState('D') & 0x8000) side += 1.0f;
		if (GetAsyncKeyState('A') & 0x8000) side -= 1.0f;
		if (GetAsyncKeyState(VK_SPACE) & 0x8000) vertical += 1.0f;
		if (GetAsyncKeyState(VK_LCONTROL) & 0x8000) vertical -= 1.0f;

		if ((forward != 0.0f || side != 0.0f || vertical != 0.0f) && game::typing_in_chat())
		{
			forward = side = vertical = 0.0f;
		}
	}

	math::vector3 direction{};
	if ((forward != 0.0f || side != 0.0f) && game::camera != 0)
	{
		math::matrix3 camera_rotation = memory->read<math::matrix3>(game::camera + Offsets::Camera::Rotation);
		direction = camera_rotation * math::vector3{ side, 0.0f, -forward };
		if (!is_finite(direction))
		{
			direction = {};
		}
	}
	direction.y += vertical;

	const float length = direction.length();
	const bool moving = length > 0.01f;
	if (moving)
	{
		direction = direction * (1.0f / length);
	}

	const float speed = within_top_speed(settings::expl::fly_speed, held_back_t::fly);
	fly_writes_t writes{};
	writes.primitive = prim.address;

	if (settings::expl::fly_mode != 0 || !moving)
	{

		if (moving)
		{
			hold_position = hold_position + direction * (speed * dt);
		}

		writes.position = true;
		writes.position_value = hold_position;
		writes.velocity = true;
	}
	else
	{

		math::vector3 velocity = direction * speed;
		hold_position.x = position.x;
		hold_position.z = position.z;

		if (std::abs(direction.y) > 0.05f)
		{
			hold_position.y = position.y;
		}
		else
		{

			velocity.y = 0.0f;
			writes.height = true;
			writes.position_value.y = hold_position.y;
		}

		writes.velocity = true;
		writes.velocity_value = velocity;
	}

	fly_writes = writes;
}

// move the freecam this tick
static void freecam_tick(const local_character_t& character, float dt)
{

	static std::uint64_t applied_humanoid = 0;
	static math::vector3 original_offset{};
	static std::uint64_t anchored_primitive = 0;

	static math::vector3 offset{};

	const std::uint8_t anchor_bit = static_cast<std::uint8_t>(Offsets::PrimitiveFlags::Anchored);
	const bool active = settings::mover::freecam::enabled && character.humanoid != 0 && character.root != 0 && is_keybind_active(mover_keybinds.freecam);
	const std::uint64_t primitive = active ? rbx::part_t{ character.root }.get_primitive().address : 0;

	if (applied_humanoid != 0 && (primitive == 0 || applied_humanoid != character.humanoid))
	{
		if (applied_humanoid == character.humanoid)
		{
			memory->write<math::vector3>(applied_humanoid + Offsets::Humanoid::CameraOffset, original_offset);
		}

		const std::uint64_t current = character.root != 0 ? memory->read<std::uint64_t>(character.root + Offsets::BasePart::Primitive) : 0;
		if (anchored_primitive != 0 && current == anchored_primitive)
		{
			const std::uint8_t flags = memory->read<std::uint8_t>(anchored_primitive + Offsets::Primitive::Flags);
			memory->write<std::uint8_t>(anchored_primitive + Offsets::Primitive::Flags, static_cast<std::uint8_t>(flags & ~anchor_bit));
		}

		LOG_INFO("freecam off");
		applied_humanoid = 0;
		anchored_primitive = 0;
	}

	if (primitive == 0)
		return;

	if (applied_humanoid == 0)
	{
		applied_humanoid = character.humanoid;
		original_offset = memory->read<math::vector3>(character.humanoid + Offsets::Humanoid::CameraOffset);
		if (!is_finite(original_offset))
		{
			original_offset = {};
		}
		offset = {};
		LOG_INFO("freecam on");
	}

	const std::uint8_t flags = memory->read<std::uint8_t>(primitive + Offsets::Primitive::Flags);
	if ((flags & anchor_bit) == 0)
	{
		memory->write<std::uint8_t>(primitive + Offsets::Primitive::Flags, static_cast<std::uint8_t>(flags | anchor_bit));

		anchored_primitive = primitive;
	}

	float forward = 0.0f, side = 0.0f, vertical = 0.0f;
	if (is_roblox_focused() && !game::typing_in_chat())
	{
		if (GetAsyncKeyState('W') & 0x8000) forward += 1.0f;
		if (GetAsyncKeyState('S') & 0x8000) forward -= 1.0f;
		if (GetAsyncKeyState('D') & 0x8000) side += 1.0f;
		if (GetAsyncKeyState('A') & 0x8000) side -= 1.0f;
		if (GetAsyncKeyState(VK_SPACE) & 0x8000) vertical += 1.0f;
		if (GetAsyncKeyState(VK_LCONTROL) & 0x8000) vertical -= 1.0f;
	}

	math::vector3 direction{};
	if ((forward != 0.0f || side != 0.0f) && game::camera != 0)
	{
		const math::matrix3 camera_rotation = memory->read<math::matrix3>(game::camera + Offsets::Camera::Rotation);
		direction = camera_rotation * math::vector3{ side, 0.0f, -forward };
		if (!is_finite(direction))
		{
			direction = {};
		}
	}
	direction.y += vertical;

	const float length = direction.length();
	if (length > 0.01f)
	{
		const float speed = std::clamp(settings::mover::freecam::speed, settings::mover::freecam::speed_min, settings::mover::freecam::speed_max);
		offset = offset + direction * (speed * dt / length);
	}

	const math::matrix3 r = memory->read<math::matrix3>(primitive + Offsets::Primitive::Rotation);
	const math::vector3 local{
		r.m[0][0] * offset.x + r.m[1][0] * offset.y + r.m[2][0] * offset.z,
		r.m[0][1] * offset.x + r.m[1][1] * offset.y + r.m[2][1] * offset.z,
		r.m[0][2] * offset.x + r.m[1][2] * offset.y + r.m[2][2] * offset.z };

	if (is_finite(local))
	{
		memory->write<math::vector3>(character.humanoid + Offsets::Humanoid::CameraOffset, original_offset + local);
	}
}

// background loop: drive all the movement hacks
void mover::run()
{

	precise_sleeper sleeper;

	auto last_tick = std::chrono::steady_clock::now();

	while (g_running)
	{

		if (game::local_player.address == 0 || game::game_changed)
		{
			fly_writes = {};
			teleport_writes = {};
			stabilizer::root_primitive = 0;
			stabilizer::character_model = 0;
			stabilizer::humanoid = 0;
			stabilizer::seated_root = 0;
			stabilizer::standing = settings::mover::stabilizer::enabled ? stabilizer::standing_t::no_character : stabilizer::standing_t::off;
			std::this_thread::sleep_for(100ms);
			last_tick = std::chrono::steady_clock::now();
			continue;
		}

		auto now = std::chrono::steady_clock::now();
		const float dt = (std::min)(std::chrono::duration<float>(now - last_tick).count(), 0.05f);
		last_tick = now;

		update_keybind_settings();

		const local_character_t character = get_local_character();

		try
		{
			speedhack_tick(character);

			teleport_tick(character);
			flyhack_tick(character, dt);
			freecam_tick(character, dt);
			stabilizer_tick(character);
			jumppower_tick(character);
			noclip_tick(character);
		}
		catch (...) {
		}

		if (fly_writes.primitive != 0 || teleport_writes.primitive != 0)
		{

			stabilizer::own_move_until = GetTickCount64() + 300;
			const auto until = now + 1ms;
			do
			{
				if (fly_writes.primitive != 0)
				{
					apply_fly_writes();
				}
				if (teleport_writes.primitive != 0)
				{
					apply_teleport_writes();
				}
			} while (g_running && std::chrono::steady_clock::now() < until);

			continue;
		}

		const bool anything_enabled =
			settings::mover::speedhack::enabled || settings::expl::fly_enabled ||
			settings::mover::noclip::enabled || settings::mover::jumppower::enabled ||
			settings::mover::teleport::enabled || settings::mover::stabilizer::enabled ||
			settings::mover::freecam::enabled;

		if (anything_enabled)
		{
			sleeper.sleep_ms(1.0);
		}
		else
		{

			std::this_thread::sleep_for(1ms);
		}
	}
}
