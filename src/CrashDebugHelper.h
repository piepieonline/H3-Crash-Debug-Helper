#pragma once

#include <Glacier/ZScene.h>

#include <IPluginInterface.h>

class CrashDebugHelper : public IPluginInterface {
public:
    ~CrashDebugHelper() override;

    void Init() override;
    void OnEngineInitialized() override;

private:
    DECLARE_PLUGIN_DETOUR(CrashDebugHelper, bool, OnLoadScene, ZEntitySceneContext*, SSceneInitParameters&);
    DECLARE_PLUGIN_DETOUR(CrashDebugHelper, void, ZEntitySceneContext_SetLoadingStage, ZEntitySceneContext*, ESceneLoadingStage);

    void SceneLoadCrashHandler();
    bool LogInvalidSceneTemps(ZRuntimeResourceID resourceID, ZString ridPath, std::vector<std::string>* outputMessageList);

    typedef void* (__fastcall* ZArray_PushBack_t)(void* th, void* newData);

    void InstallArrayPushBackHook();
    void RemoveArrayPushBackHook();
    void OnArrayPushBack(void* th, void* newData);
    static void* __fastcall ZArray_PushBackDetour(void* th, void* newData);

    // Passing through 4 integer arguments covers any function that takes up to 4
    // integer / pointer arguments, and returning the original's result keeps the
    // return value intact regardless of what the real signature is.
    typedef uintptr_t (__fastcall* PrimitiveCtor_t)(uintptr_t, uintptr_t, uintptr_t, uintptr_t);

    void InstallPrimitiveOverflowHook();
    void RemovePrimitiveOverflowHook();
    static uintptr_t __fastcall PrimitiveCtorDetour(uintptr_t a1, uintptr_t a2, uintptr_t a3, uintptr_t a4);

    static CrashDebugHelper* instance;
    static ZArray_PushBack_t originalArrayPushBack;
    static PrimitiveCtor_t originalPrimitiveCtor;
    void* arrayPushBackTarget = nullptr;
    void* primitiveCtorTarget = nullptr;

    int mostRecentTemplate = -1;
};

DEFINE_ZHM_PLUGIN(CrashDebugHelper)
