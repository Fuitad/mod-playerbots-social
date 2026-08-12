/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef PLAYERBOTS_PLAYERBOTSOCIALTYPES_H
#define PLAYERBOTS_PLAYERBOTSOCIALTYPES_H

#include <array>
#include <cstddef>
#include <string_view>

#include "Define.h"

enum class PlayerbotSocialRolloutStage : uint8
{
    HumanReplies = 0,
    GroundedStarters,
    BoundedContinuation
};

enum class PlayerbotSocialProfileLoadState : uint8
{
    Pending = 0,          // A storage read is in flight. Social admission waits for its result.
    Loaded,               // A stored row was present and this build understands it.
    AbsentUsingBase,      // The read succeeded and no row exists. Defined first use behavior.
    RejectedUsingBase,    // A stored row existed but was not usable. The bot speaks with the seeded
                          // base personality; the state survives only as a diagnostic condition.
    UnavailableUsingBase  // Storage did not answer. Never equivalent to a successful absence.
};

enum class PlayerbotSocialMemorySourceKind : uint8
{
    HumanObservation = 0,
    AuthoritativeSource,
    GeneratedDelivery
};

[[nodiscard]] inline constexpr bool PlayerbotSocialMemorySourceKindIsValid(PlayerbotSocialMemorySourceKind kind)
{
    switch (kind)
    {
        case PlayerbotSocialMemorySourceKind::HumanObservation:
        case PlayerbotSocialMemorySourceKind::AuthoritativeSource:
        case PlayerbotSocialMemorySourceKind::GeneratedDelivery:
            return true;
    }

    return false;
}

[[nodiscard]] inline constexpr std::string_view PlayerbotSocialMemorySourceKindName(
    PlayerbotSocialMemorySourceKind kind)
{
    switch (kind)
    {
        case PlayerbotSocialMemorySourceKind::HumanObservation:
            return "human_observation";
        case PlayerbotSocialMemorySourceKind::AuthoritativeSource:
            return "authoritative_source";
        case PlayerbotSocialMemorySourceKind::GeneratedDelivery:
            return "generated_delivery";
    }

    return {};
}

/*
 * Versioned value contract shared by the worldserver social core, the generation provider bridge, and the
 * Medivh operator projections.
 *
 * Everything declared here is an immutable value. A live Player*, Unit*, or any other long lived raw
 * pointer never becomes part of this contract, because provider requests and telemetry records outlive
 * the world tick that produced them.
 *
 * Public identities are opaque by construction. Character GUIDs and integer row identities stay inside
 * the worldserver and the Medivh backend and never appear in a payload contract.
 */

inline constexpr uint32 PLAYERBOT_SOCIAL_SCHEMA_VERSION = 1;
inline constexpr uint32 PLAYERBOT_SOCIAL_PROTOCOL_VERSION = 1;

// Versions are rejected rather than coerced: an unknown peer is a bug or an unfinished deployment.
[[nodiscard]] inline constexpr bool PlayerbotSocialSchemaVersionIsSupported(uint32 version)
{
    return version == PLAYERBOT_SOCIAL_SCHEMA_VERSION;
}

[[nodiscard]] inline constexpr bool PlayerbotSocialProtocolVersionIsSupported(uint32 version)
{
    return version == PLAYERBOT_SOCIAL_PROTOCOL_VERSION;
}

// Opaque public identities -------------------------------------------------------------------------

inline constexpr std::size_t PLAYERBOT_SOCIAL_PUBLIC_ID_BODY_LENGTH = 32;

enum class PlayerbotSocialIdKind : uint8
{
    Actor = 0,
    Event,
    Thread,
    Memory,
    Relationship,
    ModerationCase,
    Request
};

// The kind tag keeps one identity from being accepted where another is required.
[[nodiscard]] inline constexpr std::string_view PlayerbotSocialPublicIdPrefix(PlayerbotSocialIdKind kind)
{
    switch (kind)
    {
        case PlayerbotSocialIdKind::Actor:
            return "act";
        case PlayerbotSocialIdKind::Event:
            return "evt";
        case PlayerbotSocialIdKind::Thread:
            return "thr";
        case PlayerbotSocialIdKind::Memory:
            return "mem";
        case PlayerbotSocialIdKind::Relationship:
            return "rel";
        case PlayerbotSocialIdKind::ModerationCase:
            return "cas";
        case PlayerbotSocialIdKind::Request:
            return "req";
    }

    return {};
}

