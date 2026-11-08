#pragma once

// Unknown TEMP is referenced by an entity
class MissingTemplateDetector {
public:
    ~MissingTemplateDetector();

    void Install();
    void Remove();

private:
    typedef void* (__fastcall* ZArray_PushBack_t)(void* th, void* newData);

    void OnArrayPushBack(void* th, void* newData);
    static void* __fastcall ZArray_PushBackDetour(void* th, void* newData);

    static MissingTemplateDetector* instance;
    static ZArray_PushBack_t originalArrayPushBack;
    void* arrayPushBackTarget = nullptr;

    int mostRecentTemplate = -1;
};
