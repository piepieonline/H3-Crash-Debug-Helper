#pragma once

#include <cstdint>
#include <string>

// Template names from the community hash list, which the game itself does not keep at runtime
namespace ResourceNames {
    void StartLoading();

    // Short name for a TEMP or TBLU runtime resource id, or the id in hex while the list is unavailable
    std::string Name(uint64_t runtimeResourceId);
}
