#include "CrashDebugHelper.h"

#include <Globals.h>
#include <Logging.h>

#include <Glacier/ZEntityManager.h>
#include <Glacier/ZScene.h>
#include <Glacier/ZModule.h>

extern void FlushLoggers();

void CrashDebugHelper::OnEngineInitialized() {
    Logger::Info("CrashDebugHelper has been initialized!");

    Hooks::ZEntitySceneContext_LoadScene->AddDetour(this, &CrashDebugHelper::OnLoadScene);
    Hooks::ZArray_PushBack->AddDetour(this, &CrashDebugHelper::ZArray_PushBack);
}

// Scene Crashes
// Invalid scene or brick is trying to load
DEFINE_PLUGIN_DETOUR(CrashDebugHelper, void, OnLoadScene, ZEntitySceneContext* th, SSceneInitParameters& p_parameters) {
    // I'd rather not use __try, but calling GetResourcePtr during scene load causes a crash... so make sure we only call it if we are crashing anyway, rather than preemptively
    __try
    {
        p_Hook->CallOriginal(th, p_parameters);
        return HookResult<void>(HookAction::Return());
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        SceneLoadCrashHandler();
    }
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

DEFINE_PLUGIN_DETOUR(CrashDebugHelper, void*, ZArray_PushBack, void* th, void* newData)
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

    return HookResult<void*>(HookAction::Continue());
}

DECLARE_ZHM_PLUGIN(CrashDebugHelper);
