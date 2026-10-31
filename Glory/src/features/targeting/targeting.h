#pragma once
#include <cache/cache.h>

namespace targeting
{
	inline cache::entity_t player{};
	inline cache::entity_t sticky_target{};

	void run();
}
