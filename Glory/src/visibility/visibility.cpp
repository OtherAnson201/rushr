#include "visibility.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

#include <globals.h>
#include <game/game.h>
#include <logger/logger.h>
#include <memory/memory.h>
#include <Offsets/Offsets.hpp>
#include <render/assets/settings.h>

namespace
{
	enum class kind_t
	{
		other,
		part,
		humanoid
	};

	struct scan_stats_t
	{
		std::size_t instances = 0;
		std::size_t parts = 0;
		std::size_t not_collidable = 0;
		std::size_t too_small = 0;
		std::size_t characters = 0;
	};

	struct scan_t
	{
		std::vector<rbx::obb> obstacles;
		scan_stats_t stats;

		std::unordered_map<std::uint64_t, kind_t> classes;
	};

	// true if the class name is a BasePart
	bool is_basepart_class(const std::string& className)
	{
		return className == "BasePart" ||
			className == "Part" ||
			className == "MeshPart" ||
			className == "WedgePart" ||
			className == "CornerWedgePart" ||
			className == "Ball" ||
			className == "Cylinder" ||
			className == "UnionOperation" ||
			className == "IntersectOperation" ||
			className == "TrussPart" ||
			className == "TriangleMeshPart" ||
			className == "SmoothVoxelPart" ||
			className == "PartOperation" ||
			className == "SpawnLocation" ||
			className == "Seat" ||
			className == "VehicleSeat";
	}

	// sort an instance into part/model/other
	kind_t classify(std::uint64_t instance, scan_t& scan)
	{
		std::uint64_t descriptor = memory->read<std::uint64_t>(instance + Offsets::Instance::ClassDescriptor);

		auto known = scan.classes.find(descriptor);
		if (known != scan.classes.end())
			return known->second;

		const std::string name = rbx::instance_t{ instance }.get_class_name();
		const kind_t kind = is_basepart_class(name) ? kind_t::part : (name == "Humanoid" ? kind_t::humanoid : kind_t::other);

		scan.classes[descriptor] = kind;
		return kind;
	}

	// the child instance addresses of a parent
	std::vector<std::uint64_t> read_child_addresses(std::uint64_t parent)
	{
		std::vector<std::uint64_t> children;

		std::uint64_t children_struct = memory->read<std::uint64_t>(parent + Offsets::Instance::ChildrenStart);
		if (children_struct == 0)
			return children;

		std::uint64_t start = memory->read<std::uint64_t>(children_struct);
		std::uint64_t end = memory->read<std::uint64_t>(children_struct + Offsets::Instance::ChildrenEnd);
		if (start == 0 || end <= start)
			return children;

		constexpr std::uint64_t entry_size = 16;
		constexpr std::uint64_t max_children = 2000000;

		const std::uint64_t byte_size = end - start;
		if (byte_size % entry_size != 0 || byte_size > max_children * entry_size)
			return children;

		std::vector<std::uint64_t> raw(byte_size / sizeof(std::uint64_t));
		memory->read_buffer(start, raw.data(), byte_size);

		children.reserve(raw.size() / 2);
		for (std::size_t i = 0; i < raw.size(); i += 2)
		{
			if (raw[i] != 0 && raw[i] != 0xFFFFFFFFFFFFFFFF)
			{
				children.push_back(raw[i]);
			}
		}

		return children;
	}

	// add a part to the scan set
	void add_part(std::uint64_t instance, scan_t& scan)
	{
		scan.stats.parts++;

		std::uint64_t prim = memory->read<std::uint64_t>(instance + Offsets::BasePart::Primitive);
		if (prim == 0)
			return;

		std::uint8_t flags = memory->read<std::uint8_t>(prim + Offsets::Primitive::Flags);
		if ((flags & Offsets::PrimitiveFlags::CanCollide) == 0)
		{
			scan.stats.not_collidable++;
			return;
		}

		math::vector3 size = memory->read<math::vector3>(prim + Offsets::Primitive::Size);
		math::vector3 center = memory->read<math::vector3>(prim + Offsets::Primitive::Position);

		const float smallest = (std::min)((std::min)(size.x, size.y), size.z);
		const float largest = (std::max)((std::max)(size.x, size.y), size.z);
		if (!std::isfinite(smallest) || !std::isfinite(largest) || !std::isfinite(center.x) || !std::isfinite(center.y) || !std::isfinite(center.z))
			return;

		if (smallest <= 0.0f || largest < 1.0f || largest > 8000.0f)
		{
			scan.stats.too_small++;
			return;
		}

		scan.obstacles.emplace_back(center, size, memory->read<math::matrix3>(prim + Offsets::Primitive::Rotation));
	}

