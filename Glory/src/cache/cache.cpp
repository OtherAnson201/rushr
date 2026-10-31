#include "cache.h"
#include <thread>
#include <chrono>
#include <algorithm>
#include <unordered_set>
#include <globals.h>
#include <game/game.h>
#include <logger/logger.h>
#include <memory/memory.h>
#include <render/assets/settings.h>
#include <visibility/visibility.h>
#include <features/friends/friends.h>
#include <Offsets/Offsets.hpp>
#include <cmath>
#include <cstring>

// debug: log every players position
static void log_positions(const std::vector<cache::entity_t>& players, std::uint64_t local_player_address)
{
	LOG_INFO("positions of {} players", players.size());

	for (const cache::entity_t& entity : players)
	{
		auto root = entity.parts.find("HumanoidRootPart");
		if (root == entity.parts.end())
		{
			LOG_INFO("  {} has no HumanoidRootPart", entity.name);
			continue;
		}

		math::vector3 position = root->second.get_primitive().get_position();
		LOG_INFO("  {}{} x={:.2f} y={:.2f} z={:.2f} health={:.0f}/{:.0f}",
			entity.name, entity.instance.address == local_player_address ? " (local)" : "",
			position.x, position.y, position.z, entity.health, entity.max_health);
	}
}

// read the teams and their colors from the game
static std::vector<cache::team_t> scan_teams()
{

	static std::uint32_t known_session = 0;
	static rbx::instance_t teams_service{};

	if (game::session != known_session || teams_service.address == 0)
	{
		known_session = game::session;
		teams_service = game::datamodel.find_first_child_by_class("Teams");
	}

	std::vector<cache::team_t> teams;
	if (teams_service.address == 0)
		return teams;

	for (const rbx::instance_t& child : teams_service.get_children())
	{
		if (child.get_class_name() == "Team")
		{
			cache::team_t team{};
			team.address = child.address;
			team.name = child.get_name();
			team.color = memory->read<std::int32_t>(child.address + Offsets::Team::BrickColor);
			teams.push_back(team);
		}
	}

	return teams;
}

static const char* resolve_teams(std::vector<cache::entity_t>& players, const std::vector<std::int32_t>& player_colors,
	std::vector<cache::team_t>& teams, std::uint64_t local_player_address, std::uint64_t& local_team, int& without_team)
{
	const char* matched_by = "no teams in this game";

	std::unordered_map<std::uint64_t, cache::team_t*> by_address;
	for (cache::team_t& team : teams)
	{
		team.players = 0;
		by_address[team.address] = &team;
	}

	if (!teams.empty())
	{

		bool pointers_work = false;
		for (const cache::entity_t& entity : players)
		{
			if (by_address.contains(entity.team))
			{
				pointers_work = true;
				break;
			}
		}

		if (pointers_work)
		{
			matched_by = "team pointer";
		}
		else
		{

			matched_by = "team colour";
			for (std::size_t i = 0; i < players.size(); i++)
			{
				players[i].team = 0;
				for (const cache::team_t& team : teams)
				{
					if (team.color != 0 && team.color == player_colors[i])
					{
						players[i].team = team.address;
						break;
					}
				}
			}
		}
	}

	local_team = 0;
	for (const cache::entity_t& entity : players)
	{
		if (entity.instance.address == local_player_address)
		{
			local_team = entity.team;
			break;
		}
	}

	std::unordered_set<std::string> spotter_off;
	std::unordered_set<std::string> target_off;
	{
		std::lock_guard<std::mutex> lock(cache::mtx);
		spotter_off = cache::spotter_off_teams;
		target_off = cache::target_off_teams;
	}

	without_team = 0;
	for (cache::entity_t& entity : players)
	{
		auto found = by_address.find(entity.team);
		const std::string& team_name = found != by_address.end() ? found->second->name : cache::no_team_name;

		if (found != by_address.end())
		{
			found->second->players++;
			entity.team_name = found->second->name;
		}
		else
		{

			if (!teams.empty())
			{
				entity.team = 0;
			}
			without_team++;
		}

		entity.teammate = entity.team != 0 && entity.team == local_team;

		entity.spotter_allowed = entity.hostile || !spotter_off.contains(team_name);
		entity.target_allowed = entity.hostile || !target_off.contains(team_name);

		if (settings::universal::hostile_inmates_only && team_name == "Inmates" && !entity.hostile)
		{
			entity.spotter_allowed = false;
			entity.target_allowed = false;
		}
	}

	return matched_by;
}

// true if a player is downed
static bool is_player_knocked(const cache::entity_t& player)
{
	rbx::model_instance_t model_instance = rbx::player_t(player.instance.address).get_model_instance();
	if (model_instance.address == 0)
		return false;

	rbx::instance_t body_effects = model_instance.find_first_child("BodyEffects");
	if (body_effects.address == 0)
		return false;

	rbx::instance_t ko = body_effects.find_first_child("K.O");
	if (ko.address == 0 || ko.get_class_name() != "BoolValue")
		return false;

	return memory->read<bool>(ko.address + Offsets::Misc::Value);
}

