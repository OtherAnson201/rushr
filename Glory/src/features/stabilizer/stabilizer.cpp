#include "stabilizer.h"

#include <globals.h>
#include <logger/logger.h>
#include <memory/memory.h>
#include <game/game.h>
#include <Offsets/Offsets.hpp>
#include <render/assets/settings.h>
#include <features/netblock/netblock.h>
#include <features/macros/macros.h>
#include <sdk/math/math.h>
#include <timing/precise_sleep.h>
#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstring>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <vector>
#include <windows.h>

using namespace std::chrono_literals;

#define AF_SAY(...) LOG_POST(::logger::level::info, __VA_ARGS__)
#define AF_WARN(...) LOG_POST(::logger::level::warn, __VA_ARGS__)
#define AF_SAY_EVERY(interval_ms, ...) LOG_POST_EVERY(interval_ms, ::logger::level::info, __VA_ARGS__)
#define AF_WARN_EVERY(interval_ms, ...) LOG_POST_EVERY(interval_ms, ::logger::level::warn, __VA_ARGS__)

// what closes that.

namespace
{
	using steady = std::chrono::steady_clock;

	constexpr steady::time_point never{};

	struct pv_t
	{
		math::matrix3 rotation;
		math::vector3 position;
		math::vector3 linear;
		math::vector3 angular;
	};

	static_assert(sizeof(pv_t) == 72, "pv_t is read from and written to the game in one piece");

	constexpr bool one_block =
		Offsets::Primitive::Position == Offsets::Primitive::Rotation + sizeof(math::matrix3) &&
		Offsets::Primitive::AssemblyLinearVelocity == Offsets::Primitive::Position + sizeof(math::vector3) &&
		Offsets::Primitive::AssemblyAngularVelocity == Offsets::Primitive::AssemblyLinearVelocity + sizeof(math::vector3);

	// read a parts position and velocity
	pv_t read_pv(std::uint64_t primitive)
	{
		pv_t pv{};

		if (one_block)
		{
			memory->read_buffer(primitive + Offsets::Primitive::Rotation, &pv, sizeof(pv));
		}
		else
		{
			pv.rotation = memory->read<math::matrix3>(primitive + Offsets::Primitive::Rotation);
			pv.position = memory->read<math::vector3>(primitive + Offsets::Primitive::Position);
			pv.linear = memory->read<math::vector3>(primitive + Offsets::Primitive::AssemblyLinearVelocity);
			pv.angular = memory->read<math::vector3>(primitive + Offsets::Primitive::AssemblyAngularVelocity);
		}

		return pv;
	}

	// write a parts position and velocity
	void write_pv(std::uint64_t primitive, const pv_t& pv)
	{
		if (one_block)
		{
			memory->write_buffer(primitive + Offsets::Primitive::Rotation, &pv, sizeof(pv));
		}
		else
		{
			memory->write<math::matrix3>(primitive + Offsets::Primitive::Rotation, pv.rotation);
			memory->write<math::vector3>(primitive + Offsets::Primitive::Position, pv.position);
			memory->write<math::vector3>(primitive + Offsets::Primitive::AssemblyLinearVelocity, pv.linear);
			memory->write<math::vector3>(primitive + Offsets::Primitive::AssemblyAngularVelocity, pv.angular);
		}
	}

	struct motion_t
	{
		math::vector3 position;
		math::vector3 linear;
		math::vector3 angular;
	};

	// read a parts full motion (pos, linear, angular)
	motion_t read_motion(std::uint64_t primitive)
	{
		motion_t motion{};

		if (one_block)
		{
			memory->read_buffer(primitive + Offsets::Primitive::Position, &motion, sizeof(motion));
		}
		else
		{
			motion.position = memory->read<math::vector3>(primitive + Offsets::Primitive::Position);
			motion.linear = memory->read<math::vector3>(primitive + Offsets::Primitive::AssemblyLinearVelocity);
			motion.angular = memory->read<math::vector3>(primitive + Offsets::Primitive::AssemblyAngularVelocity);
		}

		return motion;
	}

	// true if the value has no nan or inf
	bool is_finite(const math::vector3& v)
	{
		return std::isfinite(v.x) && std::isfinite(v.y) && std::isfinite(v.z);
	}

	// true if the value has no nan or inf
	bool is_finite(const pv_t& pv)
	{
		for (const float value : pv.rotation.m[0]) if (!std::isfinite(value)) return false;
		for (const float value : pv.rotation.m[1]) if (!std::isfinite(value)) return false;
		for (const float value : pv.rotation.m[2]) if (!std::isfinite(value)) return false;

		return is_finite(pv.position) && is_finite(pv.linear) && is_finite(pv.angular);
	}

	// horizontal speed from a velocity
	float over_ground(const math::vector3& velocity)
	{
		return std::hypot(velocity.x, velocity.z);
	}

	struct tally_t
	{
		steady::time_point since = never;

		int looks = 0;
		steady::time_point last_look = never;
		float longest_gap = 0.0f;

		int late = 0;

		math::vector3 position{};
		float ground = 0.0f;
		float up = 0.0f;
		float down = 0.0f;
		float spin = 0.0f;

		int cuts = 0;
		float cut_from = 0.0f;

		int held_down = 0;
		float held_from = 0.0f;

		int pushed = 0;
		float pushed_from = 0.0f;

		int throws = 0;
		int jumps = 0;
		int hops = 0;
		int put_again = 0;

		float fastest_part = 0.0f;
		int kept_off = 0;
		int released = 0;
		int switched_back = 0;

		float fastest_seat = 0.0f;
		int relocked = 0;

		float carried = 0.0f;
	};

	tally_t tally;

	constexpr auto status_every = 5s;

	std::atomic<int> scan_primitives{ 0 };
	std::atomic<int> scan_movable{ 0 };
	std::atomic<int> scan_seats{ 0 };
	std::atomic<float> scan_took{ 0.0f };

	struct nearest_seat_t
	{

		float distance = -1.0f;
		bool locked = false;
		bool thrown = false;

		float parked = 0.0f;
		float in_reach = 0.0f;
	};

	nearest_seat_t nearest_seat;

	bool seats_may_take = true;

	std::unordered_set<std::uint64_t> riding;

	// tally one guard pass for stats
	void count_look(const pv_t& pv, steady::time_point when)
	{
		tally.looks++;

		if (tally.last_look != never)
		{
			const float gap = std::chrono::duration<float, std::milli>(when - tally.last_look).count();
			tally.longest_gap = (std::max)(tally.longest_gap, gap);

			if (gap > 4.0f)
			{
				tally.late++;
			}

			if (gap > 25.0f)
			{
				AF_WARN_EVERY(5000, "anti fling: the character was not looked at for {:.0f} ms, this thread was held up", gap);
			}
		}
		tally.last_look = when;

		if (!is_finite(pv))
			return;

		tally.position = pv.position;
		tally.ground = (std::max)(tally.ground, over_ground(pv.linear));
		tally.up = (std::max)(tally.up, pv.linear.y);
		tally.down = (std::max)(tally.down, -pv.linear.y);
		tally.spin = (std::max)(tally.spin, pv.angular.length());
	}

	constexpr float too_fast = 250.0f;

	constexpr auto calm_for = 3s;

	// the CanCollide bit value
	std::uint8_t collide_bit()
	{
		return static_cast<std::uint8_t>(Offsets::PrimitiveFlags::CanCollide);
	}

	// true if the character part is alive
	bool is_alive(std::uint64_t primitive)
	{
		const std::uint64_t part = memory->read<std::uint64_t>(primitive + Offsets::Primitive::Owner);
		return part != 0 && memory->read<std::uint64_t>(part + Offsets::BasePart::Primitive) == primitive;
	}

	struct movable_t
	{
		std::uint64_t primitive = 0;

		float reach = 0.0f;
	};

	// speed of a point on a spinning part
	float surface_speed(const motion_t& motion, float reach)
	{
		const float speed = motion.linear.length() + motion.angular.length() * reach;
		return std::isfinite(speed) ? speed : 1.0e9f;
	}

	std::mutex movable_mutex;
	std::shared_ptr<const std::vector<movable_t>> movable_parts;

	// the shared list of movable parts
	std::shared_ptr<const std::vector<movable_t>> get_movable_parts()
	{
		std::lock_guard<std::mutex> lock(movable_mutex);
		return movable_parts;
	}

	// replace the shared movable-parts list
	void set_movable_parts(std::shared_ptr<const std::vector<movable_t>> parts)
	{
		std::lock_guard<std::mutex> lock(movable_mutex);
		movable_parts = std::move(parts);
	}

	std::shared_ptr<const std::vector<std::uint64_t>> seat_parts;

	// the shared list of seat parts
	std::shared_ptr<const std::vector<std::uint64_t>> get_seat_parts()
	{
		std::lock_guard<std::mutex> lock(movable_mutex);
		return seat_parts;
	}

	// replace the shared seat-parts list
	void set_seat_parts(std::shared_ptr<const std::vector<std::uint64_t>> seats)
	{
		std::lock_guard<std::mutex> lock(movable_mutex);
		seat_parts = std::move(seats);
	}

	// true if a part is a seat
	bool is_seat(std::uint64_t part)
	{
		static std::unordered_map<std::uint64_t, bool> known;
		static std::uint32_t known_session = 0;

		if (game::session != known_session)
		{
			known_session = game::session;
			known.clear();
		}

		const std::uint64_t descriptor = memory->read<std::uint64_t>(part + Offsets::Instance::ClassDescriptor);
		if (descriptor == 0)
			return false;

		const auto found = known.find(descriptor);
		if (found != known.end())
			return found->second;

		const std::uint64_t name = memory->read<std::uint64_t>(descriptor + Offsets::Instance::ClassName);
		const std::string class_name = name != 0 ? memory->read_string(name) : std::string{};
		const bool seat = class_name == "Seat" || class_name == "VehicleSeat";

		if (known.size() < 4096)
		{
			known[descriptor] = seat;
		}
		return seat;
	}

	std::mutex asked_mutex;
	std::vector<std::uint64_t> asked_about;

	// queue an instance to be named later
	void ask_about(std::uint64_t primitive)
	{
		std::lock_guard<std::mutex> lock(asked_mutex);
		if (asked_about.size() < 8)
		{
			asked_about.push_back(primitive);
		}
	}

	// much of it.
	std::string path_of(std::uint64_t instance)
	{
		std::string path;

		for (int level = 0; level < 5 && instance != 0; level++)
		{
			std::string name;
			if (instance == game::workspace.address)
			{
				name = "Workspace";
			}
			else
			{
				for (const char c : rbx::instance_t{ instance }.get_name())
				{
					if (name.size() < 40 && c >= ' ' && c <= '~')
					{
						name += c;
					}
				}
			}

			path = path.empty() ? name : name + "." + path;

			if (instance == game::workspace.address)
				break;

			instance = memory->read<std::uint64_t>(instance + Offsets::Instance::Parent);
		}

		return path;
	}

