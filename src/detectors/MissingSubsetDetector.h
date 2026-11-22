#pragma once

#include <cstdint>

class ZString;
class ZTemplateEntityBlueprintFactory;
struct STemplateEntityBlueprint;

// An entity is placed in a subset (e.g. AudioEmitters) that its own template does not declare
class MissingSubsetDetector {
public:
    ~MissingSubsetDetector();

    void Install();
    void Remove();

private:
    typedef void(__fastcall* BuildSubsets_t)(ZTemplateEntityBlueprintFactory* factory, STemplateEntityBlueprint* blueprint);
    typedef void*(__fastcall* FindSubset_t)(void* memberType, ZString* subsetName);

    static void __fastcall BuildSubsetsDetour(ZTemplateEntityBlueprintFactory* factory, STemplateEntityBlueprint* blueprint);
    static void* __fastcall FindSubsetDetour(void* memberType, ZString* subsetName);

    void ReportMissingSubset(void* memberType, const ZString& subsetName) const;

    static MissingSubsetDetector* instance;
    static BuildSubsets_t originalBuildSubsets;
    static FindSubset_t originalFindSubset;

    void* buildSubsetsTarget = nullptr;
    void* findSubsetTarget = nullptr;
    uintptr_t subsetBuildCallReturn = 0;
};
