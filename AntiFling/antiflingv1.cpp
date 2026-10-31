// Anti fling on its own (copy of Glory's guard). Not for use alongside Glory's anti fling. Args: [60-500] [--check] [--verbose] [--force]
// Build: cl /nologo /std:c++latest /EHsc /MT /O2 /W3 antiflingv1.cpp /link /MANIFEST:EMBED /MANIFESTUAC:"level='requireAdministrator' uiAccess='false'"

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <TlHelp32.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <format>
#include <iterator>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

#include "../Glory/src/Offsets/Offsets.hpp"

#pragma comment(lib, "user32.lib")

using namespace std::chrono_literals;

std::atomic<bool> g_running{ true };

namespace math
{
	struct vector3 final
	{
		float x{ 0.f }, y{ 0.f }, z{ 0.f };

		float length() const
		{
			return std::sqrt(x * x + y * y + z * z);
		}

		float distance(const vector3& other) const
		{
			float dx = x - other.x;
			float dy = y - other.y;
			float dz = z - other.z;
			return std::sqrt(dx * dx + dy * dy + dz * dz);
		}
	};

	inline vector3 operator+(const vector3& a, const vector3& b)
	{
		return { a.x + b.x, a.y + b.y, a.z + b.z };
	}

	inline vector3 operator-(const vector3& a, const vector3& b)
	{
		return { a.x - b.x, a.y - b.y, a.z - b.z };
	}

	inline vector3 operator*(const vector3& v, float scalar)
	{
		return { v.x * scalar, v.y * scalar, v.z * scalar };
	}

	inline vector3 operator*(float scalar, const vector3& v)
	{
		return { v.x * scalar, v.y * scalar, v.z * scalar };
	}

	struct matrix3 final
	{
		float m[3][3];

		static matrix3 identity()
		{
			matrix3 result{};
			result.m[0][0] = 1.f; result.m[0][1] = 0.f; result.m[0][2] = 0.f;
			result.m[1][0] = 0.f; result.m[1][1] = 1.f; result.m[1][2] = 0.f;
			result.m[2][0] = 0.f; result.m[2][1] = 0.f; result.m[2][2] = 1.f;
			return result;
		}
	};
}

namespace logger
{
	enum class level : int
	{
		trace,
		debug,
		info,
		warn,
		error,
		fatal
	};

	struct site_t
	{
		const char* file;
		std::uint32_t line;
		const char* function;
	};

	inline level minimum = level::info;

	inline bool should_log(level lvl)
	{
		return lvl >= minimum;
	}

	inline const char* level_name(level lvl)
	{
		switch (lvl)
		{
		case level::trace: return "trace";
		case level::debug: return "debug";
		case level::info: return "info";
		case level::warn: return "WARN";
		case level::error: return "ERROR";
		default: return "FATAL";
		}
	}

	inline void write(level lvl, const site_t&, std::string message)
	{
		static std::mutex output_mutex;

		SYSTEMTIME time{};
		GetLocalTime(&time);

		std::lock_guard<std::mutex> lock(output_mutex);
		std::printf("[%02d:%02d:%02d.%03d] %-5s %s\n", time.wHour, time.wMinute, time.wSecond, time.wMilliseconds, level_name(lvl), message.c_str());
		std::fflush(stdout);
	}

	template <typename... Args>
	void log(level lvl, const site_t& site, std::format_string<Args...> fmt, Args&&... args)
	{
		write(lvl, site, std::format(fmt, std::forward<Args>(args)...));
	}

	inline bool throttle(std::atomic<std::uint64_t>& last_tick, std::uint64_t interval_ms)
	{
		const std::uint64_t now = GetTickCount64();
		std::uint64_t last = last_tick.load();

		if (last != 0 && now - last < interval_ms)
			return false;

		return last_tick.compare_exchange_strong(last, now);
	}
}

#define LOG_AT(lvl, ...) \
	do { \
		if (::logger::should_log(lvl)) \
			::logger::log(lvl, ::logger::site_t{ __FILE__, static_cast<std::uint32_t>(__LINE__), __FUNCTION__ }, __VA_ARGS__); \
	} while (0)

#define LOG_TRACE(...) LOG_AT(::logger::level::trace, __VA_ARGS__)
#define LOG_DEBUG(...) LOG_AT(::logger::level::debug, __VA_ARGS__)
#define LOG_INFO(...)  LOG_AT(::logger::level::info, __VA_ARGS__)
#define LOG_WARN(...)  LOG_AT(::logger::level::warn, __VA_ARGS__)
#define LOG_ERROR(...) LOG_AT(::logger::level::error, __VA_ARGS__)
#define LOG_FATAL(...) LOG_AT(::logger::level::fatal, __VA_ARGS__)

#define LOG_EVERY(interval_ms, lvl, ...) \
	do { \
		static std::atomic<std::uint64_t> log_last_tick{ 0 }; \
		if (::logger::should_log(lvl) && ::logger::throttle(log_last_tick, interval_ms)) \
			::logger::log(lvl, ::logger::site_t{ __FILE__, static_cast<std::uint32_t>(__LINE__), __FUNCTION__ }, __VA_ARGS__); \
	} while (0)

class precise_sleeper final
{
public:
	precise_sleeper()
	{
		timer = CreateWaitableTimerExW(nullptr, nullptr, 0x00000002, TIMER_ALL_ACCESS);
	}

	~precise_sleeper()
	{
		if (timer != nullptr)
		{
			CloseHandle(timer);
		}
	}

	precise_sleeper(const precise_sleeper&) = delete;
	precise_sleeper& operator=(const precise_sleeper&) = delete;

	void sleep_ms(double milliseconds)
	{
		if (timer != nullptr)
		{
			LARGE_INTEGER due{};
			due.QuadPart = -static_cast<LONGLONG>(milliseconds * 10000.0);

			if (SetWaitableTimer(timer, &due, 0, nullptr, nullptr, FALSE))
			{
				WaitForSingleObject(timer, 1000);
				return;
			}
		}

		Sleep(milliseconds < 1.0 ? 1 : static_cast<DWORD>(milliseconds));
	}

private:
	HANDLE timer = nullptr;
};

namespace settings::movement::antifling
{
	inline float max_speed = 100.0f;
	inline constexpr float max_speed_min = 60.0f;
	inline constexpr float max_speed_max = 500.0f;
}

class memory_t final
{
public:
	std::uint32_t find_process_id(const std::string& process_name);
	std::uint64_t find_module_address(const std::string& module_name);

	static bool has_visible_window(std::uint32_t process_id);

	bool attach_to_process(const std::string& process_name);
	void detach();
	bool process_ended();

	std::string read_string(std::uint64_t address);

