#define IMGUI_DEFINE_MATH_OPERATORS
#include <render/render.h>

#include <dwmapi.h>
#include <cstdio>
#include <chrono>
#include <thread>
#include <map>
#include <vector>
#include <string>
#include <unordered_set>
#include <algorithm>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <iomanip>

#include "assets/font_exo2_medium.h"

#include <globals.h>
#include <logger/logger.h>
#include <render/assets/settings.h>
#include <render/keybind/keybind.h>
#include <render/menu_mouse.h>

#include <game/game.h>
#include <cache/cache.h>

#include <render/config/config.h>
#include <features/spotter/spotter.h>
#include <sdk/sdk.h>
#include <sdk/math/math.h>
#include <memory/memory.h>
#include <Offsets/Offsets.hpp>

extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam);

#define MENU_TITLE      "Glory"
#define BUILD_DATE      __DATE__

// window message handler for the overlay
LRESULT CALLBACK wnd_proc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
    if (ImGui_ImplWin32_WndProcHandler(hwnd, msg, wParam, lParam))
    {
        return true;
    }

    switch (msg)
    {
    case WM_MOUSEACTIVATE:

        return MA_NOACTIVATE;

    case WM_SYSCOMMAND:
        if ((wParam & 0xfff0) == SC_KEYMENU)
        {
            return 0;
        }
        break;

    case WM_SYSKEYDOWN:
        if (wParam == VK_F4) {
            DestroyWindow(hwnd);
            return 0;
        }
        break;

    case WM_DESTROY:
        PostQuitMessage(0);
        break;
    case WM_CLOSE:
        return 0;
    }

    return DefWindowProcA(hwnd, msg, wParam, lParam);
}

render_t::render_t()
{
    detail = std::make_unique<detail_t>();
}

render_t::~render_t()
{
    destroy_imgui();
    destroy_window();
    destroy_device();
}

// make the transparent overlay window
bool render_t::create_window()
{
    detail->window_class.cbSize = sizeof(detail->window_class);
    detail->window_class.style = CS_CLASSDC;
    detail->window_class.lpszClassName = "T4";
    detail->window_class.hInstance = GetModuleHandleA(0);
    detail->window_class.lpfnWndProc = wnd_proc;

    RegisterClassExA(&detail->window_class);

    detail->window = CreateWindowExA(
        WS_EX_TOPMOST | WS_EX_TRANSPARENT | WS_EX_LAYERED | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE,
        detail->window_class.lpszClassName,
        "T4",
        WS_POPUP,
        0,
        0,
        GetSystemMetrics(SM_CXSCREEN),
        GetSystemMetrics(SM_CYSCREEN),
        0,
        0,
        detail->window_class.hInstance,
        0
    );

    if (!detail->window)
    {
        return false;
    }

    SetLayeredWindowAttributes(detail->window, RGB(0, 0, 0), BYTE(255), LWA_ALPHA);

    RECT client_area{};
    RECT window_area{};

    GetClientRect(detail->window, &client_area);
    GetWindowRect(detail->window, &window_area);

    POINT diff{};
    ClientToScreen(detail->window, &diff);

    MARGINS margins
    {
        window_area.left + (diff.x - window_area.left),
        window_area.top + (diff.y - window_area.top),
        window_area.right,
        window_area.bottom,
    };

    DwmExtendFrameIntoClientArea(detail->window, &margins);

    ShowWindow(detail->window, SW_SHOW);
    UpdateWindow(detail->window);

    return true;
}

