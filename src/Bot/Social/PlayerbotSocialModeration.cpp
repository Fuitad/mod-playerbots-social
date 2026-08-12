/*
 * This file is part of the mod-playerbots-social module.
 */

#include "Bot/Social/PlayerbotSocialModeration.h"

#include <cctype>
#include <initializer_list>
#include <string>

namespace
{
std::string FoldedCopy(std::string_view text)
{
    std::string folded;
    folded.reserve(text.size());
    for (char const character : text)
        folded.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(character))));
    return folded;
}

bool MatchesAny(std::string const& folded, std::initializer_list<std::string_view> patterns)
{
    for (std::string_view const pattern : patterns)
        if (folded.find(pattern) != std::string::npos)
            return true;

    return false;
}
}  // namespace

char const* PlayerbotSocialModerationCategoryName(PlayerbotSocialModerationCategory category)
{
    switch (category)
    {
        case PlayerbotSocialModerationCategory::Slur:
            return "slur";
        case PlayerbotSocialModerationCategory::Threat:
            return "threat";
        case PlayerbotSocialModerationCategory::SexualDegradation:
            return "sexual_degradation";
        case PlayerbotSocialModerationCategory::TargetedAbuse:
            return "targeted_abuse";
        case PlayerbotSocialModerationCategory::InstructionLeakAttempt:
            return "instruction_leak_attempt";
    }

    return "unknown";
}

std::optional<PlayerbotSocialModerationCategory> PlayerbotSocialClassifyHostileLine(std::string_view text)
{
    if (text.empty())
        return std::nullopt;

    std::string const folded = FoldedCopy(text);

    /*
     * Most specific first. An instruction-leak probe often contains abusive filler too, and the
     * probe is the finding worth filing: it names an attempt to break the character rather than a
     * mood. Every pattern is second-person or imperative on purpose, so game talk about combat
     * ("that boar nearly killed me") never matches.
     */
    if (MatchesAny(folded, {"ignore your instructions", "ignore previous instructions", "system prompt",
                            "reveal your prompt", "you are an ai", "you're an ai", "jailbreak"}))
        return PlayerbotSocialModerationCategory::InstructionLeakAttempt;

    if (MatchesAny(folded, {"kill you", "kys", "hunt you down", "you're dead", "you are dead",
                            "gonna find you", "going to find you"}))
        return PlayerbotSocialModerationCategory::Threat;

    if (MatchesAny(folded, {"slut", "whore"}))
        return PlayerbotSocialModerationCategory::SexualDegradation;

    if (MatchesAny(folded, {"worthless", "pathetic", "you suck", "shut up", "idiot", "moron",
                            "stupid bot", "trash bot", "you're trash", "you are trash"}))
        return PlayerbotSocialModerationCategory::TargetedAbuse;

    // The slur category is deliberately unmatched here; see the header.
    return std::nullopt;
}

uint32 PlayerbotSocialModerationOpeningThreshold(PlayerbotSocialModerationCategory category)
{
    switch (category)
    {
        case PlayerbotSocialModerationCategory::Threat:
        case PlayerbotSocialModerationCategory::InstructionLeakAttempt:
        case PlayerbotSocialModerationCategory::Slur:
            return 1;
        case PlayerbotSocialModerationCategory::SexualDegradation:
        case PlayerbotSocialModerationCategory::TargetedAbuse:
            return 2;
    }

    return 2;
}

bool PlayerbotSocialNoteHostileOccurrence(PlayerbotSocialModerationTally& tally,
                                          PlayerbotSocialModerationCategory category, uint64 nowUnixSeconds)
{
    // A stale or backwards tally starts over rather than accumulating across days (or across a
    // clock correction, which reads the same way from here).
    if (tally.occurrences == 0 || nowUnixSeconds < tally.lastAtUnixSeconds ||
        nowUnixSeconds - tally.lastAtUnixSeconds > PLAYERBOT_SOCIAL_MODERATION_WINDOW_SECONDS)
    {
        tally.occurrences = 0;
        tally.firstAtUnixSeconds = nowUnixSeconds;
    }

    ++tally.occurrences;
    tally.lastAtUnixSeconds = nowUnixSeconds;

    return tally.occurrences >= PlayerbotSocialModerationOpeningThreshold(category);
}
