cmake_minimum_required(VERSION 3.15)

project(CrashDebugHelper CXX)

# Find latest version at https://github.com/OrfeasZ/ZHMModSDK/releases
# Set ZHMMODSDK_DIR variable to a local directory to use a local copy of the ZHMModSDK.
set(ZHMMODSDK_VER "v4.0.0-rc.1")
set(ZHMMODSDK_ARTIFACT_ID "4414569588") # Use an artifact for github actions while it's not merged
# set(ZHMMODSDK_DIR "D:/Game Modding/Hitman/2021 Tools/ZHMModSDK")

include(cmake/setup-zhmmodsdk.cmake)

# Add libraries
find_package(directxtk12 CONFIG REQUIRED)
find_package(directx-headers CONFIG REQUIRED)

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
)

# Set UTF-8 flag.
set_target_properties(CrashDebugHelper PROPERTIES COMPILE_FLAGS "/utf-8")

# Add SDK and DX headers dependencies.
find_package(directx-headers CONFIG REQUIRED)

target_link_libraries(CrashDebugHelper
    ZHMModSDK
    Microsoft::DirectXTK12
    Microsoft::DirectX-Guids
    Microsoft::DirectX-Headers
    Version
)

install(TARGETS CrashDebugHelper
    RUNTIME DESTINATION bin
)

# Install the mod to the game folder when the `GAME_INSTALL_PATH` variable is set.
zhmmodsdk_install(CrashDebugHelper)
