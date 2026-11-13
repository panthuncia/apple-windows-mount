// SPDX-License-Identifier: GPL-3.0-or-later
#include "afc_session.h"
#include <charconv>
#include <algorithm>
#include <limits>
namespace awm {
namespace {
struct Dictionary {
    char** value = nullptr;
    ~Dictionary() { if (value) afc_dictionary_free(value); }
    uint64_t number(const char* key) const {
        std::string field = text(key);
        if (field.empty()) return 0;
        uint64_t output = 0;
        auto parsed = std::from_chars(field.data(), field.data() + field.size(), output);
        if (parsed.ec != std::errc{} || parsed.ptr != field.data() + field.size()) throw DeviceError("Malformed AFC metadata", AFC_E_OP_HEADER_INVALID);
        return output;
    }
    std::string text(const char* key) const {
        if (value) for (size_t index = 0; value[index] && value[index + 1]; index += 2)
            if (std::string_view(value[index]) == key) return value[index + 1];
        return {};
    }
};
}
void AfcSession::check(afc_error_t status, const char* operation)
{
    if (status == AFC_E_MUX_ERROR || status == AFC_E_SERVICE_NOT_CONNECTED || status == AFC_E_OP_TIMEOUT) failed_ = true;
    if (status != AFC_E_SUCCESS) throw DeviceError(operation, status);
}
void AfcSession::ensure() { if (failed_) throw DeviceError("File service session ended", AFC_E_SERVICE_NOT_CONNECTED); }
AfcSession::AfcSession(const std::string& udid, const std::string& app)
{
    idevice_t device = nullptr;
    int status = idevice_new_with_options(&device, udid.c_str(), IDEVICE_LOOKUP_USBMUX);
    if (status != IDEVICE_E_SUCCESS) throw DeviceError("Connect device", AFC_E_SERVICE_NOT_CONNECTED);
    device_.reset(device);
    lockdownd_client_t lockdown = nullptr;
    status = lockdownd_client_new_with_handshake(device, &lockdown, "apple-windows-mount");
    lockdown_.reset(lockdown);
    if (status != LOCKDOWN_E_SUCCESS) throw DeviceError("Validate device trust", AFC_E_PERM_DENIED);
    lockdownd_service_descriptor_t descriptor = nullptr;
    status = lockdownd_start_service(lockdown, app.empty() ? "com.apple.afc" : "com.apple.mobile.house_arrest", &descriptor);
    std::unique_ptr<lockdownd_service_descriptor, decltype(&lockdownd_service_descriptor_free)> service(descriptor, lockdownd_service_descriptor_free);
    if (status != LOCKDOWN_E_SUCCESS) throw DeviceError("Start file service", AFC_E_SERVICE_NOT_CONNECTED);
    afc_client_t afc = nullptr;
    if (app.empty()) {
        check(afc_client_new(device, descriptor, &afc), "Connect media AFC");
    } else {
        house_arrest_client_t house = nullptr;
        status = house_arrest_client_new(device, descriptor, &house);
        house_.reset(house);
        if (status != HOUSE_ARREST_E_SUCCESS) throw DeviceError("Connect app documents", AFC_E_PERM_DENIED);
        if (house_arrest_send_command(house, "VendDocuments", app.c_str()) != HOUSE_ARREST_E_SUCCESS)
            throw DeviceError("Request app documents", AFC_E_PERM_DENIED);
        plist_t response = nullptr;
        status = house_arrest_get_result(house, &response);
        bool failed = status != HOUSE_ARREST_E_SUCCESS || !response || plist_dict_get_item(response, "Error");
        if (response) plist_free(response);
        if (failed) throw DeviceError("App documents unavailable", AFC_E_PERM_DENIED);
        check(afc_client_new_from_house_arrest_client(house, &afc), "Connect app AFC");
        root_ = "/Documents";
    }
    afc_.reset(afc);
}
std::string AfcSession::rooted(const std::string& path) const { return root_ + path; }
Metadata AfcSession::stat(const std::string& path)
{
    std::lock_guard lock(mutex_); ensure();
    Dictionary data;
    check(afc_get_file_info(afc_.get(), rooted(path).c_str(), &data.value), "Read file metadata");
    Metadata info;
    info.directory = data.text("st_ifmt") == "S_IFDIR";
    info.symlink = data.text("st_ifmt") == "S_IFLNK";
    info.size = data.number("st_size");
    info.modified_ns = data.number("st_mtime");
    info.created_ns = data.number("st_birthtime");
    return info;
}
std::vector<std::string> AfcSession::list(const std::string& path)
{
    std::lock_guard lock(mutex_); ensure();
    Dictionary data;
    check(afc_read_directory(afc_.get(), rooted(path).c_str(), &data.value), "Read directory");
    std::vector<std::string> output;
    if (data.value) for (size_t index = 0; data.value[index]; ++index) {
        std::string entry = data.value[index];
        if (entry != "." && entry != "..") output.push_back(entry);
    }
    return output;
}
std::pair<uint64_t, uint64_t> AfcSession::capacity()
{
    std::lock_guard lock(mutex_); ensure();
    Dictionary data;
    check(afc_get_device_info(afc_.get(), &data.value), "Read capacity");
    return {data.number("FSTotalBytes"), data.number("FSFreeBytes")};
}
uint64_t AfcSession::open(const std::string& path, bool writable)
{
    std::lock_guard lock(mutex_); ensure();
    uint64_t handle = 0;
    check(afc_file_open(afc_.get(), rooted(path).c_str(), writable ? AFC_FOPEN_RW : AFC_FOPEN_RDONLY, &handle), "Open file");
    return handle;
}
void AfcSession::close(uint64_t handle) { std::lock_guard lock(mutex_); ensure(); check(afc_file_close(afc_.get(), handle), "Close file"); }
uint32_t AfcSession::read(uint64_t handle, uint64_t offset, void* buffer, uint32_t length)
{
    std::lock_guard lock(mutex_); ensure();
    check(afc_file_seek(afc_.get(), handle, static_cast<int64_t>(offset), SEEK_SET), "Seek file");
    uint32_t total = 0;
    while (total < length) {
        uint32_t bytes = 0;
        auto status = afc_file_read(afc_.get(), handle, static_cast<char*>(buffer) + total, std::min<uint32_t>(length - total, 256 * 1024), &bytes);
        if (status == AFC_E_END_OF_DATA) break;
        check(status, "Read file");
        if (!bytes) break;
        if (bytes > length - total) throw DeviceError("Invalid read count", AFC_E_OP_HEADER_INVALID);
        total += bytes;
    }
    return total;
}
uint32_t AfcSession::write(uint64_t handle, uint64_t offset, const void* buffer, uint32_t length)
{
    std::lock_guard lock(mutex_); ensure();
    check(afc_file_seek(afc_.get(), handle, static_cast<int64_t>(offset), SEEK_SET), "Seek file");
    uint32_t total = 0;
    while (total < length) {
        uint32_t bytes = 0;
        check(afc_file_write(afc_.get(), handle, static_cast<const char*>(buffer) + total, std::min<uint32_t>(length - total, 256 * 1024), &bytes), "Write file");
        if (!bytes || bytes > length - total) throw DeviceError("Invalid write count", AFC_E_WRITE_ERROR);
        total += bytes;
    }
    return total;
}
void AfcSession::resize(uint64_t handle, uint64_t size) { std::lock_guard lock(mutex_); ensure(); check(afc_file_truncate(afc_.get(), handle, size), "Resize file"); }
void AfcSession::mkdir(const std::string& path) { std::lock_guard lock(mutex_); ensure(); check(afc_make_directory(afc_.get(), rooted(path).c_str()), "Create directory"); }
void AfcSession::remove(const std::string& path) { std::lock_guard lock(mutex_); ensure(); check(afc_remove_path(afc_.get(), rooted(path).c_str()), "Delete entry"); }
void AfcSession::rename(const std::string& from, const std::string& to) { std::lock_guard lock(mutex_); ensure(); check(afc_rename_path(afc_.get(), rooted(from).c_str(), rooted(to).c_str()), "Rename entry"); }
void AfcSession::set_time(const std::string& path, uint64_t ns) { std::lock_guard lock(mutex_); ensure(); check(afc_set_file_time(afc_.get(), rooted(path).c_str(), ns), "Set file time"); }
}
