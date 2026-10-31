#include "logger.h"

#include <windows.h>
#include <dbghelp.h>

#include <algorithm>
#include <condition_variable>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <exception>
#include <filesystem>
#include <mutex>
#include <thread>

#include <render/assets/settings.h>

#pragma comment(lib, "dbghelp.lib")

namespace
{
	constexpr std::size_t max_log_files = 10;
	constexpr std::size_t max_dump_files = 5;
	constexpr int max_stack_frames = 48;
	constexpr int level_count = 6;

	std::mutex g_mutex;

	HANDLE g_file = INVALID_HANDLE_VALUE;
	bool g_file_failed = false;
	std::string g_file_path;

	thread_local std::string t_thread_name;

	LPTOP_LEVEL_EXCEPTION_FILTER g_previous_filter = nullptr;
	std::atomic<DWORD> g_crashing_thread{ 0 };
	bool g_symbols_ready = false;

	DWORD g_original_input_mode = 0;
	bool g_input_mode_saved = false;

	// the filename part of a path
	const char* base_name(const char* path)
	{
		const char* name = path;
		for (const char* c = path; *c; ++c)
		{
			if (*c == '\\' || *c == '/')
				name = c + 1;
		}
		return name;
	}

	// a timestamp string for this run
	const std::string& session_stamp()
	{
		static const std::string stamp = []
		{
			SYSTEMTIME st{};
			GetLocalTime(&st);
			return std::format("{:04}-{:02}-{:02}_{:02}-{:02}-{:02}_{}", st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond, GetCurrentProcessId());
		}();
		return stamp;
	}

	// delete old log files, keeping the newest
	void prune_old_files(const std::string& directory, const char* extension, std::size_t keep)
	{
		try
		{
			std::vector<std::filesystem::path> files;
			for (const auto& entry : std::filesystem::directory_iterator(directory))
			{
				if (entry.path().extension() == extension)
					files.push_back(entry.path());
			}

			std::sort(files.begin(), files.end());

			std::error_code ec;
			for (std::size_t i = 0; i + keep < files.size(); ++i)
				std::filesystem::remove(files[i], ec);
		}
		catch (...) {}
	}

