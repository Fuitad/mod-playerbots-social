#include "Bot/Social/PlayerbotSocialConfig.h"

#include <cmath>
#include <utility>

#include "Config.h"

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
    // Zero is an absent or unusable cadence, not "immediately"; the policy layer applies the same
    // fallback, so both agree on what an invalid value means.
    if (values.socialChatAmbientCadenceSeconds == 0)
        values.socialChatAmbientCadenceSeconds = 300;
    if (values.socialChatAutonomousMaxConsecutiveBotTurns == 0)
        values.socialChatAutonomousMaxConsecutiveBotTurns = 6;
    if (!std::isfinite(values.socialChatAutonomousBotTurnDecay) || values.socialChatAutonomousBotTurnDecay <= 0.0f ||
        values.socialChatAutonomousBotTurnDecay > 1.0f)
        values.socialChatAutonomousBotTurnDecay = 0.95f;
    if (!std::isfinite(values.socialChatAutonomousContinuationPressureBase) ||
        values.socialChatAutonomousContinuationPressureBase <= 0.0f ||
        values.socialChatAutonomousContinuationPressureBase > 1.0f)
        values.socialChatAutonomousContinuationPressureBase = 0.95f;
    // Zero would read as "unset" at the gate and quietly restore the built-in rail; anything above
    // the built-in cooldown would LENGTHEN it, which is not what this option is for.
    if (values.socialChatAutonomousBotReplyCooldownSeconds == 0 ||
        values.socialChatAutonomousBotReplyCooldownSeconds > 45)
        values.socialChatAutonomousBotReplyCooldownSeconds = 3;
    if (!std::isfinite(values.socialChatWhisperMinFamiliarity) || values.socialChatWhisperMinFamiliarity <= 0.0f ||
        values.socialChatWhisperMinFamiliarity > 1.0f)
        values.socialChatWhisperMinFamiliarity = 0.01f;
    if (values.socialChatWhisperPairCooldownSeconds == 0)
        values.socialChatWhisperPairCooldownSeconds = 21600;
    if (values.socialChatTelemetryRetentionHours < 48)
        values.socialChatTelemetryRetentionHours = 48;
    return values;
}

void ReloadPlayerbotSocialConfig()
{
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
    values.socialChatAmbientCadenceSeconds =
        sConfigMgr->GetOption<uint32>("PlayerbotsSocial.AmbientCadenceSeconds", 300);
    values.socialChatAutonomousMaxConsecutiveBotTurns =
        sConfigMgr->GetOption<uint32>("PlayerbotsSocial.Autonomous.MaxConsecutiveBotTurns", 6);
    values.socialChatAutonomousBotTurnDecay =
        sConfigMgr->GetOption<float>("PlayerbotsSocial.Autonomous.BotTurnDecay", 0.95f);
    values.socialChatAutonomousContinuationPressureBase =
        sConfigMgr->GetOption<float>("PlayerbotsSocial.Autonomous.ContinuationPressureBase", 0.95f);
    values.socialChatAutonomousBotReplyCooldownSeconds =
        sConfigMgr->GetOption<uint32>("PlayerbotsSocial.Autonomous.BotReplyCooldownSeconds", 3);
    values.socialChatWhisperMinFamiliarity =
        sConfigMgr->GetOption<float>("PlayerbotsSocial.Whisper.MinFamiliarity", 0.01f);
    values.socialChatWhisperPairCooldownSeconds =
        sConfigMgr->GetOption<uint32>("PlayerbotsSocial.Whisper.PairCooldownSeconds", 21600);
    values.socialChatProviderHourlyBudget =
        sConfigMgr->GetOption<uint32>("PlayerbotsSocial.Provider.HourlyBudget", 120);
    values.socialChatTelemetryRetentionHours =
        sConfigMgr->GetOption<uint32>("PlayerbotsSocial.TelemetryRetentionHours", 48);
    values.socialChatControlToken = sConfigMgr->GetOption<std::string>("PlayerbotsSocial.ControlToken", "");
    sPlayerbotSocialConfig = NormalizePlayerbotSocialConfig(std::move(values));
}
