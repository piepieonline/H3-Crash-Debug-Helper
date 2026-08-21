# CrashDebugHelper

A Hitman SDK mod that adds additional error messages to help modders track down sources of game crashes.

Current additional error messages are logged when:

- An scene or brick file is missing when loading a scene
- A referenced entity TEMP is not found when loading a scene
- A scene creates more render primitives than the engine has slots for. (The count is also reported once each scene finishes loading)

## Installation Instructions

1. Download the latest version of [ZHMModSDK](https://github.com/OrfeasZ/ZHMModSDK) and install it.
2. Download the latest version of `CrashDebugHelper` and copy it to the ZHMModSDK `mods` folder (e.g. `C:\Games\HITMAN 3\Retail\mods`).
3. Run the game and once in the main menu, press the `~` key (`^` on QWERTZ layouts) and enable `CrashDebugHelper` from the menu at the top of the screen (you may need to restart your game afterwards).
4. Enjoy!

## Building on Windows

### 1. Clone this repository locally with all submodules.

You can either use `git clone --recurse-submodules` or run `git submodule update --init --recursive` after cloning.

### 2. Install Visual Studio (any edition).

Make sure you install the C++ and game development workloads.

### 3. Open the project in your IDE of choice.

See instructions for [Visual Studio](https://github.com/OrfeasZ/ZHMModSDK/wiki/Setting-up-Visual-Studio-for-development) or [CLion](https://github.com/OrfeasZ/ZHMModSDK/wiki/Setting-up-CLion-for-development).

## Building on Linux (cross-compiling to Windows)

There is no Linux build of the game, so this cross-compiles a Windows DLL with `clang-cl` +
`lld-link` against the real MSVC CRT and Windows SDK headers, which
[xwin](https://github.com/Jake-Shadle/xwin) downloads from Microsoft.

All of the cross-compilation machinery (the Nix dev shell, the `clang-cl` toolchain file, the
vcpkg triplet, and the cross-build overlay ports) lives in the ZHMModSDK repository, so a local
checkout of the SDK is required — the released `DevPkg-ZHMModSDK.zip` does not contain it.

You will need [Nix](https://nixos.org/download/) with flakes enabled. Everything else (CMake,
Ninja, clang, lld, vcpkg, xwin, wine) comes from the dev shell.

### 1. Clone this repository and the SDK.

```sh
git clone --recurse-submodules https://github.com/piepieonline/H3-Crash-Debug-Helper.git
git clone --recurse-submodules https://github.com/OrfeasZ/ZHMModSDK.git
```

### 2. Fetch the MSVC CRT and Windows SDK.

From the ZHMModSDK directory:

```sh
nix develop --command cmake/scripts/fetch-xwin.sh
```

This only needs to be done once. It leaves roughly 1.7 GB in `ZHMModSDK/.xwin` (800 MB of splatted
headers and libraries, the rest a download cache that can be deleted afterwards). The dev shell
exports `XWIN_SPLAT_DIR` pointing at it, which is how the toolchain file finds the headers and
import libraries.

### 3. Create `CMakeUserPresets.json` in this repository.

This file is gitignored because the paths are machine-specific. Substitute your own paths for
`/path/to/ZHMModSDK` and the game directory:

```json
{
    "version": 3,
    "configurePresets": [
        {
            "name": "x64-Release-Cross",
            "displayName": "Cross-compilation from Linux to Windows",
            "generator": "Ninja",
            "binaryDir": "${sourceDir}/_build/${presetName}",
            "architecture": { "value": "x64", "strategy": "external" },
            "condition": {
                "type": "equals",
                "lhs": "${hostSystemName}",
                "rhs": "Linux"
            },
            "cacheVariables": {
                "CMAKE_BUILD_TYPE": "RelWithDebInfo",
                "CMAKE_MSVC_RUNTIME_LIBRARY": "MultiThreaded",
                "CMAKE_INSTALL_PREFIX": "${sourceDir}/_install/${presetName}",
                "CMAKE_TOOLCHAIN_FILE": {
                    "value": "/path/to/ZHMModSDK/External/vcpkg/scripts/buildsystems/vcpkg.cmake",
                    "type": "FILEPATH"
                },
                "VCPKG_OVERLAY_TRIPLETS": "${sourceDir}/cmake/vcpkg-overlays",
                "VCPKG_OVERLAY_PORTS": "/path/to/ZHMModSDK/cmake/vcpkg-ports-cross",
                "VCPKG_TARGET_TRIPLET": "x64-windows-zhm-cross",
                "VCPKG_HOST_TRIPLET": "x64-linux",
                "VCPKG_CHAINLOAD_TOOLCHAIN_FILE": "${sourceDir}/cmake/toolchains/clang-cl.cmake",
                "VCPKG_INSTALL_OPTIONS": "--allow-unsupported",
                "VCPKG_APPLOCAL_DEPS": "OFF",
                "CMAKE_POLICY_DEFAULT_CMP0091": "NEW",
                "ZHMMODSDK_DIR": "/path/to/ZHMModSDK",
                "ZHMMODSDK_PRESET": "x64-Release-Cross"
            }
        },
        {
            "name": "x64-Release-Cross-Install",
            "inherits": ["x64-Release-Cross"],
            "cacheVariables": {
                "GAME_INSTALL_PATH": "/path/to/SteamLibrary/steamapps/common/HITMAN 3"
            }
        }
    ]
}
```

Three of those settings are worth explaining:

- `CMAKE_TOOLCHAIN_FILE` points at the **SDK's** vcpkg checkout rather than this repository's
  `vcpkg` submodule. Both would work, but sharing one vcpkg registry means `directxtk12` resolves
  to the same port version the SDK itself was built with, and the packages come straight out of
  the shared binary cache instead of being rebuilt.
- `VCPKG_OVERLAY_PORTS` is required. Upstream `directxtk12` cannot cross-build — its shader step
  runs a Windows batch file and expects a host `dxc.exe`. The SDK's overlay port runs both under
  wine.
- `ZHMMODSDK_PRESET` must name a cross preset. Without it the SDK builds itself with the default
  `x64-Release` preset, which uses `cl.exe` and cannot configure on Linux.

Setting `ZHMMODSDK_DIR` also means the `ZHMMODSDK_VER` release download in `CMakeLists.txt` is
bypassed entirely, and the SDK is built from your checkout instead.

### 4. Build.

The dev shell derives `XWIN_SPLAT_DIR` and `VCPKG_ROOT` from the working directory, so it must be
entered from the ZHMModSDK directory, then changed into this one:

```sh
cd /path/to/ZHMModSDK
nix develop --command bash -c '
    cd /path/to/H3-Crash-Debug-Helper &&
    cmake --preset x64-Release-Cross . &&
    cmake --build _build/x64-Release-Cross --parallel
'
```

Configuring builds and installs the SDK first (`cmake/setup-zhmmodsdk.cmake` shells out to a nested
SDK build), so the first run takes a while. The result is
`_build/x64-Release-Cross/CrashDebugHelper.dll`.

The DLL is much larger than an MSVC-built one because the toolchain emits DWARF alongside CodeView,
so `gdb` on the Linux side can read symbols directly out of the DLL while the game runs under
Proton.

### 5. Install (optional).

Use the `-Install` preset and run the install step:

```sh
cd /path/to/ZHMModSDK
nix develop --command bash -c '
    cd /path/to/H3-Crash-Debug-Helper &&
    cmake --preset x64-Release-Cross-Install . &&
    cmake --build _build/x64-Release-Cross-Install --parallel &&
    cmake --install _build/x64-Release-Cross-Install
'
```

`.vscode/tasks.json` wraps these same commands, so `Ctrl+Shift+B` runs build + install from the
editor. `cross: configure` is the one to run after editing `CMakeLists.txt` or the presets.

Note that this copies `CrashDebugHelper.dll` into `Retail/mods` **and** overwrites
`Retail/ZHMModSDK.dll` with the locally built SDK. That is deliberate — the mod is compiled against
your local SDK's headers and import library, so the matching runtime has to be installed with it.
Any other mods in that folder were built against whatever SDK you had before, so you may want to
rebuild those too.
