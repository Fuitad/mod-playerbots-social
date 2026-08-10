#include <cstdlib>

#include "Bot/Social/PlayerbotSocialConfig.h"

int main()
{
    PlayerbotSocialConfigValues values;
    values.socialChatEnable = true;
    values.socialChatStage = "grounded_presence";
    values.socialChatDensityMultiplierQuiet = 0.75f;
    values.socialChatTelemetryRetentionHours = 72;

    PlayerbotSocialConfigValues const loaded = NormalizePlayerbotSocialConfig(values);
    if (!loaded.socialChatEnable || loaded.socialChatStage != "grounded_presence" ||
        loaded.socialChatDensityMultiplierQuiet != 0.75f || loaded.socialChatTelemetryRetentionHours != 72)
    {
        return EXIT_FAILURE;
    }
    return EXIT_SUCCESS;
}
