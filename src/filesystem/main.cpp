// SPDX-License-Identifier: GPL-3.0-or-later
#include "volume.h"
#include "device.h"
#include "names.h"
#include <windows.h>
#include <iostream>
namespace {
volatile LONG stopped = 0;
BOOL WINAPI on_control(DWORD event) {
    if (event == CTRL_C_EVENT || event == CTRL_BREAK_EVENT || event == CTRL_CLOSE_EVENT) { InterlockedExchange(&stopped, 1); return TRUE; }
    return FALSE;
}
}
int main(int argc, char** argv)
{
    try {
        std::string udid;
        std::wstring drive = L"I:";
        std::wstring stop_name, refresh_name, ready_name;
        bool read_only = false, media_only = false;
        for (int index = 1; index < argc; ++index) {
            std::string option = argv[index];
            if (option == "--read-only") read_only = true;
            else if (option == "--media-only") media_only = true;
            else if (option == "--udid" || option == "--drive" || option == "--stop-event" || option == "--refresh-event" || option == "--ready-event") {
                if (++index == argc) throw std::runtime_error("Missing mount option");
                if (option == "--udid") udid = argv[index];
                if (option == "--drive") drive = awm::widen(argv[index]);
                if (option == "--stop-event") stop_name = awm::widen(argv[index]);
                if (option == "--refresh-event") refresh_name = awm::widen(argv[index]);
                if (option == "--ready-event") ready_name = awm::widen(argv[index]);
            } else throw std::runtime_error("Unknown mount option " + option);
        }
        if (udid.empty()) throw std::runtime_error("--udid is required");
        if (drive.size() != 2 || drive[1] != ':' || towupper(drive[0]) < 'D' || towupper(drive[0]) > 'Z') throw std::runtime_error("Drive must be D: through Z:");
        drive[0] = towupper(drive[0]);
        _putenv_s("USBMUXD_SOCKET_ADDRESS", awm::mux_endpoint);
        HANDLE stop = stop_name.empty() ? nullptr : OpenEventW(SYNCHRONIZE, FALSE, stop_name.c_str());
        HANDLE refresh = refresh_name.empty() ? nullptr : OpenEventW(SYNCHRONIZE | EVENT_MODIFY_STATE, FALSE, refresh_name.c_str());
        if ((!stop_name.empty() && !stop) || (!refresh_name.empty() && !refresh)) throw std::runtime_error("Cannot open mount control events");
        SetConsoleCtrlHandler(on_control, TRUE);
        awm::Volume volume(udid, drive, read_only, media_only);
        volume.start();
        if (!ready_name.empty()) {
            HANDLE ready = OpenEventW(EVENT_MODIFY_STATE, FALSE, ready_name.c_str());
            if (!ready) throw std::runtime_error("Cannot open mount readiness event");
            SetEvent(ready); CloseHandle(ready);
        }
        std::cout << awm::Json{{"udid", udid}, {"drive", awm::narrow(drive)}, {"mounted", true}, {"read_only", read_only}}.dump() << std::endl;
        unsigned discovery_failures = 0;
        while (!InterlockedCompareExchange(&stopped, 0, 0) && !volume.failed() && (!stop || WaitForSingleObject(stop, 0) != WAIT_OBJECT_0)) {
            if (volume.stalled()) {
                std::cerr << "A device request exceeded 30 seconds. Terminating this mount generation.\n";
                TerminateProcess(GetCurrentProcess(), 3);
            }
            if (refresh && WaitForSingleObject(refresh, 0) == WAIT_OBJECT_0) {
                ResetEvent(refresh);
                try { volume.refresh_apps(); } catch (const std::exception& error) { std::cerr << error.what() << '\n'; }
            }
            try {
                auto devices = awm::devices();
                bool present = false;
                for (const auto& device : devices) if (device.at("udid") == udid) present = true;
                if (!present) break;
                discovery_failures = 0;
            } catch (...) { if (++discovery_failures >= 2) break; }
            Sleep(1000);
        }
        volume.stop();
        if (stop) CloseHandle(stop);
        if (refresh) CloseHandle(refresh);
        return volume.failed() ? 2 : 0;
    } catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
