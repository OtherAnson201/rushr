//============ Copyright KiwiHax, All rights reserved ============//
//
//  Purpose: 
//
//================================================================//

#include "../imgui_internal.h"
#include "imgui_addons.h"

#include <map>
#include <string>
#include <cstdio>
#include <cstring>

using namespace ImGui;

ImVec4 ImAdd::HexToColorVec4(unsigned int hex_color, float alpha) {
    ImVec4 color;

    color.x = ((hex_color >> 16) & 0xFF) / 255.0f;
    color.y = ((hex_color >> 8) & 0xFF) / 255.0f;
    color.z = (hex_color & 0xFF) / 255.0f;
    color.w = alpha;

    return color;
}

void ImAdd::SeparatorText(const char* label, float thickness)
{
    ImGuiWindow* window = GetCurrentWindow();
    if (window->SkipItems)
        return;

    ImGuiContext& g = *GImGui;
    const ImGuiStyle& style = g.Style;
    const ImGuiID id = window->GetID(label);
    const ImVec2 label_size = CalcTextSize(label, NULL, true);

    ImVec2 pos = window->DC.CursorPos;
    ImVec2 size = CalcItemSize(ImVec2(-0.1f, g.FontSize), label_size.x, g.FontSize);

    const ImRect total_bb(pos, pos + size);
    ItemSize(total_bb);
    if (!ItemAdd(total_bb, id)) {
        return;
    }

    PushStyleColor(ImGuiCol_Text, style.Colors[ImGuiCol_TextDisabled]);
    RenderText(pos, label);
    ImGui::PopStyleColor();

    if (thickness > 0)
    {
        window->DrawList->AddLine(pos + ImVec2(label_size.x + style.ItemInnerSpacing.x + thickness, size.y / 2), pos + ImVec2(size.x - thickness, size.y / 2), GetColorU32(ImGuiCol_Border), thickness);
    }
}

void ImAdd::VSeparator(float margin, float thickness)
{
    if (thickness <= 0)
        return;

    ImGuiWindow* window = GetCurrentWindow();
    if (window->SkipItems)
        return;

    ImGuiContext& g = *GImGui;
    const ImGuiStyle& style = g.Style;

    ImVec2 pos = window->DC.CursorPos;
    ImVec2 size = CalcItemSize(ImVec2(thickness, -0.1f), thickness, thickness);

    const ImRect bb(pos, pos + size);
    const ImRect bb_rect(pos + ImVec2(0, margin), pos + size - ImVec2(0, margin));

    ItemSize(ImVec2(thickness, 0.0f));
    if (!ItemAdd(bb, 0))
        return;

    window->DrawList->AddRectFilled(bb_rect.Min, bb_rect.Max, GetColorU32(ImGuiCol_Border));
}

void ImAdd::DoubleText(ImVec4 color1, ImVec4 color2, const char* label1, const char* label2)
{
    ImGuiWindow* window = GetCurrentWindow();
    if (window->SkipItems)
        return;

    ImGuiContext& g = *GImGui;
    const ImGuiStyle& style = g.Style;
    const ImGuiID id = window->GetID(std::string(label1 + std::string(label2)).c_str());
    const ImVec2 label1_size = CalcTextSize(label1, NULL, true);
    const ImVec2 label2_size = CalcTextSize(label2, NULL, true);

    ImVec2 pos = window->DC.CursorPos;
    ImVec2 size = CalcItemSize(ImVec2(-0.1f, g.FontSize), label1_size.x + label2_size.x, g.FontSize);

    const ImRect total_bb(pos, pos + size);
    ItemSize(total_bb);
    if (!ItemAdd(total_bb, id)) {
        return;
    }

    PushStyleColor(ImGuiCol_Text, color1);
    RenderText(pos, label1);
    PopStyleColor();

    PushStyleColor(ImGuiCol_Text, color2);
    RenderText(pos + ImVec2(size.x - ImGui::CalcTextSize(label2).x, 0), label2);
    PopStyleColor();
}

bool ImAdd::SelectableLabel(const char* label, bool selected, bool centered, const ImVec2& size_arg)
{
    ImGuiWindow* window = GetCurrentWindow();
    if (window->SkipItems)
        return false;

    ImGuiContext& g = *GImGui;
    const ImGuiStyle& style = g.Style;
    const ImGuiID id = window->GetID(label);
    const ImVec2 label_size = CalcTextSize(label, NULL, true);

    ImVec2 pos = window->DC.CursorPos;
    ImVec2 size = CalcItemSize(size_arg, label_size.x, label_size.y);

    const ImRect total_bb(pos, pos + size);
    ItemSize(size);
    if (!ItemAdd(total_bb, id))
        return false;

    // Behaviors
    bool hovered, held;
    bool pressed = ButtonBehavior(total_bb, id, &hovered, &held);

    // Colors
    ImVec4 colLabel = GetStyleColorVec4(hovered || selected ? ImGuiCol_Text : ImGuiCol_TextDisabled);

    // Animations
    struct stColors_State {
        ImColor Label;
    };

    static std::map<ImGuiID, stColors_State> anim;
    auto it_anim = anim.find(id);

    if (it_anim == anim.end())
    {
        anim.insert({ id, stColors_State() });
        it_anim = anim.find(id);

        it_anim->second.Label = colLabel;
    }

    it_anim->second.Label.Value = ImLerp(it_anim->second.Label.Value, colLabel, 1.0f / IMADD_ANIMATIONS_SPEED * GetIO().DeltaTime);

    RenderNavCursor(total_bb, id);

    PushStyleColor(ImGuiCol_Text, it_anim->second.Label.Value);
    RenderText(pos + ImTrunc(ImVec2(centered ? (size.x / 2 - label_size.x / 2) : 0.0f, size.y / 2 - label_size.y / 2)), label);
    PopStyleColor();

    return pressed;
}

bool ImAdd::SelectableFrame(const char* label, bool selected, bool centered, const ImVec2& size_arg)
{
    ImGuiWindow* window = GetCurrentWindow();
    if (window->SkipItems)
        return false;

    ImGuiContext& g = *GImGui;
    const ImGuiStyle& style = g.Style;
    const ImGuiID id = window->GetID(label);
    const ImVec2 label_size = CalcTextSize(label, NULL, true);

    ImVec2 pos = window->DC.CursorPos;
    ImVec2 size = CalcItemSize(size_arg, label_size.x, label_size.y);

    const ImRect total_bb(pos, pos + size);
    ItemSize(size);
    if (!ItemAdd(total_bb, id))
        return false;

    // Behaviors
    bool hovered, held;
    bool pressed = ButtonBehavior(total_bb, id, &hovered, &held);

    // Colors
    ImVec4 colLabel = GetStyleColorVec4(hovered || selected ? ImGuiCol_Text : ImGuiCol_TextDisabled);
    ImVec4 colFrame = GetStyleColorVec4(selected ? ImGuiCol_SliderGrabActive : ImGuiCol_FrameBg);
    ImVec4 colBorder = GetStyleColorVec4(selected ? ImGuiCol_SliderGrab : ImGuiCol_Border);

    // Animations
    struct stColors_State {
        ImColor Label;
        ImColor Frame;
        ImColor Border;
    };

    static std::map<ImGuiID, stColors_State> anim;
    auto it_anim = anim.find(id);

    if (it_anim == anim.end())
    {
        anim.insert({ id, stColors_State() });
        it_anim = anim.find(id);

        it_anim->second.Label = colLabel;
        it_anim->second.Frame = colFrame;
        it_anim->second.Border = colBorder;
    }

    it_anim->second.Label.Value = ImLerp(it_anim->second.Label.Value, colLabel, 1.0f / IMADD_ANIMATIONS_SPEED * GetIO().DeltaTime);
    it_anim->second.Frame.Value = ImLerp(it_anim->second.Frame.Value, colFrame, 1.0f / IMADD_ANIMATIONS_SPEED * GetIO().DeltaTime);
    it_anim->second.Border.Value = ImLerp(it_anim->second.Border.Value, colBorder, 1.0f / IMADD_ANIMATIONS_SPEED * GetIO().DeltaTime);

    RenderNavCursor(total_bb, id);

    window->DrawList->AddRectFilled(pos, pos + size, it_anim->second.Frame, style.FrameRounding);

    if (style.FrameBorderSize > 0)
    {
        window->DrawList->AddRect(pos, pos + size, it_anim->second.Border, style.FrameRounding, 0, style.FrameBorderSize);
    }

    PushStyleColor(ImGuiCol_Text, it_anim->second.Label.Value);
    RenderText(pos + ImTrunc(ImVec2(centered ? (size.x / 2 - label_size.x / 2) : 0.0f, size.y / 2 - label_size.y / 2)), label);
    PopStyleColor();

    return pressed;
}

