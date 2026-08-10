/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef PLAYERBOTS_PLAYERBOTSOCIALPERSONALITY_H
#define PLAYERBOTS_PLAYERBOTSOCIALPERSONALITY_H

#include <cstddef>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "Bot/Personality/PlayerbotPersonality.h"
#include "Bot/Social/PlayerbotSocialPolicy.h"
#include "Define.h"

/*
 * The social layer.
 *
 * Everything above this line is the persistent base contract used by career and Social consumers.
 * Everything below is the evolving social layer stored separately and layered on top without
 * changing the base. The two versions are deliberately separate so a social change never forces a
 * career recalculation.
 */
inline constexpr uint32 PLAYERBOT_SOCIAL_PERSONA_VERSION = 3;

[[nodiscard]] inline constexpr bool PlayerbotSocialPersonaVersionIsSupported(uint32 version)
{
    return version == PLAYERBOT_SOCIAL_PERSONA_VERSION;
}

// How long a failed biography generation waits before it may be attempted again.
inline constexpr uint64 PLAYERBOT_SOCIAL_BIOGRAPHY_RETRY_SECONDS = 900;

/*
 * How long a request may stay in flight before it is treated as abandoned.
 *
 * The pending state is durable, so a request lost to a worldserver restart or a dead provider
 * would otherwise leave that bot permanently without a biography and with nothing to retry. After
 * this long the request is considered gone and may be issued again.
 *
 * The contract this creates is deliberate and narrow, so state it plainly: this timeout cannot
 * tell a lost request from a slow one, so a provider call that was merely slow may still return
 * after a fresh request has been issued. Duplicates after the timeout are therefore PERMITTED, and
 * this state machine cannot prevent them.
 *
 * Be precise about what the state alone can and cannot guarantee, because the two are easy to
 * confuse. Requiring the profile to still be Pending before a completion is applied does guarantee
 * that a completion never replaces a biography that is already Ready. It does NOT identify WHICH
 * request a completion answers, so it cannot reject a stale result from a request that was already
 * superseded: after an operator reset and a fresh request, a very late reply to the original call
 * would still find the profile Pending and would be accepted. Closing that needs a request token
 * carried out to the provider and echoed back, which only the provider boundary can mint. Task 10
 * Definition of Done criterion 7 owns both halves, and neither is optional hardening.
 */
inline constexpr uint64 PLAYERBOT_SOCIAL_BIOGRAPHY_PENDING_TIMEOUT_SECONDS = 3600;

/*
 * Persisted bounded social traits, plus the interests and aversions the bot has picked up. All of
 * it changes only through the bounded evolution path in PlayerbotSocialPolicy, never directly from
 * player text.
 */
struct PlayerbotSocialTraits
{
    uint32 version = PLAYERBOT_SOCIAL_PERSONA_VERSION;
    uint8 warmth = PLAYERBOT_SOCIAL_MOOD_NEUTRAL;
    uint8 talkativeness = PLAYERBOT_SOCIAL_MOOD_NEUTRAL;
    uint8 curiosity = PLAYERBOT_SOCIAL_MOOD_NEUTRAL;
    uint8 humor = PLAYERBOT_SOCIAL_MOOD_NEUTRAL;
    uint8 formality = PLAYERBOT_SOCIAL_MOOD_NEUTRAL;
    std::vector<std::string> interests;
    std::vector<std::string> aversions;
    uint64 lastEvolvedAtUnixSeconds = 0;
};

// Mirrors the biography_state column. Absent means one has never been generated, Pending means a
// request is in flight, and RetryableFailure means a bounded retry is allowed later.
enum class PlayerbotBiographyState : uint8
{
    Absent = 0,
    Pending,
    Ready,
    RetryableFailure
};

/*
 * Checkable because this build compiles without -Wswitch and without -Werror, so a value cast in
 * from a corrupt row would not be caught by the compiler. Every path that decides whether to
 * generate, retrieve, or expose a biography must reject an unrecognized state outright.
 */
[[nodiscard]] inline constexpr bool PlayerbotBiographyStateIsValid(PlayerbotBiographyState state)
{
    switch (state)
    {
        case PlayerbotBiographyState::Absent:
        case PlayerbotBiographyState::Pending:
        case PlayerbotBiographyState::Ready:
        case PlayerbotBiographyState::RetryableFailure:
            return true;
    }

    return false;
}

// Identity facts. These come only from authoritative character data and are never invented.
struct PlayerbotBiographyIdentity
{
    std::string characterName;
    uint8 raceId = 0;
    uint8 classId = 0;
    uint8 genderId = 0;
};