	void read_buffer(std::uint64_t address, void* buffer, std::size_t size);
	void write_buffer(std::uint64_t address, const void* buffer, std::size_t size);

	template <typename T>
	T read(std::uint64_t address)
	{
		T buffer{};
		read_buffer(address, &buffer, sizeof(T));
		return buffer;
	}

	template <typename T>
	void write(std::uint64_t address, T value)
	{
		write_buffer(address, &value, sizeof(T));
	}

	std::uint32_t get_process_id() { return process_id; }
	std::uint64_t get_module_address() { return base_address; }
	std::uint64_t get_module_size() { return module_size; }
	HANDLE get_process_handle() { return process_handle; }

private:
	std::uint32_t process_id{};
	std::uint64_t base_address{};
	std::uint64_t module_size{};
	std::atomic<HANDLE> process_handle{};
};

inline std::unique_ptr<memory_t> memory = std::make_unique<memory_t>();

bool memory_t::has_visible_window(std::uint32_t process_id)
{
	struct search_t
	{
		DWORD process_id = 0;
		bool found = false;
	};

	search_t search{};
	search.process_id = process_id;

	EnumWindows([](HWND window, LPARAM lParam) -> BOOL
		{
			search_t& search = *reinterpret_cast<search_t*>(lParam);

			DWORD owner = 0;
			GetWindowThreadProcessId(window, &owner);
			if (owner == search.process_id && IsWindowVisible(window))
			{
				search.found = true;
				return FALSE;
			}

			return TRUE;
		}, reinterpret_cast<LPARAM>(&search));

	return search.found;
}

std::uint32_t memory_t::find_process_id(const std::string& process_name)
{
	HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);

	if (snapshot == INVALID_HANDLE_VALUE)
	{
		return 0;
	}

	std::vector<std::uint32_t> matches;

	PROCESSENTRY32W process_entry{};
	process_entry.dwSize = sizeof(PROCESSENTRY32W);

	if (Process32FirstW(snapshot, &process_entry))
	{
		std::wstring w_process_name(process_name.begin(), process_name.end());
		do
		{
			if (!lstrcmpiW(w_process_name.c_str(), process_entry.szExeFile))
			{
				matches.push_back(process_entry.th32ProcessID);
			}
		} while (Process32NextW(snapshot, &process_entry));
	}

	CloseHandle(snapshot);

	if (matches.empty())
	{
		return 0;
	}

	for (std::uint32_t candidate : matches)
	{
		if (has_visible_window(candidate))
		{
			return candidate;
		}
	}

	return matches.front();
}

std::uint64_t memory_t::find_module_address(const std::string& module_name)
{
	std::uint64_t module_address = 0;

	HANDLE process = process_handle;
	if (!process)
	{
		return module_address;
	}

	DWORD pID = GetProcessId(process);

	HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPMODULE | TH32CS_SNAPMODULE32, pID);

	if (snapshot == INVALID_HANDLE_VALUE)
	{
		LOG_EVERY(30000, logger::level::warn, "module snapshot of pid {} failed (error {})", pID, GetLastError());
		return module_address;
	}

	MODULEENTRY32W module_entry{};
	module_entry.dwSize = sizeof(MODULEENTRY32W);

	std::wstring w_module_name(module_name.begin(), module_name.end());

	if (Module32FirstW(snapshot, &module_entry))
	{
		do
		{
			if (!lstrcmpiW(w_module_name.c_str(), module_entry.szModule))
			{
				module_address = reinterpret_cast<std::uint64_t>(module_entry.modBaseAddr);
				base_address = module_address;
				module_size = module_entry.modBaseSize;
				break;
			}
		} while (Module32NextW(snapshot, &module_entry));
	}

	CloseHandle(snapshot);
	return module_address;
}

bool memory_t::attach_to_process(const std::string& process_name)
{
	std::uint32_t pID = find_process_id(process_name);
	if (pID == 0)
	{
		LOG_DEBUG("process {} is not running", process_name);
		return false;
	}

	HANDLE process = OpenProcess(PROCESS_VM_READ | PROCESS_VM_WRITE | PROCESS_VM_OPERATION | PROCESS_QUERY_LIMITED_INFORMATION | SYNCHRONIZE, false, pID);
	if (!process || process == INVALID_HANDLE_VALUE)
	{
		const DWORD error = GetLastError();
		LOG_EVERY(30000, logger::level::error, "OpenProcess failed for {} (pid {}, error {}{})", process_name, pID, error,
			error == ERROR_ACCESS_DENIED ? ", access denied: start this as administrator" : "");
		return false;
	}

	HANDLE previous = process_handle.exchange(process);
	process_id = pID;
	base_address = 0;
	module_size = 0;

	if (previous)
	{
		CloseHandle(previous);
	}

	LOG_DEBUG("opened {} (pid {})", process_name, pID);
	return true;
}

void memory_t::detach()
{
	HANDLE previous = process_handle.exchange(nullptr);
	process_id = 0;
	base_address = 0;
	module_size = 0;

	if (previous)
	{
		CloseHandle(previous);
	}
}

bool memory_t::process_ended()
{
	HANDLE process = process_handle;
	return process != nullptr && WaitForSingleObject(process, 0) == WAIT_OBJECT_0;
}

std::string memory_t::read_string(std::uint64_t address)
{
	struct
	{
		char text[16];
		std::uint64_t length;
		std::uint64_t capacity;
	} object{};
	read_buffer(address, &object, sizeof(object));

	const std::int32_t string_length = static_cast<std::int32_t>(object.length);

	if (string_length <= 0 || string_length > 255)
	{
		if (string_length != 0)
		{
			LOG_EVERY(5000, logger::level::warn, "read_string: bad length {} at 0x{:X}", string_length, address);
		}
		return "Unknown";
	}

	bool behind_pointer = string_length >= 16;
	if (object.capacity == 15 && string_length <= 15)
	{
		behind_pointer = false;
	}
	else if (object.capacity >= 16 && object.capacity < 0x100000000ull && object.capacity >= static_cast<std::uint64_t>(string_length))
	{
		behind_pointer = true;
	}

	if (!behind_pointer)
	{
		return std::string(object.text, string_length);
	}

	std::uint64_t string_address = 0;
	std::memcpy(&string_address, object.text, sizeof(string_address));

	std::vector<char> buffer(string_length + 1, 0);
	read_buffer(string_address, buffer.data(), buffer.size());

	return std::string(buffer.data(), string_length);
}

void memory_t::read_buffer(std::uint64_t address, void* buffer, std::size_t size)
{
	ReadProcessMemory(process_handle, reinterpret_cast<const void*>(address), buffer, size, nullptr);
}

void memory_t::write_buffer(std::uint64_t address, const void* buffer, std::size_t size)
{
	WriteProcessMemory(process_handle, reinterpret_cast<void*>(address), buffer, size, nullptr);
}

