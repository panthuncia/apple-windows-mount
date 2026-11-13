// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "device.h"
#include <windows.h>
#include <filesystem>
#include <functional>
#include <atomic>
namespace awm {
std::wstring user_sid();
std::wstring pipe_name();
std::filesystem::path executable_directory();
std::filesystem::path application_directory();
PSECURITY_DESCRIPTOR private_descriptor();
Json controller_request(const Json& request, bool start_if_missing, const std::wstring& name = {});
Json setup_usb_drivers();
void serve_controller(const std::function<Json(const Json&)>& handler, const std::atomic<bool>& stop, const std::wstring& name = {});
}
