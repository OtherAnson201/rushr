#include "spotter.h"

#include <cfloat>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <mutex>
#include <string>
#include <vector>

#include <imgui/imgui.h>
#include <imgui/imgui_internal.h>

#include <render/assets/settings.h>
#include <game/game.h>
#include <cache/cache.h>
#include <render/render.h>
#include <font/config/font_config.h>

// turn an rgba setting into an imgui color
static ImU32 to_colour(const float (&rgba)[4])
{
	return ImGui::ColorConvertFloat4ToU32({ rgba[0], rgba[1], rgba[2], rgba[3] });
}

// draw text with a black outline
static void draw_text_outlined(ImDrawList* draw, ImFont* font, float font_size, ImVec2 pos, ImU32 col, const char* text)
{
	pos.x = roundf(pos.x);
	pos.y = roundf(pos.y);

	const char* text_end = text + std::strlen(text);
	if (text_end == text)
		return;

	const ImU32 outline_col = IM_COL32(0, 0, 0, col >> IM_COL32_A_SHIFT);

	for (int x = -1; x <= 1; x++)
	{
		for (int y = -1; y <= 1; y++)
		{
			if (x == 0 && y == 0)
			{
				continue;
			}
			draw->AddText(font, font_size, ImVec2(pos.x + x, pos.y + y), outline_col, text, text_end);
		}
	}

	draw->AddText(font, font_size, pos, col, text, text_end);
}

// draw a sharp one pixel box with a black outline
static void draw_box(ImDrawList* draw, float left, float top, float right, float bottom, ImU32 color)
{
	const ImVec2 a(roundf(left), roundf(top));
	const ImVec2 b(roundf(right), roundf(bottom));

	const ImDrawListFlags old_flags = draw->Flags;
	draw->Flags &= ~ImDrawListFlags_AntiAliasedLines;

	const ImU32 outline = IM_COL32(0, 0, 0, color >> IM_COL32_A_SHIFT);
	draw->AddRect(a, b, outline);
	draw->AddRect({ a.x - 1.f, a.y - 1.f }, { b.x + 1.f, b.y + 1.f }, color);
	draw->AddRect({ a.x - 2.f, a.y - 2.f }, { b.x + 2.f, b.y + 2.f }, outline);

	draw->Flags = old_flags;
}

// true if this part belongs to an r15 body
static bool is_valid_r15_body_part(const std::string& part_name)
{
	return part_name == "Head" ||
		part_name == "UpperTorso" ||
		part_name == "LowerTorso" ||
		part_name == "LeftUpperArm" ||
		part_name == "LeftLowerArm" ||
		part_name == "LeftHand" ||
		part_name == "RightUpperArm" ||
		part_name == "RightLowerArm" ||
		part_name == "RightHand" ||
		part_name == "LeftUpperLeg" ||
		part_name == "LeftLowerLeg" ||
		part_name == "LeftFoot" ||
		part_name == "RightUpperLeg" ||
		part_name == "RightLowerLeg" ||
		part_name == "RightFoot";
}

// true if this part belongs to an r6 body
static bool is_valid_r6_body_part(const std::string& part_name)
{
	return part_name == "Head" ||
		part_name == "Torso" ||
		part_name == "Left Arm" ||
		part_name == "Right Arm" ||
		part_name == "Left Leg" ||
		part_name == "Right Leg";
}

// draw the box, health bar and name for every other player
void spotter::run()
{
	if (!settings::visuals::enabled)
	{
		return;
	}

	static constexpr math::vector3 corners[8] =
	{
		{-1, -1, -1}, {1, -1, -1}, {-1, 1, -1},{1, 1, -1},
		{-1, -1, 1}, {1, -1, 1}, {-1, 1, 1}, {1, 1, 1}
	};

	const math::vector2 dims = game::visengine.get_dimensions();
	const math::matrix4 view = game::visengine.get_viewmatrix();

	std::vector<cache::entity_t> players;
	std::uint64_t local_player = 0;
	{
		std::lock_guard<std::mutex> lock(cache::mtx);
		players = cache::cached_players;
		local_player = cache::cached_local_player.instance.address;
	}

	ImDrawList* draw = ImGui::GetBackgroundDrawList();
	ImFont* const font = spotter_font ? spotter_font_tahoma : ImGui::GetFont();
	const float font_size = spotter_font ? font_config::tahoma::font_size : ImGui::GetFontSize();

	for (const cache::entity_t& entity : players)
	{
		if (entity.instance.address == 0 || entity.instance.address == local_player)
		{
			continue;
		}

		bool on_screen = false;
		float left = FLT_MAX, top = FLT_MAX;
		float right = -FLT_MAX, bottom = -FLT_MAX;

		const bool is_r15 = entity.rig_type == 1;

		for (const auto& [part_name, part] : entity.parts)
		{
			// only body parts count, so hats and big hitboxes dont stretch the box
			if (!(is_r15 ? is_valid_r15_body_part(part_name) : is_valid_r6_body_part(part_name)))
			{
				continue;
			}

			rbx::primitive_t prim = part.get_primitive();
			const math::vector3 size = prim.get_size();
			if (size.x == 0.f || size.y == 0.f || size.z == 0.f)
			{
				continue;
			}

			const math::vector3 pos = prim.get_position();
			const math::matrix3 rot = prim.get_rotation();

			for (const math::vector3& corner : corners)
			{
				const math::vector3 world = pos + rot * math::vector3{
					corner.x * size.x * 0.5f,
					corner.y * size.y * 0.5f,
					corner.z * size.z * 0.5f
				};

				math::vector2 screen{};
				if (game::visengine.world_to_screen(world, screen, dims, view))
				{
					on_screen = true;
					if (screen.x < left) left = screen.x;
					if (screen.y < top) top = screen.y;
					if (screen.x > right) right = screen.x;
					if (screen.y > bottom) bottom = screen.y;
				}
			}
		}

		if (!on_screen || left >= right || top >= bottom)
		{
			continue;
		}

		// dont draw across another window sitting on top of the game
		if (game::is_covered(left - 10.0f, top - 20.0f, right + 10.0f, bottom + 10.0f))
		{
			continue;
		}

		if (settings::visuals::box)
		{
			draw_box(draw, left, top, right, bottom, to_colour(settings::visuals::box_color));
		}

		if (settings::visuals::healthbar && entity.humanoid.address != 0 && entity.max_health > 0.f)
		{
			const float box_left = roundf(left);
			const float box_top = roundf(top);
			const float box_bottom = roundf(bottom);

			float health = entity.health / entity.max_health;
			health = health < 0.f ? 0.f : (health > 1.f ? 1.f : health);

			draw->AddRectFilled(ImVec2(box_left - 7.f, box_top - 2.f), ImVec2(box_left - 3.f, box_bottom + 2.f), IM_COL32(0, 0, 0, 255));
			if (health > 0.f)
			{
				const float fill_height = (box_bottom - box_top + 2.f) * health;
				draw->AddRectFilled(ImVec2(box_left - 6.f, box_bottom + 1.f - fill_height), ImVec2(box_left - 4.f, box_bottom + 1.f), to_colour(settings::visuals::healthbar_color));
			}
		}

		if (settings::visuals::name)
		{
			const float text_width = font->CalcTextSizeA(font_size, FLT_MAX, 0.f, entity.display_name.c_str()).x;
			const ImVec2 name_pos(left + (right - left) * 0.5f - text_width * 0.5f, roundf(top) - font_size - 5.f);
			draw_text_outlined(draw, font, font_size, name_pos, to_colour(settings::visuals::name_color), entity.display_name.c_str());
		}
	}
}