namespace rbx
{
	struct instance_t
	{
		std::uint64_t address = 0;

		instance_t() = default;
		instance_t(std::uint64_t address) : address(address) {}

		std::string get_name() const;
		std::string get_class_name() const;
		std::vector<instance_t> get_children() const;
		instance_t find_first_child_by_class(std::string_view str) const;
	};

	inline std::string instance_t::get_name() const
	{
		std::uint64_t name_container = memory->read<std::uint64_t>(this->address + Offsets::Instance::NameContainer);

		if (name_container)
		{
			return memory->read_string(name_container + Offsets::Instance::Name);
		}

		return "unknown";
	}

	inline std::string instance_t::get_class_name() const
	{
		std::uint64_t class_descriptor = memory->read<std::uint64_t>(this->address + Offsets::Instance::ClassDescriptor);
		std::uint64_t class_name = memory->read<std::uint64_t>(class_descriptor + Offsets::Instance::ClassName);

		if (class_name)
		{
			return memory->read_string(class_name);
		}

		return "unknown";
	}

	inline std::vector<instance_t> instance_t::get_children() const
	{
		std::vector<instance_t> children;

		if (address == 0)
			return children;

		try
		{
			std::uint64_t children_struct = memory->read<std::uint64_t>(address + Offsets::Instance::ChildrenStart);
			if (children_struct == 0)
				return children;

			std::uint64_t start = memory->read<std::uint64_t>(children_struct);
			std::uint64_t end = memory->read<std::uint64_t>(children_struct + Offsets::Instance::ChildrenEnd);

			if (start == 0 || end == 0 || end <= start)
				return children;

			constexpr std::uint64_t kStep = 16;
			constexpr std::uint64_t kMaxChildren = 10000;
			constexpr std::uint64_t kMaxByteSize = kMaxChildren * kStep;

			std::uint64_t byte_size = end - start;
			if (byte_size > kMaxByteSize)
				return children;

			if (byte_size % kStep != 0)
				return children;

			std::uint64_t count = byte_size / kStep;
			if (count == 0)
				return children;

			children.reserve(static_cast<std::size_t>(count));

			for (std::uint64_t instance = start; instance < end; instance += kStep)
			{
				std::uint64_t child_address = memory->read<std::uint64_t>(instance);
				if (child_address != 0 && child_address != 0xFFFFFFFFFFFFFFFF)
				{
					children.emplace_back(child_address);
				}
			}
		}
		catch (...)
		{
			return children;
		}

		return children;
	}

	inline instance_t instance_t::find_first_child_by_class(std::string_view str) const
	{
		std::vector<instance_t> children = this->get_children();

		for (const instance_t& child : children)
		{
			if (child.get_class_name() == str)
			{
				return child;
			}
		}

		return {};
	}
}

namespace game
{
	inline rbx::instance_t datamodel{};
	inline rbx::instance_t players{};
	inline rbx::instance_t workspace{};
	inline std::uint64_t visual_engine = 0;

	inline std::atomic<bool> game_changed{ true };
	inline std::atomic<std::uint32_t> session{ 0 };
	inline std::uint64_t current_job_id = 0;

	bool check_game_changed();
	bool reinitialize_game_state();
	void detach();
}

namespace
{
	bool is_pointer(std::uint64_t value)
	{
		return value >= 0x10000 && value < 0x7FFFFFFFFFFFull;
	}

	struct game_pointers_t
	{
		std::uint64_t visual_engine = 0;
		std::uint64_t datamodel = 0;
	};

	game_pointers_t resolve_pointers()
	{
		game_pointers_t found;

		const std::uint64_t module_base = memory->get_module_address();
		if (module_base == 0)
			return found;

		const std::uint64_t visual_engine = memory->read<std::uint64_t>(module_base + Offsets::VisualEngine::Pointer);
		if (!is_pointer(visual_engine))
			return found;

		const std::uint64_t fake_datamodel = memory->read<std::uint64_t>(visual_engine + Offsets::VisualEngine::FakeDataModel);
		if (!is_pointer(fake_datamodel))
			return found;

		const std::uint64_t datamodel = memory->read<std::uint64_t>(fake_datamodel + Offsets::FakeDataModel::RealDataModel);
		if (!is_pointer(datamodel))
			return found;

		found.visual_engine = visual_engine;
		found.datamodel = datamodel;
		return found;
	}
}

bool game::check_game_changed()
{
	const game_pointers_t now = resolve_pointers();

	bool changed = now.datamodel != game::datamodel.address || now.visual_engine != game::visual_engine;

	if (!changed && now.datamodel != 0)
	{
		const std::uint64_t job_id = memory->read<std::uint64_t>(now.datamodel + Offsets::DataModel::JobId);
		if (game::current_job_id == 0)
		{
			game::current_job_id = job_id;
		}
		else if (job_id != game::current_job_id)
		{
			changed = true;
		}
	}

	if (changed)
	{
		game::game_changed = true;
	}

	return changed;
}

bool game::reinitialize_game_state()
{
	const game_pointers_t found = resolve_pointers();
	if (found.datamodel == 0)
	{
		LOG_DEBUG("no datamodel to be found yet (module base 0x{:X})", memory->get_module_address());
		return false;
	}

	const rbx::instance_t players_service = rbx::instance_t{ found.datamodel }.find_first_child_by_class("Players");
	if (players_service.address == 0)
	{
		LOG_DEBUG("datamodel 0x{:X} has no Players service yet", found.datamodel);
		return false;
	}

	game::visual_engine = found.visual_engine;
	game::datamodel = rbx::instance_t{ found.datamodel };
	game::players = players_service;

	game::workspace = rbx::instance_t{};

	game::current_job_id = 0;

	game::session++;
	game::game_changed = false;

	LOG_INFO("game found: visual engine 0x{:X}, datamodel 0x{:X}, players 0x{:X}", found.visual_engine, found.datamodel, players_service.address);
	return true;
}

void game::detach()
{
	game::game_changed = true;

	game::visual_engine = 0;
	game::datamodel = rbx::instance_t{};
	game::players = rbx::instance_t{};
	game::workspace = rbx::instance_t{};
	game::current_job_id = 0;
}

namespace antifling
{
	void run();

	float top_speed();

	inline std::atomic<std::uint64_t> root_primitive{ 0 };
	inline std::atomic<std::uint64_t> character_model{ 0 };

	inline std::atomic<int> throws_stopped{ 0 };
	inline std::atomic<int> parts_kept_off{ 0 };
	inline std::atomic<std::uint64_t> last_slowed{ 0 };
}


namespace
{
	using steady = std::chrono::steady_clock;

	constexpr steady::time_point never{};