	// resolve the names of the queued instances
	void name_asked_parts()
	{
		std::vector<std::uint64_t> asked;
		{
			std::lock_guard<std::mutex> lock(asked_mutex);
			asked.swap(asked_about);
		}

		for (const std::uint64_t primitive : asked)
		{
			if (!is_alive(primitive))
			{
				AF_SAY("anti fling: the part that was kept off is gone again, too soon to name it");
				continue;
			}

			const std::uint64_t part = memory->read<std::uint64_t>(primitive + Offsets::Primitive::Owner);
			const math::vector3 size = memory->read<math::vector3>(primitive + Offsets::Primitive::Size);
			const motion_t motion = read_motion(primitive);

			AF_SAY("anti fling: the part kept off is {} (a {}, {:.1f} x {:.1f} x {:.1f}), at ({:.0f}, {:.0f}, {:.0f}), by now moving at {:.0f} and spinning at {:.0f}",
				path_of(part), rbx::instance_t{ part }.get_class_name(), size.x, size.y, size.z,
				motion.position.x, motion.position.y, motion.position.z, motion.linear.length(), motion.angular.length());
		}
	}

	// one pass gathering movable and seat parts
	void scan_once()
	{
		const std::uint64_t model = stabilizer::character_model;
		const std::uint64_t workspace = game::workspace.address;
		if (model == 0 || workspace == 0)
		{
			set_movable_parts(nullptr);
			set_seat_parts(nullptr);
			return;
		}

		const std::uint64_t world = memory->read<std::uint64_t>(workspace + Offsets::Workspace::World);
		if (world == 0)
			return;

		const std::uint64_t first = memory->read<std::uint64_t>(world + Offsets::World::Primitives);
		const std::uint64_t last = memory->read<std::uint64_t>(world + Offsets::World::Primitives + sizeof(std::uint64_t));
		if (first == 0 || last <= first || (last - first) % 8 != 0 || last - first > 8ull * 4000000)
			return;

		std::vector<std::uint64_t> primitives((last - first) / 8);
		memory->read_buffer(first, primitives.data(), last - first);

		const std::uint8_t anchored = static_cast<std::uint8_t>(Offsets::PrimitiveFlags::Anchored);

		const bool want_seats = settings::mover::stabilizer::leave_vehicles || settings::mover::stabilizer::lock_seats;

		auto found = std::make_shared<std::vector<movable_t>>();
		auto seats = std::make_shared<std::vector<std::uint64_t>>();
		for (const std::uint64_t primitive : primitives)
		{
			if (primitive == 0 || !g_running)
				continue;

			const std::uint8_t flags = memory->read<std::uint8_t>(primitive + Offsets::Primitive::Flags);
			if ((flags & anchored) != 0)
				continue;

			const bool collides = (flags & collide_bit()) != 0;
			if (!collides && !want_seats)
				continue;

			const std::uint64_t part = memory->read<std::uint64_t>(primitive + Offsets::Primitive::Owner);
			if (part == 0 || memory->read<std::uint64_t>(part + Offsets::BasePart::Primitive) != primitive)
				continue;

			if (want_seats && is_seat(part))
			{
				seats->push_back(primitive);
			}

			if (!collides)
				continue;

			const std::uint64_t parent = memory->read<std::uint64_t>(part + Offsets::Instance::Parent);
			if (parent == model || (parent != 0 && memory->read<std::uint64_t>(parent + Offsets::Instance::Parent) == model))
				continue;

			const math::vector3 size = memory->read<math::vector3>(primitive + Offsets::Primitive::Size);
			const float largest = (std::max)((std::max)(size.x, size.y), size.z);

			found->push_back({ primitive, std::isfinite(largest) ? std::clamp(largest * 0.5f, 0.0f, 50.0f) : 0.0f });
		}

		scan_primitives = static_cast<int>(primitives.size());
		scan_movable = static_cast<int>(found->size());
		scan_seats = static_cast<int>(seats->size());

		set_movable_parts(std::move(found));
		set_seat_parts(std::move(seats));
	}

	// true if any guarded feature is on
	bool anything_to_scan_for()
	{
		return stabilizer::character_model != 0;
	}

	// background: keep the part lists fresh
	void scan()
	{
		while (g_running)
		{
			if (!anything_to_scan_for() || game::game_changed)
			{
				set_movable_parts(nullptr);
				set_seat_parts(nullptr);
				std::this_thread::sleep_for(100ms);
				continue;
			}

			const steady::time_point started = steady::now();
			scan_once();
			const steady::duration took = steady::now() - started;
			scan_took = std::chrono::duration<float, std::milli>(took).count();

			const steady::time_point next = steady::now() + (std::max)(steady::duration{ 250ms }, took * 15);
			while (g_running && steady::now() < next && anything_to_scan_for())
			{
				name_asked_parts();
				std::this_thread::sleep_for(50ms);
			}
		}
	}

	struct kept_off_t
	{
		movable_t part;
		steady::time_point last_too_fast = never;
	};

	std::vector<kept_off_t> kept_off;

	struct released_t
	{
		movable_t part;
		steady::time_point until = never;
	};

	std::vector<released_t> released;

	// true if a part is being forced non-colliding
	bool is_kept_off(std::uint64_t primitive)
	{
		for (const kept_off_t& kept : kept_off)
		{
			if (kept.part.primitive == primitive)
				return true;
		}
		return false;
	}

	// restore collision on all parts
	void let_all_touch_again()
	{
		for (const kept_off_t& kept : kept_off)
		{
			if (!is_alive(kept.part.primitive))
				continue;

			const std::uint8_t flags = memory->read<std::uint8_t>(kept.part.primitive + Offsets::Primitive::Flags);
			memory->write<std::uint8_t>(kept.part.primitive + Offsets::Primitive::Flags, static_cast<std::uint8_t>(flags | collide_bit()));
		}

		kept_off.clear();
		released.clear();
		stabilizer::parts_kept_off = 0;
	}

	// turn off collision for fast parts
	void keep_fast_parts_off()
	{
		const steady::time_point now = steady::now();

		int flying = 0;
		float fastest = 0.0f;

		std::uint64_t fastest_part = 0;
		math::vector3 fastest_at{};

		auto watch = [&](const movable_t& part)
		{
			if (riding.contains(part.primitive))
				return;

			const motion_t motion = read_motion(part.primitive);
			const float surface = surface_speed(motion, part.reach);

			if (surface <= too_fast)
			{

				tally.fastest_part = (std::max)(tally.fastest_part, surface);
				return;
			}

			if (is_kept_off(part.primitive) || !is_alive(part.primitive))
				return;

			const std::uint8_t flags = memory->read<std::uint8_t>(part.primitive + Offsets::Primitive::Flags);
			if ((flags & collide_bit()) == 0 || (flags & static_cast<std::uint8_t>(Offsets::PrimitiveFlags::Anchored)) != 0)
				return;

			memory->write<std::uint8_t>(part.primitive + Offsets::Primitive::Flags, static_cast<std::uint8_t>(flags & ~collide_bit()));
			kept_off.push_back({ part, now });
			tally.kept_off++;

			const float speed = motion.linear.length();
			if (!(speed <= too_fast))
			{
				flying++;

				const float counted = std::isfinite(speed) ? speed : 1.0e9f;
				if (counted > fastest)
				{
					fastest = counted;
					fastest_part = part.primitive;
					fastest_at = motion.position;
				}
			}
		};

		const std::shared_ptr<const std::vector<movable_t>> parts = get_movable_parts();
		if (parts)
		{
			for (const movable_t& part : *parts)
			{
				watch(part);
			}
		}

		for (std::size_t i = 0; i < released.size();)
		{
			if (now >= released[i].until)
			{
				released.erase(released.begin() + static_cast<std::ptrdiff_t>(i));
				continue;
			}

			watch(released[i].part);
			i++;
		}

		if (flying > 0)
		{
			AF_SAY_EVERY(2000, "anti fling: {} part{} moving at up to {:.0f} studs a second can no longer touch the character, the fastest was {:.0f} studs from it",
				flying, flying == 1 ? "" : "s", fastest, is_finite(fastest_at) ? fastest_at.distance(tally.position) : -1.0f);

			static steady::time_point asked_at = never;
			if (fastest_part != 0 && (asked_at == never || now - asked_at >= 2s))
			{
				asked_at = now;
				ask_about(fastest_part);
			}
		}

		for (std::size_t i = 0; i < kept_off.size();)
		{
			kept_off_t& kept = kept_off[i];

			if (!is_alive(kept.part.primitive))
			{

				kept_off.erase(kept_off.begin() + static_cast<std::ptrdiff_t>(i));
				continue;
			}

			const bool calm = surface_speed(read_motion(kept.part.primitive), kept.part.reach) < too_fast * 0.5f;
			const std::uint8_t flags = memory->read<std::uint8_t>(kept.part.primitive + Offsets::Primitive::Flags);

			if (!calm)
			{
				kept.last_too_fast = now;

				if ((flags & collide_bit()) != 0)
				{
					memory->write<std::uint8_t>(kept.part.primitive + Offsets::Primitive::Flags, static_cast<std::uint8_t>(flags & ~collide_bit()));
					tally.switched_back++;
				}
			}
			else if (now - kept.last_too_fast >= calm_for)
			{
				memory->write<std::uint8_t>(kept.part.primitive + Offsets::Primitive::Flags, static_cast<std::uint8_t>(flags | collide_bit()));
				released.push_back({ kept.part, now + 5s });
				kept_off.erase(kept_off.begin() + static_cast<std::ptrdiff_t>(i));
				tally.released++;
				continue;
			}

			i++;
		}

		stabilizer::parts_kept_off = static_cast<int>(kept_off.size());
	}

	// log the anti-fling status line
	void report_status(steady::time_point now, const char* doing, bool on_its_feet)
	{

		static bool was_on_its_feet = true;
		if (on_its_feet != was_on_its_feet)
		{
			was_on_its_feet = on_its_feet;
			tally = {};
		}

		if (tally.since == never)
		{
			tally.since = now;
			return;
		}

		if (now - tally.since < status_every)
			return;

		std::string character;
		if (on_its_feet)
		{
			character = std::format("fastest {:.0f} over the ground, {:.0f} up, {:.0f} down, spin {:.0f}; top speed {:.0f}, cut {} times{}, held down {} times{}, pushes held back {} times{}; {} throws undone, {} jumps taken for teleports and {} hops under 5 studs, {} put back; {} looks, longest gap {:.1f} ms, {} over 4 ms",
				tally.ground, tally.up, tally.down, tally.spin, stabilizer::top_speed(), tally.cuts,
				tally.cuts > 0 ? std::format(" from as much as {:.0f}", tally.cut_from) : std::string{},
				tally.held_down, tally.held_down > 0 ? std::format(" from as much as {:.0f} up", tally.held_from) : std::string{},
				tally.pushed, tally.pushed > 0 ? std::format(" from as much as {:.0f}", tally.pushed_from) : std::string{},
				tally.throws, tally.jumps, tally.hops, tally.put_again, tally.looks, tally.longest_gap, tally.late);
		}
		else
		{
			character = std::format("carried at up to {:.0f}, a vehicle may do {:.0f}; {} throws of the vehicle taken back", tally.carried, stabilizer::vehicle_speed(), tally.throws);
		}

		std::string nearest;
		if (nearest_seat.distance >= 0.0f)
		{
			nearest = std::format(", nearest {:.0f} studs away: {}{}, stood still for {:.1f} s, in reach for {:.1f} s", nearest_seat.distance,
				nearest_seat.locked ? "locked" : "open", nearest_seat.thrown ? ", thrown" : "", nearest_seat.parked, nearest_seat.in_reach);
		}

		AF_SAY("anti fling status, {:.0f} s {} at ({:.0f}, {:.0f}, {:.0f}): {} | parts: {} free to move, fastest {:.0f} (kept off above {:.0f}), {} kept off ({} new, {} let touch again, {} the game had switched back on) | seats: {} watched, {} locked, {} thrown, fastest {:.0f} (thrown above {:.0f}), {} the game had switched back on{} | scan: {} primitives in {:.0f} ms",
			std::chrono::duration<double>(now - tally.since).count(), doing, tally.position.x, tally.position.y, tally.position.z, character,
			scan_movable.load(), tally.fastest_part, too_fast, stabilizer::parts_kept_off.load(), tally.kept_off, tally.released, tally.switched_back,
			stabilizer::seats_watched.load(), stabilizer::seats_locked.load(), stabilizer::seats_thrown.load(), tally.fastest_seat, stabilizer::vehicle_speed(), tally.relocked, nearest,
			scan_primitives.load(), scan_took.load());

		const steady::time_point last_look = tally.last_look;
		tally = {};
		tally.since = now;
		tally.last_look = last_look;
	}

