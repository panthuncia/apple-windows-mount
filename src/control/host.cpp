// SPDX-License-Identifier: GPL-3.0-or-later
#include "ipc.h"
#include <shellapi.h>
#include <fstream>
#include <map>
#include <mutex>
#include <set>
#include <thread>
#include <chrono>

namespace {
using awm::Json;
std::wstring wide(const std::string& text) {
    int size = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text.data(), static_cast<int>(text.size()), nullptr, 0);
    std::wstring result(size, L'\0');
    if (size) MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text.data(), static_cast<int>(text.size()), result.data(), size);
    return result;
}
std::wstring quote(const std::wstring& text) {
    std::wstring output = L"\"";
    size_t slashes = 0;
    for (wchar_t value : text) {
        if (value == L'\\') { ++slashes; continue; }
        output.append(slashes * (value == L'"' ? 2 : 1), L'\\');
        slashes = 0;
        if (value == L'"') output += L'\\';
        output += value;
    }
    output.append(slashes * 2, L'\\');
    return output + L'"';
}
struct Child {
    HANDLE process = nullptr, stop = nullptr, ready = nullptr, refresh = nullptr;
    std::wstring stop_name, ready_name, refresh_name;
    Child() = default;
    Child(const Child&) = delete;
    Child& operator=(const Child&) = delete;
    ~Child() { shutdown(); }
    bool running() const { return process && WaitForSingleObject(process, 0) == WAIT_TIMEOUT; }
    void launch(const std::wstring& executable, const std::vector<std::wstring>& arguments, const std::string& tag, bool mount) {
        auto descriptor = awm::private_descriptor();
        SECURITY_ATTRIBUTES attributes{sizeof(attributes), descriptor, FALSE};
        auto prefix = L"Local\\AppleWindowsMount-" + awm::user_sid() + L"-" + std::to_wstring(GetCurrentProcessId()) + L"-" + wide(tag);
        stop_name = prefix + L"-stop";
        stop = CreateEventW(&attributes, TRUE, FALSE, stop_name.c_str());
        std::vector<std::wstring> options = arguments;
        options.insert(options.end(), {L"--stop-event", stop_name});
        if (mount) {
            ready_name = prefix + L"-ready"; refresh_name = prefix + L"-refresh";
            ready = CreateEventW(&attributes, TRUE, FALSE, ready_name.c_str());
            refresh = CreateEventW(&attributes, TRUE, FALSE, refresh_name.c_str());
            options.insert(options.end(), {L"--ready-event", ready_name, L"--refresh-event", refresh_name});
        }
        LocalFree(descriptor);
        if (!stop || (mount && (!ready || !refresh))) throw std::runtime_error("Cannot create worker control events");
        auto file = awm::executable_directory() / executable;
        std::wstring command = quote(file.wstring());
        for (const auto& option : options) command += L" " + quote(option);
        auto log = awm::application_directory() / (wide(tag) + L".log");
        SECURITY_ATTRIBUTES inherited{sizeof(inherited), nullptr, TRUE};
        HANDLE output = CreateFileW(log.c_str(), GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, &inherited, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        HANDLE input = CreateFileW(L"NUL", GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, &inherited, OPEN_EXISTING, 0, nullptr);
        if (output == INVALID_HANDLE_VALUE || input == INVALID_HANDLE_VALUE) {
            if (output != INVALID_HANDLE_VALUE) CloseHandle(output);
            if (input != INVALID_HANDLE_VALUE) CloseHandle(input);
            throw std::runtime_error("Cannot open worker log");
        }
        SIZE_T attribute_size = 0;
        InitializeProcThreadAttributeList(nullptr, 1, 0, &attribute_size);
        std::vector<BYTE> storage(attribute_size);
        auto list = reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(storage.data());
        if (!InitializeProcThreadAttributeList(list, 1, 0, &attribute_size)) throw std::runtime_error("Cannot initialize worker attributes");
        HANDLE inherited_handles[]{output, input};
        BOOL configured = UpdateProcThreadAttribute(list, 0, PROC_THREAD_ATTRIBUTE_HANDLE_LIST, inherited_handles, sizeof(inherited_handles), nullptr, nullptr);
        STARTUPINFOEXW startup{};
        startup.StartupInfo.cb = sizeof(startup);
        startup.StartupInfo.dwFlags = STARTF_USESTDHANDLES;
        startup.StartupInfo.hStdOutput = startup.StartupInfo.hStdError = output;
        startup.StartupInfo.hStdInput = input;
        startup.lpAttributeList = list;
        PROCESS_INFORMATION spawned{};
        BOOL created = configured && CreateProcessW(file.c_str(), command.data(), nullptr, nullptr, TRUE,
            CREATE_NO_WINDOW | EXTENDED_STARTUPINFO_PRESENT, nullptr, nullptr, &startup.StartupInfo, &spawned);
        DeleteProcThreadAttributeList(list);
        CloseHandle(output); CloseHandle(input);
        if (!created) throw std::runtime_error("Cannot launch worker. Win32 error " + std::to_string(GetLastError()));
        process = spawned.hProcess;
        CloseHandle(spawned.hThread);
        if (mount) {
            HANDLE wait[]{ready, process};
            if (WaitForMultipleObjects(2, wait, FALSE, 30000) != WAIT_OBJECT_0) {
                shutdown();
                throw std::runtime_error("Mount worker failed to become ready. See " + log.string());
            }
        }
    }
    void shutdown() {
        if (stop) SetEvent(stop);
        if (process) {
            if (WaitForSingleObject(process, 5000) == WAIT_TIMEOUT) { TerminateProcess(process, 3); WaitForSingleObject(process, 5000); }
            CloseHandle(process); process = nullptr;
        }
        for (HANDLE* handle : {&stop, &ready, &refresh}) if (*handle) { CloseHandle(*handle); *handle = nullptr; }
    }
};
class Controller {
public:
    Controller() {
        _putenv_s("USBMUXD_SOCKET_ADDRESS", awm::mux_endpoint);
        auto file = awm::application_directory() / L"preferences.json";
        if (std::filesystem::exists(file)) {
            std::ifstream input(file);
            input >> preferences_;
            if (!preferences_.is_object()) throw std::runtime_error("Invalid preferences file");
        }
        try { awm::devices(); }
        catch (...) {
            broker_ = std::make_unique<Child>();
            broker_->launch(L"awm-usbmuxd.exe", {}, "transport", false);
            bool ready = false;
            for (unsigned attempt = 0; attempt < 40 && broker_->running(); ++attempt) {
                try { awm::devices(); ready = true; break; } catch (...) { Sleep(100); }
            }
            if (!ready) throw std::runtime_error("USB transport failed to start. See the application transport log");
        }
    }
    ~Controller() { workers_.clear(); }
    Json snapshot() { std::lock_guard lock(snapshot_mutex_); return snapshot_; }
    void poll() {
        std::lock_guard lock(mutex_);
        try {
            if (broker_ && !broker_->running()) {
                workers_.clear();
                broker_->shutdown();
                broker_->launch(L"awm-usbmuxd.exe", {}, "transport", false);
            }
            auto devices = awm::devices();
            transport_error_.clear();
            std::set<std::string> present;
            for (const auto& device : devices) present.insert(device.at("udid").get<std::string>());
            for (auto iterator = workers_.begin(); iterator != workers_.end();) {
                if (!present.contains(iterator->first) || !iterator->second->running()) {
                    if (present.contains(iterator->first)) {
                        blocked_.insert(iterator->first);
                        records_[iterator->first]["state"] = "error";
                        records_[iterator->first]["error"] = "Mount worker stopped. Retry mounting or inspect its log.";
                    }
                    iterator = workers_.erase(iterator);
                } else ++iterator;
            }
            for (auto iterator = records_.begin(); iterator != records_.end();) {
                if (!present.contains(iterator->first)) { blocked_.erase(iterator->first); iterator = records_.erase(iterator); }
                else ++iterator;
            }
            for (const auto& udid : present) {
                if (workers_.contains(udid) || blocked_.contains(udid)) continue;
                records_[udid] = {{"udid", udid}, {"state", "awaiting_trust"}};
                try {
                    awm::Device device(udid);
                    auto info = device.info();
                    records_[udid]["name"] = info.at("DeviceName");
                    records_[udid]["state"] = "ready";
                    mount(udid, Json::object());
                } catch (const std::exception& error) { records_[udid]["error"] = error.what(); }
            }
        } catch (const std::exception& error) { transport_error_ = error.what(); }
        publish();
    }
    Json command(const Json& request) {
        std::lock_guard lock(mutex_);
        auto command = request.at("command").get<std::string>();
        if (command == "status") return snapshot();
        if (command == "shutdown") { stopped = true; return {{"stopping", true}}; }
        if (command == "diagnostics") {
            auto output = awm::diagnostics(false);
            output["controller"] = snapshot();
            auto file = awm::application_directory() / L"diagnostics.json";
            std::ofstream stream(file);
            stream << output.dump(2);
            if (!stream) throw std::runtime_error("Cannot export diagnostics");
            return {{"saved", file.string()}};
        }
        std::string udid = request.value("udid", "");
        if (udid.empty()) {
            auto found = awm::devices();
            if (found.size() != 1) throw std::runtime_error("Specify --udid when zero or multiple devices are connected");
            udid = found[0].at("udid").get<std::string>();
        }
        if (udid.empty() || udid.size() > 128 || udid.find_first_not_of("0123456789abcdefABCDEF-") != std::string::npos)
            throw std::runtime_error("Invalid UDID");
        if (command == "mount") { blocked_.erase(udid); mount(udid, request); }
        else if (command == "unmount") {
            if (!records_.contains(udid)) throw std::runtime_error("Device is not connected");
            workers_.erase(udid); blocked_.insert(udid); records_[udid]["state"] = "ready"; records_[udid].erase("drive");
        }
        else if (command == "refresh") {
            if (!workers_.contains(udid)) throw std::runtime_error("Device is not mounted");
            SetEvent(workers_.at(udid)->refresh);
        } else if (command == "pair") { awm::Device device(udid); device.pair(); blocked_.erase(udid); }
        else if (command == "unpair") {
            workers_.erase(udid); blocked_.insert(udid); awm::Device device(udid); device.unpair();
            records_[udid]["state"] = "awaiting_trust"; records_[udid].erase("drive");
        } else throw std::runtime_error("Unknown controller command");
        publish();
        return snapshot();
    }
    std::atomic<bool> stopped{false};
private:
    void mount(const std::string& udid, const Json& request) {
        if (workers_.contains(udid)) {
            if (!request.contains("read_only") && !request.contains("drive")) return;
        }
        auto preference = preferences_.value(udid, Json::object());
        std::string drive = request.value("drive", preference.value("drive", "I:"));
        if (drive.size() != 2 || drive[1] != ':' || toupper(drive[0]) < 'D' || toupper(drive[0]) > 'Z') throw std::runtime_error("Drive must be D: through Z:");
        drive[0] = static_cast<char>(toupper(drive[0]));
        auto occupied = GetLogicalDrives();
        if (workers_.contains(udid) && records_.contains(udid) && records_.at(udid).contains("drive")) {
            auto own_drive = records_.at(udid).at("drive").get<std::string>();
            occupied &= ~(1UL << (own_drive[0] - 'A'));
        }
        if (occupied & (1UL << (drive[0] - 'A'))) {
            if (request.contains("drive")) throw std::runtime_error("Requested drive letter is already in use");
            drive.clear();
            for (char letter : std::string("IJKLMNOPQRSTUVWXYZDEFGH")) if (!(occupied & (1UL << (letter - 'A')))) { drive = std::string(1, letter) + ':'; break; }
            if (drive.empty()) throw std::runtime_error("No drive letters available");
        }
        bool read_only = request.value("read_only", preference.value("read_only", false));
        workers_.erase(udid);
        records_[udid]["udid"] = udid;
        records_[udid]["state"] = "ready";
        records_[udid].erase("drive");
        auto worker = std::make_unique<Child>();
        std::vector<std::wstring> options{L"--udid", wide(udid), L"--drive", wide(drive)};
        if (read_only) options.push_back(L"--read-only");
        try { worker->launch(L"awm-mount.exe", options, "mount-" + udid, true); }
        catch (const std::exception& error) { records_[udid]["state"] = "error"; records_[udid]["error"] = error.what(); publish(); throw; }
        workers_[udid] = std::move(worker);
        preferences_[udid] = {{"drive", drive}, {"read_only", read_only}};
        auto destination = awm::application_directory() / L"preferences.json";
        auto temporary = destination; temporary += L".tmp";
        { std::ofstream output(temporary); output << preferences_.dump(2); output.flush(); if (!output) throw std::runtime_error("Cannot save mount preferences"); }
        if (!MoveFileExW(temporary.c_str(), destination.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) throw std::runtime_error("Cannot commit mount preferences");
        records_[udid]["udid"] = udid;
        records_[udid]["state"] = "mounted";
        records_[udid]["drive"] = drive;
        records_[udid]["read_only"] = read_only;
        records_[udid].erase("error");
    }
    void publish() {
        Json output{{"running", true}, {"devices", Json::array()}};
        if (!transport_error_.empty()) output["transport_error"] = transport_error_;
        for (const auto& [unused, record] : records_) output["devices"].push_back(record);
        std::lock_guard lock(snapshot_mutex_);
        snapshot_ = std::move(output);
    }
    std::recursive_mutex mutex_;
    std::mutex snapshot_mutex_;
    Json snapshot_{{"running", true}, {"devices", Json::array()}};
    Json preferences_ = Json::object();
    std::unique_ptr<Child> broker_;
    std::map<std::string, std::unique_ptr<Child>> workers_;
    std::map<std::string, Json> records_;
    std::set<std::string> blocked_;
    std::string transport_error_;
};

Controller* controller = nullptr;
NOTIFYICONDATAW icon{};
constexpr UINT tray_message = WM_APP + 1;
constexpr UINT error_message = WM_APP + 2;
void send_command(HWND window, Json request) {
    std::thread([window, request = std::move(request)] {
        try {
            auto response = awm::controller_request(request, false);
            if (response.contains("error")) throw std::runtime_error(response.at("error").get<std::string>());
        } catch (const std::exception& error) { PostMessageW(window, error_message, 0, reinterpret_cast<LPARAM>(new std::wstring(wide(error.what())))); }
    }).detach();
}
LRESULT CALLBACK window_proc(HWND window, UINT message, WPARAM wparam, LPARAM lparam) {
    if (message == error_message) {
        std::unique_ptr<std::wstring> error(reinterpret_cast<std::wstring*>(lparam));
        MessageBoxW(window, error->c_str(), L"Apple Windows Mount", MB_OK | MB_ICONERROR);
        return 0;
    }
    if (message == tray_message && (lparam == WM_RBUTTONUP || lparam == WM_LBUTTONUP)) {
        auto status = controller->snapshot();
        HMENU menu = CreatePopupMenu();
        auto records = status.at("devices");
        if (records.empty()) AppendMenuW(menu, MF_STRING | MF_GRAYED, 1, L"Connect an iPhone or iPad");
        for (size_t index = 0; index < records.size(); ++index) {
            const auto& record = records[index];
            auto label = wide(record.value("name", record.at("udid").get<std::string>())) + L" (" + wide(record.value("state", "ready")) + L")";
            HMENU actions = CreatePopupMenu();
            UINT base = 100 + static_cast<UINT>(index) * 10;
            AppendMenuW(actions, MF_STRING, base, L"Open in Explorer");
            AppendMenuW(actions, MF_STRING, base + 1, L"Mount");
            AppendMenuW(actions, MF_STRING, base + 2, L"Unmount");
            AppendMenuW(actions, MF_STRING | (record.value("read_only", false) ? MF_CHECKED : 0), base + 3, L"Read-only mount");
            AppendMenuW(actions, MF_STRING, base + 4, L"Refresh apps");
            AppendMenuW(actions, MF_STRING, base + 5, L"Forget pairing");
            AppendMenuW(menu, MF_POPUP, reinterpret_cast<UINT_PTR>(actions), label.c_str());
        }
        AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
        AppendMenuW(menu, MF_STRING, 10, L"Open logs");
        AppendMenuW(menu, MF_STRING, 11, L"Start with Windows");
        AppendMenuW(menu, MF_STRING, 12, L"Exit and unmount");
        AppendMenuW(menu, MF_STRING, 13, L"Export diagnostics");
        AppendMenuW(menu, MF_STRING, 14, L"Set up USB devices...");
        POINT point; GetCursorPos(&point); SetForegroundWindow(window);
        UINT selected = TrackPopupMenu(menu, TPM_RETURNCMD | TPM_NONOTIFY, point.x, point.y, 0, window, nullptr);
        DestroyMenu(menu);
        if (selected == 10) ShellExecuteW(window, L"open", awm::application_directory().c_str(), nullptr, nullptr, SW_SHOWNORMAL);
        if (selected == 11) {
            HKEY key;
            if (RegCreateKeyExW(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\Run", 0, nullptr, 0, KEY_READ | KEY_WRITE, nullptr, &key, nullptr) == ERROR_SUCCESS) {
                DWORD size = 0;
                if (RegQueryValueExW(key, L"AppleWindowsMount", nullptr, nullptr, nullptr, &size) == ERROR_SUCCESS) RegDeleteValueW(key, L"AppleWindowsMount");
                else {
                    auto command = quote((awm::executable_directory() / L"awm-host.exe").wstring());
                    RegSetValueExW(key, L"AppleWindowsMount", 0, REG_SZ, reinterpret_cast<const BYTE*>(command.c_str()), static_cast<DWORD>((command.size() + 1) * sizeof(wchar_t)));
                }
                RegCloseKey(key);
            }
        }
        if (selected == 12) { controller->stopped = true; PostQuitMessage(0); }
        if (selected == 13) { send_command(window, {{"command", "diagnostics"}}); ShellExecuteW(window, L"open", awm::application_directory().c_str(), nullptr, nullptr, SW_SHOWNORMAL); }
        if (selected == 14) std::thread([window] {
            try {
                awm::setup_usb_drivers();
                MessageBoxW(window, L"USB setup finished. Reconnect your device. Windows may require a restart.", L"Apple Windows Mount", MB_OK | MB_ICONINFORMATION);
            } catch (const std::exception& error) { PostMessageW(window, error_message, 0, reinterpret_cast<LPARAM>(new std::wstring(wide(error.what())))); }
        }).detach();
        if (selected >= 100 && (selected - 100) / 10 < records.size()) {
            auto record = records[(selected - 100) / 10];
            unsigned action = (selected - 100) % 10;
            if (action == 0 && record.contains("drive")) ShellExecuteW(window, L"open", (wide(record.at("drive")) + L"\\").c_str(), nullptr, nullptr, SW_SHOWNORMAL);
            else {
                const char* commands[]{"mount", "mount", "unmount", "mount", "refresh", "unpair"};
                Json request{{"command", commands[action]}, {"udid", record.at("udid")}};
                if (action == 3) request["read_only"] = !record.value("read_only", false);
                send_command(window, request);
            }
        }
        return 0;
    }
    if (message == WM_TIMER && controller->stopped) { PostQuitMessage(0); return 0; }
    if (message == WM_DESTROY) { PostQuitMessage(0); return 0; }
    return DefWindowProcW(window, message, wparam, lparam);
}
}

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR, int)
{
    HANDLE singleton = nullptr;
    try {
        auto singleton_name = L"Local\\AppleWindowsMount-" + awm::user_sid();
        singleton = CreateMutexW(nullptr, TRUE, singleton_name.c_str());
        if (!singleton || GetLastError() == ERROR_ALREADY_EXISTS) { if (singleton) CloseHandle(singleton); return 0; }
        Controller owner;
        controller = &owner;
        WNDCLASSW window_class{};
        window_class.lpfnWndProc = window_proc; window_class.hInstance = instance; window_class.lpszClassName = L"AppleWindowsMountTray";
        RegisterClassW(&window_class);
        HWND window = CreateWindowW(window_class.lpszClassName, L"Apple Windows Mount", 0, 0, 0, 0, 0, HWND_MESSAGE, nullptr, instance, nullptr);
        if (!window) throw std::runtime_error("Cannot create tray window");
        icon.cbSize = sizeof(icon); icon.hWnd = window; icon.uID = 1;
        icon.uFlags = NIF_MESSAGE | NIF_ICON | NIF_TIP;
        icon.uCallbackMessage = tray_message; icon.hIcon = LoadIconW(nullptr, IDI_APPLICATION);
        wcscpy_s(icon.szTip, L"Apple Windows Mount");
        Shell_NotifyIconW(NIM_ADD, &icon);
        SetTimer(window, 1, 500, nullptr);
        std::thread polling([&] { while (!owner.stopped) { owner.poll(); for (unsigned i = 0; i < 10 && !owner.stopped; ++i) Sleep(100); } });
        std::thread serving([&] {
            try { awm::serve_controller([&](const Json& request) { return owner.command(request); }, owner.stopped); }
            catch (const std::exception& error) { PostMessageW(window, error_message, 0, reinterpret_cast<LPARAM>(new std::wstring(wide(error.what())))); owner.stopped = true; }
        });
        MSG message;
        while (GetMessageW(&message, nullptr, 0, 0) > 0) { TranslateMessage(&message); DispatchMessageW(&message); }
        owner.stopped = true;
        polling.join(); serving.join();
        Shell_NotifyIconW(NIM_DELETE, &icon);
        DestroyWindow(window);
        CloseHandle(singleton);
        return 0;
    } catch (const std::exception& error) {
        MessageBoxW(nullptr, wide(error.what()).c_str(), L"Apple Windows Mount", MB_OK | MB_ICONERROR);
        if (singleton) CloseHandle(singleton);
        return 1;
    }
}
