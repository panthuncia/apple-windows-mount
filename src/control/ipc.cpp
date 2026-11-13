// SPDX-License-Identifier: GPL-3.0-or-later
#include "ipc.h"
#include <sddl.h>
#include <shlobj.h>
#include <shellapi.h>
#include <atomic>
#include <vector>
#include <stdexcept>
namespace awm {
std::wstring user_sid()
{
    HANDLE token = nullptr;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token)) throw std::runtime_error("Cannot inspect user identity");
    DWORD size = 0;
    GetTokenInformation(token, TokenUser, nullptr, 0, &size);
    std::vector<BYTE> buffer(size);
    BOOL queried = GetTokenInformation(token, TokenUser, buffer.data(), size, &size);
    CloseHandle(token);
    LPWSTR sid = nullptr;
    if (!queried || !ConvertSidToStringSidW(reinterpret_cast<TOKEN_USER*>(buffer.data())->User.Sid, &sid)) throw std::runtime_error("Cannot read user SID");
    std::wstring result = sid;
    LocalFree(sid);
    return result;
}
std::wstring pipe_name() { return L"\\\\.\\pipe\\AppleWindowsMount-" + user_sid(); }
PSECURITY_DESCRIPTOR private_descriptor()
{
    auto sid = user_sid();
    auto sddl = L"D:P(A;OICI;FA;;;" + sid + L")(A;OICI;FA;;;SY)";
    PSECURITY_DESCRIPTOR descriptor = nullptr;
    if (!ConvertStringSecurityDescriptorToSecurityDescriptorW(sddl.c_str(), SDDL_REVISION_1, &descriptor, nullptr)) throw std::runtime_error("Cannot create private ACL");
    return descriptor;
}
std::filesystem::path executable_directory()
{
    std::vector<wchar_t> path(32768);
    auto size = GetModuleFileNameW(nullptr, path.data(), static_cast<DWORD>(path.size()));
    if (!size || size >= path.size()) throw std::runtime_error("Cannot locate executable directory");
    return std::filesystem::path(std::wstring(path.data(), size)).parent_path();
}
std::filesystem::path application_directory()
{
    wchar_t* local = nullptr;
    size_t size = 0;
    if (_wdupenv_s(&local, &size, L"LOCALAPPDATA") || !local) throw std::runtime_error("LOCALAPPDATA unavailable");
    auto path = std::filesystem::path(local) / L"AppleWindowsMount";
    free(local);
    auto descriptor = private_descriptor();
    SECURITY_ATTRIBUTES attributes{sizeof(attributes), descriptor, FALSE};
    BOOL created = CreateDirectoryW(path.c_str(), &attributes);
    auto error = GetLastError();
    BOOL secured = SetFileSecurityW(path.c_str(), DACL_SECURITY_INFORMATION | PROTECTED_DACL_SECURITY_INFORMATION, descriptor);
    LocalFree(descriptor);
    if ((!created && error != ERROR_ALREADY_EXISTS) || !secured) throw std::runtime_error("Cannot secure application state directory");
    return path;
}
Json setup_usb_drivers()
{
    wchar_t common[MAX_PATH];
    if (FAILED(SHGetFolderPathW(nullptr, CSIDL_COMMON_APPDATA, nullptr, SHGFP_TYPE_CURRENT, common)))
        throw std::runtime_error("Cannot locate machine driver backups");
    auto journal = std::filesystem::path(common) / L"AppleWindowsMount" / L"DriverBackups" / L"interactive";
    auto executable = executable_directory() / L"awm-driver.exe";
    auto arguments = L"install-connected --journal-dir \"" + journal.wstring() + L"\"";
    SHELLEXECUTEINFOW launch{sizeof(launch)};
    launch.fMask = SEE_MASK_NOCLOSEPROCESS;
    launch.lpVerb = L"runas"; launch.lpFile = executable.c_str(); launch.lpParameters = arguments.c_str(); launch.nShow = SW_HIDE;
    if (!ShellExecuteExW(&launch)) throw std::runtime_error("USB setup did not start. Win32 error " + std::to_string(GetLastError()));
    auto waited = WaitForSingleObject(launch.hProcess, 120000);
    DWORD code = 0;
    GetExitCodeProcess(launch.hProcess, &code);
    CloseHandle(launch.hProcess);
    if (waited != WAIT_OBJECT_0) throw std::runtime_error("USB setup is still running. Recheck the driver journal before retrying");
    if (code != 0 && code != 3010) throw std::runtime_error("USB setup failed. Inspect the driver backup directory");
    return {{"setup_complete", true}, {"reconnect_device", true}, {"driver_backups", journal.string()}};
}
namespace {
class Event {
public:
    HANDLE value = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    ~Event() { if (value) CloseHandle(value); }
};
void transfer(HANDLE pipe, void* data, DWORD size, bool write, DWORD timeout)
{
    DWORD done = 0;
    auto deadline = GetTickCount64() + timeout;
    while (done < size) {
        auto now = GetTickCount64();
        if (now >= deadline) throw std::runtime_error("Controller request timed out");
        Event event;
        if (!event.value) throw std::runtime_error("Cannot allocate IPC event");
        OVERLAPPED overlapped{};
        overlapped.hEvent = event.value;
        DWORD count = 0;
        BOOL completed = write ? WriteFile(pipe, static_cast<BYTE*>(data) + done, size - done, &count, &overlapped)
                               : ReadFile(pipe, static_cast<BYTE*>(data) + done, size - done, &count, &overlapped);
        if (!completed) {
            if (GetLastError() != ERROR_IO_PENDING) throw std::runtime_error("Controller pipe closed");
            if (WaitForSingleObject(event.value, static_cast<DWORD>(deadline - now)) != WAIT_OBJECT_0) {
                CancelIoEx(pipe, &overlapped);
                GetOverlappedResult(pipe, &overlapped, &count, TRUE);
                throw std::runtime_error("Controller request timed out");
            }
            if (!GetOverlappedResult(pipe, &overlapped, &count, FALSE)) throw std::runtime_error("Controller pipe operation failed");
        }
        if (!count) throw std::runtime_error("Controller pipe EOF");
        done += count;
    }
}
Json receive(HANDLE pipe, DWORD timeout)
{
    DWORD size = 0;
    transfer(pipe, &size, sizeof(size), false, timeout);
    if (!size || size > 65536) throw std::runtime_error("Invalid controller message size");
    std::string text(size, '\0');
    transfer(pipe, text.data(), size, false, timeout);
    return Json::parse(text);
}
void send(HANDLE pipe, const Json& message)
{
    auto text = message.dump();
    if (text.size() > 65536) throw std::runtime_error("Controller response is too large");
    DWORD size = static_cast<DWORD>(text.size());
    transfer(pipe, &size, sizeof(size), true, 5000);
    transfer(pipe, text.data(), size, true, 5000);
}
}
Json controller_request(const Json& request, bool start_if_missing, const std::wstring& override_name)
{
    auto name = override_name.empty() ? pipe_name() : override_name;
    if (!WaitNamedPipeW(name.c_str(), 200) && start_if_missing) {
        auto executable = executable_directory() / L"awm-host.exe";
        std::wstring command = L"\"" + executable.wstring() + L"\"";
        STARTUPINFOW startup{sizeof(startup)};
        PROCESS_INFORMATION process{};
        if (!CreateProcessW(executable.c_str(), command.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr, nullptr, &startup, &process))
            throw std::runtime_error("Cannot start device controller");
        CloseHandle(process.hThread); CloseHandle(process.hProcess);
        for (unsigned attempt = 0; attempt < 100 && !WaitNamedPipeW(name.c_str(), 100); ++attempt) Sleep(50);
    }
    HANDLE pipe = CreateFileW(name.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_EXISTING, FILE_FLAG_OVERLAPPED, nullptr);
    if (pipe == INVALID_HANDLE_VALUE) throw std::runtime_error("Device controller is unavailable");
    try {
        send(pipe, request);
        auto response = receive(pipe, 60000);
        BYTE acknowledged = 1;
        transfer(pipe, &acknowledged, 1, true, 5000);
        CloseHandle(pipe);
        return response;
    } catch (...) { CloseHandle(pipe); throw; }
}
void serve_controller(const std::function<Json(const Json&)>& handler, const std::atomic<bool>& stop, const std::wstring& override_name)
{
    auto descriptor = private_descriptor();
    SECURITY_ATTRIBUTES attributes{sizeof(attributes), descriptor, FALSE};
    auto name = override_name.empty() ? pipe_name() : override_name;
    HANDLE pipe = CreateNamedPipeW(name.c_str(), PIPE_ACCESS_DUPLEX | FILE_FLAG_OVERLAPPED | FILE_FLAG_FIRST_PIPE_INSTANCE,
        PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT | PIPE_REJECT_REMOTE_CLIENTS, 1, 65540, 65540, 5000, &attributes);
    LocalFree(descriptor);
    if (pipe == INVALID_HANDLE_VALUE) throw std::runtime_error("Cannot create private controller pipe");
    while (!stop) {
        Event event;
        OVERLAPPED connected{};
        connected.hEvent = event.value;
        BOOL immediate = ConnectNamedPipe(pipe, &connected);
        DWORD error = immediate ? ERROR_SUCCESS : GetLastError();
        bool accepted = immediate || error == ERROR_PIPE_CONNECTED;
        if (error == ERROR_IO_PENDING) {
            while (!stop && WaitForSingleObject(event.value, 200) == WAIT_TIMEOUT) {}
            if (!stop) { DWORD ignored = 0; accepted = GetOverlappedResult(pipe, &connected, &ignored, FALSE); }
            else { CancelIoEx(pipe, &connected); DWORD ignored; GetOverlappedResult(pipe, &connected, &ignored, TRUE); }
        }
        if (accepted && !stop) {
            Json response;
            try { response = handler(receive(pipe, 5000)); }
            catch (const std::exception& exception) { response = {{"error", exception.what()}}; }
            try {
                send(pipe, response);
                BYTE acknowledged = 0;
                transfer(pipe, &acknowledged, 1, false, 5000);
            } catch (...) {}
        }
        DisconnectNamedPipe(pipe);
    }
    CloseHandle(pipe);
}
}
