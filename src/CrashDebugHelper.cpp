#include "CrashDebugHelper.h"
#include "ZBitAlloc.h"

#include <atomic>

#include <Globals.h>
#include <Logging.h>

#include <Glacier/ZEntityManager.h>
#include <Glacier/ZScene.h>
#include <Glacier/ZModule.h>

#include <MinHook.h>

// ZArray::push_back - four instantiations share this prologue, so the pattern runs on to the element size that separates them
static const char* c_ArrayPushBackPattern =
    "\x40\x53\x57\x48\x83\xEC\x38\x48\x8B\xD9\x48\x89\x6C\x24\x58\x48\x89\x74\x24\x60\x48\x8B\xEA\x48\x8B\x71\x10\x48\x8B\xCE\x48\xC1"
    "\xE9\x3E\x80\xE1\x01\x74\x10\x48\x8B\xC6\x40\x0F\xB6\xFE\x48\xC1\xE8\x08\x0F\xB6\xD0\xEB\x24\x48\x8B\x7B\x08\x48\x8B\xD6\x48\x2B"
    "\x3B\x48\xB8\xFF\xFF\xFF\xFF\xFF\xFF\xFF\x3F\x48\x23\xD0\x48\xC1\xFF\x03\x48\x2B\x13\x8B\xFF\x48\xC1\xFA\x03\x8B\xC2\x48\x3B\xF8"
    "\x0F\x85\x09\x01\x00\x00\x48\x8B\xC7\x4C\x89\x7C\x24\x30\x48\xC1\xE8\x02\x48\x8D\x57\x01\x48\x03\xD0\x4C\x8D\x3C\xD5\x00\x00\x00"
    "\x00\x49\x83\xFF\x10\x77\x32\x84\xC9\x74\x2E\x48\x8B\x45\x00\x48\x89\x04\xFB\x48\x8D\x47\x01\x48\xC1\xE2\x08\x48\x0B\xD0\x48\x8B"
    "\x43\x10\x48\x25\x00\x00\xFF\xFF\x48\x0B\xD0\x48\x89\x53\x10\x4C\x8B\x7C\x24\x30\xE9\xFD\x00\x00\x00\x48\xB8\x00\x00\x00\x00\x00"
    "\x00\x00\xC0\x4C\x89\x74\x24\x68\x45\x33\xF6\x48\x23\xF0\x66\x44\x89\x74\x24\x50\xE8\x87\x31\xFC\xFF\x4C\x8D\x4C\x24\x50\x4C\x89"
    "\x74\x24\x20\x45\x8D\x46\x08";
static const char* c_ArrayPushBackMask =
    "xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx"
    "xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx????xxxxxxxxxxxxxxxxxxxxxxxxxx"
    "xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx????xxxxxxx"
    "xxxxxxxxxxxxxxxxxxxxx????xxxxxxxxxxxxxx";

// Render primitive slot overflows
//
// Every IRenderPrimitive allocates a slot index from the render manager's ZBitAlloc and stores it in
// m_BufferDataIndex. That index addresses Globals::PrimitiveBufferData, a fixed 65536-entry table.
//
// When the allocator is full the constructor leaves the index at its 0xFFFF failure sentinel - which is also a
// *valid* slot index, so consumers cannot tell it apart from a real allocation and end up sharing slot 65535.

// The render manager's unique-id allocator (m_PrimIds in the symbolled console builds), matched on the allocation
// site inside IRenderPrimitive's constructor. The stock SDK has no Globals::PrimitiveBufferIds, so resolve it here.
static const char* c_PrimitiveBufferIdsPattern = "\x48\x8D\x0D\x00\x00\x00\x00\xE8\x00\x00\x00\x00\x8B\x5C\x24\x60";
static const char* c_PrimitiveBufferIdsMask = "xxx????x????xxxx";
static const ptrdiff_t c_PrimitiveBufferIdsOffset = 3;

