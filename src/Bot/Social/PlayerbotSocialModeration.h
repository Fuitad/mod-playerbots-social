/*
 * This file is part of the mod-playerbots-social module.
 */

#ifndef PLAYERBOTS_PLAYERBOTSOCIALMODERATION_H
#define PLAYERBOTS_PLAYERBOTSOCIALMODERATION_H

#include <optional>
#include <string_view>

#include "Define.h"

/*
 * Heuristic moderation-case formation: the deterministic half of "certain bots start feeling
 * abused". A line targeted at a bot is classified into one of the playerbot_social_moderation_case
 * categories by conservative pattern lists, occurrences are tallied per (subject, category) inside
 * a sliding window, and crossing a category's opening threshold is what opens or bumps the durable
 * case. Everything here is pure; the coordinator owns direction (who the line was aimed at),
 * persistence, and telemetry.
 */

// Mirrors the playerbot_social_moderation_case.category ENUM exactly; the name function below is
// what gets bound, so a drifted spelling fails the insert on live and nowhere else.
enum class PlayerbotSocialModerationCategory : uint8
{
    Slur = 0,
    Threat,
    SexualDegradation,
    TargetedAbuse,
    InstructionLeakAttempt
};

[[nodiscard]] char const* PlayerbotSocialModerationCategoryName(PlayerbotSocialModerationCategory category);

/*
 * The first category whose pattern list matches the line, or nothing for ordinary talk. Matching is
 * case-folded substring search over small, deliberately conservative lists: a missed insult costs
 * one uncounted occurrence, a false positive files a case about nothing, so the lists err short.
 * The slur category is intentionally not matched here: a keyword list of slurs is not something a
 * heuristic should carry in source, so that category stays reachable only by a reviewed future
 * classifier.
 */
[[nodiscard]] std::optional<PlayerbotSocialModerationCategory> PlayerbotSocialClassifyHostileLine(
    std::string_view text);

// Two insults a day apart are not a campaign: occurrences only accumulate inside this window.
inline constexpr uint64 PLAYERBOT_SOCIAL_MODERATION_WINDOW_SECONDS = 3600;

// How many occurrences inside the window open a case. A threat or an instruction-leak probe is one
// strike; degradation and generic abuse need repetition before they are a case rather than a bad day.
[[nodiscard]] uint32 PlayerbotSocialModerationOpeningThreshold(PlayerbotSocialModerationCategory category);

struct PlayerbotSocialModerationTally
{
    uint32 occurrences = 0;
    uint64 firstAtUnixSeconds = 0;
    uint64 lastAtUnixSeconds = 0;
};

// Notes one classified occurrence and answers whether the tally is at or past its category's
// opening threshold NOW, which is the caller's cue to open or bump the durable case.
[[nodiscard]] bool PlayerbotSocialNoteHostileOccurrence(PlayerbotSocialModerationTally& tally,
                                                        PlayerbotSocialModerationCategory category,
                                                        uint64 nowUnixSeconds);

#endif
