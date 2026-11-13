// SPDX-License-Identifier: GPL-3.0-or-later
#include <windows.h>
#include <setupapi.h>
#include <newdev.h>
#include <cfgmgr32.h>
#include <objbase.h>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <vector>
#include <nlohmann/json.hpp>
#include <libusb.h>

int wmain(int argc, wchar_t** argv);

namespace {
using Json = nlohmann::json;
void check(BOOL success, const char* operation)
{
    if (!success) throw std::runtime_error(std::string(operation) + " failed. Win32 error " + std::to_string(GetLastError()));
}

std::string utf8(const std::wstring& value)
{
    if (value.empty()) return {};
    int size = WideCharToMultiByte(CP_UTF8, 0, value.data(), static_cast<int>(value.size()), nullptr, 0, nullptr, nullptr);
    std::string output(size, '\0');
    WideCharToMultiByte(CP_UTF8, 0, value.data(), static_cast<int>(value.size()), output.data(), size, nullptr, nullptr);
    return output;
}

void require_administrator()
{
    SID_IDENTIFIER_AUTHORITY authority = SECURITY_NT_AUTHORITY;
    PSID administrators = nullptr;
    check(AllocateAndInitializeSid(&authority, 2, SECURITY_BUILTIN_DOMAIN_RID,
        DOMAIN_ALIAS_RID_ADMINS, 0, 0, 0, 0, 0, 0, &administrators), "Create administrator SID");
    BOOL member = FALSE;
    BOOL queried = CheckTokenMembership(nullptr, administrators, &member);
    FreeSid(administrators);
    if (!queried || !member) throw std::runtime_error("Run driver installation or restoration in an elevated terminal");
}
std::wstring wide(const std::string& value)
{
    if (value.empty()) return {};
    int size = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, value.data(), static_cast<int>(value.size()), nullptr, 0);
    if (!size) throw std::runtime_error("Invalid UTF-8");
    std::wstring output(size, L'\0');
    MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, value.data(), static_cast<int>(value.size()), output.data(), size);
    return output;
}

class Device {
public:
    explicit Device(const std::wstring& instance)
    {
        set_ = SetupDiCreateDeviceInfoList(nullptr, nullptr);
        if (set_ == INVALID_HANDLE_VALUE) throw std::runtime_error("Cannot create device set");
        info_.cbSize = sizeof(info_);
        if (!SetupDiOpenDeviceInfoW(set_, instance.c_str(), nullptr, 0, &info_)) {
            SetupDiDestroyDeviceInfoList(set_);
            throw std::runtime_error("Cannot open selected USB device");
        }
    }
    ~Device() { SetupDiDestroyDeviceInfoList(set_); }
    Device(const Device&) = delete;
    Device& operator=(const Device&) = delete;