	// open this runs log file
	void open_file()
	{
		std::string directory = logger::get_log_directory();
		prune_old_files(directory, ".log", max_log_files - 1);

		g_file_path = directory + "glory_" + session_stamp() + ".log";
		g_file = CreateFileA(g_file_path.c_str(), FILE_APPEND_DATA, FILE_SHARE_READ, nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
		if (g_file == INVALID_HANDLE_VALUE)
		{
			g_file_failed = true;
			g_file_path.clear();
		}
	}

	// print one colored line to the console
	void write_console(logger::level lvl, const std::string& text)
	{
		HANDLE console = GetStdHandle(STD_OUTPUT_HANDLE);
		if (!console || console == INVALID_HANDLE_VALUE)
			return;

		static constexpr WORD colours[level_count] = {
			FOREGROUND_INTENSITY,
			FOREGROUND_GREEN | FOREGROUND_BLUE | FOREGROUND_INTENSITY,
			FOREGROUND_GREEN | FOREGROUND_INTENSITY,
			FOREGROUND_RED | FOREGROUND_GREEN | FOREGROUND_INTENSITY,
			FOREGROUND_RED | FOREGROUND_INTENSITY,
			BACKGROUND_RED | FOREGROUND_RED | FOREGROUND_GREEN | FOREGROUND_BLUE | FOREGROUND_INTENSITY,
		};

		CONSOLE_SCREEN_BUFFER_INFO info{};
		bool is_console = GetConsoleScreenBufferInfo(console, &info) != 0;

		if (is_console)
			SetConsoleTextAttribute(console, colours[static_cast<int>(lvl)]);

		DWORD written = 0;
		WriteFile(console, text.data(), static_cast<DWORD>(text.size()), &written, nullptr);

		if (is_console)
			SetConsoleTextAttribute(console, info.wAttributes);
	}

	// format a log line with level and location
	std::string make_line(logger::level lvl, const logger::site_t& site, const std::string& message)
	{
		SYSTEMTIME st{};
		GetLocalTime(&st);

		const std::string thread = t_thread_name.empty() ? std::to_string(GetCurrentThreadId()) : t_thread_name;

		return std::format("{:04}-{:02}-{:02} {:02}:{:02}:{:02}.{:03} [{:<5}] [{}] {}:{} {} | {}\r\n",
			st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond, st.wMilliseconds,
			logger::level_name(lvl), thread, base_name(site.file), site.line, site.function, message);
	}

	// write a line to the file and console
	void write_line(logger::level lvl, const std::string& text)
	{
		if (settings::logging::to_file)
		{
			if (g_file == INVALID_HANDLE_VALUE && !g_file_failed)
				open_file();

			if (g_file != INVALID_HANDLE_VALUE)
			{
				DWORD written = 0;
				WriteFile(g_file, text.data(), static_cast<DWORD>(text.size()), &written, nullptr);
			}
		}

		if (settings::logging::to_console)
			write_console(lvl, text);
	}

	struct posted_t
	{
		logger::level lvl;
		std::string text;
	};

	struct post_office_t
	{
		std::mutex mutex;
		std::condition_variable wake;
		std::vector<posted_t> waiting;

		std::size_t turned_away = 0;
		bool started = false;
	};

	constexpr std::size_t max_waiting = 4096;

	// the shared queue of posted log lines
	post_office_t& post_office()
	{
		static post_office_t* const office = new post_office_t;
		return *office;
	}

	// flush the queued log lines
	void write_posted()
	{
		logger::name_thread("log");

		post_office_t& office = post_office();
		std::vector<posted_t> lines;

		for (;;)
		{
			std::size_t turned_away = 0;
			{
				std::unique_lock<std::mutex> lock(office.mutex);
				office.wake.wait(lock, [&office] { return !office.waiting.empty(); });
				lines.swap(office.waiting);
				turned_away = office.turned_away;
				office.turned_away = 0;
			}

			{
				std::lock_guard<std::mutex> lock(g_mutex);
				for (const posted_t& line : lines)
				{
					write_line(line.lvl, line.text);
				}
			}
			lines.clear();

			if (turned_away > 0)
			{
				LOG_WARN("{} lines were not logged, the log could not be written fast enough", turned_away);
			}
		}
	}

	// write text to the log as-is
	void write_raw(const char* text)
	{
		DWORD length = static_cast<DWORD>(std::strlen(text));
		DWORD written = 0;

		if (g_file != INVALID_HANDLE_VALUE)
			WriteFile(g_file, text, length, &written, nullptr);

		HANDLE console = GetStdHandle(STD_OUTPUT_HANDLE);
		if (settings::logging::to_console && console && console != INVALID_HANDLE_VALUE)
			WriteFile(console, text, length, &written, nullptr);
	}

	// printf-style write to the log
	void write_raw_format(const char* format, ...)
	{
		char buffer[2048];

		va_list args;
		va_start(args, format);
		std::vsnprintf(buffer, sizeof(buffer), format, args);
		va_end(args);

		write_raw(buffer);
	}

	// readable name for an exception code
	const char* exception_name(DWORD code)
	{
		switch (code)
		{
		case EXCEPTION_ACCESS_VIOLATION:         return "ACCESS_VIOLATION";
		case EXCEPTION_ARRAY_BOUNDS_EXCEEDED:    return "ARRAY_BOUNDS_EXCEEDED";
		case EXCEPTION_BREAKPOINT:               return "BREAKPOINT";
		case EXCEPTION_DATATYPE_MISALIGNMENT:    return "DATATYPE_MISALIGNMENT";
		case EXCEPTION_FLT_DIVIDE_BY_ZERO:       return "FLT_DIVIDE_BY_ZERO";
		case EXCEPTION_FLT_INVALID_OPERATION:    return "FLT_INVALID_OPERATION";
		case EXCEPTION_FLT_OVERFLOW:             return "FLT_OVERFLOW";
		case EXCEPTION_ILLEGAL_INSTRUCTION:      return "ILLEGAL_INSTRUCTION";
		case EXCEPTION_IN_PAGE_ERROR:            return "IN_PAGE_ERROR";
		case EXCEPTION_INT_DIVIDE_BY_ZERO:       return "INT_DIVIDE_BY_ZERO";
		case EXCEPTION_INT_OVERFLOW:             return "INT_OVERFLOW";
		case EXCEPTION_INVALID_HANDLE:           return "INVALID_HANDLE";
		case EXCEPTION_NONCONTINUABLE_EXCEPTION: return "NONCONTINUABLE_EXCEPTION";
		case EXCEPTION_PRIV_INSTRUCTION:         return "PRIV_INSTRUCTION";
		case EXCEPTION_STACK_OVERFLOW:           return "STACK_OVERFLOW";
		case 0xC0000374:                         return "HEAP_CORRUPTION";
		case 0xC0000409:                         return "STACK_BUFFER_OVERRUN";
		case 0xE06D7363:                         return "C++ exception";
		default:                                 return "unknown";
		}
	}

	// turn an address into module+symbol text
	void describe_address(DWORD64 address, bool is_return_address, char* buffer, std::size_t size)
	{
		char module_name[MAX_PATH] = "???";
		DWORD64 module_offset = address;

		HMODULE module = nullptr;
		if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, reinterpret_cast<LPCSTR>(address), &module) && module)
		{
			char module_path[MAX_PATH]{};
			if (GetModuleFileNameA(module, module_path, MAX_PATH))
				strncpy_s(module_name, base_name(module_path), _TRUNCATE);

			module_offset = address - reinterpret_cast<DWORD64>(module);
		}

