#pragma once

#include <IPluginInterface.h>

#include "detectors/MissingTemplateDetector.h"
#include "detectors/PrimitiveOverflowDetector.h"
#include "detectors/SaveEntityRefDetector.h"
#include "detectors/SceneLoadDetector.h"

class CrashDebugHelper : public IPluginInterface {
public:
    void Init() override;
    void OnEngineInitialized() override;
    void OnDrawMenu() override;
    void OnDrawUI(bool p_HasFocus) override;

private:
    SceneLoadDetector sceneLoadDetector;
    MissingTemplateDetector missingTemplateDetector;
    PrimitiveOverflowDetector primitiveOverflowDetector;
    SaveEntityRefDetector saveEntityRefDetector;

    bool showWindow = false;
    bool extendedSaveInfo = false;
};

DEFINE_ZHM_PLUGIN(CrashDebugHelper)