	struct pv_t
	{
		math::matrix3 rotation;
		math::vector3 position;
		math::vector3 linear;
		math::vector3 angular;
	};

	static_assert(sizeof(pv_t) == 72, "pv_t is read from and written to the game in one piece");

	constexpr bool one_block =
		Offsets::Primitive::Position == Offsets::Primitive::Rotation + sizeof(math::matrix3) &&
		Offsets::Primitive::AssemblyLinearVelocity == Offsets::Primitive::Position + sizeof(math::vector3) &&
		Offsets::Primitive::AssemblyAngularVelocity == Offsets::Primitive::AssemblyLinearVelocity + sizeof(math::vector3);

	pv_t read_pv(std::uint64_t primitive)
	{
		pv_t pv{};

		if (one_block)
		{
			memory->read_buffer(primitive + Offsets::Primitive::Rotation, &pv, sizeof(pv));
		}
		else
		{
			pv.rotation = memory->read<math::matrix3>(primitive + Offsets::Primitive::Rotation);
			pv.position = memory->read<math::vector3>(primitive + Offsets::Primitive::Position);
			pv.linear = memory->read<math::vector3>(primitive + Offsets::Primitive::AssemblyLinearVelocity);
			pv.angular = memory->read<math::vector3>(primitive + Offsets::Primitive::AssemblyAngularVelocity);
		}

		return pv;
	}

	void write_pv(std::uint64_t primitive, const pv_t& pv)
	{
		if (one_block)
		{
			memory->write_buffer(primitive + Offsets::Primitive::Rotation, &pv, sizeof(pv));
		}
		else
		{
			memory->write<math::matrix3>(primitive + Offsets::Primitive::Rotation, pv.rotation);
			memory->write<math::vector3>(primitive + Offsets::Primitive::Position, pv.position);
			memory->write<math::vector3>(primitive + Offsets::Primitive::AssemblyLinearVelocity, pv.linear);
			memory->write<math::vector3>(primitive + Offsets::Primitive::AssemblyAngularVelocity, pv.angular);
		}
	}

	struct motion_t
	{
		math::vector3 position;
		math::vector3 linear;
		math::vector3 angular;
	};

	motion_t read_motion(std::uint64_t primitive)
	{
		motion_t motion{};

		if (one_block)
		{
			memory->read_buffer(primitive + Offsets::Primitive::Position, &motion, sizeof(motion));
		}
		else
		{
			motion.position = memory->read<math::vector3>(primitive + Offsets::Primitive::Position);
			motion.linear = memory->read<math::vector3>(primitive + Offsets::Primitive::AssemblyLinearVelocity);
			motion.angular = memory->read<math::vector3>(primitive + Offsets::Primitive::AssemblyAngularVelocity);
		}

		return motion;
	}

	bool is_finite(const math::vector3& v)
	{
		return std::isfinite(v.x) && std::isfinite(v.y) && std::isfinite(v.z);
	}

	bool is_finite(const pv_t& pv)
	{
		for (const float value : pv.rotation.m[0]) if (!std::isfinite(value)) return false;
		for (const float value : pv.rotation.m[1]) if (!std::isfinite(value)) return false;
		for (const float value : pv.rotation.m[2]) if (!std::isfinite(value)) return false;

		return is_finite(pv.position) && is_finite(pv.linear) && is_finite(pv.angular);
	}

	constexpr float too_fast = 250.0f;
	constexpr auto calm_for = 3s;

	std::uint8_t collide_bit()
	{
		return static_cast<std::uint8_t>(Offsets::PrimitiveFlags::CanCollide);
	}

	bool is_alive(std::uint64_t primitive)
	{
		const std::uint64_t part = memory->read<std::uint64_t>(primitive + Offsets::Primitive::Owner);
		return part != 0 && memory->read<std::uint64_t>(part + Offsets::BasePart::Primitive) == primitive;
	}

	struct movable_t
	{
		std::uint64_t primitive = 0;
		float reach = 0.0f;
	};

	float surface_speed(const motion_t& motion, float reach)
	{
		const float speed = motion.linear.length() + motion.angular.length() * reach;
		return std::isfinite(speed) ? speed : 1.0e9f;
	}

	std::mutex movable_mutex;
	std::shared_ptr<const std::vector<movable_t>> movable_parts;

	std::shared_ptr<const std::vector<movable_t>> get_movable_parts()
	{
		std::lock_guard<std::mutex> lock(movable_mutex);
		return movable_parts;
	}

	void set_movable_parts(std::shared_ptr<const std::vector<movable_t>> parts)
	{
		std::lock_guard<std::mutex> lock(movable_mutex);
		movable_parts = std::move(parts);
	}

	void scan_once()
	{
		const std::uint64_t model = antifling::character_model;
		const std::uint64_t workspace = game::workspace.address;
		if (model == 0 || workspace == 0)
		{
			set_movable_parts(nullptr);
			return;
		}

		const std::uint64_t world = memory->read<std::uint64_t>(workspace + Offsets::Workspace::World);
		if (world == 0)
			return;

		const std::uint64_t first = memory->read<std::uint64_t>(world + Offsets::World::Primitives);
		const std::uint64_t last = memory->read<std::uint64_t>(world + Offsets::World::Primitives + sizeof(std::uint64_t));
		if (first == 0 || last <= first || (last - first) % 8 != 0 || last - first > 8ull * 4000000)
			return;

		std::vector<std::uint64_t> primitives((last - first) / 8);
		memory->read_buffer(first, primitives.data(), last - first);

		const std::uint8_t anchored = static_cast<std::uint8_t>(Offsets::PrimitiveFlags::Anchored);

		auto found = std::make_shared<std::vector<movable_t>>();
		for (const std::uint64_t primitive : primitives)
		{
			if (primitive == 0 || !g_running)
				continue;

			const std::uint8_t flags = memory->read<std::uint8_t>(primitive + Offsets::Primitive::Flags);
			if ((flags & anchored) != 0 || (flags & collide_bit()) == 0)
				continue;

			const std::uint64_t part = memory->read<std::uint64_t>(primitive + Offsets::Primitive::Owner);
			if (part == 0 || memory->read<std::uint64_t>(part + Offsets::BasePart::Primitive) != primitive)
				continue;

			const std::uint64_t parent = memory->read<std::uint64_t>(part + Offsets::Instance::Parent);
			if (parent == model || (parent != 0 && memory->read<std::uint64_t>(parent + Offsets::Instance::Parent) == model))
				continue;

			const math::vector3 size = memory->read<math::vector3>(primitive + Offsets::Primitive::Size);
			const float largest = (std::max)((std::max)(size.x, size.y), size.z);

			found->push_back({ primitive, std::isfinite(largest) ? std::clamp(largest * 0.5f, 0.0f, 50.0f) : 0.0f });
		}

		set_movable_parts(std::move(found));
	}

