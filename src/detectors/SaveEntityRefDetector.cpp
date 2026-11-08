#include "SaveEntityRefDetector.h"

#include "../HookUtils.h"

#include <algorithm>
#include <atomic>
#include <cstddef>
#include <map>
#include <string>
#include <unordered_map>
#include <vector>

#include <Globals.h>
#include <Logging.h>

// ZEntity.h declares IEntityFactory, which EntityFactory.h uses without declaring
#include <Glacier/ZEntity.h>
#include <Glacier/EntityFactory.h>

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

static std::string DescribeSaveEntityRef(uintptr_t refValue)
{
    const ZEntityRef ref(reinterpret_cast<ZEntityType**>(refValue));
    const ZEntityImpl* entity = ref.GetEntity();
    const ZEntityType* type = entity == nullptr ? nullptr : entity->GetType();

    if (type == nullptr)
    {
        return fmt::format("{} ({})", SaveEntityRefTypeName(refValue), fmt::ptr(reinterpret_cast<void*>(refValue)));
    }

    const ZEntityRef owner = ref.GetBlueprintFactory() != nullptr ? ref : ref.GetClosestParentWithBlueprintFactory();
    ZEntityBlueprintFactoryBase* factory = owner ? owner.GetBlueprintFactory() : nullptr;

    std::string name = "?";
    std::string blueprint = "?";

    if (factory != nullptr)
    {
        blueprint = fmt::format("{:08X}{:08X}", factory->m_ridResource.m_IDHigh, factory->m_ridResource.m_IDLow);

        if (*reinterpret_cast<void**>(factory) == Globals::ZTemplateEntityBlueprintFactory_vtbl)
        {
            const auto* templateBlueprint = static_cast<ZTemplateEntityBlueprintFactory*>(factory)->m_pTemplateEntityBlueprint;

            if (templateBlueprint != nullptr)
            {
                for (const auto& subEntity : templateBlueprint->subEntities)
                {
                    if (subEntity.entityId == type->m_nEntityID)
                    {
                        name = subEntity.entityName.c_str();
                        break;
                    }
                }
            }
        }
    }

    return fmt::format("{} [{}] id {:016X} in {}", name, SaveEntityRefTypeName(refValue), type->m_nEntityID, blueprint);
}

static void LogSaveEntityRefDuplication(uintptr_t entityRefTable, uint32_t used)
{
    const auto* entries = reinterpret_cast<const uintptr_t*>(entityRefTable);
    const uint32_t count = std::min(used, c_EntityRefTableCapacity);

    std::unordered_map<uintptr_t, uint32_t> refsPerEntity;
    refsPerEntity.reserve(count);

    for (uint32_t i = 0; i < count; ++i)
    {
        ++refsPerEntity[entries[i]];
    }

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

    std::vector<std::pair<uintptr_t, uint32_t>> worstEntities(refsPerEntity.begin(), refsPerEntity.end());
    const size_t worstCount = std::min<size_t>(worstEntities.size(), 25);

    std::partial_sort(worstEntities.begin(), worstEntities.begin() + worstCount, worstEntities.end(),
        [](const auto& a, const auto& b) { return a.second > b.second; });

    Logger::Info("CrashDebugHelper: SAVE REF DUPLICATION: worst {} entities", worstCount);

    for (size_t i = 0; i < worstCount; ++i)
    {
        Logger::Info("CrashDebugHelper:   {:>6} refs: {}", worstEntities[i].second, DescribeSaveEntityRef(worstEntities[i].first));
    }

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
