#include "CrashDebugHelper.h"
#include "ZBitAlloc.h"

#include <atomic>

#include <Globals.h>
#include <Logging.h>

#include <Glacier/ZEntityManager.h>
#include <Glacier/ZScene.h>
#include <Glacier/ZModule.h>

#include <MinHook.h>

static const char* c_ArrayPushBackPattern =
    "\x40\x53\x57\x48\x83\xEC\x38\x48\x8B\xD9\x48\x89\x6C\x24\x58\x48\x89\x74\x24\x60\x48\x8B\xEA\x48\x8B\x71\x10\x48\x8B\xCE\x48\xC1\xE9\x3E\x80\xE1\x01\x74\x10\x48\x8B\xC6\x40\x0F\xB6\xFE\x48\xC1\xE8\x08\x0F\xB6\xD0\xEB\x24\x48\x8B\x7B\x08\x48\x8B\xD6\x48\x2B\x3B\x48\xB8\xFF\xFF\xFF\xFF\xFF\xFF\xFF\x3F\x48\x23\xD0\x48\xC1\xFF\x03\x48\x2B\x13\x8B\xFF\x48\xC1\xFA\x03\x8B\xC2\x48\x3B\xF8";
static const char* c_ArrayPushBackMask =
    "xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx";
// The game has several byte-identical copies of push_back (one per ZArray<T> instantiation the linker didn't fold),
// so this pattern is expected to match more than once. The first copy is the one the template factories use.

// Render primitive slot overflows
//
// Every IRenderPrimitive allocates a slot index from the render manager's ZBitAlloc and stores it in
// m_BufferDataIndex. That index addresses Globals::PrimitiveBufferData, a fixed 65536-entry table.
//
// When the allocator is full the constructor leaves the index at its 0xFFFF failure sentinel - which is also a
// *valid* slot index, so consumers cannot tell it apart from a real allocation and end up sharing slot 65535.

// IRenderPrimitive::IRenderPrimitive - the prologue up to the 0x4000000000000000 constant, with the rip-relative
// vtable and xmm constant loads wildcarded.
static const char* c_PrimitiveCtorPattern =
    "\x48\x89\x5C\x24\x10\x57\x48\x83\xEC\x50\x48\x8B\xF9\xC7\x41\x08\x01\x00\x00\x00\x48\x8D\x05\x00\x00\x00\x00\x0F\x29\x74\x24\x40\x48\x89\x01\x0F\x57\xF6\x33\xC9\x0F\x29\x7C\x24\x30\x0F\x28\x3D\x00\x00\x00\x00\x48\xB8\x00\x00\x00\x00\x00\x00\x00\x40";
static const char* c_PrimitiveCtorMask =
    "xxxxxxxxxxxxxxxxxxxxxxx????xxxxxxxxxxxxxxxxxxxxx????xxxxxxxxxx";

// The render manager's unique-id allocator (m_PrimIds in the symbolled console builds), matched on the allocation
// site inside IRenderPrimitive's constructor. The stock SDK has no Globals::PrimitiveBufferIds, so resolve it here.
// Only the constructor itself is scanned, so a match can't come from anywhere else.
static const char* c_PrimitiveBufferIdsPattern = "\x48\x8D\x0D\x00\x00\x00\x00\xE8\x00\x00\x00\x00\x8B\x5C\x24\x60";
static const char* c_PrimitiveBufferIdsMask = "xxx????x????xxxx";
static const ptrdiff_t c_PrimitiveBufferIdsOffset = 3;
static const size_t c_PrimitiveCtorScanSize = 0x200;

// Globals::PrimitiveBufferData has this many entries, so the allocator handing out indices into it must match
static const uint32_t c_PrimitiveBufferCapacity = 65536;

static std::atomic<ZBitAlloc*> g_PrimitiveBufferIds = nullptr;

// The allocator may not be set up yet when Init runs, in which case it is checked on the first constructor call
static std::atomic<bool> g_PrimitiveBufferIdsValidated = false;

// How many times the constructor has run while the allocator was already at (or over) capacity.
static std::atomic<uint32_t> g_PrimitiveExcessCount = 0;

CrashDebugHelper* CrashDebugHelper::instance = nullptr;
CrashDebugHelper::ZArray_PushBack_t CrashDebugHelper::originalArrayPushBack = nullptr;
CrashDebugHelper::PrimitiveCtor_t CrashDebugHelper::originalPrimitiveCtor = nullptr;