    Json property(DWORD key)
    {
        DWORD type = 0, size = 0;
        SetupDiGetDeviceRegistryPropertyW(set_, &info_, key, &type, nullptr, 0, &size);
        if (!size) return nullptr;
        std::vector<BYTE> bytes(size);
        check(SetupDiGetDeviceRegistryPropertyW(set_, &info_, key, &type, bytes.data(), size, nullptr), "Read device property");
        return {{"type", type}, {"bytes", bytes}};
    }
    void restore_property(DWORD key, const Json& value)
    {
        if (value.is_null()) {
            if (!SetupDiSetDeviceRegistryPropertyW(set_, &info_, key, nullptr, 0) && GetLastError() != ERROR_INVALID_DATA)
                check(FALSE, "Clear device property");
        } else {
            auto bytes = value.at("bytes").get<std::vector<BYTE>>();
            check(SetupDiSetDeviceRegistryPropertyW(set_, &info_, key, bytes.data(), static_cast<DWORD>(bytes.size())), "Restore device property");
        }
    }
    Json parameter(const wchar_t* name)
    {
        HKEY key = SetupDiOpenDevRegKey(set_, &info_, DICS_FLAG_GLOBAL, 0, DIREG_DEV, KEY_READ);
        if (key == INVALID_HANDLE_VALUE) {
            if (GetLastError() == ERROR_FILE_NOT_FOUND || GetLastError() == ERROR_KEY_DOES_NOT_EXIST) return nullptr;
            throw std::runtime_error("Cannot read device hardware key");
        }
        DWORD type = 0, size = 0;
        LSTATUS result = RegQueryValueExW(key, name, nullptr, &type, nullptr, &size);
        if (result == ERROR_FILE_NOT_FOUND) { RegCloseKey(key); return nullptr; }
        if (result != ERROR_SUCCESS) { RegCloseKey(key); throw std::runtime_error("Cannot read device parameter"); }
        std::vector<BYTE> bytes(size);
        result = RegQueryValueExW(key, name, nullptr, &type, bytes.data(), &size);
        RegCloseKey(key);
        if (result != ERROR_SUCCESS) throw std::runtime_error("Cannot read device parameter data");
        return {{"type", type}, {"bytes", bytes}};
    }
    void set_parameter(const wchar_t* name, const Json& value)
    {
        HKEY key = SetupDiCreateDevRegKeyW(set_, &info_, DICS_FLAG_GLOBAL, 0, DIREG_DEV, nullptr, nullptr);
        if (key == INVALID_HANDLE_VALUE) throw std::runtime_error("Cannot write device hardware key");
        LSTATUS result;
        if (value.is_null()) {
            result = RegDeleteValueW(key, name);
            if (result == ERROR_FILE_NOT_FOUND) result = ERROR_SUCCESS;
        } else {
            auto bytes = value.at("bytes").get<std::vector<BYTE>>();
            result = RegSetValueExW(key, name, 0, value.at("type").get<DWORD>(), bytes.data(), static_cast<DWORD>(bytes.size()));
        }
        RegCloseKey(key);
        if (result != ERROR_SUCCESS) throw std::runtime_error("Cannot write device parameter. Error " + std::to_string(result));
    }
    Json backup(const std::wstring& instance)
    {
        HKEY driver = SetupDiOpenDevRegKey(set_, &info_, DICS_FLAG_GLOBAL, 0, DIREG_DRV, KEY_READ);
        if (driver == INVALID_HANDLE_VALUE) throw std::runtime_error("Cannot back up current driver");
        wchar_t inf[MAX_PATH]{}, section[MAX_PATH]{};
        DWORD size = sizeof(inf);
        LSTATUS first = RegGetValueW(driver, nullptr, L"InfPath", RRF_RT_REG_SZ, nullptr, inf, &size);
        size = sizeof(section);
        LSTATUS second = RegGetValueW(driver, nullptr, L"InfSection", RRF_RT_REG_SZ, nullptr, section, &size);
        RegCloseKey(driver);
        if (first != ERROR_SUCCESS || second != ERROR_SUCCESS) throw std::runtime_error("Current driver metadata missing");
        return {{"instance", utf8(instance)}, {"inf", utf8(inf)}, {"section", utf8(section)},
            {"upper_filters", property(SPDRP_UPPERFILTERS)}, {"lower_filters", property(SPDRP_LOWERFILTERS)},
            {"original_configuration", parameter(L"OriginalConfigurationValue")},
            {"interface_guids", parameter(L"DeviceInterfaceGUIDs")}};
    }
    bool install(const std::wstring& inf, const std::wstring& section)
    {
        GUID driver_class{};
        wchar_t class_name[MAX_CLASS_NAME_LEN]{};
        check(SetupDiGetINFClassW(inf.c_str(), &driver_class, class_name, MAX_CLASS_NAME_LEN, nullptr), "Read INF class");
        if (!IsEqualGUID(info_.ClassGuid, driver_class)) {
            wchar_t class_guid[40];
            if (!StringFromGUID2(driver_class, class_guid, 40)) throw std::runtime_error("Invalid INF class GUID");
            check(SetupDiSetDeviceRegistryPropertyW(set_, &info_, SPDRP_CLASSGUID,
                reinterpret_cast<const BYTE*>(class_guid), static_cast<DWORD>((wcslen(class_guid) + 1) * sizeof(wchar_t))), "Set driver setup class");
        }
        SP_DEVINSTALL_PARAMS_W parameters{sizeof(parameters)};
        check(SetupDiGetDeviceInstallParamsW(set_, &info_, &parameters), "Get global driver search parameters");
        parameters.Flags |= DI_ENUMSINGLEINF;
        parameters.FlagsEx |= DI_FLAGSEX_ALLOWEXCLUDEDDRVS;
        if (inf.size() >= MAX_PATH) throw std::runtime_error("INF path too long");
        wcscpy_s(parameters.DriverPath, inf.c_str());
        check(SetupDiSetDeviceInstallParamsW(set_, &info_, &parameters), "Set global driver search parameters");
        check(SetupDiBuildDriverInfoList(set_, &info_, SPDIT_CLASSDRIVER), "Build inbox driver list");
        SP_DRVINFO_DATA_W driver{sizeof(driver)};
        for (DWORD index = 0; SetupDiEnumDriverInfoW(set_, &info_, SPDIT_CLASSDRIVER, index, &driver); ++index) {
            DWORD size = 0;
            SetupDiGetDriverInfoDetailW(set_, &info_, &driver, nullptr, 0, &size);
            std::vector<BYTE> buffer(std::max<size_t>(size, sizeof(SP_DRVINFO_DETAIL_DATA_W)));
            auto detail = reinterpret_cast<SP_DRVINFO_DETAIL_DATA_W*>(buffer.data());
            detail->cbSize = sizeof(*detail);
            check(SetupDiGetDriverInfoDetailW(set_, &info_, &driver, detail, static_cast<DWORD>(buffer.size()), nullptr), "Read driver detail");
            std::wstring requested = section;
            auto decoration = requested.find(L".NT");
            if (decoration != std::wstring::npos) requested.resize(decoration);
            if (_wcsicmp(detail->SectionName, requested.c_str()) != 0) continue;
            BOOL reboot = FALSE;
            check(DiInstallDevice(nullptr, set_, &info_, &driver, DIIDFLAG_NOFINISHINSTALLUI, &reboot), "Install selected inbox driver");
            return reboot != FALSE;
        }
        throw std::runtime_error("Requested section missing from selected INF");
    }
    bool restart()
    {
        SP_PROPCHANGE_PARAMS change{};
        change.ClassInstallHeader.cbSize = sizeof(SP_CLASSINSTALL_HEADER);
        change.ClassInstallHeader.InstallFunction = DIF_PROPERTYCHANGE;
        change.StateChange = DICS_PROPCHANGE;
        change.Scope = DICS_FLAG_CONFIGSPECIFIC;
        check(SetupDiSetClassInstallParamsW(set_, &info_, &change.ClassInstallHeader, sizeof(change)), "Set restart parameters");
        check(SetupDiCallClassInstaller(DIF_PROPERTYCHANGE, set_, &info_), "Restart selected device");
        SP_DEVINSTALL_PARAMS_W parameters{sizeof(parameters)};
        check(SetupDiGetDeviceInstallParamsW(set_, &info_, &parameters), "Read restart status");
        return (parameters.Flags & (DI_NEEDREBOOT | DI_NEEDRESTART)) != 0;
    }
private:
    HDEVINFO set_ = INVALID_HANDLE_VALUE;
    SP_DEVINFO_DATA info_{};
};