// IRenderPrimitive::IRenderPrimitive, matched on its prologue with the rel32 to its vtable wildcarded
static const char* c_PrimitiveCtorPattern =
    "\x48\x89\x5C\x24\x10\x57\x48\x83\xEC\x50\x48\x8B\xF9\xC7\x41\x08\x01\x00\x00\x00\x48\x8D\x05\x7D\xF6\xD6\x00\x0F\x29\x74\x24\x40";
static const char* c_PrimitiveCtorMask = "xxxxxxxxxxxxxxxxxxxxxxx????xxxxx";

static ZBitAlloc* g_PrimitiveBufferIds = nullptr;

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

// How many times the constructor has run while the allocator was already at (or over) capacity.
static std::atomic<uint32_t> g_PrimitiveExcessCount = 0;

CrashDebugHelper* CrashDebugHelper::instance = nullptr;
CrashDebugHelper::ZArray_PushBack_t CrashDebugHelper::originalArrayPushBack = nullptr;
CrashDebugHelper::PrimitiveCtor_t CrashDebugHelper::originalPrimitiveCtor = nullptr;
CrashDebugHelper::ZSaveSystem_UpdateSaveStates_t CrashDebugHelper::originalUpdateSaveStates = nullptr;

static void FlushLoggers()
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

// SDK's PatternHook clone
static void* FindGameFunction(const char* pattern, const char* mask)
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

// SDK's PatternGlobalRelative clone - finds an instruction that references a global via a rel32 displacement,
// then resolves that displacement to the global's address
static void* FindGameGlobal(const char* pattern, const char* mask, ptrdiff_t offset)
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
// function. Both of our hooks share the one instance, so it is only torn down once the last of them is gone.
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

CrashDebugHelper::~CrashDebugHelper()
{
    RemoveArrayPushBackHook();
    RemovePrimitiveOverflowHook();
    RemoveSaveEntityRefHook();

    instance = nullptr;
}

void CrashDebugHelper::Init()
{
    // Both of these need to be in place before the first scene loads, which is well before OnEngineInitialized
    Hooks::ZEntitySceneContext_SetLoadingStage->AddDetour(this, &CrashDebugHelper::ZEntitySceneContext_SetLoadingStage);
    InstallPrimitiveOverflowHook();
}

void CrashDebugHelper::OnEngineInitialized() {
    Logger::Info("CrashDebugHelper: Initialized!");

    instance = this;

    Hooks::ZEntitySceneContext_LoadScene->AddDetour(this, &CrashDebugHelper::OnLoadScene);
    InstallArrayPushBackHook();
    InstallSaveEntityRefHook();
}

// Scene Crashes
// Invalid scene or brick is trying to load
DEFINE_PLUGIN_DETOUR(CrashDebugHelper, bool, OnLoadScene, ZEntitySceneContext* th, SSceneInitParameters& p_parameters) { 
    bool res = false;
    // I'd rather not use __try, but calling GetResourcePtr during scene load causes a crash... so make sure we only call it if we are crashing anyway, rather than preemptively
    __try {
        res = p_Hook->CallOriginal(th, p_parameters);
    }
    __except (EXCEPTION_EXECUTE_HANDLER) {
        SceneLoadCrashHandler();
        return HookResult<bool>(HookAction::Return(), false);
    }

    return HookResult<bool>(HookAction::Return(), res);
}

void CrashDebugHelper::SceneLoadCrashHandler()
{
    bool wasErrorFound = false;
    auto messageList = new std::vector<std::string>();

    if (LogInvalidSceneTemps(
        Globals::Hitman5Module->m_pEntitySceneContext->m_SceneConfig.m_ridSceneFactory,
        Globals::Hitman5Module->m_pEntitySceneContext->m_SceneInitParameters.m_SceneResource,
        messageList
    )) wasErrorFound = true;
    for (int i = 0; i < Globals::Hitman5Module->m_pEntitySceneContext->m_SceneConfig.m_aAdditionalBrickFactoryRIDs.size(); i++)
    {
        if (LogInvalidSceneTemps(
            Globals::Hitman5Module->m_pEntitySceneContext->m_SceneConfig.m_aAdditionalBrickFactoryRIDs[i],
            Globals::Hitman5Module->m_pEntitySceneContext->m_SceneInitParameters.m_aAdditionalBrickResources[i],
            messageList
        )) wasErrorFound = true;
    }

    if (wasErrorFound)
    {
        Logger::Error("CrashDebugHelper: SCENE OR BRICK CRASH");
        for (auto message : *messageList)
        {
            Logger::Info("CrashDebugHelper: {}", message);
        }
    }

    FlushLoggers();
}

