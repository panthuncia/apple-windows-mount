// SPDX-License-Identifier: GPL-3.0-or-later
#include <filesystem>
#include <iostream>
#include <string>
#include "windows_compat.h"
extern "C" {
#include "conf.h"
#include "usbmuxd-proto.h"
}

void require(bool condition, const char* message)
{
    if (!condition) throw std::runtime_error(message);
}

int main()
{
    auto directory = std::filesystem::temp_directory_path() / ("awm-store-test-" + std::to_string(GetCurrentProcessId()));
    try {
        auto bytes = directory.u8string();
        std::string path(bytes.begin(), bytes.end());
        require(config_set_config_dir(path.c_str()) == 0, "initialize private store");
        require(sizeof(usbmuxd_header) == 16, "mux header wire size");
        require(sizeof(usbmuxd_device_record) == 268, "mux device wire size");
        require(sizeof(tcphdr) == 20, "TCP wire size");
        char payload[] = "payload";
        require(config_set_device_record("../escape", payload, 7) == -EINVAL, "reject traversal");
        require(config_set_device_record("", payload, 7) == -EINVAL, "reject empty identifier");
        require(config_set_device_record("00008030-000469D81420C02E", payload, 7) == 0, "save record");
        char* data = nullptr;
        uint64_t size = 0;
        require(config_get_device_record("00008030-000469D81420C02E", &data, &size) == 0, "load record");
        require(size == 7 && std::string(data, size) == "payload", "roundtrip bytes");
        free(data);
        char replacement[] = "replacement";
        require(config_set_device_record("00008030-000469D81420C02E", replacement, 11) == 0, "atomic replace");
        require(config_get_device_record("00008030-000469D81420C02E", &data, &size) == 0 && size == 11, "replacement length");
        free(data);
        char* first = nullptr;
        char* second = nullptr;
        config_get_system_buid(&first);
        config_get_system_buid(&second);
        require(first && second && std::string(first) == second && strlen(first) == 36, "persistent BUID");
        free(first);
        free(second);
        require(config_remove_device_record("00008030-000469D81420C02E") == 0, "remove record");
        require(config_remove_device_record("00008030-000469D81420C02E") == -ENOENT, "missing record");
        std::filesystem::remove_all(directory);
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        std::filesystem::remove_all(directory);
        return 1;
    }
}