		int length = std::snprintf(buffer, size, "%s+0x%llX", module_name, module_offset);
		if (!g_symbols_ready || length < 0 || static_cast<std::size_t>(length) >= size)
			return;

		DWORD64 lookup = is_return_address ? address - 1 : address;
		HANDLE process = GetCurrentProcess();

		alignas(SYMBOL_INFO) char symbol_buffer[sizeof(SYMBOL_INFO) + MAX_SYM_NAME]{};
		SYMBOL_INFO* symbol = reinterpret_cast<SYMBOL_INFO*>(symbol_buffer);
		symbol->SizeOfStruct = sizeof(SYMBOL_INFO);
		symbol->MaxNameLen = MAX_SYM_NAME;

		DWORD64 symbol_displacement = 0;
		if (!SymFromAddr(process, lookup, &symbol_displacement, symbol))
			return;

		length += std::snprintf(buffer + length, size - length, " %s+0x%llX", symbol->Name, symbol_displacement);
		if (static_cast<std::size_t>(length) >= size)
			return;

		IMAGEHLP_LINE64 source_line{};
		source_line.SizeOfStruct = sizeof(source_line);

		DWORD line_displacement = 0;
		if (SymGetLineFromAddr64(process, lookup, &line_displacement, &source_line))
			std::snprintf(buffer + length, size - length, " (%s:%lu)", base_name(source_line.FileName), source_line.LineNumber);
	}

	// walk the stack collecting return addresses
	int capture_frames(const CONTEXT* start, DWORD64* frames, int max_frames)
	{
		CONTEXT context = *start;
		int count = 0;

		__try
		{
			while (count < max_frames && context.Rip != 0)
			{
				frames[count++] = context.Rip;

				DWORD64 image_base = 0;
				PRUNTIME_FUNCTION function = RtlLookupFunctionEntry(context.Rip, &image_base, nullptr);
				if (!function)
				{

					context.Rip = *reinterpret_cast<DWORD64*>(context.Rsp);
					context.Rsp += sizeof(DWORD64);
				}
				else
				{
					void* handler_data = nullptr;
					DWORD64 establisher_frame = 0;
					RtlVirtualUnwind(UNW_FLAG_NHANDLER, image_base, context.Rip, function, &context, &handler_data, &establisher_frame, nullptr);
				}
			}
		}
		__except (EXCEPTION_EXECUTE_HANDLER) {}

		return count;
	}

	// write a stack trace to the log
	void write_stack_trace(const CONTEXT* context)
	{
		DWORD64 frames[max_stack_frames]{};
		int frame_count = capture_frames(context, frames, max_stack_frames);

		write_raw("stack trace:\r\n");
		for (int i = 0; i < frame_count; ++i)
		{
			char description[1024]{};
			describe_address(frames[i], i > 0, description, sizeof(description));
			write_raw_format("  #%02d 0x%016llX %s\r\n", i, frames[i], description);
		}
	}

	// write a crash minidump file
	void write_minidump(EXCEPTION_POINTERS* info)
	{
		std::string directory = logger::get_log_directory();
		prune_old_files(directory, ".dmp", max_dump_files - 1);

		std::string path = directory + "glory_" + session_stamp() + ".dmp";
		HANDLE file = CreateFileA(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
		if (file == INVALID_HANDLE_VALUE)
		{
			write_raw_format("minidump: could not create %s (error %lu)\r\n", path.c_str(), GetLastError());
			return;
		}

		MINIDUMP_EXCEPTION_INFORMATION exception_info{};
		exception_info.ThreadId = GetCurrentThreadId();
		exception_info.ExceptionPointers = info;
		exception_info.ClientPointers = FALSE;

		if (MiniDumpWriteDump(GetCurrentProcess(), GetCurrentProcessId(), file, MiniDumpNormal, &exception_info, nullptr, nullptr))
			write_raw_format("minidump: %s\r\n", path.c_str());
		else
			write_raw_format("minidump: MiniDumpWriteDump failed (error 0x%08lX)\r\n", GetLastError());

		CloseHandle(file);
	}

	enum class crash_claim
	{
		first,
		reentered,
	};

	// make sure only one thread reports the crash
	crash_claim claim_crash()
	{
		DWORD thread_id = GetCurrentThreadId();
		DWORD expected = 0;
		if (g_crashing_thread.compare_exchange_strong(expected, thread_id))
			return crash_claim::first;

		if (expected != thread_id)
			Sleep(INFINITE);

		return crash_claim::reentered;
	}

	// start writing a crash report
	void begin_crash_report(const char* title)
	{
		if (settings::logging::to_file && g_file == INVALID_HANDLE_VALUE && !g_file_failed)
			open_file();

		SymSetOptions(SYMOPT_LOAD_LINES | SYMOPT_UNDNAME | SYMOPT_DEFERRED_LOADS | SYMOPT_FAIL_CRITICAL_ERRORS | SYMOPT_NO_PROMPTS);
		g_symbols_ready = SymInitialize(GetCurrentProcess(), nullptr, TRUE) != FALSE;

		SYSTEMTIME st{};
		GetLocalTime(&st);

		write_raw_format("\r\n==================== %s ====================\r\n", title);
		write_raw_format("time     %04u-%02u-%02u %02u:%02u:%02u.%03u\r\n", st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond, st.wMilliseconds);
		write_raw_format("thread   %lu (%s)\r\n", GetCurrentThreadId(), t_thread_name.empty() ? "unnamed" : t_thread_name.c_str());
	}

	void on_terminate();

	// top level crash handler
	LONG WINAPI on_unhandled_exception(EXCEPTION_POINTERS* info)
	{
		if (!settings::logging::enabled)
			return g_previous_filter ? g_previous_filter(info) : EXCEPTION_CONTINUE_SEARCH;

		if (claim_crash() == crash_claim::reentered)
			return EXCEPTION_EXECUTE_HANDLER;

		const EXCEPTION_RECORD* record = info->ExceptionRecord;
		const CONTEXT* context = info->ContextRecord;
		DWORD64 address = reinterpret_cast<DWORD64>(record->ExceptionAddress);

		begin_crash_report("UNHANDLED EXCEPTION");

		char description[1024]{};
		describe_address(address, false, description, sizeof(description));

		write_raw_format("code     0x%08lX (%s)\r\n", record->ExceptionCode, exception_name(record->ExceptionCode));
		write_raw_format("address  0x%016llX %s\r\n", address, description);

		if ((record->ExceptionCode == EXCEPTION_ACCESS_VIOLATION || record->ExceptionCode == EXCEPTION_IN_PAGE_ERROR) && record->NumberParameters >= 2)
		{
			const char* operation = record->ExceptionInformation[0] == 0 ? "reading" : record->ExceptionInformation[0] == 1 ? "writing" : "executing";
			write_raw_format("fault    %s 0x%016llX\r\n", operation, static_cast<DWORD64>(record->ExceptionInformation[1]));
		}

		write_raw_format("rax %016llX  rbx %016llX  rcx %016llX  rdx %016llX\r\n", context->Rax, context->Rbx, context->Rcx, context->Rdx);
		write_raw_format("rsi %016llX  rdi %016llX  rbp %016llX  rsp %016llX\r\n", context->Rsi, context->Rdi, context->Rbp, context->Rsp);
		write_raw_format("r8  %016llX  r9  %016llX  r10 %016llX  r11 %016llX\r\n", context->R8, context->R9, context->R10, context->R11);
		write_raw_format("r12 %016llX  r13 %016llX  r14 %016llX  r15 %016llX\r\n", context->R12, context->R13, context->R14, context->R15);
		write_raw_format("rip %016llX  eflags %08lX\r\n", context->Rip, context->EFlags);

		write_stack_trace(context);
		write_minidump(info);

		if (record->ExceptionCode == 0xE06D7363 && g_previous_filter)
		{
			std::set_terminate(on_terminate);
			return g_previous_filter(info);
		}

		write_raw("==========================================================\r\n");
		logger::hold_console();
		return EXCEPTION_EXECUTE_HANDLER;
	}

	// std::terminate handler
	void on_terminate()
	{
		char reason[512] = "no active exception";
		if (std::exception_ptr current = std::current_exception())
		{
			try
			{
				std::rethrow_exception(current);
			}
			catch (const std::exception& e)
			{
				strncpy_s(reason, e.what(), _TRUNCATE);
			}
			catch (...)
			{
				strncpy_s(reason, "exception not derived from std::exception", _TRUNCATE);
			}
		}

		if (settings::logging::enabled)
		{
			if (claim_crash() == crash_claim::first)
			{
				begin_crash_report("STD::TERMINATE");

				CONTEXT context{};
				RtlCaptureContext(&context);
				write_stack_trace(&context);
			}

			write_raw_format("reason   %s\r\n", reason);
			write_raw("==========================================================\r\n");
		}

		logger::hold_console();
		TerminateProcess(GetCurrentProcess(), 3);
	}
}