// set up the d3d11 device and swap chain
bool render_t::create_device()
{
    DXGI_SWAP_CHAIN_DESC swap_chain_desc{};

    swap_chain_desc.BufferCount = 1;

    swap_chain_desc.BufferDesc.Width = 0;
    swap_chain_desc.BufferDesc.Height = 0;
    swap_chain_desc.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;

    swap_chain_desc.OutputWindow = detail->window;

    swap_chain_desc.SwapEffect = DXGI_SWAP_EFFECT_DISCARD;
    swap_chain_desc.Windowed = 1;

    swap_chain_desc.Flags = DXGI_SWAP_CHAIN_FLAG_ALLOW_MODE_SWITCH;

    swap_chain_desc.SampleDesc.Count = 2;
    swap_chain_desc.SampleDesc.Quality = 0;

    swap_chain_desc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;

    D3D_FEATURE_LEVEL feature_level;
    D3D_FEATURE_LEVEL feature_level_list[2] = { D3D_FEATURE_LEVEL_11_0, D3D_FEATURE_LEVEL_10_0 };

    HRESULT result = D3D11CreateDeviceAndSwapChain(
        nullptr,
        D3D_DRIVER_TYPE_HARDWARE,
        nullptr,
        0,
        feature_level_list,
        2,
        D3D11_SDK_VERSION,
        &swap_chain_desc,
        &detail->swap_chain,
        &detail->device,
        &feature_level,
        &detail->device_context
    );

    if (result == DXGI_ERROR_UNSUPPORTED)
    {
        result = D3D11CreateDeviceAndSwapChain(
            nullptr,
            D3D_DRIVER_TYPE_WARP,
            nullptr,
            0,
            feature_level_list,
            2,
            D3D11_SDK_VERSION,
            &swap_chain_desc,
            &detail->swap_chain,
            &detail->device,
            &feature_level,
            &detail->device_context
        );
    }

    if (result != S_OK)
    {
        LOG_ERROR("D3D11CreateDeviceAndSwapChain failed (hresult 0x{:08X})", static_cast<std::uint32_t>(result));
        MessageBoxA(nullptr, "This software can not run on your computer.", "Critical Problem", MB_ICONERROR | MB_OK);

        return false;
    }

    ID3D11Texture2D* back_buffer{ nullptr };
    detail->swap_chain->GetBuffer(0, IID_PPV_ARGS(&back_buffer));

    if (back_buffer)
    {
        detail->device->CreateRenderTargetView(back_buffer, nullptr, &detail->render_target_view);
        back_buffer->Release();

        return true;
    }

    return false;
}

