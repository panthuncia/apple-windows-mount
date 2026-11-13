// SPDX-License-Identifier: GPL-3.0-or-later
#include <filesystem>
#include <fstream>
#include <mutex>
#include <string>
#include <vector>
#include <sddl.h>
#include <bcrypt.h>
#include <objbase.h>

extern "C" {
#include "conf.h"
}

namespace {
std::filesystem::path state_directory;
std::string directory_utf8;
std::recursive_mutex state_mutex;
constexpr std::uint64_t max_record_size = 1024 * 1024;

bool valid_identifier(const char* identifier)
{
    if (!identifier || !*identifier) return false;
    std::string key(identifier);
    return key.size() <= 128 && key.find_first_not_of("0123456789abcdefABCDEF-") == std::string::npos;
}

void protect_directory(const std::filesystem::path& path)
{
    HANDLE token = nullptr;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token)) throw std::runtime_error("OpenProcessToken");
    DWORD size = 0;
    GetTokenInformation(token, TokenUser, nullptr, 0, &size);
    std::vector<BYTE> buffer(size);
    BOOL ok = GetTokenInformation(token, TokenUser, buffer.data(), size, &size);
    CloseHandle(token);
    if (!ok) throw std::runtime_error("GetTokenInformation");
    LPWSTR sid = nullptr;
    if (!ConvertSidToStringSidW(reinterpret_cast<TOKEN_USER*>(buffer.data())->User.Sid, &sid))
        throw std::runtime_error("ConvertSidToStringSid");
    std::wstring sddl = L"D:P(A;OICI;FA;;;SY)(A;OICI;FA;;;" + std::wstring(sid) + L")";
    LocalFree(sid);
    PSECURITY_DESCRIPTOR descriptor = nullptr;
    if (!ConvertStringSecurityDescriptorToSecurityDescriptorW(sddl.c_str(), SDDL_REVISION_1, &descriptor, nullptr))
        throw std::runtime_error("Invalid DACL");
    SECURITY_ATTRIBUTES attributes{sizeof(attributes), descriptor, FALSE};
    BOOL created = CreateDirectoryW(path.c_str(), &attributes);
    DWORD error = GetLastError();
    BOOL secured = SetFileSecurityW(path.c_str(), DACL_SECURITY_INFORMATION | PROTECTED_DACL_SECURITY_INFORMATION, descriptor);
    LocalFree(descriptor);
    if ((!created && error != ERROR_ALREADY_EXISTS) || !secured) throw std::runtime_error("Cannot protect pair record directory");
    DWORD flags = GetFileAttributesW(path.c_str());
    if (flags == INVALID_FILE_ATTRIBUTES || (flags & FILE_ATTRIBUTE_REPARSE_POINT))
        throw std::runtime_error("Pair record directory is a reparse point");
}

void initialize()
{
    if (!state_directory.empty()) return;
    wchar_t* local = nullptr;
    size_t length = 0;
    if (_wdupenv_s(&local, &length, L"LOCALAPPDATA") || !local) throw std::runtime_error("LOCALAPPDATA missing");
    std::filesystem::path parent = std::filesystem::path(local) / L"AppleWindowsMount";
    free(local);
    protect_directory(parent);
    auto directory = parent / L"Pairing";
    protect_directory(directory);
    auto bytes = directory.u8string();
    directory_utf8.assign(bytes.begin(), bytes.end());
    state_directory = std::move(directory);
}

std::vector<char> read_record(const std::filesystem::path& file)
{
    std::ifstream input(file, std::ios::binary | std::ios::ate);
    if (!input) throw std::runtime_error("Record missing");
    auto size = input.tellg();
    if (size <= 0 || size > max_record_size) throw std::runtime_error("Invalid pair record size");
    std::vector<char> bytes(static_cast<size_t>(size));
    input.seekg(0);
    if (!input.read(bytes.data(), bytes.size())) throw std::runtime_error("Record read failed");
    return bytes;
}

std::string new_uuid()
{
    GUID id{};
    if (FAILED(CoCreateGuid(&id))) throw std::runtime_error("UUID generation failed");
    char output[37];
    snprintf(output, sizeof(output), "%08lX-%04X-%04X-%02X%02X-%02X%02X%02X%02X%02X%02X",
        id.Data1, id.Data2, id.Data3, id.Data4[0], id.Data4[1], id.Data4[2], id.Data4[3],
        id.Data4[4], id.Data4[5], id.Data4[6], id.Data4[7]);
    return output;
}