// set up logging and the crash handlers
void logger::initialize()
{
	g_previous_filter = SetUnhandledExceptionFilter(on_unhandled_exception);

	HANDLE input = GetStdHandle(STD_INPUT_HANDLE);
	DWORD mode = 0;
	if (input && input != INVALID_HANDLE_VALUE && GetConsoleMode(input, &mode))
	{
		g_original_input_mode = mode;
		g_input_mode_saved = true;
		SetConsoleMode(input, (mode & ~ENABLE_QUICK_EDIT_MODE) | ENABLE_EXTENDED_FLAGS);
	}

	name_thread("main");

	LOG_INFO("Glory build {} {}, pid {}", __DATE__, __TIME__, GetCurrentProcessId());
	LOG_INFO("log file: {}", get_log_path());
}

// true if this level is enabled
bool logger::should_log(level lvl)
{
	return settings::logging::enabled && static_cast<int>(lvl) >= settings::logging::min_level;
}

// log a message now
void logger::write(level lvl, const site_t& site, std::string message)
{
	const std::string text = make_line(lvl, site, message);

	std::lock_guard<std::mutex> lock(g_mutex);
	write_line(lvl, text);
}

// queue a message for the log thread (cheap on hot threads)
void logger::post(level lvl, const site_t& site, std::string message)
{
	std::string text = make_line(lvl, site, message);

	post_office_t& office = post_office();
	{
		std::lock_guard<std::mutex> lock(office.mutex);

		if (office.waiting.size() >= max_waiting)
		{
			office.turned_away++;
			return;
		}

		office.waiting.push_back({ lvl, std::move(text) });

		if (!office.started)
		{
			office.started = true;
			std::thread(write_posted).detach();
		}
	}

	office.wake.notify_one();
}

