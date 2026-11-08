#include "SaveEntityRefDetector.h"

#include "../HookUtils.h"

#include <cstddef>

#include <Logging.h>

// Save handlers append every serialized entity ref (not deduplicated, unchecked) to a fixed table; overflow corrupts its count
static const char* c_UpdateSaveStatesPattern =
    "\x40\x55\x53\x56\x57\x41\x54\x41\x56\x41\x57\x48\x8D\x6C\x24\xF0\x48\x81\xEC\x10\x01\x00\x00\x8B\x81\x80\x01\x00\x00\x48\x8B\xF9\x83\xF8\x0A\x0F\x85\x00\x00\x00\x00\x33\xF6";
static const char* c_UpdateSaveStatesMask = "xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx????xx";

static const ptrdiff_t c_SaveSystemStateOffset = 0x180;
static const ptrdiff_t c_SaveSystemJobOffset = 0x148;
static const ptrdiff_t c_SaveJobEntityRefTableOffset = 0x160;
static const ptrdiff_t c_EntityRefTableCountOffset = 3200000;
static const uint32_t c_EntityRefTableCapacity = 400000;
static const uint32_t c_EntityRefTableWarnPercent = 80;

static const int c_SaveStateSerialize = 10;

SaveEntityRefDetector::ZSaveSystem_UpdateSaveStates_t SaveEntityRefDetector::originalUpdateSaveStates = nullptr;

SaveEntityRefDetector::~SaveEntityRefDetector()
{
    Remove();
}

void SaveEntityRefDetector::Install()
{
    void* target = FindGameFunction(c_UpdateSaveStatesPattern, c_UpdateSaveStatesMask);

    if (target == nullptr)
    {
        Logger::Error("CrashDebugHelper: Could not find ZSaveSystem's save state update. Save entity reference reporting is disabled - the pattern probably needs updating for this version of the game.");
        return;
    }

    if (InstallMinHook(
        target,
        reinterpret_cast<void*>(&SaveEntityRefDetector::ZSaveSystem_UpdateSaveStatesDetour),
        reinterpret_cast<void**>(&originalUpdateSaveStates),
        "ZSaveSystem save state update"
    ))
    {
        updateSaveStatesTarget = target;
    }
}

void SaveEntityRefDetector::Remove()
{
    RemoveMinHook(updateSaveStatesTarget);
}

void __fastcall SaveEntityRefDetector::ZSaveSystem_UpdateSaveStatesDetour(uintptr_t saveSystem)
{
    const int stateBefore = *reinterpret_cast<int*>(saveSystem + c_SaveSystemStateOffset);

    originalUpdateSaveStates(saveSystem);

    if (stateBefore != c_SaveStateSerialize)
    {
        return;
    }

    const uintptr_t saveJob = *reinterpret_cast<uintptr_t*>(saveSystem + c_SaveSystemJobOffset);
    const uintptr_t entityRefTable = saveJob == 0 ? 0 : *reinterpret_cast<uintptr_t*>(saveJob + c_SaveJobEntityRefTableOffset);

    if (entityRefTable == 0)
    {
        return;
    }

    const uint32_t used = *reinterpret_cast<uint32_t*>(entityRefTable + c_EntityRefTableCountOffset);
    const uint32_t percent = static_cast<uint32_t>(static_cast<uint64_t>(used) * 100 / c_EntityRefTableCapacity);

    if (used >= c_EntityRefTableWarnPercent * (c_EntityRefTableCapacity / 100))
    {
        Logger::Warn("CrashDebugHelper: SAVE CRASH LIKELY SOON: Save used {}/{} entity reference slots ({}%). Saving past {} crashes the game.", used, c_EntityRefTableCapacity, percent, c_EntityRefTableCapacity);
        FlushLoggers();
    }
    else
    {
        Logger::Info("CrashDebugHelper: Save used {}/{} entity reference slots ({}%).", used, c_EntityRefTableCapacity, percent);
    }
}
