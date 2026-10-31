#include "config.h"
#include <render/assets/settings.h>
#include <cache/cache.h>
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

    constexpr const char* teams_section = "teams";
    constexpr const char* spotter_off_key = "esp_off";
    constexpr const char* target_off_key = "target_off";

    constexpr const char* spotter_player_key = "esp_player";

    constexpr const char* spotter_whitelist_key = "esp_whitelist";

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
            make("aimbot", "target_part", s::targeting::target_part, 0, 6),
            make("aimbot", "sticky_aim", s::targeting::sticky_aim),
            make("aimbot", "fov", s::targeting::fov),
            make("aimbot", "draw_fov", s::targeting::draw_fov),
            make("aimbot", "fov_circle_colour", s::targeting::fov_circle_colour),
            make("aimbot", "fov_outline_colour", s::targeting::fov_outline_colour),
            make("aimbot", "sensitivity_x", s::targeting::sensitivity_x),
            make("aimbot", "sensitivity_y", s::targeting::sensitivity_y),
            make("aimbot", "switch_delay", s::targeting::switch_delay),
            make("aimbot", "enable_prediction", s::targeting::enable_prediction),
            make("aimbot", "prediction_x", s::targeting::prediction_x),
            make("aimbot", "prediction_y", s::targeting::prediction_y),

            make("silentaim", "enabled", s::correction::enabled),
            make("silentaim", "keybind", s::correction::keybind, 0, max_key),
            make("silentaim", "activation_mode", s::correction::activation_mode, 0, max_mode),
            make("silentaim", "target_part", s::correction::target_part, 0, s::correction::random_part),
            make("silentaim", "sticky_aim", s::correction::sticky_aim),
            make("silentaim", "switch_delay", s::correction::switch_delay),
            make("silentaim", "fov", s::correction::fov),
            make("silentaim", "draw_fov", s::correction::draw_fov),
            make("silentaim", "fov_circle_colour", s::correction::fov_circle_colour),
            make("silentaim", "fov_outline_colour", s::correction::fov_outline_colour),
            make("silentaim", "enable_prediction", s::correction::enable_prediction),
            make("silentaim", "prediction_x", s::correction::prediction_x),
            make("silentaim", "prediction_y", s::correction::prediction_y),

            make("triggerbot", "enabled", s::autofire::enabled),
            make("triggerbot", "keybind", s::autofire::keybind, 0, max_key),
            make("triggerbot", "activation_mode", s::autofire::activation_mode, 0, max_mode),
            make("triggerbot", "delay", s::autofire::delay),
            make("triggerbot", "randomize_delay", s::autofire::randomize_delay),
            make("triggerbot", "delay_min", s::autofire::delay_min),
            make("triggerbot", "delay_max", s::autofire::delay_max),
            make("triggerbot", "hit_radius", s::autofire::hit_radius),
            make("triggerbot", "target_part", s::autofire::target_part, 0, 3),

            make("visuals", "enabled", s::visuals::enabled),
            make("visuals", "box", s::visuals::box),
            make("visuals", "box_type", s::visuals::box_type, 0, 1),
            make("visuals", "box_color", s::visuals::box_color),
            make("visuals", "team_colors", s::visuals::team_colors),
            make("visuals", "guard_color", s::visuals::guard_color),
            make("visuals", "inmate_color", s::visuals::inmate_color),
            make("visuals", "criminal_color", s::visuals::criminal_color),
            make("visuals", "friends", s::visuals::friends),
            make("visuals", "friend_color", s::visuals::friend_color),
            make("visuals", "whitelist_color", s::visuals::whitelist_color),
            make("visuals", "name", s::visuals::name),
            make("visuals", "name_color", s::visuals::name_color),
            make("visuals", "healthbar", s::visuals::healthbar),
            make("visuals", "healthbar_color", s::visuals::healthbar_color),

            make("universal", "teamcheck", s::universal::teamcheck),
            make("universal", "friend_check", s::universal::friend_check),
            make("universal", "knock_check", s::universal::knock_check),
            make("universal", "health_check", s::universal::health_check),
            make("universal", "min_health", s::universal::min_health),
            make("universal", "distance_check", s::universal::distance_check),
            make("universal", "max_distance", s::universal::max_distance),
            make("universal", "wall_check", s::universal::wall_check),
            make("universal", "hostile_inmates_only", s::universal::hostile_inmates_only),

            make("movement", "speedhack_enabled", s::mover::speedhack::enabled),
            make("movement", "speedhack_keybind", s::mover::speedhack::keybind, 0, max_key),
            make("movement", "speedhack_activation_mode", s::mover::speedhack::activation_mode, 0, max_mode),
            make("movement", "speedhack_mode", s::mover::speedhack::mode, 0, 1),
            make("movement", "speedhack_speed", s::mover::speedhack::speed),
            make("movement", "noclip_enabled", s::mover::noclip::enabled),
            make("movement", "noclip_keybind", s::mover::noclip::keybind, 0, max_key),
            make("movement", "noclip_activation_mode", s::mover::noclip::activation_mode, 0, max_mode),
            make("movement", "jumppower_enabled", s::mover::jumppower::enabled),
            make("movement", "jumppower_keybind", s::mover::jumppower::keybind, 0, max_key),
            make("movement", "jumppower_activation_mode", s::mover::jumppower::activation_mode, 0, max_mode),
            make("movement", "jumppower_value", s::mover::jumppower::value),
            make("movement", "freecam_enabled", s::mover::freecam::enabled),
            make("movement", "freecam_keybind", s::mover::freecam::keybind, 0, max_key),
            make("movement", "freecam_activation_mode", s::mover::freecam::activation_mode, 0, max_mode),
            make("movement", "freecam_speed", s::mover::freecam::speed),
            make("movement", "teleport_enabled", s::mover::teleport::enabled),
            make("movement", "teleport_keybind", s::mover::teleport::keybind, 0, max_key),
            make("movement", "teleport_activation_mode", s::mover::teleport::activation_mode, 0, max_mode),
            make("movement", "lagswitch_enabled", s::mover::netblock::enabled),
            make("movement", "lagswitch_keybind", s::mover::netblock::keybind, 0, max_key),
            make("movement", "lagswitch_activation_mode", s::mover::netblock::activation_mode, 0, max_mode),
            make("movement", "antifling_enabled", s::mover::stabilizer::enabled),
            make("movement", "antifling_max_speed", s::mover::stabilizer::max_speed),
            make("movement", "antifling_lock_seats", s::mover::stabilizer::lock_seats),
            make("movement", "antifling_anti_ragdoll", s::mover::stabilizer::anti_ragdoll),
            make("movement", "antifling_leave_vehicles", s::mover::stabilizer::leave_vehicles),
            make("movement", "antifling_vehicle_speed", s::mover::stabilizer::vehicle_speed),

            make("fly", "enabled", s::expl::fly_enabled),
            make("fly", "keybind", s::expl::fly_keybind, 0, max_key),
            make("fly", "mode", s::expl::fly_keybind_mode, 0, max_mode),
            make("fly", "fly_mode", s::expl::fly_mode, 0, 2),
            make("fly", "speed", s::expl::fly_speed),

            make("macro", "enabled", s::macros::enabled),
            make("macro", "keybind", s::macros::keybind, 0, max_key),
            make("macro", "spin", s::macros::spin, s::macros::spin_min, s::macros::spin_max),
            make("macro", "dpi", s::macros::dpi, s::macros::dpi_min, s::macros::dpi_max),
            make("macro", "sensitivity", s::macros::sensitivity),

            make("autoclicker", "clicks_per_second", s::clicker::clicks_per_second, s::clicker::clicks_per_second_min, s::clicker::clicks_per_second_max),
            make("autoclicker", "keybind", s::clicker::keybind, 0, max_key),

            make("logging", "enabled", s::logging::enabled),
            make("logging", "min_level", s::logging::min_level, 0, 5),
            make("logging", "to_file", s::logging::to_file),
            make("logging", "to_console", s::logging::to_console),
            make("logging", "positions", s::logging::positions),
            make("logging", "positions_interval", s::logging::positions_interval),

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

    {
        std::lock_guard<std::mutex> lock(cache::mtx);

        auto write_teams = [&file](const char* key, const std::unordered_set<std::string>& teams)
        {
            for (const std::string& team : teams)
            {

                if (team.find_first_of("\r\n") == std::string::npos)
                {
                    file << key << "=" << team << "\n";
                }
            }
        };

        file << "\n[" << teams_section << "]\n";
        write_teams(spotter_off_key, cache::spotter_off_teams);
        write_teams(target_off_key, cache::target_off_teams);
        write_teams(spotter_player_key, cache::spotter_always_players);
        write_teams(spotter_whitelist_key, cache::spotter_whitelist_players);
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

            if (current_section == teams_section) {
                std::lock_guard<std::mutex> lock(cache::mtx);
                cache::spotter_off_teams.clear();
                cache::target_off_teams.clear();
                cache::spotter_always_players.clear();
                cache::spotter_whitelist_players.clear();
            }
            continue;
        }

        size_t eq_pos = line.find('=');
        if (eq_pos == std::string::npos) continue;

        std::string key = line.substr(0, eq_pos);
        std::string value = line.substr(eq_pos + 1);

        if (current_section == teams_section && (key == spotter_off_key || key == target_off_key || key == spotter_player_key || key == spotter_whitelist_key)) {
            if (!value.empty()) {
                std::lock_guard<std::mutex> lock(cache::mtx);
                (key == spotter_off_key ? cache::spotter_off_teams : key == target_off_key ? cache::target_off_teams : key == spotter_player_key ? cache::spotter_always_players : cache::spotter_whitelist_players).insert(value);
            }
            continue;
        }

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

    settings::mover::netblock::enabled = false;

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
