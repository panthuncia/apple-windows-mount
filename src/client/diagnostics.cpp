// SPDX-License-Identifier: GPL-3.0-or-later
#include "device.h"
#include <winsock2.h>
#include <windows.h>
#include <setupapi.h>
#include <libusb.h>
#include <vector>
#include <string>

namespace awm {
namespace {
std::string utf8(const wchar_t* input)
{
    if (!input || !*input) return {};
    int count = WideCharToMultiByte(CP_UTF8, 0, input, -1, nullptr, 0, nullptr, nullptr);
    std::string output(count, '\0');
    WideCharToMultiByte(CP_UTF8, 0, input, -1, output.data(), count, nullptr, nullptr);
    output.pop_back();
    return output;
}

std::string property(HDEVINFO set, SP_DEVINFO_DATA& device, DWORD key)
{
    wchar_t buffer[4096]{};
    if (!SetupDiGetDeviceRegistryPropertyW(set, &device, key, nullptr, reinterpret_cast<PBYTE>(buffer), sizeof(buffer), nullptr)) return {};
    return utf8(buffer);
}

std::string registry_string(HKEY key, const wchar_t* name)
{
    wchar_t buffer[4096]{};
    DWORD size = sizeof(buffer);
    if (RegGetValueW(key, nullptr, name, RRF_RT_REG_SZ, nullptr, buffer, &size) != ERROR_SUCCESS) return {};
    return utf8(buffer);
}
}

Json diagnostics(bool usbdk)
{
    const char* endpoint = getenv("USBMUXD_SOCKET_ADDRESS");
    Json report{{"endpoint", endpoint ? endpoint : mux_endpoint}, {"backend", usbdk ? "usbdk" : "winusb"},
        {"pnp_devices", Json::array()}, {"usb_devices", Json::array()}, {"apple_services", Json::array()}};
    bool apple_binding = false;
    HDEVINFO set = SetupDiGetClassDevsW(nullptr, L"USB", nullptr, DIGCF_PRESENT | DIGCF_ALLCLASSES);
    if (set == INVALID_HANDLE_VALUE) throw std::runtime_error("Cannot enumerate USB device bindings");
    SP_DEVINFO_DATA device{sizeof(device)};
    for (DWORD index = 0; SetupDiEnumDeviceInfo(set, index, &device); ++index) {
        auto hardware = property(set, device, SPDRP_HARDWAREID);
        if (hardware.find("VID_05AC") == std::string::npos && hardware.find("VID_05ac") == std::string::npos) continue;
        HKEY driver = SetupDiOpenDevRegKey(set, &device, DICS_FLAG_GLOBAL, 0, DIREG_DRV, KEY_READ);
        Json item{{"hardware_id", hardware}, {"name", property(set, device, SPDRP_DEVICEDESC)},
            {"service", property(set, device, SPDRP_SERVICE)}, {"upper_filter", property(set, device, SPDRP_UPPERFILTERS)},
            {"lower_filter", property(set, device, SPDRP_LOWERFILTERS)}};
        if (driver != INVALID_HANDLE_VALUE) {
            auto provider = registry_string(driver, L"ProviderName");
            item["provider"] = provider;
            item["inf"] = registry_string(driver, L"InfPath");
            item["version"] = registry_string(driver, L"DriverVersion");
            apple_binding |= provider.find("Apple") != std::string::npos;
            RegCloseKey(driver);
        }
        report["pnp_devices"].push_back(item);
    }
    SetupDiDestroyDeviceInfoList(set);
    SC_HANDLE manager = OpenSCManagerW(nullptr, nullptr, SC_MANAGER_ENUMERATE_SERVICE);
    if (!manager) throw std::runtime_error("Cannot inspect installed services");
    DWORD size = 0, count = 0, resume = 0;
    EnumServicesStatusExW(manager, SC_ENUM_PROCESS_INFO, SERVICE_WIN32, SERVICE_STATE_ALL, nullptr, 0, &size, &count, &resume, nullptr);
    std::vector<BYTE> service_buffer(size);
    resume = 0;
    if (!service_buffer.empty() && !EnumServicesStatusExW(manager, SC_ENUM_PROCESS_INFO, SERVICE_WIN32, SERVICE_STATE_ALL,
        service_buffer.data(), size, &size, &count, &resume, nullptr)) {
        CloseServiceHandle(manager);
        throw std::runtime_error("Cannot enumerate installed services");
    }
    auto services = reinterpret_cast<ENUM_SERVICE_STATUS_PROCESSW*>(service_buffer.data());
    for (DWORD index = 0; index < count; ++index) {
        auto name = utf8(services[index].lpServiceName);
        if (name.find("Apple") != std::string::npos || name.find("Bonjour") != std::string::npos)
            report["apple_services"].push_back({{"name", name}, {"running", services[index].ServiceStatusProcess.dwCurrentState == SERVICE_RUNNING}});
    }
    CloseServiceHandle(manager);
    report["apple_free_environment"] = !apple_binding && report["apple_services"].empty();
    // This observation does not replace clean-machine acceptance testing.
    report["hardware_acceptance_passed"] = false;
    try { report["devices"] = devices(); report["transport_available"] = true; }
    catch (const std::exception& error) { report["transport_available"] = false; report["transport_error"] = error.what(); }

    libusb_context* context = nullptr;
    int status = libusb_init(&context);
    if (status != LIBUSB_SUCCESS) { report["usb_error"] = libusb_error_name(status); return report; }
    if (usbdk) {
        status = libusb_set_option(context, LIBUSB_OPTION_USE_USBDK);
        if (status != LIBUSB_SUCCESS) {
            report["usb_error"] = libusb_error_name(status);
            libusb_exit(context);
            return report;
        }
    }
    libusb_device** list = nullptr;
    auto devices_count = libusb_get_device_list(context, &list);
    if (devices_count < 0) {
        report["usb_error"] = libusb_error_name(static_cast<int>(devices_count));
    } else for (ssize_t index = 0; index < devices_count; ++index) {
        libusb_device_descriptor descriptor{};
        if (libusb_get_device_descriptor(list[index], &descriptor) != 0 || descriptor.idVendor != 0x05ac) continue;
        Json entry{{"vendor_id", descriptor.idVendor}, {"product_id", descriptor.idProduct},
            {"bus", libusb_get_bus_number(list[index])}, {"address", libusb_get_device_address(list[index])},
            {"configurations", Json::array()}};
        libusb_device_handle* handle = nullptr;
        int opened = libusb_open(list[index], &handle);
        if (opened == 0) {
            int configuration = 0;
            int queried = libusb_get_configuration(handle, &configuration);
            if (queried == 0) entry["active_configuration"] = configuration;
            else entry["configuration_error"] = libusb_error_name(queried);
            libusb_close(handle);
        } else entry["open_error"] = libusb_error_name(opened);
        for (uint8_t config_index = 0; config_index < descriptor.bNumConfigurations; ++config_index) {
            libusb_config_descriptor* config = nullptr;
            if (libusb_get_config_descriptor(list[index], config_index, &config) != 0) continue;
            Json interfaces = Json::array();
            for (uint8_t interface_index = 0; interface_index < config->bNumInterfaces; ++interface_index) {
                for (int alternate = 0; alternate < config->interface[interface_index].num_altsetting; ++alternate) {
                    auto& interface = config->interface[interface_index].altsetting[alternate];
                    bool mux = interface.bInterfaceClass == 255 && interface.bInterfaceSubClass == 254 && interface.bInterfaceProtocol == 2;
                    if (mux) interfaces.push_back({{"interface", interface.bInterfaceNumber}, {"alternate", interface.bAlternateSetting}, {"endpoints", interface.bNumEndpoints}});
                }
            }
            entry["configurations"].push_back({{"value", config->bConfigurationValue}, {"mux_interfaces", interfaces}});
            libusb_free_config_descriptor(config);
        }
        report["usb_devices"].push_back(entry);
    }
    if (list) libusb_free_device_list(list, 1);
    libusb_exit(context);
    return report;
}
}
