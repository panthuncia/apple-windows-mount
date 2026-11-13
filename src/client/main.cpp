// SPDX-License-Identifier: GPL-3.0-or-later
#include "device.h"
#include "ipc.h"
#include <iostream>
#include <charconv>
#include <limits>

namespace {
struct Options {
    std::string command;
    std::string udid;
    std::string path = "/";
    uint64_t bytes = 8 * 1024 * 1024;
    bool json = false;
    bool usbdk = false;
    unsigned port = 37491;
    std::string drive;
    int read_only = -1;
};

Options parse(int argc, char** argv)
{
    Options options;
    if (argc < 2) { options.command = "help"; return options; }
    options.command = argv[1];
    for (int index = 2; index < argc; ++index) {
        std::string argument = argv[index];
        if (argument == "--json") options.json = true;
        else if (argument == "--read-only") options.read_only = 1;
        else if (argument == "--read-write") options.read_only = 0;
        else if (argument == "--usbdk") options.usbdk = true;
        else if (argument == "--udid" || argument == "--path" || argument == "--bytes" || argument == "--port" || argument == "--drive") {
            if (++index == argc) throw std::runtime_error("Missing value for " + argument);
            if (argument == "--udid") options.udid = argv[index];
            else if (argument == "--drive") options.drive = argv[index];
            else if (argument == "--path") options.path = argv[index];
            else if (argument == "--port") {
                std::string value = argv[index];
                auto parsed = std::from_chars(value.data(), value.data() + value.size(), options.port);
                if (parsed.ec != std::errc{} || parsed.ptr != value.data() + value.size() || !options.port || options.port > 65535)
                    throw std::runtime_error("Invalid endpoint port");
            }
            else {
                std::string value = argv[index];
                auto parsed = std::from_chars(value.data(), value.data() + value.size(), options.bytes);
                if (parsed.ec != std::errc{} || parsed.ptr != value.data() + value.size() || !options.bytes ||
                    options.bytes > static_cast<uint64_t>(std::numeric_limits<int64_t>::max()))
                    throw std::runtime_error("Invalid transfer size");
            }
        } else throw std::runtime_error("Unknown option " + argument);
    }
    return options;
}

void print(const awm::Json& output, bool json)
{
    if (json) { std::cout << output.dump() << '\n'; return; }
    std::cout << output.dump(2) << '\n';
}
}

int main(int argc, char** argv)
{
    bool json = false;
    for (int index = 1; index < argc; ++index) if (std::string(argv[index]) == "--json") json = true;
    try {
        auto options = parse(argc, argv);
        // Always select this project's broker, including for library-internal pairing.
        auto endpoint = "127.0.0.1:" + std::to_string(options.port);
        _putenv_s("USBMUXD_SOCKET_ADDRESS", endpoint.c_str());
        if (options.command == "help" || options.command == "--help") {
            std::cout << "Apple Windows Mount transport tools\n"
                "  awm devices [--json]\n"
                "  awm doctor [--usbdk] [--json]\n"
                "  awm setup [--json] (administrator-assisted USB driver setup)\n"
                "  awm start|status|shutdown [--json]\n"
                "  awm mount [--udid UDID] [--drive I:] [--read-only|--read-write] [--json]\n"
                "  awm unmount|refresh [--udid UDID] [--json]\n"
                "  awm pair|unpair|info|apps --udid UDID [--json]\n"
                "  awm ls --udid UDID [--path /] [--json]\n"
                "  awm transfer-probe --udid UDID [--path /] [--bytes 8388608] [--json]\n"
                "The probe creates, verifies, and removes a unique test directory on the device.\n";
            return 0;
        }
        awm::Json output;
        if (options.command == "setup") output = awm::setup_usb_drivers();
        else if (options.command == "start" || options.command == "status" || options.command == "shutdown" ||
            options.command == "mount" || options.command == "unmount" || options.command == "refresh" ||
            options.command == "pair" || options.command == "unpair") {
            awm::Json request{{"command", options.command == "start" ? "status" : options.command}};
            if (!options.udid.empty()) request["udid"] = options.udid;
            if (!options.drive.empty()) request["drive"] = options.drive;
            if (options.read_only >= 0) request["read_only"] = options.read_only != 0;
            try { output = awm::controller_request(request, options.command == "start" || options.command == "mount" || options.command == "pair"); }
            catch (...) { if (options.command == "status") output = {{"running", false}}; else throw; }
            if (output.contains("error")) throw std::runtime_error(output.at("error").get<std::string>());
        }
        else if (options.command == "doctor") output = awm::diagnostics(options.usbdk);
        else if (options.command == "devices") output = awm::devices();
        else {
            if (options.command != "pair" && options.command != "unpair" && options.command != "info" &&
                options.command != "apps" && options.command != "ls" && options.command != "transfer-probe")
                throw std::runtime_error("Unknown command " + options.command);
            if (options.udid.empty()) {
                auto found = awm::devices();
                if (found.size() != 1) throw std::runtime_error("Specify --udid when zero or multiple devices are connected");
                options.udid = found[0].at("udid").get<std::string>();
            }
            awm::Device device(options.udid);
            if (options.command == "pair") output = device.pair();
            else if (options.command == "unpair") output = device.unpair();
            else if (options.command == "info") output = device.info();
            else if (options.command == "apps") output = device.apps();
            else if (options.command == "ls") output = device.list(options.path);
            else output = device.transfer_probe(options.path, options.bytes);
        }
        print(output, options.json);
        return 0;
    } catch (const awm::DeviceError& error) {
        if (json) std::cout << awm::Json{{"error", error.what()}, {"code", error.code()}}.dump() << '\n';
        else std::cerr << error.what() << '\n';
        return 2;
    } catch (const std::exception& error) {
        if (json) std::cout << awm::Json{{"error", error.what()}}.dump() << '\n';
        else std::cerr << error.what() << '\n';
        return 1;
    }
}
