#pragma once

#include <IPluginInterface.h>

class CrashDebugHelper : public IPluginInterface {
public:
    void OnEngineInitialized() override;

private:
    DECLARE_PLUGIN_DETOUR(CrashDebugHelper, void, OnLoadScene, ZEntitySceneContext* th, ZSceneData& p_SceneData);
    DECLARE_PLUGIN_DETOUR(CrashDebugHelper, void*, ZArray_PushBack, void*, void*);
    
    void SceneLoadCrashHandler();
    bool LogInvalidSceneTemps(ZRuntimeResourceID resourceID, ZString ridPath, std::vector<std::string>* outputMessageList);

    int mostRecentTemplate = -1;
};

DEFINE_ZHM_PLUGIN(CrashDebugHelper)