static void FlushLoggers()
{
    const auto loggers = GetLoggers();

    for (size_t i = 0; i < loggers.Count; ++i)
    {
        loggers.Loggers[i]->flush();
    }
}

// SDK's Util::ProcessUtils::SearchPattern clone. If matchCount is given the whole range is scanned and every match
// counted, but the first match is still the one returned.
static uintptr_t SearchPattern(uintptr_t baseAddress, size_t scanSize, const uint8_t* pattern, const char* mask, size_t* matchCount = nullptr)
{
    const size_t patternSize = strlen(mask);

    if (matchCount != nullptr)
    {
        *matchCount = 0;
    }

    if (patternSize <= 1 || patternSize > scanSize)
    {
        return 0;
    }

    const uintptr_t searchEnd = baseAddress + scanSize - patternSize;
    uintptr_t firstMatch = 0;

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
            if (matchCount == nullptr)
            {
                return searchAddr;
            }

            if (++*matchCount == 1)
            {
                firstMatch = searchAddr;
            }
        }
    }

    return firstMatch;
}

// SDK's PatternHook clone
static void* FindGameFunction(const char* pattern, const char* mask, size_t* matchCount = nullptr)
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
        mask,
        matchCount
    ));
}

// SDK's PatternGlobalRelative clone, restricted to [baseAddress, baseAddress + scanSize) - finds an instruction that
// references a global via a rel32 displacement, then resolves that displacement to the global's address
static void* FindGameGlobal(uintptr_t baseAddress, size_t scanSize, const char* pattern, const char* mask, ptrdiff_t offset)
{
    const uintptr_t match = SearchPattern(baseAddress, scanSize, reinterpret_cast<const uint8_t*>(pattern), mask);

    if (match == 0)
    {
        return nullptr;
    }

    const uintptr_t relAddrPtr = match + offset;
    const int32_t relAddr = *reinterpret_cast<int32_t*>(relAddrPtr);

    return reinterpret_cast<void*>(relAddrPtr + relAddr + sizeof(int32_t));
}

// A wrong match here means reading m_nUsed / m_nCapacity from some unrelated global. A capacity of 0 means the
// render manager hasn't initialized the allocator yet, which can't be judged either way.
static bool IsPrimitiveBufferIdsPlausible(const ZBitAlloc* ids)
{
    return ids->m_nCapacity == c_PrimitiveBufferCapacity && ids->m_nUsed <= ids->m_nCapacity;
}

