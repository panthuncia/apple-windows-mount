add_library(usbmux_protocol STATIC
    ../../third_party/usbmuxd/client.c
    ../../third_party/usbmuxd/device.c
    ../../third_party/usbmuxd/usb.c
    windows_compat.c
    config_store.cpp
)
target_include_directories(usbmux_protocol PUBLIC ${PROJECT_SOURCE_DIR}/third_party/usbmuxd ${CMAKE_CURRENT_SOURCE_DIR})
target_compile_options(usbmux_protocol PRIVATE /FIwindows_compat.h)
target_compile_options(usbmux_protocol PRIVATE /utf-8)
target_link_libraries(usbmux_protocol PUBLIC libusb::libusb unofficial::libplist::libplist
    unofficial::libimobiledevice-glue::libimobiledevice-glue ws2_32 advapi32 bcrypt iphlpapi ole32)
add_executable(awm-usbmuxd main.cpp)
target_link_libraries(awm-usbmuxd PRIVATE usbmux_protocol)
install(TARGETS awm-usbmuxd RUNTIME_DEPENDENCY_SET awm_runtime RUNTIME DESTINATION bin)
