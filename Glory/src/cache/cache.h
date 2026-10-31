#pragma once
#include <string>
#include <mutex>
#include <vector>
#include <unordered_map>
#include <unordered_set>

#include <sdk/sdk.h>
#include <sdk/math/math.h>

namespace cache
{
	inline std::mutex mtx;

	struct part_data_t
	{
		rbx::part_t part;
		std::string name;
		math::vector3 position;
		math::vector3 size;
		math::matrix3 rotation;
	};

	struct entity_t final
	{
		rbx::instance_t instance;
		std::string name;
		std::string display_name;

		std::int64_t user_id{ 0 };

		bool is_friend{ false };

		bool whitelisted{ false };

		std::uint8_t rig_type{ 0 };

		rbx::humanoid_t humanoid;
		std::unordered_map<std::string, rbx::part_t> parts;

		std::uint64_t team{ 0 };

		std::string team_name;

		bool teammate{ false };

		bool spotter_allowed{ true };
		bool target_allowed{ true };

		float health{ 0.0f };
		float max_health{ 0.0f };
		bool knocked{ false };

		bool hostile{ false };

		std::string tool_name;
	};

	struct team_t
	{
		std::uint64_t address{ 0 };
		std::string name;

		std::int32_t color{ 0 };

		int players{ 0 };
	};

	inline const std::string no_team_name = "(no team)";

	inline cache::entity_t cached_local_player;
	inline std::vector<cache::entity_t> cached_players;

	inline std::vector<cache::team_t> cached_teams;
	inline int players_without_team = 0;
	inline std::uint64_t local_team = 0;

	inline std::unordered_set<std::string> spotter_off_teams;
	inline std::unordered_set<std::string> target_off_teams;

	inline std::unordered_set<std::string> spotter_always_players;

	inline std::unordered_set<std::string> spotter_whitelist_players;

	inline std::string teleport_target;

	void run();
}
