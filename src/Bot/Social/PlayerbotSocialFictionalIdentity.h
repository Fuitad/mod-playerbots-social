/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef PLAYERBOTS_PLAYERBOTSOCIALFICTIONALIDENTITY_H
#define PLAYERBOTS_PLAYERBOTSOCIALFICTIONALIDENTITY_H

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

#include "Bot/Personality/PlayerbotFictionalIdentity.h"

enum class PlayerbotFictionalIdentityRequest : std::uint8_t
{
    None = 0,
    Age,
    HomeCountry,
    AgeAndHomeCountry
};

struct PlayerbotFictionalIdentityPromptContext
{
    PlayerbotFictionalIdentityRequest request = PlayerbotFictionalIdentityRequest::None;
    std::optional<std::uint8_t> age;
    std::optional<std::string> homeCountry;
};

struct PlayerbotEffectiveSocialPersona;

namespace PlayerbotFictionalIdentity
{
[[nodiscard]] PlayerbotFictionalIdentityPromptContext ResolveRequest(PlayerbotFictionalIdentityValue const& identity,
                                                                     std::string_view message, bool addressedDirectly,
                                                                     PlayerbotEffectiveSocialPersona const& persona);
}

#endif