bool ImAdd::Tab(const char* label, int* v, int current_id, ImDrawFlags draw_flags, const ImVec2& size_arg)
{
    ImGuiWindow* window = GetCurrentWindow();
    if (window->SkipItems)
        return false;

    ImGuiContext& g = *GImGui;
    const ImGuiStyle& style = g.Style;
    const ImGuiID id = window->GetID(label);
    const ImVec2 label_size = CalcTextSize(label, NULL, true);

    ImVec2 pos = window->DC.CursorPos;
    ImVec2 size = CalcItemSize(size_arg, label_size.x + style.FramePadding.x * 2.0f, label_size.y + style.FramePadding.y * 2.0f);

    const ImRect total_bb(pos, pos + size);
    ItemSize(size);
    if (!ItemAdd(total_bb, id))
        return false;

    // Behaviors
    bool hovered, held;
    bool pressed = ButtonBehavior(total_bb, id, &hovered, &held);
    if (pressed)
        *v = current_id;

    bool active = (*v == current_id);

    // Colors
    ImVec4 colLabel = GetStyleColorVec4(active || hovered ? ImGuiCol_Text : ImGuiCol_TextDisabled);

    ImVec4 colLineMain = GetStyleColorVec4(ImGuiCol_SliderGrab);
    ImVec4 colLineNull = colLineMain;
    colLineNull.w = 0.0f;
    ImVec4 colLine = active ? colLineMain : colLineNull;

    // Animations
    struct stColors_State {
        ImColor Label;
        ImColor Line;
    };

    static std::map<ImGuiID, stColors_State> anim;
    auto it_anim = anim.find(id);

    if (it_anim == anim.end())
    {
        anim.insert({ id, stColors_State() });
        it_anim = anim.find(id);

        it_anim->second.Label = colLabel;
        it_anim->second.Line = colLine;
    }

    it_anim->second.Label.Value = ImLerp(it_anim->second.Label.Value, colLabel, 1.0f / IMADD_ANIMATIONS_SPEED * GetIO().DeltaTime);
    it_anim->second.Line.Value = ImLerp(it_anim->second.Line.Value, colLine, 1.0f / IMADD_ANIMATIONS_SPEED * GetIO().DeltaTime);

    RenderNavCursor(total_bb, id);

    if (style.FrameBorderSize > 0)
    {
        window->DrawList->AddRect(pos, pos + ImVec2(size.x, active ? (size.y + style.FrameBorderSize) : size.y), GetColorU32(ImGuiCol_Border), style.FrameRounding, draw_flags, style.FrameBorderSize);
        window->DrawList->AddLine(pos, pos + ImVec2(size.x, 0), it_anim->second.Line, style.FrameBorderSize);
    }

    PushStyleColor(ImGuiCol_Text, it_anim->second.Label.Value);
    RenderText(pos + size / 2 - label_size / 2 - ImVec2(0, 1), label);
    ImGui::PopStyleColor();

    return pressed;
}

bool ImAdd::Button(const char* label, const ImVec2& size_arg, ImDrawFlags draw_flags, ImGuiButtonFlags button_flags)
{
    ImGuiWindow* window = GetCurrentWindow();
    if (window->SkipItems)
        return false;

    ImGuiContext& g = *GImGui;
    const ImGuiStyle& style = g.Style;
    const ImGuiID id = window->GetID(label);
    const ImVec2 label_size = CalcTextSize(label, NULL, true);

    ImVec2 pos = window->DC.CursorPos;
    ImVec2 size = CalcItemSize(size_arg, label_size.x + style.FramePadding.x * 2.0f, label_size.y + style.FramePadding.y * 2.0f);

    const ImRect total_bb(pos, pos + size);
    ItemSize(size);
    if (!ItemAdd(total_bb, id))
        return false;

    // Behaviors
    bool hovered, held;
    bool pressed = ButtonBehavior(total_bb, id, &hovered, &held, button_flags);

    // Colors
    ImVec4 colFrame = GetStyleColorVec4((hovered && held) ? ImGuiCol_ButtonActive : hovered ? ImGuiCol_ButtonHovered : ImGuiCol_Button);
    colFrame.w = style.Alpha;

    // Animations
    struct stColors_State {
        ImColor Frame;
    };

    static std::map<ImGuiID, stColors_State> anim;
    auto it_anim = anim.find(id);

    if (it_anim == anim.end())
    {
        anim.insert({ id, stColors_State() });
        it_anim = anim.find(id);

        it_anim->second.Frame = colFrame;
    }

    it_anim->second.Frame.Value = ImLerp(it_anim->second.Frame.Value, colFrame, 1.0f / IMADD_ANIMATIONS_SPEED * GetIO().DeltaTime);

    RenderNavCursor(total_bb, id);

    window->DrawList->AddRectFilled(total_bb.Min, total_bb.Max, it_anim->second.Frame, style.FrameRounding, draw_flags);
    window->DrawList->AddRectFilledMultiColorRounded(total_bb.Min, total_bb.Max, GetColorU32(ImGuiCol_ButtonShadow, 0.0f), GetColorU32(ImGuiCol_ButtonShadow, 0.0f), GetColorU32(ImGuiCol_ButtonShadow), GetColorU32(ImGuiCol_ButtonShadow), style.FrameRounding, draw_flags);

    if (style.FrameBorderSize > 0)
    {
        window->DrawList->AddRect(total_bb.Min, total_bb.Max, GetColorU32(ImGuiCol_Border), style.FrameRounding, draw_flags, style.FrameBorderSize);
    }

    RenderTextClipped(total_bb.Min + style.FramePadding, total_bb.Max - style.FramePadding, label, NULL, &label_size, style.ButtonTextAlign, &total_bb);

    return pressed;
}

bool ImAdd::CheckBox(const char* label, bool* checked)
{
    ImGuiWindow* window = GetCurrentWindow();
    if (window->SkipItems)
        return false;

    ImGuiContext& g = *GImGui;
    const ImGuiStyle& style = g.Style;
    const ImGuiID id = window->GetID(label);
    const ImVec2 label_size = CalcTextSize(label, NULL, true);
    const float square_sz = g.FontSize;

    ImVec2 pos = window->DC.CursorPos;
    ImVec2 size = ImVec2(square_sz + style.ItemInnerSpacing.x + label_size.x, square_sz);

    const ImRect check_bb(pos, pos + ImVec2(square_sz, square_sz));
    const ImRect total_bb(pos, pos + size);
    ItemSize(size);
    if (!ItemAdd(total_bb, id))
        return false;

    // Behaviors
    bool hovered, held;
    bool pressed = ButtonBehavior(total_bb, id, &hovered, &held);

    if (pressed) *checked = !*checked;

    // Colors
    ImVec4 colLabel = GetStyleColorVec4((*checked) ? ImGuiCol_Text : ImGuiCol_TextDisabled);
    ImVec4 colFrame = GetStyleColorVec4((hovered && held && !*checked) ? ImGuiCol_FrameBgActive : (hovered && !*checked) ? ImGuiCol_FrameBgHovered : ImGuiCol_FrameBg);

    ImVec4 colCheckMain = GetStyleColorVec4(ImGuiCol_SliderGrab);
    ImVec4 colCheckNull = colCheckMain;
    colCheckNull.w = 0.0f;
    ImVec4 colCheck = *checked ? colCheckMain : colCheckNull;

    // Animations
    struct stColors_State {
        ImColor Label;
        ImColor Frame;
        ImColor Check;
    };

    static std::map<ImGuiID, stColors_State> anim;
    auto it_anim = anim.find(id);

    if (it_anim == anim.end())
    {
        anim.insert({ id, stColors_State() });
        it_anim = anim.find(id);

        it_anim->second.Label = colLabel;
        it_anim->second.Frame = colFrame;
        it_anim->second.Check = colCheck;
    }

    it_anim->second.Label.Value = ImLerp(it_anim->second.Label.Value, colLabel, 1.0f / IMADD_ANIMATIONS_SPEED * GetIO().DeltaTime);
    it_anim->second.Frame.Value = ImLerp(it_anim->second.Frame.Value, colFrame, 1.0f / IMADD_ANIMATIONS_SPEED * GetIO().DeltaTime);
    it_anim->second.Check.Value = ImLerp(it_anim->second.Check.Value, colCheck, 1.0f / IMADD_ANIMATIONS_SPEED * GetIO().DeltaTime);

    RenderNavCursor(total_bb, id);

    window->DrawList->AddRectFilled(check_bb.Min, check_bb.Max, it_anim->second.Frame, style.FrameRounding);

    if (style.FrameBorderSize > 0)
    {
        window->DrawList->AddRect(check_bb.Min, check_bb.Max, GetColorU32(ImGuiCol_Border), style.FrameRounding, 0, style.FrameBorderSize);
    }

    //const float pad = ImMax(1.0f, IM_TRUNC(square_sz / 4.3f));
    const float pad = style.FrameBorderSize + 1.0f;
    window->DrawList->AddRectFilled(check_bb.Min + ImVec2(pad, pad), check_bb.Max - ImVec2(pad, pad), it_anim->second.Check, style.FrameRounding);
    //ImAdd::RenderCheckMark(window->DrawList, check_bb.Min + ImVec2(pad, pad), it_anim->second.Check, square_sz - pad * 2.0f);

	PushStyleColor(ImGuiCol_Text, it_anim->second.Label.Value);
    RenderText(pos + ImVec2(size.y + style.ItemInnerSpacing.x, 0), label);
    PopStyleColor();

    return pressed;
}