[[nodiscard]] inline constexpr bool PlayerbotSocialPublicIdIsValid(PlayerbotSocialIdKind kind, std::string_view text)
{
    std::string_view const prefix = PlayerbotSocialPublicIdPrefix(kind);
    if (prefix.empty())
        return false;

    if (text.size() != prefix.size() + 1 + PLAYERBOT_SOCIAL_PUBLIC_ID_BODY_LENGTH)
        return false;

    if (text.substr(0, prefix.size()) != prefix)
        return false;

    if (text[prefix.size()] != '_')
        return false;

    for (std::size_t index = prefix.size() + 1; index < text.size(); ++index)
    {
        char const symbol = text[index];
        bool const isDigit = symbol >= '0' && symbol <= '9';
        bool const isLowercaseHex = symbol >= 'a' && symbol <= 'f';
        if (!isDigit && !isLowercaseHex)
            return false;
    }

    return true;
}

// Channels and memory privacy ----------------------------------------------------------------------

// The only surfaces this feature may ever read from or deliver to. World, guild, raid, yell, trade,
// Looking For Group, and the defense channels are deliberately absent.
enum class PlayerbotSocialChannel : uint8
{
    General = 0,
    Say,
    Party,
    Whisper
};

// Ordered from least to most private. The numeric order is the privacy lattice and is load bearing.
enum class PlayerbotSocialPrivacyScope : uint8
{
    Public = 0,
    Party,
    Whisper
};

/*
 * The number of channels, which is also the size of anything indexed by one.
 *
 * Kept next to the enum so the two cannot drift: a channel added above without widening this would
 * make every per-channel array one short, and an index that reads past the end is the failure this
 * feature can least afford to have in its silencing controls.
 */
inline constexpr std::size_t PLAYERBOT_SOCIAL_CHANNEL_COUNT = 4;

/*
 * Checkable because this build does not compile the module with -Wswitch or -Werror, so an
 * enumerator added later, or a corrupt value cast in from a payload, would not be caught by the
 * compiler. Any path that decides how to STORE a message must reject an invalid channel outright
 * rather than rely on the retrieval side failing closed.
 */
[[nodiscard]] inline constexpr bool PlayerbotSocialChannelIsValid(PlayerbotSocialChannel channel)
{
    switch (channel)
    {
        case PlayerbotSocialChannel::General:
        case PlayerbotSocialChannel::Say:
        case PlayerbotSocialChannel::Party:
        case PlayerbotSocialChannel::Whisper:
            return true;
    }

    return false;
}

[[nodiscard]] inline constexpr bool PlayerbotSocialPrivacyScopeIsValid(PlayerbotSocialPrivacyScope scope)
{
    switch (scope)
    {
        case PlayerbotSocialPrivacyScope::Public:
        case PlayerbotSocialPrivacyScope::Party:
        case PlayerbotSocialPrivacyScope::Whisper:
            return true;
    }

    return false;
}

[[nodiscard]] inline constexpr uint8 PlayerbotSocialPrivacyRank(PlayerbotSocialPrivacyScope scope)
{
    return static_cast<uint8>(scope);
}

[[nodiscard]] inline constexpr PlayerbotSocialPrivacyScope PlayerbotSocialChannelPrivacyScope(
    PlayerbotSocialChannel channel)
{
    switch (channel)
    {
        case PlayerbotSocialChannel::General:
        case PlayerbotSocialChannel::Say:
            return PlayerbotSocialPrivacyScope::Public;
        case PlayerbotSocialChannel::Party:
            return PlayerbotSocialPrivacyScope::Party;
        case PlayerbotSocialChannel::Whisper:
            return PlayerbotSocialPrivacyScope::Whisper;
    }

    // Fail closed. An unrecognized channel is treated as the least private one, so only public
    // memory can ever be retrieved for it. Reporting Whisper here would do the opposite and make a
    // corrupt channel value the widest possible retrieval scope.
    return PlayerbotSocialPrivacyScope::Public;
}