bool CrashDebugHelper::LogInvalidSceneTemps(ZRuntimeResourceID resourceID, ZString ridPath, std::vector<std::string>* outputMessageList)
{
    TResourcePtr<ZTemplateEntityBlueprintFactory> resPtr;
    Globals::ResourceManager->GetResourcePtr(resPtr, resourceID, 0);
    auto resInfo = resPtr.GetResourceInfo();
    auto resData = resPtr.GetResourceData();

    if (resInfo.status == EResourceStatus::RESOURCE_STATUS_FAILED)
    {
        outputMessageList->push_back(std::format("CRASH: Failed to load: {} (TEMP: {:08X}{:08X})", ridPath.c_str(), resInfo.rid.m_IDHigh, resInfo.rid.m_IDLow));
        return true;
    }
    else
    {
        outputMessageList->push_back(std::format("Loaded: {} (TEMP: {:08X}{:08X})", ridPath.c_str(), resInfo.rid.m_IDHigh, resInfo.rid.m_IDLow));
        return false;
    }
}

// Entity Crashes
// Unknown TEMP is referenced

void CrashDebugHelper::InstallArrayPushBackHook()
{
    void* target = FindGameFunction(c_ArrayPushBackPattern, c_ArrayPushBackMask);

    if (target == nullptr)
    {
        Logger::Error("CrashDebugHelper: Could not find ZArray::push_back. Missing entity template detection is disabled - the pattern probably needs updating for this version of the game.");
        return;
    }

    if (!AcquireMinHook())
    {
        return;
    }

    MH_STATUS status = MH_CreateHook(
        target,
        reinterpret_cast<LPVOID>(&CrashDebugHelper::ZArray_PushBackDetour),
        reinterpret_cast<LPVOID*>(&originalArrayPushBack)
    );

    if (status != MH_OK)
    {
        Logger::Error("CrashDebugHelper: Could not create the ZArray::push_back hook at {}. Error code: {}.", fmt::ptr(target), static_cast<int>(status));
        ReleaseMinHook();
        return;
    }

    status = MH_EnableHook(target);

    if (status != MH_OK)
    {
        Logger::Error("CrashDebugHelper: Could not enable the ZArray::push_back hook at {}. Error code: {}.", fmt::ptr(target), static_cast<int>(status));
        MH_RemoveHook(target);
        ReleaseMinHook();
        return;
    }

    arrayPushBackTarget = target;

    Logger::Debug("CrashDebugHelper: Hooked ZArray::push_back at {}.", fmt::ptr(target));
}

void CrashDebugHelper::RemoveArrayPushBackHook()
{
    if (arrayPushBackTarget == nullptr)
    {
        return;
    }

    MH_DisableHook(arrayPushBackTarget);
    MH_RemoveHook(arrayPushBackTarget);
    ReleaseMinHook();

    arrayPushBackTarget = nullptr;
}

void* __fastcall CrashDebugHelper::ZArray_PushBackDetour(void* th, void* newData)
{
    if (instance != nullptr)
    {
        instance->OnArrayPushBack(th, newData);
    }

    return originalArrayPushBack(th, newData);
}