bool ImAdd::Combo(const char* label, int* selected_index, std::vector<const char*> items)
{
    ImGuiWindow* window = GetCurrentWindow();
    if (window->SkipItems)
        return false;

    ImGuiContext& g = *GImGui;
    const ImGuiStyle& style = g.Style;
    const ImGuiID id = window->GetID(label);
    const ImVec2 label_size = CalcTextSize(label, NULL, true);

    const float width = CalcItemWidth();
    const float height = GetFrameHeight();

    int items_count = items.size();

    ImVec2 pos = window->DC.CursorPos;
    ImVec2 size = ImVec2(width, height + (label_size.x > 0 ? (g.FontSize + style.ItemInnerSpacing.y) : 0.0f));

    const ImRect total_bb(pos, pos + size);
    const ImRect combo_bb(pos + ImVec2(0.0f, label_size.x > 0 ? (g.FontSize + style.ItemInnerSpacing.y) : 0.0f), pos + size);
    ItemSize(size);
    if (!ItemAdd(total_bb, id))
        return false;

    // Behaviors
    bool hovered, held;
    bool pressed = ButtonBehavior(total_bb, id, &hovered, &held);

    std::string popup_str_id = std::string(std::string(label) + "::combo_popup");

    if (pressed)
    {
        OpenPopup(popup_str_id.c_str());
    }

    PushStyleVar(ImGuiStyleVar_WindowPadding, style.FramePadding);
    PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(style.ItemSpacing.x, style.FramePadding.y));
    if (BeginPopupEx(GetID(popup_str_id.c_str()), ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove))
    {
        SetWindowPos(combo_bb.Min + ImVec2(0, height + style.FramePadding.y), ImGuiCond_Always);
        SetWindowSize(ImVec2(width, ImGui::GetFontSize() * items_count + style.FramePadding.y * (items_count + 1)), ImGuiCond_Always);

        for (int i = 0; i < items_count; i++)
        {
            if (ImAdd::SelectableLabel(items[i], i == *selected_index, false, ImVec2(GetContentRegionAvail().x, GetFontSize())))
            {
                *selected_index = i;
                CloseCurrentPopup();
            }
        }

        EndPopup();
    }
    PopStyleVar(2);

    // Colors
    ImVec4 colLabel = GetStyleColorVec4(IsPopupOpen(popup_str_id.c_str()) ? ImGuiCol_Text : ImGuiCol_TextDisabled);
    ImVec4 colFrame = GetStyleColorVec4((hovered && held) ? ImGuiCol_ButtonActive : hovered ? ImGuiCol_ButtonHovered : ImGuiCol_Button);

    // Animations
    struct stColors_State {
        ImColor Label;
        ImColor Frame;
    };

    static std::map<ImGuiID, stColors_State> anim;
    auto it_anim = anim.find(id);

    if (it_anim == anim.end())
    {
        anim.insert({ id, stColors_State() });
        it_anim = anim.find(id);

        it_anim->second.Frame = colFrame;
        it_anim->second.Label = colLabel;
    }

    it_anim->second.Label.Value = ImLerp(it_anim->second.Label.Value, colLabel, 1.0f / IMADD_ANIMATIONS_SPEED * GetIO().DeltaTime);
    it_anim->second.Frame.Value = ImLerp(it_anim->second.Frame.Value, colFrame, 1.0f / IMADD_ANIMATIONS_SPEED * GetIO().DeltaTime);

    RenderNavCursor(total_bb, id);

    window->DrawList->AddRectFilled(combo_bb.Min, combo_bb.Max, it_anim->second.Frame, style.FrameRounding);
    window->DrawList->AddRectFilledMultiColorRounded(combo_bb.Min, combo_bb.Max, GetColorU32(ImGuiCol_ButtonShadow, 0.0f), GetColorU32(ImGuiCol_ButtonShadow, 0.0f), GetColorU32(ImGuiCol_ButtonShadow), GetColorU32(ImGuiCol_ButtonShadow), style.FrameRounding);

    if (style.FrameBorderSize > 0)
    {
        window->DrawList->AddRect(combo_bb.Min, combo_bb.Max, GetColorU32(ImGuiCol_Border), style.FrameRounding, 0, style.FrameBorderSize);
    }

    std::string preview_item;
    if (*selected_index > items.size()) {
        preview_item = "*unknown item*";
    }
    else
    {
        preview_item = items[*selected_index];
    }

    window->DrawList->AddText(combo_bb.Min + style.FramePadding, GetColorU32(ImGuiCol_Text), preview_item.c_str());

    RenderArrow(window->DrawList, combo_bb.Min + ImVec2(width - GetFontSize() - style.FramePadding.x, style.FramePadding.y), GetColorU32(ImGuiCol_TextDisabled), ImGui::GetFontSize(), ImGuiDir_Down);

    if (label_size.x > 0)
        {
        PushStyleColor(ImGuiCol_Text, it_anim->second.Label.Value);
        RenderText(pos, label);
        PopStyleColor();
    }

    return pressed;
}

bool ImAdd::MultiCombo(const char* label, int* selected_mask, std::vector<const char*> items)
{
    ImGuiWindow* window = GetCurrentWindow();
    if (window->SkipItems)
        return false;

    ImGuiContext& g = *GImGui;
    const ImGuiStyle& style = g.Style;
    const ImGuiID id = window->GetID(label);
    const ImVec2 label_size = CalcTextSize(label, NULL, true);

    const float width = CalcItemWidth();
    const float height = GetFrameHeight();

    int items_count = items.size();

    ImVec2 pos = window->DC.CursorPos;
    ImVec2 size = ImVec2(width, height + (label_size.x > 0 ? (g.FontSize + style.ItemInnerSpacing.y) : 0.0f));

    const ImRect total_bb(pos, pos + size);
    const ImRect combo_bb(pos + ImVec2(0.0f, label_size.x > 0 ? (g.FontSize + style.ItemInnerSpacing.y) : 0.0f), pos + size);
    ItemSize(size);
    if (!ItemAdd(total_bb, id))
        return false;

    bool hovered, held;
    bool pressed = ButtonBehavior(total_bb, id, &hovered, &held);

    std::string popup_str_id = std::string(std::string(label) + "::multicombo_popup");

    if (pressed)
    {
        OpenPopup(popup_str_id.c_str());
    }

    PushStyleVar(ImGuiStyleVar_WindowPadding, style.FramePadding);
    PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(style.ItemSpacing.x, style.FramePadding.y));
    if (BeginPopupEx(GetID(popup_str_id.c_str()), ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove))
    {
        SetWindowPos(combo_bb.Min + ImVec2(0, height + style.FramePadding.y), ImGuiCond_Always);
        SetWindowSize(ImVec2(width, ImGui::GetFontSize() * items_count + style.FramePadding.y * (items_count + 1)), ImGuiCond_Always);

        bool value_changed = false;
        for (int i = 0; i < items_count; i++)
        {
            bool is_selected = (*selected_mask & (1 << i)) != 0;
            if (ImAdd::SelectableLabel(items[i], is_selected, false, ImVec2(GetContentRegionAvail().x, GetFontSize())))
            {
                if (is_selected)
                {
                    *selected_mask &= ~(1 << i);
                }
                else
                {
                    *selected_mask |= (1 << i);
                }
                value_changed = true;
            }
        }

        EndPopup();
        PopStyleVar(2);
        
        if (value_changed)
        {
            return true;
        }
    }
    else
    {
        PopStyleVar(2);
    }

    ImVec4 colLabel = GetStyleColorVec4(IsPopupOpen(popup_str_id.c_str()) ? ImGuiCol_Text : ImGuiCol_TextDisabled);
    ImVec4 colFrame = GetStyleColorVec4((hovered && held) ? ImGuiCol_ButtonActive : hovered ? ImGuiCol_ButtonHovered : ImGuiCol_Button);

    struct stColors_State {
        ImColor Label;
        ImColor Frame;
    };

    static std::map<ImGuiID, stColors_State> anim;
    auto it_anim = anim.find(id);

    if (it_anim == anim.end())
    {
        anim.insert({ id, stColors_State() });
        it_anim = anim.find(id);

        it_anim->second.Frame = colFrame;
        it_anim->second.Label = colLabel;
    }

    it_anim->second.Label.Value = ImLerp(it_anim->second.Label.Value, colLabel, 1.0f / IMADD_ANIMATIONS_SPEED * GetIO().DeltaTime);
    it_anim->second.Frame.Value = ImLerp(it_anim->second.Frame.Value, colFrame, 1.0f / IMADD_ANIMATIONS_SPEED * GetIO().DeltaTime);

    RenderNavCursor(total_bb, id);

    window->DrawList->AddRectFilled(combo_bb.Min, combo_bb.Max, it_anim->second.Frame, style.FrameRounding);
    window->DrawList->AddRectFilledMultiColorRounded(combo_bb.Min, combo_bb.Max, GetColorU32(ImGuiCol_ButtonShadow, 0.0f), GetColorU32(ImGuiCol_ButtonShadow, 0.0f), GetColorU32(ImGuiCol_ButtonShadow), GetColorU32(ImGuiCol_ButtonShadow), style.FrameRounding);

    if (style.FrameBorderSize > 0)
    {
        window->DrawList->AddRect(combo_bb.Min, combo_bb.Max, GetColorU32(ImGuiCol_Border), style.FrameRounding, 0, style.FrameBorderSize);
    }

    std::string preview_item;
    int selected_count = 0;
    for (int i = 0; i < items_count; i++)
    {
        if (*selected_mask & (1 << i))
        {
            selected_count++;
        }
    }

    if (selected_count == 0)
    {
        preview_item = "None";
    }
    else if (selected_count == 1)
    {
        for (int i = 0; i < items_count; i++)
        {
            if (*selected_mask & (1 << i))
            {
                preview_item = items[i];
                break;
            }
        }
    }
    else
    {
        preview_item = std::to_string(selected_count) + " selected";
    }

    window->DrawList->AddText(combo_bb.Min + style.FramePadding, GetColorU32(ImGuiCol_Text), preview_item.c_str());

    RenderArrow(window->DrawList, combo_bb.Min + ImVec2(width - GetFontSize() - style.FramePadding.x, style.FramePadding.y), GetColorU32(ImGuiCol_TextDisabled), ImGui::GetFontSize(), ImGuiDir_Down);

    if (label_size.x > 0)
    {
        PushStyleColor(ImGuiCol_Text, it_anim->second.Label.Value);
        RenderText(pos, label);
        PopStyleColor();
    }

    return false;
}

