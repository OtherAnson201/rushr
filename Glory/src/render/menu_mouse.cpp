#include "menu_mouse.h"

#include <imgui/imgui.h>
#include <imgui/imgui_internal.h>

namespace
{

	int down_now = 0;
	int pressed_now = 0;

	int of_menu = 0;

	// true if imgui is using a mouse button
	bool imgui_holds(int button)
	{
		const ImGuiContext& g = *GImGui;

		for (int n = g.InputEventsQueue.Size - 1; n >= 0; n--)
		{
			const ImGuiInputEvent& event = g.InputEventsQueue[n];
			if (event.Type == ImGuiInputEventType_MouseButton && event.MouseButton.Button == button)
				return event.MouseButton.Down;
		}

		return g.IO.MouseDown[button];
	}
}

// start routing mouse input this frame
menu_mouse::frame_t menu_mouse::begin_frame(int down, bool had_mouse)
{
	frame_t frame{};
	frame.idle = down == 0 && down_now == 0;

	const int pressed = down & ~down_now;

	if (had_mouse)
	{
		of_menu |= pressed;
	}
	of_menu &= down;

	ImGuiIO& io = ImGui::GetIO();
	for (int button = 0; button < button_count; button++)
	{
		const int bit = 1 << button;
		if ((down & bit) == 0 && (down_now & bit) == 0 && imgui_holds(button))
		{
			io.AddMouseButtonEvent(button, false);
			frame.let_go |= bit;
		}
	}

	pressed_now = pressed;
	down_now = down;
	return frame;
}

// true if the game has the mouse, not the menu
bool menu_mouse::game_holds_a_button()
{
	return of_menu == 0 && down_now != 0;
}

// finish mouse routing this frame
bool menu_mouse::end_frame(bool menu_drawn)
{
	if (!menu_drawn)
	{

		of_menu = 0;
		return false;
	}

	const int of_game = down_now & ~of_menu;

	if ((pressed_now & of_game) != 0 && ImGui::IsPopupOpen(nullptr, ImGuiPopupFlags_AnyPopup))
	{
		ImGui::ClosePopupsOverWindow(nullptr, false);
	}

	if (of_menu != 0)
		return true;

	if (of_game != 0)
		return false;

	return ImGui::IsWindowHovered(ImGuiHoveredFlags_AnyWindow | ImGuiHoveredFlags_AllowWhenBlockedByPopup | ImGuiHoveredFlags_AllowWhenBlockedByActiveItem);
}
