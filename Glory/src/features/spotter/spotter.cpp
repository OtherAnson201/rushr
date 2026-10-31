#include "spotter.h"

#include <vector>
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>
#include <unordered_map>

#include <imgui/imgui.h>
#include <imgui/imgui_internal.h>

#include <render/assets/settings.h>
#include <game/game.h>
#include <cache/cache.h>
#include <render/render.h>
#include <memory/memory.h>
#include <Offsets/Offsets.hpp>
#include <visibility/visibility.h>
#include <font/config/font_config.h>

namespace helper
{
	// clamp a value between min and max
	__forceinline float clamp(float value, float min_val, float max_val)
	{
		return value < min_val ? min_val : (value > max_val ? max_val : value);
	}

	// scale a colors alpha by an opacity
	__forceinline ImU32 apply_opacity(ImU32 color, float opacity)
	{
		ImU32 r = (color >> 0) & 0xFF;
		ImU32 g = (color >> 8) & 0xFF;
		ImU32 b = (color >> 16) & 0xFF;
		ImU32 a = (color >> 24) & 0xFF;
		a = static_cast<ImU32>(a * opacity);
		return IM_COL32(r, g, b, a);
	}

	// pack an rgba color at the given opacity
	__forceinline ImU32 color_with_opacity(float r, float g, float b, float a, float opacity)
	{
		return IM_COL32(
			static_cast<int>(r * 255.f),
			static_cast<int>(g * 255.f),
			static_cast<int>(b * 255.f),
			static_cast<int>(a * opacity * 255.f)
		);
	}

