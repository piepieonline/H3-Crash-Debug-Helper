#include "CrashDebugHelper.h"
#include "HookUtils.h"

#include <IconsMaterialDesign.h>
#include <Logging.h>

void CrashDebugHelper::Init()
{
    // Needs to be in place before the first scene loads, which is well before OnEngineInitialized
    primitiveOverflowDetector.Install();
}

void CrashDebugHelper::OnEngineInitialized() {
    Logger::Info("CrashDebugHelper: Initialized!");

    sceneLoadDetector.Install();
    missingTemplateDetector.Install();
    saveEntityRefDetector.Install();
}

void CrashDebugHelper::OnDrawMenu()
{
    if (ImGui::Button(ICON_MD_BUG_REPORT " CRASH HELPER"))
    {
        showWindow = !showWindow;
    }
}

void CrashDebugHelper::OnDrawUI(bool p_HasFocus)
{
    if (!p_HasFocus || !showWindow)
    {
        return;
    }

    const ImVec2 viewportSize = ImGui::GetMainViewport()->Size;
    ImGui::SetNextWindowSize(ImVec2(viewportSize.x * 0.16f, viewportSize.y * 0.08f), ImGuiCond_FirstUseEver);

    if (ImGui::Begin(ICON_MD_BUG_REPORT " Crash Debug Helper", &showWindow))
    {
        if (ImGui::Button(ICON_MD_SAVE " Flush logs"))
        {
            FlushLoggers();
        }
    }

    ImGui::End();
}

DECLARE_ZHM_PLUGIN(CrashDebugHelper);