/*
 * A memory may only surface in a channel at least as private as the scope it was learned under, so a
 * party or whisper fact can never be introduced into General or say. Restricting a memory to the right
 * owner and subject is a separate concern layered on top of this channel rule.
 */
[[nodiscard]] inline constexpr bool PlayerbotSocialMemoryIsRetrievableInChannel(PlayerbotSocialPrivacyScope memoryScope,
                                                                                PlayerbotSocialChannel channel)
{
    /*
     * An unrecognized channel retrieves nothing at all, not even public memory.
     *
     * Mapping it to the Public scope and allowing public facts through would be fail closed only if
     * "public" meant "safe anywhere", and it does not: it means the fact was learned on a surface
     * this feature is allowed to speak on. General and say are those surfaces. A channel value this
     * build does not recognise is, by definition, not one of them, so delivering anything into it
     * would be routing content to an unknown destination. That is the exact shape the leak would
     * take if a later enumerator were added here and this switch were not updated with it.
     */
    if (!PlayerbotSocialChannelIsValid(channel))
        return false;

    if (!PlayerbotSocialPrivacyScopeIsValid(memoryScope))
        return false;

    return PlayerbotSocialPrivacyRank(PlayerbotSocialChannelPrivacyScope(channel)) >=
           PlayerbotSocialPrivacyRank(memoryScope);
}

// Directional relationship values ------------------------------------------------------------------

inline constexpr float PLAYERBOT_SOCIAL_FAMILIARITY_MIN = 0.0f;
inline constexpr float PLAYERBOT_SOCIAL_FAMILIARITY_MAX = 1.0f;
inline constexpr float PLAYERBOT_SOCIAL_AFFINITY_MIN = -1.0f;
inline constexpr float PLAYERBOT_SOCIAL_AFFINITY_MAX = 1.0f;
inline constexpr float PLAYERBOT_SOCIAL_TRUST_MIN = -1.0f;
inline constexpr float PLAYERBOT_SOCIAL_TRUST_MAX = 1.0f;

// A stranger is neutral, never mildly hostile and never pre-trusted.
inline constexpr float PLAYERBOT_SOCIAL_NEUTRAL_FAMILIARITY = 0.0f;
inline constexpr float PLAYERBOT_SOCIAL_NEUTRAL_AFFINITY = 0.0f;
inline constexpr float PLAYERBOT_SOCIAL_NEUTRAL_TRUST = 0.0f;

struct PlayerbotSocialRelationshipValues
{
    float familiarity = PLAYERBOT_SOCIAL_NEUTRAL_FAMILIARITY;
    float affinity = PLAYERBOT_SOCIAL_NEUTRAL_AFFINITY;
    float trust = PLAYERBOT_SOCIAL_NEUTRAL_TRUST;
};

namespace PlayerbotSocialDetail
{
[[nodiscard]] inline constexpr float ClampToRange(float value, float minimum, float maximum)
{
    if (value < minimum)
        return minimum;

    if (value > maximum)
        return maximum;

    return value;
}

// Decay and delta arithmetic can produce a NaN, and a NaN compares false against every bound,
// so it would otherwise pass straight through the clamp and reach a relationship row.
[[nodiscard]] inline constexpr float NeutralIfNotANumber(float value, float neutral)
{
    return value == value ? value : neutral;
}
}  // namespace PlayerbotSocialDetail

[[nodiscard]] inline constexpr bool PlayerbotSocialRelationshipIsInRange(
    PlayerbotSocialRelationshipValues const& values)
{
    return values.familiarity >= PLAYERBOT_SOCIAL_FAMILIARITY_MIN &&
           values.familiarity <= PLAYERBOT_SOCIAL_FAMILIARITY_MAX && values.affinity >= PLAYERBOT_SOCIAL_AFFINITY_MIN &&
           values.affinity <= PLAYERBOT_SOCIAL_AFFINITY_MAX && values.trust >= PLAYERBOT_SOCIAL_TRUST_MIN &&
           values.trust <= PLAYERBOT_SOCIAL_TRUST_MAX;
}

