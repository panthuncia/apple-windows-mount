// SPDX-License-Identifier: GPL-3.0-or-later
#include <algorithm>
#include <iostream>
#include <string>
#include <stdexcept>
#include "windows_compat.h"
#include <libusb.h>
extern "C" {
#include "client.h"
#include "device.h"
#include "usb.h"
#include "conf.h"
#include "log.h"
}

namespace {
volatile LONG stopping = 0;
BOOL WINAPI on_control(DWORD event)
{
    if (event == CTRL_C_EVENT || event == CTRL_BREAK_EVENT || event == CTRL_CLOSE_EVENT) {
        InterlockedExchange(&stopping, 1);
        return TRUE;
    }
    return FALSE;
}
}

int main(int argc, char** argv)
{
    try {
        bool usbdk = false;
        bool no_usb = false;
        unsigned port = 37491;
        HANDLE stop_event = nullptr;
        for (int index = 1; index < argc; ++index) {
            std::string argument = argv[index];
            if (argument == "--usbdk") usbdk = true;
            else if (argument == "--no-usb") no_usb = true;
            else if (argument == "--verbose") log_level = LL_DEBUG;
            else if (argument == "--port" && index + 1 < argc) {
                std::string value = argv[++index];
                size_t end = 0;
                port = std::stoul(value, &end);
                if (end != value.size() || port == 0 || port > 65535) throw std::runtime_error("Invalid endpoint port");
            }
            else if (argument == "--state-dir" && index + 1 < argc) {
                if (config_set_config_dir(argv[++index]) != 0) throw std::runtime_error("Cannot initialize state directory");
            }
            else if (argument == "--stop-event" && index + 1 < argc) {
                int size = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, argv[++index], -1, nullptr, 0);
                if (!size) throw std::runtime_error("Invalid event name");
                std::wstring name(size, L'\0');
                MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, argv[index], -1, name.data(), size);
                stop_event = OpenEventW(SYNCHRONIZE, FALSE, name.c_str());
                if (!stop_event) throw std::runtime_error("Cannot open stop event");
            } else if (argument == "--help") {
                std::cout << "awm-usbmuxd [--usbdk] [--verbose] [--stop-event NAME] [--state-dir PATH] [--no-usb]\nListens on 127.0.0.1:37491. Pair records are private to the current user.\n";
                return 0;
            } else throw std::runtime_error("Unknown transport option " + argument);
        }
        if (!config_get_config_dir()) throw std::runtime_error("Cannot initialize private pairing directory");
        WSADATA data{};
        if (WSAStartup(MAKEWORD(2, 2), &data) != 0) throw std::runtime_error("WSAStartup failed");
        SOCKET listener = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        if (listener == INVALID_SOCKET) throw std::runtime_error("socket failed");
        int exclusive = 1;
        setsockopt(listener, SOL_SOCKET, SO_EXCLUSIVEADDRUSE, &exclusive, sizeof(exclusive));
        sockaddr_in address{};
        address.sin_family = AF_INET;
        address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        address.sin_port = htons(static_cast<u_short>(port));
        if (bind(listener, reinterpret_cast<sockaddr*>(&address), sizeof(address)) != 0 || listen(listener, 16) != 0) {
            closesocket(listener);
            throw std::runtime_error("Cannot bind transport endpoint 127.0.0.1:" + std::to_string(port));
        }
        fcntl(listener, F_SETFL, O_NONBLOCK);
        SetConsoleCtrlHandler(on_control, TRUE);
        awm_usb_backend_usbdk = usbdk;
        client_init();
        device_init();
        if (!no_usb && usb_init() < 0) throw std::runtime_error("USB transport initialization failed");
        std::cerr << "Transport listening on 127.0.0.1:" << port << " using " << (usbdk ? "UsbDk" : "WinUSB") << '\n';
        fdlist sockets{};
        fdlist_create(&sockets);
        int result = 0;
        while (!InterlockedCompareExchange(&stopping, 0, 0) &&
            (!stop_event || WaitForSingleObject(stop_event, 0) == WAIT_TIMEOUT)) {
            fdlist_reset(&sockets);
            fdlist_add(&sockets, FD_LISTEN, listener, POLLIN);
            client_get_fds(&sockets);
            int ready = WSAPoll(sockets.fds, sockets.count, 10);
            if (ready == SOCKET_ERROR) { result = 1; break; }
            for (int index = 0; index < sockets.count; ++index) {
                if (!sockets.fds[index].revents) continue;
                if (sockets.owners[index] == FD_LISTEN) client_accept(listener);
                else client_process(sockets.fds[index].fd, sockets.fds[index].revents);
            }
            // libusb on Windows exposes no pollable Unix descriptors. Drive its
            // completions on the same thread as upstream's client/device state.
            if (!no_usb && usb_process() < 0) { result = 1; break; }
            device_check_timeouts();
        }
        fdlist_free(&sockets);
        if (!no_usb) device_kill_connections();
        if (!no_usb) usb_shutdown();
        device_shutdown();
        client_shutdown();
        closesocket(listener);
        if (stop_event) CloseHandle(stop_event);
        WSACleanup();
        return result;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