static void DisablePrimitiveBufferIds(const ZBitAlloc* ids)
{
    Logger::Error("CrashDebugHelper: The render primitive id allocator at {} looks wrong ({}/{} slots used, expected a capacity of {}). Primitive overflow detection is disabled - the pattern probably needs updating for this version of the game.", fmt::ptr(ids), ids->m_nUsed, ids->m_nCapacity, c_PrimitiveBufferCapacity);
    FlushLoggers();

    g_PrimitiveBufferIds = nullptr;
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
            FlushLoggers();
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

// Load tracking
//
// Remembers which template the game was configuring when it crashed, and how far through its sub-entities it got.
// Loading is single threaded, so this is plain global state. Only pointers and ids are recorded while loading - names
// are looked up in the crash handler.

struct STemplateFrame
{
    const ZTemplateEntityFactory* factory;
    uint64_t resourceId;
    // Index into the factory's m_pFactories of the latest sub-entity to start configuring, or -1 before the first
    int32_t lastSubEntity;
    uint32_t subEntitiesStarted;
    uint64_t lastSubEntityResourceId;
};

static const uint32_t c_MaxTemplateDepth = 32;

// Templates nest - a template's sub-entity can itself be a template
static STemplateFrame g_TemplateStack[c_MaxTemplateDepth];
static uint32_t g_TemplateDepth = 0;

// Kept once a template finishes, for crashes that happen after loading rather than during it
static STemplateFrame g_LastTemplate = {};

static char g_CurrentScene[512] = {};

// Exceptions that get caught further down are seen here too, so only report the first few
static const uint32_t c_MaxCrashReports = 3;
static std::atomic<uint32_t> g_CrashReportCount = 0;
static bool g_InCrashHandler = false;
static PVOID g_CrashHandler = nullptr;

static uint64_t PackResourceId(const ZRuntimeResourceID& rid)
{
    return (static_cast<uint64_t>(rid.m_IDHigh) << 32) | rid.m_IDLow;
}

// Called as each sub-entity starts configuring. A template factory's m_pFactories has one factory per sub-entity, so
// finding this one in it gives the sub-entity's index. Sub-entities can share a factory, so search on from the
// previous match first, which picks the right one when they are configured in order. Anything not found is a
// sub-entity of something other than the template (an aspect's, say) and is ignored.
static void NoteSubEntity(const IEntityFactory* factory, const ZRuntimeResourceID& rid)
{
    if (g_TemplateDepth == 0 || g_TemplateDepth > c_MaxTemplateDepth)
    {
        return;
    }

    STemplateFrame& frame = g_TemplateStack[g_TemplateDepth - 1];
    const auto& factories = frame.factory->m_pFactories;
    const auto count = static_cast<int32_t>(factories.size());

    int32_t found = -1;

    for (int32_t i = frame.lastSubEntity + 1; i < count && found < 0; ++i)
    {
        if (factories[i] == factory)
        {
            found = i;
        }
    }

    for (int32_t i = 0; i <= frame.lastSubEntity && i < count && found < 0; ++i)
    {
        if (factories[i] == factory)
        {
            found = i;
        }
    }

    if (found < 0)
    {
        return;
    }

    frame.lastSubEntity = found;
    frame.lastSubEntityResourceId = PackResourceId(rid);
    ++frame.subEntitiesStarted;
}

static void PushTemplate(const ZTemplateEntityFactory* factory)
{
    if (g_TemplateDepth < c_MaxTemplateDepth)
    {
        g_TemplateStack[g_TemplateDepth] = { factory, PackResourceId(factory->m_ridResource), -1, 0, 0 };
    }

    ++g_TemplateDepth;
}

static void PopTemplate()
{
    if (g_TemplateDepth == 0)
    {
        return;
    }

    --g_TemplateDepth;

    if (g_TemplateDepth < c_MaxTemplateDepth)
    {
        g_LastTemplate = g_TemplateStack[g_TemplateDepth];
    }
}

static void LogTemplate(const char* label, const STemplateFrame& frame)
{
    const auto* blueprintFactory = frame.factory->m_blueprintResource.GetResource();
    const STemplateEntityBlueprint* blueprint = blueprintFactory != nullptr ? blueprintFactory->m_pTemplateEntityBlueprint : nullptr;
    const size_t subEntityCount = frame.factory->m_pFactories.size();

    if (blueprint != nullptr && blueprint->rootEntityIndex >= 0 && static_cast<size_t>(blueprint->rootEntityIndex) < blueprint->subEntities.size())
    {
        Logger::Error("CRASH: {} template {:016X} ({}).", label, frame.resourceId, blueprint->subEntities[blueprint->rootEntityIndex].entityName);
    }
    else
    {
        Logger::Error("CRASH: {} template {:016X} (no blueprint to name it from).", label, frame.resourceId);
    }

    if (frame.lastSubEntity < 0)
    {
        Logger::Error("CRASH: None of its {} sub-entities had started configuring.", subEntityCount);
        return;
    }

    Logger::Error("CRASH: Got to sub-entity {} of {} ({} started), factory {:016X}.", frame.lastSubEntity + 1, subEntityCount, frame.subEntitiesStarted, frame.lastSubEntityResourceId);

    if (blueprint == nullptr || static_cast<size_t>(frame.lastSubEntity) >= blueprint->subEntities.size())
    {
        return;
    }

    const auto& subEntity = blueprint->subEntities[frame.lastSubEntity];
    Logger::Error("CRASH: That sub-entity is {} ({:016x}).", subEntity.entityName, subEntity.entityId);

    if (blueprint->subEntities.size() != subEntityCount)
    {
        Logger::Error("CRASH: The blueprint has {} sub-entities rather than {}, so that name may be wrong.", blueprint->subEntities.size(), subEntityCount);
    }
}

static bool IsCrashException(DWORD code)
{
    switch (code)
    {
    case EXCEPTION_ACCESS_VIOLATION:
    case EXCEPTION_IN_PAGE_ERROR:
    case EXCEPTION_ILLEGAL_INSTRUCTION:
    case EXCEPTION_PRIV_INSTRUCTION:
    case EXCEPTION_INT_DIVIDE_BY_ZERO:
        return true;
    default:
        return false;
    }
}

static void ReportCrash(const EXCEPTION_RECORD* record)
{
    const auto address = reinterpret_cast<uintptr_t>(record->ExceptionAddress);

    HMODULE module = nullptr;
    char modulePath[MAX_PATH] = "?";

    if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, reinterpret_cast<LPCSTR>(address), &module))
    {
        GetModuleFileNameA(module, modulePath, MAX_PATH);
    }

    const char* moduleName = strrchr(modulePath, '\\');
    moduleName = moduleName != nullptr ? moduleName + 1 : modulePath;

    Logger::Error("CRASH: Exception {:08X} at {}+{:X}.", record->ExceptionCode, moduleName, address - reinterpret_cast<uintptr_t>(module));

    if (record->ExceptionCode == EXCEPTION_ACCESS_VIOLATION && record->NumberParameters >= 2)
    {
        const auto operation = record->ExceptionInformation[0];
        Logger::Error("CRASH: {} of address {:X}.", operation == 0 ? "Read" : operation == 1 ? "Write" : "Execute", record->ExceptionInformation[1]);
    }

    if (g_CurrentScene[0] != '\0')
    {
        Logger::Error("CRASH: Scene: {}", g_CurrentScene);
    }

    if (g_TemplateDepth > 0)
    {
        LogTemplate("Configuring", g_TemplateStack[std::min(g_TemplateDepth, c_MaxTemplateDepth) - 1]);
    }
    else if (g_LastTemplate.factory != nullptr)
    {
        LogTemplate("Not configuring anything. Last configured", g_LastTemplate);
    }
}