	struct guard_t
	{
		std::uint64_t primitive = 0;

		bool has_good = false;
		pv_t good{};
		steady::time_point good_at = never;

		pv_t seen{};

		bool has_before_jump = false;
		pv_t before_jump{};
		steady::time_point jumped = never;

		bool teleported = false;

		steady::time_point changed_at = never;
		steady::time_point changed_before = never;

		math::vector3 refused_at{};
		steady::time_point refused_when = never;

		steady::time_point undone_at = never;

		steady::time_point held_until = never;

		steady::time_point last_throw = never;
		steady::time_point attack_began = never;
		int undone = 0;
		float worst_speed = 0.0f;

		int kinds[4]{};
		float furthest = 0.0f;

		steady::time_point slowed_since = never;
		steady::time_point slowed_last = never;
		float pushed_to = 0.0f;

		int cuts = 0;
		bool push_told = false;

		steady::time_point rising_since = never;
	};

	constexpr auto halves_apart = 1500us;

	constexpr auto settle_for = 250ms;

	// how long a jump is allowed to last
	steady::duration jump_window(const guard_t& guard)
	{
		return guard.teleported ? steady::duration{ settle_for } : steady::duration{ halves_apart };
	}

	// true if the local player caused this move
	bool own_move()
	{
		return GetTickCount64() < stabilizer::own_move_until;
	}

	constexpr float spin_of_a_throw = 200.0f;

	constexpr float a_hair = 1.02f;

	// clamp a velocity to a top speed
	bool cut_to(math::vector3& velocity, float top)
	{
		bool cut = false;

		const float sideways = over_ground(velocity);
		if (sideways > top * a_hair)
		{
			velocity.x *= top / sideways;
			velocity.z *= top / sideways;
			cut = true;
		}

		if (velocity.y > top * a_hair)
		{
			velocity.y = top;
			cut = true;
		}

		return cut;
	}

	// and nothing more.
	bool is_thrown(float before, float after, float top)
	{
		return after > top * 2.0f && after - before > top;
	}

	// at all.
	bool is_throw(const pv_t& before, const pv_t& after, float top)
	{
		if (!is_finite(after))
			return true;

		if (is_thrown(over_ground(before.linear), over_ground(after.linear), top) ||
			is_thrown(std::abs(before.linear.y), std::abs(after.linear.y), top))
			return true;

		const float spin = after.angular.length();
		return spin > spin_of_a_throw && spin - before.angular.length() > spin_of_a_throw * 0.5f;
	}

	// true if the motion change looks like a jump
	bool is_jump(const pv_t& before, const pv_t& after, steady::duration elapsed)
	{
		const float seconds = (std::min)(std::chrono::duration<float>(elapsed).count(), 0.05f);
		const float fastest = (std::max)(before.linear.length(), after.linear.length());
		return after.position.distance(before.position) > fastest * seconds * 3.0f + 0.5f;
	}

	struct stood_t
	{
		pv_t pv{};
		steady::time_point at = never;
	};

	constexpr auto stood_apart = 10ms;

	std::array<stood_t, 128> stood_ring{};
	std::size_t stood_next = 0;
	steady::time_point stood_last = never;

	std::uint64_t stood_primitive = 0;

	// drop the remembered standing spot
	void forget_standing()
	{
		stood_ring.fill({});
		stood_last = never;
	}

	// remember where a part was standing
	void remember_standing(std::uint64_t primitive, const pv_t& pv, steady::time_point when)
	{
		if (primitive != stood_primitive)
		{
			forget_standing();
			stood_primitive = primitive;
		}

		if (stood_last != never && when - stood_last < stood_apart)
			return;

		stood_ring[stood_next] = { pv, when };
		stood_next = (stood_next + 1) % stood_ring.size();
		stood_last = when;
	}

	// where a part last stood still
	bool where_it_stood(std::uint64_t primitive, steady::time_point now, steady::duration at_least, pv_t& out)
	{
		if (primitive != stood_primitive)
			return false;

		const stood_t* newest = nullptr;
		for (const stood_t& entry : stood_ring)
		{
			if (entry.at == never || now - entry.at < at_least || now - entry.at > 2s)
				continue;

			if (newest == nullptr || entry.at > newest->at)
			{
				newest = &entry;
			}
		}

		if (newest == nullptr)
			return false;

		out = newest->pv;
		return true;
	}

	// not touched.
	void slow_down(guard_t& guard, pv_t& now, float top, steady::time_point when)
	{

		const float sideways = over_ground(now.linear);
		const float upwards = now.linear.y;
		const float speed = (std::max)(sideways, upwards);
		if (!cut_to(now.linear, top))
			return;

		memory->write<math::vector3>(guard.primitive + Offsets::Primitive::AssemblyLinearVelocity, now.linear);

		if (guard.slowed_since == never)
		{
			guard.slowed_since = when;
		}
		guard.slowed_last = when;
		guard.pushed_to = (std::max)(guard.pushed_to, speed);
		guard.cuts++;
		stabilizer::last_slowed = GetTickCount64();

		tally.cuts++;
		tally.cut_from = (std::max)(tally.cut_from, speed);

		if (!guard.push_told && speed > top * 1.25f)
		{
			guard.push_told = true;

			AF_SAY("anti fling: something pushes the character past its top speed of {:.0f}: {:.0f} over the ground and {:.0f} upwards, at ({:.0f}, {:.0f}, {:.0f}). Held to the top speed",
				top, sideways, upwards, now.position.x, now.position.y, now.position.z);
		}
	}

	constexpr float rising_fast = 30.0f;

	constexpr float gravity = 196.2f;

	constexpr std::uint16_t climbing_state = 12;

	// the humanoids jump velocity
	float jump_speed(std::uint64_t humanoid)
	{
		if (humanoid == 0)
			return 50.0f;

		const bool by_power = memory->read<std::uint8_t>(humanoid + Offsets::Humanoid::UseJumpPower) != 0;
		const float speed = by_power ? memory->read<float>(humanoid + Offsets::Humanoid::JumpPower) :
			std::sqrt(2.0f * gravity * (std::max)(memory->read<float>(humanoid + Offsets::Humanoid::JumpHeight), 0.0f));

		return std::isfinite(speed) ? (std::max)(speed, 50.0f) : 50.0f;
	}

	// pin a part in place
	void hold_down(guard_t& guard, pv_t& now, steady::time_point when)
	{

		if (now.linear.y <= rising_fast || settings::expl::fly_enabled || GetTickCount64() - macros::last_run < 1000 || netblock::lifting)
		{
			guard.rising_since = never;
			return;
		}

		if (guard.rising_since == never)
		{
			guard.rising_since = when;
		}

		const std::uint64_t humanoid = stabilizer::humanoid;
		const float jump = jump_speed(humanoid);
		const float rising_for = std::chrono::duration<float>(when - guard.rising_since).count();

		const bool too_long = rising_for > (jump - rising_fast) / gravity + 0.1f;
		const bool too_fast = now.linear.y > jump * 1.1f;
		if (!too_long && !too_fast)
			return;

		const std::uint64_t state = humanoid != 0 ? memory->read<std::uint64_t>(humanoid + Offsets::Humanoid::HumanoidState) : 0;
		if (state != 0 && memory->read<std::uint16_t>(state + Offsets::Humanoid::HumanoidStateID) == climbing_state)
			return;

		const float pushed = now.linear.y;
		now.linear.y = too_long ? 0.0f : jump;
		memory->write<math::vector3>(guard.primitive + Offsets::Primitive::AssemblyLinearVelocity, now.linear);

		tally.held_down++;
		tally.held_from = (std::max)(tally.held_from, pushed);

		AF_SAY_EVERY(2000, "anti fling: something pushes the character up at {:.0f} studs a second{}, more than its jump of {:.0f} does, at ({:.0f}, {:.0f}, {:.0f}). Held down: it is most likely stuck in a wall, a roof or a vehicle",
			pushed, too_long ? std::format(" for {:.2f} s", rising_for) : std::string{}, jump, now.position.x, now.position.y, now.position.z);
	}

	// cancel an outside push on a part
	void hold_back_push(const guard_t& guard, pv_t& now)
	{
		const std::uint64_t humanoid = stabilizer::humanoid;
		if (humanoid == 0 || settings::expl::fly_enabled || own_move())
			return;

		const float walk = memory->read<float>(humanoid + Offsets::Humanoid::Walkspeed);
		const float own = (std::max)({ std::isfinite(walk) ? walk : 16.0f, stabilizer::own_ground_speed.load(), 16.0f });

		const float allowed = (std::min)((std::max)(own * 1.15f + 2.0f, 30.0f), stabilizer::top_speed());

		const float sideways = over_ground(now.linear);
		if (!(sideways > allowed))
			return;

		now.linear.x *= allowed / sideways;
		now.linear.z *= allowed / sideways;
		memory->write<math::vector3>(guard.primitive + Offsets::Primitive::AssemblyLinearVelocity, now.linear);

		tally.pushed++;
		tally.pushed_from = (std::max)(tally.pushed_from, sideways);
		AF_SAY_EVERY(2000, "anti fling: something pushes the character sideways at {:.0f} studs a second, more than it goes by itself ({:.0f}), at ({:.0f}, {:.0f}, {:.0f}). Held to that",
			sideways, allowed, now.position.x, now.position.y, now.position.z);
	}

	enum class throw_t
	{
		over_ground,
		up_or_down,
		spin,
		not_numbers
	};

	// classify how a part was thrown
	throw_t kind_of_throw(const pv_t& before, const pv_t& after, float top)
	{
		if (!is_finite(after))
			return throw_t::not_numbers;

		if (is_thrown(over_ground(before.linear), over_ground(after.linear), top))
			return throw_t::over_ground;

		if (is_thrown(std::abs(before.linear.y), std::abs(after.linear.y), top))
			return throw_t::up_or_down;

		return throw_t::spin;
	}