std::wstring windows_inf(const std::wstring& name)
{
    wchar_t directory[MAX_PATH];
    if (!GetWindowsDirectoryW(directory, MAX_PATH)) throw std::runtime_error("Cannot locate Windows directory");
    return std::wstring(directory) + L"\\INF\\" + name;
}
std::wstring find_child(const std::wstring& parent, unsigned interface_number, bool allow_missing = false)
{
    DEVINST node;
    check(CM_Locate_DevNodeW(&node, const_cast<wchar_t*>(parent.c_str()), CM_LOCATE_DEVNODE_NORMAL) == CR_SUCCESS, "Locate composite parent");
    DEVINST child;
    if (CM_Get_Child(&child, node, 0) != CR_SUCCESS) {
        if (allow_missing) return {};
        throw std::runtime_error("Device interfaces unavailable");
    }
    wchar_t marker[16];
    swprintf_s(marker, L"&MI_%02X\\", interface_number);
    do {
        wchar_t instance[MAX_DEVICE_ID_LEN]{};
        if (CM_Get_Device_IDW(child, instance, MAX_DEVICE_ID_LEN, 0) == CR_SUCCESS) {
            std::wstring identifier(instance);
            if (identifier.find(marker) != std::wstring::npos) return identifier;
        }
    } while (CM_Get_Sibling(&child, child, 0) == CR_SUCCESS);
    if (allow_missing) return {};
    throw std::runtime_error("Requested multiplexing interface unavailable");
}
struct Layout { unsigned configuration_index, interface_number; };
std::vector<Layout> layouts(const std::wstring& instance)
{
    auto pid_marker = instance.find(L"&PID_");
    if (pid_marker == std::wstring::npos) throw std::runtime_error("USB product ID missing");
    auto pid = std::stoul(instance.substr(pid_marker + 5, 4), nullptr, 16);
    if (pid < 0x1290 || pid > 0x12af) throw std::runtime_error("Select a normal-mode iOS device");
    libusb_context* context = nullptr;
    if (libusb_init(&context) != 0) throw std::runtime_error("Cannot inspect USB descriptors");
    libusb_device** devices = nullptr;
    auto count = libusb_get_device_list(context, &devices);
    std::vector<Layout> output;
    for (ssize_t device = 0; device < count && output.empty(); ++device) {
        libusb_device_descriptor descriptor{};
        if (libusb_get_device_descriptor(devices[device], &descriptor) || descriptor.idVendor != 0x05ac || descriptor.idProduct != pid) continue;
        for (uint8_t index = 0; index < descriptor.bNumConfigurations; ++index) {
            libusb_config_descriptor* config = nullptr;
            if (libusb_get_config_descriptor(devices[device], index, &config)) continue;
            for (unsigned position = 0; position < config->bNumInterfaces; ++position) {
                if (config->interface[position].num_altsetting == 0) continue;
                auto& interface = config->interface[position].altsetting[0];
                if (interface.bInterfaceClass == 255 && interface.bInterfaceSubClass == 254 && interface.bInterfaceProtocol == 2 && interface.bNumEndpoints == 2)
                    output.push_back({index, interface.bInterfaceNumber});
            }
            libusb_free_config_descriptor(config);
        }
    }
    if (devices) libusb_free_device_list(devices, 1);
    libusb_exit(context);
    return output;
}
Json dword(DWORD value)
{
    auto begin = reinterpret_cast<BYTE*>(&value);
    return {{"type", REG_DWORD}, {"bytes", std::vector<BYTE>(begin, begin + sizeof(value))}};
}
Json multi_string(const std::wstring& value)
{
    std::vector<wchar_t> buffer(value.begin(), value.end());
    buffer.push_back(L'\0');
    buffer.push_back(L'\0');
    auto begin = reinterpret_cast<BYTE*>(buffer.data());
    return {{"type", REG_MULTI_SZ}, {"bytes", std::vector<BYTE>(begin, begin + buffer.size() * sizeof(wchar_t))}};
}
void save(const std::filesystem::path& path, const Json& record)
{
    auto temporary = path;
    temporary += L".tmp";
    {
        std::ofstream stream(temporary, std::ios::binary | std::ios::trunc);
        stream << record.dump(2);
        stream.flush();
        if (!stream) throw std::runtime_error("Cannot write driver journal");
    }
    check(MoveFileExW(temporary.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH), "Commit driver journal");
}

