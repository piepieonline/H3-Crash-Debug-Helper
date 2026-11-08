#include "HookUtils.h"

#include <cstdint>
#include <cstring>

#include <Common.h>
#include <Logging.h>

#include <MinHook.h>

void FlushLoggers()
{
    const auto loggers = GetLoggers();

    for (size_t i = 0; i < loggers.Count; ++i)
    {
        loggers.Loggers[i]->flush();
    }
}

// SDK's Util::ProcessUtils::SearchPattern clone
static uintptr_t SearchPattern(uintptr_t baseAddress, size_t scanSize, const uint8_t* pattern, const char* mask)
{
    const size_t patternSize = strlen(mask);

    if (patternSize <= 1 || patternSize > scanSize)
    {
        return 0;
    }

    const uintptr_t searchEnd = baseAddress + scanSize - patternSize;

    for (uintptr_t searchAddr = baseAddress; searchAddr <= searchEnd; ++searchAddr)
    {
        const uint8_t* memoryPtr = reinterpret_cast<uint8_t*>(searchAddr);

        bool found = true;

        for (size_t i = 0; i < patternSize; ++i)
        {
            if (mask[i] == '?')
            {
                continue;
            }

            if (memoryPtr[i] != pattern[i])
            {
                found = false;
                break;
            }
        }

        if (found)
        {
            return searchAddr;
        }
    }

    return 0;
}

void* FindGameFunction(const char* pattern, const char* mask)
{
    const HMODULE module = GetModuleHandleA(nullptr);

    if (module == nullptr)
    {
        return nullptr;
    }

    const auto* dosHeader = reinterpret_cast<PIMAGE_DOS_HEADER>(module);
    const auto* ntHeader = reinterpret_cast<PIMAGE_NT_HEADERS>(reinterpret_cast<uintptr_t>(module) + dosHeader->e_lfanew);

    const uintptr_t baseOfCode = reinterpret_cast<uintptr_t>(module) + ntHeader->OptionalHeader.BaseOfCode;

    return reinterpret_cast<void*>(SearchPattern(
        baseOfCode,
        ntHeader->OptionalHeader.SizeOfCode,
        reinterpret_cast<const uint8_t*>(pattern),
        mask
    ));
}

void* FindGameGlobal(const char* pattern, const char* mask, ptrdiff_t offset)
{
    const auto match = reinterpret_cast<uintptr_t>(FindGameFunction(pattern, mask));

    if (match == 0)
    {
        return nullptr;
    }

    const uintptr_t relAddrPtr = match + offset;
    const int32_t relAddr = *reinterpret_cast<int32_t*>(relAddrPtr);

    return reinterpret_cast<void*>(relAddrPtr + relAddr + sizeof(int32_t));
}

// We have our own copy of MinHook, separate from the SDK's. That's fine as long as the two never hook the same
// function. All of our hooks share the one instance, so it is only torn down once the last of them is gone.
static int g_MinHookUsers = 0;

static bool AcquireMinHook()
{
    if (g_MinHookUsers == 0)
    {
        const MH_STATUS status = MH_Initialize();

        if (status != MH_OK && status != MH_ERROR_ALREADY_INITIALIZED)
        {
            Logger::Error("CrashDebugHelper: Could not initialize MinHook. Error code: {}.", static_cast<int>(status));
            return false;
        }
    }

    ++g_MinHookUsers;

    return true;
}

static void ReleaseMinHook()
{
    if (g_MinHookUsers == 0)
    {
        return;
    }

    if (--g_MinHookUsers == 0)
    {
        MH_Uninitialize();
    }
}

bool InstallMinHook(void* target, void* detour, void** original, const char* hookName)
{
    if (!AcquireMinHook())
    {
        return false;
    }

    MH_STATUS status = MH_CreateHook(target, detour, original);

    if (status != MH_OK)
    {
        Logger::Error("CrashDebugHelper: Could not create the {} hook at {}. Error code: {}.", hookName, fmt::ptr(target), static_cast<int>(status));
        ReleaseMinHook();
        return false;
    }

    status = MH_EnableHook(target);

    if (status != MH_OK)
    {
        Logger::Error("CrashDebugHelper: Could not enable the {} hook at {}. Error code: {}.", hookName, fmt::ptr(target), static_cast<int>(status));
        MH_RemoveHook(target);
        ReleaseMinHook();
        return false;
    }

    Logger::Debug("CrashDebugHelper: Hooked {} at {}.", hookName, fmt::ptr(target));

    return true;
}

void RemoveMinHook(void*& target)
{
    if (target == nullptr)
    {
        return;
    }

    MH_DisableHook(target);
    MH_RemoveHook(target);
    ReleaseMinHook();

    target = nullptr;
}
