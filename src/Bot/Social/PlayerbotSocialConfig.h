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
    uint32 socialChatAmbientCadenceSeconds = 300;

    // Applied only while the stage is autonomous_society; every earlier stage keeps the built-in
    // two-turn cap and 0.62 decay untouched.
    uint32 socialChatAutonomousMaxConsecutiveBotTurns = 6;
    float socialChatAutonomousBotTurnDecay = 0.85f;
    float socialChatAutonomousContinuationPressureBase = 0.8f;

    // Relationship-driven whisper check-ins (autonomous_society only): how warm a pair must be
    // before a bot may open a whisper, and how long a pair rests between check-ins.
    float socialChatWhisperMinFamiliarity = 0.01f;
    uint32 socialChatWhisperPairCooldownSeconds = 21600;

    // Server-wide provider (LLM sidecar) call ceiling per sliding hour. Zero means no ceiling.
    uint32 socialChatProviderHourlyBudget = 120;
    uint32 socialChatTelemetryRetentionHours = 48;
    std::string socialChatControlToken;
};

[[nodiscard]] PlayerbotSocialConfigValues NormalizePlayerbotSocialConfig(PlayerbotSocialConfigValues values);
void ReloadPlayerbotSocialConfig();

extern PlayerbotSocialConfigValues sPlayerbotSocialConfig;

#endif
