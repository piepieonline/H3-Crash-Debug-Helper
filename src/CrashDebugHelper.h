#pragma once

#include <Glacier/ZScene.h>

#include <IPluginInterface.h>

class CrashDebugHelper : public IPluginInterface {
public:
    ~CrashDebugHelper() override;

    void OnEngineInitialized() override;

private:
    DECLARE_PLUGIN_DETOUR(CrashDebugHelper, bool, OnLoadScene, ZEntitySceneContext*, SSceneInitParameters&);

    void SceneLoadCrashHandler();
    bool LogInvalidSceneTemps(ZRuntimeResourceID resourceID, ZString ridPath, std::vector<std::string>* outputMessageList);

    typedef void* (__fastcall* ZArray_PushBack_t)(void* th, void* newData);

    void InstallArrayPushBackHook();
    void RemoveArrayPushBackHook();
    void OnArrayPushBack(void* th, void* newData);
    static void* __fastcall ZArray_PushBackDetour(void* th, void* newData);

    static CrashDebugHelper* instance;
    static ZArray_PushBack_t originalArrayPushBack;
    void* arrayPushBackTarget = nullptr;

    int mostRecentTemplate = -1;
};

DEFINE_ZHM_PLUGIN(CrashDebugHelper)