	// walk the workspace gathering parts
	void collect(std::uint64_t root, scan_t& scan)
	{
		std::vector<std::uint64_t> pending{ root };

		constexpr std::size_t max_instances = 5000000;

		while (!pending.empty() && g_running && scan.stats.instances < max_instances)
		{
			const std::uint64_t parent = pending.back();
			pending.pop_back();

			const std::vector<std::uint64_t> children = read_child_addresses(parent);
			if (children.empty())
				continue;

			std::vector<kind_t> kinds(children.size());
			bool has_humanoid = false;
			for (std::size_t i = 0; i < children.size(); i++)
			{
				kinds[i] = classify(children[i], scan);
				has_humanoid = has_humanoid || kinds[i] == kind_t::humanoid;
			}

			scan.stats.instances += children.size();

			if (has_humanoid && parent != root)
			{
				scan.stats.characters++;
				continue;
			}

			for (std::size_t i = 0; i < children.size(); i++)
			{
				if (kinds[i] == kind_t::part)
				{
					add_part(children[i], scan);
				}

				if (kinds[i] != kind_t::humanoid)
				{
					pending.push_back(children[i]);
				}
			}
		}
	}

	// true if any feature needs wall checks
	bool any_check_enabled()
	{

		const bool random_hitbone = settings::correction::enabled && settings::correction::target_part == settings::correction::random_part;

		return settings::universal::wall_check || settings::visuals::debug_visibility || random_hitbone;
	}
}

// snapshot the workspace parts for ray checks
bool c_visibility::cache_workspace() {
	if (game::datamodel.address == 0) {
		return false;
	}

	rbx::instance_t workspace = game::datamodel.find_first_child_by_class("Workspace");
	if (workspace.address == 0) {
		return false;
	}

	auto started = std::chrono::steady_clock::now();

	scan_t scan;
	collect(workspace.address, scan);

	auto took = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - started).count();

	LOG_EVERY(60000, logger::level::info,
		"wallcheck scan: {} walls from {} parts ({} walk-through, {} tiny), {} instances, {} characters skipped, {} ms",
		scan.obstacles.size(), scan.stats.parts, scan.stats.not_collidable, scan.stats.too_small,
		scan.stats.instances, scan.stats.characters, took);

	if (scan.obstacles.empty()) {
		return false;
	}

	auto found = std::make_shared<obstacle_list>(std::move(scan.obstacles));

	std::lock_guard<std::mutex> lock(mutex);
	obstacles = std::move(found);
	return true;
}

// drop the cached workspace
void c_visibility::clear() {
	std::lock_guard<std::mutex> lock(mutex);
	obstacles.reset();
}

// true if nothing blocks the line to the target
bool c_visibility::is_visible(const math::vector3& origin, const math::vector3& target) {
	std::shared_ptr<const obstacle_list> list;
	{
		std::lock_guard<std::mutex> lock(mutex);
		list = obstacles;
	}

	if (!list)
		return true;

	const math::vector3 delta = target - origin;
	const float distance = delta.length();
	if (distance < 0.001f)
		return true;

	const math::vector3 dir = delta * (1.0f / distance);

	for (const rbx::obb& box : *list) {
		if (box.blocks(origin, dir, distance))
			return false;
	}

	return true;
}

// background loop: keep the workspace snapshot fresh
void c_visibility::run() {
	using namespace std::chrono_literals;

	while (g_running) {
		if (game::game_changed) {

			visibility->clear();
		}

		if (!any_check_enabled() || game::game_changed) {
			std::this_thread::sleep_for(250ms);
			continue;
		}

		visibility->cache_workspace();

		for (int i = 0; i < 20 && g_running; i++) {
			std::this_thread::sleep_for(250ms);
		}
	}
}
