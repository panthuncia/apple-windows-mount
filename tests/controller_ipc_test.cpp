// SPDX-License-Identifier: GPL-3.0-or-later
#include "ipc.h"
#include <thread>
#include <iostream>
int main()
{
    std::atomic<bool> stop{false};
    std::exception_ptr failure;
    auto name = awm::pipe_name() + L"-test-" + std::to_wstring(GetCurrentProcessId());
    std::thread server([&] {
        try {
            awm::serve_controller([](const awm::Json& request) {
                if (request.value("fail", false)) throw std::runtime_error("Injected controller error");
                return awm::Json{{"echo", request}};
            }, stop, name);
        } catch (...) { failure = std::current_exception(); }
    });
    int status = 0;
    try {
        for (unsigned attempt = 0; attempt < 100 && !WaitNamedPipeW(name.c_str(), 10); ++attempt) Sleep(10);
        for (unsigned index = 0; index < 50; ++index) {
            auto response = awm::controller_request({{"sequence", index}}, false, name);
            if (response.at("echo").at("sequence") != index) throw std::runtime_error("Controller frame mismatch");
        }
        auto response = awm::controller_request({{"fail", true}}, false, name);
        if (response.at("error") != "Injected controller error") throw std::runtime_error("Controller exception response lost");
    } catch (const std::exception& error) { std::cerr << error.what() << '\n'; status = 1; }
    stop = true;
    server.join();
    if (failure) { try { std::rethrow_exception(failure); } catch (const std::exception& error) { std::cerr << error.what() << '\n'; } status = 1; }
    return status;
}
