#ifndef PLAYERBOTS_PLAYERBOTSOCIALCONFIG_H
#define PLAYERBOTS_PLAYERBOTSOCIALCONFIG_H

#include <string>

#include "Define.h"

struct PlayerbotSocialConfigValues
{
    bool socialChatEnable = false;
    std::string socialChatStage = "human_replies";
    std::string socialChatDensity = "normal";
    float socialChatDensityMultiplierQuiet = 0.55f;
    float socialChatDensityMultiplierNormal = 1.0f;
    float socialChatDensityMultiplierLively = 1.6f;
    float socialChatGeneralStarterPressureMultiplier = 0.55f;
    uint32 socialChatTelemetryRetentionHours = 48;
    std::string socialChatControlToken;
};

[[nodiscard]] PlayerbotSocialConfigValues NormalizePlayerbotSocialConfig(PlayerbotSocialConfigValues values);
void ReloadPlayerbotSocialConfig();

extern PlayerbotSocialConfigValues sPlayerbotSocialConfig;

#endif