void write_record(const std::filesystem::path& file, const char* data, size_t size)
{
    auto temporary = file;
    temporary += L"." + std::filesystem::path(new_uuid()).wstring() + L".tmp";
    try {
        HANDLE handle = CreateFileW(temporary.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (handle == INVALID_HANDLE_VALUE) throw std::runtime_error("Record create failed");
        DWORD written = 0;
        BOOL ok = WriteFile(handle, data, static_cast<DWORD>(size), &written, nullptr);
        BOOL flushed = FlushFileBuffers(handle);
        CloseHandle(handle);
        if (!ok || written != size || !flushed) throw std::runtime_error("Record write failed");
        if (!MoveFileExW(temporary.c_str(), file.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
            throw std::runtime_error("Record replace failed");
    } catch (...) {
        DeleteFileW(temporary.c_str());
        throw;
    }
}
}

extern "C" const char* config_get_config_dir()
{
    std::lock_guard lock(state_mutex);
    try { initialize(); return directory_utf8.c_str(); } catch (...) { return nullptr; }
}

extern "C" int config_set_config_dir(const char* path)
{
    std::lock_guard lock(state_mutex);
    try {
        if (!path || !*path) return -EINVAL;
        auto directory = std::filesystem::u8path(path);
        if (!directory.is_absolute()) return -EINVAL;
        protect_directory(directory);
        state_directory = std::move(directory);
        directory_utf8 = path;
        return 0;
    } catch (...) { return -EIO; }
}

extern "C" int config_get_device_record(const char* udid, char** data, uint64_t* size)
{
    std::lock_guard lock(state_mutex);
    if (!data || !size || !valid_identifier(udid)) return -EINVAL;
    *data = nullptr;
    *size = 0;
    try {
        initialize();
        auto file = state_directory / (std::string(udid) + ".plist");
        if (!std::filesystem::exists(file)) return -ENOENT;
        auto bytes = read_record(file);
        *data = static_cast<char*>(malloc(bytes.size()));
        if (!*data) return -ENOMEM;
        memcpy(*data, bytes.data(), bytes.size());
        *size = bytes.size();
        return 0;
    } catch (...) { return -EIO; }
}

extern "C" int config_has_device_record(const char* udid)
{
    char* data = nullptr;
    uint64_t size = 0;
    int result = config_get_device_record(udid, &data, &size);
    free(data);
    return result == 0;
}

extern "C" int config_set_device_record(const char* udid, char* data, uint64_t size)
{
    std::lock_guard lock(state_mutex);
    if (!valid_identifier(udid) || !data || size == 0 || size > max_record_size) return -EINVAL;
    try {
        initialize();
        write_record(state_directory / (std::string(udid) + ".plist"), data, static_cast<size_t>(size));
        return 0;
    } catch (...) { return -EIO; }
}

extern "C" int config_remove_device_record(const char* udid)
{
    std::lock_guard lock(state_mutex);
    if (!valid_identifier(udid)) return -EINVAL;
    try {
        initialize();
        return std::filesystem::remove(state_directory / (std::string(udid) + ".plist")) ? 0 : -ENOENT;
    } catch (...) { return -EIO; }
}

extern "C" void config_get_system_buid(char** buid)
{
    std::lock_guard lock(state_mutex);
    if (!buid) return;
    *buid = nullptr;
    try {
        initialize();
        auto file = state_directory / L"SystemBUID.txt";
        if (std::filesystem::exists(file)) {
            auto bytes = read_record(file);
            std::string stored(bytes.begin(), bytes.end());
            if (stored.size() != 36 || !valid_identifier(stored.c_str())) throw std::runtime_error("Invalid BUID");
            *buid = _strdup(stored.c_str());
        } else {
            auto generated = new_uuid();
            write_record(file, generated.data(), generated.size());
            *buid = _strdup(generated.c_str());
        }
    } catch (...) { }
}

extern "C" void config_device_record_get_host_id(const char* udid, char** host_id)
{
    if (!host_id) return;
    *host_id = nullptr;
    char* data = nullptr;
    uint64_t size = 0;
    if (config_get_device_record(udid, &data, &size) != 0) return;
    plist_t record = nullptr;
    plist_from_memory(data, static_cast<uint32_t>(size), &record, nullptr);
    free(data);
    if (record) {
        plist_t item = plist_dict_get_item(record, "HostID");
        if (item && plist_get_node_type(item) == PLIST_STRING) plist_get_string_val(item, host_id);
        plist_free(record);
    }
}
