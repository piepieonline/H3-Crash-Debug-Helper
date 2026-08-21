#include "CrashDebugHelper.h"

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

CrashDebugHelper* CrashDebugHelper::instance = nullptr;
CrashDebugHelper::ZArray_PushBack_t CrashDebugHelper::originalArrayPushBack = nullptr;

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

CrashDebugHelper::~CrashDebugHelper()
{
    RemoveArrayPushBackHook();

    instance = nullptr;
}

void CrashDebugHelper::OnEngineInitialized() {
    Logger::Info("CrashDebugHelper has been initialized!");

    instance = this;

    Hooks::ZEntitySceneContext_LoadScene->AddDetour(this, &CrashDebugHelper::OnLoadScene);
    InstallArrayPushBackHook();
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
        return;
    }

    // We have our own copy of MinHook, separate from the SDK's. That's fine as long as the two never hook the same function
    const MH_STATUS initStatus = MH_Initialize();

    if (initStatus != MH_OK && initStatus != MH_ERROR_ALREADY_INITIALIZED)
    {
        Logger::Error("CrashDebugHelper: Could not initialize MinHook. Error code: {}.", static_cast<int>(initStatus));
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
        MH_Uninitialize();
        return;
    }

    status = MH_EnableHook(target);

    if (status != MH_OK)
    {
        Logger::Error("CrashDebugHelper: Could not enable the ZArray::push_back hook at {}. Error code: {}.", fmt::ptr(target), static_cast<int>(status));
        MH_RemoveHook(target);
        MH_Uninitialize();
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
    MH_Uninitialize();

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

DECLARE_ZHM_PLUGIN(CrashDebugHelper);
