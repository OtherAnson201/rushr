#pragma once
#include <windows.h>
#include <TlHelp32.h>
#include <vector>
#include <string>
#include <memory>
#include <cstddef>

extern "C" intptr_t
Luck_ReadVirtualMemory
(
    HANDLE ProcessHandle,
    PVOID BaseAddress,
    PVOID Buffer,
    ULONG NumberOfBytesToRead,
    PULONG NumberOfBytesRead
);

extern "C" intptr_t
Luck_WriteVirtualMemory
(
    HANDLE Processhandle,
    PVOID BaseAddress,
    PVOID Buffer,
    ULONG NumberOfBytesToWrite,
    PULONG NumberOfBytesWritten
);

class memory_t final
{
public:
    memory_t() = default;
    ~memory_t() = default;

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
    T read(std::uint64_t address);

    template <typename T>
    void write(std::uint64_t address, T value);

    std::uint32_t get_process_id();
    std::uint64_t get_module_address();
    std::uint64_t get_module_size();
    HANDLE get_process_handle();
private:
    std::uint32_t process_id{};
    std::uint64_t base_address{};
    std::uint64_t module_size{};
    HANDLE process_handle{};
};

template <typename T>
T memory_t::read(uint64_t address)
{
    T buffer{};

    Luck_ReadVirtualMemory(process_handle, reinterpret_cast<void*>(address), &buffer, sizeof(T), nullptr);

    return buffer;
}

template <typename T>
void memory_t::write(uint64_t address, T value)
{
    Luck_WriteVirtualMemory(process_handle, reinterpret_cast<void*>(address), &value, sizeof(T), nullptr);
}

inline std::unique_ptr<memory_t> memory = std::make_unique<memory_t>();