// set up imgui on the overlay
bool render_t::create_imgui()
{
    bool result = true;

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();

    ImGuiStyle& style = ImGui::GetStyle();
    ImGuiIO& io = ImGui::GetIO(); (void)io;

    io.IniFilename = nullptr;   // Disable INI File

    ImGui::StyleColorsDark();

    style.WindowRounding    = 5;
    style.ChildRounding     = 2;
    style.FrameRounding     = 0;
    style.GrabRounding      = 0;
    style.PopupRounding     = 0;

    style.ScrollbarSize     = 7.0f;
    style.GrabMinSize       = 6.0f;

    style.WindowBorderSize  = 1;
    style.FrameBorderSize   = 1;
    style.PopupBorderSize   = 1;

    style.WindowPadding     = ImVec2(10, 10);
    style.ChildPadding      = ImVec2(12, 12);
    style.ItemSpacing       = ImVec2(10, 9);
    style.ItemInnerSpacing  = ImVec2(6, 6);
    style.FramePadding      = ImVec2(5.0f, 5.0f);
    style.CellPadding       = ImVec2(0.0f, 0.0f);

    style.Colors[ImGuiCol_WindowBg]             = ImAdd::HexToColorVec4(0x1e1e1e);
    style.Colors[ImGuiCol_ChildBg]              = ImAdd::HexToColorVec4(0x1e1e1e);
    style.Colors[ImGuiCol_Header]               = ImAdd::HexToColorVec4(0x363636);
    style.Colors[ImGuiCol_PopupBg]              = ImAdd::HexToColorVec4(0x2a2a2a);

    style.Colors[ImGuiCol_CheckMark]            = style.Colors[ImGuiCol_ChildBg];

    style.Colors[ImGuiCol_Text]                 = ImAdd::HexToColorVec4(0xefefef);
    style.Colors[ImGuiCol_TextDisabled]         = ImAdd::HexToColorVec4(0x8e8e8e);

    style.Colors[ImGuiCol_SliderGrab]           = ImAdd::HexToColorVec4(0xc0392b);
    style.Colors[ImGuiCol_SliderGrabActive]     = ImAdd::HexToColorVec4(0x922b21);

    style.Colors[ImGuiCol_SeparatorHovered]     = style.Colors[ImGuiCol_SliderGrab];
    style.Colors[ImGuiCol_SeparatorActive]      = style.Colors[ImGuiCol_SliderGrabActive];

    style.Colors[ImGuiCol_BorderShadow]         = ImAdd::HexToColorVec4(0x000000, 0.00f);
    style.Colors[ImGuiCol_Border]               = ImAdd::HexToColorVec4(0x3a3a3a);
    style.Colors[ImGuiCol_Separator]            = style.Colors[ImGuiCol_Border];

    style.Colors[ImGuiCol_Button]               = ImAdd::HexToColorVec4(0x2a2a2a);
    style.Colors[ImGuiCol_ButtonHovered]        = ImAdd::HexToColorVec4(0x2c2c2c);
    style.Colors[ImGuiCol_ButtonActive]         = ImAdd::HexToColorVec4(0x6e1010);
    style.Colors[ImGuiCol_ButtonShadow]         = ImAdd::HexToColorVec4(0x000000, 0.2f);

    style.Colors[ImGuiCol_FrameBg]              = style.Colors[ImGuiCol_Button];
    style.Colors[ImGuiCol_FrameBgHovered]       = style.Colors[ImGuiCol_ButtonHovered];
    style.Colors[ImGuiCol_FrameBgActive]        = style.Colors[ImGuiCol_ButtonActive];
    style.Colors[ImGuiCol_FrameBgShadow]        = ImAdd::HexToColorVec4(0x000000, 0.3f);

    style.Colors[ImGuiCol_ScrollbarBg]          = style.Colors[ImGuiCol_Border];
    style.Colors[ImGuiCol_ScrollbarGrab]        = ImAdd::HexToColorVec4(0x414141);
    style.Colors[ImGuiCol_ScrollbarGrabHovered] = style.Colors[ImGuiCol_ScrollbarGrab];
    style.Colors[ImGuiCol_ScrollbarGrabActive]  = style.Colors[ImGuiCol_ScrollbarGrab];

    style.Colors[ImGuiCol_ResizeGrip]           = ImVec4();
    style.Colors[ImGuiCol_ResizeGripHovered]    = ImVec4();
    style.Colors[ImGuiCol_ResizeGripActive]     = ImVec4();
    style.Colors[ImGuiCol_SeparatorActive]      = ImVec4();
    style.Colors[ImGuiCol_SeparatorHovered]     = ImVec4();

    // Setup Font
    ImFontConfig font_cfg_main;
    font_cfg_main.FontLoaderFlags = ImGuiFreeTypeLoaderFlags_ForceAutoHint;
    font_cfg_main.GlyphOffset = ImVec2(0, -1);
    font_cfg_main.SizePixels = 14.0f;
    io.Fonts->AddFontFromMemoryCompressedTTF(font_exo2_medium_compressed_data, font_exo2_medium_compressed_size, font_cfg_main.SizePixels, &font_cfg_main, io.Fonts->GetGlyphRangesDefault());

    result = ImGui_ImplWin32_Init(detail->window);
    if (!result) return false;

    if (!detail->device || !detail->device_context)
    {
        return false;
    }

    result = ImGui_ImplDX11_Init(detail->device, detail->device_context);
    if (!result) return false;

    return true;
}

// tear down the d3d11 device
void render_t::destroy_device()
{
	if (detail->render_target_view) detail->render_target_view->Release();
	if (detail->swap_chain) detail->swap_chain->Release();
	if (detail->device_context) detail->device_context->Release();
	if (detail->device) detail->device->Release();
}

// destroy the overlay window
void render_t::destroy_window()
{
    DestroyWindow(detail->window);
    UnregisterClassA(detail->window_class.lpszClassName, detail->window_class.hInstance);
}

// shut down imgui
void render_t::destroy_imgui()
{
    ImGui_ImplDX11_Shutdown();
    ImGui_ImplWin32_Shutdown();
    ImGui::DestroyContext();
}

#ifndef WDA_EXCLUDEFROMCAPTURE
#define WDA_EXCLUDEFROMCAPTURE 0x00000011
#endif

