#include "cache.h"
#include <thread>
#include <chrono>
#include <globals.h>
#include <game/game.h>
#include <logger/logger.h>
#include <memory/memory.h>
#include <Offsets/Offsets.hpp>

// background loop: rebuild the player cache
void cache::run()
{
	while (g_running)
	{
		if (game::game_changed)
		{
			LOG_EVERY(5000, logger::level::debug, "game changed, cache cleared until the game state is reinitialized");
			{
				std::lock_guard<std::mutex> lock(mtx);
				cached_players.clear();
				cached_local_player = cache::entity_t{};
			}
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
		const std::uint64_t local_player_address = memory->read<std::uint64_t>(game::players.address + Offsets::Player::LocalPlayer);

		for (rbx::player_t& player : players)
		{
			if (player.get_class_name() != "Player")
				continue;

			cache::entity_t entity{};

			entity.instance = { player.address };
			entity.name = player.get_name();
			entity.display_name = player.get_display_name();

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

		cache::entity_t local_entity{};
		for (const cache::entity_t& entity : temp_cache)
		{
			if (local_player_address != 0 && entity.instance.address == local_player_address)
			{
				local_entity = entity;
				break;
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
		}

		std::this_thread::sleep_for(std::chrono::milliseconds(100));
	}
}
