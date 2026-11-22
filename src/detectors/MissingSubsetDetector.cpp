#include "MissingSubsetDetector.h"

#include "../HookUtils.h"
#include "../ResourceNames.h"

#include <intrin.h>
#include <string_view>

#include <Logging.h>

#include <Glacier/ZEntityManager.h>
#include <Glacier/ZModule.h>
#include <Glacier/ZScene.h>
#include <Glacier/ZResource.h>

// ZTemplateEntityBlueprintFactory's subset builder, called from its constructor with the blueprint being loaded
static const char* c_BuildSubsetsPattern =
    "\x48\x89\x54\x24\x10\x48\x89\x4C\x24\x08\x55\x53\x56\x57\x41\x54\x41\x55\x41\x56\x41\x57\x48\x8D\x6C\x24\x88\x48\x81\xEC\x78\x01"
    "\x00\x00\x4C\x8B\x42\x18\x48\x8D\x42\x08\x48\x89\x44\x24\x48\x4C\x8B\xF9";
static const char* c_BuildSubsetsMask =
    "xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx";

// The builder's call to the member type's subset-by-name lookup, whose result it dereferences without a null check
static const char* c_FindSubsetCallPattern =
    "\x4C\x89\x65\x18\xE8\x00\x00\x00\x00\x4C\x8B\xC0\x44\x0F\xB6\x48\x10\x48\x8D\x58\x18";
static const char* c_FindSubsetCallMask =
    "xxxxx????xxxxxxxxxxxx";
static constexpr ptrdiff_t c_FindSubsetCallOffset = 4;
static constexpr ptrdiff_t c_FindSubsetCallLength = 5;

// Per-sub-entity types the builder creates; the lookup is called on one of these
static constexpr ptrdiff_t c_SubEntityTypesOffset = 0x158;

MissingSubsetDetector* MissingSubsetDetector::instance = nullptr;
MissingSubsetDetector::BuildSubsets_t MissingSubsetDetector::originalBuildSubsets = nullptr;
MissingSubsetDetector::FindSubset_t MissingSubsetDetector::originalFindSubset = nullptr;

static thread_local ZTemplateEntityBlueprintFactory* buildingFactory = nullptr;
static thread_local STemplateEntityBlueprint* buildingBlueprint = nullptr;

MissingSubsetDetector::~MissingSubsetDetector()
{
    Remove();
}

void MissingSubsetDetector::Install()
{
    instance = this;

    auto* buildSubsets = FindGameFunction(c_BuildSubsetsPattern, c_BuildSubsetsMask);
    auto* findSubsetCall = static_cast<uint8_t*>(FindGameFunction(c_FindSubsetCallPattern, c_FindSubsetCallMask));

    if (buildSubsets == nullptr || findSubsetCall == nullptr)
    {
        Logger::Error("CrashDebugHelper: Could not find the entity subset builder. Missing subset detection is disabled - the pattern probably needs updating for this version of the game.");
        return;
    }

    uint8_t* callInstruction = findSubsetCall + c_FindSubsetCallOffset;
    subsetBuildCallReturn = reinterpret_cast<uintptr_t>(callInstruction + c_FindSubsetCallLength);
    void* findSubset = callInstruction + c_FindSubsetCallLength + *reinterpret_cast<int32_t*>(callInstruction + 1);

    if (InstallMinHook(findSubset, reinterpret_cast<void*>(&FindSubsetDetour), reinterpret_cast<void**>(&originalFindSubset), "entity subset lookup"))
    {
        findSubsetTarget = findSubset;
    }

    if (InstallMinHook(buildSubsets, reinterpret_cast<void*>(&BuildSubsetsDetour), reinterpret_cast<void**>(&originalBuildSubsets), "entity subset builder"))
    {
        buildSubsetsTarget = buildSubsets;
    }
}

void MissingSubsetDetector::Remove()
{
    RemoveMinHook(buildSubsetsTarget);
    RemoveMinHook(findSubsetTarget);

    instance = nullptr;
}

void __fastcall MissingSubsetDetector::BuildSubsetsDetour(ZTemplateEntityBlueprintFactory* factory, STemplateEntityBlueprint* blueprint)
{
    auto* outerFactory = buildingFactory;
    auto* outerBlueprint = buildingBlueprint;
    buildingFactory = factory;
    buildingBlueprint = blueprint;

    originalBuildSubsets(factory, blueprint);

    buildingFactory = outerFactory;
    buildingBlueprint = outerBlueprint;
}

void* __fastcall MissingSubsetDetector::FindSubsetDetour(void* memberType, ZString* subsetName)
{
    void* subset = originalFindSubset(memberType, subsetName);

    if (subset == nullptr && instance != nullptr && reinterpret_cast<uintptr_t>(_ReturnAddress()) == instance->subsetBuildCallReturn)
    {
        instance->ReportMissingSubset(memberType, *subsetName);
    }

    return subset;
}

void MissingSubsetDetector::ReportMissingSubset(void* memberType, const ZString& subsetName) const
{
    if (buildingFactory == nullptr || buildingBlueprint == nullptr)
    {
        Logger::Error("CrashDebugHelper: CRASH LIKELY: An entity is in subset '{}' but its template does not declare it (scene blueprint unknown)", subsetName);
        FlushLoggers();
        return;
    }

    const auto& subEntities = buildingBlueprint->subEntities;
    const auto& rootEntity = subEntities[buildingBlueprint->rootEntityIndex];
    const auto& memberTypes = *reinterpret_cast<TArray<void*>*>(reinterpret_cast<uint8_t*>(buildingFactory) + c_SubEntityTypesOffset);
    const std::string_view missingName(subsetName.c_str(), subsetName.size());

    int memberIndex = -1;
    for (size_t i = 0; i < memberTypes.size(); ++i)
    {
        if (memberTypes[i] == memberType)
        {
            memberIndex = static_cast<int>(i);
            break;
        }
    }

    Logger::Error(
        "CrashDebugHelper: CRASH LIKELY: An entity is in subset '{}' but its template does not declare it - Scene Blueprint {} (Root Entity {} ({:X}))",
        subsetName, ResourceNames::Name(buildingFactory->m_ridResource.GetID()), rootEntity.entityName, rootEntity.entityId
    );

    if (memberIndex < 0 || static_cast<size_t>(memberIndex) >= subEntities.size())
    {
        FlushLoggers();
        return;
    }

    const auto& member = subEntities[memberIndex];
    const auto& memberFactories = buildingFactory->m_aBlueprintFactories;
    const auto* memberFactory = static_cast<size_t>(memberIndex) < memberFactories.size()
        ? static_cast<ZEntityBlueprintFactoryBase*>(memberFactories[memberIndex])
        : nullptr;

    Logger::Error(
        "CrashDebugHelper:     Member: {} ({:X}) - Template {}",
        member.entityName, member.entityId, memberFactory ? ResourceNames::Name(memberFactory->m_ridResource.GetID()) : std::string("unknown")
    );

    for (const auto& owner : subEntities)
    {
        for (const auto& [name, subset] : owner.entitySubsets)
        {
            if (std::string_view(name.c_str(), name.size()) != missingName)
            {
                continue;
            }

            for (const int32 index : subset.entities)
            {
                if (index == memberIndex)
                {
                    Logger::Error("CrashDebugHelper:     Subset owner: {} ({:X})", owner.entityName, owner.entityId);
                }
            }
        }
    }

    FlushLoggers();
}