	// record that a part got thrown
	void note_throw(guard_t& guard, const pv_t& before, const pv_t& thrown, float top, steady::duration since_good, bool after_jump)
	{
		static const char* const names[4] = { "over the ground", "up or down", "into a spin", "to values that are not numbers" };

		const throw_t kind = kind_of_throw(before, thrown, top);
		const float moved = is_finite(thrown.position) ? thrown.position.distance(guard.good.position) : 0.0f;

		tally.throws++;
		guard.kinds[static_cast<int>(kind)]++;
		guard.furthest = (std::max)(guard.furthest, moved);

		if (guard.undone != 1)
			return;

		AF_SAY("anti fling: thrown {}. Over the ground {:.0f} -> {:.0f}, up and down {:.0f} -> {:.0f}, spin {:.0f} -> {:.0f}, with a top speed of {:.0f}; {:.1f} studs from where it stood, {:.2f} ms after it was last seen in order. Taken back to ({:.0f}, {:.0f}, {:.0f}){}",
			names[static_cast<int>(kind)], over_ground(before.linear), over_ground(thrown.linear), before.linear.y, thrown.linear.y,
			before.angular.length(), thrown.angular.length(), top, moved, std::chrono::duration<double, std::milli>(since_good).count(),
			guard.good.position.x, guard.good.position.y, guard.good.position.z, after_jump ? ", where it was before it jumped a moment ago" : "");
	}

	// true if a part is currently held
	bool part_is_held()
	{
		return netblock::held_on_part.load();
	}

	// scan the parts for fling attempts and guard them
	bool look(guard_t& guard)
	{
		if (part_is_held())
			return false;

		pv_t now = read_pv(guard.primitive);
		const steady::time_point when = steady::now();

		if (part_is_held())
			return false;
		const float top = stabilizer::top_speed();

		count_look(now, when);

		if (!guard.has_good)
		{

			if (is_finite(now))
			{
				slow_down(guard, now, top, when);
				guard.good = now;
				guard.good_at = when;
				guard.seen = now;
				guard.has_good = true;
				remember_standing(guard.primitive, now, when);
			}
			return false;
		}

		if (is_throw(guard.good, now, top))
		{

			const pv_t before = guard.good;
			const steady::duration since_good = when - guard.good_at;

			const bool after_jump = guard.has_before_jump && when - guard.jumped < jump_window(guard);
			if (after_jump)
			{
				guard.good = guard.before_jump;
			}
			guard.has_before_jump = false;
			guard.teleported = false;

			cut_to(guard.good.linear, top);
			write_pv(guard.primitive, guard.good);
			guard.seen = guard.good;

			const float speed = now.linear.length();
			guard.worst_speed = (std::max)(guard.worst_speed, std::isfinite(speed) ? speed : 1.0e9f);
			guard.undone++;
			guard.undone_at = when;
			guard.last_throw = when;
			stabilizer::throws_stopped++;

			note_throw(guard, before, now, top, since_good, after_jump);
			return true;
		}

		slow_down(guard, now, top, when);
		hold_down(guard, now, when);
		hold_back_push(guard, now);

		const bool settled = std::memcmp(&now, &guard.seen, sizeof(pv_t)) == 0;
		guard.seen = now;
		if (!settled)
		{
			guard.changed_before = guard.changed_at;
			guard.changed_at = when;
		}
		if (!settled && when - guard.good_at < 20ms)
			return false;

		if (is_jump(guard.good, now, when - guard.good_at))
		{
			if ((guard.undone_at != never && when - guard.undone_at < halves_apart) || when < guard.held_until)
			{

				write_pv(guard.primitive, guard.good);
				guard.seen = guard.good;

				tally.put_again++;
				AF_SAY_EVERY(1000, "anti fling: the character was {:.0f} studs from where it belongs {}, written back",
					now.position.distance(guard.good.position), when < guard.held_until ? "while it is held where it was put back" : "right after a throw was taken back");
				return false;
			}

			const float moved = now.position.distance(guard.good.position);
			const bool own = own_move();

			if (!own && moved >= 5.0f)
			{
				const float frozen = guard.changed_before != never ?
					(std::min)(std::chrono::duration<float>(guard.changed_at - guard.changed_before).count(), 0.5f) : 0.0f;
				const bool own_pace = moved <= guard.good.linear.length() * frozen * 1.5f + 1.0f;
				const bool insists = guard.refused_when != never && when - guard.refused_when < 1s && now.position.distance(guard.refused_at) < 4.0f;

				if (!own_pace && !insists)
				{

					const bool chain = guard.has_before_jump && guard.teleported && when - guard.jumped < settle_for;
					pv_t back = chain ? guard.before_jump : guard.good;
					if (chain)
					{
						back.linear = {};
						back.angular = {};
						guard.jumped = when;
					}

					write_pv(guard.primitive, back);
					guard.good = back;
					guard.good_at = when;
					guard.seen = back;
					guard.refused_at = now.position;
					guard.refused_when = when;

					tally.put_again++;
					AF_SAY_EVERY(1000, "anti fling: the character was moved {:.0f} studs in one go, to ({:.0f}, {:.0f}, {:.0f}), not by this program and not at its own pace{}. Taken back to ({:.0f}, {:.0f}, {:.0f})",
						moved, now.position.x, now.position.y, now.position.z, chain ? ", after other such jumps" : "",
						back.position.x, back.position.y, back.position.z);
					return false;
				}

				if (insists)
				{
					AF_SAY_EVERY(1000, "anti fling: the character was put at ({:.0f}, {:.0f}, {:.0f}) again after being taken back from there: the game's own teleport, let be",
						now.position.x, now.position.y, now.position.z);
				}
			}

			if (!guard.has_before_jump || when - guard.jumped >= jump_window(guard))
			{
				guard.before_jump = guard.good;
				guard.has_before_jump = true;
				guard.jumped = when;
				guard.teleported = false;
			}

			if (moved < 5.0f)
			{
				tally.hops++;
			}
			else
			{
				tally.jumps++;
				guard.teleported = !own;
				AF_SAY_EVERY(1000, "anti fling: the character is suddenly {:.0f} studs from where it was, at ({:.0f}, {:.0f}, {:.0f}) and moving at {:.0f}. Taken for a teleport{}",
					moved, now.position.x, now.position.y, now.position.z, now.linear.length(), own ? " of this program" : " once it has stayed put for a moment");
			}
		}
		else if (guard.has_before_jump && when - guard.jumped >= jump_window(guard))
		{
			guard.has_before_jump = false;
			guard.teleported = false;
		}

		guard.good = now;
		guard.good_at = when;

		if (!guard.teleported)
		{
			remember_standing(guard.primitive, now, when);
		}
		return false;
	}

	// true if the part is being pushed now
	bool is_pushed(const guard_t& guard, steady::time_point now)
	{
		return guard.slowed_last != never && now - guard.slowed_last < 100ms && guard.pushed_to > stabilizer::top_speed() * 1.25f;
	}

	// something really pushed.
	void report_slowing(guard_t& guard, steady::time_point now)
	{
		if (guard.slowed_since == never || now - guard.slowed_last < 1s)
			return;

		const float top = stabilizer::top_speed();
		if (guard.pushed_to > top * 1.25f)
		{
			AF_SAY("anti fling: the character was held to {:.0f} studs a second for {:.1f} s, by cutting its speed {} times; something was pushing it to as much as {:.0f}",
				top, std::chrono::duration<double>(guard.slowed_last - guard.slowed_since).count(), guard.cuts, guard.pushed_to);
		}

		guard.slowed_since = never;
		guard.slowed_last = never;
		guard.pushed_to = 0.0f;
		guard.cuts = 0;
		guard.push_told = false;
	}

	constexpr auto stay_for = 2s;

	void watch_vehicles(steady::time_point now);
	void lock_moving_open_seats(steady::time_point now);

	// true if the humanoid is seated
	bool sits(std::uint64_t humanoid)
	{
		return humanoid != 0 && memory->read<std::uint64_t>(humanoid + Offsets::Humanoid::SeatPart) != 0;
	}

	// for a while.
	void defend(guard_t& guard)
	{
		guard.attack_began = guard.last_throw;

		const char* ended = "nothing more came for 2 s";

		steady::time_point last_pass = never;
		steady::time_point told = guard.attack_began;
		int told_undone = guard.undone;
		for (;;)
		{
			look(guard);

			const steady::time_point now = steady::now();
			if (now - guard.last_throw >= stay_for)
				break;

			if (!g_running || stabilizer::root_primitive != guard.primitive)
			{
				ended = "the character is no longer the one that was guarded (it sat down, died, or anti fling was switched off)";
				break;
			}

			if (sits(stabilizer::humanoid))
			{
				ended = "the character sat down";
				break;
			}

			if (now - last_pass >= 4ms)
			{
				last_pass = now;
				keep_fast_parts_off();
			}

			watch_vehicles(now);
			lock_moving_open_seats(now);

			if (now - told >= 1s)
			{
				told = now;

				if (guard.undone > told_undone)
				{
					told_undone = guard.undone;
					AF_SAY("anti fling: still being thrown, {} throws undone in {:.1f} s so far, the hardest at {:.0f} studs a second; {} parts kept off",
						guard.undone, std::chrono::duration<double>(now - guard.attack_began).count(), guard.worst_speed, kept_off.size());
				}
			}

			report_status(now, "on its feet and being thrown", true);
		}

		AF_SAY("anti fling: {} throw{} undone over {:.1f} s ({} over the ground, {} up or down, {} into a spin, {} not numbers), the hardest one would have had the character at {:.0f} studs a second, the furthest {:.1f} studs from where it stood; {} part{} kept from touching it. Over because {}",
			guard.undone, guard.undone == 1 ? "" : "s", std::chrono::duration<double>(guard.last_throw - guard.attack_began).count(),
			guard.kinds[0], guard.kinds[1], guard.kinds[2], guard.kinds[3],
			guard.worst_speed, guard.furthest, kept_off.size(), kept_off.size() == 1 ? "" : "s", ended);

		guard.undone = 0;
		guard.worst_speed = 0.0f;
		guard.last_throw = never;
		guard.furthest = 0.0f;
		for (int& kind : guard.kinds)
		{
			kind = 0;
		}
	}

	constexpr auto stretch = 25ms;

	constexpr auto stay_out_for = 3s;

	struct seat_watch_t
	{
		std::uint64_t root = 0;

		math::vector3 from{};
		steady::time_point from_at = never;

		int fast = 0;

		bool has_before = false;
		pv_t before{};

		bool has_calm = false;
		pv_t calm{};

		steady::time_point out_until = never;

		std::vector<std::uint64_t> car;
		std::vector<pv_t> car_good;
		steady::time_point car_good_at = never;

		std::uint64_t seat_primitive = 0;

		math::vector3 last_position{};
		steady::time_point moved_at = never;
		float step_speed = 0.0f;

		steady::time_point car_throw_at = never;
		bool has_car_back = false;
		pv_t car_back{};
	};

	// gather a models parts recursively
	void collect_parts(std::uint64_t instance, int depth, std::vector<std::uint64_t>& parts)
	{
		if (depth > 6 || parts.size() > 400)
			return;

		for (const rbx::instance_t& child : rbx::instance_t{ instance }.get_children())
		{
			const std::uint64_t primitive = memory->read<std::uint64_t>(child.address + Offsets::BasePart::Primitive);
			if (primitive != 0 && memory->read<std::uint64_t>(primitive + Offsets::Primitive::Owner) == child.address)
			{
				parts.push_back(primitive);
			}
			collect_parts(child.address, depth + 1, parts);
		}
	}