	void scan()
	{
		while (g_running)
		{
			if (antifling::root_primitive == 0 || game::game_changed)
			{
				set_movable_parts(nullptr);
				std::this_thread::sleep_for(100ms);
				continue;
			}

			const steady::time_point started = steady::now();
			scan_once();
			const steady::duration took = steady::now() - started;

			const steady::time_point next = steady::now() + (std::max)(steady::duration{ 1s }, took * 20);
			while (g_running && steady::now() < next && antifling::root_primitive != 0)
			{
				std::this_thread::sleep_for(50ms);
			}
		}
	}

	struct kept_off_t
	{
		movable_t part;
		steady::time_point last_too_fast = never;
	};

	std::vector<kept_off_t> kept_off;

	struct released_t
	{
		movable_t part;
		steady::time_point until = never;
	};

	std::vector<released_t> released;

	bool is_kept_off(std::uint64_t primitive)
	{
		for (const kept_off_t& kept : kept_off)
		{
			if (kept.part.primitive == primitive)
				return true;
		}
		return false;
	}

	void let_all_touch_again()
	{
		for (const kept_off_t& kept : kept_off)
		{
			if (!is_alive(kept.part.primitive))
				continue;

			const std::uint8_t flags = memory->read<std::uint8_t>(kept.part.primitive + Offsets::Primitive::Flags);
			memory->write<std::uint8_t>(kept.part.primitive + Offsets::Primitive::Flags, static_cast<std::uint8_t>(flags | collide_bit()));
		}

		kept_off.clear();
		released.clear();
		antifling::parts_kept_off = 0;
	}

	void keep_fast_parts_off()
	{
		const steady::time_point now = steady::now();

		int flying = 0;
		float fastest = 0.0f;

		auto watch = [&](const movable_t& part)
		{
			const motion_t motion = read_motion(part.primitive);
			if (surface_speed(motion, part.reach) <= too_fast || is_kept_off(part.primitive) || !is_alive(part.primitive))
				return;

			const std::uint8_t flags = memory->read<std::uint8_t>(part.primitive + Offsets::Primitive::Flags);
			if ((flags & collide_bit()) == 0 || (flags & static_cast<std::uint8_t>(Offsets::PrimitiveFlags::Anchored)) != 0)
				return;

			memory->write<std::uint8_t>(part.primitive + Offsets::Primitive::Flags, static_cast<std::uint8_t>(flags & ~collide_bit()));
			kept_off.push_back({ part, now });

			const float speed = motion.linear.length();
			if (!(speed <= too_fast))
			{
				flying++;
				fastest = (std::max)(fastest, std::isfinite(speed) ? speed : 1.0e9f);
			}
		};

		const std::shared_ptr<const std::vector<movable_t>> parts = get_movable_parts();
		if (parts)
		{
			for (const movable_t& part : *parts)
			{
				watch(part);
			}
		}

		for (std::size_t i = 0; i < released.size();)
		{
			if (now >= released[i].until)
			{
				released.erase(released.begin() + static_cast<std::ptrdiff_t>(i));
				continue;
			}

			watch(released[i].part);
			i++;
		}

		if (flying > 0)
		{
			LOG_EVERY(2000, logger::level::info, "anti fling: {} part{} moving at up to {:.0f} studs a second can no longer touch the character",
				flying, flying == 1 ? "" : "s", fastest);
		}

		for (std::size_t i = 0; i < kept_off.size();)
		{
			kept_off_t& kept = kept_off[i];

			if (!is_alive(kept.part.primitive))
			{
				kept_off.erase(kept_off.begin() + static_cast<std::ptrdiff_t>(i));
				continue;
			}

			const bool calm = surface_speed(read_motion(kept.part.primitive), kept.part.reach) < too_fast * 0.5f;
			const std::uint8_t flags = memory->read<std::uint8_t>(kept.part.primitive + Offsets::Primitive::Flags);

			if (!calm)
			{
				kept.last_too_fast = now;

				if ((flags & collide_bit()) != 0)
				{
					memory->write<std::uint8_t>(kept.part.primitive + Offsets::Primitive::Flags, static_cast<std::uint8_t>(flags & ~collide_bit()));
				}
			}
			else if (now - kept.last_too_fast >= calm_for)
			{
				memory->write<std::uint8_t>(kept.part.primitive + Offsets::Primitive::Flags, static_cast<std::uint8_t>(flags | collide_bit()));
				released.push_back({ kept.part, now + 5s });
				kept_off.erase(kept_off.begin() + static_cast<std::ptrdiff_t>(i));
				continue;
			}

			i++;
		}

		antifling::parts_kept_off = static_cast<int>(kept_off.size());
	}

	struct guard_t
	{
		std::uint64_t primitive = 0;

		bool has_good = false;
		pv_t good{};
		steady::time_point good_at = never;

		pv_t seen{};

		bool has_before_jump = false;
		pv_t before_jump{};
		steady::time_point jumped = never;

		steady::time_point undone_at = never;

		steady::time_point last_throw = never;
		steady::time_point attack_began = never;
		int undone = 0;
		float worst_speed = 0.0f;

		steady::time_point slowed_since = never;
		steady::time_point slowed_last = never;
		float pushed_to = 0.0f;
	};

	constexpr auto halves_apart = 1500us;

	constexpr float spin_of_a_throw = 200.0f;

	float over_ground(const math::vector3& velocity)
	{
		return std::hypot(velocity.x, velocity.z);
	}

	constexpr float a_hair = 1.02f;

	bool cut_to(math::vector3& velocity, float top)
	{
		bool cut = false;

		const float sideways = over_ground(velocity);
		if (sideways > top * a_hair)
		{
			velocity.x *= top / sideways;
			velocity.z *= top / sideways;
			cut = true;
		}

		if (velocity.y > top * a_hair)
		{
			velocity.y = top;
			cut = true;
		}

		return cut;
	}

	bool is_thrown(float before, float after, float top)
	{
		return after > top * 2.0f && after - before > top;
	}

	bool is_throw(const pv_t& before, const pv_t& after, float top)
	{
		if (!is_finite(after))
			return true;

		if (is_thrown(over_ground(before.linear), over_ground(after.linear), top) ||
			is_thrown(std::abs(before.linear.y), std::abs(after.linear.y), top))
			return true;

		const float spin = after.angular.length();
		return spin > spin_of_a_throw && spin - before.angular.length() > spin_of_a_throw * 0.5f;
	}

	bool is_jump(const pv_t& before, const pv_t& after, steady::duration elapsed)
	{
		const float seconds = (std::min)(std::chrono::duration<float>(elapsed).count(), 0.05f);
		const float fastest = (std::max)(before.linear.length(), after.linear.length());
		return after.position.distance(before.position) > fastest * seconds * 3.0f + 0.5f;
	}

