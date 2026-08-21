#pragma once

#include <Common.h>
#include <Glacier/ZPrimitives.h>

/**
 * Unique-id allocator used by the render manager.
 *
 * The render manager owns one of these (m_PrimIds in the symbolled console
 * builds) and hands out the index each IRenderPrimitive stores in
 * m_BufferDataIndex. That index addresses Globals::PrimitiveBufferData.
 *
 * Only the two counters have been verified against the retail Windows build;
 * everything before them is padded rather than guessed. The allocation site
 * compares them directly as `m_nUsed < m_nCapacity`.
 *
 * Inlined here rather than pulled from the SDK: the stock ZHMModSDK has no
 * ZBitAlloc header, so shipping our own copy keeps this mod buildable against
 * an unmodified SDK.
 */
class ZBitAlloc {
public:
    PAD(0x08); // 0x00, bit storage
    uint32 m_nUsed; // 0x08
    uint32 m_nCapacity; // 0x0C
};

static_assert(offsetof(ZBitAlloc, m_nUsed) == 0x08);
static_assert(offsetof(ZBitAlloc, m_nCapacity) == 0x0C);
