#include <algorithm>
#include <cstdlib>
#include <iostream>
#include <vector>

#include "Bot/Social/PlayerbotSocialContent.h"

namespace
{
void Require(bool condition, char const* message)
{
    if (condition)
        return;

    std::cerr << message << '\n';
    std::exit(EXIT_FAILURE);
}
}  // namespace

int main()
{
    using Capability = PlayerbotSocialContentCapability;

    Require(PlayerbotSocialActiveContentExpansion() == 2u, "the active Social content expansion must be Wrath");

    for (Capability capability : {
             Capability::ClassicContent,
             Capability::Outland,
             Capability::BloodElf,
             Capability::Draenei,
             Capability::DeathKnight,
             Capability::BurningCrusadeProfession,
             Capability::WrathProfession,
             Capability::OtherBurningCrusade,
             Capability::OtherWrath,
         })
    {
        Require(PlayerbotSocialContentIsAllowed(capability),
                "every recognized Wrath content capability must be allowed");
    }

    Require(!PlayerbotSocialContentIsAllowed(Capability::Unknown), "unknown content must remain fail closed");
    Require(!PlayerbotSocialContentIsAllowed(static_cast<Capability>(255)), "invalid content must remain fail closed");
    Require(!PlayerbotSocialContentIsAllowed(std::vector<Capability>{}),
            "an empty capability set cannot authorize roleplay");
    Require(!PlayerbotSocialContentIsAllowed(std::vector<Capability>{Capability::OtherWrath, Capability::OtherWrath}),
            "duplicate capabilities must remain malformed");
    Require(
        !PlayerbotSocialContentIsAllowed(std::vector<Capability>{Capability::ClassicContent, Capability::OtherWrath}),
        "classic content cannot be combined with another capability");
    Require(PlayerbotSocialContentIsAllowed(
                std::vector<Capability>{Capability::Outland, Capability::DeathKnight, Capability::OtherWrath}),
            "a valid mixed Wrath capability set must be allowed");

    std::vector<Capability> const detected =
        PlayerbotSocialDetectContentCapabilities("A blood elf death knight returned from Northrend.");
    Require(std::find(detected.begin(), detected.end(), Capability::BloodElf) != detected.end(),
            "content detection must retain blood elf evidence");
    Require(std::find(detected.begin(), detected.end(), Capability::DeathKnight) != detected.end(),
            "content detection must retain death knight evidence");
    Require(std::find(detected.begin(), detected.end(), Capability::OtherWrath) != detected.end(),
            "content detection must retain Wrath evidence");

    return EXIT_SUCCESS;
}
