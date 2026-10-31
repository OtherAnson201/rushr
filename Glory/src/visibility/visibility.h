#pragma once
#include <memory>
#include <mutex>
#include <vector>

#include "obb.h"
#include <sdk/sdk.h>
#include <sdk/math/math.h>

class c_visibility final {
public:
	c_visibility() = default;
	~c_visibility() = default;

	static void run();

	bool cache_workspace();

	void clear();

	bool is_visible(const math::vector3& origin, const math::vector3& target);

private:
	using obstacle_list = std::vector<rbx::obb>;

	std::mutex mutex;
	std::shared_ptr<const obstacle_list> obstacles;
};

inline std::unique_ptr<c_visibility> visibility = std::make_unique<c_visibility>();