// hide the overlay from screen capture
void render_t::update_capture_exclusion()
{
    if (detail->window == nullptr)
    {
        return;
    }

    const DWORD wanted = settings::overlay::hide_from_capture ? WDA_EXCLUDEFROMCAPTURE : WDA_NONE;

    if (wanted == rejected_affinity)
    {
        return;
    }
    rejected_affinity = 0xFFFFFFFF;

    DWORD current = WDA_NONE;
    if (GetWindowDisplayAffinity(detail->window, &current) && current == wanted)
    {
        return;
    }

    if (SetWindowDisplayAffinity(detail->window, wanted))
    {
        LOG_INFO("overlay {} screen capture", wanted == WDA_NONE ? "visible to" : "hidden from");
        return;
    }

    rejected_affinity = wanted;
    LOG_WARN("could not change the overlay's capture visibility (error {}), needs Windows 10 version 2004 or later", GetLastError());
}

// read which mouse buttons are down
static int read_mouse_buttons()
{
    static const int keys[menu_mouse::button_count] = { VK_LBUTTON, VK_RBUTTON, VK_MBUTTON, VK_XBUTTON1, VK_XBUTTON2 };

    const bool swapped = GetSystemMetrics(SM_SWAPBUTTON) != 0;

    int down = 0;
    for (int button = 0; button < menu_mouse::button_count; button++)
    {
        const int key = (swapped && button < 2) ? keys[1 - button] : keys[button];
        if ((GetAsyncKeyState(key) & 0x8000) != 0)
        {
            down |= 1 << button;
        }
    }

    return down;
}

// begin a frame
void render_t::start_render()
{
    update_capture_exclusion();

    MSG msg;
    while (PeekMessage(&msg, nullptr, 0, 0, PM_REMOVE))
    {

        if (msg.message == WM_QUIT)
        {
            LOG_INFO("overlay window closed, exiting");
            instant_terminate(0);
        }

        TranslateMessage(&msg);
        DispatchMessage(&msg);
    }

    const menu_mouse::frame_t mouse = menu_mouse::begin_frame(read_mouse_buttons(), menu_has_mouse);

    if (running)
    {
        POINT cursor{};
        if (menu_mouse::game_holds_a_button())
        {
            ImGui::GetIO().AddMousePosEvent(-FLT_MAX, -FLT_MAX);
        }
        else if (GetCursorPos(&cursor) && ScreenToClient(detail->window, &cursor))
        {
            ImGui::GetIO().AddMousePosEvent(static_cast<float>(cursor.x), static_cast<float>(cursor.y));
        }
    }

    if (mouse.let_go != 0)
    {
        LOG_INFO("a mouse button came up where the overlay could not see it (buttons 0x{:X}), the menu has let go of it", mouse.let_go);
    }

    if (mouse.idle && GetCapture() == detail->window)
    {
        ReleaseCapture();
        LOG_INFO("the overlay was still holding the mouse with every button up, let go");
    }

    ImGui_ImplDX11_NewFrame();
    ImGui_ImplWin32_NewFrame();
    ImGui::NewFrame();
}

// present the frame
void render_t::end_render()
{
    ImGui::Render();

    float clear_color[4]{ 0, 0, 0, 0 };
    detail->device_context->OMSetRenderTargets(1, &detail->render_target_view, nullptr);
    detail->device_context->ClearRenderTargetView(detail->render_target_view, clear_color);

    ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());

    detail->swap_chain->Present(0, 0);
}

// toggle clicks passing through the overlay
void render_t::set_click_through(bool enabled)
{

    LONG_PTR ex_style = GetWindowLongPtr(detail->window, GWL_EXSTYLE);
    const bool is_click_through = (ex_style & WS_EX_TRANSPARENT) != 0;

    if (is_click_through != click_through)
    {
        LOG_EVERY(5000, logger::level::warn, "the overlay window was {} without this program having asked for it", is_click_through ? "letting clicks through" : "taking clicks");
    }
    click_through = enabled;

    if (is_click_through == enabled)
    {
        return;
    }

    ex_style = enabled ? (ex_style | WS_EX_TRANSPARENT) : (ex_style & ~static_cast<LONG_PTR>(WS_EX_TRANSPARENT));
    SetWindowLongPtr(detail->window, GWL_EXSTYLE, ex_style);

    SetWindowPos(detail->window, nullptr, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE | SWP_FRAMECHANGED);
}

