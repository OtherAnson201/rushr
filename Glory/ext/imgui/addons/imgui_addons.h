//============ Copyright KiwiHax, All rights reserved ============//
//
//  Purpose: 
//
//================================================================//

#pragma once

#include "../imgui.h"

#define IMADD_ANIMATIONS_SPEED	0.07f

#include <vector>

namespace ImAdd
{
	// Helper
	ImVec4  HexToColorVec4(unsigned int hex_color, float alpha = 1.0f);

	// Separators
	void    SeparatorText(const char* label, float thickness = 1.0f);
	void    VSeparator(float margin = 0.0f, float thickness = 1.0f);
	void    DoubleText(ImVec4 color1, ImVec4 color2, const char* label1, const char* label2);

	// Selectable
	bool	SelectableLabel(const char* label, bool selected, bool centered = false, const ImVec2& size_arg = ImVec2(0, 0));
	bool	SelectableFrame(const char* label, bool selected, bool centered = false, const ImVec2& size_arg = ImVec2(0, 0));
	bool	Tab(const char* label, int* v, int current_id, ImDrawFlags draw_flags = 0, const ImVec2& size_arg = ImVec2(0, 0));

	bool	Button(const char* label, const ImVec2& size_arg = ImVec2(0, 0), ImDrawFlags draw_flags = 0, ImGuiButtonFlags button_flags = 0);

	// Togglables
	bool	CheckBox(const char* label, bool* checked);

	// Others
	bool	Combo(const char* label, int* selected_index, std::vector<const char*> items);
	bool	MultiCombo(const char* label, int* selected_mask, std::vector<const char*> items);
	bool    InputText(ImTextureRef icon_texture, const char* label, char* buf, size_t buf_size, ImGuiInputTextFlags flags = 0);

	// Keybind
	bool	KeyBind(const char* str_id, ImGuiKey* k, const ImVec2& size_arg = ImVec2(0, 0));

	// Color Stuff
	bool	SliderAlpha(const char* str_id, ImVec4& col);
	bool	SliderHue(const char* str_id, ImVec4& col);
	bool	ColorPalette(const char* str_id, ImVec4& col, const ImVec2& size_arg = ImVec2(0, 0));
	bool	ColorButton(const char* desc_id, const ImVec4& col, bool has_alpha, float square_sz);
	void	ColorPicker4(const char* label, float col[4], bool has_alpha);
	bool    ColorEdit4(const char* name, float col[4]);

	// Slider
	bool	SliderScalar(const char* label, ImGuiDataType data_type, void* p_data, const void* p_min, const void* p_max, const char* format = NULL);
	bool	SliderFloat(const char* label, float* v, float v_min, float v_max, const char* format = "%.1f");
	bool	SliderInt(const char* label, int* v, int v_min, int v_max, const char* format = "%d");

	// Grouping
	bool	BeginChild(const char* label, const ImVec2& size_arg = ImVec2(0, 0));
	void	EndChild();

	// Render
	void	RenderArrow(ImDrawList* draw_list, ImVec2 pos, ImU32 col, float sz, ImGuiDir direction);
	void	RenderCheckMark(ImDrawList* draw_list, ImVec2 pos, ImU32 col, float sz);
	
	// SubTabs
	void	RenderSubTabs(const std::vector<const char*>& subtabs, int& current_subtab);
	
	// Color
	bool	ColorEdit4WithHex(const char* label, float col[4]);
}
