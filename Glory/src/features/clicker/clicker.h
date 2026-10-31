#pragma once
#include <atomic>
#include <cstdint>

namespace clicker
{
	void run();

	inline constexpr std::uintptr_t click_tag = 0x474C5259;

	inline std::atomic<bool> on{ false };

	inline std::atomic<bool> clicking{ false };

	inline std::atomic<int> clicks_per_second{ 0 };
}
