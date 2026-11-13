// SPDX-License-Identifier: GPL-3.0-or-later
#include "names.h"
#include <windows.h>
#include <algorithm>
#include <stdexcept>
#include <cwctype>
#include <bcrypt.h>
#include <array>
namespace awm {
std::wstring widen(const std::string& value)
{
    if (value.empty()) return {};
    int size = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, value.data(), static_cast<int>(value.size()), nullptr, 0);
    if (!size) throw std::runtime_error("Invalid UTF-8 filename");
    std::wstring output(size, L'\0');
    MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, value.data(), static_cast<int>(value.size()), output.data(), size);
    return output;
}
std::string narrow(const std::wstring& value)
{
    if (value.empty()) return {};
    int size = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, value.data(), static_cast<int>(value.size()), nullptr, 0, nullptr, nullptr);
    if (!size) throw std::runtime_error("Invalid UTF-16 filename");
    std::string output(size, '\0');
    WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, value.data(), static_cast<int>(value.size()), output.data(), size, nullptr, nullptr);
    return output;
}
bool equal_name(const std::wstring& left, const std::wstring& right)
{
    return CompareStringOrdinal(left.data(), static_cast<int>(left.size()), right.data(), static_cast<int>(right.size()), TRUE) == CSTR_EQUAL;
}
std::wstring escape_name(const std::string& value)
{
    static constexpr wchar_t hex[] = L"0123456789ABCDEF";
    std::wstring output = L"~";
    for (unsigned char byte : value) { output += hex[byte >> 4]; output += hex[byte & 15]; }
    if (output.size() > 255) {
        std::array<unsigned char, 32> digest{};
        if (BCryptHash(BCRYPT_SHA256_ALG_HANDLE, nullptr, 0,
            reinterpret_cast<PUCHAR>(const_cast<char*>(value.data())), static_cast<ULONG>(value.size()), digest.data(), static_cast<ULONG>(digest.size())) < 0)
            throw std::runtime_error("Cannot encode long device filename");
        output = L"~H";
        for (auto byte : digest) { output += hex[byte >> 4]; output += hex[byte & 15]; }
    }
    return output;
}
static bool safe(const std::wstring& name)
{
    if (name.empty() || name.front() == '~' || name.back() == '.' || name.back() == ' ') return false;
    for (wchar_t value : name) if (value < 32 || std::wstring_view(L"<>:\"/\\|?*").find(value) != std::wstring_view::npos) return false;
    auto base = name.substr(0, name.find('.'));
    for (const wchar_t* reserved : {L"CON", L"PRN", L"AUX", L"NUL", L"CONIN$", L"CONOUT$"})
        if (equal_name(base, reserved)) return false;
    if (base.size() == 4 && (equal_name(base.substr(0, 3), L"COM") || equal_name(base.substr(0, 3), L"LPT")) &&
        ((base[3] >= '0' && base[3] <= '9') || base[3] == 0xB9 || base[3] == 0xB2 || base[3] == 0xB3)) return false;
    return name != L"." && name != L"..";
}
std::string decode_name(const std::wstring& value)
{
    if (value.empty()) throw std::runtime_error("Empty filename");
    if (value.front() != L'~') {
        if (!safe(value)) throw std::runtime_error("Invalid Windows filename");
        return narrow(value);
    }
    if (value.size() < 3 || value.size() % 2 == 0) throw std::runtime_error("Invalid escaped filename");
    std::string output;
    auto digit = [](wchar_t c) -> int {
        if (c >= '0' && c <= '9') return c - '0';
        if (c >= 'A' && c <= 'F') return c - 'A' + 10;
        if (c >= 'a' && c <= 'f') return c - 'a' + 10;
        throw std::runtime_error("Invalid filename escape");
    };
    for (size_t index = 1; index < value.size(); index += 2)
        output += static_cast<char>((digit(value[index]) << 4) | digit(value[index + 1]));
    if (output == "." || output == ".." || output.find('/') != std::string::npos || output.find('\0') != std::string::npos)
        throw std::runtime_error("Filename escapes the source root");
    return output;
}
std::vector<DirectoryName> directory_names(const std::vector<std::string>& names)
{
    std::vector<DirectoryName> result;
    for (const auto& raw : names) {
        if (raw == "." || raw == "..") continue;
        std::wstring visible;
        try { visible = widen(raw); if (!safe(visible)) visible = escape_name(raw); }
        catch (...) { visible = escape_name(raw); }
        result.push_back({raw, visible});
    }
    std::sort(result.begin(), result.end(), [](const auto& left, const auto& right) {
        return CompareStringOrdinal(left.visible.data(), static_cast<int>(left.visible.size()), right.visible.data(), static_cast<int>(right.visible.size()), TRUE) == CSTR_LESS_THAN;
    });
    for (size_t first = 0; first < result.size();) {
        size_t last = first + 1;
        while (last < result.size() && equal_name(result[first].visible, result[last].visible)) ++last;
        if (last - first > 1) for (size_t index = first; index < last; ++index) result[index].visible = escape_name(result[index].original);
        first = last;
    }
    std::sort(result.begin(), result.end(), [](const auto& left, const auto& right) {
        return CompareStringOrdinal(left.visible.data(), static_cast<int>(left.visible.size()), right.visible.data(), static_cast<int>(right.visible.size()), TRUE) == CSTR_LESS_THAN;
    });
    for (size_t index = 1; index < result.size(); ++index)
        if (equal_name(result[index - 1].visible, result[index].visible)) throw std::runtime_error("Device filename alias collision");
    return result;
}
std::vector<std::wstring> path_components(const std::wstring& path)
{
    if (path.empty() || path.front() != L'\\') throw std::runtime_error("Volume path must be absolute");
    std::vector<std::wstring> result;
    size_t begin = 1;
    while (begin < path.size()) {
        auto end = path.find(L'\\', begin);
        if (end == std::wstring::npos) end = path.size();
        auto component = path.substr(begin, end - begin);
        if (component.empty() || component == L"." || component == L".." || component.find(L':') != std::wstring::npos)
            throw std::runtime_error("Invalid volume path component");
        result.push_back(component);
        begin = end + 1;
    }
    return result;
}
}
