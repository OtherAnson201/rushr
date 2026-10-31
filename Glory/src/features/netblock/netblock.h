#pragma once
#include <atomic>
#include <cstdint>

namespace netblock
{
	void run();

	inline std::atomic<bool> holding{ false };
	inline std::atomic<std::uint64_t> holding_since{ 0 };
	inline std::atomic<std::uint64_t> failed_at{ 0 };

	inline std::atomic<bool> held_on_part{ false };

	inline std::atomic<bool> gliding{ false };

	inline std::atomic<bool> lifting{ false };

	enum class ignored_t : int
	{
		none,

		elsewhere,

		menu_keyboard,

		no_game
	};

	inline std::atomic<std::uint64_t> ignored_at{ 0 };
	inline std::atomic<ignored_t> ignored_why{ ignored_t::none };
}
