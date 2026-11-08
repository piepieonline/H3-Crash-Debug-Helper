#pragma once

#include <cstdint>

#include <Hooks.h>
#include <Glacier/ZScene.h>

// Too many render primitives in a scene, overflowing the fixed slot table
//
// Every IRenderPrimitive allocates a slot index from the render manager's ZBitAlloc and stores it in
// m_BufferDataIndex. That index addresses Globals::PrimitiveBufferData, a fixed 65536-entry table.
//
// When the allocator is full the constructor leaves the index at its 0xFFFF failure sentinel - which is also a
// *valid* slot index, so consumers cannot tell it apart from a real allocation and end up sharing slot 65535.
class PrimitiveOverflowDetector {
public:
    ~PrimitiveOverflowDetector();

    void Install();
    void Remove();

private:
    DECLARE_DETOUR_WITH_CONTEXT(PrimitiveOverflowDetector, void, ZEntitySceneContext_SetLoadingStage, ZEntitySceneContext*, ESceneLoadingStage);

    // Passing through 4 integer arguments covers any function that takes up to 4
    // integer / pointer arguments, and returning the original's result keeps the
    // return value intact regardless of what the real signature is.
    typedef uintptr_t (__fastcall* PrimitiveCtor_t)(uintptr_t, uintptr_t, uintptr_t, uintptr_t);

    void InstallPrimitiveCtorHook();
    static uintptr_t __fastcall PrimitiveCtorDetour(uintptr_t a1, uintptr_t a2, uintptr_t a3, uintptr_t a4);

    static PrimitiveCtor_t originalPrimitiveCtor;
    void* primitiveCtorTarget = nullptr;
};
