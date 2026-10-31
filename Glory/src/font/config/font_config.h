#pragma once
#include <imgui/imgui.h>

namespace font_config
{
    namespace tahoma
    {
        inline float font_size = 14.0f;
    }
    namespace sp7
    {
        inline float font_size = 14.0f;
    }
    namespace arial
    {
        inline float font_size = 14.0f;
    }
}

extern ImFont* spotter_font;
extern ImFont* spotter_font_tahoma;
extern ImFont* spotter_font_sp7;
extern ImFont* spotter_font_arial;
