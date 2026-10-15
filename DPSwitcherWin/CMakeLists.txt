cmake_minimum_required(VERSION 3.24)
project(DPSwitcherWin LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")   # static CRT -> single exe

set(DPS_DEFINITIONS UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX _WIN32_WINNT=0x0A00 WINVER=0x0A00)
set(DPS_OPTIONS /W4 /WX /permissive- /EHsc /utf-8 /Zc:__cplusplus /bigobj)

# ---- The app ------------------------------------------------------------------------------
file(GLOB_RECURSE SRC CONFIGURE_DEPENDS src/*.cpp)
add_executable(DPSwitcher WIN32 ${SRC} app.rc app.manifest)   # CMake embeds .manifest with MSVC

target_include_directories(DPSwitcher PRIVATE src)
target_compile_definitions(DPSwitcher PRIVATE ${DPS_DEFINITIONS})
target_compile_options(DPSwitcher PRIVATE ${DPS_OPTIONS})
target_precompile_headers(DPSwitcher PRIVATE src/pch.h)       # C++/WinRT headers dominate compile time

target_link_libraries(DPSwitcher PRIVATE
    windowsapp        # C++/WinRT / WinRT ABI
    CoreMessaging     # CreateDispatcherQueueController
    d3d11 dxgi d2d1 dcomp dwmapi dxva2 dxguid avrt
    user32 gdi32 gdiplus shell32 comctl32 shcore)

# ---- Diagnostic: live DDC/CI error-rate graph, sharing the real app's DdcWorker -----------
add_executable(DdcProbe WIN32 tools/DdcProbe.cpp src/Ddc.cpp src/util/Log.cpp)
target_include_directories(DdcProbe PRIVATE src)
target_compile_definitions(DdcProbe PRIVATE ${DPS_DEFINITIONS})
target_compile_options(DdcProbe PRIVATE ${DPS_OPTIONS})
target_precompile_headers(DdcProbe PRIVATE src/pch.h)
target_link_libraries(DdcProbe PRIVATE windowsapp dwmapi dxva2 user32 gdi32 shell32 shcore)

# ---- Headless state-machine tests (no WinRT, no window, no monitor) ----------------------
enable_testing()
add_executable(SwitchControllerTests
    tests/SwitchControllerTests.cpp
    src/SwitchController.cpp
    src/util/Log.cpp)
target_include_directories(SwitchControllerTests PRIVATE src)
target_compile_definitions(SwitchControllerTests PRIVATE ${DPS_DEFINITIONS})
target_compile_options(SwitchControllerTests PRIVATE ${DPS_OPTIONS})
target_link_libraries(SwitchControllerTests PRIVATE user32 shell32 ole32)
add_test(NAME SwitchControllerTests COMMAND SwitchControllerTests)
