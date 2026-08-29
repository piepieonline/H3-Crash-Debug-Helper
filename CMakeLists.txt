cmake_minimum_required(VERSION 3.15)

project(CrashDebugHelper CXX)

# Find latest version at https://github.com/OrfeasZ/ZHMModSDK/releases
# Set ZHMMODSDK_DIR variable to a local directory to use a local copy of the ZHMModSDK.
set(ZHMMODSDK_VER "v4.1.0")
# set(ZHMMODSDK_DIR "D:/Game Modding/Hitman/2021 Tools/ZHMModSDK")

include(cmake/setup-zhmmodsdk.cmake)

# Add libraries
find_package(directxtk12 CONFIG REQUIRED)
find_package(directx-headers CONFIG REQUIRED)

# The SDK statically links its own copy of MinHook and doesn't expose it
find_package(minhook CONFIG REQUIRED)

# The v4.1.0 dev package stopped shipping implot, which IModSDK.h still includes
find_package(implot CONFIG REQUIRED)

# vcpkg's implot.h sits next to vcpkg's own imgui.h, which would shadow the patched one the SDK ships
get_target_property(IMPLOT_VCPKG_INCLUDE_DIR implot::implot INTERFACE_INCLUDE_DIRECTORIES)
set(IMPLOT_INCLUDE_DIR "${CMAKE_CURRENT_BINARY_DIR}/implot-include")
file(COPY
    "${IMPLOT_VCPKG_INCLUDE_DIR}/implot.h"
    "${IMPLOT_VCPKG_INCLUDE_DIR}/implot_internal.h"
    DESTINATION "${IMPLOT_INCLUDE_DIR}"
)

# Set C++ standard to C++23.
set(CMAKE_CXX_STANDARD 23)

# Set version numbers
set(VERSION_MAJOR 1)
set(VERSION_MINOR 0)
set(VERSION_PATCH 0)
set(VERSION_BUILD 0)

# Configure the version.rc file
configure_file(
    ${CMAKE_CURRENT_SOURCE_DIR}/version.rc.in
    ${CMAKE_CURRENT_BINARY_DIR}/version.rc
    @ONLY
)

# Create the CrashDebugHelper mod library.
add_library(CrashDebugHelper SHARED
    src/CrashDebugHelper.cpp
    src/CrashDebugHelper.h
    src/ZBitAlloc.h
)

# Set UTF-8 flag.
set_target_properties(CrashDebugHelper PROPERTIES COMPILE_FLAGS "/utf-8")

# Ahead of the SDK's include dir, so implot.h resolves here and its "imgui.h" resolves to the SDK's
target_include_directories(CrashDebugHelper PRIVATE "${IMPLOT_INCLUDE_DIR}")

# Add SDK and DX headers dependencies.
find_package(directx-headers CONFIG REQUIRED)

target_link_libraries(CrashDebugHelper
    ZHMModSDK
    implot::implot
    minhook::minhook
    Microsoft::DirectXTK12
    Microsoft::DirectX-Guids
    Microsoft::DirectX-Headers
    version
)

install(TARGETS CrashDebugHelper
    RUNTIME DESTINATION bin
)

# Install the mod to the game folder when the `GAME_INSTALL_PATH` variable is set.
zhmmodsdk_install(CrashDebugHelper)
