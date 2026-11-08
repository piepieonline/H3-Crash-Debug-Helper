#pragma once

#include <cstddef>

void FlushLoggers();

// SDK's PatternHook clone
void* FindGameFunction(const char* pattern, const char* mask);

// SDK's PatternGlobalRelative clone - finds an instruction that references a global via a rel32 displacement,
// then resolves that displacement to the global's address
void* FindGameGlobal(const char* pattern, const char* mask, ptrdiff_t offset);

bool InstallMinHook(void* target, void* detour, void** original, const char* hookName);
void RemoveMinHook(void*& target);