void restore_device(const Json& record)
{
    Device device(wide(record.at("instance").get<std::string>()));
    device.set_parameter(L"OriginalConfigurationValue", record.at("original_configuration"));
    device.install(windows_inf(wide(record.at("inf").get<std::string>())), wide(record.at("section").get<std::string>()));
    device.restore_property(SPDRP_UPPERFILTERS, record.at("upper_filters"));
    device.restore_property(SPDRP_LOWERFILTERS, record.at("lower_filters"));
    device.set_parameter(L"OriginalConfigurationValue", record.at("original_configuration"));
    device.set_parameter(L"DeviceInterfaceGUIDs", record.at("interface_guids"));
    device.restart();
}

void restore_bindings(const Json& record)
{
    std::string errors;
    // A vanished child must not prevent restoration of the parent's configuration.
    for (const char* key : {"interface", "parent"}) {
        if (record.at(key).is_null()) continue;
        try { restore_device(record.at(key)); }
        catch (const std::exception& error) {
            if (!errors.empty()) errors += "; ";
            errors += std::string(key) + ": " + error.what();
        }
    }
    if (!errors.empty()) throw std::runtime_error(errors);
}
}

int wmain(int argc, wchar_t** argv)
{
    try {
        std::wstring command = argc > 1 ? argv[1] : L"help";
        std::wstring instance;
        std::filesystem::path journal;
        std::filesystem::path journal_directory;
        unsigned configuration = 2, interface_number = 1;
        for (int index = 2; index < argc; ++index) {
            std::wstring option = argv[index];
            if (++index == argc) throw std::runtime_error("Missing option value");
            if (option == L"--instance") instance = argv[index];
            else if (option == L"--journal") journal = argv[index];
            else if (option == L"--journal-dir") journal_directory = argv[index];
            else if (option == L"--configuration" || option == L"--interface") {
                std::wstring value = argv[index];
                size_t end = 0;
                auto number = std::stoul(value, &end);
                if (end != value.size() || number > 255) throw std::runtime_error("Invalid USB configuration/interface");
                if (option == L"--configuration") configuration = static_cast<unsigned>(number);
                else interface_number = static_cast<unsigned>(number);
            } else throw std::runtime_error("Unknown driver option");
        }
        if (command == L"help" || command == L"--help") {
            std::cout << "awm-driver inspect|install --instance DEVICE_ID [--configuration 2] [--interface 1] --journal PATH\n"
                "awm-driver restore --journal PATH\n"
                "awm-driver install-connected|restore-connected --journal-dir PATH\n"
                "Configuration is a zero-based descriptor index. Install/restore require elevation. Inspect is read-only.\n";
            return 0;
        }
        if (command == L"install-connected" || command == L"restore-connected") {
            require_administrator();
            if (journal_directory.empty() || !journal_directory.is_absolute()) throw std::runtime_error("An absolute journal directory is required");
            std::filesystem::create_directories(journal_directory);
            std::vector<std::vector<std::wstring>> commands;
            if (command == L"restore-connected") {
                for (const auto& file : std::filesystem::recursive_directory_iterator(journal_directory)) {
                    if (!file.is_regular_file() || file.path().extension() != L".json") continue;
                    Json stored;
                    std::ifstream stream(file.path()); stream >> stored;
                    auto state = stored.value("state", "");
                    if (state == "installed" || state == "parent_installed" || state == "rollback_required")
                        commands.push_back({argv[0], L"restore", L"--journal", file.path().wstring()});
                }
            } else {
                HDEVINFO devices = SetupDiGetClassDevsW(nullptr, L"USB", nullptr, DIGCF_ALLCLASSES | DIGCF_PRESENT);
                if (devices == INVALID_HANDLE_VALUE) throw std::runtime_error("Cannot enumerate connected devices");
                SP_DEVINFO_DATA info{sizeof(info)};
                for (DWORD index = 0; SetupDiEnumDeviceInfo(devices, index, &info); ++index) {
                    wchar_t identifier[MAX_DEVICE_ID_LEN]{};
                    if (!SetupDiGetDeviceInstanceIdW(devices, &info, identifier, MAX_DEVICE_ID_LEN, nullptr)) continue;
                    std::wstring id(identifier);
                    if (id.rfind(L"USB\\VID_05AC&PID_12", 0) != 0 || id.find(L"&MI_") != std::wstring::npos) continue;
                    auto supported = layouts(id);
                    if (supported.empty()) continue;
                    auto serial = id.substr(id.rfind('\\') + 1);
                    for (auto& value : serial) if (!iswalnum(value) && value != '-') value = '_';
                    auto file = journal_directory / (serial + L".json");
                    if (std::filesystem::exists(file)) continue;
                    commands.push_back({argv[0], L"install", L"--instance", id,
                        L"--configuration", std::to_wstring(supported[0].configuration_index),
                        L"--interface", std::to_wstring(supported[0].interface_number), L"--journal", file.wstring()});
                }
                SetupDiDestroyDeviceInfoList(devices);
            }
            bool failed = false;
            for (auto& options : commands) {
                std::vector<wchar_t*> arguments;
                for (auto& option : options) arguments.push_back(option.data());
                int status = wmain(static_cast<int>(arguments.size()), arguments.data());
                failed |= status != 0 && status != 3010;
            }
            return failed ? 1 : 0;
        }
        if (command == L"restore") {
            require_administrator();
            Json record;
            std::ifstream input(journal);
            input >> record;
            input.close();
            // Restore the child before restoring the parent's configuration.
            restore_bindings(record);
            record["state"] = "restored";
            save(journal, record);
            std::cout << "Driver settings restored. Reconnect the device if requested by Windows.\n";
            return 0;
        }
        if (command != L"inspect" && command != L"install") throw std::runtime_error("Unknown driver command");
        if (command == L"install") require_administrator();
        if (instance.rfind(L"USB\\VID_05AC&PID_12", 0) != 0 || instance.find(L"&MI_") != std::wstring::npos)
            throw std::runtime_error("Select a normal-mode Apple USB composite parent instance");
        auto supported = layouts(instance);
        bool valid = false;
        for (const auto& layout : supported) valid |= layout.configuration_index == configuration && layout.interface_number == interface_number;
        if (!valid) throw std::runtime_error("Selected descriptor has no matching USB multiplexing interface");
        auto child_instance = find_child(instance, interface_number, true);
        Device parent(instance);
        Json child_backup = nullptr;
        if (!child_instance.empty()) {
            Device child(child_instance);
            // Missing driver keys are expected on a newly enumerated interface.
            // Other backup failures must stop setup before it changes bindings.
            if (!child.property(SPDRP_DRIVER).is_null()) child_backup = child.backup(child_instance);
        }
        Json record{{"version", 1}, {"state", "prepared"}, {"configuration", configuration},
            {"parent", parent.backup(instance)}, {"interface", child_backup}};
        if (command == L"inspect") { std::cout << record.dump(2) << '\n'; return 0; }
        if (record["parent"]["inf"] == "usb.inf" && !child_backup.is_null() && child_backup["inf"] == "winusb.inf") {
            std::cout << "Microsoft USB bindings are already installed.\n";
            return 0;
        }
        if (journal.empty() || !journal.is_absolute()) throw std::runtime_error("An absolute --journal path is required");
        if (std::filesystem::exists(journal)) throw std::runtime_error("Journal exists. Restore or retain it before another installation");
        save(journal, record);
        try {
            parent.set_parameter(L"OriginalConfigurationValue", dword(configuration));
            bool reboot = parent.install(windows_inf(L"usb.inf"), L"Composite.Dev");
            parent.restore_property(SPDRP_UPPERFILTERS, nullptr);
            parent.restore_property(SPDRP_LOWERFILTERS, nullptr);
            record["state"] = "parent_installed";
            save(journal, record);
            reboot |= parent.restart();
            // Configuration changes can recreate the interface PDO.
            child_instance = find_child(instance, interface_number);
            Device new_child(child_instance);
            reboot |= new_child.install(windows_inf(L"winusb.inf"), L"WINUSB");
            new_child.restore_property(SPDRP_UPPERFILTERS, nullptr);
            new_child.restore_property(SPDRP_LOWERFILTERS, nullptr);
            new_child.set_parameter(L"DeviceInterfaceGUIDs", multi_string(L"{60A7C98F-69A5-4F67-BB40-FBF92E1F8E4B}"));
            reboot |= new_child.restart();
            record["state"] = "installed";
            record["reboot_required"] = reboot;
            save(journal, record);
            std::cout << record.dump(2) << '\n';
            return reboot ? 3010 : 0;
        } catch (const std::exception& error) {
            std::string failure = error.what();
            try {
                restore_bindings(record);
                record["state"] = "rolled_back";
            } catch (const std::exception& rollback) {
                record["state"] = "rollback_required";
                failure += ". Rollback failed: " + std::string(rollback.what());
            }
            record["error"] = failure;
            save(journal, record);
            throw std::runtime_error(failure);
        }
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
