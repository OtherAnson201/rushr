#include "imgui_notification.h"

void ImNotify::Print(NotifyLevel type/*, ImTextureID icon*/, const char* text, ...)
{
    mNotifications.emplace_back(text);
    //mNotificationsIcon.push_back(icon);
    mNotificationsType.push_back(type);
    mNotificationTimers.push_back(fWaitTime);
}

// Helper
ImVec4 HexToColorVec4(unsigned int hex_color, float alpha)
{
    ImVec4 color;

    color.x = ((hex_color >> 16) & 0xFF) / 255.0f;
    color.y = ((hex_color >> 8) & 0xFF) / 255.0f;
    color.z = (hex_color & 0xFF) / 255.0f;
    color.w = alpha;

    return color;
}

void ImNotify::Update()
{
    if (!mNotifications.empty())
    {
        for (size_t i = 0; i < mNotificationTimers.size(); i++)
        {
            mNotificationTimers[i] -= ImGui::GetIO().DeltaTime;
            if (mNotificationTimers[i] <= 0.0f)
            {
                mNotifications.erase(mNotifications.begin() + i);
                //mNotificationsIcon.erase(mNotificationsIcon.begin() + i);
                mNotificationsType.erase(mNotificationsType.begin() + i);
                mNotificationTimers.erase(mNotificationTimers.begin() + i);
                i--;
            }
        }
    }
}

void ImNotify::Render()
{
    ImGuiContext& g = *GImGui;
    const ImGuiStyle& style = g.Style;
    const ImGuiIO& io = g.IO;

    //float windowHeight = (ImGui::GetFontSize() * 2) + (style.WindowPadding.y * 3);
    float windowHeight = ImGui::GetFontSize() + style.WindowPadding.y * 2.0f;
    float spacing = style.DisplayWindowPadding.y;
    float windowPosY = style.DisplayWindowPadding.y;

    for (int i = mNotifications.size() - 1; i >= 0; i--)
    {
        const auto& notification = mNotifications[i];
        NotifyLevel type = mNotificationsType[i];
        //ImTextureID icon = mNotificationsIcon[i];
        ImVec4 mainColor;

        switch (type)
        {
        case (NotifyLevel::Info):
        {
            mainColor = HexToColorVec4(0x40A2ED, 1.f);
            break;
        }
        case (NotifyLevel::Success):
        {
            mainColor = HexToColorVec4(0x15961e, 1.f);
            break;
        }
        case (NotifyLevel::Warn):
        {
            mainColor = HexToColorVec4(0xED9B40, 1.f);
            break;
        }
        case (NotifyLevel::Error):
        {
            mainColor = HexToColorVec4(0xD64550, 1.f);
            break;
        }
        default:
        {
            mainColor = HexToColorVec4(0xd4d4d4, 1.f);
            break;
        }
        }

        ImVec2 notification_size = ImVec2(ImGui::CalcTextSize(notification.c_str()).x + style.WindowPadding.x * 2, windowHeight);

        ImGui::SetNextWindowPos(ImVec2(spacing, io.DisplaySize.y - windowPosY - notification_size.y));
        ImGui::SetNextWindowSize(notification_size);
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2());
        ImGui::Begin(("Notification" + std::to_string(i)).c_str(), nullptr, ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoBackground);
        ImGui::PopStyleVar();
        auto pos = ImGui::GetWindowPos();
        auto size = ImGui::GetWindowSize();
        {
            float Delta = (mNotificationTimers[i] / fWaitTime);

            ImDrawList* pDrawList = ImGui::GetForegroundDrawList();

            pDrawList->AddRectFilled(pos, pos + size, ImGui::GetColorU32(ImGuiCol_PopupBg), style.WindowRounding);
            pDrawList->AddRectFilled(pos, pos + ImVec2(int(size.x * (1 - Delta)), size.y), ImGui::GetColorU32(ImGui::GetColorU32(mainColor), 0.3f), style.WindowRounding);

            if (style.WindowBorderSize > 0)
            {
                pDrawList->AddRect(pos, pos + size, ImGui::GetColorU32(ImGuiCol_Border), style.WindowRounding);
            }

            pDrawList->AddText(pos + ImVec2(style.WindowPadding.x, size.y / 2 - ImGui::GetFontSize() / 2), ImGui::GetColorU32(mainColor), notification.c_str());
        }
        ImGui::End();

        windowPosY += windowHeight + spacing;
    }
}

