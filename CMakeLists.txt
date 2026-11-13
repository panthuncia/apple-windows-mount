cmake_minimum_required(VERSION 3.25...4.3)
project(AppleWindowsMount VERSION 0.1.0 LANGUAGES C CXX)

if(NOT WIN32 OR NOT CMAKE_SIZEOF_VOID_P EQUAL 8)
    message(FATAL_ERROR "Apple Windows Mount requires 64-bit Windows")
endif()
set(CMAKE_C_STANDARD 11)
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")
add_compile_definitions(WIN32_LEAN_AND_MEAN NOMINMAX _CRT_SECURE_NO_WARNINGS _WIN32_WINNT=0x0A00)
find_path(LIBUSB_INCLUDE_DIR libusb.h PATH_SUFFIXES libusb-1.0 REQUIRED)
find_library(LIBUSB_LIBRARY NAMES libusb-1.0 REQUIRED)
find_library(LIBUSB_LIBRARY_DEBUG NAMES libusb-1.0 PATH_SUFFIXES debug/lib REQUIRED)
add_library(libusb::libusb UNKNOWN IMPORTED)
set_target_properties(libusb::libusb PROPERTIES
    IMPORTED_LOCATION "${LIBUSB_LIBRARY}"
    IMPORTED_LOCATION_DEBUG "${LIBUSB_LIBRARY_DEBUG}"
    INTERFACE_INCLUDE_DIRECTORIES "${LIBUSB_INCLUDE_DIR}")
find_package(unofficial-libplist CONFIG REQUIRED)
find_package(unofficial-libimobiledevice-glue CONFIG REQUIRED)
find_package(unofficial-libusbmuxd CONFIG REQUIRED)
find_package(unofficial-libimobiledevice CONFIG REQUIRED)
find_package(nlohmann_json CONFIG REQUIRED)

add_subdirectory(src/transport)
add_subdirectory(src/client)
add_subdirectory(src/filesystem)
add_subdirectory(src/control)
include(CTest)
if(BUILD_TESTING)
    add_subdirectory(tests)
endif()
install(FILES LICENSE THIRD_PARTY.md DESTINATION share/apple-windows-mount)
install(FILES "${WINFSP_INCLUDE_DIR}/../License.txt" DESTINATION share/apple-windows-mount/licenses/winfsp)
set(CMAKE_INSTALL_SYSTEM_RUNTIME_DESTINATION bin)
include(InstallRequiredSystemLibraries)
install(RUNTIME_DEPENDENCY_SET awm_runtime
    DIRECTORIES "$<TARGET_FILE_DIR:awm>" "$<TARGET_FILE_DIR:awm-usbmuxd>"
    PRE_EXCLUDE_REGEXES "api-ms-.*" "ext-ms-.*"
    POST_EXCLUDE_REGEXES ".*[\\/][Ss]ystem32[\\/].*" ".*[\\/][Ww]in[Ss]x[Ss][\\/].*"
    RUNTIME DESTINATION bin LIBRARY DESTINATION bin)
foreach(dependency libusb libplist libimobiledevice-glue libusbmuxd libimobiledevice openssl nlohmann-json)
    install(FILES "${VCPKG_INSTALLED_DIR}/${VCPKG_TARGET_TRIPLET}/share/${dependency}/copyright"
        DESTINATION "share/apple-windows-mount/licenses/${dependency}")
endforeach()
set(CPACK_GENERATOR ZIP)
set(CPACK_PACKAGE_NAME AppleWindowsMount)
set(CPACK_PACKAGE_VERSION ${PROJECT_VERSION})
include(CPack)
