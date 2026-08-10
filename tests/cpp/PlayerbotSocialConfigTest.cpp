#include <cstdlib>

#include "Bot/Social/PlayerbotSocialConfig.h"
#include "Config.h"

int main()
{
    sConfigMgr->SetOption<bool>("PlayerbotsSocial.Enable", true);
    sConfigMgr->SetOption<std::string>("PlayerbotsSocial.Stage", "grounded_presence");
    sConfigMgr->SetOption<std::string>("PlayerbotsSocial.Density", "lively");
    sConfigMgr->SetOption<float>("PlayerbotsSocial.DensityMultiplier.Quiet", 0.75f);
    sConfigMgr->SetOption<float>("PlayerbotsSocial.DensityMultiplier.Normal", 1.25f);
    sConfigMgr->SetOption<float>("PlayerbotsSocial.DensityMultiplier.Lively", 1.9f);
    sConfigMgr->SetOption<float>("PlayerbotsSocial.GeneralStarterPressureMultiplier", 0.8f);
    sConfigMgr->SetOption<uint32>("PlayerbotsSocial.TelemetryRetentionHours", 72);
    sConfigMgr->SetOption<std::string>("PlayerbotsSocial.ControlToken", "test-control-token");

    ReloadPlayerbotSocialConfig();

    PlayerbotSocialConfigValues const& loaded = sPlayerbotSocialConfig;
    if (!loaded.socialChatEnable || loaded.socialChatStage != "grounded_presence" ||
        loaded.socialChatDensity != "lively" ||
        loaded.socialChatDensityMultiplierQuiet != 0.75f || loaded.socialChatDensityMultiplierNormal != 1.25f ||
        loaded.socialChatDensityMultiplierLively != 1.9f || loaded.socialChatGeneralStarterPressureMultiplier != 0.8f ||
        loaded.socialChatTelemetryRetentionHours != 72 || loaded.socialChatControlToken != "test-control-token")
    {
        return EXIT_FAILURE;
    }
    return EXIT_SUCCESS;
}
