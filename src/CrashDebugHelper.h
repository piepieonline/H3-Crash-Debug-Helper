#pragma once

#include <IPluginInterface.h>

class CrashDebugHelper : public IPluginInterface {
public:
    void OnEngineInitialized() override;

private:
    DECLARE_PLUGIN_DETOUR(CrashDebugHelper, void, OnLoadScene, ZEntitySceneContext* th, ZSceneData& p_SceneData);
    DECLARE_PLUGIN_DETOUR(CrashDebugHelper, void*, Unknown_In_ZTemplateEntityBlueprintFactory_ctor, void*, void*);
    
    void SceneLoadCrashHandler();
    void LogInvalidSceneTemps(ZRuntimeResourceID resourceID, ZString ridPath);

    int mostRecentTemplate = -1;
};

DEFINE_ZHM_PLUGIN(CrashDebugHelper)
