#include "CrashDebugHelper.h"

#include <Globals.h>
#include <Logging.h>

#include <Glacier/ZEntityManager.h>
#include <Glacier/ZScene.h>
#include <Glacier/ZModule.h>

void CrashDebugHelper::OnEngineInitialized() {
    Logger::Info("CrashDebugHelper has been initialized!");

    Hooks::ZEntitySceneContext_LoadScene->AddDetour(this, &CrashDebugHelper::OnLoadScene);
    Hooks::Unknown_In_ZTemplateEntityBlueprintFactory_ctor->AddDetour(this, &CrashDebugHelper::Unknown_In_ZTemplateEntityBlueprintFactory_ctor);
}

// Scene Crashes
// Invalid scene or brick is trying to load
DEFINE_PLUGIN_DETOUR(CrashDebugHelper, void, OnLoadScene, ZEntitySceneContext* th, ZSceneData& p_SceneData) {
    __try
    {
        p_Hook->CallOriginal(th, p_SceneData);
        return HookResult<void>(HookAction::Return());
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        SceneLoadCrashHandler();
    }
}

void CrashDebugHelper::SceneLoadCrashHandler()
{
    Logger::Error("SCENE OR BRICK CRASH");
    LogInvalidSceneTemps(
        Globals::Hitman5Module->m_pEntitySceneContext->m_SceneConfig.m_ridSceneFactory,
        Globals::Hitman5Module->m_pEntitySceneContext->m_sceneData.m_sceneName
    );
    for (int i = 0; i < Globals::Hitman5Module->m_pEntitySceneContext->m_SceneConfig.m_aAdditionalBrickFactoryRIDs.size(); i++)
    {
        LogInvalidSceneTemps(
            Globals::Hitman5Module->m_pEntitySceneContext->m_SceneConfig.m_aAdditionalBrickFactoryRIDs[i],
            Globals::Hitman5Module->m_pEntitySceneContext->m_sceneData.m_sceneBricks[i]
        );
    }
    Logger::Flush();
}

void CrashDebugHelper::LogInvalidSceneTemps(ZRuntimeResourceID resourceID, ZString ridPath)
{
    TResourcePtr<ZTemplateEntityBlueprintFactory> resPtr;
    Globals::ResourceManager->GetResourcePtr(resPtr, resourceID, 0);
    auto resInfo = resPtr.GetResourceInfo();
    auto resData = resPtr.GetResourceData();

    if (resInfo.status == EResourceStatus::RESOURCE_STATUS_FAILED)
    {
        Logger::Error("CRASH: Failed to load: {} (TEMP: {:08X}{:08X})", ridPath, resInfo.rid.m_IDHigh, resInfo.rid.m_IDLow);
    }
    else
    {
        Logger::Info("Loaded: {} (TEMP: {:08X}{:08X})", ridPath, resInfo.rid.m_IDHigh, resInfo.rid.m_IDLow);
    }
}

// Entity Crashes
// Unknown TEMP is referenced

DEFINE_PLUGIN_DETOUR(CrashDebugHelper, void*, Unknown_In_ZTemplateEntityBlueprintFactory_ctor, void* a1, void* a2)
{
    auto factoryCall1 = reinterpret_cast<ZTemplateEntityBlueprintFactory*>((uintptr_t*)((char*)a1 - 48));
    if (factoryCall1->IsTemplateEntityBlueprintFactory())
    {
        mostRecentTemplate = *(int*)a2;
    }

    auto factoryCall2 = reinterpret_cast<ZTemplateEntityBlueprintFactory*>((uintptr_t*)((char*)a1 - 216));
    if (factoryCall2->IsTemplateEntityBlueprintFactory())
    {
        if (mostRecentTemplate == 0)
        {
            auto entManager = Globals::EntityManager->m_pContext;

            for (auto& s_Brick : entManager->m_aLoadedBricks)
            {
                if (s_Brick.runtimeResourceID != ResId<"[assembly:/_sdk/hitmen.brick].pc_entitytype">)
                    continue;
            }

            auto mainSceneResourceId = ResId<"[assembly:/_pro/scenes/missions/skunk/scene_skunk_mild_solitaire.entity].pc_entitytemplate">;
            auto brickResourceId = ResId<"[assembly:/_pro/scenes/missions/skunk/ass_night_mild.brick].pc_entitytype">;

            auto entity = reinterpret_cast<STemplateBlueprintSubEntity*>((uintptr_t*)((char*)a2 - 0x28));
            auto rootEntName = factoryCall2->m_pTemplateEntityBlueprint->subEntities[factoryCall2->m_pTemplateEntityBlueprint->rootEntityIndex].entityName;
            auto rootEntId = factoryCall2->m_pTemplateEntityBlueprint->subEntities[factoryCall2->m_pTemplateEntityBlueprint->rootEntityIndex].entityId;
            Logger::Error("CRASH LIKELY: Missing entity template: {} ({:X}) - Blueprint {:08X}{:08X} (Root Entity {} ({:X}))", entity->entityName, entity->entityId, factoryCall2->m_ridResource.m_IDHigh, factoryCall2->m_ridResource.m_IDLow, rootEntName, rootEntId);
            Logger::Flush();
        }
        mostRecentTemplate = -1;
    }

    return HookResult<void*>(HookAction::Continue());
}

DECLARE_ZHM_PLUGIN(CrashDebugHelper);