	// the parts of a seats vehicle
	std::vector<std::uint64_t> vehicle_parts(std::uint64_t seat_part)
	{
		std::uint64_t model = 0;
		std::uint64_t at = memory->read<std::uint64_t>(seat_part + Offsets::Instance::Parent);
		for (int level = 0; level < 8 && at != 0 && at != game::workspace.address; level++)
		{
			if (rbx::instance_t{ at }.get_class_name() != "Model")
				break;
			model = at;
			at = memory->read<std::uint64_t>(at + Offsets::Instance::Parent);
		}

		std::vector<std::uint64_t> parts;
		if (model != 0)
		{
			collect_parts(model, 0, parts);
		}
		if (parts.size() > 400)
		{
			parts.clear();
		}
		return parts;
	}

	// press or release the space key
	void press_space(bool down)
	{
		INPUT input{};
		input.type = INPUT_KEYBOARD;
		input.ki.wVk = VK_SPACE;
		input.ki.wScan = static_cast<WORD>(MapVirtualKeyW(VK_SPACE, MAPVK_VK_TO_VSC));
		input.ki.dwFlags = down ? 0 : KEYEVENTF_KEYUP;
		SendInput(1, &input, sizeof(INPUT));
	}

	// true if roblox has keyboard focus
	bool game_has_keyboard()
	{
		const HWND roblox = game::get_roblox_window();
		return roblox != nullptr && GetForegroundWindow() == roblox;
	}

	// make the character get out of its seat
	bool leave_seat(std::uint64_t humanoid, steady::time_point give_up_at)
	{
		const steady::time_point began = steady::now();
		steady::time_point space_down = never;
		steady::time_point space_up = never;
		bool key_used = false;
		bool out = false;

		for (;;)
		{
			if (!sits(humanoid))
			{
				out = true;
				break;
			}

			const steady::time_point now = steady::now();
			if (!g_running || now >= give_up_at || stabilizer::humanoid != humanoid)
				break;

			memory->write<std::uint8_t>(humanoid + Offsets::Humanoid::Jump, 1);

			if (space_down != never)
			{
				if (now - space_down >= 40ms)
				{
					press_space(false);
					space_down = never;
					space_up = now;
				}
			}

			else if (now - began >= 250ms && (space_up == never || now - space_up >= 150ms) && game_has_keyboard() && !game::typing_in_chat())
			{
				press_space(true);
				space_down = now;
				key_used = true;
			}
		}

		if (space_down != never)
		{
			press_space(false);
		}

		const double took = std::chrono::duration<double, std::milli>(steady::now() - began).count();
		if (out)
		{
			AF_SAY("anti fling: out of the seat after {:.0f} ms{}", took, key_used ? ", it took the space key" : "");
		}
		else
		{
			AF_WARN("anti fling: still in the seat after {:.0f} ms of trying to jump out{}{}", took, key_used ? ", space key included" : "",
				stabilizer::humanoid != humanoid ? " (the character is no longer the one that sat)" : !g_running ? " (the program is closing)" : "");
		}

		return out;
	}

	struct put_back_t
	{
		std::uint64_t primitive = 0;
		pv_t pv{};
		steady::time_point at = never;
	};

	put_back_t last_back;

	struct back_check_t
	{
		std::uint64_t primitive = 0;
		math::vector3 spot{};

		float taken = -1.0f;
		steady::time_point began = never;
		steady::time_point due = never;
	};

	back_check_t back_check;

	constexpr auto written_for = 30ms;

	constexpr auto held_for = 300ms;

	// move a flung part back where it stood
	void put_back(std::uint64_t root, pv_t back, const math::vector3& taken_to)
	{
		back.linear = {};
		back.angular = {};

		const steady::time_point began = steady::now();
		while (g_running && steady::now() - began < written_for)
		{
			write_pv(root, back);
		}

		const steady::time_point now = steady::now();
		last_back = { root, back, now };
		back_check = { root, back.position, is_finite(taken_to) ? taken_to.distance(back.position) : -1.0f, began, now + 150ms };

		stabilizer::put_backs++;
	}

	// log a put-back
	void report_put_back(steady::time_point now)
	{
		if (back_check.due == never || now < back_check.due)
			return;

		const back_check_t check = back_check;
		back_check = {};

		if (stabilizer::root_primitive != check.primitive)
			return;

		const math::vector3 position = memory->read<math::vector3>(check.primitive + Offsets::Primitive::Position);
		const float off = is_finite(position) ? position.distance(check.spot) : 1.0e9f;
		const double after = std::chrono::duration<double, std::milli>(now - check.began).count();

		if (off <= 30.0f)
		{
			AF_SAY("anti fling: back where it stood, at ({:.0f}, {:.0f}, {:.0f}). The vehicle had taken the character {:.0f} studs away, {:.0f} ms later it is {:.1f} studs from the spot",
				check.spot.x, check.spot.y, check.spot.z, check.taken, after, off);
		}
		else
		{
			AF_WARN("anti fling: putting the character back did not hold. The vehicle had taken it {:.0f} studs away, {:.0f} ms later it is at ({:.0f}, {:.0f}, {:.0f}), {:.0f} studs from where it stood at ({:.0f}, {:.0f}, {:.0f})",
				check.taken, after, position.x, position.y, position.z, off, check.spot.x, check.spot.y, check.spot.z);
		}
	}

	// resume guarding after a put-back
	void resume_from(guard_t& guard, const put_back_t& back, steady::time_point now)
	{
		guard.good = back.pv;
		guard.good_at = now;
		guard.seen = back.pv;
		guard.has_good = true;
		guard.has_before_jump = false;
		guard.teleported = false;
		guard.held_until = back.at + held_for;
	}

	// handle returning from a seat
	void back_from_seat(guard_t& guard, std::uint64_t root, steady::time_point now)
	{
		pv_t stood{};
		if (!where_it_stood(root, now, 0ms, stood))
			return;

		const math::vector3 position = memory->read<math::vector3>(root + Offsets::Primitive::Position);
		const float seconds = (std::max)(std::chrono::duration<float>(now - stood_last).count(), 0.001f);
		const float away = is_finite(position) ? position.distance(stood.position) : 1.0e9f;
		if (away < 20.0f || away / seconds <= stabilizer::vehicle_speed())
			return;

		AF_SAY("anti fling: the character got up from a seat {:.0f} studs from where it stood {:.0f} ms before, faster than a vehicle drives: a thrown vehicle. Put back to ({:.0f}, {:.0f}, {:.0f})",
			away, seconds * 1000.0f, stood.position.x, stood.position.y, stood.position.z);

		put_back(root, stood, position);
		resume_from(guard, last_back, steady::now());
	}

	// force the character out of a seat
	void get_out(const seat_watch_t& seat, std::uint64_t root, std::uint64_t humanoid, const pv_t* back)
	{
		if (!leave_seat(humanoid, seat.out_until) || back == nullptr)
			return;

		const math::vector3 taken_to = memory->read<math::vector3>(root + Offsets::Primitive::Position);
		put_back(root, *back, taken_to);
	}

	constexpr float parked_within = 2.0f;

	constexpr float within_reach = 15.0f;
	constexpr auto in_reach_for = 250ms;

	// the CanTouch bit value
	std::uint8_t touch_bit()
	{
		return static_cast<std::uint8_t>(Offsets::PrimitiveFlags::CanTouch);
	}

	struct seat_track_t
	{

		math::vector3 from{};
		steady::time_point from_at = never;

		steady::time_point thrown_until = never;
		int fast_passes = 0;

		math::vector3 parked_at{};
		steady::time_point parked_since = never;
		steady::time_point in_reach_since = never;

		bool locked = false;

		// the character
		bool open = true;

		steady::time_point checked = never;
	};

	std::unordered_map<std::uint64_t, seat_track_t> seat_tracks;

	std::shared_ptr<const std::vector<std::uint64_t>> tracked_seats;
	steady::time_point last_seat_pass = never;

	// stop a seat from grabbing the character
	void lock_seat(std::uint64_t primitive, seat_track_t& track, steady::time_point now)
	{
		track.checked = now;
		track.open = false;

		if (!is_alive(primitive))
			return;

		const std::uint8_t flags = memory->read<std::uint8_t>(primitive + Offsets::Primitive::Flags);
		if ((flags & touch_bit()) == 0)
			return;

		memory->write<std::uint8_t>(primitive + Offsets::Primitive::Flags, static_cast<std::uint8_t>(flags & ~touch_bit()));

		if (track.locked)
		{
			tally.relocked++;
			AF_SAY_EVERY(10000, "anti fling: the game had switched the touch of a locked seat back on, it is off again");
		}

		track.locked = true;
	}

	// let a seat work again
	void unlock_seat(std::uint64_t primitive, seat_track_t& track)
	{
		track.open = true;

		if (!track.locked)
			return;
		track.locked = false;

		if (!is_alive(primitive))
			return;

		const std::uint8_t flags = memory->read<std::uint8_t>(primitive + Offsets::Primitive::Flags);
		memory->write<std::uint8_t>(primitive + Offsets::Primitive::Flags, static_cast<std::uint8_t>(flags | touch_bit()));
	}

	// unlock every tracked seat
	void unlock_all_seats()
	{
		for (auto& [primitive, track] : seat_tracks)
		{
			unlock_seat(primitive, track);
		}

		seat_tracks.clear();
		tracked_seats = nullptr;
		stabilizer::seats_locked = 0;
	}

