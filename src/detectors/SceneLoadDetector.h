#pragma once

#include <string>
#include <vector>

#include <Hooks.h>
#include <Glacier/ZScene.h>

// Invalid scene or brick is trying to load
class SceneLoadDetector {
public:
    ~SceneLoadDetector();

    void Install();

private:
    DECLARE_DETOUR_WITH_CONTEXT(SceneLoadDetector, bool, OnLoadScene, ZEntitySceneContext*, SSceneInitParameters&);

    void SceneLoadCrashHandler();
    bool LogInvalidSceneTemps(ZRuntimeResourceID resourceID, ZString ridPath, std::vector<std::string>* outputMessageList);
};