bool ImAdd::InputText(ImTextureRef icon_texture, const char* label, char* buf, size_t buf_size, ImGuiInputTextFlags flags)
{
    IM_ASSERT(!(flags & ImGuiInputTextFlags_Multiline)); // call InputTextMultiline()

    ImGuiWindow* window = GetCurrentWindow();
    if (window->SkipItems)
        return false;

    ImGuiContext& g = *GImGui;
    const ImGuiStyle& style = g.Style;

    ImVec2 pos = GetCursorScreenPos();
    bool has_icon = icon_texture != nullptr;

    ImGui::BeginChild(label, ImVec2(CalcItemWidth(), GetFrameHeight()), ImGuiChildFlags_None, ImGuiWindowFlags_NoBackground);

    ImVec4 frame_bg_color = GetStyleColorVec4(ImGuiCol_FrameBg);
    if (frame_bg_color.w > 0.0f) {
        window->DrawList->AddRectFilled(pos, pos + GetWindowSize(), GetColorU32(ImGuiCol_FrameBg), style.FrameRounding);
    }

    if (style.FrameBorderSize > 0) {
        ImVec4 border_color = GetStyleColorVec4(ImGuiCol_Border);
        if (border_color.w > 0.0f) {
            window->DrawList->AddRect(pos, pos + GetWindowSize(), GetColorU32(ImGuiCol_Border), style.FrameRounding, 0, style.FrameBorderSize);
        }
    }

    if (has_icon) {
        window->DrawList->AddImage(icon_texture, pos + style.FramePadding, pos + ImVec2(GetWindowHeight(), GetWindowHeight()) - style.FramePadding, ImVec2(0, 0), ImVec2(1, 1), ImGui::GetColorU32(ImGuiCol_Text));
    }

    PushStyleColor(ImGuiCol_FrameBg, ImVec4(0, 0, 0, 0));
    PushStyleVar(ImGuiStyleVar_FrameBorderSize, 0);
    PushItemWidth(has_icon ? (GetWindowWidth() - GetFontSize() - style.FramePadding.x) : GetWindowWidth());
    if (has_icon) {
        SetCursorScreenPos(pos + ImVec2(GetFontSize() + style.FramePadding.x, 0));
    }
    bool input = InputTextEx(std::string("##" + std::string(label)).c_str(), NULL, buf, (int)buf_size, ImVec2(0, 0), flags);
    PopItemWidth();
    PopStyleVar();
    PopStyleColor();

    if (has_icon) {
        GetForegroundDrawList()->AddRectFilledMultiColor(pos + ImVec2(GetFontSize() + style.FramePadding.x, style.FrameBorderSize), pos + ImVec2(GetWindowHeight(), GetWindowHeight() - style.FrameBorderSize), GetColorU32(ImGuiCol_FrameBg), GetColorU32(ImGuiCol_FrameBg, 0.0f), GetColorU32(ImGuiCol_FrameBg, 0.0f), GetColorU32(ImGuiCol_FrameBg));
    }

    if (!IsItemActive() && strlen(buf) == 0) {
        window->DrawList->AddText(pos + ImVec2(has_icon ? GetWindowHeight() : style.FramePadding.x, style.FramePadding.y) + ImVec2(1, 1), IM_COL32(0, 0, 0, style.Alpha * 255), label);
        window->DrawList->AddText(pos + ImVec2(has_icon ? GetWindowHeight() : style.FramePadding.x, style.FramePadding.y) + ImVec2(-1, -1), IM_COL32(0, 0, 0, style.Alpha * 255), label);
        window->DrawList->AddText(pos + ImVec2(has_icon ? GetWindowHeight() : style.FramePadding.x, style.FramePadding.y) + ImVec2(-1, 1), IM_COL32(0, 0, 0, style.Alpha * 255), label);
        window->DrawList->AddText(pos + ImVec2(has_icon ? GetWindowHeight() : style.FramePadding.x, style.FramePadding.y) + ImVec2(1, -1), IM_COL32(0, 0, 0, style.Alpha * 255), label);
        window->DrawList->AddText(pos + ImVec2(has_icon ? GetWindowHeight() : style.FramePadding.x, style.FramePadding.y), GetColorU32(ImGuiCol_TextDisabled), label);
    }

    ImGui::EndChild();

    return input;
}

bool ImAdd::KeyBind(const char* str_id, ImGuiKey* k, const ImVec2& size_arg)
{
    ImGuiWindow* window = ImGui::GetCurrentWindow();
    if (window->SkipItems)
        return false;

    ImGuiContext& g = *GImGui;
    ImGuiIO& io = g.IO;
    const ImGuiStyle& style = g.Style;
    const ImGuiID id = window->GetID(str_id);

    ImVec2 pos = window->DC.CursorPos;

    char buf_display[32] = "Unbinded";

    bool is_selecing = false;

    if (*k != 0 && g.ActiveId != id)
    {
        strcpy_s(buf_display, sizeof buf_display, GetKeyName(*k));
    }
    else if (g.ActiveId == id)
    {
        is_selecing = true;
        strcpy_s(buf_display, sizeof buf_display, "...");
    }

    ImVec2 size = CalcItemSize(size_arg, CalcTextSize(GetKeyName(*k)).x, g.FontSize);
    ImRect frame_bb(pos, pos + size);
    ImRect total_bb(pos, frame_bb.Max);

    ImGui::ItemSize(total_bb);
    if (!ImGui::ItemAdd(total_bb, id))
        return false;

    const bool hovered = ImGui::ItemHoverable(frame_bb, id, 0);

    // Colors
    ImVec4 colLabel = GetStyleColorVec4(is_selecing ? ImGuiCol_Text : ImGuiCol_TextDisabled);

    // Animations
    struct stColors_State {
        ImColor Label;
    };

    static std::map<ImGuiID, stColors_State> anim;
    auto it_anim = anim.find(id);

    if (it_anim == anim.end())
    {
        anim.insert({ id, stColors_State() });
        it_anim = anim.find(id);

        it_anim->second.Label = colLabel;
    }

    it_anim->second.Label.Value = ImLerp(it_anim->second.Label.Value, colLabel, 1.0f / IMADD_ANIMATIONS_SPEED * GetIO().DeltaTime);

    if (hovered)
    {
        ImGui::SetHoveredID(id);
        g.MouseCursor = ImGuiMouseCursor_Hand;
    }

    const bool user_clicked = hovered && IsMouseClicked(ImGuiMouseButton_Left);

    if (user_clicked)
    {
        ImGui::SetActiveID(id, window);
        ImGui::FocusWindow(window);
    }
    else if (IsMouseClicked(ImGuiMouseButton_Left))
    {
        if (g.ActiveId == id)
            ImGui::ClearActiveID();
    }

    bool value_changed = false;
    int key = *k;

    if (hovered && IsMouseClicked(ImGuiMouseButton_Left))
    {
        if (g.ActiveId != id)
        {
            memset(io.MouseDown, 0, sizeof(io.MouseDown));
            SetActiveID(id, window);
            FocusWindow(window);
        }
    }

    if (IsMouseClicked(ImGuiMouseButton_Left) && g.ActiveId == id && !hovered)
    {
        ClearActiveID();
    }

    static std::map<ImGuiID, int> keybind_frame_count;
    static std::map<ImGuiID, bool> keybind_just_activated;
    
    if (g.ActiveId == id)
    {
        auto it = keybind_frame_count.find(id);
        if (it == keybind_frame_count.end())
        {
            keybind_frame_count[id] = 0;
            keybind_just_activated[id] = true;
            memset(io.MouseDown, 0, sizeof(io.MouseDown));
        }
        
        if (keybind_just_activated[id])
        {
            keybind_frame_count[id]++;
            if (keybind_frame_count[id] > 3)
            {
                keybind_just_activated[id] = false;
            }
        }
        
        if (!keybind_just_activated[id])
        {
            for (int btn = 0; btn < 5; btn++)
        {
            if (ImGui::IsMouseClicked(btn))
            {
                *k = (ImGuiKey)(ImGuiKey_MouseLeft + btn);
                value_changed = true;
                ImGui::ClearActiveID();
                    keybind_frame_count.erase(id);
                    keybind_just_activated.erase(id);
                break;
            }
        }

        if (!value_changed)
        {
            for (int i = ImGuiKey_NamedKey_BEGIN; i < ImGuiKey_NamedKey_END; i++)
            {
                ImGuiKey key_test = (ImGuiKey)i;

                if ((key_test >= ImGuiKey_MouseLeft && key_test <= ImGuiKey_MouseWheelY) || key_test == ImGuiKey_Escape)
                    continue;

                if (IsKeyPressed(key_test))
                {
                    *k = key_test;
                    value_changed = true;
                    ClearActiveID();
                        keybind_frame_count.erase(id);
                        keybind_just_activated.erase(id);
                    break;
                    }
                }
            }
        }

        if (IsKeyPressed(ImGuiKey_Escape))
        {
            ClearActiveID();
            keybind_frame_count.erase(id);
            keybind_just_activated.erase(id);
        }
    }
    else
    {
        keybind_frame_count.erase(id);
        keybind_just_activated.erase(id);
    }

    ImGui::RenderNavHighlight(total_bb, id);

    ImVec2 buf_display_size = ImGui::CalcTextSize(buf_display, NULL, true);
    window->DrawList->AddText(pos + ImVec2(size.x - buf_display_size.x, size.y / 2 - buf_display_size.y / 2), it_anim->second.Label, buf_display);

    return value_changed;
}