// route input between the menu and the game
void render_t::update_input_passthrough(bool menu_drawn)
{

    const bool takes_mouse = menu_mouse::end_frame(menu_drawn);
    set_click_through(!takes_mouse);
    menu_has_mouse = takes_mouse;

    const bool item_active = menu_drawn && ImGui::GetActiveID() != 0;
    const bool item_waits_for_keys = item_active && !ImGui::IsAnyMouseDown();
    idle_active_frames = item_waits_for_keys ? ImMin(idle_active_frames + 1, 2) : 0;

    const bool wants_keyboard = keyboard_grabbed ? item_active : idle_active_frames >= 2;
    const bool has_keyboard = GetForegroundWindow() == detail->window;

    if (!wants_keyboard)
    {

        if (has_keyboard)
        {
            HWND roblox_window = game::get_roblox_window();
            if (roblox_window != nullptr && !SetForegroundWindow(roblox_window))
            {
                LOG_EVERY(5000, logger::level::warn, "could not give the keyboard back to the game (error {})", GetLastError());
            }
        }

        keyboard_grabbed = false;
        return;
    }

    if (has_keyboard)
    {
        keyboard_grabbed = true;
        return;
    }

    if (keyboard_grabbed)
    {

        keyboard_grabbed = false;
        ImGui::ClearActiveID();
        return;
    }

    static ULONGLONG last_attempt = 0;
    const ULONGLONG now = GetTickCount64();
    if (now - last_attempt >= 250)
    {
        last_attempt = now;
        if (!SetForegroundWindow(detail->window))
        {
            LOG_DEBUG("could not give the menu keyboard focus");
        }
    }
}

// tooltip when hovering an item
static void hint(const char* text)
{
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_Stationary | ImGuiHoveredFlags_DelayNormal))
    {
        ImGui::SetTooltip("%s", text);
    }
}

// keep a window inside the screen
static ImVec2 keep_inside(const ImVec2& pos, const ImVec2& size, const ImRect& rect)
{
    return ImMax(rect.Min, ImMin(pos, rect.Max - size));
}

