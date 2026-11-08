#pragma once

#include <cstdint>

// Too many entity references serialized in one save, overflowing the fixed reference table
class SaveEntityRefDetector {
public:
    ~SaveEntityRefDetector();

    void Install();
    void Remove();

private:
    typedef void (__fastcall* ZSaveSystem_UpdateSaveStates_t)(uintptr_t saveSystem);

    static void __fastcall ZSaveSystem_UpdateSaveStatesDetour(uintptr_t saveSystem);

    static ZSaveSystem_UpdateSaveStates_t originalUpdateSaveStates;
    void* updateSaveStatesTarget = nullptr;
};
