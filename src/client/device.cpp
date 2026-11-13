// SPDX-License-Identifier: GPL-3.0-or-later
#include "device.h"
#include <algorithm>
#include <array>
#include <chrono>
#include <iomanip>
#include <sstream>
#include <vector>
#include <winsock2.h>
#include <windows.h>
#include <bcrypt.h>
#include <libimobiledevice/installation_proxy.h>
#include <usbmuxd.h>

namespace awm {
namespace {
struct PlistDeleter { void operator()(void* value) const { plist_free(value); } };
using Plist = std::unique_ptr<void, PlistDeleter>;
struct DictionaryDeleter { void operator()(char** value) const { afc_dictionary_free(value); } };
using Dictionary = std::unique_ptr<char*, DictionaryDeleter>;

std::string string_value(plist_t item)
{
    if (!item || plist_get_node_type(item) != PLIST_STRING) return {};
    char* value = nullptr;
    plist_get_string_val(item, &value);
    std::string result = value ? value : "";
    free(value);
    return result;
}

std::string guidance(lockdownd_error_t error)
{
    switch (error) {
    case LOCKDOWN_E_PASSWORD_PROTECTED: return "Unlock the device and retry.";
    case LOCKDOWN_E_PAIRING_DIALOG_RESPONSE_PENDING: return "Accept Trust This Computer on the device and retry.";
    case LOCKDOWN_E_USER_DENIED_PAIRING: return "The device rejected pairing. Reconnect and accept Trust This Computer.";
    case LOCKDOWN_E_INVALID_HOST_ID: return "Pair this device with awm pair --udid UDID.";
    default: return "Check the USB connection, unlock the device, and run awm doctor.";
    }
}

void check_afc(afc_error_t error, const char* operation)
{
    if (error != AFC_E_SUCCESS) throw DeviceError(operation, error);
}

class RemoteFile {
public:
    RemoteFile(afc_client_t client, const std::string& path, afc_file_mode_t mode) : client_(client)
    {
        check_afc(afc_file_open(client_, path.c_str(), mode, &handle_), "AFC open");
    }
    ~RemoteFile() { if (open_) afc_file_close(client_, handle_); }
    void close()
    {
        auto result = afc_file_close(client_, handle_);
        open_ = false;
        check_afc(result, "AFC close");
    }
    uint64_t get() const { return handle_; }
private:
    afc_client_t client_;
    uint64_t handle_ = 0;
    bool open_ = true;
};

class Sha256 {
public:
    Sha256()
    {
        if (BCryptOpenAlgorithmProvider(&algorithm_, BCRYPT_SHA256_ALGORITHM, nullptr, 0) < 0)
            throw std::runtime_error("SHA256 provider unavailable");
        if (BCryptCreateHash(algorithm_, &hash_, nullptr, 0, nullptr, 0, 0) < 0) {
            BCryptCloseAlgorithmProvider(algorithm_, 0);
            throw std::runtime_error("SHA256 initialization failed");
        }
    }
    ~Sha256() { BCryptDestroyHash(hash_); BCryptCloseAlgorithmProvider(algorithm_, 0); }
    void update(const char* data, uint32_t count)
    {
        if (BCryptHashData(hash_, reinterpret_cast<PUCHAR>(const_cast<char*>(data)), count, 0) < 0)
            throw std::runtime_error("SHA256 update failed");
    }
    std::string finish()
    {
        std::array<unsigned char, 32> digest{};
        if (BCryptFinishHash(hash_, digest.data(), static_cast<ULONG>(digest.size()), 0) < 0)
            throw std::runtime_error("SHA256 finalization failed");
        std::ostringstream output;
        for (auto byte : digest) output << std::hex << std::setw(2) << std::setfill('0') << static_cast<unsigned>(byte);
        return output.str();
    }
private:
    BCRYPT_ALG_HANDLE algorithm_ = nullptr;
    BCRYPT_HASH_HANDLE hash_ = nullptr;
};
}

DeviceError::DeviceError(std::string operation, int code, std::string advice)
    : std::runtime_error(operation + " failed (" + std::to_string(code) + "). " + advice), code_(code) {}

Json devices()
{
    // Distinguish an unavailable broker from an empty USB device list.
    usbmuxd_device_info_t* mux_list = nullptr;
    int count = usbmuxd_get_device_list(&mux_list);
    if (count < 0) throw DeviceError("Transport discovery", count, "Start awm-usbmuxd, then run awm doctor.");
    Json result = Json::array();
    for (int index = 0; index < count; ++index) {
        if (mux_list[index].conn_type == CONNECTION_TYPE_USB)
            result.push_back({{"udid", mux_list[index].udid}, {"connection", "usb"}, {"product_id", mux_list[index].product_id}});
    }
    usbmuxd_device_list_free(&mux_list);
    return result;
}

Device::Device(const std::string& udid) : udid_(udid)
{
    idevice_t raw = nullptr;
    auto result = idevice_new_with_options(&raw, udid.c_str(), IDEVICE_LOOKUP_USBMUX);
    if (result != IDEVICE_E_SUCCESS) throw DeviceError("Open device", result);
    device_.reset(raw);
}

LockdownHandle Device::connect(bool handshake)
{
    lockdownd_client_t raw = nullptr;
    auto result = handshake ? lockdownd_client_new_with_handshake(device_.get(), &raw, "apple-windows-mount")
                            : lockdownd_client_new(device_.get(), &raw, "apple-windows-mount");
    LockdownHandle handle(raw);
    if (result != LOCKDOWN_E_SUCCESS) throw DeviceError("Lockdown connection", result, guidance(result));
    return handle;
}

Json Device::pair()
{
    // The handshake pairs when necessary and validates the resulting SSL session.
    auto lockdown = connect(true);
    return {{"udid", udid_}, {"paired", true}};
}

Json Device::unpair()
{
    auto lockdown = connect(false);
    auto result = lockdownd_unpair(lockdown.get(), nullptr);
    if (result != LOCKDOWN_E_SUCCESS) throw DeviceError("Unpair", result, guidance(result));
    return {{"udid", udid_}, {"paired", false}};
}

Json Device::info()
{
    auto lockdown = connect(true);
    Json output{{"udid", udid_}};
    for (const char* key : {"DeviceName", "ProductType", "ProductVersion", "DeviceClass"}) {
        plist_t value = nullptr;
        auto result = lockdownd_get_value(lockdown.get(), nullptr, key, &value);
        Plist owner(value);
        if (result != LOCKDOWN_E_SUCCESS) throw DeviceError("Device information", result);
        output[key] = string_value(value);
    }
    return output;
}

Json Device::apps()
{
    auto lockdown = connect(true);
    instproxy_client_t raw = nullptr;
    auto result = instproxy_client_start_service(device_.get(), &raw, "apple-windows-mount");
    std::unique_ptr<instproxy_client_private, decltype(&instproxy_client_free)> client(raw, instproxy_client_free);
    if (result != INSTPROXY_E_SUCCESS) throw DeviceError("Application discovery", result);
    Plist options(instproxy_client_options_new());
    instproxy_client_options_add(options.get(), "ApplicationType", "Any", nullptr);
    instproxy_client_options_set_return_attributes(options.get(), "CFBundleIdentifier", "CFBundleDisplayName", "UIFileSharingEnabled", nullptr);
    plist_t raw_apps = nullptr;
    result = instproxy_browse(client.get(), options.get(), &raw_apps);
    Plist apps(raw_apps);
    if (result != INSTPROXY_E_SUCCESS || !raw_apps || plist_get_node_type(raw_apps) != PLIST_ARRAY)
        throw DeviceError("Application enumeration", result);
    Json output = Json::array();
    for (uint32_t index = 0; index < plist_array_get_size(raw_apps); ++index) {
        auto app = plist_array_get_item(raw_apps, index);
        auto enabled = plist_dict_get_item(app, "UIFileSharingEnabled");
        uint8_t sharing = 0;
        if (enabled && plist_get_node_type(enabled) == PLIST_BOOLEAN) plist_get_bool_val(enabled, &sharing);
        if (sharing) output.push_back({{"bundle_id", string_value(plist_dict_get_item(app, "CFBundleIdentifier"))},
            {"name", string_value(plist_dict_get_item(app, "CFBundleDisplayName"))}});
    }
    return output;
}

AfcHandle Device::media()
{
    service_session_ = connect(true);
    lockdownd_service_descriptor_t descriptor = nullptr;
    auto started = lockdownd_start_service(service_session_.get(), "com.apple.afc", &descriptor);
    std::unique_ptr<lockdownd_service_descriptor, decltype(&lockdownd_service_descriptor_free)>
        service(descriptor, lockdownd_service_descriptor_free);
    if (started != LOCKDOWN_E_SUCCESS) throw DeviceError("Start media service", started, guidance(started));
    afc_client_t raw = nullptr;
    auto result = afc_client_new(device_.get(), descriptor, &raw);
    AfcHandle owner(raw);
    check_afc(result, "Start AFC");
    return owner;
}

Json Device::list(const std::string& path)
{
    auto client = media();
    char** raw = nullptr;
    auto result = afc_read_directory(client.get(), path.c_str(), &raw);
    Dictionary entries(raw);
    check_afc(result, "List directory");
    Json output = Json::array();
    if (raw) for (size_t index = 0; raw[index]; ++index) {
        std::string name(raw[index]);
        if (name != "." && name != "..") output.push_back(name);
    }
    return output;
}

Json Device::transfer_probe(const std::string& directory, uint64_t bytes)
{
    if (directory.empty() || directory.front() != '/' || directory.find("..") != std::string::npos)
        throw std::runtime_error("Probe directory must be an absolute AFC path without parent traversal");
    if (!bytes) throw std::runtime_error("Probe size must be positive");
    auto client = media();
    std::array<unsigned char, 16> random{};
    if (BCryptGenRandom(nullptr, random.data(), static_cast<ULONG>(random.size()), BCRYPT_USE_SYSTEM_PREFERRED_RNG) < 0)
        throw std::runtime_error("Random generator unavailable");
    std::ostringstream suffix;
    for (auto byte : random) suffix << std::hex << std::setw(2) << std::setfill('0') << static_cast<unsigned>(byte);
    auto path = directory + (directory.back() == '/' ? "" : "/") + ".awm-probe-" + suffix.str();
    check_afc(afc_make_directory(client.get(), path.c_str()), "Create unique probe directory");
    const auto file = path + "/payload.bin";
    try {
        Sha256 sent_hash, received_hash;
        std::vector<char> buffer(256 * 1024);
        auto start = std::chrono::steady_clock::now();
        RemoteFile writer(client.get(), file, AFC_FOPEN_WRONLY);
        for (uint64_t offset = 0; offset < bytes;) {
            auto count = static_cast<uint32_t>(std::min<uint64_t>(buffer.size(), bytes - offset));
            if (BCryptGenRandom(nullptr, reinterpret_cast<PUCHAR>(buffer.data()), count, BCRYPT_USE_SYSTEM_PREFERRED_RNG) < 0)
                throw std::runtime_error("Probe payload generation failed");
            uint32_t completed = 0;
            while (completed < count) {
                uint32_t written = 0;
                check_afc(afc_file_write(client.get(), writer.get(), buffer.data() + completed, count - completed, &written), "Probe write");
                if (!written || written > count - completed) throw std::runtime_error("Invalid AFC short write");
                completed += written;
            }
            sent_hash.update(buffer.data(), count);
            offset += count;
        }
        writer.close();
        RemoteFile reader(client.get(), file, AFC_FOPEN_RDONLY);
        for (uint64_t offset = 0; offset < bytes;) {
            auto count = static_cast<uint32_t>(std::min<uint64_t>(buffer.size(), bytes - offset));
            uint32_t received = 0;
            check_afc(afc_file_read(client.get(), reader.get(), buffer.data(), count, &received), "Probe read");
            if (!received || received > count) throw std::runtime_error("Unexpected EOF during probe");
            received_hash.update(buffer.data(), received);
            offset += received;
        }
        uint32_t extra = 0;
        auto end = afc_file_read(client.get(), reader.get(), buffer.data(), 1, &extra);
        if ((end != AFC_E_SUCCESS && end != AFC_E_END_OF_DATA) || extra != 0)
            throw std::runtime_error("Probe file has an unexpected size");
        reader.close();
        auto sent = sent_hash.finish();
        auto received = received_hash.finish();
        if (sent != received) throw std::runtime_error("Probe SHA256 mismatch");
        check_afc(afc_remove_path(client.get(), file.c_str()), "Remove probe payload");
        check_afc(afc_remove_path(client.get(), path.c_str()), "Remove probe directory");
        auto seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
        return {{"udid", udid_}, {"bytes_each_direction", bytes}, {"sha256", sent}, {"verified", true}, {"elapsed_seconds", seconds}};
    } catch (const std::exception& error) {
        auto removed_file = afc_remove_path(client.get(), file.c_str());
        auto removed_directory = afc_remove_path(client.get(), path.c_str());
        if ((removed_file != AFC_E_SUCCESS && removed_file != AFC_E_OBJECT_NOT_FOUND) || removed_directory != AFC_E_SUCCESS)
            throw std::runtime_error(std::string(error.what()) + ". Probe cleanup incomplete at " + path);
        throw;
    }
}
}