// true if enough time passed to log again
bool logger::throttle(std::atomic<std::uint64_t>& last_tick, std::uint64_t interval_ms)
{
	std::uint64_t now = GetTickCount64();
	std::uint64_t last = last_tick.load(std::memory_order_relaxed);
	if (last != 0 && now - last < interval_ms)
		return false;

	return last_tick.compare_exchange_strong(last, now, std::memory_order_relaxed);
}

// keep the console open at exit
void logger::hold_console()
{
	if (!settings::logging::enabled || !settings::logging::to_console || !GetConsoleWindow())
		return;

	static std::atomic<bool> held{ false };
	if (held.exchange(true))
		return;

	char system_directory[MAX_PATH]{};
	if (!GetSystemDirectoryA(system_directory, MAX_PATH))
		return;

	char application[MAX_PATH + 16]{};
	std::snprintf(application, sizeof(application), "%s\\cmd.exe", system_directory);

	HANDLE input = GetStdHandle(STD_INPUT_HANDLE);
	if (g_input_mode_saved && input && input != INVALID_HANDLE_VALUE)
		SetConsoleMode(input, g_original_input_mode);

	HANDLE console = GetStdHandle(STD_OUTPUT_HANDLE);
	if (console && console != INVALID_HANDLE_VALUE)
	{
		const char notice[] = "\r\nGlory has exited. This window stays open so the output above can be read.\r\n";
		DWORD written = 0;
		WriteFile(console, notice, sizeof(notice) - 1, &written, nullptr);
	}

	char command[] = "cmd.exe /c pause";

	STARTUPINFOA startup{};
	startup.cb = sizeof(startup);

	PROCESS_INFORMATION process{};
	if (CreateProcessA(application, command, nullptr, nullptr, FALSE, 0, nullptr, nullptr, &startup, &process))
	{
		CloseHandle(process.hThread);
		CloseHandle(process.hProcess);
	}
}

// label the current thread for logs
void logger::name_thread(const char* name)
{
	t_thread_name = name;

	std::set_terminate(on_terminate);
}

// readable name for a log level
const char* logger::level_name(level lvl)
{
	static constexpr const char* names[level_count] = { "TRACE", "DEBUG", "INFO", "WARN", "ERROR", "FATAL" };
	return names[static_cast<int>(lvl)];
}

// the folder logs go in
std::string logger::get_log_directory()
{
	char appdata[MAX_PATH]{};
	DWORD length = GetEnvironmentVariableA("APPDATA", appdata, MAX_PATH);

	std::string path = std::string(length > 0 && length < MAX_PATH ? appdata : ".") + "\\1\\logs\\";

	std::error_code ec;
	std::filesystem::create_directories(path, ec);
	return path;
}

// the current log files path
std::string logger::get_log_path()
{
	std::lock_guard<std::mutex> lock(g_mutex);
	return g_file_path;
}