// read a named body parts world position
static bool part_position(const cache::entity_t& entity, const char* first, const char* second, math::vector3& out)
{
	auto it = entity.parts.find(first);
	if (it == entity.parts.end() || it->second.address == 0)
		it = entity.parts.find(second);
	if (it == entity.parts.end() || it->second.address == 0)
		return false;

	rbx::primitive_t prim = it->second.get_primitive();
	if (prim.address == 0)
		return false;

	out = prim.get_position();
	return std::isfinite(out.x) && std::isfinite(out.y) && std::isfinite(out.z);
}

// apply the shared per-player filters
static void apply_universal_checks(std::vector<cache::entity_t>& players, std::uint64_t local_player_address, const std::unordered_set<std::string>& always)
{
	namespace u = settings::universal;

	math::vector3 camera_pos{};
	const bool has_camera = game::camera != 0;
	if (has_camera)
	{
		camera_pos = memory->read<math::vector3>(game::camera + Offsets::Camera::Position);
	}

	math::vector3 origin = camera_pos;
	bool has_origin = has_camera;
	for (const cache::entity_t& entity : players)
	{
		if (entity.instance.address == local_player_address)
		{
			math::vector3 own{};
			if (part_position(entity, "HumanoidRootPart", "Head", own))
			{
				origin = own;
				has_origin = true;
			}
			break;
		}
	}

	for (cache::entity_t& entity : players)
	{
		if (entity.instance.address == local_player_address)
			continue;

		entity.knocked = u::knock_check && is_player_knocked(entity);

		if (always.contains(entity.name))
		{

			entity.spotter_allowed = true;
			entity.target_allowed = true;
		}
		else
		{
			bool fails = false;
			if (u::teamcheck && entity.teammate)
				fails = true;
			if (entity.knocked)
				fails = true;

			if (u::health_check && entity.humanoid.address != 0 && (entity.health <= 0.0f || entity.health < u::min_health))
				fails = true;

			math::vector3 root{};
			if (!fails && u::distance_check && has_origin && part_position(entity, "HumanoidRootPart", "Head", root) &&
				root.distance(origin) > u::max_distance)
				fails = true;

			if (fails)
			{
				entity.spotter_allowed = false;
				entity.target_allowed = false;
				continue;
			}
		}

		math::vector3 head{};
		if (u::wall_check && has_camera && part_position(entity, "Head", "HumanoidRootPart", head) &&
			!visibility->is_visible(camera_pos, head))
		{
			entity.target_allowed = false;
		}
	}
}

// read each players display name
static void read_name_tags(std::vector<cache::entity_t>& players)
{

	static const std::string anger_mark = "\xF0\x9F\x92\xA2";

	for (cache::entity_t& entity : players)
	{
		entity.hostile = false;
		if (entity.humanoid.address == 0)
			continue;

		const std::string shown = memory->read_string(entity.humanoid.address + Offsets::Humanoid::DisplayName);
		entity.hostile = shown.find(anger_mark) != std::string::npos;
	}
}

// flag which cached players are friends
static void mark_friends(std::vector<cache::entity_t>& players, std::uint64_t local_player_address)
{

	const bool always_shown = settings::visuals::friends;
	const bool never_aimed_at = settings::visuals::friends && settings::universal::friend_check;

	if (!always_shown && !never_aimed_at)
		return;

	std::string summary;

	for (cache::entity_t& entity : players)
	{
		if (entity.instance.address == local_player_address)
			continue;

		entity.is_friend = friends::contains(entity.user_id);
		if (!entity.is_friend)
			continue;

		if (always_shown)
		{
			entity.spotter_allowed = true;
		}
		if (never_aimed_at)
		{
			entity.target_allowed = false;
		}

		summary += entity.name + ", ";
	}

	static std::string last_summary = "-";
	if (summary != last_summary)
	{
		last_summary = summary;
		LOG_INFO("friends in this server: {}", summary.empty() ? "none" : summary);
	}
}

// debug: log the teams found
static void log_teams(const std::vector<cache::team_t>& teams, int without_team, std::uint64_t local_team, const char* matched_by)
{
	std::string summary;
	std::string own = cache::no_team_name;

	for (const cache::team_t& team : teams)
	{
		summary += std::format("{} {}, ", team.name, team.players);
		if (team.address == local_team)
		{
			own = team.name;
		}
	}
	summary += std::format("no team {}; yours: {}; matched by {}", without_team, own, matched_by);

	static std::string last_summary;
	if (summary != last_summary)
	{
		last_summary = summary;
		LOG_INFO("teams: {}", summary);
	}
}