	void slow_down(guard_t& guard, pv_t& now, float top, steady::time_point when)
	{
		const float speed = (std::max)(over_ground(now.linear), now.linear.y);
		if (!cut_to(now.linear, top))
			return;

		memory->write<math::vector3>(guard.primitive + Offsets::Primitive::AssemblyLinearVelocity, now.linear);

		if (guard.slowed_since == never)
		{
			guard.slowed_since = when;
		}
		guard.slowed_last = when;
		guard.pushed_to = (std::max)(guard.pushed_to, speed);
		antifling::last_slowed = GetTickCount64();
	}

	bool look(guard_t& guard)
	{
		pv_t now = read_pv(guard.primitive);
		const steady::time_point when = steady::now();
		const float top = antifling::top_speed();

		if (!guard.has_good)
		{
			if (is_finite(now))
			{
				slow_down(guard, now, top, when);
				guard.good = now;
				guard.good_at = when;
				guard.seen = now;
				guard.has_good = true;
			}
			return false;
		}

		if (is_throw(guard.good, now, top))
		{
			if (guard.has_before_jump && when - guard.jumped < halves_apart)
			{
				guard.good = guard.before_jump;
			}
			guard.has_before_jump = false;

			cut_to(guard.good.linear, top);
			write_pv(guard.primitive, guard.good);
			guard.seen = guard.good;

			const float speed = now.linear.length();
			guard.worst_speed = (std::max)(guard.worst_speed, std::isfinite(speed) ? speed : 1.0e9f);
			guard.undone++;
			guard.undone_at = when;
			guard.last_throw = when;
			antifling::throws_stopped++;
			return true;
		}

		slow_down(guard, now, top, when);

		const bool settled = std::memcmp(&now, &guard.seen, sizeof(pv_t)) == 0;
		guard.seen = now;
		if (!settled && when - guard.good_at < 20ms)
			return false;

		if (is_jump(guard.good, now, when - guard.good_at))
		{
			if (guard.undone_at != never && when - guard.undone_at < halves_apart)
			{
				write_pv(guard.primitive, guard.good);
				guard.seen = guard.good;
				return false;
			}

			if (!guard.has_before_jump || when - guard.jumped >= halves_apart)
			{
				guard.before_jump = guard.good;
				guard.has_before_jump = true;
				guard.jumped = when;
			}
		}
		else if (guard.has_before_jump && when - guard.jumped >= halves_apart)
		{
			guard.has_before_jump = false;
		}

		guard.good = now;
		guard.good_at = when;
		return false;
	}

	bool is_pushed(const guard_t& guard, steady::time_point now)
	{
		return guard.slowed_last != never && now - guard.slowed_last < 100ms && guard.pushed_to > antifling::top_speed() * 1.25f;
	}

	void report_slowing(guard_t& guard, steady::time_point now)
	{
		if (guard.slowed_since == never || now - guard.slowed_last < 1s)
			return;

		const float top = antifling::top_speed();
		if (guard.pushed_to > top * 1.25f)
		{
			LOG_INFO("anti fling: the character was held to {:.0f} studs a second for {:.1f} s, something was pushing it to as much as {:.0f}",
				top, std::chrono::duration<double>(guard.slowed_last - guard.slowed_since).count(), guard.pushed_to);
		}

		guard.slowed_since = never;
		guard.slowed_last = never;
		guard.pushed_to = 0.0f;
	}

	constexpr auto stay_for = 2s;

	void defend(guard_t& guard)
	{
		guard.attack_began = guard.last_throw;

		steady::time_point last_pass = never;
		for (;;)
		{
			look(guard);

			const steady::time_point now = steady::now();
			if (now - guard.last_throw >= stay_for || !g_running || antifling::root_primitive != guard.primitive)
				break;

			if (now - last_pass >= 4ms)
			{
				last_pass = now;
				keep_fast_parts_off();
			}
		}

		LOG_INFO("anti fling: {} throw{} undone over {:.1f} s, the hardest one would have had the character at {:.0f} studs a second; {} part{} kept from touching it",
			guard.undone, guard.undone == 1 ? "" : "s", std::chrono::duration<double>(guard.last_throw - guard.attack_began).count(),
			guard.worst_speed, kept_off.size(), kept_off.size() == 1 ? "" : "s");

		guard.undone = 0;
		guard.worst_speed = 0.0f;
		guard.last_throw = never;
	}
}

float antifling::top_speed()
{
	return std::clamp(settings::movement::antifling::max_speed, settings::movement::antifling::max_speed_min, settings::movement::antifling::max_speed_max);
}

void antifling::run()
{
	std::thread scanner(scan);

	SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_HIGHEST);

	precise_sleeper sleeper;
	guard_t guard;
	steady::time_point last_pass = never;

	while (g_running)
	{
		const std::uint64_t primitive = root_primitive.load();

		if (primitive == 0)
		{
			if (!kept_off.empty())
			{
				let_all_touch_again();
			}

			guard = {};
			std::this_thread::sleep_for(20ms);
			continue;
		}

		if (primitive != guard.primitive)
		{
			guard = {};
			guard.primitive = primitive;
		}

		if (look(guard))
		{
			defend(guard);
			continue;
		}

		const steady::time_point now = steady::now();
		report_slowing(guard, now);

		if (now - last_pass >= 4ms)
		{
			last_pass = now;
			keep_fast_parts_off();
		}

		if (is_pushed(guard, now))
			continue;

		sleeper.sleep_ms(0.5);
	}

	let_all_touch_again();
	scanner.join();
}

namespace
{
	enum class guard_state
	{
		no_game,
		no_character,
		seated,
		dead,
		guarding
	};

	std::atomic<guard_state> current_state{ guard_state::no_game };

	const char* describe(guard_state state)
	{
		switch (state)
		{
		case guard_state::no_game: return "no game to guard in";
		case guard_state::no_character: return "no character yet";
		case guard_state::seated: return "sitting, left alone";
		case guard_state::dead: return "dead, left alone";
		default: return "guarding";
		}
	}

	struct local_character_t
	{
		std::uint64_t model = 0;
		std::uint64_t root = 0;
		std::uint64_t humanoid = 0;
	};

