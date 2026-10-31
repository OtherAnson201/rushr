#pragma once
#include <atomic>
#include <cstdint>

namespace stabilizer
{
	void run();

	float top_speed();

	inline std::atomic<std::uint64_t> root_primitive{ 0 };

	inline std::atomic<std::uint64_t> character_model{ 0 };

	float vehicle_speed();

	inline std::atomic<std::uint64_t> humanoid{ 0 };

	inline std::atomic<std::uint64_t> seated_root{ 0 };

	enum class standing_t : int
	{

		off,

		no_character,
		dead,

		sitting_watched,

		sitting,

		guarded
	};

	inline std::atomic<standing_t> standing{ standing_t::off };

	inline std::atomic<std::uint64_t> own_move_until{ 0 };

	inline std::atomic<float> own_ground_speed{ 0.0f };

	inline std::atomic<int> throws_stopped{ 0 };
	inline std::atomic<int> parts_kept_off{ 0 };
	inline std::atomic<std::uint64_t> last_slowed{ 0 };
	inline std::atomic<int> vehicles_left{ 0 };
	inline std::atomic<int> put_backs{ 0 };

	inline std::atomic<int> seats_watched{ 0 };
	inline std::atomic<int> seats_thrown{ 0 };
	inline std::atomic<int> seats_locked{ 0 };
}
