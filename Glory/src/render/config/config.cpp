#include "config.h"
#include <render/assets/settings.h>
#include <logger/logger.h>
#include <algorithm>
#include <climits>
#include <cmath>
#include <cstring>
#include <exception>
#include <stdexcept>
#include <filesystem>
#include <fstream>
#include <sstream>

namespace
{
    enum class value_type
    {
        boolean,
        integer,
        real,
        colour
    };

    struct entry_t
    {
        const char* section;
        const char* key;
        value_type type;
        void* value;

        int min_value = INT_MIN;
        int max_value = INT_MAX;
    };

    // describe one saveable setting
    entry_t make(const char* section, const char* key, bool& value)
    {
        return { section, key, value_type::boolean, &value };
    }

    // describe one saveable setting
    entry_t make(const char* section, const char* key, int& value, int min_value = INT_MIN, int max_value = INT_MAX)
    {
        return { section, key, value_type::integer, &value, min_value, max_value };
    }

    // describe one saveable setting
    entry_t make(const char* section, const char* key, float& value)
    {
        return { section, key, value_type::real, &value };
    }

    // describe one saveable setting
    entry_t make(const char* section, const char* key, float (&value)[4])
    {
        return { section, key, value_type::colour, value };
    }

    // all the settings that get saved
    const std::vector<entry_t>& get_entries()
    {
        namespace s = settings;

        constexpr int max_mode = 2;
        constexpr int max_key = 255;

        static const std::vector<entry_t> entries = {
            make("aimbot", "enabled", s::targeting::enabled),
            make("aimbot", "keybind", s::targeting::keybind, 0, max_key),
            make("aimbot", "activation_mode", s::targeting::activation_mode, 0, max_mode),
            make("aimbot", "target_part", s::targeting::target_part, 0, 2),
            make("aimbot", "fov", s::targeting::fov),
            make("aimbot", "draw_fov", s::targeting::draw_fov),
            make("aimbot", "fov_circle_colour", s::targeting::fov_circle_colour),
            make("aimbot", "fov_outline_colour", s::targeting::fov_outline_colour),
            make("aimbot", "sensitivity", s::targeting::sensitivity),

            make("visuals", "enabled", s::visuals::enabled),
            make("visuals", "box", s::visuals::box),
            make("visuals", "box_color", s::visuals::box_color),
            make("visuals", "name", s::visuals::name),
            make("visuals", "name_color", s::visuals::name_color),
            make("visuals", "healthbar", s::visuals::healthbar),
            make("visuals", "healthbar_color", s::visuals::healthbar_color),

            make("logging", "enabled", s::logging::enabled),
            make("logging", "min_level", s::logging::min_level, 0, 5),
            make("logging", "to_file", s::logging::to_file),
            make("logging", "to_console", s::logging::to_console),

            make("overlay", "hide_from_capture", s::overlay::hide_from_capture),
        };

        return entries;
    }

    // the appdata folder path
    std::string get_appdata_path()
    {
        #pragma warning(suppress: 4996)
        const char* appdata = std::getenv("APPDATA");
        return appdata ? appdata : "";
    }

    // write one setting to the file
    void write_value(std::ofstream& file, const entry_t& entry)
    {
        switch (entry.type)
        {
        case value_type::boolean:
            file << *static_cast<bool*>(entry.value);
            break;
        case value_type::integer:
            file << *static_cast<int*>(entry.value);
            break;
        case value_type::real:
            file << *static_cast<float*>(entry.value);
            break;
        case value_type::colour:
        {
            const float* colour = static_cast<float*>(entry.value);
            file << colour[0] << "," << colour[1] << "," << colour[2] << "," << colour[3];
            break;
        }
        }
    }

    // parse a float from text
    float read_real(const std::string& text)
    {
        const float value = std::stof(text);
        if (!std::isfinite(value))
        {
            throw std::invalid_argument("not a finite number");
        }
        return value;
    }

    // parse one setting from text
    void read_value(const entry_t& entry, const std::string& text)
    {
        switch (entry.type)
        {
        case value_type::boolean:
            *static_cast<bool*>(entry.value) = std::stoi(text) != 0;
            break;
        case value_type::integer:
            *static_cast<int*>(entry.value) = std::clamp(std::stoi(text), entry.min_value, entry.max_value);
            break;
        case value_type::real:
            *static_cast<float*>(entry.value) = read_real(text);
            break;
        case value_type::colour:
        {

            float parsed[4]{};
            std::stringstream stream(text);
            std::string item;
            int count = 0;
            while (count < 4 && std::getline(stream, item, ','))
            {
                parsed[count++] = read_real(item);
            }
            if (count == 4)
            {
                std::memcpy(entry.value, parsed, sizeof(parsed));
            }
            break;
        }
        }
    }
}

// the configs folder, made if missing
std::string config::get_config_directory() {
    std::string path = get_appdata_path() + "\\1\\configs\\";
    std::error_code error;
    std::filesystem::create_directories(path, error);
    return path;
}

// save settings to a named file
void config::save_config(const std::string& name) {
    std::string path = get_config_directory() + name + ".cfg";
    std::ofstream file(path);
    if (!file.is_open())
    {
        LOG_ERROR("could not open {} for writing", path);
        return;
    }

    const char* current_section = "";
    for (const entry_t& entry : get_entries())
    {
        if (std::strcmp(entry.section, current_section) != 0)
        {

            if (current_section[0] != '\0')
            {
                file << "\n";
            }
            file << "[" << entry.section << "]\n";
            current_section = entry.section;
        }

        file << entry.key << "=";
        write_value(file, entry);
        file << "\n";
    }

    file.close();
    LOG_INFO("config saved: {}", path);
}

// load settings from a named file
void config::load_config(const std::string& name) {
    std::string path = get_config_directory() + name + ".cfg";
    std::ifstream file(path);
    if (!file.is_open())
    {
        LOG_WARN("could not open {} for reading", path);
        return;
    }

    const std::vector<entry_t>& entries = get_entries();

    std::string current_section;
    std::string line;
    int line_number = 0;

    while (std::getline(file, line)) {
        line_number++;

        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (line.empty()) continue;

        if (line[0] == '[' && line.back() == ']') {
            current_section = line.substr(1, line.length() - 2);
            continue;
        }

        size_t eq_pos = line.find('=');
        if (eq_pos == std::string::npos) continue;

        std::string key = line.substr(0, eq_pos);
        std::string value = line.substr(eq_pos + 1);

        auto entry = std::find_if(entries.begin(), entries.end(), [&](const entry_t& candidate) {
            return current_section == candidate.section && key == candidate.key;
        });

        if (entry == entries.end()) {
            LOG_DEBUG("{} line {}: ignored unknown setting [{}] {}", path, line_number, current_section, key);
            continue;
        }

        try {
            read_value(*entry, value);
        }
        catch (const std::exception&) {
            LOG_WARN("{} line {}: could not read value '{}' for [{}] {}", path, line_number, value, current_section, key);
        }
    }

    file.close();
    LOG_INFO("config loaded: {}", path);
}

// list the saved config names
std::vector<std::string> config::get_config_list() {
    std::vector<std::string> configs;
    std::string config_dir = get_config_directory();

    std::error_code error;
    for (std::filesystem::directory_iterator it(config_dir, error), end; !error && it != end; it.increment(error)) {
        if (it->path().extension() == ".cfg") {
            configs.push_back(it->path().stem().string());
        }
    }
    return configs;
}
