#include "PrimitiveOverflowDetector.h"
#include "ZBitAlloc.h"

#include "../HookUtils.h"

#include <atomic>

#include <Globals.h>
#include <Logging.h>

// The render manager's unique-id allocator (m_PrimIds in the symbolled console builds), matched on the allocation
// site inside IRenderPrimitive's constructor. The stock SDK has no Globals::PrimitiveBufferIds, so resolve it here.
static const char* c_PrimitiveBufferIdsPattern = "\x48\x8D\x0D\x00\x00\x00\x00\xE8\x00\x00\x00\x00\x8B\x5C\x24\x60";
static const char* c_PrimitiveBufferIdsMask = "xxx????x????xxxx";
static const ptrdiff_t c_PrimitiveBufferIdsOffset = 3;

// IRenderPrimitive::IRenderPrimitive, matched on its prologue with the rel32 to its vtable wildcarded
static const char* c_PrimitiveCtorPattern =
    "\x48\x89\x5C\x24\x10\x57\x48\x83\xEC\x50\x48\x8B\xF9\xC7\x41\x08\x01\x00\x00\x00\x48\x8D\x05\x7D\xF6\xD6\x00\x0F\x29\x74\x24\x40";
static const char* c_PrimitiveCtorMask = "xxxxxxxxxxxxxxxxxxxxxxx????xxxxx";

static ZBitAlloc* g_PrimitiveBufferIds = nullptr;

// How many times the constructor has run while the allocator was already at (or over) capacity.
static std::atomic<uint32_t> g_PrimitiveExcessCount = 0;

PrimitiveOverflowDetector::PrimitiveCtor_t PrimitiveOverflowDetector::originalPrimitiveCtor = nullptr;

PrimitiveOverflowDetector::~PrimitiveOverflowDetector()
{
    Remove();
}

void PrimitiveOverflowDetector::Install()
{
    Hooks::ZEntitySceneContext_SetLoadingStage->AddDetour(this, &PrimitiveOverflowDetector::ZEntitySceneContext_SetLoadingStage);
    InstallPrimitiveCtorHook();
}

void PrimitiveOverflowDetector::Remove()
{
    Hooks::ZEntitySceneContext_SetLoadingStage->RemoveDetoursWithContext(this);
    RemoveMinHook(primitiveCtorTarget);
}

void PrimitiveOverflowDetector::InstallPrimitiveCtorHook()
{
    g_PrimitiveBufferIds = static_cast<ZBitAlloc*>(
        FindGameGlobal(c_PrimitiveBufferIdsPattern, c_PrimitiveBufferIdsMask, c_PrimitiveBufferIdsOffset)
    );

    if (g_PrimitiveBufferIds == nullptr)
    {
        Logger::Error("CrashDebugHelper: Could not find the render primitive id allocator. Primitive overflow detection is disabled - the pattern probably needs updating for this version of the game.");
        return;
    }

    Logger::Debug("CrashDebugHelper: Render primitive id allocator at {}, {}/{} slots used.", fmt::ptr(g_PrimitiveBufferIds), g_PrimitiveBufferIds->m_nUsed, g_PrimitiveBufferIds->m_nCapacity);

    if (Globals::PrimitiveBufferData != nullptr)
    {
        Logger::Debug("CrashDebugHelper: Render primitive buffer table at {}.", fmt::ptr(Globals::PrimitiveBufferData));
    }

    void* target = FindGameFunction(c_PrimitiveCtorPattern, c_PrimitiveCtorMask);

    if (target == nullptr)
    {
        Logger::Error("CrashDebugHelper: Could not find the IRenderPrimitive constructor. Primitive overflow detection is disabled - the pattern probably needs updating for this version of the game.");
        return;
    }

    if (InstallMinHook(
        target,
        reinterpret_cast<void*>(&PrimitiveOverflowDetector::PrimitiveCtorDetour),
        reinterpret_cast<void**>(&originalPrimitiveCtor),
        "IRenderPrimitive constructor"
    ))
    {
        primitiveCtorTarget = target;
    }
}

uintptr_t __fastcall PrimitiveOverflowDetector::PrimitiveCtorDetour(uintptr_t a1, uintptr_t a2, uintptr_t a3, uintptr_t a4)
{
    if (g_PrimitiveBufferIds != nullptr)
    {
        const auto used = g_PrimitiveBufferIds->m_nUsed;
        const auto capacity = g_PrimitiveBufferIds->m_nCapacity;

        if (used >= capacity)
        {
            const uint32_t excess = g_PrimitiveExcessCount.fetch_add(1) + 1;

            Logger::Error("CrashDebugHelper: CRASH LIKELY: Excess render primitive ({} + {})/{} - slot 0xFFFF is being shared", used, excess, capacity);

            // Only worth paying for on the first one - after that the scene is already doomed and this can fire
            // thousands of times
            if (excess == 1)
            {
                FlushLoggers();
            }
        }
    }

    return originalPrimitiveCtor(a1, a2, a3, a4);
}

DEFINE_DETOUR_WITH_CONTEXT(PrimitiveOverflowDetector, void, ZEntitySceneContext_SetLoadingStage, ZEntitySceneContext* th, ESceneLoadingStage stage) {
    if (stage == ESceneLoadingStage::eLoading_Start)
    {
        // Start counting overflows fresh for the scene that's about to load
        g_PrimitiveExcessCount = 0;
    }

    if (stage == ESceneLoadingStage::eLoading_ScenePlaying && g_PrimitiveBufferIds != nullptr)
    {
        Logger::Info("CrashDebugHelper: Scene finished loading. Render primitive slots: {}/{} filled, {} overflow(s).", g_PrimitiveBufferIds->m_nUsed, g_PrimitiveBufferIds->m_nCapacity, g_PrimitiveExcessCount.load());
    }

    return HookResult<void>(HookAction::Continue());
}
