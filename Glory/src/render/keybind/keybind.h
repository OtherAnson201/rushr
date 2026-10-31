#pragma once
#include <windows.h>
#include <string>
#include <imgui/imgui.h>

namespace keybind
{
	enum class activation_mode
	{
		toggle,
		hold,
		always
	};

	struct keybind_t
	{
		int key = 0;
		activation_mode mode = activation_mode::hold;
		bool state = false;
		bool was_pressed = false;
	};

	bool is_active(keybind_t& kb);

	bool key_down(int vk_code);
	std::string get_key_name(int vk_code);

	ImGuiKey vk_to_imgui_key(int vk_code);
	int imgui_key_to_vk(ImGuiKey imgui_key);
	std::string truncate_text_left(const std::string& text, float max_width);
	bool custom_keybind(const char* str_id, int& key, float max_width = 0.0f);
	bool keybind_with_mode(const char* label, int& key, activation_mode& mode, const char* unique_id, float max_width = 0.0f);
}

