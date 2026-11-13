// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <string>
#include <vector>
namespace awm {
std::wstring widen(const std::string& value);
std::string narrow(const std::wstring& value);
bool equal_name(const std::wstring& left, const std::wstring& right);
std::wstring escape_name(const std::string& value);
std::string decode_name(const std::wstring& value);
struct DirectoryName { std::string original; std::wstring visible; };
std::vector<DirectoryName> directory_names(const std::vector<std::string>& names);
std::vector<std::wstring> path_components(const std::wstring& path);
}
