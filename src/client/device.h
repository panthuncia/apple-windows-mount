// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <memory>
#include <stdexcept>
#include <string>
#include <nlohmann/json.hpp>
#include <libimobiledevice/libimobiledevice.h>
#include <libimobiledevice/lockdown.h>
#include <libimobiledevice/afc.h>

namespace awm {
using Json = nlohmann::json;
inline constexpr char mux_endpoint[] = "127.0.0.1:37491";

class DeviceError : public std::runtime_error {
public:
    DeviceError(std::string operation, int code, std::string guidance = {});
    int code() const noexcept { return code_; }
private:
    int code_;
};

struct DeviceDeleter { void operator()(idevice_t value) const { idevice_free(value); } };
struct LockdownDeleter { void operator()(lockdownd_client_t value) const { lockdownd_client_free(value); } };
struct AfcDeleter { void operator()(afc_client_t value) const { afc_client_free(value); } };
using DeviceHandle = std::unique_ptr<idevice_private, DeviceDeleter>;
using LockdownHandle = std::unique_ptr<lockdownd_client_private, LockdownDeleter>;
using AfcHandle = std::unique_ptr<afc_client_private, AfcDeleter>;

Json devices();
Json diagnostics(bool usbdk);
class Device {
public:
    explicit Device(const std::string& udid);
    Json pair();
    Json unpair();
    Json info();
    Json apps();
    Json list(const std::string& path);
    Json transfer_probe(const std::string& directory, uint64_t bytes);
private:
    LockdownHandle connect(bool handshake);
    AfcHandle media();
    DeviceHandle device_;
    std::string udid_;
    LockdownHandle service_session_;
};
}
