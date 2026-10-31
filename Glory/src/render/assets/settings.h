#pragma once
#include <imgui/imgui.h>
#include <vector>
#include <string>
#include <windows.h>

namespace settings
{
	namespace targeting
	{
		inline bool enabled = false;
		inline int keybind = 0;
		inline int activation_mode = 1;

		// 0 closest to the cursor, 1 head, 2 torso
		inline int target_part = 1;

		inline float fov = 100.f;

		// 1 to 100, 100 snaps at once
		inline float sensitivity = 100.f;

		inline bool draw_fov = false;
		inline float fov_circle_colour[4] = { 1.f, 1.f, 1.f, 1.f };
		inline float fov_outline_colour[4] = { 0.f, 0.f, 0.f, 1.f };
	}

	namespace visuals
	{
		inline bool enabled = true;

		inline bool box = false;
		inline float box_color[4] = { 1.f, 1.f, 1.f, 1.f };

		inline bool name = false;
		inline float name_color[4] = { 1.f, 1.f, 1.f, 1.f };

		inline bool healthbar = false;
		inline float healthbar_color[4] = { 0.f, 1.f, 0.f, 1.f };
	}

	namespace logging
	{
		inline bool enabled = true;
		inline int min_level = 2;
		inline bool to_file = true;
		inline bool to_console = true;
	}

	namespace overlay
	{
		inline bool hide_from_capture = true;
	}
}