void ImNotify::RenderAtPosition(ImVec2 base_pos, float width, bool align_center)
{
    ImGuiContext& g = *GImGui;
    const ImGuiStyle& style = g.Style;
    const ImGuiIO& io = g.IO;

    float windowHeight = ImGui::GetFontSize() + style.WindowPadding.y * 2.0f;
    float spacing = 8.0f;
    float current_y = base_pos.y;

    for (int i = mNotifications.size() - 1; i >= 0; i--)
    {
        const auto& notification = mNotifications[i];
        NotifyLevel type = mNotificationsType[i];
        ImVec4 mainColor;

        switch (type)
        {
        case (NotifyLevel::Info):
        {
            mainColor = HexToColorVec4(0x40A2ED, 1.f);
            break;
        }
        case (NotifyLevel::Success):
        {
            mainColor = HexToColorVec4(0x15961e, 1.f);
            break;
        }
        case (NotifyLevel::Warn):
        {
            mainColor = HexToColorVec4(0xED9B40, 1.f);
            break;
        }
        case (NotifyLevel::Error):
        {
            mainColor = HexToColorVec4(0xD64550, 1.f);
            break;
        }
        default:
        {
            mainColor = HexToColorVec4(0xd4d4d4, 1.f);
            break;
        }
        }

        ImVec2 notification_size = ImVec2(ImGui::CalcTextSize(notification.c_str()).x + style.WindowPadding.x * 2, windowHeight);
        float notification_x = align_center ? base_pos.x + (width - notification_size.x) * 0.5f : base_pos.x;

        ImVec2 pos = ImVec2(notification_x, current_y);
        ImVec2 size = notification_size;

        float Delta = (mNotificationTimers[i] / fWaitTime);
        float fade_in_duration = 0.2f;
        float fade_out_duration = 0.3f;
        float fade_alpha = 1.0f;
        
        float elapsed = fWaitTime - mNotificationTimers[i];
        if (elapsed < fade_in_duration) {
            fade_alpha = elapsed / fade_in_duration;
        } else if (mNotificationTimers[i] < fade_out_duration) {
            fade_alpha = mNotificationTimers[i] / fade_out_duration;
        }

        ImDrawList* pDrawList = ImGui::GetForegroundDrawList();

        ImVec4 bg_color = mainColor;
        bg_color.w = 0.3f * fade_alpha;

        pDrawList->AddRectFilled(pos, pos + size, ImGui::ColorConvertFloat4ToU32(ImVec4(0.12f, 0.12f, 0.12f, fade_alpha * 0.9f)), style.WindowRounding);
        pDrawList->AddRectFilled(pos, pos + ImVec2(size.x * (1 - Delta), size.y), ImGui::ColorConvertFloat4ToU32(bg_color), style.WindowRounding);

        if (style.WindowBorderSize > 0)
        {
            ImVec4 border_color = mainColor;
            border_color.w = fade_alpha;
            pDrawList->AddRect(pos, pos + size, ImGui::ColorConvertFloat4ToU32(border_color), style.WindowRounding);
        }

        ImVec4 text_color = mainColor;
        text_color.w = fade_alpha;
        pDrawList->AddText(pos + ImVec2(style.WindowPadding.x, size.y / 2 - ImGui::GetFontSize() / 2), ImGui::ColorConvertFloat4ToU32(text_color), notification.c_str());

        current_y += windowHeight + spacing;
    }
}