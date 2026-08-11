/*
 * This file is part of the mod-playerbots-social module.
 */

#ifndef PLAYERBOTS_PLAYERBOTSOCIALCONTENT_H
#define PLAYERBOTS_PLAYERBOTSOCIALCONTENT_H

#include <cstdint>
#include <string_view>
#include <vector>

enum class PlayerbotSocialContentCapability : std::uint8_t
{
    ClassicContent,
    Outland,
    BloodElf,
    Draenei,
    DeathKnight,
    BurningCrusadeProfession,
    WrathProfession,
    OtherBurningCrusade,
    OtherWrath,
    Unknown
};

[[nodiscard]] std::uint8_t PlayerbotSocialActiveContentExpansion();
[[nodiscard]] bool PlayerbotSocialContentIsAllowed(PlayerbotSocialContentCapability capability);
[[nodiscard]] bool PlayerbotSocialContentIsAllowed(std::vector<PlayerbotSocialContentCapability> const& capabilities);
[[nodiscard]] std::vector<PlayerbotSocialContentCapability> PlayerbotSocialDetectContentCapabilities(
    std::string_view text);

#endif