// background loop: rebuild the player cache
void cache::run()
{
	std::vector<cache::team_t> teams;
	std::chrono::steady_clock::time_point last_team_scan{};

	while (g_running)
	{

		if (game::game_changed)
		{
			LOG_EVERY(5000, logger::level::debug, "game changed, cache cleared until the game state is reinitialized");
			{
				std::lock_guard<std::mutex> lock(mtx);
				cached_players.clear();
				cached_teams.clear();
				cached_local_player = cache::entity_t{};
				players_without_team = 0;
				local_team = 0;
			}
			teams.clear();
			last_team_scan = {};
			std::this_thread::sleep_for(std::chrono::milliseconds(100));
			continue;
		}

		std::vector<cache::entity_t> temp_cache;

		rbx::instance_t workspace = game::datamodel.find_first_child_by_class("Workspace");
		if (workspace.address != 0)
		{
			game::workspace = workspace;
			game::camera = memory->read<std::uint64_t>(workspace.address + Offsets::Workspace::CurrentCamera);
		}

		if (!game::players.address)
		{
			LOG_EVERY(5000, logger::level::warn, "players service is null, nothing to cache");
			std::this_thread::sleep_for(std::chrono::milliseconds(100));
			continue;
		}

		std::vector<rbx::player_t> players = game::players.get_children<rbx::player_t>();
		rbx::player_t local_player_obj{ memory->read<std::uint64_t>(game::players.address + Offsets::Player::LocalPlayer) };

		game::local_player = rbx::instance_t{ local_player_obj.address };

		auto now = std::chrono::steady_clock::now();
		if (now - last_team_scan >= std::chrono::seconds(1))
		{
			teams = scan_teams();
			last_team_scan = now;
		}

		std::vector<std::int32_t> player_colors;

		for (rbx::player_t& player : players)
		{

			if (player.get_class_name() != "Player")
				continue;

			cache::entity_t entity{};

			entity.instance = { player.address };
			entity.name = player.get_name();
			entity.display_name = player.get_display_name();
			entity.user_id = memory->read<std::int64_t>(player.address + Offsets::Player::UserId);

			entity.team = memory->read<std::uint64_t>(player.address + Offsets::Player::Team);
			player_colors.push_back(memory->read<std::int32_t>(player.address + Offsets::Player::TeamColor));

			rbx::model_instance_t model_instance = player.get_model_instance();

			if (model_instance.address != 0)
			{
				std::vector<rbx::instance_t> children = model_instance.get_children();
				for (rbx::instance_t& child : children)
				{
					std::string child_name = child.get_name();
					std::string child_class = child.get_class_name();

					if (child_class.find("Part") != std::string::npos)
					{
						entity.parts[child_name] = rbx::part_t(child.address);
					}
					else if (child_class == "Humanoid")
					{
						entity.humanoid = { child.address };
					}
					else if (child_class == "Tool")
					{
						entity.tool_name = child_name;
					}
				}
			}

			if (entity.humanoid.address != 0)
			{
				entity.rig_type = entity.humanoid.get_rig_type();
				entity.health = memory->read<float>(entity.humanoid.address + Offsets::Humanoid::Health);
				entity.max_health = memory->read<float>(entity.humanoid.address + Offsets::Humanoid::MaxHealth);
			}

			temp_cache.push_back(entity);
		}

		std::uint64_t own_team = 0;
		int without_team = 0;
		read_name_tags(temp_cache);
		const char* matched_by = resolve_teams(temp_cache, player_colors, teams, local_player_obj.address, own_team, without_team);
		log_teams(teams, without_team, own_team, matched_by);

		std::unordered_set<std::string> always_players;
		std::unordered_set<std::string> whitelist_players;
		{
			std::lock_guard<std::mutex> lock(mtx);
			always_players = spotter_always_players;
			whitelist_players = spotter_whitelist_players;
		}
		apply_universal_checks(temp_cache, local_player_obj.address, always_players);

		mark_friends(temp_cache, local_player_obj.address);

		for (cache::entity_t& entity : temp_cache)
		{
			entity.whitelisted = whitelist_players.contains(entity.name);
		}

		cache::entity_t local_entity{};
		for (const cache::entity_t& entity : temp_cache)
		{
			if (local_player_obj.address != 0 && entity.instance.address == local_player_obj.address)
			{
				local_entity = entity;
				break;
			}
		}

		if (settings::logging::positions)
		{
			static std::atomic<std::uint64_t> last_snapshot{ 0 };
			if (logger::throttle(last_snapshot, static_cast<std::uint64_t>(settings::logging::positions_interval * 1000.0f)))
			{
				log_positions(temp_cache, local_player_obj.address);
			}
		}

		static std::size_t last_player_count = 0;
		if (temp_cache.size() != last_player_count)
		{
			LOG_DEBUG("cached players {} -> {}", last_player_count, temp_cache.size());
			last_player_count = temp_cache.size();
		}

		{
			std::lock_guard<std::mutex> lock(mtx);
			cached_players = std::move(temp_cache);
			cached_local_player = local_entity;
			cached_teams = teams;
			players_without_team = without_team;
			local_team = own_team;
		}

		std::this_thread::sleep_for(std::chrono::milliseconds(100));
	}
}
