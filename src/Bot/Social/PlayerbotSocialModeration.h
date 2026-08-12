/*
 * This file is part of the mod-playerbots-social module.
 */

#ifndef PLAYERBOTS_PLAYERBOTSOCIALMODERATION_H
#define PLAYERBOTS_PLAYERBOTSOCIALMODERATION_H

#include <optional>
#include <string>
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

/*
 * The values the case upsert binds, built pure so a test can prove exactly what a row is opened
 * with; the statement itself fixes status='open' in its SQL and adds occurrences on collision.
 * The evidence JSON is bounded: a truncated last line, the speaker's actor id when known, and the
 * window occurrence count.
 */
struct PlayerbotSocialModerationCaseBinding
{
    uint32 subjectActorId = 0;
    std::string category;
    uint32 occurrenceContribution = 1;
    uint64 firstOccurredAtUnixSeconds = 0;
    uint64 lastOccurredAtUnixSeconds = 0;
    std::string evidenceJson;
};

[[nodiscard]] PlayerbotSocialModerationCaseBinding PlayerbotSocialBuildModerationCaseBinding(
    uint32 subjectActorId, PlayerbotSocialModerationCategory category,
    PlayerbotSocialModerationTally const& tally, std::optional<uint32> speakerActorId, std::string_view line);

#endif