bool ImAdd::SliderAlpha(const char* str_id, ImVec4& col)
{
    ImGuiWindow* window = GetCurrentWindow();
    if (window->SkipItems)
        return false;

    ImGuiContext& g = *GImGui;
    const ImGuiStyle& style = g.Style;
    ImGuiIO& io = g.IO;
    const ImGuiID id = window->GetID(str_id);

    float width = CalcItemWidth();
    float height = GetFrameHeight();
    float half_height = ImTrunc(height / 2.0f);

    ImVec2 pos = window->DC.CursorPos;
    const ImVec2 size(width, height);

    const ImRect bb(pos, pos + size);

    ItemSize(bb);
    if (!ItemAdd(bb, id))
        return false;

    bool hovered, held;
    bool pressed = ButtonBehavior(bb, id, &hovered, &held);

    const float slider_min_x = bb.Min.x + half_height;
    const float slider_max_x = bb.Max.x - half_height;

    if (held)
    {
        // Calculate the available slider area
        const float slider_width = slider_max_x - slider_min_x;

        if (slider_width > 0.0f)
        {
            // Clamp mouse position to slider bounds and calculate alpha value
            float mouse_x = ImClamp(io.MousePos.x, slider_min_x, slider_max_x);
            col.w = 1.0f - ((mouse_x - slider_min_x) / slider_width); // Inverted for correct alpha mapping
            col.w = ImClamp(col.w, 0.0f, 1.0f);
        }
    }

    ImColor col_rgb = ImVec4(col.x, col.y, col.z, 1.0f);
    ImColor col_rgb_width_alpha = ImVec4(col.x, col.y, col.z, col.w);

    window->DrawList->AddRectFilled(pos, pos + ImVec2(half_height, height), col_rgb, style.FrameRounding, ImDrawFlags_RoundCornersLeft);
    RenderColorRectWithAlphaCheckerboard(window->DrawList, pos + ImVec2(half_height, 0), pos + size, 0, half_height, ImVec2(0.0f, 0.0f), style.FrameRounding, ImDrawFlags_RoundCornersRight);
    window->DrawList->AddRectFilledMultiColorRounded(pos + ImVec2(half_height, 0), pos + ImVec2(width - half_height, height), col_rgb, col_rgb & ~IM_COL32_A_MASK, col_rgb & ~IM_COL32_A_MASK, col_rgb);

    // Calculate circle position within slider bounds (inverted for correct visual mapping)
    const float circle_radius = ImMax(ImTrunc(g.FontSize / 2.0f) - 2.0f, 1.0f);
    const float slider_width = slider_max_x - slider_min_x;
    float circle_x = slider_min_x + slider_width * (1.0f - col.w); // Inverted for correct visual position

    RenderColorRectWithAlphaCheckerboard(window->DrawList, ImVec2(circle_x - circle_radius, pos.y + half_height - circle_radius), ImVec2(circle_x + circle_radius, pos.y + half_height + circle_radius), col_rgb_width_alpha, circle_radius, ImVec2(0.0f, 0.0f), circle_radius + 1.0f);
    window->DrawList->AddCircle(ImVec2(circle_x, pos.y + half_height), circle_radius + 1.0f, IM_COL32_BLACK, 0, 1.0f);

    return pressed;
}

bool ImAdd::SliderHue(const char* str_id, ImVec4& col)
{
    ImGuiWindow* window = GetCurrentWindow();
    if (window->SkipItems)
        return false;

    ImGuiContext& g = *GImGui;
    const ImGuiStyle& style = g.Style;
    ImGuiIO& io = g.IO;
    const ImGuiID id = window->GetID(str_id);

    float width = CalcItemWidth();
    float height = GetFrameHeight();
    float half_height = ImTrunc(height / 2.0f);

    ImVec2 pos = window->DC.CursorPos;
    const ImVec2 size(width, height);

    const ImRect bb(pos, pos + size);

    ItemSize(bb);
    if (!ItemAdd(bb, id))
        return false;

    bool hovered, held;
    bool pressed = ButtonBehavior(bb, id, &hovered, &held);

    const float slider_min_x = bb.Min.x + half_height;
    const float slider_max_x = bb.Max.x - half_height;

    // Extract HSV values from RGB color
    float h, s, v;
    ImGui::ColorConvertRGBtoHSV(col.x, col.y, col.z, h, s, v);

    if (held)
    {
        // Calculate the available slider area
        const float slider_width = slider_max_x - slider_min_x;

        if (slider_width > 0.0f)
        {
            // Clamp mouse position to slider bounds and calculate hue value
            float mouse_x = ImClamp(io.MousePos.x, slider_min_x, slider_max_x);
            h = (mouse_x - slider_min_x) / slider_width; // Hue ranges from 0.0 to 1.0
            h = ImClamp(h, 0.0f, 1.0f);

            // Convert back to RGB
            ImGui::ColorConvertHSVtoRGB(h, s, v, col.x, col.y, col.z);
        }
    }

    // Draw hue gradient in the middle section
    const ImVec2 gradient_start = pos + ImVec2(half_height, 0);
    const ImVec2 gradient_end = pos + ImVec2(width - half_height, height);

    // Draw hue spectrum gradient
    const int num_segments = 6; // For the 6 main hue segments
    const float segment_width = (width - height) / num_segments;

    for (int i = 0; i < num_segments; i++)
    {
        float hue_start = i / (float)num_segments;
        float hue_end = (i + 1) / (float)num_segments;

        ImVec4 color_start, color_end;
        ImGui::ColorConvertHSVtoRGB(hue_start, 1.0f, 1.0f, color_start.x, color_start.y, color_start.z);
        ImGui::ColorConvertHSVtoRGB(hue_end, 1.0f, 1.0f, color_end.x, color_end.y, color_end.z);
        color_start.w = color_end.w = 1.0f;

        ImVec2 seg_start = gradient_start + ImVec2(segment_width * i, 0);
        ImVec2 seg_end = gradient_start + ImVec2(segment_width * (i + 1), height);

        window->DrawList->AddRectFilledMultiColor(
            seg_start, seg_end,
            ImColor(color_start), ImColor(color_end), ImColor(color_end), ImColor(color_start)
        );
    }

    // Draw static color areas on left and right (repeating the hue spectrum edges)
    ImVec4 left_color, right_color;
    ImGui::ColorConvertHSVtoRGB(0.0f, 1.0f, 1.0f, left_color.x, left_color.y, left_color.z);
    ImGui::ColorConvertHSVtoRGB(1.0f, 1.0f, 1.0f, right_color.x, right_color.y, right_color.z);
    left_color.w = right_color.w = 1.0f;

    window->DrawList->AddRectFilled(pos, pos + ImVec2(half_height, height), ImColor(left_color), style.FrameRounding, ImDrawFlags_RoundCornersLeft);
    window->DrawList->AddRectFilled(pos + ImVec2(width - half_height, 0), pos + size, ImColor(right_color), style.FrameRounding, ImDrawFlags_RoundCornersRight);

    // Calculate circle position within slider bounds
    const float circle_radius = ImMax(ImTrunc(g.FontSize / 2.0f) - 2.0f, 1.0f);
    const float slider_width = slider_max_x - slider_min_x;
    float circle_x = slider_min_x + slider_width * h;

    // Het HUE color
    ImVec4 hue_color;
    ImGui::ColorConvertHSVtoRGB(h, 1.0f, 1.0f, hue_color.x, hue_color.y, hue_color.z); // Full saturation & value
    hue_color.w = 1.0f;

    window->DrawList->AddCircleFilled(ImVec2(circle_x, pos.y + half_height), circle_radius, GetColorU32(hue_color));
    window->DrawList->AddCircle(ImVec2(circle_x, pos.y + half_height), circle_radius + 1.0f, IM_COL32_BLACK, 0, 1.0f);

    return pressed;
}