// The compact player-style social profile, generated once and stable afterwards. The historical
// Biography name and field layout remain the wire/storage contract. Memory enriches how a bot
// behaves but never rewrites these fields.
struct PlayerbotBiography
{
    uint32 version = PLAYERBOT_SOCIAL_PERSONA_VERSION;
    PlayerbotBiographyIdentity identity;
    std::string origin;
    std::string motivation;
    std::string formativeExperience;
    std::string interests;
    std::string aversions;
    std::string preferredTopics;
    std::string mannerisms;
    std::string values;
};

struct PlayerbotSocialProfile
{
    uint32 version = PLAYERBOT_SOCIAL_PERSONA_VERSION;
    PlayerbotSocialTraits traits;
    PlayerbotBiographyState biographyState = PlayerbotBiographyState::Absent;
    PlayerbotBiography biography;
    uint64 biographyAttemptedAtUnixSeconds = 0;

    /*
     * Which request the profile is currently waiting on, and zero when it is waiting on none.
     *
     * The second half of the staleness rule, and durable for the same reason the state is: the
     * state says a request is in flight but cannot say WHICH, so after the pending timeout and a
     * fresh request, a very late reply to the superseded call would still find the profile Pending
     * and be accepted. A completion carrying a token this does not match is discarded.
     */
    uint64 biographyRequestToken = 0;
};

// A compact player-profile field. Anything longer is a sign the model wrote prose.
inline constexpr std::size_t PLAYERBOT_SOCIAL_BIOGRAPHY_MAX_FIELD_LENGTH = 240;

// New reasons are appended, never inserted, because the numeric value is what durable telemetry
// will carry once the operator surfaces exist.
enum class PlayerbotBiographyRejection : uint8
{
    None = 0,
    UnsupportedVersion,
    IdentityMismatch,
    MissingRequiredField,
    FieldTooLong,
    ForbiddenClaim,
    UnknownField,
    DuplicateField,
    LockedProgressionContent
};

struct PlayerbotBiographyValidation
{
    bool accepted = false;
    PlayerbotBiographyRejection rejection = PlayerbotBiographyRejection::None;
};

/*
 * The versioned structure is the whitelist. A generated payload may carry only these field names,
 * so a response that tries to introduce anything else, an instruction field in particular, is
 * rejected at the parse boundary instead of being silently dropped into the biography.
 *
 * Names are compared exactly, including case, so a near miss is an unknown field and not a match.
 */
[[nodiscard]] bool PlayerbotBiographyFieldIsKnown(std::string_view name);

/*
 * How many generated fields a biography has.
 *
 * Exposed alongside the predicate so a producer on the other side of the bridge can assert it
 * carries the WHOLE list rather than a subset. The predicate alone only proves each name it offers
 * is known, which a producer missing a field passes just as happily; that field then arrives as
 * MissingRequiredField and refuses a biography that was already generated and paid for.
 */
inline constexpr std::size_t PLAYERBOT_SOCIAL_BIOGRAPHY_FIELD_COUNT = 8;

/*
 * Whether a loaded document carries an identity and every generated field.
 *
 * Presence only. Bounds and content are ValidateBiography's job at the parse boundary, where the
 * text is new and untrusted; this answers a different question about text that has already been
 * through it and come back from storage, namely whether all of it came back.
 *
 * A single sentinel field cannot answer that: it proves one field survived and says nothing about
 * the other twelve. Ready is a claim that a whole biography exists, so a partial document has to
 * fail that claim rather than pass it on the strength of its first field.
 *
 * This is the second of two checks and it reads VALUES. Whether every key was present in the stored
 * JSON at all is decided by the query, because a missing `gender_id` and a real GENDER_MALE both
 * load as zero and no amount of inspecting the loaded values can separate them.
 */
[[nodiscard]] bool PlayerbotBiographyDocumentIsComplete(PlayerbotBiography const& biography);

// Stable label for durable telemetry and the operator UI. Never reword an existing one.
[[nodiscard]] char const* PlayerbotBiographyRejectionName(PlayerbotBiographyRejection rejection);

/*
 * One generated field exactly as it arrived, before anything about it is typed. This is what the
 * provider boundary hands in: the transport decides how to get from its own encoding to a list of
 * name and value pairs, and everything after that is this module's problem.
 */
struct PlayerbotBiographyFieldValue
{
    std::string name;
    std::string value;
};

struct PlayerbotBiographyAssembly
{
    bool accepted = false;
    PlayerbotBiographyRejection rejection = PlayerbotBiographyRejection::None;
    PlayerbotBiography biography;
};

/*
 * Why a generated biography was not applied to the profile that asked for it.
 *
 * Separate from PlayerbotBiographyRejection because these are answers to a different question. That
 * enum says whether a biography is acceptable in itself; this one says whether this profile is
 * still waiting on this particular request. A perfectly valid biography is refused when it answers
 * a call that was superseded, and that refusal must be distinguishable from a bad payload, because
 * the two mean opposite things about the provider.
 *
 * Appended to, never reordered: the numeric value is what durable telemetry carries.
 */
