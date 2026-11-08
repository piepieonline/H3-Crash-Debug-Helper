#include "CrashDebugHelper.h"
#include "HookUtils.h"
#include "ResourceNames.h"

#include <string_view>

#include <IconsMaterialDesign.h>
#include <Logging.h>

static const std::string_view c_SaveSettingsSection = "Save";
static const std::string_view c_ExtendedInfoSetting = "ExtendedInfo";

void CrashDebugHelper::Init()
{
    // Needs to be in place before the first scene loads, which is well before OnEngineInitialized
    primitiveOverflowDetector.Install();
}

void CrashDebugHelper::OnEngineInitialized() {
    Logger::Info("CrashDebugHelper: Initialized!");

    ResourceNames::StartLoading();

    sceneLoadDetector.Install();
    missingTemplateDetector.Install();
    extendedSaveInfo = GetSettingBool(c_SaveSettingsSection, c_ExtendedInfoSetting, false);
    saveEntityRefDetector.SetExtendedInfo(extendedSaveInfo);
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
    ImGui::SetNextWindowSize(ImVec2(viewportSize.x * 0.16f, viewportSize.y * 0.12f), ImGuiCond_FirstUseEver);

    if (ImGui::Begin(ICON_MD_BUG_REPORT " Crash Debug Helper", &showWindow))
    {
        if (ImGui::Button(ICON_MD_SAVE " Flush logs"))
        {
            FlushLoggers();
        }

        if (ImGui::Checkbox("Extended save info", &extendedSaveInfo))
        {
            saveEntityRefDetector.SetExtendedInfo(extendedSaveInfo);
            SetSettingBool(c_SaveSettingsSection, c_ExtendedInfoSetting, extendedSaveInfo);
        }

        if (ImGui::IsItemHovered())
        {
            ImGui::SetTooltip("Logs which entities and types fill the save's slots.");
        }
    }

    ImGui::End();
}

DECLARE_ZHM_PLUGIN(CrashDebugHelper);
