#include "SaveEntityRefDetector.h"

#include "../HookUtils.h"
#include "../ResourceNames.h"

#include <algorithm>
#include <atomic>
#include <cctype>
#include <cstddef>
#include <map>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include <excpt.h>

#include <Globals.h>
#include <Logging.h>

// ZEntity.h declares IEntityFactory, which EntityFactory.h uses without declaring
#include <Glacier/ZEntity.h>
#include <Glacier/EntityFactory.h>
#include <Glacier/ZModule.h>
#include <Glacier/ZScene.h>

// Save handlers append every serialized entity ref (not deduplicated, unchecked) to a fixed table; overflow corrupts its count
static const char* c_UpdateSaveStatesPattern =
    "\x40\x55\x53\x56\x57\x41\x54\x41\x56\x41\x57\x48\x8D\x6C\x24\xF0\x48\x81\xEC\x10\x01\x00\x00\x8B\x81\x80\x01\x00\x00\x48\x8B\xF9\x83\xF8\x0A\x0F\x85\x00\x00\x00\x00\x33\xF6";
static const char* c_UpdateSaveStatesMask = "xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx????xx";

static const ptrdiff_t c_SaveSystemStateOffset = 0x180;
static const ptrdiff_t c_SaveSystemJobOffset = 0x148;
static const ptrdiff_t c_SaveJobEntityRefTableOffset = 0x160;
static const ptrdiff_t c_EntityRefTableCountOffset = 3200000;
static const uint32_t c_EntityRefTableCapacity = 400000;
static const uint32_t c_EntityRefTableWarnPercent = 80;

static const int c_SaveStateSerialize = 10;

static const int c_MaxSubEntityDepth = 256;

static std::atomic<bool> g_ExtendedSaveInfo = false;

SaveEntityRefDetector::ZSaveSystem_UpdateSaveStates_t SaveEntityRefDetector::originalUpdateSaveStates = nullptr;

SaveEntityRefDetector::~SaveEntityRefDetector()
{
    Remove();
}

void SaveEntityRefDetector::Install()
{
    void* target = FindGameFunction(c_UpdateSaveStatesPattern, c_UpdateSaveStatesMask);

    if (target == nullptr)
    {
        Logger::Error("CrashDebugHelper: Could not find ZSaveSystem's save state update. Save entity reference reporting is disabled - the pattern probably needs updating for this version of the game.");
        return;
    }

    if (InstallMinHook(
        target,
        reinterpret_cast<void*>(&SaveEntityRefDetector::ZSaveSystem_UpdateSaveStatesDetour),
        reinterpret_cast<void**>(&originalUpdateSaveStates),
        "ZSaveSystem save state update"
    ))
    {
        updateSaveStatesTarget = target;
    }
}

void SaveEntityRefDetector::Remove()
{
    RemoveMinHook(updateSaveStatesTarget);
}

void SaveEntityRefDetector::SetExtendedInfo(bool enabled)
{
    g_ExtendedSaveInfo = enabled;
}

// The reference table is not deduplicated, so the same entity can occupy a great many of its slots
static std::string_view SaveEntityRefTypeName(uintptr_t refValue)
{
    const ZEntityRef ref(reinterpret_cast<ZEntityType**>(refValue));
    const ZEntityImpl* entity = ref.GetEntity();

    if (entity == nullptr)
    {
        return "<null entity>";
    }

    const ZEntityType* type = entity->GetType();

    if (type == nullptr || type->m_pInterfaceData == nullptr || type->m_pInterfaceData->size() == 0)
    {
        return "<no interfaces>";
    }

    const STypeID* typeId = (*type->m_pInterfaceData)[0].m_Type;
    const IType* typeInfo = typeId == nullptr ? nullptr : typeId->GetTypeInfo();

    if (typeInfo == nullptr || typeInfo->pszTypeName == nullptr)
    {
        return "<no type name>";
    }

    return typeInfo->pszTypeName;
}

static std::unordered_map<uintptr_t, uint32_t> CountRefsPerEntity(uintptr_t entityRefTable, uint32_t count)
{
    const auto* entries = reinterpret_cast<const uintptr_t*>(entityRefTable);

    std::unordered_map<uintptr_t, uint32_t> refsPerEntity;
    refsPerEntity.reserve(count);

    for (uint32_t i = 0; i < count; ++i)
    {
        ++refsPerEntity[entries[i]];
    }

    return refsPerEntity;
}

