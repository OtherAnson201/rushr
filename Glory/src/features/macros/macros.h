#pragma once
#include <atomic>
#include <cstdint>

namespace macros
{
	void run();

	inline std::atomic<std::uint64_t> last_run{ 0 };
}