// draw the whole menu
void render_t::render_menu()
{
    if (!running)
    {
        return;
    }

    ImGuiStyle& style = ImGui::GetStyle();
    ImGuiIO& io = ImGui::GetIO(); (void)io;

    static std::vector<const char*> m_Tabs = { "Aimbot", "Visual", "Settings" };
    static int m_iCurrentPage = 0;

    const char* const menu_name = "External";

    const ImRect screen(ImVec2(0.0f, 0.0f), io.DisplaySize);
    const bool has_game_view = game::window_width > 0.0f && game::window_height > 0.0f;
    const ImRect game_view = has_game_view
        ? ImRect(game::window_offset_x, game::window_offset_y, game::window_offset_x + game::window_width, game::window_offset_y + game::window_height)
        : screen;

    ImGui::SetNextWindowSize(ImVec2(570, 510), ImGuiCond_Once);
    ImGui::SetNextWindowPos(game_view.GetCenter(), ImGuiCond_Once, ImVec2(0.5f, 0.5f));

    if (const ImGuiWindow* menu = ImGui::FindWindowByName(menu_name))
    {
        const ImVec2 inside = keep_inside(keep_inside(menu->Pos, menu->Size, game_view), menu->Size, screen);
        if (inside.x != menu->Pos.x || inside.y != menu->Pos.y)
        {
            ImGui::SetNextWindowPos(inside);
        }
    }

    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0);
    bool main_window = ImGui::Begin(menu_name, (bool*)0, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoBackground);
    ImGui::PopStyleVar(2);

    if (main_window)
    {
        float header_height = ImGui::GetFontSize() + style.WindowPadding.y * 2.0f;
        float menubar_height = ImGui::GetFontSize() + style.WindowPadding.y * 2.0f;

        ImRect body_bb;
        ImRect content_bb;
        ImRect menubar_bb;

        if (ImGui::GetCurrentWindow()->Flags & ImGuiWindowFlags_NoBackground)
        {
            ImVec2 pos = ImGui::GetWindowPos();
            ImVec2 size = ImGui::GetWindowSize();
            ImDrawList* pDrawList = ImGui::GetWindowDrawList();

            body_bb = ImRect(pos, pos + size);
            menubar_bb = ImRect(pos + ImVec2(style.WindowPadding.x, header_height), pos + ImVec2(size.x - style.WindowPadding.x, header_height + menubar_height));
            content_bb = ImRect(pos + ImVec2(style.WindowPadding.x, header_height + menubar_height), pos + size - style.WindowPadding);

            float roudning = style.WindowRounding;
            float rounding_ex = style.WindowRounding + (style.WindowRounding > 0 ? style.WindowBorderSize : 0.0f);

            pDrawList->AddRectFilled(body_bb.Min, body_bb.Max, ImGui::GetColorU32(ImGuiCol_WindowBg), rounding_ex);
            pDrawList->AddRectFilledMultiColorRounded(body_bb.Min, ImVec2(body_bb.Max.x, body_bb.Min.y + header_height), ImGui::GetColorU32(ImGuiCol_ButtonActive), ImGui::GetColorU32(ImGuiCol_ButtonActive), ImGui::GetColorU32(ImGuiCol_ButtonActive, 0), ImGui::GetColorU32(ImGuiCol_ButtonActive, 0), rounding_ex, ImDrawFlags_RoundCornersTop);
            pDrawList->AddRectFilled(menubar_bb.Min, content_bb.Max, ImGui::GetColorU32(ImGuiCol_ChildBg), style.ChildRounding);

            if (style.WindowBorderSize > 0.0f)
            {
                ImU32 black_col = IM_COL32(0, 0, 0, 255);
                pDrawList->AddRect(body_bb.Min, body_bb.Max, black_col, rounding_ex, 0, style.WindowBorderSize);
                pDrawList->AddRect(body_bb.Min + ImVec2(style.WindowBorderSize, style.WindowBorderSize), body_bb.Max - ImVec2(style.WindowBorderSize, style.WindowBorderSize), ImGui::GetColorU32(ImGuiCol_SliderGrabActive), roudning, 0, style.WindowBorderSize);
                pDrawList->AddRect(menubar_bb.Min, content_bb.Max, ImGui::GetColorU32(ImGuiCol_Border), style.ChildRounding, 0, style.WindowBorderSize);
            }

            pDrawList->AddText(pos + style.WindowPadding, ImGui::GetColorU32(ImGuiCol_SliderGrab), MENU_TITLE);
            pDrawList->AddText(pos + style.WindowPadding + ImVec2(ImGui::CalcTextSize(MENU_TITLE).x, 0.0f), ImGui::GetColorU32(ImGuiCol_Text), " - " BUILD_DATE);
        }

        ImGui::SetCursorScreenPos(menubar_bb.Min);
        if (ImGui::BeginChild("menubar", menubar_bb.GetSize(), ImGuiActivateFlags_None, ImGuiWindowFlags_NoBackground))
        {
            ImGui::PushStyleColor(ImGuiCol_FrameBgActive, style.Colors[ImGuiCol_ChildBg]);
            ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(-style.FrameBorderSize, 0.0f));
            ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, style.ChildRounding);

            for (int i = 0; i < m_Tabs.size(); i++)
            {
                ImVec2 tab_size(i == (m_Tabs.size() - 1) ? ImGui::GetContentRegionAvail().x : ImTrunc((ImGui::GetWindowWidth() + style.FrameBorderSize * m_Tabs.size()) / m_Tabs.size()), ImGui::GetWindowHeight());
                ImDrawFlags draw_flags = i == 0 ? ImDrawFlags_RoundCornersTopLeft : i == (m_Tabs.size() - 1) ? ImDrawFlags_RoundCornersTopRight : ImDrawFlags_RoundCornersNone;

                ImAdd::Tab(m_Tabs[i], &m_iCurrentPage, i, draw_flags, tab_size);
                if (i < m_Tabs.size() - 1) ImGui::SameLine();
            }

            ImGui::PopStyleVar(2);
            ImGui::PopStyleColor();

            ImGui::EndChild();
        }

        ImGui::SetCursorScreenPos(content_bb.Min);
        if (ImGui::BeginChild("content", content_bb.GetSize(), ImGuiChildFlags_AlwaysUseWindowPadding, ImGuiWindowFlags_NoBackground))
        {
            float group_width = ImTrunc((ImGui::GetContentRegionAvail().x - style.ItemSpacing.x) / 2.0f);

            switch (m_iCurrentPage)
            {
            case 0:
            {
                if (ImAdd::BeginChild("Aimbot", ImVec2(ImGui::GetContentRegionAvail().x, 0.0f)))
                {
                    ImAdd::CheckBox("Enable", &settings::targeting::enabled);

                    float available_width = ImGui::GetContentRegionAvail().x - style.WindowPadding.x * 2.0f;
                    float max_keybind_width = ImMin(available_width * 0.4f, 120.0f);

                    ImGui::SameLine(ImGui::GetContentRegionAvail().x + style.WindowPadding.x - max_keybind_width);
                    keybind::activation_mode& targeting_mode = reinterpret_cast<keybind::activation_mode&>(settings::targeting::activation_mode);
                    keybind::keybind_with_mode("Aimbot key", settings::targeting::keybind, targeting_mode, "aimbot", max_keybind_width);
                    settings::targeting::activation_mode = static_cast<int>(targeting_mode);

                    static std::vector<const char*> parts = { "Closest", "Head", "Torso" };
                    ImAdd::Combo("Hitbone", &settings::targeting::target_part, parts);

                    ImAdd::SeparatorText("FOV");

                    ImAdd::SliderFloat("Size", &settings::targeting::fov, 0.f, 1000.f, "%.0f");
                    ImAdd::CheckBox("Draw", &settings::targeting::draw_fov);
                    float fov_color_width = ImGui::GetFontSize() * 2.0f + style.ItemSpacing.x;
                    ImGui::SameLine(ImGui::GetContentRegionAvail().x + style.WindowPadding.x - fov_color_width);
                    ImAdd::ColorEdit4WithHex("Fill", settings::targeting::fov_circle_colour);
                    ImGui::SameLine();
                    ImAdd::ColorEdit4WithHex("Outline", settings::targeting::fov_outline_colour);

                    ImAdd::SeparatorText("Lock");

                    ImAdd::SliderFloat("Sensitivity", &settings::targeting::sensitivity, 1.f, 100.f, "%.0f");
                    hint("How fast the lock turns onto the target, 100 snaps at once.");

                    ImAdd::EndChild();
                }
                break;
            }
            case 1:
            {
                if (ImAdd::BeginChild("ESP", ImVec2(ImGui::GetContentRegionAvail().x, 0.0f)))
                {
                    ImAdd::CheckBox("Enable", &settings::visuals::enabled);

                    ImAdd::CheckBox("Box", &settings::visuals::box);
                    ImGui::SameLine(ImGui::GetContentRegionAvail().x - ImGui::GetFontSize() * 1.5f);
                    ImAdd::ColorEdit4WithHex("BoxColor", settings::visuals::box_color);

                    ImAdd::CheckBox("Name", &settings::visuals::name);
                    ImGui::SameLine(ImGui::GetContentRegionAvail().x - ImGui::GetFontSize() * 1.5f);
                    ImAdd::ColorEdit4WithHex("NameColor", settings::visuals::name_color);

                    ImAdd::CheckBox("Healthbar", &settings::visuals::healthbar);
                    ImGui::SameLine(ImGui::GetContentRegionAvail().x - ImGui::GetFontSize() * 1.5f);
                    ImAdd::ColorEdit4WithHex("HealthbarColor", settings::visuals::healthbar_color);

                    ImAdd::EndChild();
                }
                break;
            }
            case 2:
            {
                if (ImAdd::BeginChild("Configs", ImVec2(group_width, 0.0f)))
                {
                    static char config_name[256] = "";
                    static std::string selected_config;

                    ImGui::InputTextWithHint("##config_name", "Config name", config_name, sizeof(config_name));

                    const ImVec2 action_size(ImTrunc((ImGui::GetContentRegionAvail().x - style.ItemSpacing.x * 2.0f) / 3.0f), 0.0f);

                    if (ImAdd::Button("Save", action_size))
                    {
                        if (strlen(config_name) > 0)
                        {
                            config::save_config(config_name);
                        }
                    }

                    ImGui::SameLine();
                    if (ImAdd::Button("Load", action_size))
                    {
                        if (strlen(config_name) > 0)
                        {
                            config::load_config(config_name);
                        }
                    }

                    static std::string status_message;
                    static float status_time = 0;

                    ImGui::SameLine();
                    if (ImAdd::Button("Delete", ImVec2(ImGui::GetContentRegionAvail().x, 0)))
                    {
                        if (strlen(config_name) > 0)
                        {
                            std::string path = config::get_config_directory() + config_name + ".cfg";
                            std::error_code remove_error;
                            if (std::filesystem::remove(path, remove_error))
                            {
                                status_message = "Config deleted: " + std::string(config_name);
                                status_time = ImGui::GetTime();
                            }
                            else
                            {
                                status_message = "Failed to delete: " + std::string(config_name);
                                status_time = ImGui::GetTime();
                            }
                        }
                    }

                    // Show status message
                    if (ImGui::GetTime() - status_time < 3.0f && !status_message.empty())
                    {
                        ImGui::TextColored(ImVec4(0, 1, 0, 1), "%s", status_message.c_str());
                    }

                    ImAdd::SeparatorText("Saved");

                    std::vector<std::string> configs = config::get_config_list();
                    if (configs.empty())
                    {
                        ImGui::TextDisabled("None yet");
                    }
                    else
                    {
                        for (const auto& cfg : configs)
                        {
                            bool is_selected = (selected_config == cfg);
                            if (ImGui::Selectable(cfg.c_str(), is_selected))
                            {
                                selected_config = cfg;
                                size_t len = cfg.length();
                                if (len > 255) len = 255;
                                memcpy(config_name, cfg.c_str(), len);
                                config_name[len] = '\0';
                            }
                        }
                    }

                    ImAdd::EndChild();
                }

                ImGui::SameLine();

                if (ImAdd::BeginChild("General", ImVec2(group_width, 0.0f)))
                {
                    ImAdd::CheckBox("Hide from OBS and clips", &settings::overlay::hide_from_capture);
                    hint("The ESP and this menu stay on your monitor but are left out of OBS,\nclipping software and screenshots. Needs Windows 10 version 2004 or later.");

                    if (ImAdd::Button("Unload", ImVec2(ImGui::GetContentRegionAvail().x, 0)))
                    {
                        LOG_INFO("unload requested from the menu");
                        instant_terminate(0);
                    }

                    ImAdd::EndChild();
                }
                break;
            }
            default:
                break;
            }
            ImGui::EndChild();
        }

        ImGui::End();
    }
}