[[nodiscard]] inline constexpr PlayerbotSocialRelationshipValues PlayerbotSocialClampRelationship(
    PlayerbotSocialRelationshipValues const& values)
{
    PlayerbotSocialRelationshipValues clamped;
    clamped.familiarity = PlayerbotSocialDetail::ClampToRange(
        PlayerbotSocialDetail::NeutralIfNotANumber(values.familiarity, PLAYERBOT_SOCIAL_NEUTRAL_FAMILIARITY),
        PLAYERBOT_SOCIAL_FAMILIARITY_MIN, PLAYERBOT_SOCIAL_FAMILIARITY_MAX);
    clamped.affinity = PlayerbotSocialDetail::ClampToRange(
        PlayerbotSocialDetail::NeutralIfNotANumber(values.affinity, PLAYERBOT_SOCIAL_NEUTRAL_AFFINITY),
        PLAYERBOT_SOCIAL_AFFINITY_MIN, PLAYERBOT_SOCIAL_AFFINITY_MAX);
    clamped.trust = PlayerbotSocialDetail::ClampToRange(
        PlayerbotSocialDetail::NeutralIfNotANumber(values.trust, PLAYERBOT_SOCIAL_NEUTRAL_TRUST),
        PLAYERBOT_SOCIAL_TRUST_MIN, PLAYERBOT_SOCIAL_TRUST_MAX);
    return clamped;
}

// Raw event retention ------------------------------------------------------------------------------

// Raw message text lives only in the rolling event table, and never for less than this.
inline constexpr uint32 PLAYERBOT_SOCIAL_MIN_RETENTION_HOURS = 48;

[[nodiscard]] inline constexpr uint32 PlayerbotSocialNormalizeRetentionHours(uint32 configuredHours)
{
    return configuredHours < PLAYERBOT_SOCIAL_MIN_RETENTION_HOURS ? PLAYERBOT_SOCIAL_MIN_RETENTION_HOURS
                                                                  : configuredHours;
}

[[nodiscard]] inline constexpr uint64 PlayerbotSocialEventExpiry(uint64 capturedAtUnixSeconds, uint32 configuredHours)
{
    return capturedAtUnixSeconds +
           (static_cast<uint64>(PlayerbotSocialNormalizeRetentionHours(configuredHours)) * 3600ull);
}

// Provider requests and budget admission -----------------------------------------------------------

enum class PlayerbotSocialRequestKind : uint8
{
    ChatResponse = 0,
    BackstoryGeneration,
    MemoryExtraction,
    ModerationClassification,
    CareerGeneration
};

/*
 * Admission order, highest priority first. Direct human engagement outranks a mixed human and bot
 * thread, which outranks bot only continuation, which outranks a new starter.
 *
 * Career generation is deliberately not chatter. The plan places it between direct conversation and
 * background extraction, so it sits below the two human lanes and above the bot only lanes: starving a
 * functional gameplay system to fund ambient chatter would invert the plan's stated posture that
 * functional behavior survives.
 */
enum class PlayerbotSocialPriorityLane : uint8
{
    DirectHuman = 0,
    MixedHumanBot,
    CareerGeneration,
    BotOnlyContinuation,
    NewStarter,
    BackgroundExtraction
};

[[nodiscard]] inline constexpr bool PlayerbotSocialLaneIsHigherPriority(PlayerbotSocialPriorityLane left,
                                                                        PlayerbotSocialPriorityLane right)
{
    return static_cast<uint8>(left) < static_cast<uint8>(right);
}

// Only work a human is actively waiting on may draw from the protected reserve.
[[nodiscard]] inline constexpr bool PlayerbotSocialLaneMayUseHumanReserve(PlayerbotSocialPriorityLane lane)
{
    return lane == PlayerbotSocialPriorityLane::DirectHuman || lane == PlayerbotSocialPriorityLane::MixedHumanBot;
}

/*
 * One reservation row per model attempt. A reservation is created Reserved and settles exactly once:
 * Completed when the provider reported an actual cost, Released when the attempt failed before
 * spending, Expired when a crash abandoned it and a later transaction recovered it.
 */
enum class PlayerbotSocialBudgetState : uint8
{
    Reserved = 0,
    Completed,
    Released,
    Expired
};

[[nodiscard]] inline constexpr bool PlayerbotSocialBudgetTransitionIsAllowed(PlayerbotSocialBudgetState from,
                                                                             PlayerbotSocialBudgetState to)
{
    if (from != PlayerbotSocialBudgetState::Reserved)
        return false;

    return to != PlayerbotSocialBudgetState::Reserved;
}

