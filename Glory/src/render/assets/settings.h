#pragma once
#include <imgui/imgui.h>
#include <vector>
#include <string>
#include <unordered_set>
#include <windows.h>

namespace settings
{
	namespace targeting
	{
		inline bool enabled = false;
		inline int keybind = 0;
		inline int activation_mode = 1;

		inline int target_part = 1;

		inline float fov = 100.f;

		inline float sensitivity_x = 100.f;
		inline float sensitivity_y = 100.f;

		inline float switch_delay = 200.f;

		inline bool enable_prediction = false;
		inline float prediction_x = 10.f;
		inline float prediction_y = 10.f;

		inline bool air_prediction_enabled = false;
		inline float air_prediction_x = 10.f;
		inline float air_prediction_y = 10.f;

		inline bool air_part_enabled = false;
		inline int air_part = 1;

		inline bool offset_enabled = false;
		inline float offset_x = 0.0f;
		inline float offset_y = 0.0f;

		inline bool sticky_aim = false;

		inline bool draw_fov = false;
		inline float fov_circle_colour[4] = { 1.f, 1.f, 1.f, 1.f };
		inline float fov_outline_colour[4] = { 0.f, 0.f, 0.f, 1.f };

	}

	namespace correction
	{
		inline bool enabled = false;
		inline int keybind = 0;
		inline int activation_mode = 1;

		inline int target_part = 1;

		// not aimed at.
		inline constexpr int random_part = 7;

		inline float fov = 100.f;
		inline bool draw_fov = false;
		inline float fov_circle_colour[4] = { 1.f, 1.f, 1.f, 1.f };
		inline float fov_outline_colour[4] = { 0.f, 0.f, 0.f, 1.f };

		inline bool enable_prediction = false;
		inline float prediction_x = 10.f;
		inline float prediction_y = 10.f;

		inline bool sticky_aim = false;

		inline float switch_delay = 200.f;
		inline bool spoof_mouse = true;

	}

	namespace autofire
	{
		inline bool enabled = false;
		inline int keybind = 0;
		inline int activation_mode = 1;

		inline float delay = 10.f;
		inline bool randomize_delay = false;
		inline float delay_min = 5.f;
		inline float delay_max = 15.f;

		inline float hit_radius = 15.0f;

		// New customization
		inline int target_part = 0;
	}

	namespace mover
	{

		namespace speedhack
		{
			inline bool enabled = false;
			inline int mode = 0;
			inline float speed = 50.0f;
			inline int keybind = 0;

			inline int activation_mode = 0;
		}

		namespace noclip
		{
			inline bool enabled = false;
			inline int keybind = 0;
			inline int activation_mode = 0;
		}

		namespace jumppower
		{
			inline bool enabled = false;
			inline float value = 50.0f;
			inline int keybind = 0;
			inline int activation_mode = 0;
		}

		namespace freecam
		{
			inline bool enabled = false;
			inline int keybind = 0;

			inline int activation_mode = 0;
			inline float speed = 50.0f;
			inline constexpr float speed_min = 5.0f;
			inline constexpr float speed_max = 500.0f;
		}

		namespace teleport
		{
			inline bool enabled = false;
			inline int keybind = 0;

			inline int activation_mode = 0;
		}

		namespace netblock
		{
			inline bool enabled = false;
			inline int keybind = 0;

			inline int activation_mode = 1;
		}

		namespace stabilizer
		{
			inline bool enabled = false;

			// speed about 100.
			inline float max_speed = 100.0f;
			inline constexpr float max_speed_min = 60.0f;
			inline constexpr float max_speed_max = 500.0f;

			inline bool lock_seats = true;

			inline bool anti_ragdoll = true;

			inline bool leave_vehicles = true;
			inline float vehicle_speed = 250.0f;
			inline constexpr float vehicle_speed_min = 150.0f;
			inline constexpr float vehicle_speed_max = 2000.0f;
		}

	}

	namespace expl
	{
		inline bool fly_enabled = false;
		inline int fly_keybind = 0;
		inline int fly_keybind_mode = 0;
		inline int fly_mode = 0;
		inline float fly_speed = 50.0f;
	}

	namespace macros
	{
		inline bool enabled = false;

		inline int keybind = 'Q';

		inline int spin = 11000;

		inline int dpi = 800;
		inline float sensitivity = 0.36f;

		inline constexpr int spin_min = 1000;
		inline constexpr int spin_max = 50000;
		inline constexpr int dpi_min = 100;
		inline constexpr int dpi_max = 32000;
		inline constexpr float sensitivity_min = 0.01f;
		inline constexpr float sensitivity_max = 10.0f;
	}

	namespace clicker
	{

		inline int clicks_per_second = 60;

		inline int keybind = VK_F6;
		inline constexpr int clicks_per_second_min = 1;
		inline constexpr int clicks_per_second_max = 1000;
	}

	namespace logging
	{
		inline bool enabled = true;
		inline int min_level = 2;
		inline bool to_file = true;
		inline bool to_console = true;

		inline bool positions = false;
		inline float positions_interval = 2.0f;
	}

	namespace overlay
	{

		inline bool hide_from_capture = true;
	}

	namespace universal
	{
		inline bool teamcheck = false;
		inline bool knock_check = false;

		inline bool health_check = false;
		inline float min_health = 0.0f;

		inline bool distance_check = false;
		inline float max_distance = 1000.0f;

		inline bool wall_check = false;

		inline bool friend_check = true;

		inline bool hostile_inmates_only = false;
	}

	namespace visuals
	{
		inline bool enabled = true;

		inline bool box = false;
		inline int box_type = 0;
		inline float box_color[4] = { 1.f, 1.f, 1.f, 1.f };

		inline bool team_colors = true;
		inline float guard_color[4] = { 0.25f, 0.55f, 1.f, 1.f };
		inline float inmate_color[4] = { 1.f, 0.6f, 0.1f, 1.f };
		inline float criminal_color[4] = { 1.f, 0.2f, 0.2f, 1.f };

		inline bool friends = true;
		inline float friend_color[4] = { 0.2f, 1.f, 0.3f, 1.f };

		inline float whitelist_color[4] = { 0.f, 1.f, 0.f, 1.f };

		inline bool name = false;
		inline int name_display_type = 0;
		inline float name_color[4] = { 1.f, 1.f, 1.f, 1.f };
		inline float name_color_blend_start[4] = { 1.f, 1.f, 1.f, 1.f };
		inline float name_color_blend_end[4] = { 0.f, 0.f, 1.f, 1.f };
		inline bool blend = false;

		inline bool healthbar = false;
		inline float healthbar_color[4] = { 0.f, 1.f, 0.f, 1.f };
		inline bool health_based_healthbar = false;

		inline int spotter_font = 0;

		inline bool debug_visibility = false;

		inline float fade_in_speed = 5.0f;
		inline float fade_out_speed = 5.0f;

	}

}
