#include "memory.h"
#include <cstring>
#include <logger/logger.h>

// true if a process has a visible window
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

// find a process id by name
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

// find a loaded modules base address
std::uint64_t memory_t::find_module_address(const std::string& module_name)
{
    std::uint64_t module_address = 0;

    if (!process_handle)
    {
        return module_address;
    }

    DWORD pID = GetProcessId(process_handle);

    HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPMODULE | TH32CS_SNAPMODULE32, pID);

    if (snapshot == INVALID_HANDLE_VALUE)
    {

        LOG_EVERY(30000, logger::level::warn, "module snapshot of pid {} failed (error {})", pID, GetLastError());
        return module_address;
    }

    MODULEENTRY32 module_entry{};
    module_entry.dwSize = sizeof(MODULEENTRY32);

    if (Module32First(snapshot, &module_entry))
    {
        do
        {
            if (!_stricmp(module_name.c_str(), module_entry.szModule))
            {
                module_address = reinterpret_cast<uint64_t>(module_entry.modBaseAddr);
                base_address = module_address;
                module_size = module_entry.modBaseSize;
                break;
            }
        } while (Module32Next(snapshot, &module_entry));
    }

    CloseHandle(snapshot);
    return module_address;
}

// open a handle to the target process
bool memory_t::attach_to_process(const std::string& process_name)
{
    std::uint32_t pID = find_process_id(process_name);
    if (pID == 0)
    {
        LOG_DEBUG("process {} is not running", process_name);
        return false;
    }

    HANDLE process = OpenProcess(PROCESS_ALL_ACCESS, false, pID);
    if (!process || process == INVALID_HANDLE_VALUE)
    {
        LOG_EVERY(30000, logger::level::error, "OpenProcess failed for {} (pid {}, error {})", process_name, pID, GetLastError());
        return false;
    }

    HANDLE previous = process_handle;
    process_handle = process;
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

// close the process handle
void memory_t::detach()
{
    HANDLE previous = process_handle;
    process_handle = nullptr;
    process_id = 0;
    base_address = 0;
    module_size = 0;

    if (previous)
    {
        CloseHandle(previous);
    }
}

// true if the target process exited
bool memory_t::process_ended()
{
    return process_handle != nullptr && WaitForSingleObject(process_handle, 0) == WAIT_OBJECT_0;
}

// read a roblox string at an address
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
    Luck_ReadVirtualMemory(process_handle, reinterpret_cast<void*>(string_address), buffer.data(), static_cast<ULONG>(buffer.size()), nullptr);

    return std::string(buffer.data(), string_length);
}

// read raw bytes from the process
void memory_t::read_buffer(std::uint64_t address, void* buffer, std::size_t size)
{
    Luck_ReadVirtualMemory(process_handle, reinterpret_cast<void*>(address), buffer, static_cast<ULONG>(size), nullptr);
}

// write raw bytes into the process
void memory_t::write_buffer(std::uint64_t address, const void* buffer, std::size_t size)
{
    Luck_WriteVirtualMemory(process_handle, reinterpret_cast<void*>(address), const_cast<void*>(buffer), static_cast<ULONG>(size), nullptr);
}

// the attached process id
std::uint32_t memory_t::get_process_id()
{
    return process_id;
}
// the modules base address
std::uint64_t memory_t::get_module_address()
{
    return base_address;
}

// the modules size
std::uint64_t memory_t::get_module_size()
{
    return module_size;
}

// the open process handle
HANDLE memory_t::get_process_handle()
{
    return process_handle;
}
