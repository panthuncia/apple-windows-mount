add_library(awm_device STATIC device.cpp diagnostics.cpp)
target_link_libraries(awm_device PUBLIC
    unofficial::libimobiledevice::libimobiledevice
    unofficial::libusbmuxd::libusbmuxd nlohmann_json::nlohmann_json
    libusb::libusb bcrypt setupapi cfgmgr32 advapi32)
target_include_directories(awm_device PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_compile_options(awm_device PRIVATE /W4 /EHsc /utf-8)
add_executable(awm main.cpp)
target_link_libraries(awm PRIVATE awm_device)
target_compile_options(awm PRIVATE /W4 /EHsc /utf-8)
install(TARGETS awm RUNTIME_DEPENDENCY_SET awm_runtime RUNTIME DESTINATION bin)
add_executable(awm-driver driver.cpp)
target_compile_definitions(awm-driver PRIVATE UNICODE _UNICODE)
target_link_libraries(awm-driver PRIVATE nlohmann_json::nlohmann_json setupapi newdev advapi32 cfgmgr32 ole32 libusb::libusb)
target_compile_options(awm-driver PRIVATE /W4 /EHsc /utf-8)
install(TARGETS awm-driver RUNTIME_DEPENDENCY_SET awm_runtime RUNTIME DESTINATION bin)