	// watch nearby vehicles for seat-grab flings
	void watch_vehicles(steady::time_point now)
	{
		if (now - last_seat_pass < stretch)
			return;
		last_seat_pass = now;

		const std::shared_ptr<const std::vector<std::uint64_t>> seats = get_seat_parts();
		if (!seats)
		{

			unlock_all_seats();
			stabilizer::seats_watched = 0;
			stabilizer::seats_thrown = 0;
			nearest_seat = {};
			seats_may_take = !settings::mover::stabilizer::lock_seats;
			return;
		}

		if (seats != tracked_seats)
		{
			std::unordered_map<std::uint64_t, seat_track_t> kept;
			kept.reserve(seats->size());
			for (const std::uint64_t primitive : *seats)
			{
				const auto known = seat_tracks.find(primitive);
				if (known != seat_tracks.end())
				{
					kept.emplace(primitive, known->second);
					seat_tracks.erase(known);
				}
			}

			for (auto& [primitive, track] : seat_tracks)
			{
				unlock_seat(primitive, track);
			}

			seat_tracks.swap(kept);
			tracked_seats = seats;
		}

		const bool locking = settings::mover::stabilizer::lock_seats;

		const std::uint64_t guarded = stabilizer::root_primitive;
		const std::uint64_t own = guarded != 0 ? guarded : stabilizer::seated_root.load();
		const math::vector3 own_position = own != 0 ? memory->read<math::vector3>(own + Offsets::Primitive::Position) : math::vector3{};
		const bool has_own = own != 0 && is_finite(own_position);

		const float limit = stabilizer::vehicle_speed();
		int thrown = 0;
		int newly_thrown = 0;
		int locked = 0;
		int opened = 0;
		int open_now = 0;
		float fastest = 0.0f;

		float thrown_away = -1.0f;
		nearest_seat_t nearest;

		for (const std::uint64_t primitive : *seats)
		{
			const math::vector3 position = memory->read<math::vector3>(primitive + Offsets::Primitive::Position);
			seat_track_t& track = seat_tracks[primitive];
			const bool lost = !is_finite(position);

			const float away = !lost && has_own ? position.distance(own_position) : -1.0f;

			bool standing = false;

			if (track.from_at != never)
			{
				const float seconds = std::chrono::duration<float>(now - track.from_at).count();
				const float sideways = std::hypot(position.x - track.from.x, position.z - track.from.z) / seconds;
				const float upwards = (position.y - track.from.y) / seconds;
				const float speed = (std::max)(sideways, upwards);

				standing = !lost && std::abs(upwards) < 15.0f && sideways < 6.0f;

				if (!lost)
				{
					tally.fastest_seat = (std::max)(tally.fastest_seat, speed);
				}

				const bool fast = lost || speed > limit;
				track.fast_passes = fast ? track.fast_passes + 1 : 0;

				if (fast && track.fast_passes >= 2 && !riding.contains(primitive))
				{
					if (now >= track.thrown_until)
					{
						newly_thrown++;
					}
					track.thrown_until = now + stay_out_for;
					fastest = (std::max)(fastest, lost ? 1.0e9f : speed);

					if (away >= 0.0f && (thrown_away < 0.0f || away < thrown_away))
					{
						thrown_away = away;
					}
				}
			}

			if (!lost)
			{
				track.from = position;
			}
			track.from_at = now;

			if (now < track.thrown_until)
			{
				thrown++;
			}

			if (lost)
			{
				track.parked_since = never;
			}
			else if (track.parked_since == never || position.distance(track.parked_at) > parked_within)
			{
				track.parked_at = position;
				track.parked_since = now;
			}

			const bool in_reach = away >= 0.0f && away < within_reach;
			if (!in_reach)
			{
				track.in_reach_since = never;
			}
			else if (track.in_reach_since == never)
			{
				track.in_reach_since = now;
			}

			const bool may_take = !locking ||
				(now >= track.thrown_until && standing &&
				 track.in_reach_since != never && now - track.in_reach_since >= in_reach_for);

			if (may_take)
			{
				if (track.locked)
				{
					opened++;
				}
				unlock_seat(primitive, track);
				open_now++;
			}
			else if (track.open || now - track.checked >= 1s)
			{

				lock_seat(primitive, track, now);
			}

			if (track.locked)
			{
				locked++;
			}

			if (away >= 0.0f && (nearest.distance < 0.0f || away < nearest.distance))
			{
				nearest.distance = away;
				nearest.locked = track.locked;
				nearest.thrown = now < track.thrown_until;
				nearest.parked = track.parked_since != never ? std::chrono::duration<float>(now - track.parked_since).count() : 0.0f;
				nearest.in_reach = track.in_reach_since != never ? std::chrono::duration<float>(now - track.in_reach_since).count() : 0.0f;
			}
		}

		nearest_seat = nearest;
		seats_may_take = !locking || open_now > 0;

		if (locked > 0 && stabilizer::seats_locked == 0)
		{
			AF_SAY("anti fling: {} of {} vehicle seats cannot take the character from now on, only one that is parked next to it can", locked, seats->size());
		}
		if (opened > 0)
		{
			AF_SAY_EVERY(5000, "anti fling: {} seat{} of a vehicle parked next to the character can take it again, the nearest seat is {:.0f} studs away",
				opened, opened == 1 ? "" : "s", nearest.distance);
		}

		stabilizer::seats_watched = static_cast<int>(seats->size());
		stabilizer::seats_thrown = thrown;
		stabilizer::seats_locked = locked;

		if (newly_thrown > 0)
		{
			AF_SAY_EVERY(2000, "anti fling: {} seat{} moving at up to {:.0f} studs a second, more than the {:.0f} a vehicle may do: a thrown vehicle, its nearest seat {:.0f} studs from the character. The character will not stay in it",
				newly_thrown, newly_thrown == 1 ? "" : "s", fastest, limit, thrown_away);
		}
	}

	// lock empty seats on moving vehicles
	void lock_moving_open_seats(steady::time_point now)
	{
		if (!settings::mover::stabilizer::lock_seats)
			return;

		for (auto& [primitive, track] : seat_tracks)
		{

			if (!track.open || track.parked_since == never || riding.contains(primitive))
				continue;

			const math::vector3 position = memory->read<math::vector3>(primitive + Offsets::Primitive::Position);
			if (is_finite(position) && position.distance(track.parked_at) <= parked_within)
				continue;

			track.parked_at = position;
			track.parked_since = is_finite(position) ? now : never;
			lock_seat(primitive, track, now);
			seats_may_take = false;

			AF_SAY_EVERY(2000, "anti fling: a vehicle parked next to the character moved off, its seats cannot take the character any more");
		}
	}

	constexpr auto car_snapshot_every = 10ms;

	// guard against a vehicle fling
	bool guard_vehicle(seat_watch_t& seat, std::uint64_t root, std::uint64_t humanoid, steady::time_point now)
	{
		if (seat.car.empty() || seat.seat_primitive == 0)
			return false;

		const math::vector3 position = memory->read<math::vector3>(seat.seat_primitive + Offsets::Primitive::Position);
		const bool lost = !is_finite(position);

		const float limit = stabilizer::vehicle_speed();
		bool thrown = false;
		float step = 0.0f;

		float stepped = 0.0f;
		float step_ms = 0.0f;

		if (lost || seat.moved_at == never || position.distance(seat.last_position) > 0.0f)
		{
			if (seat.moved_at != never)
			{

				const float seconds = std::clamp(std::chrono::duration<float>(now - seat.moved_at).count(), 0.004f, 0.025f);
				stepped = lost ? 0.0f : position.distance(seat.last_position);
				step_ms = seconds * 1000.0f;

				const float sideways = std::hypot(position.x - seat.last_position.x, position.z - seat.last_position.z) / seconds;
				const float upwards = (position.y - seat.last_position.y) / seconds;
				step = lost ? 1.0e9f : (std::max)(sideways, upwards);

				thrown = lost || step > limit * 10.0f;
			}

			if (!thrown)
			{
				seat.step_speed = step;
				seat.last_position = position;
				seat.moved_at = now;
			}
		}

		if (!thrown)
		{

			const bool honest = step <= limit * 1.5f && (seat.car_throw_at == never || now - seat.car_throw_at > 500ms);
			if (honest && now - seat.car_good_at >= car_snapshot_every)
			{
				seat.car_good.resize(seat.car.size());
				for (std::size_t i = 0; i < seat.car.size(); i++)
				{
					seat.car_good[i] = read_pv(seat.car[i]);
				}
				seat.car_good_at = now;
			}
			return false;
		}

		if (seat.has_car_back && now - seat.car_throw_at < 500ms)
		{
			seat.out_until = now + stay_out_for;
			stabilizer::vehicles_left++;

			AF_SAY("anti fling: the vehicle the character sits in was thrown again, {:.0f} studs a second, {:.0f} ms after it was put back: another player's game moves it. Getting out, back to ({:.0f}, {:.0f}, {:.0f}), and staying out of seats for {} s",
				step, std::chrono::duration<float, std::milli>(now - seat.car_throw_at).count(),
				seat.car_back.position.x, seat.car_back.position.y, seat.car_back.position.z,
				std::chrono::duration_cast<std::chrono::seconds>(stay_out_for).count());

			const pv_t back = seat.car_back;
			get_out(seat, root, humanoid, &back);
			return true;
		}

		if (seat.car_good.size() != seat.car.size() || now - seat.car_good_at > 2s)
			return false;

		int written = 0;
		for (std::size_t i = 0; i < seat.car.size(); i++)
		{
			if (is_finite(seat.car_good[i]) && is_alive(seat.car[i]))
			{
				write_pv(seat.car[i], seat.car_good[i]);
				written++;
			}
		}

		seat.car_back = seat.car_good.back();
		seat.car_back.linear = {};
		seat.car_back.angular = {};
		seat.has_car_back = true;
		seat.car_throw_at = now;

		seat.moved_at = never;
		seat.step_speed = 0.0f;

		seat.from = seat.car_back.position;
		seat.from_at = now;
		seat.fast = 0;

		tally.throws++;
		stabilizer::throws_stopped++;

		AF_SAY_EVERY(1000, "anti fling: the vehicle the character sits in was thrown, {:.0f} studs a second in one physics step ({:.1f} studs in {:.1f} ms). All {} of its parts are put back where they were {:.0f} ms before, at ({:.0f}, {:.0f}, {:.0f})",
			step, stepped, step_ms, written, std::chrono::duration<float, std::milli>(now - seat.car_good_at).count(),
			seat.car_back.position.x, seat.car_back.position.y, seat.car_back.position.z);
		return true;
	}

	// watch the character while seated
	void watch_seated(seat_watch_t& seat, std::uint64_t root, std::uint64_t humanoid)
	{
		const steady::time_point now = steady::now();

		if (part_is_held())
			return;

		if (now < seat.out_until)
		{
			const bool known = last_back.primitive == root && now - last_back.at < stay_out_for + 1s;
			const pv_t back = last_back.pv;
			get_out(seat, root, humanoid, known ? &back : nullptr);
			return;
		}

		const pv_t pv = read_pv(root);
		const math::vector3& position = pv.position;

		const bool lost = !is_finite(position);

		if (!lost && root != seat.root)
		{

			seat.root = root;
			seat.from = position;
			seat.from_at = now;
			seat.fast = 0;
			seat.has_calm = false;

			seat.has_before = where_it_stood(root, now, 30ms, seat.before);

			const std::uint64_t seat_part = memory->read<std::uint64_t>(humanoid + Offsets::Humanoid::SeatPart);
			const std::uint64_t seat_primitive = seat_part != 0 ? memory->read<std::uint64_t>(seat_part + Offsets::BasePart::Primitive) : 0;
			const auto known = seat_tracks.find(seat_primitive);
			const bool was_locked = known != seat_tracks.end() && known->second.locked;

			const bool own_car = riding.contains(seat_primitive);

			seat.car = seat_part != 0 ? vehicle_parts(seat_part) : std::vector<std::uint64_t>{};
			riding.clear();
			riding.insert(seat.car.begin(), seat.car.end());
			const auto seat_in_car = std::find(seat.car.begin(), seat.car.end(), seat_primitive);
			seat.seat_primitive = seat_in_car != seat.car.end() ? seat_primitive : 0;
			if (!seat.car.empty())
			{
				seat.car.push_back(root);
			}
			seat.car_good.clear();
			seat.car_good_at = never;
			seat.moved_at = never;
			seat.step_speed = 0.0f;
			seat.car_throw_at = never;
			seat.has_car_back = false;

			if (known != seat_tracks.end() && now < known->second.thrown_until)
			{
				seat.out_until = now + stay_out_for;
				stabilizer::vehicles_left++;

				AF_SAY("anti fling: the character was sat down in a vehicle that is being thrown{}, at ({:.0f}, {:.0f}, {:.0f}). Getting out{}, and staying out of seats for {} s",
					was_locked ? ", by a seat whose touch was switched off here" : "",
					position.x, position.y, position.z,
					seat.has_before ? " and back to where it stood" : " (where it stood before is not known)",
					std::chrono::duration_cast<std::chrono::seconds>(stay_out_for).count());

				get_out(seat, root, humanoid, seat.has_before ? &seat.before : nullptr);
			}
			else if (was_locked && !own_car)
			{

				seat.out_until = now + stay_out_for;
				stabilizer::vehicles_left++;

				AF_SAY("anti fling: a seat took the character although its touch was switched off here, at ({:.0f}, {:.0f}, {:.0f}): not a vehicle parked next to it. Getting out{}, and staying out of seats for {} s",
					position.x, position.y, position.z,
					seat.has_before ? " and back to where it stood" : " (where it stood before is not known)",
					std::chrono::duration_cast<std::chrono::seconds>(stay_out_for).count());

				get_out(seat, root, humanoid, seat.has_before ? &seat.before : nullptr);
			}
			else
			{
				AF_SAY("anti fling: the character sat down at ({:.0f}, {:.0f}, {:.0f}), in {}. Where the seat takes it is watched from here on",
					position.x, position.y, position.z,
					known != seat_tracks.end() ? "a vehicle seat that was left able to take it" : "a seat that is not among the vehicle seats being watched");
			}

			return;
		}

		if (guard_vehicle(seat, root, humanoid, now))
			return;

		if (!lost && now - seat.from_at < stretch)
			return;

		const float seconds = std::chrono::duration<float>(now - seat.from_at).count();
		const float sideways = std::hypot(position.x - seat.from.x, position.z - seat.from.z) / seconds;
		const float upwards = (position.y - seat.from.y) / seconds;

		const float limit = stabilizer::vehicle_speed() * 1.5f;
		const float speed = (std::max)(sideways, upwards);

		seat.from = position;
		seat.from_at = now;

		if (!lost)
		{
			tally.position = position;
			tally.carried = (std::max)(tally.carried, speed);
		}

		if (!lost && !(speed > limit))
		{
			seat.fast = 0;
			seat.calm = pv;
			seat.has_calm = true;
			return;
		}

		if (!lost && speed < stabilizer::vehicle_speed() * 4.0f && ++seat.fast < 2)
		{

			AF_SAY_EVERY(1000, "anti fling: the vehicle carried the character at {:.0f} studs a second over the last {:.0f} ms ({:.0f} sideways, {:.0f} upwards), more than the {:.0f} it is let do. Once more and it gets out",
				speed, seconds * 1000.0f, sideways, upwards, limit);
			return;
		}

		seat.fast = 0;
		seat.out_until = now + stay_out_for;
		stabilizer::vehicles_left++;

		const pv_t* back = seat.has_calm ? &seat.calm : seat.has_before ? &seat.before : nullptr;

		AF_SAY("anti fling: the vehicle is carrying the character at {:.0f} studs a second ({:.0f} sideways, {:.0f} upwards), more than the {:.0f} it is let do. Getting out{}, and staying out of seats for {} s",
			lost ? 1.0e9f : speed, sideways, upwards, limit,
			back == nullptr ? " (there is nowhere known to put it back to)" : seat.has_calm ? " and back to where the ride was last an honest one" : " and back to where it stood before the seat took it",
			std::chrono::duration_cast<std::chrono::seconds>(stay_out_for).count());

		get_out(seat, root, humanoid, back);
	}
}