// draw the aimbot fov circle
static void draw_fov_circle(float fov_radius, float circle_colour[4], float outline_colour[4])
{
	ImVec2 center(ImGui::GetIO().DisplaySize.x * 0.5f, ImGui::GetIO().DisplaySize.y * 0.5f);

	if (game::visengine.address != 0)
	{
		math::vector2 dims = game::visengine.get_dimensions();
		if (dims.x > 0.0f && dims.y > 0.0f)
		{
			center = ImVec2(dims.x * 0.5f + game::window_offset_x, dims.y * 0.5f + game::window_offset_y);
		}
	}

	POINT cursor{};
	if (GetCursorPos(&cursor))
	{
		center = ImVec2(static_cast<float>(cursor.x), static_cast<float>(cursor.y));
	}

	ImDrawList* draw = ImGui::GetBackgroundDrawList();

	ImU32 outline_col = IM_COL32(
		static_cast<int>(outline_colour[0] * 255.f),
		static_cast<int>(outline_colour[1] * 255.f),
		static_cast<int>(outline_colour[2] * 255.f),
		static_cast<int>(outline_colour[3] * 255.f)
	);

	ImU32 circle_col = IM_COL32(
		static_cast<int>(circle_colour[0] * 255.f),
		static_cast<int>(circle_colour[1] * 255.f),
		static_cast<int>(circle_colour[2] * 255.f),
		static_cast<int>(circle_colour[3] * 255.f)
	);

	draw->AddCircle(center, fov_radius, outline_col, 0, 3.f);
	draw->AddCircle(center, fov_radius, circle_col, 0, 1.f);
}

// draw the on-screen visuals
void render_t::render_visuals()
{

	game::update_window_offset();

	static ULONGLONG last_cover_check = 0;
	const ULONGLONG tick = GetTickCount64();
	if (tick - last_cover_check >= 100)
	{
		last_cover_check = tick;
		game::update_covering_windows(detail->window);
	}

	// Run ESP rendering
	spotter::run();

	if (settings::targeting::draw_fov && settings::targeting::enabled)
	{
		draw_fov_circle(settings::targeting::fov, settings::targeting::fov_circle_colour, settings::targeting::fov_outline_colour);
	}
}
