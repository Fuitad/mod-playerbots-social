#include <cstdlib>

#include "Bot/Social/PlayerbotSocialConfig.h"
#include "Config.h"

int main()
{
    sConfigMgr->SetOption<bool>("PlayerbotsSocial.Enable", true);
    sConfigMgr->SetOption<std::string>("PlayerbotsSocial.Stage", "grounded_presence");
    sConfigMgr->SetOption<float>("PlayerbotsSocial.DensityMultiplier.Quiet", 0.75f);
    sConfigMgr->SetOption<uint32>("PlayerbotsSocial.TelemetryRetentionHours", 72);

    ReloadPlayerbotSocialConfig();

    PlayerbotSocialConfigValues const& loaded = sPlayerbotSocialConfig;
    if (!loaded.socialChatEnable || loaded.socialChatStage != "grounded_presence" ||
        loaded.socialChatDensityMultiplierQuiet != 0.75f || loaded.socialChatTelemetryRetentionHours != 72)
    {
        return EXIT_FAILURE;
    }
    return EXIT_SUCCESS;
}