	// draw text with a dark outline
	__forceinline void draw_text_outlined(ImDrawList* draw, ImFont* font, float font_size, ImVec2 pos, ImU32 col, const char* text_begin, const char* text_end = nullptr)
	{
		pos.x = roundf(pos.x);
		pos.y = roundf(pos.y);

		const char* text_end_ptr = text_end ? text_end : text_begin + std::strlen(text_begin);
		if (text_end_ptr == text_begin)
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
				draw->AddText(font, font_size, ImVec2(pos.x + x, pos.y + y), outline_col, text_begin, text_end_ptr);
			}
		}

		draw->AddText(font, font_size, pos, col, text_begin, text_end_ptr);
	}

	// draw text faded by an opacity
	__forceinline void draw_text_blended(ImDrawList* draw, ImFont* font, float font_size, ImVec2 pos, ImU32 col_start, ImU32 col_end, const char* text_begin, const char* text_end = nullptr)
	{
		pos.x = roundf(pos.x);
		pos.y = roundf(pos.y);

		const char* text_end_ptr = text_end ? text_end : text_begin + std::strlen(text_begin);

		const int text_len = ImTextCountCharsFromUtf8(text_begin, text_end_ptr);

		if (text_len == 0)
			return;

		float start_r = static_cast<float>((col_start >> 0) & 0xFF);
		float start_g = static_cast<float>((col_start >> 8) & 0xFF);
		float start_b = static_cast<float>((col_start >> 16) & 0xFF);
		float start_a = static_cast<float>((col_start >> 24) & 0xFF);

		float end_r = static_cast<float>((col_end >> 0) & 0xFF);
		float end_g = static_cast<float>((col_end >> 8) & 0xFF);
		float end_b = static_cast<float>((col_end >> 16) & 0xFF);
		float end_a = static_cast<float>((col_end >> 24) & 0xFF);

		float current_x = pos.x;
		const char* char_begin = text_begin;
		for (int i = 0; i < text_len && char_begin < text_end_ptr; i++)
		{
			float t = text_len > 1 ? static_cast<float>(i) / static_cast<float>(text_len - 1) : 0.0f;

			float r = start_r + (end_r - start_r) * t;
			float g = start_g + (end_g - start_g) * t;
			float b = start_b + (end_b - start_b) * t;
			float a = start_a + (end_a - start_a) * t;

			ImU32 char_col = IM_COL32(static_cast<int>(r), static_cast<int>(g), static_cast<int>(b), static_cast<int>(a));
			ImU32 outline_col = IM_COL32(0, 0, 0, static_cast<int>(a));

			unsigned int code_point = 0;
			int char_bytes = ImTextCharFromUtf8(&code_point, char_begin, text_end_ptr);
			if (char_bytes <= 0)
			{
				char_bytes = 1;
			}
			const char* char_end = char_begin + char_bytes;

			for (int x = -1; x <= 1; x++)
			{
				for (int y = -1; y <= 1; y++)
				{
					if (x == 0 && y == 0)
					{
						continue;
					}
					draw->AddText(font, font_size, ImVec2(current_x + x, pos.y + y), outline_col, char_begin, char_end);
				}
			}

			float char_width = font->CalcTextSizeA(font_size, FLT_MAX, 0.0f, char_begin, char_end).x;
			draw->AddText(font, font_size, ImVec2(current_x, pos.y), char_col, char_begin, char_end);
			current_x += char_width + 1.0f;

			char_begin = char_end;
		}
	}

	// draw a 2d box between two corners
	__forceinline void box(ImVec2& c1, ImVec2& c2, ImU32 color)
	{
		c1.x = roundf(c1.x);
		c1.y = roundf(c1.y);
		c2.x = roundf(c2.x);
		c2.y = roundf(c2.y);

		ImDrawList* draw = ImGui::GetBackgroundDrawList();

		const ImDrawListFlags old_flags = draw->Flags;
		draw->Flags &= ~ImDrawListFlags_AntiAliasedLines;

		ImRect rect(c1.x, c1.y, c1.x + c2.x, c1.y + c2.y);

		draw->AddRect(rect.Min, rect.Max, IM_COL32(0, 0, 0, color >> 24));
		draw->AddRect({ rect.Min.x - 1.f, rect.Min.y - 1.f }, { rect.Max.x + 1.f, rect.Max.y + 1.f }, color);
		draw->AddRect({ rect.Min.x - 2.f, rect.Min.y - 2.f }, { rect.Max.x + 2.f, rect.Max.y + 2.f }, IM_COL32(0, 0, 0, color >> 24));

		draw->Flags = old_flags;
	}

	// draw just the corners of a box
	__forceinline void corner_box(ImDrawList* draw, ImVec2 min, ImVec2 max, ImU32 col, float thickness = 1.f)
	{
		float x1 = roundf(min.x) - 1;
		float y1 = roundf(min.y) - 1;
		float x2 = roundf(max.x) + 1;
		float y2 = roundf(max.y) + 1;

		ImU32 outline_col = IM_COL32(0, 0, 0, 255);

		float box_width = x2 - x1;
		float box_height = y2 - y1;
		float length = (std::min)(box_width * 0.3f, box_height * 0.3f);
		length = (std::max)(length, 5.f);
		length = (std::min)(length, 15.f);

		float x1_len = roundf(x1 + length);
		float y1_len = roundf(y1 + length);
		float x2_len = roundf(x2 - length);
		float y2_len = roundf(y2 - length);

		draw->AddRectFilled(ImVec2(x1 - 1.f, y1 - 1.f), ImVec2(x1_len + 1.f, y1 + thickness + 1.f), outline_col);
		draw->AddRectFilled(ImVec2(x1 - 1.f, y1 - 1.f), ImVec2(x1 + thickness + 1.f, y1_len + 1.f), outline_col);

		draw->AddRectFilled(ImVec2(x2_len - 1.f, y1 - 1.f), ImVec2(x2 + 1.f, y1 + thickness + 1.f), outline_col);
		draw->AddRectFilled(ImVec2(x2 - thickness - 1.f, y1 - 1.f), ImVec2(x2 + 1.f, y1_len + 1.f), outline_col);

		draw->AddRectFilled(ImVec2(x1 - 1.f, y2 - thickness - 1.f), ImVec2(x1_len + 1.f, y2 + 1.f), outline_col);
		draw->AddRectFilled(ImVec2(x1 - 1.f, y2_len - 1.f), ImVec2(x1 + thickness + 1.f, y2 + 1.f), outline_col);

		draw->AddRectFilled(ImVec2(x2_len - 1.f, y2 - thickness - 1.f), ImVec2(x2 + 1.f, y2 + 1.f), outline_col);
		draw->AddRectFilled(ImVec2(x2 - thickness - 1.f, y2_len - 1.f), ImVec2(x2 + 1.f, y2 + 1.f), outline_col);

		draw->AddRectFilled(ImVec2(x1, y1), ImVec2(x1_len, y1 + thickness), col);
		draw->AddRectFilled(ImVec2(x1, y1), ImVec2(x1 + thickness, y1_len), col);

		draw->AddRectFilled(ImVec2(x2_len, y1), ImVec2(x2, y1 + thickness), col);
		draw->AddRectFilled(ImVec2(x2 - thickness, y1), ImVec2(x2, y1_len), col);

		draw->AddRectFilled(ImVec2(x1, y2 - thickness), ImVec2(x1_len, y2), col);
		draw->AddRectFilled(ImVec2(x1, y2_len), ImVec2(x1 + thickness, y2), col);

		draw->AddRectFilled(ImVec2(x2_len, y2 - thickness), ImVec2(x2, y2), col);
		draw->AddRectFilled(ImVec2(x2 - thickness, y2_len), ImVec2(x2, y2), col);
	}
}