static LONG CALLBACK CrashHandler(EXCEPTION_POINTERS* info)
{
    if (g_InCrashHandler || !IsCrashException(info->ExceptionRecord->ExceptionCode))
    {
        return EXCEPTION_CONTINUE_SEARCH;
    }

    if (g_CrashReportCount.fetch_add(1) >= c_MaxCrashReports)
    {
        return EXCEPTION_CONTINUE_SEARCH;
    }

    g_InCrashHandler = true;

    // Memory may well be corrupt by now - if reporting faults, give up on the report rather than make things worse
    __try
    {
        ReportCrash(info->ExceptionRecord);
        FlushLoggers();
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
    }

    g_InCrashHandler = false;

    return EXCEPTION_CONTINUE_SEARCH;
}

CrashDebugHelper::~CrashDebugHelper()
{
    RemoveArrayPushBackHook();
    RemovePrimitiveOverflowHook();

    if (g_CrashHandler != nullptr)
    {
        RemoveVectoredExceptionHandler(g_CrashHandler);
        g_CrashHandler = nullptr;
    }

    instance = nullptr;
}

void CrashDebugHelper::Init()
{
    // The SDK points the unhandled exception filter at Sentry or the engine's own handler. A vectored handler runs
    // before either of those and leaves them in place.
    g_CrashHandler = AddVectoredExceptionHandler(1, &CrashHandler);

    if (g_CrashHandler == nullptr)
    {
        Logger::Error("CrashDebugHelper: Could not install the crash handler. Error code: {}.", GetLastError());
        FlushLoggers();
    }

    // All of these need to be in place before the first scene loads, which is well before OnEngineInitialized
    Hooks::ZEntitySceneContext_SetLoadingStage->AddDetour(this, &CrashDebugHelper::ZEntitySceneContext_SetLoadingStage);
    Hooks::ZAspectEntityFactory_ConfigureEntity->AddDetour(this, &CrashDebugHelper::ZAspectEntityFactory_ConfigureEntity);
    Hooks::ZCppEntityFactory_ConfigureEntity->AddDetour(this, &CrashDebugHelper::ZCppEntityFactory_ConfigureEntity);
    Hooks::ZRenderMaterialEntityFactory_ConfigureEntity->AddDetour(this, &CrashDebugHelper::ZRenderMaterialEntityFactory_ConfigureEntity);
    InstallPrimitiveOverflowHook();
}