void CrashDebugHelper::OnArrayPushBack(void* th, void* newData)
{
    // Calculated in the middle of a function, no previous function call makes it easy to associate the null constructor with the template
    // This isn't clean... but it works

    // The first call is adding the factory for the entity - if it's null, record it
    auto factoryCall1 = reinterpret_cast<ZTemplateEntityBlueprintFactory*>((uintptr_t*)((char*)th - 48));
    if (factoryCall1->IsTemplateEntityBlueprintFactory())
    {
        mostRecentTemplate = *(int*)newData;
    }

    // The second is actually adding the SubEntity, which, if the previous call was null, gives us the entity that has an issue
    auto factoryCall2 = reinterpret_cast<ZTemplateEntityBlueprintFactory*>((uintptr_t*)((char*)th - 216));
    if (factoryCall2->IsTemplateEntityBlueprintFactory())
    {
        if (mostRecentTemplate == 0)
        {
            auto entity = reinterpret_cast<STemplateBlueprintSubEntity*>((uintptr_t*)((char*)newData - 0x28));
            auto rootEntName = factoryCall2->m_pTemplateEntityBlueprint->subEntities[factoryCall2->m_pTemplateEntityBlueprint->rootEntityIndex].entityName;
            auto rootEntId = factoryCall2->m_pTemplateEntityBlueprint->subEntities[factoryCall2->m_pTemplateEntityBlueprint->rootEntityIndex].entityId;
            Logger::Error("CrashDebugHelper: CRASH LIKELY: Missing entity template: {} ({:X}) - Scene Blueprint {:08X}{:08X} (Root Entity {} ({:X}))", entity->entityName, entity->entityId, factoryCall2->m_ridResource.m_IDHigh, factoryCall2->m_ridResource.m_IDLow, rootEntName, rootEntId);
            FlushLoggers();
        }
        mostRecentTemplate = -1;
    }
}

// Render Primitive Crashes
// Too many render primitives in a scene, overflowing the fixed slot table

void CrashDebugHelper::InstallPrimitiveOverflowHook()
{
    g_PrimitiveBufferIds = static_cast<ZBitAlloc*>(
        FindGameGlobal(c_PrimitiveBufferIdsPattern, c_PrimitiveBufferIdsMask, c_PrimitiveBufferIdsOffset)
    );

    if (g_PrimitiveBufferIds == nullptr)
    {
        Logger::Error("CrashDebugHelper: Could not find the render primitive id allocator. Primitive overflow detection is disabled - the pattern probably needs updating for this version of the game.");
        return;
    }

    Logger::Debug("CrashDebugHelper: Render primitive id allocator at {}, {}/{} slots used.", fmt::ptr(g_PrimitiveBufferIds), g_PrimitiveBufferIds->m_nUsed, g_PrimitiveBufferIds->m_nCapacity);

    if (Globals::PrimitiveBufferData != nullptr)
    {
        Logger::Debug("CrashDebugHelper: Render primitive buffer table at {}.", fmt::ptr(Globals::PrimitiveBufferData));
    }

    void* target = FindGameFunction(c_PrimitiveCtorPattern, c_PrimitiveCtorMask);

    if (target == nullptr)
    {
        Logger::Error("CrashDebugHelper: Could not find the IRenderPrimitive constructor. Primitive overflow detection is disabled - the pattern probably needs updating for this version of the game.");
        return;
    }

    if (!AcquireMinHook())
    {
        return;
    }

    MH_STATUS status = MH_CreateHook(
        target,
        reinterpret_cast<LPVOID>(&CrashDebugHelper::PrimitiveCtorDetour),
        reinterpret_cast<LPVOID*>(&originalPrimitiveCtor)
    );

    if (status != MH_OK)
    {
        Logger::Error("CrashDebugHelper: Could not create the IRenderPrimitive constructor hook at {}. Error code: {}.", fmt::ptr(target), static_cast<int>(status));
        ReleaseMinHook();
        return;
    }

    status = MH_EnableHook(target);

    if (status != MH_OK)
    {
        Logger::Error("CrashDebugHelper: Could not enable the IRenderPrimitive constructor hook at {}. Error code: {}.", fmt::ptr(target), static_cast<int>(status));
        MH_RemoveHook(target);
        ReleaseMinHook();
        return;
    }

    primitiveCtorTarget = target;

    Logger::Debug("CrashDebugHelper: Hooked the IRenderPrimitive constructor at {}.", fmt::ptr(target));
}

void CrashDebugHelper::RemovePrimitiveOverflowHook()
{
    if (primitiveCtorTarget == nullptr)
    {
        return;
    }

    MH_DisableHook(primitiveCtorTarget);
    MH_RemoveHook(primitiveCtorTarget);
    ReleaseMinHook();

    primitiveCtorTarget = nullptr;
}