struct health_smooth_state_t
{
	float smooth_health = 0.0f;
};

static std::unordered_map<uint64_t, health_smooth_state_t> g_health_smooth_states;

struct spotter_fade_state_t
{
	float box_opacity = 0.0f;
	float name_opacity = 0.0f;
	float healthbar_opacity = 0.0f;
};

static spotter_fade_state_t g_spotter_fade_state;

// ease the fade opacity toward on or off
static float update_fade_opacity(float current_opacity, bool enabled, float fade_in_speed, float fade_out_speed)
{
	float target_opacity = enabled ? 1.0f : 0.0f;
	float speed = enabled ? fade_in_speed : fade_out_speed;
	float delta_time = ImGui::GetIO().DeltaTime;

	if (current_opacity < target_opacity)
	{
		current_opacity += speed * delta_time;
		if (current_opacity > target_opacity)
			current_opacity = target_opacity;
	}
	else if (current_opacity > target_opacity)
	{
		current_opacity -= speed * delta_time;
		if (current_opacity < target_opacity)
			current_opacity = target_opacity;
	}

	return current_opacity;
}

// true if the name is an r15 body part
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

// true if the name is an r6 body part
static bool is_valid_r6_body_part(const std::string& part_name)
{
	return part_name == "Head" ||
		part_name == "Torso" ||
		part_name == "Left Arm" ||
		part_name == "Right Arm" ||
		part_name == "Left Leg" ||
		part_name == "Right Leg";
}

// true if the name is a body part for this rig
static bool is_body_part(const std::string& part_name, bool is_r15)
{
	if (is_r15)
		return is_valid_r15_body_part(part_name);
	return is_valid_r6_body_part(part_name);
}

