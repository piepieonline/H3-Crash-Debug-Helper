#include "MissingTemplateDetector.h"

#include "../HookUtils.h"

#include <cstdint>

#include <Logging.h>

#include <Glacier/ZEntityManager.h>
#include <Glacier/ZModule.h>
#include <Glacier/ZScene.h>
#include <Glacier/ZResource.h>

// ZArray::push_back - four instantiations share this prologue, so the pattern runs on to the element size that separates them
static const char* c_ArrayPushBackPattern =
    "\x40\x53\x57\x48\x83\xEC\x38\x48\x8B\xD9\x48\x89\x6C\x24\x58\x48\x89\x74\x24\x60\x48\x8B\xEA\x48\x8B\x71\x10\x48\x8B\xCE\x48\xC1"
    "\xE9\x3E\x80\xE1\x01\x74\x10\x48\x8B\xC6\x40\x0F\xB6\xFE\x48\xC1\xE8\x08\x0F\xB6\xD0\xEB\x24\x48\x8B\x7B\x08\x48\x8B\xD6\x48\x2B"
    "\x3B\x48\xB8\xFF\xFF\xFF\xFF\xFF\xFF\xFF\x3F\x48\x23\xD0\x48\xC1\xFF\x03\x48\x2B\x13\x8B\xFF\x48\xC1\xFA\x03\x8B\xC2\x48\x3B\xF8"
    "\x0F\x85\x09\x01\x00\x00\x48\x8B\xC7\x4C\x89\x7C\x24\x30\x48\xC1\xE8\x02\x48\x8D\x57\x01\x48\x03\xD0\x4C\x8D\x3C\xD5\x00\x00\x00"
    "\x00\x49\x83\xFF\x10\x77\x32\x84\xC9\x74\x2E\x48\x8B\x45\x00\x48\x89\x04\xFB\x48\x8D\x47\x01\x48\xC1\xE2\x08\x48\x0B\xD0\x48\x8B"
    "\x43\x10\x48\x25\x00\x00\xFF\xFF\x48\x0B\xD0\x48\x89\x53\x10\x4C\x8B\x7C\x24\x30\xE9\xFD\x00\x00\x00\x48\xB8\x00\x00\x00\x00\x00"
    "\x00\x00\xC0\x4C\x89\x74\x24\x68\x45\x33\xF6\x48\x23\xF0\x66\x44\x89\x74\x24\x50\xE8\x87\x31\xFC\xFF\x4C\x8D\x4C\x24\x50\x4C\x89"
    "\x74\x24\x20\x45\x8D\x46\x08";
static const char* c_ArrayPushBackMask =
    "xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx"
    "xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx????xxxxxxxxxxxxxxxxxxxxxxxxxx"
    "xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx????xxxxxxx"
    "xxxxxxxxxxxxxxxxxxxxx????xxxxxxxxxxxxxx";

MissingTemplateDetector* MissingTemplateDetector::instance = nullptr;
MissingTemplateDetector::ZArray_PushBack_t MissingTemplateDetector::originalArrayPushBack = nullptr;

MissingTemplateDetector::~MissingTemplateDetector()
{
    Remove();
}

void MissingTemplateDetector::Install()
{
    instance = this;

    void* target = FindGameFunction(c_ArrayPushBackPattern, c_ArrayPushBackMask);

    if (target == nullptr)
    {
        Logger::Error("CrashDebugHelper: Could not find ZArray::push_back. Missing entity template detection is disabled - the pattern probably needs updating for this version of the game.");
        return;
    }

    if (InstallMinHook(
        target,
        reinterpret_cast<void*>(&MissingTemplateDetector::ZArray_PushBackDetour),
        reinterpret_cast<void**>(&originalArrayPushBack),
        "ZArray::push_back"
    ))
    {
        arrayPushBackTarget = target;
    }
}

void MissingTemplateDetector::Remove()
{
    RemoveMinHook(arrayPushBackTarget);

    instance = nullptr;
}

void* __fastcall MissingTemplateDetector::ZArray_PushBackDetour(void* th, void* newData)
{
    if (instance != nullptr)
    {
        instance->OnArrayPushBack(th, newData);
    }

    return originalArrayPushBack(th, newData);
}

void MissingTemplateDetector::OnArrayPushBack(void* th, void* newData)
{
    // Calculated in the middle of a function, no previous function call makes it easy to associate the null constructor with the template
    // This isn't clean... but it works

    // The first call is adding the factory for the entity - if it's null, record it
    auto factoryCall1 = reinterpret_cast<ZTemplateEntityBlueprintFactory*>((uintptr_t*)((char*)th - 48));
    if (factoryCall1->IsTemplateEntityBlueprintFactory())
    {
        mostRecentTemplate = *(int*)newData;
    }

    // The second is actually adding the SubEntity, which, if the previous call was null, gives us the entity that has an issue
    auto factoryCall2 = reinterpret_cast<ZTemplateEntityBlueprintFactory*>((uintptr_t*)((char*)th - 216));
    if (factoryCall2->IsTemplateEntityBlueprintFactory())
    {
        if (mostRecentTemplate == 0)
        {
            auto entity = reinterpret_cast<STemplateBlueprintSubEntity*>((uintptr_t*)((char*)newData - 0x28));
            auto rootEntName = factoryCall2->m_pTemplateEntityBlueprint->subEntities[factoryCall2->m_pTemplateEntityBlueprint->rootEntityIndex].entityName;
            auto rootEntId = factoryCall2->m_pTemplateEntityBlueprint->subEntities[factoryCall2->m_pTemplateEntityBlueprint->rootEntityIndex].entityId;
            Logger::Error("CrashDebugHelper: CRASH LIKELY: Missing entity template: {} ({:X}) - Scene Blueprint {:08X}{:08X} (Root Entity {} ({:X}))", entity->entityName, entity->entityId, factoryCall2->m_ridResource.m_IDHigh, factoryCall2->m_ridResource.m_IDLow, rootEntName, rootEntId);
            FlushLoggers();
        }
        mostRecentTemplate = -1;
    }
}