// The template hook calls the original itself so it can pop once the template is done, then returns - which skips any
// detour added after it. Detours run in the order they were added, so this one is only added at the first scene load
// after every mod's OnEngineInitialized (where MissionCompanion adds its own). A mod reloaded later goes after us and
// will be skipped.
// The original also runs while the SDK still holds the hook's shared SRW lock, and nested templates take it again -
// that only deadlocks if a detour is added to or removed from this hook in the middle of a load.
DEFINE_PLUGIN_DETOUR(CrashDebugHelper, void, ZTemplateEntityFactory_ConfigureEntity, ZTemplateEntityFactory* th, ZEntityType** pEntity, const SExternalReferences& externalRefs, uint8_t* unk0) {
    NoteSubEntity(th, th->m_ridResource);
    PushTemplate(th);
    p_Hook->CallOriginal(th, pEntity, externalRefs, unk0);
    PopTemplate();

    return HookResult<void>(HookAction::Return());
}

DEFINE_PLUGIN_DETOUR(CrashDebugHelper, void, ZAspectEntityFactory_ConfigureEntity, ZAspectEntityFactory* th, ZEntityType** pEntity, const SExternalReferences& externalRefs, uint8_t* unk0) {
    NoteSubEntity(th, th->m_ridResource);

    return HookResult<void>(HookAction::Continue());
}

DEFINE_PLUGIN_DETOUR(CrashDebugHelper, void, ZCppEntityFactory_ConfigureEntity, ZCppEntityFactory* th, ZEntityType** pEntity, const SExternalReferences& externalRefs, uint8_t* unk0) {
    NoteSubEntity(th, th->m_ridResource);

    return HookResult<void>(HookAction::Continue());
}

DEFINE_PLUGIN_DETOUR(CrashDebugHelper, void, ZRenderMaterialEntityFactory_ConfigureEntity, ZRenderMaterialEntityFactory* th, ZEntityType** pEntity, const SExternalReferences& externalRefs, uint8_t* unk0) {
    NoteSubEntity(th, th->m_ridResource);

    return HookResult<void>(HookAction::Continue());
}

void CrashDebugHelper::OnEngineInitialized() {
    Logger::Info("CrashDebugHelper has been initialized!");

    instance = this;
    engineInitialized = true;

    Hooks::ZEntitySceneContext_LoadScene->AddDetour(this, &CrashDebugHelper::OnLoadScene);
    InstallArrayPushBackHook();
}

// Scene Crashes
// Invalid scene or brick is trying to load
DEFINE_PLUGIN_DETOUR(CrashDebugHelper, bool, OnLoadScene, ZEntitySceneContext* th, SSceneInitParameters& p_parameters) {
    // ZString isn't guaranteed to be null terminated
    const size_t sceneLength = std::min<size_t>(p_parameters.m_SceneResource.size(), sizeof(g_CurrentScene) - 1);
    memcpy(g_CurrentScene, p_parameters.m_SceneResource.c_str(), sceneLength);
    g_CurrentScene[sceneLength] = '\0';

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
        Logger::Error("SCENE OR BRICK CRASH");
        for (auto message : *messageList)
        {
            Logger::Info("{}", message);
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
        FlushLoggers();
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
        FlushLoggers();
        ReleaseMinHook();
        return;
    }

    status = MH_EnableHook(target);

    if (status != MH_OK)
    {
        Logger::Error("CrashDebugHelper: Could not enable the ZArray::push_back hook at {}. Error code: {}.", fmt::ptr(target), static_cast<int>(status));
        FlushLoggers();
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
            Logger::Error("CRASH LIKELY: Missing entity template: {} ({:X}) - Scene Blueprint {:08X}{:08X} (Root Entity {} ({:X}))", entity->entityName, entity->entityId, factoryCall2->m_ridResource.m_IDHigh, factoryCall2->m_ridResource.m_IDLow, rootEntName, rootEntId);
            FlushLoggers();
        }
        mostRecentTemplate = -1;
    }
}

// Render Primitive Crashes
// Too many render primitives in a scene, overflowing the fixed slot table