bool ImAdd::ColorPalette(const char* str_id, ImVec4& col, const ImVec2& size_arg)
{
    ImGuiWindow* window = GetCurrentWindow();
    if (window->SkipItems)
        return false;

    ImGuiContext& g = *GImGui;
    const ImGuiStyle& style = g.Style;
    ImGuiIO& io = g.IO;
    const ImGuiID id = window->GetID(str_id);

    // Use CalcItemWidth for both dimensions if not specified
    ImVec2 pos = window->DC.CursorPos;
    ImVec2 size = CalcItemSize(size_arg, CalcItemWidth(), CalcItemWidth());

    const ImRect bb(pos, pos + size);

    ItemSize(bb);
    if (!ItemAdd(bb, id))
        return false;

    bool hovered, held;
    bool pressed = ButtonBehavior(bb, id, &hovered, &held);

    // Extract HSV values from RGB color
    float h, s, v;
    ImGui::ColorConvertRGBtoHSV(col.x, col.y, col.z, h, s, v);

    if (held || hovered)
    {
        if (held)
        {
            // Calculate saturation and value from mouse position
            s = ImClamp((io.MousePos.x - bb.Min.x) / size.x, 0.0f, 1.0f);
            v = 1.0f - ImClamp((io.MousePos.y - bb.Min.y) / size.y, 0.0f, 1.0f); // Inverted Y axis (0 at bottom, 1 at top)

            // Convert back to RGB
            ImGui::ColorConvertHSVtoRGB(h, s, v, col.x, col.y, col.z);
        }
    }

    // Get the hue color at full saturation and value
    ImVec4 hue_color;
    ImGui::ColorConvertHSVtoRGB(h, 1.0f, 1.0f, hue_color.x, hue_color.y, hue_color.z);
    hue_color.w = 1.0f;
    ImU32 hue_color32 = ImGui::ColorConvertFloat4ToU32(hue_color);

    // Draw the saturation-value gradient using ImGui's method
    // First rectangle: White to hue color (top to bottom)
    window->DrawList->AddRectFilledMultiColorRounded(bb.Min, bb.Max,
        IM_COL32_WHITE, hue_color32, hue_color32, IM_COL32_WHITE,
        style.FrameRounding + (style.FrameRounding > 0 ? 1.5f : 0.0f)
    );

    // Second rectangle: Transparent to black (left to right) overlayed on top
    window->DrawList->AddRectFilledMultiColorRounded(bb.Min, bb.Max,
        IM_COL32(0, 0, 0, 0), IM_COL32(0, 0, 0, 0), IM_COL32_BLACK, IM_COL32_BLACK,
        style.FrameRounding
    );

    if (style.FrameBorderSize > 0)
    {
        //window->DrawList->AddRect(bb.Min, bb.Max, ImGui::GetColorU32(ImGuiCol_Border), style.FrameRounding, 0, style.FrameBorderSize);
    }

    // Draw the selection circle
    float circle_x = bb.Min.x + s * size.x;
    float circle_y = bb.Min.y + (1.0f - v) * size.y; // Inverted Y axis
    const float circle_radius = ImMax(ImTrunc(g.FontSize / 2.0f) - 2.0f, 1.0f);

    ImVec4 col_rgb_without_alpha(col.x, col.y, col.z, 1.0f);

    window->DrawList->AddCircleFilled(ImVec2(circle_x, circle_y), circle_radius, GetColorU32(col_rgb_without_alpha));
    window->DrawList->AddCircle(ImVec2(circle_x, circle_y), circle_radius + 1.0f, IM_COL32_BLACK, 0, 1.0f);
    window->DrawList->AddCircle(ImVec2(circle_x, circle_y), circle_radius, IM_COL32_WHITE, 0, 1.0f);

    return pressed;
}

bool ImAdd::ColorButton(const char* desc_id, const ImVec4& col, bool has_alpha, float square_sz)
{
    ImGuiWindow* window = GetCurrentWindow();
    if (window->SkipItems)
        return false;

    ImGuiContext& g = *GImGui;
    const ImGuiStyle& style = g.Style;
    const ImGuiID id = window->GetID(desc_id);

    ImVec2 pos = window->DC.CursorPos;
    const ImVec2 size(CalcItemSize(ImVec2(square_sz, square_sz), square_sz, square_sz));
    const ImRect bb(pos, pos + size);

    ItemSize(bb);
    if (!ItemAdd(bb, id))
        return false;

    bool hovered, held;
    bool pressed = ButtonBehavior(bb, id, &hovered, &held);

    ImVec4 col_rgb = col;
    ImVec4 col_rgb_without_alpha(col_rgb.x, col_rgb.y, col_rgb.z, 1.0f);

    ImVec4 col_source = has_alpha ? col_rgb : col_rgb_without_alpha;

    if (col_source.w < 1.0f && has_alpha)
        RenderColorRectWithAlphaCheckerboard(window->DrawList, pos, pos + size, GetColorU32(col_source), ImTrunc(square_sz / 2), ImVec2(0, 0), style.FrameRounding);
    else
        window->DrawList->AddRectFilled(pos, pos + size, GetColorU32(col_source), style.FrameRounding);

    if (style.FrameBorderSize > 0)
    {
        window->DrawList->AddRect(pos, pos + size, GetColorU32(ImGuiCol_Border), style.FrameRounding, 0, style.FrameBorderSize);
    }

    RenderNavCursor(bb, id);

    return pressed;
}

void ImAdd::ColorPicker4(const char* label, float col[4], bool has_alpha)
{
    ImGuiWindow* window = GetCurrentWindow();
    if (window->SkipItems)
        return;

    ImGuiContext& g = *GImGui;
    ImGuiStyle& style = g.Style;

    ImVec4 col_v4(col[0], col[1], col[2], has_alpha ? col[3] : 1.0F);

    float width = 200;

    PushID(label);
    BeginGroup();

    std::string str_label = label;
    std::string str_colp_id = str_label + "::color_palette";
    std::string str_hue_id = str_label + "::hue";

    ImAdd::ColorPalette(str_colp_id.c_str(), col_v4, ImVec2(width, ImTrunc(width / 2)));

    PushItemWidth(width);
    PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2());

    ImAdd::SliderHue(str_hue_id.c_str(), col_v4);

    if (has_alpha)
    {
        std::string str_alpha_id = str_label + "::alpha";
        ImAdd::SliderAlpha(str_alpha_id.c_str(), col_v4);

    }

    PopStyleVar();
    PopItemWidth();

    EndGroup();
    PopID();

    col[0] = col_v4.x;
    col[1] = col_v4.y;
    col[2] = col_v4.z;
    col[3] = col_v4.w;
}

bool ImAdd::ColorEdit4(const char* name, float col[4])
{
    ImGuiWindow* window = GetCurrentWindow();
    if (window->SkipItems)
        return false;

    ImGuiContext& g = *GImGui;
    const ImGuiStyle& style = g.Style;
    const ImVec4 col_v4(col[0], col[1], col[2], col[3]);

    ImVec2 pos = window->DC.CursorPos;

    bool result = ImAdd::ColorButton(name, col_v4, true, g.FontSize);

    std::string popup_str_id = std::string(std::string(name) + "::color_edit_4");

    if (result)
    {
        OpenPopup(popup_str_id.c_str());
    }

    PushStyleVar(ImGuiStyleVar_WindowPadding, style.FramePadding);
    PushStyleVar(ImGuiStyleVar_ItemSpacing, style.FramePadding);

    if (BeginPopupEx(GetID(popup_str_id.c_str()), ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoMove))
    {
        SetWindowPos(pos - style.WindowPadding, ImGuiCond_Always);

        ImVec4 col_rgba = ImVec4(0, 0, 0, 0);
        col_rgba.x = col[0];
        col_rgba.y = col[1];
        col_rgba.z = col[2];
        col_rgba.w = col[3];

        ImAdd::ColorButton((popup_str_id + "::color_preview").c_str(), col_rgba, true, GetFontSize());

        static std::map<std::string, char[64]> hex_buffers;
        static std::map<std::string, float[4]> prev_colors;
        char* hex_buf = hex_buffers[popup_str_id];
        float* prev_col = prev_colors[popup_str_id];
        
        char hex_input_id[128];
        sprintf_s(hex_input_id, sizeof(hex_input_id), "##hex_input_%s", popup_str_id.c_str());
        
        if (IsWindowAppearing())
        {
            int i[4] = { IM_F32_TO_INT8_UNBOUND(col[0]), IM_F32_TO_INT8_UNBOUND(col[1]), IM_F32_TO_INT8_UNBOUND(col[2]), IM_F32_TO_INT8_UNBOUND(col[3]) };
            ImFormatString(hex_buf, 64, "#%02X%02X%02X%02X", ImClamp(i[0], 0, 255), ImClamp(i[1], 0, 255), ImClamp(i[2], 0, 255), ImClamp(i[3], 0, 255));
            prev_col[0] = col[0];
            prev_col[1] = col[1];
            prev_col[2] = col[2];
            prev_col[3] = col[3];
        }

        ImGui::SameLine();
        ImGui::Text("%s", name);

        float hex_text_width = CalcTextSize(hex_buf).x;
        float hex_input_width = CalcTextSize(hex_buf).x;
        float available_width = ImGui::GetContentRegionAvail().x;
        ImGui::SameLine(available_width - hex_input_width + style.WindowPadding.x);
        ImGui::SetCursorPosY(style.WindowPadding.y);
        SetNextItemWidth(hex_input_width);
        
        ImGuiID hex_input_item_id = GetID(hex_input_id);
        bool hex_input_was_active = (g.ActiveId == hex_input_item_id);
        
        ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, { 0, 0 });
        ImGui::PushStyleColor(ImGuiCol_FrameBg, ImVec4(0, 0, 0, 0));
        ImGui::PushStyleColor(ImGuiCol_Border, ImVec4(0, 0, 0, 0));
        ImGui::PushStyleVar(ImGuiStyleVar_FrameBorderSize, 0.0f);
        
        if (ImAdd::InputText(nullptr, hex_input_id, hex_buf, 10, ImGuiInputTextFlags_CharsHexadecimal | ImGuiInputTextFlags_CharsUppercase | ImGuiInputTextFlags_ElideLeft))
        {
            unsigned int hex_val = 0;
            const char* hex_str = hex_buf;
            if (*hex_str == '#')
                hex_str++;
            
            if (sscanf_s(hex_str, "%X", &hex_val) == 1)
            {
                size_t len = strlen(hex_str);
                if (len == 8)
                {
                    col[0] = ((hex_val >> 24) & 0xFF) / 255.0f;
                    col[1] = ((hex_val >> 16) & 0xFF) / 255.0f;
                    col[2] = ((hex_val >> 8) & 0xFF) / 255.0f;
                    col[3] = (hex_val & 0xFF) / 255.0f;
                    prev_col[0] = col[0];
                    prev_col[1] = col[1];
                    prev_col[2] = col[2];
                    prev_col[3] = col[3];
                    result = true;
                }
                else if (len == 6)
                {
                    col[0] = ((hex_val >> 16) & 0xFF) / 255.0f;
                    col[1] = ((hex_val >> 8) & 0xFF) / 255.0f;
                    col[2] = (hex_val & 0xFF) / 255.0f;
                    prev_col[0] = col[0];
                    prev_col[1] = col[1];
                    prev_col[2] = col[2];
                    prev_col[3] = col[3];
                    result = true;
                }
            }
        }
        ImGui::PopStyleVar();
        ImGui::PopStyleColor(2);
        ImGui::PopStyleVar();
        
        bool hex_input_is_active = (g.ActiveId == hex_input_item_id);
        
        ImAdd::ColorPicker4(name, col, true);
        
        if (!hex_input_is_active && !IsWindowAppearing())
        {
            bool color_changed = (prev_col[0] != col[0] || prev_col[1] != col[1] || prev_col[2] != col[2] || prev_col[3] != col[3]);
            if (color_changed)
            {
                int i[4] = { IM_F32_TO_INT8_UNBOUND(col[0]), IM_F32_TO_INT8_UNBOUND(col[1]), IM_F32_TO_INT8_UNBOUND(col[2]), IM_F32_TO_INT8_UNBOUND(col[3]) };
                ImFormatString(hex_buf, 64, "#%02X%02X%02X%02X", ImClamp(i[0], 0, 255), ImClamp(i[1], 0, 255), ImClamp(i[2], 0, 255), ImClamp(i[3], 0, 255));
                prev_col[0] = col[0];
                prev_col[1] = col[1];
                prev_col[2] = col[2];
                prev_col[3] = col[3];
            }
        }

        EndPopup();
    }

    PopStyleVar(2);

    return result;
}