uintptr_t __fastcall CrashDebugHelper::PrimitiveCtorDetour(uintptr_t a1, uintptr_t a2, uintptr_t a3, uintptr_t a4)
{
    if (g_PrimitiveBufferIds != nullptr)
    {
        const auto used = g_PrimitiveBufferIds->m_nUsed;
        const auto capacity = g_PrimitiveBufferIds->m_nCapacity;

        if (used >= capacity)
        {
            const uint32_t excess = g_PrimitiveExcessCount.fetch_add(1) + 1;

            Logger::Error("CrashDebugHelper: CRASH LIKELY: Excess render primitive ({} + {})/{} - slot 0xFFFF is being shared", used, excess, capacity);

            // Only worth paying for on the first one - after that the scene is already doomed and this can fire
            // thousands of times
            if (excess == 1)
            {
                FlushLoggers();
            }
        }
    }

    return originalPrimitiveCtor(a1, a2, a3, a4);
}

// Save Crashes
// Too many entity references serialized in one save, overflowing the fixed reference table

void CrashDebugHelper::InstallSaveEntityRefHook()
{
    void* target = FindGameFunction(c_UpdateSaveStatesPattern, c_UpdateSaveStatesMask);

    if (target == nullptr)
    {
        Logger::Error("CrashDebugHelper: Could not find ZSaveSystem's save state update. Save entity reference reporting is disabled - the pattern probably needs updating for this version of the game.");
        return;
    }

    if (!AcquireMinHook())
    {
        return;
    }

    MH_STATUS status = MH_CreateHook(
        target,
        reinterpret_cast<LPVOID>(&CrashDebugHelper::ZSaveSystem_UpdateSaveStatesDetour),
        reinterpret_cast<LPVOID*>(&originalUpdateSaveStates)
    );

    if (status != MH_OK)
    {
        Logger::Error("CrashDebugHelper: Could not create the ZSaveSystem save state hook at {}. Error code: {}.", fmt::ptr(target), static_cast<int>(status));
        ReleaseMinHook();
        return;
    }

    status = MH_EnableHook(target);

    if (status != MH_OK)
    {
        Logger::Error("CrashDebugHelper: Could not enable the ZSaveSystem save state hook at {}. Error code: {}.", fmt::ptr(target), static_cast<int>(status));
        MH_RemoveHook(target);
        ReleaseMinHook();
        return;
    }

    updateSaveStatesTarget = target;

    Logger::Debug("CrashDebugHelper: Hooked ZSaveSystem's save state update at {}.", fmt::ptr(target));
}

void CrashDebugHelper::RemoveSaveEntityRefHook()
{
    if (updateSaveStatesTarget == nullptr)
    {
        return;
    }

    MH_DisableHook(updateSaveStatesTarget);
    MH_RemoveHook(updateSaveStatesTarget);
    ReleaseMinHook();

    updateSaveStatesTarget = nullptr;
}

void __fastcall CrashDebugHelper::ZSaveSystem_UpdateSaveStatesDetour(uintptr_t saveSystem)
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

DEFINE_PLUGIN_DETOUR(CrashDebugHelper, void, ZEntitySceneContext_SetLoadingStage, ZEntitySceneContext* th, ESceneLoadingStage stage) {
    if (stage == ESceneLoadingStage::eLoading_Start)
    {
        // Start counting overflows fresh for the scene that's about to load
        g_PrimitiveExcessCount = 0;
    }

    if (stage == ESceneLoadingStage::eLoading_ScenePlaying && g_PrimitiveBufferIds != nullptr)
    {
        Logger::Info("CrashDebugHelper: Scene finished loading. Render primitive slots: {}/{} filled, {} overflow(s).", g_PrimitiveBufferIds->m_nUsed, g_PrimitiveBufferIds->m_nCapacity, g_PrimitiveExcessCount.load());
    }

    return HookResult<void>(HookAction::Continue());
}

DECLARE_ZHM_PLUGIN(CrashDebugHelper);
