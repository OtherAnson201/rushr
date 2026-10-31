#pragma once

namespace menu_mouse
{

	inline constexpr int button_count = 5;

	struct frame_t
	{

		int let_go = 0;

		bool idle = false;
	};

	frame_t begin_frame(int down, bool had_mouse);

	bool game_holds_a_button();

	bool end_frame(bool menu_drawn);
}