bool ImAdd::SliderScalar(const char* label, ImGuiDataType data_type, void* p_data, const void* p_min, const void* p_max, const char* format)
{
    ImGuiWindow* window = GetCurrentWindow();
    if (window->SkipItems)
        return false;

    ImGuiContext& g = *GImGui;
    const ImGuiStyle& style = g.Style;
    const ImGuiID id = window->GetID(label);

    const float width = CalcItemWidth();
    const ImVec2 pos = window->DC.CursorPos;

    const ImVec2 label_size = CalcTextSize(label, NULL, true);
    const bool has_label = label_size.x > 0;
    const float frame_pos_y = has_label ? (g.FontSize + style.ItemInnerSpacing.y) : 0.0f;
    const float frame_height = g.FontSize;

    const ImRect frame_bb(pos + ImVec2(0, frame_pos_y), pos + ImVec2(width, frame_pos_y + frame_height));
    const ImRect total_bb(pos, frame_bb.Max);

    // The slider and the value box drawn at the end are one item to the caller: the layout continues below
    // both, and IsItemHovered() or SetItemTooltip() after the call cover the whole widget
    BeginGroup();

    ItemSize(total_bb);
    if (!ItemAdd(total_bb, id, &frame_bb, 0))
    {
        EndGroup();
        return false;
    }

    if (format == NULL)
        format = DataTypeGetInfo(data_type)->PrintFmt;

    const bool hovered = ItemHoverable(frame_bb, id, g.LastItemData.ItemFlags);
    const bool clicked = hovered && IsMouseClicked(0, ImGuiInputFlags_None, id);
    const bool held = g.ActiveId == id;
    const bool make_active = (clicked || g.NavActivateId == id);

    if (make_active)
    {
        SetActiveID(id, window);
        SetFocusID(id, window);
        FocusWindow(window);
        g.ActiveIdUsingNavDirMask |= (1 << ImGuiDir_Left) | (1 << ImGuiDir_Right);
    }

    // Colors
    ImVec4 colFrame = GetStyleColorVec4((hovered && held) ? ImGuiCol_ButtonActive : hovered ? ImGuiCol_ButtonHovered : ImGuiCol_Button);
    ImVec4 colLine = GetStyleColorVec4(held ? ImGuiCol_SliderGrabActive : ImGuiCol_SliderGrab);

    // Animations
    struct stColors_State {
        ImColor Frame;
        ImColor Line;
    };

    static std::map<ImGuiID, stColors_State> anim;
    auto it_anim = anim.find(id);

    if (it_anim == anim.end())
    {
        anim.insert({ id, stColors_State() });
        it_anim = anim.find(id);

        it_anim->second.Frame = colFrame;
        it_anim->second.Line = colLine;
    }

    it_anim->second.Frame.Value = ImLerp(it_anim->second.Frame.Value, colFrame, 1.0f / IMADD_ANIMATIONS_SPEED * GetIO().DeltaTime);
    it_anim->second.Line.Value = ImLerp(it_anim->second.Line.Value, colLine, 1.0f / IMADD_ANIMATIONS_SPEED * GetIO().DeltaTime);

    // Grab logic
    ImRect grab_bb;
    bool value_changed = SliderBehavior(frame_bb, id, data_type, p_data, p_min, p_max, format, 0, &grab_bb);
    if (value_changed)
        MarkItemEdited(id);

    float relative_value = 0.0f;
    if (data_type == ImGuiDataType_Float)
    {
        float val = *(float*)p_data;
        float min_val = *(float*)p_min;
        float max_val = *(float*)p_max;
        relative_value = (val - min_val) / (max_val - min_val);
    }
    else if (data_type == ImGuiDataType_S32)
    {
        int val = *(int*)p_data;
        int min_val = *(int*)p_min;
        int max_val = *(int*)p_max;
        relative_value = (float)(val - min_val) / (float)(max_val - min_val);
    }

    relative_value = ImClamp(relative_value, 0.0f, 1.0f);

    // Draw frame

    const float pad = ImTrunc(frame_height / 3.0f);

    window->DrawList->AddRectFilled(frame_bb.Min + ImVec2(0, pad + (grab_bb.Min.y - frame_bb.Min.y)), frame_bb.Max - ImVec2(0, pad + (grab_bb.Min.y - frame_bb.Min.y)), it_anim->second.Frame, 1.0f);

    ImVec2 fill_end = ImTrunc(ImVec2(frame_bb.Min.x + relative_value * (frame_bb.GetWidth() - style.GrabMinSize), frame_bb.Max.y));

    ImRect slider_bb = ImRect(ImTrunc(frame_bb.Min + ImVec2(0.0f, pad)), ImTrunc(fill_end - ImVec2(0, pad)));

    if (slider_bb.Max.x > slider_bb.Min.x + style.FrameRounding)
    {
        window->DrawList->AddRectFilled(slider_bb.Min, slider_bb.Max, it_anim->second.Line, style.FrameRounding);
        window->DrawList->AddRectFilledMultiColorRounded(slider_bb.Min, slider_bb.Max, GetColorU32(ImGuiCol_FrameBgShadow, 0.0f), GetColorU32(ImGuiCol_FrameBgShadow), GetColorU32(ImGuiCol_FrameBgShadow), GetColorU32(ImGuiCol_FrameBgShadow, 0.0f), style.FrameRounding);
    }

    ImRect new_grab_bb = ImRect(ImTrunc(ImVec2(fill_end.x, frame_bb.Min.y)), ImTrunc(ImVec2(fill_end.x + style.GrabMinSize, frame_bb.Max.y)));

    window->DrawList->AddRectFilled(new_grab_bb.Min, new_grab_bb.Max, GetColorU32(ImGuiCol_Text), style.GrabRounding);
    window->DrawList->AddRectFilledMultiColorRounded(new_grab_bb.Min, new_grab_bb.Max, GetColorU32(ImGuiCol_FrameBgShadow, 0.0f), GetColorU32(ImGuiCol_FrameBgShadow, 0.0f), GetColorU32(ImGuiCol_FrameBgShadow), GetColorU32(ImGuiCol_FrameBgShadow), style.GrabRounding);

    if (has_label) {
        window->DrawList->AddText(total_bb.Min, GetColorU32(ImGuiCol_Text), label, FindRenderedTextEnd(label));

        // The value is a text box at the right end of the label row: click it and type a number.
        // It is as wide as the longer end of the range, so it keeps its size while the value changes.
        char bound_buf[64];
        DataTypeFormatString(bound_buf, IM_ARRAYSIZE(bound_buf), data_type, p_min, format);
        float value_width = CalcTextSize(bound_buf).x;
        DataTypeFormatString(bound_buf, IM_ARRAYSIZE(bound_buf), data_type, p_max, format);
        value_width = ImMax(value_width, CalcTextSize(bound_buf).x);

        // Less padding than a normal frame, the box has to fit between the rows above and below
        const ImVec2 box_padding(style.FramePadding.x, 2.0f);
        const float box_width = ImTrunc(value_width + box_padding.x * 2.0f + 2.0f);

        window->DC.CursorPos = ImVec2(total_bb.Min.x + width - box_width, total_bb.Min.y - box_padding.y);

        PushID((int)id);
        PushStyleVar(ImGuiStyleVar_FramePadding, box_padding);
        SetNextItemWidth(box_width);
        if (InputScalar("##value", data_type, p_data, NULL, NULL, format, ImGuiInputTextFlags_CharsScientific))
        {
            // Typed numbers stay inside the range the slider can show
            DataTypeClamp(data_type, p_data, p_min, p_max);
            value_changed = true;
        }

        // The box does not keep the keyboard once the number is in. While it is being typed in, the
        // overlay has the keyboard and no key reaches the game or a keybind, and a box that is left
        // without Enter stays that way for as long as nothing else is clicked: a key pressed then does
        // nothing anywhere. So the typing ends by itself at the first letter, which is no part of a
        // number and so was meant for the game, and after a moment without any key.
        if (IsItemActive())
        {
            static ImGuiID typing_id = 0;
            static double last_key_time = 0.0;

            bool any_key = false;
            bool letter = false;
            for (int key = ImGuiKey_NamedKey_BEGIN; key < ImGuiKey_NamedKey_END; key++)
            {
                if (!IsKeyPressed((ImGuiKey)key))
                    continue;

                any_key = true;

                // E is the one letter a number can have in it, and with Ctrl a letter is copy, paste or
                // select all
                if (key >= ImGuiKey_A && key <= ImGuiKey_Z && key != ImGuiKey_E && !g.IO.KeyCtrl)
                    letter = true;
            }

            const ImGuiID box_id = GetItemID();
            if (typing_id != box_id || any_key)
            {
                typing_id = box_id;
                last_key_time = g.Time;
            }

            if (letter || g.Time - last_key_time > 2.5)
            {
                ClearActiveID();
                typing_id = 0;
            }
        }
        PopStyleVar();
        PopID();
    }

    EndGroup();

    return value_changed;
}