enum class PlayerbotBiographyCompletionRejection : uint8
{
    None = 0,
    NotAwaited,     // The profile is not waiting on any request, so nothing here answers anything.
    TokenMismatch,  // It is waiting, but on a different request than this one.
    Invalid         // It was awaited, and the answer did not survive validation.
};

// Stable label for the durable and operator facing surfaces. Never reword an existing one.
[[nodiscard]] char const* PlayerbotBiographyCompletionRejectionName(PlayerbotBiographyCompletionRejection rejection);

/*
 * The outcome of offering a completion to a profile.
 *
 * `profile` is what must be stored afterwards, and is the input unchanged whenever the completion
 * was refused by the fence. That makes the caller's job unconditional: it writes back what it is
 * given rather than deciding for itself when a refusal still altered something.
 *
 * `validation` carries the payload level reason and is meaningful only for `Invalid`.
 */
struct PlayerbotBiographyCompletion
{
    bool accepted = false;
    PlayerbotBiographyCompletionRejection rejection = PlayerbotBiographyCompletionRejection::None;
    PlayerbotBiographyRejection validation = PlayerbotBiographyRejection::None;
    PlayerbotSocialProfile profile;
};

/*
 * Which check refused a stored row. The state alone says a row was rejected; this says why, and
 * the operator response differs by reason: an unsupported version means a downgraded build reading
 * rows a newer one wrote, while an unknown state means the row itself is corrupt.
 *
 * This is a returned value rather than only text inside the warning, so the reason can be asserted,
 * recorded, and displayed. When more than one check fails, the first in evaluation order is
 * reported. Appended to, never reordered.
 */
enum class PlayerbotSocialProfileRejection : uint8
{
    None = 0,
    UnsupportedVersion,
    UnknownBiographyState,
    UnsupportedBiographyVersion,
    IncompleteBiography,
    MalformedStoredData
};

// The profile is always usable whatever the state, so a storage problem degrades a bot to its
// stable base personality instead of removing it from conversation.
struct PlayerbotSocialProfileLoad
{
    PlayerbotSocialProfileLoadState state = PlayerbotSocialProfileLoadState::Pending;
    PlayerbotSocialProfileRejection rejection = PlayerbotSocialProfileRejection::None;
    PlayerbotSocialProfile profile;
    bool storedRowPresent = false;
    uint32 storedSchemaVersion = 0;
    uint32 storedTraitsVersion = 0;
    PlayerbotBiographyState storedBiographyState = PlayerbotBiographyState::Absent;
    uint32 storedBiographyVersion = 0;
};

// The stable label for a load outcome, for a caller that records or displays it. The load itself
// also warns when it refuses a row, so a rejection is never silent while it waits for a caller to
// notice; this label is for the durable and operator facing surfaces, not for that warning.
[[nodiscard]] char const* PlayerbotSocialProfileLoadStateName(PlayerbotSocialProfileLoadState state);

// Stable label for the refusal reason. Never reword an existing one.
[[nodiscard]] char const* PlayerbotSocialProfileRejectionName(PlayerbotSocialProfileRejection rejection);

// Transient per opportunity context. None of this is persisted.
struct PlayerbotSocialPersonaContext
{
    PlayerbotSocialChannel channel = PlayerbotSocialChannel::Say;
    bool addressedDirectly = false;
    uint8 mood = PLAYERBOT_SOCIAL_MOOD_NEUTRAL;
};

/*
 * The assembled persona handed to the provider boundary.
 *
 * Value types only, and it owns every string it exposes. That is what makes it safe to copy onto a
 * worker: there is no Player, Unit, or other world pointer here that could dangle if the bot logs
 * out or the map unloads between assembly and use.
 */
struct PlayerbotEffectiveSocialPersona
{
    uint32 personaVersion = PLAYERBOT_SOCIAL_PERSONA_VERSION;
    PlayerbotPersonalityProfile base{};
    PlayerbotSocialTraits traits;
    PlayerbotBiographyState biographyState = PlayerbotBiographyState::Absent;
    PlayerbotBiography biography;
    PlayerbotSocialRelationshipValues relationship;
    PlayerbotSocialChannel channel = PlayerbotSocialChannel::Say;
    PlayerbotSocialPrivacyScope retrievalScope = PlayerbotSocialPrivacyScope::Public;
    uint8 engagementDisposition = 0;
    PlayerbotSocialStance stance = PlayerbotSocialStance::Neutral;
};