namespace
{

	namespace humanoid_state
	{
		constexpr std::uint16_t falling_down = 0;
		constexpr std::uint16_t ragdoll = 1;
		constexpr std::uint16_t landed = 7;
		constexpr std::uint16_t running = 8;
		constexpr std::uint16_t running_no_physics = 10;
		constexpr std::uint16_t climbing = 12;
		constexpr std::uint16_t platform_standing = 14;
		constexpr std::uint16_t physics = 16;
	}

	// readable name for a humanoid state
	const char* state_name(std::uint16_t state)
	{
		static const char* const names[] = { "falling over", "ragdoll", "getting up", "jumping", "swimming", "falling", "flying", "landed",
			"running", "?", "running", "strafing", "climbing", "seated", "standing limp", "dead", "physics", "?", "none" };
		return state < std::size(names) ? names[state] : "an unknown state";
	}

	struct upright_t
	{
		std::uint64_t primitive = 0;
		steady::time_point last_look = never;

		bool known = false;
		math::matrix3 rotation{};

		steady::time_point down_since = never;
		int stood_up = 0;

		std::uint16_t last_state = 0xFFFF;
	};

	upright_t upright;

	constexpr std::uint64_t state_row_before_walking = 0x13;
	constexpr int state_count = 19;
	constexpr int states_switched_off[] = { humanoid_state::falling_down, humanoid_state::ragdoll, humanoid_state::physics };

	struct locked_t
	{
		std::uint64_t humanoid = 0;

		std::uint8_t original[std::size(states_switched_off)]{};
		bool told_not_a_row = false;
	};

	locked_t locked;

	// address of the humanoid state row
	std::uint64_t state_row(std::uint64_t humanoid)
	{
		return humanoid + Offsets::Humanoid::IsWalking - state_row_before_walking;
	}

	// undo the first-run state setup
	void undo_first_build(std::uint64_t humanoid)
	{
		const std::uint64_t misplaced = state_row(humanoid) - 4;
		std::uint8_t before[2]{};
		memory->read_buffer(misplaced, before, sizeof(before));
		const std::uint8_t climbing = memory->read<std::uint8_t>(state_row(humanoid) + humanoid_state::climbing);

		if (before[0] != 0 || before[1] != 0 || climbing != 0)
			return;

		memory->write<std::uint8_t>(misplaced, 1);
		memory->write<std::uint8_t>(misplaced + 1, 1);
		memory->write<std::uint8_t>(state_row(humanoid) + humanoid_state::climbing, 1);
		AF_SAY("anti fling: the character still had climbing switched off by the build before, switched back on");
	}

	// sanity check the humanoid state bytes
	bool row_is_right(const std::uint8_t (&row)[state_count])
	{
		int on = 0;
		for (int i = 0; i < state_count; i++)
		{
			if (row[i] > 1)
				return false;
			if (i <= humanoid_state::physics)
				on += row[i];
		}
		return on >= 6;
	}

	// disable the states that allow flinging
	void switch_states_off(std::uint64_t humanoid)
	{
		std::uint8_t row[state_count]{};
		memory->read_buffer(state_row(humanoid), row, sizeof(row));

		if (!row_is_right(row))
		{
			if (!locked.told_not_a_row)
			{
				locked.told_not_a_row = true;
				AF_WARN("anti fling: the humanoid's row of states is not where it was found before (the offsets may have moved), so ragdolling is only undone, not prevented");
			}
			return;
		}

		if (humanoid != locked.humanoid)
		{
			locked.humanoid = humanoid;
			for (std::size_t i = 0; i < std::size(states_switched_off); i++)
			{
				locked.original[i] = row[states_switched_off[i]];
			}

			undo_first_build(humanoid);

			AF_SAY("anti fling: falling over, ragdoll and the physics state are switched off for the character, it cannot enter them any more");
		}

		for (const int state : states_switched_off)
		{
			if (row[state] != 0)
			{
				memory->write<std::uint8_t>(state_row(humanoid) + state, 0);
			}
		}
	}

	// re-enable the humanoid states
	void give_states_back(std::uint64_t current_humanoid)
	{
		if (locked.humanoid == 0)
			return;

		if (locked.humanoid == current_humanoid)
		{
			for (std::size_t i = 0; i < std::size(states_switched_off); i++)
			{
				memory->write<std::uint8_t>(state_row(locked.humanoid) + states_switched_off[i], locked.original[i]);
			}
		}

		locked.humanoid = 0;
	}

	constexpr int seated_state = 13;

	struct sitting_switch_t
	{
		std::uint64_t humanoid = 0;

		bool off = false;
	};

	sitting_switch_t sitting_switch;

	// toggle whether the character may sit
	void allow_sitting(std::uint64_t humanoid, bool allowed)
	{
		if (humanoid == 0)
			return;

		if (humanoid != sitting_switch.humanoid)
		{
			sitting_switch = { humanoid, false };
		}

		const std::uint64_t at = state_row(humanoid) + seated_state;
		const std::uint8_t now = memory->read<std::uint8_t>(at);

		if (allowed)
		{

			if (now == 0)
			{
				std::uint8_t row[state_count]{};
				memory->read_buffer(state_row(humanoid), row, sizeof(row));
				if (row_is_right(row))
				{
					memory->write<std::uint8_t>(at, 1);
					if (!sitting_switch.off)
					{
						AF_SAY("anti fling: the character's Seated state was off, left so by an earlier run of this program: on again");
					}
				}
			}
			sitting_switch.off = false;
			return;
		}

		if (now == 0)
		{

			return;
		}

		std::uint8_t row[state_count]{};
		memory->read_buffer(state_row(humanoid), row, sizeof(row));
		if (!row_is_right(row))
			return;

		const bool first = !sitting_switch.off;
		memory->write<std::uint8_t>(at, 0);
		sitting_switch.off = true;

		if (first)
		{
			AF_SAY_EVERY(10000, "anti fling: the character cannot sit down, no vehicle is parked next to it (its Seated state is off until one is)");
		}
	}

	// keep the character standing upright
	void keep_upright(std::uint64_t humanoid, std::uint64_t primitive, steady::time_point now)
	{
		if (now - upright.last_look < 5ms || part_is_held())
			return;
		upright.last_look = now;

		if (primitive != upright.primitive)
		{
			upright = {};
			upright.primitive = primitive;
			upright.last_look = now;
		}

		switch_states_off(humanoid);

		const std::uint64_t state_object = memory->read<std::uint64_t>(humanoid + Offsets::Humanoid::HumanoidState);
		const std::uint16_t state = state_object != 0 ? memory->read<std::uint16_t>(state_object + Offsets::Humanoid::HumanoidStateID) : 0xFFFF;
		const bool limp = memory->read<std::uint8_t>(humanoid + Offsets::Humanoid::PlatformStand) != 0;

		if (state == humanoid_state::climbing && upright.last_state != state)
		{
			AF_SAY_EVERY(5000, "anti fling: the character is climbing");
		}
		upright.last_state = state;

		const bool laid_down = limp || state == humanoid_state::falling_down || state == humanoid_state::ragdoll ||
			state == humanoid_state::physics || state == humanoid_state::platform_standing;

		if (!laid_down)
		{
			if (upright.down_since != never)
			{
				AF_SAY("anti fling: the character is on its feet again ({}) after {:.2f} s, it was stood up {} times",
					state_name(state), std::chrono::duration<double>(now - upright.down_since).count(), upright.stood_up);
				upright.down_since = never;
			}

			if (state == humanoid_state::running || state == humanoid_state::running_no_physics || state == humanoid_state::landed)
			{
				const pv_t pv = read_pv(primitive);
				if (is_finite(pv))
				{
					upright.rotation = pv.rotation;
					upright.known = true;
				}
			}
			return;
		}

		if ((state == humanoid_state::falling_down || state == humanoid_state::ragdoll || state == humanoid_state::physics) && locked.humanoid == humanoid)
		{
			AF_WARN_EVERY(10000, "anti fling: the character is in {} although that state is switched off here: the row of states is not where it was thought to be", state_name(state));
		}

		if (upright.down_since == never)
		{
			upright.down_since = now;
			upright.stood_up = 0;

			AF_SAY("anti fling: the game laid the character down ({}{}), standing it up{}", state_name(state), limp ? ", PlatformStand on" : "",
				upright.known ? "" : ". Its heading on its feet is not known yet, only PlatformStand is undone");
		}

		if (limp)
		{
			memory->write<std::uint8_t>(humanoid + Offsets::Humanoid::PlatformStand, 0);
		}

		if (upright.known)
		{
			memory->write<math::matrix3>(primitive + Offsets::Primitive::Rotation, upright.rotation);
			memory->write<math::vector3>(primitive + Offsets::Primitive::AssemblyAngularVelocity, math::vector3{});
		}

		upright.stood_up++;

		if (now - upright.down_since > 3s)
		{
			AF_WARN_EVERY(10000, "anti fling: the character has been down ({}) for {:.0f} s and does not get up", state_name(state),
				std::chrono::duration<double>(now - upright.down_since).count());
		}
	}

