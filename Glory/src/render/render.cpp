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
#include <features/mover/mover.h>
#include <features/stabilizer/stabilizer.h>
#include <features/netblock/netblock.h>
#include <features/clicker/clicker.h>
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

    if ((msg == WM_LBUTTONDOWN || msg == WM_LBUTTONUP || msg == WM_LBUTTONDBLCLK) && GetMessageExtraInfo() == static_cast<LPARAM>(clicker::click_tag))
    {
        return 0;
    }

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

// why the lag switch is skipped, for the menu
static const char* netblock_ignored_text()
{
    switch (netblock::ignored_why.load())
    {
    case netblock::ignored_t::elsewhere:
        return "the game was not in front";
    case netblock::ignored_t::menu_keyboard:
        return "the menu had the keyboard";
    case netblock::ignored_t::no_game:
        return "no game to hold";
    default:
        return "";
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

// names of the other players for the lists
static std::vector<std::string> other_player_names()
{
    std::vector<std::string> names;
    for (const cache::entity_t& entity : cache::cached_players)
    {
        if (entity.instance.address != cache::cached_local_player.instance.address)
        {
            names.push_back(entity.name);
        }
    }
    return names;
}

// sort names for display
static void sort_names(std::vector<std::string>& names)
{
    std::sort(names.begin(), names.end(), [](const std::string& a, const std::string& b)
    {

        const int order = ImStricmp(a.c_str(), b.c_str());

        return order != 0 ? order > 0 : a < b;
    });
}

template <typename is_selected_t>
// draw a selectable player list
static int player_list(const char* id, const std::vector<std::string>& names, is_selected_t is_selected)
{
    const ImGuiStyle& style = ImGui::GetStyle();

    const ImVec4 picked_colour = style.Colors[ImGuiCol_SliderGrabActive];
    const ImVec4 hovered_colour = style.Colors[ImGuiCol_Header];
    const ImVec4 pressed_colour = style.Colors[ImGuiCol_ButtonActive];

    ImGui::PushStyleColor(ImGuiCol_Header, picked_colour);
    ImGui::PushStyleColor(ImGuiCol_HeaderHovered, hovered_colour);
    ImGui::PushStyleColor(ImGuiCol_HeaderActive, pressed_colour);

    int clicked = -1;

    const float height = ImMax(ImGui::GetContentRegionAvail().y, ImGui::GetTextLineHeightWithSpacing() * 3.0f);
    if (ImGui::BeginListBox(id, ImVec2(-FLT_MIN, height)))
    {
        if (names.empty())
        {
            ImGui::TextDisabled("No other players yet");
        }

        for (int i = 0; i < static_cast<int>(names.size()); i++)
        {
            if (ImGui::Selectable(names[i].c_str(), is_selected(names[i])))
            {
                clicked = i;
            }
        }

        ImGui::EndListBox();
    }

    ImGui::PopStyleColor(3);

    return clicked;
}

// draw the list of picked players
static void picked_player_list(const char* id, std::unordered_set<std::string>& picked_players)
{
    std::vector<std::string> names;
    std::unordered_set<std::string> picked;
    {
        std::lock_guard<std::mutex> lock(cache::mtx);
        names = other_player_names();
        picked = picked_players;
    }
    sort_names(names);

    const int clicked = player_list(id, names, [&picked](const std::string& name) { return picked.contains(name); });
    if (clicked >= 0)
    {
        std::lock_guard<std::mutex> lock(cache::mtx);
        if (picked_players.erase(names[clicked]) == 0)
        {
            picked_players.insert(names[clicked]);
        }
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

    static std::vector<const char*> m_Tabs = { "Aimbot", "Visual", "Movement", "Utility", "Universal", "Settings" };
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
                static std::vector<const char*> targeting_subtabs = { "Aiming", "Triggerbot" };
                static int targeting_current_subtab = 0;

                ImAdd::RenderSubTabs(targeting_subtabs, targeting_current_subtab);

                ImGui::Dummy(ImVec2(0, style.WindowPadding.y / 2));

                switch (targeting_current_subtab)
                {
                case 0:
                {
                    if (ImAdd::BeginChild("Aimbot", ImVec2(group_width, 0.0f)))
                    {
                        ImAdd::CheckBox("Enable", &settings::targeting::enabled);

                        float available_width = ImGui::GetContentRegionAvail().x - style.WindowPadding.x * 2.0f;
                        float max_keybind_width = ImMin(available_width * 0.4f, 120.0f);

                        ImGui::SameLine(ImGui::GetContentRegionAvail().x + style.WindowPadding.x - max_keybind_width);
                        keybind::activation_mode& targeting_mode = reinterpret_cast<keybind::activation_mode&>(settings::targeting::activation_mode);
                        keybind::keybind_with_mode("Aimbot key", settings::targeting::keybind, targeting_mode, "aimbot", max_keybind_width);
                        settings::targeting::activation_mode = static_cast<int>(targeting_mode);

                        ImAdd::CheckBox("Sticky aim", &settings::targeting::sticky_aim);

                        static std::vector<const char*> parts = { "Closest", "Head", "HumanoidRootPart", "LeftArm", "RightArm", "LeftLeg", "RightLeg" };
                        ImAdd::Combo("Hitbone", &settings::targeting::target_part, parts);

                        ImAdd::SeparatorText("FOV");

                        ImAdd::SliderFloat("Size", &settings::targeting::fov, 0.f, 1000.f, "%.0f");
                        ImAdd::CheckBox("Draw", &settings::targeting::draw_fov);
                        float fov_color_width = ImGui::GetFontSize() * 2.0f + style.ItemSpacing.x;
                        ImGui::SameLine(ImGui::GetContentRegionAvail().x + style.WindowPadding.x - fov_color_width);
                        ImAdd::ColorEdit4WithHex("Fill", settings::targeting::fov_circle_colour);
                        ImGui::SameLine();
                        ImAdd::ColorEdit4WithHex("Outline", settings::targeting::fov_outline_colour);

                        ImAdd::SeparatorText("Misc");

                        ImAdd::SliderFloat("Sensitivity X", &settings::targeting::sensitivity_x, 1.f, 100.f, "%.0f");
                        ImAdd::SliderFloat("Sensitivity Y", &settings::targeting::sensitivity_y, 1.f, 100.f, "%.0f");

                        ImAdd::CheckBox("Prediction", &settings::targeting::enable_prediction);
                        if (settings::targeting::enable_prediction)
                        {
                            ImAdd::SliderFloat("Prediction X", &settings::targeting::prediction_x, 0.f, 10.f, "%.1f");
                            ImAdd::SliderFloat("Prediction Y", &settings::targeting::prediction_y, 0.f, 10.f, "%.1f");
                        }

                        ImAdd::SliderFloat("Switch delay", &settings::targeting::switch_delay, 0.f, 2000.f, "%.0fms");

                        ImAdd::EndChild();
                    }

                    ImGui::SameLine();

                    if (ImAdd::BeginChild("Silent aim", ImVec2(group_width, 0.0f)))
                    {
                        ImAdd::CheckBox("Enable", &settings::correction::enabled);

                        float available_width = ImGui::GetContentRegionAvail().x - style.WindowPadding.x * 2.0f;
                        float max_keybind_width = ImMin(available_width * 0.4f, 120.0f);

                        ImGui::SameLine(ImGui::GetContentRegionAvail().x + style.WindowPadding.x - max_keybind_width);
                        keybind::activation_mode& correction_mode = reinterpret_cast<keybind::activation_mode&>(settings::correction::activation_mode);
                        keybind::keybind_with_mode("Silent aim key", settings::correction::keybind, correction_mode, "silentaim", max_keybind_width);
                        settings::correction::activation_mode = static_cast<int>(correction_mode);

                        bool silent_always = settings::correction::activation_mode == static_cast<int>(keybind::activation_mode::always);
                        if (ImAdd::CheckBox("Always on", &silent_always))
                        {
                            settings::correction::activation_mode = static_cast<int>(silent_always ? keybind::activation_mode::always : keybind::activation_mode::hold);
                        }

                        ImAdd::CheckBox("Sticky aim", &settings::correction::sticky_aim);
                        hint("Stays on the same player while they are inside the FOV,\ninstead of moving on to whoever comes closer.");

                        static std::vector<const char*> correction_parts = { "Closest", "Head", "HumanoidRootPart", "LeftArm", "RightArm", "LeftLeg", "RightLeg", "Random visible" };
                        ImAdd::Combo("Hitbone", &settings::correction::target_part, correction_parts);
                        hint("Random visible: a different body part every quarter second,\nchosen from the ones with a clear line of sight.");

                        ImAdd::SeparatorText("FOV");

                        ImAdd::SliderFloat("Size", &settings::correction::fov, 0.f, 1000.f, "%.0f");
                        ImAdd::CheckBox("Draw", &settings::correction::draw_fov);
                        float fov_color_width = ImGui::GetFontSize() * 2.0f + style.ItemSpacing.x;
                        ImGui::SameLine(ImGui::GetContentRegionAvail().x + style.WindowPadding.x - fov_color_width);
                        ImAdd::ColorEdit4WithHex("Fill", settings::correction::fov_circle_colour);
                        ImGui::SameLine();
                        ImAdd::ColorEdit4WithHex("Outline", settings::correction::fov_outline_colour);

                        ImAdd::SeparatorText("Misc");

                        ImAdd::CheckBox("Prediction", &settings::correction::enable_prediction);
                        if (settings::correction::enable_prediction)
                        {
                            ImAdd::SliderFloat("Prediction X", &settings::correction::prediction_x, 0.f, 10.f, "%.1f");
                            ImAdd::SliderFloat("Prediction Y", &settings::correction::prediction_y, 0.f, 10.f, "%.1f");
                        }

                        ImAdd::SliderFloat("Switch delay", &settings::correction::switch_delay, 0.f, 2000.f, "%.0fms");

                        ImAdd::EndChild();
                    }
                    break;
                }
                case 1:
                {
                    if (ImAdd::BeginChild("Triggerbot", ImVec2(group_width, 0.0f)))
                    {
                        ImAdd::CheckBox("Enable", &settings::autofire::enabled);

                        float available_width = ImGui::GetContentRegionAvail().x - style.WindowPadding.x * 2.0f;
                        float max_keybind_width = ImMin(available_width * 0.4f, 120.0f);

                        ImGui::SameLine(ImGui::GetContentRegionAvail().x + style.WindowPadding.x - max_keybind_width);
                        keybind::activation_mode& autofire_mode = reinterpret_cast<keybind::activation_mode&>(settings::autofire::activation_mode);
                        keybind::keybind_with_mode("##triggerbot_key", settings::autofire::keybind, autofire_mode, "triggerbot", max_keybind_width);
                        settings::autofire::activation_mode = static_cast<int>(autofire_mode);

                        ImAdd::EndChild();
                    }

                    ImGui::SameLine();

                    if (ImAdd::BeginChild("Settings", ImVec2(group_width, 0.0f)))
                    {
                        ImAdd::SliderFloat("Delay", &settings::autofire::delay, 0.f, 100.f, "%.0fms");
                        ImAdd::CheckBox("Randomize delay", &settings::autofire::randomize_delay);
                        if (settings::autofire::randomize_delay)
                        {
                            ImAdd::SliderFloat("Min delay", &settings::autofire::delay_min, 0.f, 100.f, "%.0fms");
                            ImAdd::SliderFloat("Max delay", &settings::autofire::delay_max, 0.f, 100.f, "%.0fms");
                        }

                        ImAdd::SliderFloat("Hit radius", &settings::autofire::hit_radius, 1.f, 50.f, "%.0fpx");
                        hint("Fires when the crosshair is within this many pixels of the target.");

                        static std::vector<const char*> target_parts = { "All parts", "Head only", "Torso only", "Limbs only" };
                        ImAdd::Combo("Hitbone", &settings::autofire::target_part, target_parts);

                        ImAdd::EndChild();
                    }
                    break;
                }
                }
                break;
            }
            case 1:
            {
                if (ImAdd::BeginChild("ESP", ImVec2(group_width, 0.0f)))
                {
                    ImAdd::CheckBox("Enable", &settings::visuals::enabled);

                    ImAdd::CheckBox("Box", &settings::visuals::box);
                    ImGui::SameLine(ImGui::GetContentRegionAvail().x - ImGui::GetFontSize() * 1.5f);
                    ImAdd::ColorEdit4WithHex("BoxColor", settings::visuals::box_color);

                    static std::vector<const char*> box_types = { "Normal", "Corner" };
                    ImAdd::Combo("Box type", &settings::visuals::box_type, box_types);

                    ImAdd::CheckBox("Name", &settings::visuals::name);
                    ImGui::SameLine(ImGui::GetContentRegionAvail().x - ImGui::GetFontSize() * 1.5f);
                    ImAdd::ColorEdit4WithHex("NameColor", settings::visuals::name_color);

                    ImAdd::CheckBox("Healthbar", &settings::visuals::healthbar);
                    ImGui::SameLine(ImGui::GetContentRegionAvail().x - ImGui::GetFontSize() * 1.5f);
                    ImAdd::ColorEdit4WithHex("HealthbarColor", settings::visuals::healthbar_color);

                    ImAdd::CheckBox("Friends", &settings::visuals::friends);
                    hint("Your Roblox friends are always shown, in this colour, and never aimed at (Friend check). Off: they are players like any other.");
                    ImGui::SameLine(ImGui::GetContentRegionAvail().x - ImGui::GetFontSize() * 1.5f);
                    ImAdd::ColorEdit4WithHex("FriendColor", settings::visuals::friend_color);

                    ImAdd::SeparatorText("Team colours");
                    ImAdd::CheckBox("Colour by team", &settings::visuals::team_colors);
                    if (settings::visuals::team_colors)
                    {
                        ImGui::Text("Guards");
                        ImGui::SameLine(ImGui::GetContentRegionAvail().x - ImGui::GetFontSize() * 1.5f);
                        ImAdd::ColorEdit4WithHex("GuardColor", settings::visuals::guard_color);

                        ImGui::Text("Inmates");
                        ImGui::SameLine(ImGui::GetContentRegionAvail().x - ImGui::GetFontSize() * 1.5f);
                        ImAdd::ColorEdit4WithHex("InmateColor", settings::visuals::inmate_color);

                        ImGui::Text("Criminals");
                        ImGui::SameLine(ImGui::GetContentRegionAvail().x - ImGui::GetFontSize() * 1.5f);
                        ImAdd::ColorEdit4WithHex("CriminalColor", settings::visuals::criminal_color);
                    }

                    ImAdd::EndChild();
                }

                ImGui::SameLine();

                const float list_height = ImTrunc((ImGui::GetContentRegionAvail().y - style.ItemSpacing.y) / 2.0f);

                ImGui::BeginGroup();

                if (ImAdd::BeginChild("Always show", ImVec2(group_width, list_height)))
                {
                    picked_player_list("##always_show", cache::spotter_always_players);

                    ImAdd::EndChild();
                }

                if (ImAdd::BeginChild("Whitelist", ImVec2(group_width, 0.0f)))
                {
                    ImGui::Text("Colour");
                    hint("Players picked below are drawn in this colour.");
                    ImGui::SameLine(ImGui::GetContentRegionAvail().x - ImGui::GetFontSize() * 1.5f);
                    ImAdd::ColorEdit4WithHex("WhitelistColor", settings::visuals::whitelist_color);

                    picked_player_list("##whitelist", cache::spotter_whitelist_players);

                    ImAdd::EndChild();
                }

                ImGui::EndGroup();
                break;
            }
            case 2:
            {
                static std::vector<const char*> mover_subtabs = { "Movement", "Teleport" };
                static int mover_current_subtab = 0;

                ImAdd::RenderSubTabs(mover_subtabs, mover_current_subtab);

                ImGui::Dummy(ImVec2(0, style.WindowPadding.y / 2));

                switch (mover_current_subtab)
                {
                case 0:
                {
                    if (ImAdd::BeginChild("Movement", ImVec2(group_width, 0.0f)))
                    {
                        ImAdd::SeparatorText("Speedhack");

                        ImAdd::CheckBox("Enable##speed", &settings::mover::speedhack::enabled);

                        float available_width = ImGui::GetContentRegionAvail().x - style.WindowPadding.x * 2.0f;
                        float max_keybind_width = ImMin(available_width * 0.4f, 120.0f);

                        ImGui::SameLine(ImGui::GetContentRegionAvail().x + style.WindowPadding.x - max_keybind_width);
                        keybind::activation_mode& speedhack_mode = reinterpret_cast<keybind::activation_mode&>(settings::mover::speedhack::activation_mode);
                        keybind::keybind_with_mode("Speedhack key", settings::mover::speedhack::keybind, speedhack_mode, "speedhack", max_keybind_width);
                        settings::mover::speedhack::activation_mode = static_cast<int>(speedhack_mode);

                        static std::vector<const char*> speedhack_modes = { "Walkspeed", "Velocity" };
                        ImAdd::Combo("Mode##speed", &settings::mover::speedhack::mode, speedhack_modes);
                        ImAdd::SliderFloat("Speed", &settings::mover::speedhack::speed, 1.0f, 1000.0f, "%.1f");

                        ImAdd::SeparatorText("Fly");

                        ImAdd::CheckBox("Enable##fly", &settings::expl::fly_enabled);

                        ImGui::SameLine(ImGui::GetContentRegionAvail().x + style.WindowPadding.x - max_keybind_width);
                        keybind::activation_mode& flyhack_mode = reinterpret_cast<keybind::activation_mode&>(settings::expl::fly_keybind_mode);
                        keybind::keybind_with_mode("Flyhack key", settings::expl::fly_keybind, flyhack_mode, "flyhack", max_keybind_width);
                        settings::expl::fly_keybind_mode = static_cast<int>(flyhack_mode);

                        static std::vector<const char*> flyhack_modes = { "Velocity", "Position", "Anchored" };
                        ImAdd::Combo("Mode##fly", &settings::expl::fly_mode, flyhack_modes);
                        hint("Velocity lets walls stop you, Position and Anchored go through them.\nIf one sinks or jitters with no key pressed, try the next.");
                        ImAdd::SliderFloat("Speed##fly", &settings::expl::fly_speed, 1.0f, 1000.0f, "%.1f");

                        ImAdd::EndChild();
                    }

                    ImGui::SameLine();

                    if (ImAdd::BeginChild("Misc", ImVec2(group_width, 0.0f)))
                    {
                        float available_width = ImGui::GetContentRegionAvail().x - style.WindowPadding.x * 2.0f;
                        float max_keybind_width = ImMin(available_width * 0.4f, 120.0f);

                        ImAdd::SeparatorText("Jump power");
                        ImAdd::CheckBox("Enable##jumppower", &settings::mover::jumppower::enabled);

                        ImGui::SameLine(ImGui::GetContentRegionAvail().x + style.WindowPadding.x - max_keybind_width);
                        keybind::activation_mode& jumppower_mode = reinterpret_cast<keybind::activation_mode&>(settings::mover::jumppower::activation_mode);
                        keybind::keybind_with_mode("Jump key", settings::mover::jumppower::keybind, jumppower_mode, "jumppower", max_keybind_width);
                        settings::mover::jumppower::activation_mode = static_cast<int>(jumppower_mode);

                        ImAdd::SliderFloat("Power", &settings::mover::jumppower::value, 0.0f, 500.0f, "%.1f");

                        ImAdd::SeparatorText("Noclip");
                        ImAdd::CheckBox("Enable##noclip", &settings::mover::noclip::enabled);

                        ImGui::SameLine(ImGui::GetContentRegionAvail().x + style.WindowPadding.x - max_keybind_width);
                        keybind::activation_mode& noclip_mode = reinterpret_cast<keybind::activation_mode&>(settings::mover::noclip::activation_mode);
                        keybind::keybind_with_mode("Noclip key", settings::mover::noclip::keybind, noclip_mode, "noclip", max_keybind_width);
                        settings::mover::noclip::activation_mode = static_cast<int>(noclip_mode);

                        ImAdd::SeparatorText("Freecam");
                        ImAdd::CheckBox("Enable##freecam", &settings::mover::freecam::enabled);
                        hint("The camera flies with WASD, Space and Ctrl, the mouse turns it. You stay frozen where you are.\nZoom all the way in to fly it as a free camera. Not with shift lock on.");

                        ImGui::SameLine(ImGui::GetContentRegionAvail().x + style.WindowPadding.x - max_keybind_width);
                        keybind::activation_mode& freecam_mode = reinterpret_cast<keybind::activation_mode&>(settings::mover::freecam::activation_mode);
                        keybind::keybind_with_mode("Freecam key", settings::mover::freecam::keybind, freecam_mode, "freecam", max_keybind_width);
                        settings::mover::freecam::activation_mode = static_cast<int>(freecam_mode);

                        ImAdd::SliderFloat("Speed##freecam", &settings::mover::freecam::speed, settings::mover::freecam::speed_min, settings::mover::freecam::speed_max, "%.0f");

                        ImAdd::SeparatorText("Lag switch");
                        ImAdd::CheckBox("Enable##lagswitch", &settings::mover::netblock::enabled);
                        hint("While it is on the others see you stand where you were, you still see them move.\nSwitch it off and you are where you are now. The key on the right switches it too.");

                        ImGui::SameLine(ImGui::GetContentRegionAvail().x + style.WindowPadding.x - max_keybind_width);

                        keybind::custom_keybind("Lag key", settings::mover::netblock::keybind, max_keybind_width);

                        if (netblock::holding)
                        {
                            ImGui::Text("Holding");
                        }
                        else if (netblock::failed_at != 0 && GetTickCount64() - netblock::failed_at < 5000)
                        {
                            ImGui::Text("Could not hold, see the log");
                        }
                        else if (settings::mover::netblock::enabled && settings::mover::netblock::keybind == 0)
                        {
                            ImGui::Text("Starting");
                        }
                        else if (settings::mover::netblock::enabled && netblock::ignored_at != 0 && GetTickCount64() - netblock::ignored_at < 4000)
                        {
                            ImGui::Text("Key ignored: %s", netblock_ignored_text());
                        }
                        else if (settings::mover::netblock::enabled)
                        {
                            ImGui::TextDisabled("Starting");
                        }

                        ImAdd::EndChild();
                    }
                    break;
                }
                case 1:
                {
                    std::vector<std::string> names;
                    std::string target;
                    {
                        std::lock_guard<std::mutex> lock(cache::mtx);
                        names = other_player_names();
                        target = cache::teleport_target;
                    }

                    if (!target.empty() && std::find(names.begin(), names.end(), target) == names.end())
                    {
                        names.push_back(target);
                    }
                    sort_names(names);

                    if (ImAdd::BeginChild("Teleport", ImVec2(group_width, 0.0f)))
                    {
                        ImAdd::CheckBox("Enable", &settings::mover::teleport::enabled);
                        hint("Puts you on the picked player, again every 0.25 seconds.\nThe key starts the loop and stops it.");

                        float available_width = ImGui::GetContentRegionAvail().x - style.WindowPadding.x * 2.0f;
                        float max_keybind_width = ImMin(available_width * 0.4f, 120.0f);

                        ImGui::SameLine(ImGui::GetContentRegionAvail().x + style.WindowPadding.x - max_keybind_width);
                        keybind::activation_mode& teleport_mode = reinterpret_cast<keybind::activation_mode&>(settings::mover::teleport::activation_mode);
                        keybind::keybind_with_mode("Teleport key", settings::mover::teleport::keybind, teleport_mode, "teleport", max_keybind_width);
                        settings::mover::teleport::activation_mode = static_cast<int>(teleport_mode);

                        switch (mover::get_teleport_status())
                        {
                        case mover::teleport_status::looping:
                            ImGui::Text("Loop on: %s", target.c_str());
                            break;
                        case mover::teleport_status::waiting:
                            ImGui::Text("Loop on, waiting for %s", target.c_str());
                            break;
                        case mover::teleport_status::no_target:
                            ImGui::Text("Loop on, pick a player");
                            break;
                        default:
                            if (settings::mover::teleport::enabled && settings::mover::teleport::keybind != 0)
                            {
                                ImGui::TextDisabled("Loop off, press %s to start", keybind::get_key_name(settings::mover::teleport::keybind).c_str());
                            }
                            else
                            {
                                ImGui::TextDisabled("Loop off");
                            }
                            break;
                        }

                        ImAdd::EndChild();
                    }

                    ImGui::SameLine();

                    if (ImAdd::BeginChild("Players", ImVec2(group_width, 0.0f)))
                    {
                        const int clicked = player_list("##teleport_target", names, [&target](const std::string& name) { return name == target; });
                        if (clicked >= 0)
                        {
                            std::lock_guard<std::mutex> lock(cache::mtx);
                            cache::teleport_target = names[clicked] == target ? std::string{} : names[clicked];
                        }

                        ImAdd::EndChild();
                    }
                    break;
                }
                }
                break;
            }
            case 3:
            {
                if (ImAdd::BeginChild("Anti fling", ImVec2(group_width, 0.0f)))
                {
                    ImAdd::CheckBox("Enable##antifling", &settings::mover::stabilizer::enabled);
                    hint("Cars and players that move impossibly fast cannot touch you,\nand a throw is undone before the game reports it.");

                    ImAdd::SliderFloat("Max speed", &settings::mover::stabilizer::max_speed, settings::mover::stabilizer::max_speed_min, settings::mover::stabilizer::max_speed_max, "%.0f");
                    hint("The fastest anything moves you, in studs a second. More is cut back at once,\nmore than twice as much at once is a throw and is undone. Falling is left alone.\nYour speedhack, fly and jump power stay within it too.\nWalking is 16, a car about 100.");

                    ImAdd::CheckBox("Anti ragdoll", &settings::mover::stabilizer::anti_ragdoll);
                    hint("When the game knocks you over or ragdolls you, you are stood back up.");

                    ImAdd::CheckBox("Parked seats only", &settings::mover::stabilizer::lock_seats);
                    hint("A seat can only take you while its vehicle is parked next to you.\nA car that is thrown at you cannot sit you down and carry you off.\nGetting into a parked car works as before.");

                    ImAdd::CheckBox("Leave thrown vehicles", &settings::mover::stabilizer::leave_vehicles);
                    hint("Driving is left alone. A vehicle that carries you faster than the limit\nis being thrown: you jump out, are put back where you were,\nand stay out of seats for 3 seconds.");
                    if (settings::mover::stabilizer::leave_vehicles)
                    {
                        ImAdd::SliderFloat("Vehicle limit", &settings::mover::stabilizer::vehicle_speed, settings::mover::stabilizer::vehicle_speed_min, settings::mover::stabilizer::vehicle_speed_max, "%.0f");
                        hint("Studs a second, over the ground or upwards. Falling does not count.\nA car at full speed is about 100 to 140.");
                    }

                    if (settings::mover::stabilizer::enabled)
                    {
                        if (settings::mover::stabilizer::leave_vehicles || settings::mover::stabilizer::lock_seats)
                        {
                            const int thrown = stabilizer::seats_thrown;
                            if (thrown > 0)
                            {
                                ImGui::Text("Seats: %d watched, %d locked, %d thrown", stabilizer::seats_watched.load(), stabilizer::seats_locked.load(), thrown);
                            }
                            else
                            {
                                ImGui::TextDisabled("Seats: %d watched, %d locked", stabilizer::seats_watched.load(), stabilizer::seats_locked.load());
                            }
                        }

                        const int left = stabilizer::vehicles_left;
                        if (left > 0)
                        {
                            ImGui::Text("Thrown vehicles left: %d, put back: %d", left, stabilizer::put_backs.load());
                        }

                        const int stopped = stabilizer::throws_stopped;
                        const int kept_off = stabilizer::parts_kept_off;
                        if (kept_off > 0)
                        {
                            ImGui::Text("Throws undone: %d, keeping %d parts off", stopped, kept_off);
                        }
                        else
                        {
                            ImGui::TextDisabled("Throws undone: %d", stopped);
                        }

                        if (GetTickCount64() - stabilizer::last_slowed < 1000)
                        {
                            ImGui::Text("Holding you at %.0f", stabilizer::top_speed());
                        }
                    }

                    ImAdd::EndChild();
                }

                ImGui::SameLine();

                ImGui::BeginGroup();

                const float clicker_height = ImGui::GetFrameHeight() + style.WindowPadding.y * 2.0f + ImGui::GetFontSize() * 4.0f + style.ItemInnerSpacing.y + style.ItemSpacing.y * 2.0f;

                if (ImAdd::BeginChild("Auto clicker", ImVec2(group_width, clicker_height)))
                {

                    bool clicker_on = clicker::on;
                    if (ImAdd::CheckBox("Enable##autoclicker", &clicker_on))
                    {
                        clicker::on = clicker_on;
                    }
                    hint("Left clicks where the cursor is, in the game only.\nThe key on the right switches it on and off, and so does Shift + G.");

                    {
                        const float key_width = ImMin((ImGui::GetContentRegionAvail().x - style.WindowPadding.x * 2.0f) * 0.4f, 120.0f);
                        ImGui::SameLine(ImGui::GetContentRegionAvail().x + style.WindowPadding.x - key_width);
                        keybind::custom_keybind("Clicker key", settings::clicker::keybind, key_width);
                    }

                    ImAdd::SliderInt("Clicks a second", &settings::clicker::clicks_per_second, settings::clicker::clicks_per_second_min, settings::clicker::clicks_per_second_max);
                    hint("OP Auto Clicker at \"1 millisecond\" makes about 60.\nRoblox froze at 1000, so go up in steps.");

                    if (!clicker_on)
                    {
                        ImGui::TextDisabled("Off, %s switches it too", keybind::get_key_name(settings::clicker::keybind).c_str());
                    }
                    else if (!clicker::clicking)
                    {
                        ImGui::Text("On, paused outside the game");
                    }
                    else
                    {

                        const int rate = clicker::clicks_per_second;
                        if (rate > 0)
                        {
                            ImGui::Text("Clicking, %d a second", rate);
                        }
                        else
                        {
                            ImGui::Text("Clicking");
                        }
                    }

                    ImAdd::EndChild();
                }

                if (ImAdd::BeginChild("Macros", ImVec2(group_width, 0.0f)))
                {
                    float available_width = ImGui::GetContentRegionAvail().x - style.WindowPadding.x * 2.0f;
                    float max_keybind_width = ImMin(available_width * 0.4f, 120.0f);

                    ImAdd::SeparatorText("Glitch macro");
                    ImAdd::CheckBox("Enable##macro", &settings::macros::enabled);
                    hint("Crouch with your head under a part, then press the key.");

                    ImGui::SameLine(ImGui::GetContentRegionAvail().x + style.WindowPadding.x - max_keybind_width);
                    keybind::custom_keybind("Macro key", settings::macros::keybind, max_keybind_width);

                    ImAdd::SliderInt("Spin", &settings::macros::spin, settings::macros::spin_min, settings::macros::spin_max);
                    hint("Change only if the turn is too slow or too fast.");
                    ImAdd::SliderInt("Mouse DPI", &settings::macros::dpi, settings::macros::dpi_min, settings::macros::dpi_max);
                    ImAdd::SliderFloat("Camera sensitivity", &settings::macros::sensitivity, settings::macros::sensitivity_min, settings::macros::sensitivity_max, "%.2f");

                    ImAdd::EndChild();
                }

                ImGui::EndGroup();
                break;
            }
            case 4:
            {

                if (ImAdd::BeginChild("Checks", ImVec2(group_width, 0.0f)))
                {
                    ImAdd::CheckBox("Team check", &settings::universal::teamcheck);
                    hint("Skips your own team.");
                    ImAdd::CheckBox("Friend check", &settings::universal::friend_check);
                    hint("Nothing aims at your Roblox friends. Only while Friends (Visual tab) is on.");
                    ImAdd::CheckBox("Knock check", &settings::universal::knock_check);

                    ImAdd::CheckBox("Health check", &settings::universal::health_check);
                    hint("Skips dead players and anyone below the minimum.");
                    if (settings::universal::health_check)
                    {
                        ImAdd::SliderFloat("Min health", &settings::universal::min_health, 0.0f, 100.0f, "%.0f");
                    }

                    ImAdd::CheckBox("Max distance", &settings::universal::distance_check);
                    if (settings::universal::distance_check)
                    {
                        ImAdd::SliderFloat("Distance", &settings::universal::max_distance, 10.0f, 5000.0f, "%.0f studs");
                    }

                    ImAdd::CheckBox("Wall check", &settings::universal::wall_check);
                    hint("Targeting only, ESP still shows players behind walls.");
                    ImAdd::CheckBox("Inmates: hostile only", &settings::universal::hostile_inmates_only);
                    hint("Prison Life: skips every inmate except the ones with the red tag.");

                    ImAdd::EndChild();
                }

                ImGui::SameLine();

                if (ImAdd::BeginChild("Teams", ImVec2(group_width, 0.0f)))
                {

                    std::vector<cache::team_t> teams;
                    std::unordered_set<std::string> spotter_off;
                    std::unordered_set<std::string> target_off;
                    std::uint64_t own_team = 0;
                    {
                        std::lock_guard<std::mutex> lock(cache::mtx);
                        teams = cache::cached_teams;
                        spotter_off = cache::spotter_off_teams;
                        target_off = cache::target_off_teams;
                        own_team = cache::local_team;

                        cache::team_t none{};
                        none.name = cache::no_team_name;
                        none.players = cache::players_without_team;
                        teams.push_back(none);
                    }

                    auto set_team = [](std::unordered_set<std::string>& off_teams, const std::string& name, bool on)
                    {
                        std::lock_guard<std::mutex> lock(cache::mtx);
                        if (on)
                        {
                            off_teams.erase(name);
                        }
                        else
                        {
                            off_teams.insert(name);
                        }
                    };

                    ImGui::PushStyleVar(ImGuiStyleVar_CellPadding, ImVec2(6.0f, 5.0f));
                    if (ImGui::BeginTable("##teams", 4, ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerH))
                    {
                        ImGui::TableSetupColumn("Team", ImGuiTableColumnFlags_WidthStretch);
                        ImGui::TableSetupColumn("Players", ImGuiTableColumnFlags_WidthFixed, 60.0f);
                        ImGui::TableSetupColumn("ESP", ImGuiTableColumnFlags_WidthFixed, 45.0f);
                        ImGui::TableSetupColumn("Target", ImGuiTableColumnFlags_WidthFixed, 55.0f);
                        ImGui::TableHeadersRow();

                        for (size_t i = 0; i < teams.size(); i++)
                        {
                            const cache::team_t& team = teams[i];
                            const bool is_own = team.address != 0 && team.address == own_team;

                            ImGui::TableNextRow();

                            ImGui::TableNextColumn();
                            ImGui::TextUnformatted((team.name + (is_own ? " (yours)" : "")).c_str());

                            ImGui::TableNextColumn();
                            ImGui::Text("%d", team.players);

                            ImGui::TableNextColumn();
                            bool spotter_on = !spotter_off.contains(team.name);
                            if (ImAdd::CheckBox(("##team_esp" + std::to_string(i)).c_str(), &spotter_on))
                            {
                                set_team(cache::spotter_off_teams, team.name, spotter_on);
                            }

                            ImGui::TableNextColumn();
                            bool target_on = !target_off.contains(team.name);
                            if (ImAdd::CheckBox(("##team_target" + std::to_string(i)).c_str(), &target_on))
                            {
                                set_team(cache::target_off_teams, team.name, target_on);
                            }
                        }

                        ImGui::EndTable();
                    }
                    ImGui::PopStyleVar();

                    ImAdd::EndChild();
                }
                break;
            }
            case 5:
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

// draw the custom crosshair
static void draw_custom_cursor()
{

}

// draw the lag switch status indicator
static void draw_netblock_state()
{
    char text[96]{};
    ImU32 colour = IM_COL32(255, 255, 255, 255);
    const ULONGLONG now = GetTickCount64();

    if (netblock::holding)
    {
        std::snprintf(text, sizeof(text), "Lag switch on");
        colour = IM_COL32(255, 80, 80, 255);
    }
    else if (netblock::failed_at != 0 && now - netblock::failed_at < 4000)
    {
        std::snprintf(text, sizeof(text), "Lag switch could not hold, see the log");
    }
    else if (settings::mover::netblock::enabled && netblock::ignored_at != 0 && now - netblock::ignored_at < 3000)
    {
        std::snprintf(text, sizeof(text), "Lag key ignored: %s", netblock_ignored_text());
    }
    else
    {
        return;
    }

    const bool has_view = game::window_width > 0.0f && game::window_height > 0.0f;
    const float middle = has_view ? game::window_offset_x + game::window_width * 0.5f : ImGui::GetIO().DisplaySize.x * 0.5f;
    const float top = has_view ? game::window_offset_y : 0.0f;

    const ImVec2 size = ImGui::CalcTextSize(text);
    const ImVec2 at(ImTrunc(middle - size.x * 0.5f), ImTrunc(top + 48.0f));

    ImDrawList* draw = ImGui::GetBackgroundDrawList();
    draw->AddText(at + ImVec2(1.0f, 1.0f), IM_COL32(0, 0, 0, 255), text);
    draw->AddText(at, colour, text);
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

	if (settings::correction::draw_fov && settings::correction::enabled)
	{
		draw_fov_circle(settings::correction::fov, settings::correction::fov_circle_colour, settings::correction::fov_outline_colour);
	}

	if (settings::targeting::draw_fov && settings::targeting::enabled)
	{
		draw_fov_circle(settings::targeting::fov, settings::targeting::fov_circle_colour, settings::targeting::fov_outline_colour);
	}

    draw_custom_cursor();
    draw_netblock_state();
}
