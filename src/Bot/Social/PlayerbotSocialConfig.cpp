#include "Bot/Social/PlayerbotSocialConfig.h"

#ifndef PLAYERBOTS_SOCIAL_STANDALONE
#include "Config.h"
#endif

#include <cmath>
#include <utility>

PlayerbotSocialConfigValues sPlayerbotSocialConfig;

PlayerbotSocialConfigValues NormalizePlayerbotSocialConfig(PlayerbotSocialConfigValues values)
{
    auto const normalizeMultiplier = [](float value, float fallback)
    { return std::isfinite(value) && value > 0.0f ? value : fallback; };
    values.socialChatDensityMultiplierQuiet = normalizeMultiplier(values.socialChatDensityMultiplierQuiet, 0.55f);
    values.socialChatDensityMultiplierNormal = normalizeMultiplier(values.socialChatDensityMultiplierNormal, 1.0f);
    values.socialChatDensityMultiplierLively = normalizeMultiplier(values.socialChatDensityMultiplierLively, 1.6f);
    values.socialChatGeneralStarterPressureMultiplier =
        normalizeMultiplier(values.socialChatGeneralStarterPressureMultiplier, 0.55f);
    if (values.socialChatTelemetryRetentionHours < 48)
        values.socialChatTelemetryRetentionHours = 48;
    return values;
}

void ReloadPlayerbotSocialConfig()
{
#ifndef PLAYERBOTS_SOCIAL_STANDALONE
    PlayerbotSocialConfigValues values;
    values.socialChatEnable = sConfigMgr->GetOption<bool>("PlayerbotsSocial.Enable", false);
    values.socialChatStage = sConfigMgr->GetOption<std::string>("PlayerbotsSocial.Stage", "human_replies");
    values.socialChatDensity = sConfigMgr->GetOption<std::string>("PlayerbotsSocial.Density", "normal");
    values.socialChatDensityMultiplierQuiet =
        sConfigMgr->GetOption<float>("PlayerbotsSocial.DensityMultiplier.Quiet", 0.55f);
    values.socialChatDensityMultiplierNormal =
        sConfigMgr->GetOption<float>("PlayerbotsSocial.DensityMultiplier.Normal", 1.0f);
    values.socialChatDensityMultiplierLively =
        sConfigMgr->GetOption<float>("PlayerbotsSocial.DensityMultiplier.Lively", 1.6f);
    values.socialChatGeneralStarterPressureMultiplier =
        sConfigMgr->GetOption<float>("PlayerbotsSocial.GeneralStarterPressureMultiplier", 0.55f);
    values.socialChatTelemetryRetentionHours =
        sConfigMgr->GetOption<uint32>("PlayerbotsSocial.TelemetryRetentionHours", 48);
    values.socialChatControlToken = sConfigMgr->GetOption<std::string>("PlayerbotsSocial.ControlToken", "");
    sPlayerbotSocialConfig = NormalizePlayerbotSocialConfig(std::move(values));
#endif
}
