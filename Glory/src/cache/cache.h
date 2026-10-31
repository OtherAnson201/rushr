#pragma once
#include <string>
#include <mutex>
#include <vector>
#include <unordered_map>

#include <sdk/sdk.h>
#include <sdk/math/math.h>

namespace cache
{
	inline std::mutex mtx;

	struct entity_t final
	{
		rbx::instance_t instance;
		std::string name;
		std::string display_name;

		std::uint8_t rig_type{ 0 };

		rbx::humanoid_t humanoid;
		std::unordered_map<std::string, rbx::part_t> parts;

		float health{ 0.0f };
		float max_health{ 0.0f };
	};

	inline cache::entity_t cached_local_player;
	inline std::vector<cache::entity_t> cached_players;

	void run();
}
