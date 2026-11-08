#include "SceneLoadDetector.h"

#include "../HookUtils.h"

#include <format>

#include <Globals.h>
#include <Logging.h>

#include <Glacier/ZModule.h>
#include <Glacier/ZEntityManager.h>
#include <Glacier/ZResource.h>

SceneLoadDetector::~SceneLoadDetector()
{
    Hooks::ZEntitySceneContext_LoadScene->RemoveDetoursWithContext(this);
}

void SceneLoadDetector::Install()
{
    Hooks::ZEntitySceneContext_LoadScene->AddDetour(this, &SceneLoadDetector::OnLoadScene);
}

DEFINE_DETOUR_WITH_CONTEXT(SceneLoadDetector, bool, OnLoadScene, ZEntitySceneContext* th, SSceneInitParameters& p_parameters) {
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

void SceneLoadDetector::SceneLoadCrashHandler()
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

bool SceneLoadDetector::LogInvalidSceneTemps(ZRuntimeResourceID resourceID, ZString ridPath, std::vector<std::string>* outputMessageList)
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
