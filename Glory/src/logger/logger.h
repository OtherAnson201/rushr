#pragma once
#include <atomic>
#include <cstdint>
#include <format>
#include <string>
#include <utility>
#include <vector>

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

	void initialize();

	bool should_log(level lvl);
	void write(level lvl, const site_t& site, std::string message);

	void post(level lvl, const site_t& site, std::string message);

	template <typename... Args>
	void log(level lvl, const site_t& site, std::format_string<Args...> fmt, Args&&... args)
	{
		write(lvl, site, std::format(fmt, std::forward<Args>(args)...));
	}

	template <typename... Args>
	void log_posted(level lvl, const site_t& site, std::format_string<Args...> fmt, Args&&... args)
	{
		post(lvl, site, std::format(fmt, std::forward<Args>(args)...));
	}

	bool throttle(std::atomic<std::uint64_t>& last_tick, std::uint64_t interval_ms);

	void hold_console();

	void name_thread(const char* name);

	const char* level_name(level lvl);

	std::string get_log_directory();
	std::string get_log_path();
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

#define LOG_POST(lvl, ...) \
	do { \
		if (::logger::should_log(lvl)) \
			::logger::log_posted(lvl, ::logger::site_t{ __FILE__, static_cast<std::uint32_t>(__LINE__), __FUNCTION__ }, __VA_ARGS__); \
	} while (0)

#define LOG_POST_EVERY(interval_ms, lvl, ...) \
	do { \
		static std::atomic<std::uint64_t> log_last_tick{ 0 }; \
		if (::logger::should_log(lvl) && ::logger::throttle(log_last_tick, interval_ms)) \
			::logger::log_posted(lvl, ::logger::site_t{ __FILE__, static_cast<std::uint32_t>(__LINE__), __FUNCTION__ }, __VA_ARGS__); \
	} while (0)