namespace PlayerbotPersonality
{
// Social layer -------------------------------------------------------------------------------

/*
 * Resolves the profile a bot should act on. A missing row and an unusable row both fall back to
 * a profile seeded from the stable base personality, so the bot still sounds like itself, and
 * both report a distinct state. An unsupported stored version is rejected rather than
 * reinterpreted.
 *
 * A rejected row is also warned about here, named by guid counter, rather than left for a
 * caller to notice. Only this function knows a row existed and was refused; by the time the
 * load returns, that is indistinguishable from a bot that simply has no row yet. A silent
 * degradation is exactly the failure an operator needs to see, and the state alone reaches
 * nobody until something logs it.
 */
PlayerbotSocialProfileLoad LoadSocialProfile(uint64 guidCounter, std::optional<PlayerbotSocialProfile> const& stored,
                                             PlayerbotPersonalityProfile const& base);

/*
 * Whether a biography should be requested now. False for a biography that is already in flight
 * or finished, which is what keeps generation lazy and one time rather than per chat event.
 */
bool ShouldRequestBiography(PlayerbotSocialProfile const& profile, uint64 nowUnixSeconds);

/*
 * Returns the profile as it must be stored the moment a biography request is issued. This is
 * the transition that actually makes generation one time: the predicate above stays true while
 * the state says no biography exists, so a caller that never records the request would ask
 * again on every opportunity. A call that is not due returns the profile unchanged.
 */
PlayerbotSocialProfile MarkBiographyRequested(PlayerbotSocialProfile const& profile, uint64 nowUnixSeconds,
                                              uint64 requestToken);

/*
 * Deterministic acceptance check for a generated biography. Structure, bounds, and identity are
 * verified against authoritative character data, and the free text is scanned for the claims a
 * player profile is not allowed to make. This check is authoritative: whatever the model reports
 * about its own output is advisory input only.
 */
PlayerbotBiographyValidation ValidateBiography(PlayerbotBiography const& candidate,
                                               PlayerbotBiographyIdentity const& authoritative);

/*
 * The entry point for generated output, and the only one that can enforce the whitelist. Once a
 * payload has been copied into the typed biography the unknown names are already gone, so the
 * check has to happen while the field names still exist: any name outside the versioned
 * structure, and any name given twice, is refused here rather than dropped. Identity is filled
 * from authoritative character data and is not accepted from the payload at all. What survives
 * is then put through the same content validation as any other candidate.
 */
PlayerbotBiographyAssembly AssembleBiography(std::vector<PlayerbotBiographyFieldValue> const& fields, uint32 version,
                                             PlayerbotBiographyIdentity const& authoritative);

/*
 * Offers one generated biography to the profile that requested it, and reports what must be
 * stored afterwards.
 *
 * The fence runs BEFORE validation, and the order is load bearing rather than an optimisation.
 * Validating first would let a stale or forged completion decide something about a profile that
 * was never waiting on it: a bad payload would demote a Ready biography to RetryableFailure, so
 * anything able to reach the provider boundary could erase every bot's biography by answering
 * old requests badly. A completion nobody is waiting on changes nothing at all.
 *
 * `authoritative` is the character's real identity, read from the world rather than from the
 * payload, and a biography claiming to be about somebody else is refused against it.
 */
PlayerbotBiographyCompletion ApplyBiographyCompletion(PlayerbotSocialProfile const& profile, uint64 requestToken,
                                                      PlayerbotBiography const& candidate,
                                                      PlayerbotBiographyIdentity const& authoritative);

/*
 * The same transition for a request that produced no biography at all, because the provider
 * failed, refused, or timed out.
 *
 * Fenced identically, so a failure report for a superseded request cannot open a retry against
 * the live one. Without this the bounded retry backoff is unreachable in production: a failed
 * request would sit Pending until the much longer abandonment window elapsed.
 */
PlayerbotBiographyCompletion ApplyBiographyFailure(PlayerbotSocialProfile const& profile, uint64 requestToken);

/*
 * Scores whether one bounded subject matches this bot's persisted interests or aversions.
 * Biography text is consulted only after it is Ready, so a partial or pending document cannot
 * influence selection.
 */
uint8 SocialContentRelevance(PlayerbotSocialProfile const& profile, std::string_view content);

PlayerbotSocialProfile EvolveSocialProfileAfterIndependentInteraction(PlayerbotPersonalityProfile const& base,
                                                                      PlayerbotSocialProfile const& profile,
                                                                      uint64 nowUnixSeconds);

/*
 * Assembles the persona for one opportunity. This is a pure read: it copies the base profile
 * and the biography through untouched and writes nothing back, so composing a persona never
 * turns into a profile update.
 */
PlayerbotEffectiveSocialPersona ComposeEffectiveSocialPersona(PlayerbotPersonalityProfile const& base,
                                                              PlayerbotSocialProfile const& profile,
                                                              PlayerbotSocialRelationshipValues const& relationship,
                                                              PlayerbotSocialPersonaContext const& context);
}  // namespace PlayerbotPersonality

#endif  // PLAYERBOTS_PLAYERBOTSOCIALPERSONALITY_H
