#pragma once
#include <windows.h>
#include <atomic>
#include <vector>
#include <sdk/sdk.h>

namespace game
{
	inline rbx::instance_t datamodel{};
	inline rbx::visualengine_t visengine{};
	inline rbx::instance_t workspace{};
	inline rbx::instance_t players{};
	inline rbx::instance_t local_player{};
	inline std::uint64_t camera{};
	inline HWND roblox_window = nullptr;
	inline float window_offset_x = 0.0f;
	inline float window_offset_y = 0.0f;

	inline float window_width = 0.0f;
	inline float window_height = 0.0f;

	inline bool game_changed = false;

	inline std::atomic<std::uint32_t> session{ 0 };

	inline std::uint64_t current_job_id = 0;

	HWND get_roblox_window();
	void update_window_offset();

	bool typing_in_chat();

	enum class overlay_place
	{

		above_everything,

		above_game,

		hidden
	};

	inline overlay_place overlay_position = overlay_place::above_everything;

	overlay_place order_overlay(HWND overlay);

	inline std::vector<RECT> covering_windows;
	void update_covering_windows(HWND overlay);

	bool is_covered(float left, float top, float right, float bottom);

	bool check_game_changed();

	bool reinitialize_game_state();

	void detach();
}