bool ImAdd::SliderFloat(const char* label, float* v, float v_min, float v_max, const char* format)
{
    return ImAdd::SliderScalar(label, ImGuiDataType_Float, v, &v_min, &v_max, format);
}

bool ImAdd::SliderInt(const char* label, int* v, int v_min, int v_max, const char* format)
{
    return ImAdd::SliderScalar(label, ImGuiDataType_S32, v, &v_min, &v_max, format);
}

bool ImAdd::BeginChild(const char* label, const ImVec2& size_arg)
{
    ImGuiWindow* window = GetCurrentWindow();
    if (window->SkipItems)
        return false;

    ImGuiContext& g = *GImGui;
    const ImGuiStyle& style = g.Style;

    ImGui::BeginChild(label, size_arg, ImGuiChildFlags_None, ImGuiWindowFlags_NoBackground);

    window = GetCurrentWindow();

	ImVec2 pos = window->Pos;
    ImVec2 size = window->Size;

	window->DrawList->AddRectFilled(pos, pos + ImVec2(size.x, ImGui::GetFrameHeight()), GetColorU32(ImGuiCol_Header), style.ChildRounding, ImDrawFlags_RoundCornersTop);
    if (style.ChildBorderSize > 0) {
        window->DrawList->AddLine(pos + ImVec2(style.ChildBorderSize, style.ChildBorderSize), pos + ImVec2(style.ChildBorderSize, ImGui::GetFrameHeight()), ImGui::GetColorU32(ImGuiCol_SliderGrab), style.ChildBorderSize);
    }
    window->DrawList->AddRectFilledMultiColorRounded(pos, pos + ImVec2(size.x, ImGui::GetFrameHeight()), GetColorU32(ImGuiCol_FrameBgShadow, 0), GetColorU32(ImGuiCol_FrameBgShadow, 0), GetColorU32(ImGuiCol_FrameBgShadow), GetColorU32(ImGuiCol_FrameBgShadow), style.ChildRounding, ImDrawFlags_RoundCornersTop);
	
    window->DrawList->AddRectFilled(pos + ImVec2(0, ImGui::GetFrameHeight()), pos + size, GetColorU32(ImGuiCol_ChildBg), style.ChildRounding, ImDrawFlags_RoundCornersBottom);

    if (style.ChildBorderSize > 0)
    {
        window->DrawList->AddRect(pos, pos + size, GetColorU32(ImGuiCol_Border), style.ChildRounding, 0, style.ChildBorderSize);
        window->DrawList->AddLine(pos + ImVec2(style.ChildBorderSize, ImGui::GetFrameHeight()), pos + ImVec2(size.x - style.ChildBorderSize, ImGui::GetFrameHeight()), ImGui::GetColorU32(ImGuiCol_Border), style.ChildBorderSize);
    }

    window->DrawList->AddText(pos + style.FramePadding + ImVec2(4, 1), IM_COL32_BLACK, label);
    window->DrawList->AddText(pos + style.FramePadding + ImVec2(3, 0), ImGui::GetColorU32(ImGuiCol_Text), label);
    
    SetCursorScreenPos(pos + ImVec2(0, ImGui::GetFrameHeight()));
    ImGui::BeginChild(std::string(std::string(label) + "body").c_str(), ImVec2(ImGui::GetWindowWidth(), ImGui::GetWindowHeight() - ImGui::GetFrameHeight()), ImGuiChildFlags_AlwaysUseWindowPadding, ImGuiWindowFlags_NoBackground);
    PushItemWidth(GetContentRegionAvail().x);

    return true;
}

void ImAdd::EndChild()
{
    PopItemWidth();
    ImGui::EndChild();
    ImGui::EndChild();
}

void ImAdd::RenderArrow(ImDrawList* draw_list, ImVec2 pos, ImU32 col, float sz, ImGuiDir direction)
{
    if (direction < 0 || direction >= ImGuiDir_COUNT) return;

    float thickness = 1.0f;

    float half = ImTrunc(sz / 2.0f);
    float pad_y = ImTrunc(sz / 2.4f);
    float pad_x = ImTrunc(sz / 4.0f);

    if (direction == ImGuiDir_Down)
    {
        draw_list->PathLineTo(pos + ImVec2(pad_x, pad_y));
        draw_list->PathLineTo(pos + ImVec2(half, sz - pad_y));
        draw_list->PathLineTo(pos + ImVec2(sz - pad_x, pad_y));
    }
    else if (direction == ImGuiDir_Up)
    {
        draw_list->PathLineTo(pos + ImVec2(pad_x, sz - pad_y));
        draw_list->PathLineTo(pos + ImVec2(half, pad_y));
        draw_list->PathLineTo(pos + ImVec2(sz - pad_x, sz - pad_y));
    }
    else if (direction == ImGuiDir_Right)
    {
        draw_list->PathLineTo(pos + ImVec2(pad_y, pad_x));
        draw_list->PathLineTo(pos + ImVec2(sz - pad_y, half));
        draw_list->PathLineTo(pos + ImVec2(pad_y, sz - pad_x));
    }
    else if (direction == ImGuiDir_Left)
    {
        draw_list->PathLineTo(pos + ImVec2(sz - pad_y, pad_x));
        draw_list->PathLineTo(pos + ImVec2(pad_y, half));
        draw_list->PathLineTo(pos + ImVec2(sz - pad_y, sz - pad_x));
    }

    draw_list->PathStroke(col, 0, thickness);
}

void ImAdd::RenderCheckMark(ImDrawList* draw_list, ImVec2 pos, ImU32 col, float sz)
{
    //float thickness = ImMax(sz / 5.0f, 1.0f);
    float thickness = 1.5f;

    sz -= thickness * 0.5f;
    pos += ImVec2(thickness * 0.25f, thickness * 0.25f);

    float third = sz / 3.0f;
    float bx = pos.x + third;
    float by = pos.y + sz - third * 0.5f;
    draw_list->PathLineTo(ImVec2(bx - third, by - third));
    draw_list->PathLineTo(ImVec2(bx, by));
    draw_list->PathLineTo(ImVec2(bx + third * 2.0f, by - third * 2.0f));
    draw_list->PathStroke(col, 0, thickness);
}

void ImAdd::RenderSubTabs(const std::vector<const char*>& subtabs, int& current_subtab)
{
    ImGuiStyle& style = GetStyle();
    PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(0, 0));
    PushStyleVar(ImGuiStyleVar_FrameRounding, style.ChildRounding);
    PushStyleColor(ImGuiCol_FrameBgActive, style.Colors[ImGuiCol_ChildBg]);
    
    float subtab_width = (GetContentRegionAvail().x + style.FrameBorderSize * subtabs.size()) / subtabs.size();
    
    for (int i = 0; i < subtabs.size(); i++)
    {
        ImVec2 subtab_size(i == (subtabs.size() - 1) ? GetContentRegionAvail().x : ImTrunc(subtab_width), GetFontSize() + style.FramePadding.y * 2.0f);
        ImDrawFlags draw_flags = i == 0 ? ImDrawFlags_RoundCornersTopLeft : i == (subtabs.size() - 1) ? ImDrawFlags_RoundCornersTopRight : ImDrawFlags_RoundCornersNone;
        
        Tab(subtabs[i], &current_subtab, i, draw_flags, subtab_size);
        if (i < subtabs.size() - 1) SameLine();
    }
    
    PopStyleColor();
    PopStyleVar(2);
}

bool ImAdd::ColorEdit4WithHex(const char* label, float col[4])
{
    return ColorEdit4(label, col);
}