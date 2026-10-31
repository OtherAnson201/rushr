#pragma once
#include <string>
#include <vector>

namespace config {
    std::string get_config_directory();
    std::vector<std::string> get_config_list();
    void save_config(const std::string& name);
    void load_config(const std::string& name);
}