	local_character_t get_local_character(std::uint64_t local_player)
	{
		static local_character_t character;
		static steady::time_point last_lookup{};

		std::uint64_t model = 0;
		if (local_player != 0)
		{
			model = memory->read<std::uint64_t>(local_player + Offsets::Player::ModelInstance);
		}

		if (model == 0)
		{
			character = {};
			return character;
		}

		const bool complete = character.root != 0 && character.humanoid != 0;
		auto now = steady::now();

		if (model != character.model || (!complete && now - last_lookup >= 250ms))
		{
			last_lookup = now;
			character = {};
			character.model = model;

			for (const rbx::instance_t& child : rbx::instance_t{ model }.get_children())
			{
				if (character.humanoid == 0 && child.get_class_name() == "Humanoid")
				{
					character.humanoid = child.address;
				}
				else if (character.root == 0 && child.get_name() == "HumanoidRootPart")
				{
					character.root = child.address;
				}
			}
		}

		return character;
	}

	void publish_character(const local_character_t& character)
	{
		std::uint64_t primitive = 0;
		guard_state state = guard_state::no_character;

		if (character.root != 0 && character.humanoid != 0)
		{
			const bool seated = memory->read<std::uint64_t>(character.humanoid + Offsets::Humanoid::SeatPart) != 0;
			const float health = memory->read<float>(character.humanoid + Offsets::Humanoid::Health);

			if (seated)
			{
				state = guard_state::seated;
			}
			else if (!(health > 0.0f))
			{
				state = guard_state::dead;
			}
			else
			{
				primitive = memory->read<std::uint64_t>(character.root + Offsets::BasePart::Primitive);
				state = primitive != 0 ? guard_state::guarding : guard_state::no_character;
			}
		}

		antifling::character_model = primitive != 0 ? character.model : 0;
		antifling::root_primitive = primitive;
		current_state = state;
	}

	local_character_t follow_character()
	{
		static std::uint32_t known_session = 0;
		static steady::time_point last_workspace_lookup{};
		static guard_state logged_state = guard_state::no_game;

		const std::uint64_t players = game::players.address;
		local_character_t character;

		if (game::game_changed || players == 0)
		{
			get_local_character(0);
			antifling::character_model = 0;
			antifling::root_primitive = 0;
			current_state = guard_state::no_game;
		}
		else
		{
			const std::uint32_t session = game::session;
			const steady::time_point now = steady::now();
			const auto workspace_every = game::workspace.address == 0 ? 50ms : 250ms;
			if (session != known_session || now - last_workspace_lookup >= workspace_every)
			{
				known_session = session;
				last_workspace_lookup = now;

				const rbx::instance_t workspace = game::datamodel.find_first_child_by_class("Workspace");
				if (workspace.address != 0)
				{
					game::workspace = workspace;
				}
			}

			const std::uint64_t local_player = memory->read<std::uint64_t>(players + Offsets::Player::LocalPlayer);
			character = get_local_character(local_player);
			publish_character(character);
		}

		const guard_state state = current_state;
		if (state != logged_state)
		{
			logged_state = state;
			LOG_INFO("character: {}", describe(state));
		}

		return character;
	}

	void track()
	{
		precise_sleeper sleeper;

		while (g_running)
		{
			follow_character();

			sleeper.sleep_ms(current_state == guard_state::no_game ? 100.0 : 1.0);
		}

		antifling::character_model = 0;
		antifling::root_primitive = 0;
	}

	bool attached = false;
	const char* attached_name = "RobloxPlayerBeta.exe";
	bool force = false;

	std::string roblox_version_of(HANDLE process)
	{
		wchar_t path[1024]{};
		DWORD size = static_cast<DWORD>(std::size(path));
		if (!QueryFullProcessImageNameW(process, 0, path, &size))
			return {};

		const std::wstring full(path, size);
		const std::size_t file = full.find_last_of(L"\\/");
		if (file == std::wstring::npos || file == 0)
			return {};

		const std::size_t folder_end = full.find_last_of(L"\\/", file - 1);
		const std::size_t begin = folder_end == std::wstring::npos ? 0 : folder_end + 1;
		const std::wstring folder = full.substr(begin, file - begin);

		if (folder.rfind(L"version-", 0) != 0)
			return {};

		std::string version;
		for (const wchar_t character : folder)
		{
			version.push_back(character < 0x80 ? static_cast<char>(character) : '?');
		}
		return version;
	}

	bool initialize_memory(bool retrying)
	{
		const logger::level problem = retrying ? logger::level::debug : logger::level::error;

		const char* process_name = "RobloxPlayerBeta.exe";
		if (!memory->attach_to_process(process_name))
		{
			process_name = "RobloxStudioBeta.exe";
			if (!memory->attach_to_process(process_name))
			{
				LOG_AT(problem, "could not attach to RobloxPlayerBeta.exe or RobloxStudioBeta.exe");
				return false;
			}
		}

		const std::uint64_t module_base = memory->find_module_address(process_name);
		if (module_base == 0)
		{
			LOG_AT(problem, "module {} not found in pid {}", process_name, memory->get_process_id());
			return false;
		}

		const std::string version = roblox_version_of(memory->get_process_handle());
		if (!version.empty() && version != Offsets::ClientVersion && !force)
		{
			LOG_EVERY(30000, logger::level::error, "{} is {}, but the offsets in Offsets.hpp are for {}. Writing to a game they do not fit can crash it, so nothing is guarded. Update Offsets.hpp and build again, or start this with --force.",
				process_name, version, Offsets::ClientVersion);
			return false;
		}

		if (!game::reinitialize_game_state())
		{
			LOG_AT(problem, "{} is running (pid {}) but has no game loaded yet", process_name, memory->get_process_id());
			return false;
		}

		attached_name = process_name;

		LOG_INFO("attached to {} (pid {}), module base 0x{:X}, size 0x{:X}", process_name, memory->get_process_id(), module_base, memory->get_module_size());
		return true;
	}

	bool roblox_is_gone()
	{
		if (memory->process_ended())
			return true;

		static steady::time_point last_look{};
		const auto now = steady::now();
		if (now - last_look < 3s)
			return false;
		last_look = now;

		const std::uint32_t current = memory->get_process_id();
		if (memory_t::has_visible_window(current))
			return false;

		const std::uint32_t other = memory->find_process_id(attached_name);
		return other != 0 && other != current && memory_t::has_visible_window(other);
	}

	void keep_up_with_roblox()
	{
		if (!attached)
		{
			attached = initialize_memory(true);
		}
		else if (roblox_is_gone())
		{
			LOG_INFO("Roblox is gone, waiting for it to come back");
			attached = false;
			game::detach();
			memory->detach();
		}
		else
		{
			if (game::check_game_changed())
			{
				LOG_INFO("the game changed, looking for the new one");
			}

			if (game::game_changed && !game::reinitialize_game_state())
			{
				LOG_EVERY(30000, logger::level::info, "the new game has not loaded yet, still looking");
			}
		}
	}