// Reset selection ----------------------------------------------------------------------------------

enum class PlayerbotSocialResetKind : uint8
{
    SubjectCharacter = 0,
    SingleRelationship,
    SingleMemory,
    BotCohort
};

enum class PlayerbotSocialRecordClass : uint8
{
    Actor = 0,
    Profile,
    Relationship,
    Memory,
    Event,
    Consent,
    ModerationCase,
    RuntimeControl
};

/*
 * Which record classes a reset is allowed to delete. Raw telemetry and moderation audit are never
 * removed by a reset: a player clearing what bots remember about them must not also erase the abuse
 * record or the retained event history an operator relies on.
 */
[[nodiscard]] inline constexpr bool PlayerbotSocialResetDeletes(PlayerbotSocialResetKind reset,
                                                                PlayerbotSocialRecordClass recordClass)
{
    switch (reset)
    {
        case PlayerbotSocialResetKind::SubjectCharacter:
            return recordClass == PlayerbotSocialRecordClass::Relationship ||
                   recordClass == PlayerbotSocialRecordClass::Memory;
        case PlayerbotSocialResetKind::SingleRelationship:
            return recordClass == PlayerbotSocialRecordClass::Relationship;
        case PlayerbotSocialResetKind::SingleMemory:
            return recordClass == PlayerbotSocialRecordClass::Memory;
        case PlayerbotSocialResetKind::BotCohort:
            return recordClass == PlayerbotSocialRecordClass::Actor ||
                   recordClass == PlayerbotSocialRecordClass::Profile ||
                   recordClass == PlayerbotSocialRecordClass::Relationship ||
                   recordClass == PlayerbotSocialRecordClass::Memory;
    }

    return false;
}

// How much the feature is allowed to say. Calibrated live rather than frozen here, so this names the
// profile and carries no coefficients of its own.
enum class PlayerbotSocialDensityProfile : uint8
{
    Quiet = 0,
    Normal,
    Lively
};

inline constexpr std::size_t PLAYERBOT_SOCIAL_DENSITY_PROFILE_COUNT = 3;

[[nodiscard]] inline constexpr bool PlayerbotSocialDensityProfileIsValid(PlayerbotSocialDensityProfile profile)
{
    switch (profile)
    {
        case PlayerbotSocialDensityProfile::Quiet:
        case PlayerbotSocialDensityProfile::Normal:
        case PlayerbotSocialDensityProfile::Lively:
            return true;
    }

    return false;
}

/*
 * Parses the CONFIGURED profile name. Unknown, empty, and mixed case input all resolve to Normal
 * rather than to a disabled or maximal profile: a typo in the configuration must not silently make
 * the bots mute, and it must not silently make them loud either.
 *
 * That leniency is right for a config file and wrong for a socket. An administrative control parses
 * its own value strictly and refuses anything it does not recognize, rather than reusing this.
 */
[[nodiscard]] PlayerbotSocialDensityProfile PlayerbotSocialParseDensityProfile(std::string_view text);

[[nodiscard]] char const* PlayerbotSocialDensityProfileName(PlayerbotSocialDensityProfile profile);

/*
 * The values an authenticated operator may change at runtime, which are exactly the ones the
 * authoritative table stores and reapplies after a restart.
 *
 * Deliberately does NOT carry the feature's enablement or the telemetry retention floor. Those
 * belong to the deployment's configuration, and a stored row that could overwrite them would let a
 * control outlive the configuration meant to bound it.
 *
 * Lives here rather than beside the routing gate it feeds, so the manager can store one without
 * including the routing header and closing a cycle.
 */
struct PlayerbotSocialRuntimeControl
{
    bool paused = false;
    PlayerbotSocialDensityProfile density = PlayerbotSocialDensityProfile::Normal;

    /*
     * Which surfaces are still carrying, indexed by PlayerbotSocialChannel.
     *
     * An array rather than four named booleans because every use is "the channel this message came
     * in on", and four names invite a transposition that silences the wrong surface while every test
     * checking one channel at a time still passes.
     */
    std::array<bool, PLAYERBOT_SOCIAL_CHANNEL_COUNT> channelEnabled = {true, true, true, true};
};

#endif