	struct body_part_t
	{
		std::uint64_t primitive = 0;
		bool has_good = false;
		pv_t good{};
		steady::time_point good_at = never;

		steady::time_point jumped = never;
	};

	struct body_t
	{
		std::uint64_t model = 0;
		std::vector<body_part_t> parts;
		steady::time_point died = never;
		steady::time_point listed = never;
		steady::time_point last_pass = never;

		// For the log
		int throws = 0;
		int cuts = 0;
		float hardest = 0.0f;
	};

	body_t body;

	// list a models body parts
	void list_body_parts(std::uint64_t model)
	{
		std::vector<body_part_t> parts;

		for (const rbx::instance_t& child : rbx::instance_t{ model }.get_children())
		{
			const std::uint64_t primitive = memory->read<std::uint64_t>(child.address + Offsets::BasePart::Primitive);
			if (primitive == 0 || memory->read<std::uint64_t>(primitive + Offsets::Primitive::Owner) != child.address)
				continue;

			const auto known = std::find_if(body.parts.begin(), body.parts.end(), [&](const body_part_t& part) { return part.primitive == primitive; });
			parts.push_back(known != body.parts.end() ? *known : body_part_t{ primitive });
		}

		body.parts = std::move(parts);
	}

	// guard one body part against flinging
	bool look_at_body_part(body_part_t& part, float top, steady::time_point when)
	{
		pv_t now = read_pv(part.primitive);

		if (!part.has_good)
		{
			if (!is_finite(now))
				return false;

			part.good = now;
			part.good_at = when;
			part.has_good = true;
		}

		if (is_throw(part.good, now, top))
		{
			if (!is_alive(part.primitive))
				return false;

			pv_t back = part.good;
			cut_to(back.linear, top);
			write_pv(part.primitive, back);
			part.good = back;
			part.jumped = never;

			const float speed = now.linear.length();
			body.hardest = (std::max)(body.hardest, std::isfinite(speed) ? speed : 1.0e9f);
			body.throws++;
			stabilizer::throws_stopped++;
			return true;
		}

		if (is_jump(part.good, now, when - part.good_at))
		{
			if (part.jumped == never)
			{
				part.jumped = when;
				return false;
			}
			if (when - part.jumped < halves_apart)
				return false;
		}
		part.jumped = never;

		if (cut_to(now.linear, top) && is_alive(part.primitive))
		{
			memory->write<math::vector3>(part.primitive + Offsets::Primitive::AssemblyLinearVelocity, now.linear);
			body.cuts++;
		}

		part.good = now;
		part.good_at = when;
		return false;
	}

	// finish guarding the body
	void end_body()
	{
		if (body.model == 0)
			return;

		const double seconds = std::chrono::duration<double>(steady::now() - body.died).count();
		if (body.throws > 0 || body.cuts > 0)
		{
			AF_SAY("anti fling: the body was guarded for {:.1f} s, until the character was back: {} throw{} taken back, the hardest at {:.0f} studs a second, {} time{} slowed to the top speed; {} part{} kept from touching it",
				seconds, body.throws, body.throws == 1 ? "" : "s", body.hardest, body.cuts, body.cuts == 1 ? "" : "s", tally.kept_off, tally.kept_off == 1 ? "" : "s");
		}
		else
		{
			AF_SAY("anti fling: the body was guarded for {:.1f} s, until the character was back. Nothing threw it", seconds);
		}

		body = {};
		tally = {};
	}

	// guard all of the characters body parts
	void guard_body(std::uint64_t model)
	{
		const steady::time_point now = steady::now();

		if (model != body.model)
		{
			end_body();
			body.model = model;
			body.died = now;
		}

		if (body.listed == never || now - body.listed >= 250ms)
		{
			body.listed = now;
			list_body_parts(model);

			if (body.parts.empty())
			{
				AF_WARN_EVERY(10000, "anti fling: the body of the character has no parts that can be found, it is not guarded");
			}
		}

		const float top = stabilizer::top_speed();
		for (body_part_t& part : body.parts)
		{
			if (look_at_body_part(part, top, now) && body.throws == 1)
			{
				AF_SAY("anti fling: the body was thrown, at {:.0f} studs a second, {:.1f} s after the character died. Taken back to where it lay",
					body.hardest, std::chrono::duration<double>(now - body.died).count());
			}
		}

		if (now - body.last_pass >= 4ms)
		{
			body.last_pass = now;
			keep_fast_parts_off();
		}

		watch_vehicles(now);
	}

	struct told_t
	{
		stabilizer::standing_t standing = stabilizer::standing_t::off;
		steady::time_point at = never;
		float top = 0.0f;
		float vehicle = 0.0f;
		bool lock_seats = false;
		bool leave_vehicles = false;
	};

	// log the standing state
	void report_standing(told_t& told, steady::time_point now)
	{
		const stabilizer::standing_t standing = stabilizer::standing.load();
		const float top = stabilizer::top_speed();
		const float vehicle = stabilizer::vehicle_speed();
		const bool lock_seats = settings::mover::stabilizer::lock_seats;
		const bool leave_vehicles = settings::mover::stabilizer::leave_vehicles;

		const bool on = standing != stabilizer::standing_t::off;
		const bool new_settings = on && (top != told.top || vehicle != told.vehicle || lock_seats != told.lock_seats || leave_vehicles != told.leave_vehicles);

		if (standing == told.standing && !new_settings)
			return;

		if (told.at != never && now - told.at < 250ms)
			return;

		static const char* const words[] = {
			"switched off, nothing is guarded or watched",
			"there is no character to guard right now: no game, one that is still loading, or between two lives",
			"the character has died, its body is guarded until the next one is there",
			"the character sits. It is not guarded while it does, but watched for a vehicle that is thrown",
			"the character sits. It is not guarded while it does, and not watched either: leaving thrown vehicles is off",
			"guarding the character, it is on its feet",
		};

		if (standing != told.standing)
		{
			AF_SAY("anti fling: {}", words[static_cast<int>(standing)]);
		}

		if (new_settings)
		{
			AF_SAY("anti fling settings: top speed {:.0f}, parked seats only {}, leave thrown vehicles {} (a vehicle may do {:.0f})",
				top, lock_seats ? "on" : "off", leave_vehicles ? "on" : "off", vehicle);
		}

		told.standing = standing;
		told.at = now;

		if (on)
		{
			told.top = top;
			told.vehicle = vehicle;
			told.lock_seats = lock_seats;
			told.leave_vehicles = leave_vehicles;
		}
	}
}

// the configured top speed
float stabilizer::top_speed()
{
	return std::clamp(settings::mover::stabilizer::max_speed, settings::mover::stabilizer::max_speed_min, settings::mover::stabilizer::max_speed_max);
}

// the configured vehicle top speed
float stabilizer::vehicle_speed()
{
	return std::clamp(settings::mover::stabilizer::vehicle_speed, settings::mover::stabilizer::vehicle_speed_min, settings::mover::stabilizer::vehicle_speed_max);
}

// background loop: run the anti-fling guard
void stabilizer::run()
{
	std::thread scanner(scan);

	SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_HIGHEST);

	precise_sleeper sleeper;
	guard_t guard;
	seat_watch_t seat;
	told_t told;
	steady::time_point last_pass = never;

	steady::time_point last_sat = never;

	std::uint64_t off_checked_model = 0;

	while (g_running)
	{
		const std::uint64_t primitive = root_primitive.load();

		report_standing(told, steady::now());

		const std::uint64_t dead_body = standing.load() == standing_t::dead ? character_model.load() : 0;
		if (dead_body == 0 && body.model != 0)
		{
			end_body();
		}

		if (primitive == 0 && dead_body != 0)
		{
			guard = {};
			tally.last_look = never;
			guard_body(dead_body);
			sleeper.sleep_ms(0.5);
			continue;
		}

		if (primitive == 0)
		{

			if (!kept_off.empty())
			{
				let_all_touch_again();
			}

			guard = {};

			if (standing.load() == standing_t::off)
			{
				const std::uint64_t model = game::local_player.address != 0 ? memory->read<std::uint64_t>(game::local_player.address + Offsets::Player::ModelInstance) : 0;
				if (locked.humanoid != 0 || sitting_switch.off || model != off_checked_model)
				{
					off_checked_model = model;
					const std::uint64_t current = model != 0 ? rbx::model_instance_t{ model }.find_first_child_by_class("Humanoid").address : 0;
					give_states_back(current);

					if (current != 0)
					{
						allow_sitting(current, true);
					}
					sitting_switch = {};
				}
			}

			const steady::time_point now = steady::now();
			watch_vehicles(now);

			tally.last_look = never;

			const std::uint64_t root = seated_root.load();
			const std::uint64_t seated = humanoid.load();
			if (root != 0 && seated != 0)
			{
				last_sat = now;
				watch_seated(seat, root, seated);
				report_status(now, "sitting", false);

				sleeper.sleep_ms(0.5);
			}
			else
			{
				seat.root = 0;
				seat.fast = 0;
				riding.clear();

				tally = {};
				std::this_thread::sleep_for(20ms);
			}
			continue;
		}

		if (primitive != guard.primitive)
		{

			guard = {};
			guard.primitive = primitive;
			tally.last_look = never;

			const steady::time_point now = steady::now();
			if (last_back.primitive == primitive && now - last_back.at < 1s)
			{
				resume_from(guard, last_back, now);
			}
			else if (seat.root == primitive)
			{
				back_from_seat(guard, primitive, now);
			}
		}

		const std::uint64_t own_humanoid = humanoid.load();
		if (sits(own_humanoid))
		{
			tally.last_look = never;
			last_sat = steady::now();

			guard.has_good = false;

			if (settings::mover::stabilizer::leave_vehicles)
			{
				const steady::time_point back_before = last_back.at;
				watch_seated(seat, primitive, own_humanoid);

				if (last_back.at != back_before && last_back.primitive == primitive)
				{

					resume_from(guard, last_back, steady::now());
					continue;
				}
			}

			sleeper.sleep_ms(0.5);
			continue;
		}

		seat.root = 0;
		seat.fast = 0;
		if (steady::now() - last_sat > 5s)
		{
			riding.clear();
		}

		if (look(guard))
		{
			defend(guard);
			continue;
		}

		const steady::time_point now = steady::now();
		report_slowing(guard, now);
		report_put_back(now);
		watch_vehicles(now);

		if (now - last_pass >= 4ms)
		{
			last_pass = now;
			keep_fast_parts_off();
		}

		lock_moving_open_seats(now);

		allow_sitting(own_humanoid, seats_may_take || now - last_sat < 2s);

		report_status(now, "on its feet", true);

		if (settings::mover::stabilizer::anti_ragdoll)
		{
			keep_upright(own_humanoid, primitive, now);
		}
		else
		{
			give_states_back(own_humanoid);
		}

		if (is_pushed(guard, now))
			continue;

		sleeper.sleep_ms(0.5);
	}

	let_all_touch_again();
	unlock_all_seats();

	if (humanoid.load() != 0)
	{
		allow_sitting(humanoid.load(), true);
	}
	scanner.join();
}
