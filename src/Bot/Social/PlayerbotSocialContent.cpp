/*
 * This file is part of the mod-playerbots-social module.
 */

#include "PlayerbotSocialContent.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <string>

namespace
{
constexpr std::uint8_t WRATH_CONTENT_EXPANSION = 2u;

struct ContentIndicator
{
    std::string_view phrase;
    PlayerbotSocialContentCapability capability;
};

constexpr std::array<ContentIndicator, 19> CONTENT_INDICATORS = {{
    {"outland", PlayerbotSocialContentCapability::Outland},
    {"blood elf", PlayerbotSocialContentCapability::BloodElf},
    {"blood elves", PlayerbotSocialContentCapability::BloodElf},
    {"sin dorei", PlayerbotSocialContentCapability::BloodElf},
    {"draenei", PlayerbotSocialContentCapability::Draenei},
    {"death knight", PlayerbotSocialContentCapability::DeathKnight},
    {"death knights", PlayerbotSocialContentCapability::DeathKnight},
    {"jewelcrafting", PlayerbotSocialContentCapability::BurningCrusadeProfession},
    {"inscription", PlayerbotSocialContentCapability::WrathProfession},
    {"burning crusade", PlayerbotSocialContentCapability::OtherBurningCrusade},
    {"heroics", PlayerbotSocialContentCapability::OtherBurningCrusade},
    {"heroic dungeon", PlayerbotSocialContentCapability::OtherBurningCrusade},
    {"heroic dungeons", PlayerbotSocialContentCapability::OtherBurningCrusade},
    {"hit 70", PlayerbotSocialContentCapability::OtherBurningCrusade},
    {"level 70", PlayerbotSocialContentCapability::OtherBurningCrusade},
    {"wrath of the lich king", PlayerbotSocialContentCapability::OtherWrath},
    {"northrend", PlayerbotSocialContentCapability::OtherWrath},
    {"hit 80", PlayerbotSocialContentCapability::OtherWrath},
    {"level 80", PlayerbotSocialContentCapability::OtherWrath},
}};

std::string NormalizeContentText(std::string_view text)
{
    std::string normalized;
    normalized.reserve(text.size() + 2u);
    normalized.push_back(' ');
    for (char character : text)
    {
        auto const byte = static_cast<unsigned char>(character);
        if (std::isalnum(byte))
            normalized.push_back(static_cast<char>(std::tolower(byte)));
        else if (normalized.back() != ' ')
            normalized.push_back(' ');
    }
    if (normalized.back() != ' ')
        normalized.push_back(' ');
    return normalized;
}
}  // namespace

std::uint8_t PlayerbotSocialActiveContentExpansion() { return WRATH_CONTENT_EXPANSION; }

bool PlayerbotSocialContentIsAllowed(PlayerbotSocialContentCapability capability)
{
    switch (capability)
    {
        case PlayerbotSocialContentCapability::ClassicContent:
        case PlayerbotSocialContentCapability::Outland:
        case PlayerbotSocialContentCapability::BloodElf:
        case PlayerbotSocialContentCapability::Draenei:
        case PlayerbotSocialContentCapability::DeathKnight:
        case PlayerbotSocialContentCapability::BurningCrusadeProfession:
        case PlayerbotSocialContentCapability::WrathProfession:
        case PlayerbotSocialContentCapability::OtherBurningCrusade:
        case PlayerbotSocialContentCapability::OtherWrath:
            return true;
        case PlayerbotSocialContentCapability::Unknown:
        default:
            return false;
    }
}

bool PlayerbotSocialContentIsAllowed(std::vector<PlayerbotSocialContentCapability> const& capabilities)
{
    if (capabilities.empty())
        return false;

    for (std::size_t i = 0; i < capabilities.size(); ++i)
        for (std::size_t j = i + 1; j < capabilities.size(); ++j)
            if (capabilities[i] == capabilities[j])
                return false;

    bool const hasClassicContent = std::find(capabilities.begin(), capabilities.end(),
                                             PlayerbotSocialContentCapability::ClassicContent) != capabilities.end();
    if (hasClassicContent && capabilities.size() > 1u)
        return false;

    return std::all_of(capabilities.begin(), capabilities.end(), [](PlayerbotSocialContentCapability capability)
                       { return PlayerbotSocialContentIsAllowed(capability); });
}

std::vector<PlayerbotSocialContentCapability> PlayerbotSocialDetectContentCapabilities(std::string_view text)
{
    std::vector<PlayerbotSocialContentCapability> detected;
    if (text.empty())
        return detected;

    std::string const normalized = NormalizeContentText(text);
    for (ContentIndicator const& indicator : CONTENT_INDICATORS)
    {
        std::string const padded = ' ' + std::string(indicator.phrase) + ' ';
        if (normalized.find(padded) == std::string::npos)
            continue;

        if (std::find(detected.begin(), detected.end(), indicator.capability) == detected.end())
            detected.push_back(indicator.capability);
    }
    return detected;
}