// background loop: draw the player esp
void spotter::run()
{
	if (!settings::visuals::enabled)
	{
		return;
	}

	static constexpr float FADE_DELTA = 0.01f;
	auto update_fade_if_needed = [](float current, bool enabled, float fade_in, float fade_out) -> float {
		float target = enabled ? 1.0f : 0.0f;
		if (fabsf(current - target) < FADE_DELTA) return target;
		return update_fade_opacity(current, enabled, fade_in, fade_out);
	};

	g_spotter_fade_state.box_opacity = update_fade_if_needed(g_spotter_fade_state.box_opacity, settings::visuals::box, settings::visuals::fade_in_speed, settings::visuals::fade_out_speed);
	g_spotter_fade_state.name_opacity = update_fade_if_needed(g_spotter_fade_state.name_opacity, settings::visuals::name, settings::visuals::fade_in_speed, settings::visuals::fade_out_speed);
	g_spotter_fade_state.healthbar_opacity = update_fade_if_needed(g_spotter_fade_state.healthbar_opacity, settings::visuals::healthbar, settings::visuals::fade_in_speed, settings::visuals::fade_out_speed);

	static constexpr math::vector3 corners[8] =
	{
		{-1, -1, -1}, {1, -1, -1}, {-1, 1, -1},{1, 1, -1},
		{-1, -1, 1}, {1, -1, 1}, {-1, 1, 1}, {1, 1, 1}
	};

	const math::vector2 dims = game::visengine.get_dimensions();
	const math::matrix4 view = game::visengine.get_viewmatrix();

	std::vector<cache::entity_t> snapshot;
	{
		std::lock_guard<std::mutex> lock(cache::mtx);
		snapshot = cache::cached_players;
	}

	if (g_health_smooth_states.size() > snapshot.size() + 64)
	{
		std::erase_if(g_health_smooth_states, [&snapshot](const auto& state)
			{
				return std::none_of(snapshot.begin(), snapshot.end(), [&state](const cache::entity_t& entity)
					{
						return entity.humanoid.address == state.first;
					});
			});
	}

	std::uint64_t local_player_address = 0;
	if (game::players.address != 0)
	{
		local_player_address = memory->read<std::uint64_t>(game::players.address + Offsets::Player::LocalPlayer);
	}

	math::vector3 camera_pos{};
	bool has_camera = false;
	if (game::camera != 0)
	{
		camera_pos = memory->read<math::vector3>(game::camera + Offsets::Camera::Position);
		has_camera = true;
	}

	for (cache::entity_t& entity : snapshot)
	{
		bool valid = false;
		float left = FLT_MAX, top = FLT_MAX;
		float right = -FLT_MAX, bottom = -FLT_MAX;

		if (entity.instance.address == 0)
		{
			continue;
		}

		if (entity.instance.address == local_player_address)
		{
			continue;
		}

		if (!entity.spotter_allowed)
		{
			continue;
		}

		bool has_any_valid_part = false;
		const bool is_r15 = (entity.rig_type == 1);

		for (const auto& parts : entity.parts)
		{
			const std::string& part_name = parts.first;

			if (!is_body_part(part_name, is_r15))
			{
				continue;
			}

			const rbx::part_t& part = parts.second;
			rbx::primitive_t prim = part.get_primitive();
			const math::vector3 size = prim.get_size();

			if (size.x == 0.f || size.y == 0.f || size.z == 0.f)
			{
				continue;
			}

			has_any_valid_part = true;
			const math::vector3 pos = prim.get_position();
			const math::matrix3 rot = prim.get_rotation();

			// Pre-calculate half sizes
			const float half_x = size.x * 0.5f;
			const float half_y = size.y * 0.5f;
			const float half_z = size.z * 0.5f;

			for (int i = 0; i < 8; ++i)
			{
				const math::vector3& corner = corners[i];
				math::vector3 world = pos + rot * math::vector3{
					corner.x * half_x,
					corner.y * half_y,
					corner.z * half_z
				};

				math::vector2 screen{};
				if (game::visengine.world_to_screen(world, screen, dims, view))
				{
					valid = true;
					if (screen.x < left) left = screen.x;
					if (screen.y < top) top = screen.y;
					if (screen.x > right) right = screen.x;
					if (screen.y > bottom) bottom = screen.y;
				}
			}
		}

		if (!has_any_valid_part)
		{
			continue;
		}

		ImDrawList* draw = ImGui::GetBackgroundDrawList();

		if (!valid || left >= right || top >= bottom)
		{
			continue;
		}

		if (game::is_covered(left - 10.0f, top - 20.0f, right + 10.0f, bottom + 10.0f))
		{
			continue;
		}

		ImVec2 c1(left, top);
		ImVec2 c2(right - left, bottom - top);

		float opacity_multiplier = 1.0f;
		if (settings::visuals::debug_visibility && has_camera)
		{
			auto target_it = entity.parts.find("Head");
			if (target_it == entity.parts.end() || target_it->second.address == 0)
			{
				target_it = entity.parts.find("HumanoidRootPart");
			}

			if (target_it != entity.parts.end() && target_it->second.address != 0)
			{
				rbx::primitive_t target_prim = target_it->second.get_primitive();
				if (!visibility->is_visible(camera_pos, target_prim.get_position()))
				{
					opacity_multiplier = 0.3f;
				}
			}
		}

		const float* override_color = nullptr;
		if (entity.whitelisted)
		{
			override_color = settings::visuals::whitelist_color;
		}

		else if (entity.is_friend && settings::visuals::friends)
		{
			override_color = settings::visuals::friend_color;
		}
		else if (settings::visuals::team_colors)
		{
			if (entity.team_name == "Guards") override_color = settings::visuals::guard_color;
			else if (entity.team_name == "Inmates") override_color = settings::visuals::inmate_color;
			else if (entity.team_name == "Criminals") override_color = settings::visuals::criminal_color;
		}

		if (settings::visuals::box)
		{
			const float* box_rgba = override_color ? override_color : settings::visuals::box_color;
			ImU32 box_col = helper::color_with_opacity(
				box_rgba[0],
				box_rgba[1],
				box_rgba[2],
				box_rgba[3],
				opacity_multiplier * g_spotter_fade_state.box_opacity
			);

			if (settings::visuals::box_type == 0)
			{
				helper::box(c1, c2, box_col);
			}
			else
			{
				ImRect rect_bb(c1.x, c1.y, c1.x + c2.x, c1.y + c2.y);
				helper::corner_box(draw, rect_bb.Min, rect_bb.Max, box_col, 1.f);
			}
		}

		float box_center_x = left + (right - left) * 0.5f;
		float box_top = roundf(top);
		float box_bottom = roundf(bottom);
		float box_left = roundf(left);
		float box_height = box_bottom - box_top;

		ImFont* const font = spotter_font ?
			(settings::visuals::spotter_font == 0 ? spotter_font_tahoma :
			 settings::visuals::spotter_font == 1 ? spotter_font_sp7 : spotter_font_arial) :
			ImGui::GetFont();
		const float font_size = spotter_font ?
			(settings::visuals::spotter_font == 0 ? font_config::tahoma::font_size :
			 settings::visuals::spotter_font == 1 ? font_config::sp7::font_size : font_config::arial::font_size) :
			ImGui::GetFontSize();

		if (settings::visuals::healthbar && entity.humanoid.address != 0 && entity.max_health > 0.f)
		{
			const float raw_health = entity.health;
			const float max_health = entity.max_health;

			health_smooth_state_t& state = g_health_smooth_states[entity.humanoid.address];
			state.smooth_health += (raw_health - state.smooth_health) * 0.15f;
			state.smooth_health = helper::clamp(state.smooth_health, 0.0f, max_health);

			const float health_percent = state.smooth_health / max_health;
			const float health_percent_actual = health_percent * 100.f;
			const bool use_health_based = settings::visuals::health_based_healthbar;

			float color_r, color_g, color_b;
			if (use_health_based)
			{
				if (health_percent_actual > 60.f) { color_r = 0.f; color_g = 1.f; color_b = 0.f; }
				else if (health_percent_actual >= 50.f) { color_r = 1.f; color_g = 0.647f; color_b = 0.f; }
				else if (health_percent_actual <= 20.f) { color_r = 1.f; color_g = 0.f; color_b = 0.f; }
				else { color_r = 1.f; color_g = 0.647f; color_b = 0.f; }
			}
			else
			{
				const float* color = settings::visuals::healthbar_color;
				color_r = color[0];
				color_g = color[1];
				color_b = color[2];
			}

			const float healthbar_fade = g_spotter_fade_state.healthbar_opacity;
			const float transparency = 255.f * opacity_multiplier * healthbar_fade;
			const float health_bar_height = box_height + 2.f;
			const int bg_alpha = static_cast<int>(255 * opacity_multiplier * healthbar_fade);
			draw->AddRectFilled(ImVec2(box_left - 7.f, box_top - 2.f), ImVec2(box_left - 3.f, box_bottom + 2.f), IM_COL32(0, 0, 0, bg_alpha));
			if (health_percent > 0.f)
			{
				const float health_fill_height = health_bar_height * health_percent;
				const ImVec2 fill_min(box_left - 6.f, box_bottom + 1.f - health_fill_height);
				const ImVec2 fill_max(box_left - 4.f, box_bottom + 1.f);
				draw->AddRectFilled(fill_min, fill_max, IM_COL32(color_r * 255.f, color_g * 255.f, color_b * 255.f, static_cast<int>(transparency)));
			}
		}

		if (settings::visuals::name)
		{
			const float name_fade = g_spotter_fade_state.name_opacity;
			std::string name_to_display = (settings::visuals::name_display_type == 0) ? entity.display_name : entity.name;
			float text_width = font->CalcTextSizeA(font_size, FLT_MAX, 0.f, name_to_display.c_str()).x;

			int text_len = ImTextCountCharsFromUtf8(name_to_display.c_str(), name_to_display.c_str() + name_to_display.size());
			float actual_width = text_width + (settings::visuals::blend && text_len > 1 ? (text_len - 1) * 1.0f : 0.0f);
			float name_x = box_center_x - actual_width * 0.5f;
			float name_y = roundf(box_top) - font_size - 5.f;

			if (settings::visuals::blend)
			{
				ImU32 name_col_start = ImGui::ColorConvertFloat4ToU32(
					{
						settings::visuals::name_color_blend_start[0],
						settings::visuals::name_color_blend_start[1],
						settings::visuals::name_color_blend_start[2],
						settings::visuals::name_color_blend_start[3]
					}
				);
				ImU32 name_col_end = ImGui::ColorConvertFloat4ToU32(
					{
						settings::visuals::name_color_blend_end[0],
						settings::visuals::name_color_blend_end[1],
						settings::visuals::name_color_blend_end[2],
						settings::visuals::name_color_blend_end[3]
					}
				);
				name_col_start = helper::apply_opacity(name_col_start, opacity_multiplier * name_fade);
				name_col_end = helper::apply_opacity(name_col_end, opacity_multiplier * name_fade);

				helper::draw_text_blended(draw, font, font_size,
					ImVec2(name_x, name_y),
					name_col_start, name_col_end, name_to_display.c_str());
			}
			else
			{
				const float* name_rgba = override_color ? override_color : settings::visuals::name_color;
				ImU32 name_col = ImGui::ColorConvertFloat4ToU32({ name_rgba[0], name_rgba[1], name_rgba[2], name_rgba[3] });
				name_col = helper::apply_opacity(name_col, opacity_multiplier * name_fade);

				helper::draw_text_outlined(draw, font, font_size,
					ImVec2(name_x, name_y),
					name_col, name_to_display.c_str());
			}
		}
	}
}