static ZRuntimeResourceID EntityTypeResourceId(const ZString& assemblyPath)
{
    std::string path = fmt::format("[{}].pc_entitytype", std::string_view(assemblyPath.c_str(), assemblyPath.size()));
    std::transform(path.begin(), path.end(), path.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return ZRuntimeResourceID::FromString(path);
}

// Where an entity sits in a loaded brick's sub-entity tree; top is the brick-level sub-entity an override delete would target
struct EntityPlacement {
    uint64_t brickRid;
    uint64_t topEntityId;
    uint64_t topBlueprintRid;
};

using EntityPlacements = std::unordered_map<uintptr_t, EntityPlacement>;

static uint64_t BlueprintRid(const IEntityBlueprintFactory* factory)
{
    return factory == nullptr ? 0 : static_cast<const ZEntityBlueprintFactoryBase*>(factory)->m_ridResource.GetID();
}

__declspec(noinline) static void PlaceSubEntities(EntityPlacements& placements, ZEntityType** root, IEntityBlueprintFactory* factory,
    uint64_t brickRid, uint64_t topEntityId, uint64_t topBlueprintRid, int depth)
{
    const int64_t subEntityCount = factory->GetSubEntitiesCount();

    for (int i = 0; i < subEntityCount; ++i)
    {
        ZEntityType** subEntity = factory->GetSubEntity(root, i);

        if (subEntity == nullptr)
        {
            continue;
        }

        ZEntityBlueprintFactoryBase* subFactory = factory->GetSubEntityBlueprint(i);
        const uint64_t top = depth == 0 ? factory->GetSubEntityId(i) : topEntityId;
        const uint64_t topBlueprint = depth == 0 ? BlueprintRid(subFactory) : topBlueprintRid;

        placements.try_emplace(reinterpret_cast<uintptr_t>(subEntity), EntityPlacement { brickRid, top, topBlueprint });

        if (subFactory != nullptr && depth < c_MaxSubEntityDepth && subFactory->GetSubEntitiesCount() > 0)
        {
            PlaceSubEntities(placements, subEntity, subFactory, brickRid, top, topBlueprint, depth + 1);
        }
    }
}

static bool TryPlaceBrickEntities(EntityPlacements& placements, const ZEntityRef& brickRoot, uint64_t brickRid)
{
    __try
    {
        ZEntityBlueprintFactoryBase* factory = brickRoot.GetBlueprintFactory();

        if (factory != nullptr)
        {
            PlaceSubEntities(placements, brickRoot.m_pObj, factory, brickRid, 0, 0, 0);
        }

        return true;
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        return false;
    }
}

static void PlaceLoadedBrickEntities(EntityPlacements& placements)
{
    const ZEntitySceneContext* sceneContext = Globals::Hitman5Module == nullptr ? nullptr : Globals::Hitman5Module->m_pEntitySceneContext;

    if (sceneContext == nullptr)
    {
        return;
    }

    uint32_t unplaceableBricks = 0;

    for (const SBrickAllocationInfo& brick : sceneContext->m_aLoadedBricks)
    {
        if (brick.m_EntityRef && !TryPlaceBrickEntities(placements, brick.m_EntityRef, brick.m_RuntimeResourceID.GetID()))
        {
            ++unplaceableBricks;
        }
    }

    if (sceneContext->m_pScene.m_entityRef
        && !TryPlaceBrickEntities(placements, sceneContext->m_pScene.m_entityRef, EntityTypeResourceId(sceneContext->m_SceneInitParameters.m_SceneResource).GetID()))
    {
        ++unplaceableBricks;
    }

    if (unplaceableBricks > 0)
    {
        Logger::Warn("CrashDebugHelper: {} bricks could not be walked, so their entities count as spawned at runtime.", unplaceableBricks);
    }
}

// Owners are the brick-level entities (setpieces, props, NPCs) whose sub-entity trees the saved entities belong to
static void LogSaveRefOwners(const std::unordered_map<uintptr_t, uint32_t>& refsPerEntity, uint32_t totalRefs)
{
    EntityPlacements placements;
    PlaceLoadedBrickEntities(placements);

    struct OwnerTemplateCost {
        uint64_t refs = 0;
        std::unordered_set<uint64_t> owners;
    };

    std::unordered_map<uint64_t, OwnerTemplateCost> perTemplate;
    uint64_t spawnedRefs = 0;

    for (const auto& [refValue, refCount] : refsPerEntity)
    {
        const auto placement = placements.find(refValue);

        if (placement == placements.end())
        {
            spawnedRefs += refCount;
            continue;
        }

        const EntityPlacement& place = placement->second;
        auto& cost = perTemplate[place.topBlueprintRid];
        cost.refs += refCount;
        cost.owners.insert(place.topEntityId ^ (place.brickRid * 0x9E3779B97F4A7C15ull));
    }

    std::vector<std::pair<uint64_t, const OwnerTemplateCost*>> worstTemplates;
    worstTemplates.reserve(perTemplate.size());

    for (const auto& [blueprintRid, cost] : perTemplate)
    {
        worstTemplates.emplace_back(blueprintRid, &cost);
    }

    const size_t worstCount = std::min<size_t>(worstTemplates.size(), 25);

    std::partial_sort(worstTemplates.begin(), worstTemplates.begin() + worstCount, worstTemplates.end(),
        [](const auto& a, const auto& b) { return a.second->refs > b.second->refs; });

    Logger::Info("CrashDebugHelper: SAVE REF OWNERS: worst {} of {} owner templates ({} refs from entities spawned at runtime)", worstCount, perTemplate.size(), spawnedRefs);

    for (size_t i = 0; i < worstCount; ++i)
    {
        const OwnerTemplateCost& cost = *worstTemplates[i].second;
        const size_t owners = cost.owners.size();

        Logger::Info("CrashDebugHelper:   {:>7} refs ({:>4.1f}%) {:>6} each x{:<5} {}", cost.refs, cost.refs * 100.0 / totalRefs,
            cost.refs / owners, owners, ResourceNames::Name(worstTemplates[i].first));
    }
}

static void LogSaveEntityRefDuplication(uintptr_t entityRefTable, uint32_t used)
{
    const uint32_t count = std::min(used, c_EntityRefTableCapacity);
    const auto refsPerEntity = CountRefsPerEntity(entityRefTable, count);

    if (refsPerEntity.empty())
    {
        return;
    }

    Logger::Info("CrashDebugHelper: SAVE REF DUPLICATION: {} refs over {} distinct entities ({:.2f} refs per entity, {} slots wasted on repeats)",
        count, refsPerEntity.size(), static_cast<double>(count) / refsPerEntity.size(), count - refsPerEntity.size());

    std::map<uint32_t, uint32_t> entitiesPerRefCount;

    for (const auto& [refValue, refCount] : refsPerEntity)
    {
        ++entitiesPerRefCount[refCount];
    }

    for (const auto& [refCount, entities] : entitiesPerRefCount)
    {
        const uint64_t slots = static_cast<uint64_t>(refCount) * entities;

        Logger::Info("CrashDebugHelper:   {:>6} ref(s) each: {:>6} entities, {:>7} slots ({}%)", refCount, entities, slots, slots * 100 / count);
    }

    LogSaveRefOwners(refsPerEntity, count);

    std::unordered_map<std::string_view, std::pair<uint32_t, uint32_t>> perType;

    for (const auto& [refValue, refCount] : refsPerEntity)
    {
        auto& [entities, refs] = perType[SaveEntityRefTypeName(refValue)];
        ++entities;
        refs += refCount;
    }

    std::vector<std::pair<std::string_view, std::pair<uint32_t, uint32_t>>> worstTypes(perType.begin(), perType.end());
    const size_t worstTypeCount = std::min<size_t>(worstTypes.size(), 25);

    std::partial_sort(worstTypes.begin(), worstTypes.begin() + worstTypeCount, worstTypes.end(),
        [](const auto& a, const auto& b) { return a.second.second > b.second.second; });

    Logger::Info("CrashDebugHelper: SAVE REF DUPLICATION: worst {} of {} interface types", worstTypeCount, perType.size());

    for (size_t i = 0; i < worstTypeCount; ++i)
    {
        const auto& [entities, refs] = worstTypes[i].second;

        Logger::Info("CrashDebugHelper:   {:>7} refs over {:>6} entities ({:.2f} each): {}", refs, entities, static_cast<double>(refs) / entities, worstTypes[i].first);
    }
}

void __fastcall SaveEntityRefDetector::ZSaveSystem_UpdateSaveStatesDetour(uintptr_t saveSystem)
{
    const int stateBefore = *reinterpret_cast<int*>(saveSystem + c_SaveSystemStateOffset);

    originalUpdateSaveStates(saveSystem);

    if (stateBefore != c_SaveStateSerialize)
    {
        return;
    }

    const uintptr_t saveJob = *reinterpret_cast<uintptr_t*>(saveSystem + c_SaveSystemJobOffset);
    const uintptr_t entityRefTable = saveJob == 0 ? 0 : *reinterpret_cast<uintptr_t*>(saveJob + c_SaveJobEntityRefTableOffset);

    if (entityRefTable == 0)
    {
        return;
    }

    const uint32_t used = *reinterpret_cast<uint32_t*>(entityRefTable + c_EntityRefTableCountOffset);
    const uint32_t percent = static_cast<uint32_t>(static_cast<uint64_t>(used) * 100 / c_EntityRefTableCapacity);

    if (used >= c_EntityRefTableWarnPercent * (c_EntityRefTableCapacity / 100))
    {
        Logger::Warn("CrashDebugHelper: SAVE CRASH LIKELY SOON: Save used {}/{} entity reference slots ({}%). Saving past {} crashes the game.", used, c_EntityRefTableCapacity, percent, c_EntityRefTableCapacity);
        FlushLoggers();
    }
    else
    {
        Logger::Info("CrashDebugHelper: Save used {}/{} entity reference slots ({}%).", used, c_EntityRefTableCapacity, percent);
    }

    if (g_ExtendedSaveInfo)
    {
        LogSaveEntityRefDuplication(entityRefTable, used);
        FlushLoggers();
    }
}
