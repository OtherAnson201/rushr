#include "friends.h"

#include <globals.h>
#include <cache/cache.h>
#include <logger/logger.h>
#include <render/assets/settings.h>
#include <nlohmann/json.hpp>
#include <chrono>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_set>
#include <windows.h>
#include <net/https.h>

using namespace std::chrono_literals;

namespace
{

	std::mutex list_mutex;
	std::unordered_set<std::int64_t> friend_ids;

	constexpr auto refresh_interval = 120s;

	constexpr auto retry_interval = 30s;

	constexpr int max_pages = 40;

	// true if the id looks like a real user id
	bool is_user_id(std::int64_t id)
	{
		return id > 0 && id < 1'000'000'000'000;
	}

	// pull a users friend ids from robloxs api, paging through
	bool fetch_friends(std::int64_t user_id, std::unordered_set<std::int64_t>& ids, int& hidden)
	{
		ids.clear();
		hidden = 0;

		std::string cursor;
		for (int page = 0; page < max_pages; page++)
		{
			std::wstring path = L"/v1/users/" + std::to_wstring(user_id) + L"/friends/find?limit=50";
			if (!cursor.empty())
			{
				path += L"&cursor=" + net::url_encode(cursor);
			}

			std::string body;
			const DWORD status = net::https_get(L"friends.roblox.com", path, body);

			if (status == 400)
			{

				LOG_WARN("Roblox does not know user id {}, the Player::UserId offset may be outdated", user_id);
				return false;
			}

			if (status != 200)
			{
				LOG_WARN("friends list request failed ({})", status == 0 ? "no answer" : std::format("HTTP {}", status));
				return false;
			}

			try
			{
				const nlohmann::json answer = nlohmann::json::parse(body);

				for (const nlohmann::json& item : answer.at("PageItems"))
				{
					const std::int64_t id = item.at("id").get<std::int64_t>();
					if (id > 0)
					{
						ids.insert(id);
					}
					else
					{
						hidden++;
					}
				}

				const auto next = answer.find("NextCursor");
				cursor = (next != answer.end() && next->is_string()) ? next->get<std::string>() : std::string{};
			}
			catch (const nlohmann::json::exception& error)
			{
				LOG_WARN("could not read the friends list answer: {}", error.what());
				return false;
			}

			if (cursor.empty())
				return true;
		}

		LOG_WARN("friends list is longer than {} pages, the rest is ignored", max_pages);
		return true;
	}
}

// true if this user id is a friend
bool friends::contains(std::int64_t user_id)
{
	std::lock_guard<std::mutex> lock(list_mutex);
	return friend_ids.contains(user_id);
}

// background loop: keep the local players friend list fresh
void friends::run()
{

	std::int64_t fetched_for = 0;
	std::chrono::steady_clock::time_point next_fetch{};

	std::size_t last_count = static_cast<std::size_t>(-1);

	while (g_running)
	{
		std::this_thread::sleep_for(250ms);

		if (!settings::visuals::friends && !settings::universal::friend_check)
			continue;

		std::int64_t local_id = 0;
		bool in_a_server = false;
		{
			std::lock_guard<std::mutex> lock(cache::mtx);
			local_id = cache::cached_local_player.user_id;

			in_a_server = cache::cached_local_player.instance.address != 0 && cache::cached_players.size() > 1;
		}

		if (!is_user_id(local_id))
		{
			if (in_a_server)
			{
				LOG_EVERY(30000, logger::level::warn, "local player's user id reads as {}, friends cannot be recognised (Player::UserId offset outdated?)", local_id);
			}
			continue;
		}

		const auto now = std::chrono::steady_clock::now();
		if (local_id == fetched_for && now < next_fetch)
			continue;

		if (local_id != fetched_for)
		{
			std::lock_guard<std::mutex> lock(list_mutex);
			friend_ids.clear();
			last_count = static_cast<std::size_t>(-1);
		}
		fetched_for = local_id;

		std::unordered_set<std::int64_t> ids;
		int hidden = 0;
		if (!fetch_friends(local_id, ids, hidden))
		{

			next_fetch = std::chrono::steady_clock::now() + retry_interval;
			continue;
		}

		if (ids.size() != last_count)
		{
			last_count = ids.size();
			LOG_INFO("friends: user {} has {} friends with a visible id, {} hidden by Roblox", local_id, ids.size(), hidden);
		}

		{
			std::lock_guard<std::mutex> lock(list_mutex);
			friend_ids = std::move(ids);
		}

		next_fetch = std::chrono::steady_clock::now() + refresh_interval;
	}
}
