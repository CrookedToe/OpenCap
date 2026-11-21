cmake_minimum_required(VERSION 3.24)
project(OpenCap VERSION 0.1.0 LANGUAGES CXX)

if(NOT WIN32 OR NOT CMAKE_SIZEOF_VOID_P EQUAL 8)
    message(FATAL_ERROR "OpenCap targets 64-bit Windows.")
endif()

include(FetchContent)
include(CTest)
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
add_compile_definitions(UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN)
add_compile_options("$<$<CXX_COMPILER_ID:MSVC>:/utf-8>")

option(OPENCAP_RECEIVER "Build physical UDCAP receiver support" ON)
set(OPENVR_SDK_ROOT "" CACHE PATH "Path to an existing OpenVR SDK")
if(NOT OPENVR_SDK_ROOT)
    FetchContent_Declare(openvr
        URL https://github.com/ValveSoftware/openvr/archive/0924064316de3effbcd1acf1e309182a2deb1c05.tar.gz
        SOURCE_SUBDIR unused)
    FetchContent_MakeAvailable(openvr)
    set(OPENVR_SDK_ROOT "${openvr_SOURCE_DIR}")
endif()

add_library(openvr_client STATIC IMPORTED)
set_target_properties(openvr_client PROPERTIES
    IMPORTED_LOCATION "${OPENVR_SDK_ROOT}/lib/win64/openvr_api.lib"
    INTERFACE_INCLUDE_DIRECTORIES "${OPENVR_SDK_ROOT}/headers")

add_library(opencap_bridge STATIC src/bridge.cpp src/model.cpp)
target_include_directories(opencap_bridge PUBLIC src)

add_executable(OpenCap WIN32 src/desktop.cpp src/stream.cpp)
target_link_libraries(OpenCap PRIVATE opencap_bridge shell32 ole32 comdlg32 ws2_32)
if(OPENCAP_RECEIVER)
    set(BUILD_TEST_TOOLS OFF CACHE BOOL "" FORCE)
    set(COPY_HIDAPI_DLL OFF CACHE BOOL "" FORCE)
    FetchContent_Declare(community_core
        URL https://github.com/OldestNova/UdCap-Community-HandDriver-Core/archive/14987ad6751761fde0f587f4687cb6aaa8e46414.tar.gz)
    FetchContent_MakeAvailable(community_core)
    target_sources(OpenCap PRIVATE src/receiver.cpp)
    target_link_libraries(OpenCap PRIVATE UdCapCommunityDriverCore)
    target_compile_definitions(OpenCap PRIVATE OPENCAP_RECEIVER=1)
endif()

set(hand_sample "${OPENVR_SDK_ROOT}/samples/drivers/drivers/handskeletonsimulation/src")
add_library(driver_opencap SHARED src/driver.cpp "${hand_sample}/hand_simulation.cpp")
target_include_directories(driver_opencap PRIVATE "${OPENVR_SDK_ROOT}/headers"
    "${hand_sample}" "${OPENVR_SDK_ROOT}/samples/drivers/utils/vrmath")
target_link_libraries(driver_opencap PRIVATE opencap_bridge)

add_executable(OpenCapDashboard WIN32 src/dashboard.cpp)
target_link_libraries(OpenCapDashboard PRIVATE opencap_bridge openvr_client gdi32)

install(TARGETS OpenCap OpenCapDashboard RUNTIME DESTINATION .)
install(TARGETS driver_opencap RUNTIME DESTINATION steamvr/opencap/bin/win64)
install(DIRECTORY steamvr/opencap/ DESTINATION steamvr/opencap)
install(FILES steamvr/opencap.vrmanifest DESTINATION .)
install(FILES scripts/Set-SteamVRRegistration.ps1 DESTINATION .)
install(FILES LICENSE DESTINATION .)
if(EXISTS "${OPENVR_SDK_ROOT}/bin/win64/openvr_api.dll")
    install(FILES "${OPENVR_SDK_ROOT}/bin/win64/openvr_api.dll" DESTINATION .)
endif()
if(EXISTS "${OPENVR_SDK_ROOT}/LICENSE")
    install(FILES "${OPENVR_SDK_ROOT}/LICENSE" DESTINATION licenses RENAME OpenVR.txt)
endif()
if(OPENCAP_RECEIVER)
    install(FILES "${community_core_SOURCE_DIR}/LICENSE" DESTINATION licenses RENAME Community-Core.txt)
    install(FILES $<TARGET_RUNTIME_DLLS:OpenCap> DESTINATION .)
endif()

if(BUILD_TESTING)
    add_executable(OpenCapChecks tests/checks.cpp)
    target_link_libraries(OpenCapChecks PRIVATE opencap_bridge)
    add_test(NAME OpenCapChecks COMMAND OpenCapChecks)
endif()