void CrashDebugHelper::InstallPrimitiveOverflowHook()
{
    size_t ctorMatches = 0;
    void* target = FindGameFunction(c_PrimitiveCtorPattern, c_PrimitiveCtorMask, &ctorMatches);

    if (target == nullptr)
    {
        Logger::Error("CrashDebugHelper: Could not find the IRenderPrimitive constructor. Primitive overflow detection is disabled - the pattern probably needs updating for this version of the game.");
        FlushLoggers();
        return;
    }

    if (ctorMatches > 1)
    {
        Logger::Error("CrashDebugHelper: The IRenderPrimitive constructor pattern matched {} times. Primitive overflow detection is disabled - the pattern probably needs updating for this version of the game.", ctorMatches);
        FlushLoggers();
        return;
    }

    auto* ids = static_cast<ZBitAlloc*>(FindGameGlobal(
        reinterpret_cast<uintptr_t>(target),
        c_PrimitiveCtorScanSize,
        c_PrimitiveBufferIdsPattern,
        c_PrimitiveBufferIdsMask,
        c_PrimitiveBufferIdsOffset
    ));

    if (ids == nullptr)
    {
        Logger::Error("CrashDebugHelper: Could not find the render primitive id allocator in the IRenderPrimitive constructor at {}. Primitive overflow detection is disabled - the pattern probably needs updating for this version of the game.", fmt::ptr(target));
        FlushLoggers();
        return;
    }

    Logger::Debug("CrashDebugHelper: Render primitive id allocator at {}, {}/{} slots used.", fmt::ptr(ids), ids->m_nUsed, ids->m_nCapacity);

    if (ids->m_nCapacity != 0)
    {
        if (!IsPrimitiveBufferIdsPlausible(ids))
        {
            DisablePrimitiveBufferIds(ids);
            return;
        }

        g_PrimitiveBufferIdsValidated = true;
    }

    g_PrimitiveBufferIds = ids;

    if (Globals::PrimitiveBufferData != nullptr)
    {
        Logger::Debug("CrashDebugHelper: Render primitive buffer table at {}.", fmt::ptr(Globals::PrimitiveBufferData));
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
        FlushLoggers();
        ReleaseMinHook();
        return;
    }

    status = MH_EnableHook(target);

    if (status != MH_OK)
    {
        Logger::Error("CrashDebugHelper: Could not enable the IRenderPrimitive constructor hook at {}. Error code: {}.", fmt::ptr(target), static_cast<int>(status));
        FlushLoggers();
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
    ZBitAlloc* ids = g_PrimitiveBufferIds;

    // The constructor compares against the capacity, so the allocator has to be initialized by now
    if (ids != nullptr && !g_PrimitiveBufferIdsValidated && !g_PrimitiveBufferIdsValidated.exchange(true))
    {
        if (!IsPrimitiveBufferIdsPlausible(ids))
        {
            DisablePrimitiveBufferIds(ids);
            ids = nullptr;
        }
    }

    if (ids != nullptr)
    {
        const auto used = ids->m_nUsed;
        const auto capacity = ids->m_nCapacity;

        if (used >= capacity)
        {
            const uint32_t excess = g_PrimitiveExcessCount.fetch_add(1) + 1;

            Logger::Error("CRASH LIKELY: Excess render primitive ({} + {})/{} - slot 0xFFFF is being shared", used, excess, capacity);

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

DEFINE_PLUGIN_DETOUR(CrashDebugHelper, void, ZEntitySceneContext_SetLoadingStage, ZEntitySceneContext* th, ESceneLoadingStage stage) {
    if (stage == ESceneLoadingStage::eLoading_Start)
    {
        // Start counting overflows fresh for the scene that's about to load
        g_PrimitiveExcessCount = 0;

        if (engineInitialized && !templateHookAdded)
        {
            Hooks::ZTemplateEntityFactory_ConfigureEntity->AddDetour(this, &CrashDebugHelper::ZTemplateEntityFactory_ConfigureEntity);
            templateHookAdded = true;
        }
    }

    const ZBitAlloc* ids = g_PrimitiveBufferIds;

    if (stage == ESceneLoadingStage::eLoading_ScenePlaying && ids != nullptr)
    {
        Logger::Info("Scene finished loading. Render primitive slots: {}/{} filled, {} overflow(s).", ids->m_nUsed, ids->m_nCapacity, g_PrimitiveExcessCount.load());
    }

    return HookResult<void>(HookAction::Continue());
}

DECLARE_ZHM_PLUGIN(CrashDebugHelper);
