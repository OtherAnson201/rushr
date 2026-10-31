#pragma once
#include <atomic>
#include <memory>
#include <vector>
#include <string>

#include <d3d11.h>

#include <imgui/imgui.h>
#include <imgui/imgui_internal.h>
#include <imgui/misc/imgui_freetype.h>
#include <imgui/backends/imgui_impl_dx11.h>
#include <imgui/backends/imgui_impl_win32.h>
#include <imgui/addons/imgui_addons.h>

inline ImFont* spotter_font = nullptr;
inline ImFont* spotter_font_tahoma = nullptr;
inline ImFont* spotter_font_sp7 = nullptr;
inline ImFont* spotter_font_arial = nullptr;

struct detail_t {
	HWND window = nullptr;
	WNDCLASSEX window_class = {};
	ID3D11Device* device = nullptr;
	ID3D11DeviceContext* device_context = nullptr;
	ID3D11RenderTargetView* render_target_view = nullptr;
	IDXGISwapChain* swap_chain = nullptr;
};

class render_t {
public:
	render_t();
	~render_t();

	bool running = false;

	std::atomic<bool> menu_has_mouse{ false };

	void start_render();
	void render_menu();
	void render_visuals();
	void end_render();

	void set_click_through(bool enabled);

	void update_input_passthrough(bool menu_drawn);

	void update_capture_exclusion();

	bool create_device();
	bool create_window();
	bool create_imgui();

	std::unique_ptr<detail_t> detail = std::make_unique<detail_t>();
private:

	bool click_through = true;

	bool keyboard_grabbed = false;

	int idle_active_frames = 0;

	DWORD rejected_affinity = 0xFFFFFFFF;

	void destroy_device();
	void destroy_window();
	void destroy_imgui();
};

inline render_t* render = new render_t;