	void log_status()
	{
		const guard_state state = current_state;

		if (state != guard_state::guarding)
		{
			LOG_INFO("status: {}", describe(state));
			return;
		}

		const int stopped = antifling::throws_stopped;
		const int kept_parts = antifling::parts_kept_off;
		LOG_INFO("status: guarding the character, top speed {:.0f}; {} throw{} undone, {} part{} kept from touching it",
			antifling::top_speed(), stopped, stopped == 1 ? "" : "s", kept_parts, kept_parts == 1 ? "" : "s");
	}

	int run_check()
	{
		if (!initialize_memory(false))
		{
			std::printf("\nnothing to look at: Roblox is not running, has no game loaded, or does not fit the offsets\n");
			return 1;
		}

		const std::string version = roblox_version_of(memory->get_process_handle());
		std::printf("\nRoblox:     pid %u, %s (the offsets are for %s%s)\n", memory->get_process_id(),
			version.empty() ? "version unknown" : version.c_str(), Offsets::ClientVersion.c_str(),
			version.empty() ? "" : version == Offsets::ClientVersion ? ", a match" : ", NOT a match");
		std::printf("datamodel:  0x%llX, players 0x%llX\n", game::datamodel.address, game::players.address);

		const local_character_t character = follow_character();
		std::printf("workspace:  0x%llX\n", game::workspace.address);
		std::printf("character:  model 0x%llX, humanoid 0x%llX, root part 0x%llX, root primitive 0x%llX\n",
			character.model, character.humanoid, character.root, antifling::root_primitive.load());

		if (character.humanoid != 0)
		{
			const bool seated = memory->read<std::uint64_t>(character.humanoid + Offsets::Humanoid::SeatPart) != 0;
			std::printf("humanoid:   health %.0f, %s\n", memory->read<float>(character.humanoid + Offsets::Humanoid::Health), seated ? "sitting" : "not sitting");
		}

		std::printf("the guard:  %s\n", describe(current_state));

		const std::uint64_t primitive = character.root != 0 ? memory->read<std::uint64_t>(character.root + Offsets::BasePart::Primitive) : 0;
		if (primitive != 0)
		{
			const pv_t pv = read_pv(primitive);
			std::printf("root part:  at (%.1f, %.1f, %.1f), moving at %.1f studs a second, spinning at %.1f radians a second\n",
				pv.position.x, pv.position.y, pv.position.z, pv.linear.length(), pv.angular.length());
		}

		if (character.model != 0)
		{
			antifling::character_model = character.model;
			const steady::time_point started = steady::now();
			scan_once();
			const double took = std::chrono::duration<double, std::milli>(steady::now() - started).count();
			const auto parts = get_movable_parts();
			std::printf("the world:  %zu parts can move and collide and are not the character's own (looked through in %.1f ms)\n", parts ? parts->size() : 0, took);
		}

		std::printf("\nNothing was written.\n");
		return current_state == guard_state::guarding ? 0 : 2;
	}

	HANDLE finished = nullptr;

	BOOL WINAPI console_event(DWORD type)
	{
		switch (type)
		{
		case CTRL_C_EVENT:
		case CTRL_BREAK_EVENT:
		case CTRL_CLOSE_EVENT:
		case CTRL_LOGOFF_EVENT:
		case CTRL_SHUTDOWN_EVENT:
			g_running = false;

			if (finished != nullptr)
			{
				WaitForSingleObject(finished, 4000);
			}
			return TRUE;
		default:
			return FALSE;
		}
	}

	void print_usage()
	{
		std::printf(
			"Anti fling, on its own.\n"
			"\n"
			"  antifling_standalone.exe [top speed] [--check] [--verbose] [--force]\n"
			"\n"
			"  top speed   studs a second, %.0f to %.0f, %.0f when left out\n"
			"  --check     look at Roblox and the character once and say what was found, write nothing\n"
			"  --verbose   print the debug lines as well\n"
			"  --force     guard even when Roblox is not the version the offsets are for\n"
			"\n"
			"Ctrl+C or closing the window stops it.\n",
			settings::movement::antifling::max_speed_min, settings::movement::antifling::max_speed_max, settings::movement::antifling::max_speed);
	}

	struct options_t
	{
		bool check = false;
		bool verbose = false;
	};

	int parse_arguments(int argc, char** argv, options_t& options)
	{
		for (int i = 1; i < argc; i++)
		{
			const std::string_view argument = argv[i];

			if (argument == "--check")
			{
				options.check = true;
			}
			else if (argument == "--verbose" || argument == "-v")
			{
				options.verbose = true;
			}
			else if (argument == "--force")
			{
				force = true;
			}
			else if (argument == "--help" || argument == "-h" || argument == "/?")
			{
				print_usage();
				return 0;
			}
			else
			{
				char* end = nullptr;
				const float value = std::strtof(argv[i], &end);
				if (end == argv[i] || *end != '\0' || !std::isfinite(value))
				{
					std::printf("not understood: %s\n\n", argv[i]);
					print_usage();
					return 2;
				}

				settings::movement::antifling::max_speed = value;
			}
		}

		return -1;
	}
}

int main(int argc, char** argv)
{
	options_t options;
	const int exit_code = parse_arguments(argc, argv, options);
	if (exit_code >= 0)
		return exit_code;

	if (options.verbose)
	{
		logger::minimum = logger::level::debug;
	}

	SetConsoleTitleA("Anti fling");

	const float asked = settings::movement::antifling::max_speed;
	if (antifling::top_speed() != asked)
	{
		LOG_WARN("a top speed of {:.0f} is outside {:.0f} to {:.0f}, {:.0f} is used", asked,
			settings::movement::antifling::max_speed_min, settings::movement::antifling::max_speed_max, antifling::top_speed());
	}

	if (options.check)
	{
		return run_check();
	}

	LOG_INFO("anti fling, top speed {:.0f} studs a second. Ctrl+C or closing this window stops it.", antifling::top_speed());

	finished = CreateEventW(nullptr, TRUE, FALSE, nullptr);
	SetConsoleCtrlHandler(console_event, TRUE);

	attached = initialize_memory(false);
	if (!attached)
	{
		game::detach();
		LOG_WARN("no game to attach to yet, waiting for Roblox");
	}

	std::thread tracker(track);
	std::thread guard(antifling::run);

	steady::time_point last_check = steady::now();
	steady::time_point last_status = steady::now();

	while (g_running)
	{
		std::this_thread::sleep_for(50ms);

		const steady::time_point now = steady::now();

		if (now - last_check >= (attached ? 500ms : 2000ms))
		{
			last_check = now;
			keep_up_with_roblox();
		}

		if (now - last_status >= 10s)
		{
			last_status = now;
			log_status();
		}
	}

	tracker.join();
	guard.join();

	LOG_INFO("stopped, {} throw{} undone", antifling::throws_stopped.load(), antifling::throws_stopped == 1 ? "" : "s");

	SetEvent(finished);
	return 0;
}
