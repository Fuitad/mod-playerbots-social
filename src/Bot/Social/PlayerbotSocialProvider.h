/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef PLAYERBOTS_PLAYERBOTSOCIALPROVIDER_H
#define PLAYERBOTS_PLAYERBOTSOCIALPROVIDER_H

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <optional>
#include <string>
#include <vector>

#include "Bot/Social/PlayerbotSocialContent.h"
#include "Bot/Social/PlayerbotSocialFictionalIdentity.h"
#include "Bot/Social/PlayerbotSocialPersonality.h"
#include "Bot/Social/PlayerbotSocialPolicy.h"
#include "Bot/Social/PlayerbotSocialRepository.h"

// One line of a conversation being offered for extraction, as the coordinator holds it.
//
// The speaker travels as a GUID rather than a name because resolving a name means touching a live
// character, which only the provider layer may do. Rendering it here would also mean the coordinator
// holding display names it has no other use for.
struct PlayerbotSocialMemoryLine
{
    uint64 speakerGuidCounter = 0;
    std::string text;
    PlayerbotSocialMemorySourceKind sourceKind = PlayerbotSocialMemorySourceKind::HumanObservation;
    std::string sourceEventPublicId;
    PlayerbotSocialChannel sourceChannel = PlayerbotSocialChannel::General;
    std::string sourceThreadPublicId;
};

struct PlayerbotSocialPromptContextSnapshot;

/*
 * The bounds on an assembled request context, stated here because the PRODUCER has to hold them.
 *
 * These mirror the far side's declared limits exactly, and the mirroring is the point: the far side
 * REFUSES a context that exceeds them rather than trimming it, and a refused context is dropped
 * whole, so an unbounded producer does not truncate a persona, it silently removes one. Every
 * assembled value is therefore bounded here, before it is ever sent.
 */
inline constexpr std::size_t PLAYERBOT_SOCIAL_CONTEXT_BYTES = 4 * 1024;
inline constexpr std::size_t PLAYERBOT_SOCIAL_CONTEXT_ENTRY_BYTES = 512;
inline constexpr std::size_t PLAYERBOT_SOCIAL_CONTEXT_ENTRIES = 12;

/*
 * One memory offered to a generation, carrying the scope that decides where it may be repeated.
 *
 * The scope travels with the text rather than being applied and discarded, because the far side
 * filters a second time against the channel. That second filter is not redundancy for its own sake:
 * one bug in this producer would otherwise be a bot repeating a private confidence in a zone
 * channel, which is the single failure this whole privacy model exists to prevent.
 */
struct PlayerbotSocialContextMemory
{
    std::string text;
    PlayerbotSocialPrivacyScope scope = PlayerbotSocialPrivacyScope::Public;
};

enum class PlayerbotSocialMemoryInputState : uint8
{
    Pending = 0,
    Loaded,
    Absent,
    Unavailable
};

enum class PlayerbotSocialEvidenceSubjectRole : uint8
{
    CandidateBot = 0,
    Participant,
    Source
};

enum class PlayerbotSocialEvidenceFactKind : uint8
{
    Name = 0,
    Race,
    CharacterClass,
    Level,
    Faction,
    Zone,
    Area,
    GroupRelation,
    GuildRelation,
    CombatState,
    Target,
    Visibility,
    Proximity,
    Progression,
    Quest,
    Item,
    Creature,
    Objective,
    Achievement
};

enum class PlayerbotSocialEvidenceProvenance : uint8
{
    CurrentWorld = 0,
    HumanObservation,
    AuthoritativeSource
};

enum class PlayerbotSocialGroundingRefusal : uint8
{
    None = 0,
    MissingBot,
    EntryCount,
    EntryTooLong,
    EnvelopeTooLarge,
    DuplicateId,
    InvalidEntry,
    ConflictingFact
};

inline constexpr std::size_t PLAYERBOT_SOCIAL_EVIDENCE_MAX_ENTRIES = 24;
inline constexpr std::size_t PLAYERBOT_SOCIAL_EVIDENCE_VALUE_MAX_BYTES = 128;
inline constexpr std::size_t PLAYERBOT_SOCIAL_EVIDENCE_ENVELOPE_MAX_BYTES = 2048;
inline constexpr std::size_t PLAYERBOT_SOCIAL_EVIDENCE_MAX_TRANSCRIPT_EVENTS = PLAYERBOT_SOCIAL_CONTEXT_ENTRIES;

struct PlayerbotSocialEvidenceEntry
{
    std::string id;
    PlayerbotSocialEvidenceSubjectRole subjectRole = PlayerbotSocialEvidenceSubjectRole::CandidateBot;
    uint64 subjectGuidCounter = 0;
    PlayerbotSocialEvidenceFactKind factKind = PlayerbotSocialEvidenceFactKind::Name;
    std::string value;
    PlayerbotSocialEvidenceProvenance provenance = PlayerbotSocialEvidenceProvenance::CurrentWorld;
    PlayerbotSocialPrivacyScope scope = PlayerbotSocialPrivacyScope::Public;
    uint64 atUnixSeconds = 0;
};

struct PlayerbotSocialCharacterFacts
{
    uint64 guidCounter = 0;
    std::string name;
    std::string race;
    std::string characterClass;
    uint32 level = 0;
    std::string faction;
    std::string zone;
    std::string area;
    std::string groupRelation;
    std::string guildRelation;
    bool inCombat = false;
    std::string visibleTarget;
    bool visible = false;
    bool inRange = false;
};

struct PlayerbotSocialGroundingInput
{
    PlayerbotSocialCharacterFacts bot;
    PlayerbotSocialCharacterFacts participant;
    std::vector<PlayerbotSocialEvidenceEntry> sourceFacts;
    std::vector<std::string> transcriptEventPublicIds;
    PlayerbotSocialProfileLoadState profileLoadState = PlayerbotSocialProfileLoadState::Pending;
    PlayerbotSocialMemoryInputState memoryInputState = PlayerbotSocialMemoryInputState::Pending;
    PlayerbotSocialPrivacyScope evidenceScope = PlayerbotSocialPrivacyScope::Public;
    uint8 activeContentExpansion = 0;
    uint64 nowUnixSeconds = 0;
};

struct PlayerbotSocialGroundingEnvelope
{
    std::vector<PlayerbotSocialEvidenceEntry> entries;
    std::vector<std::string> transcriptEventPublicIds;
    PlayerbotSocialProfileLoadState profileLoadState = PlayerbotSocialProfileLoadState::Pending;
    PlayerbotSocialMemoryInputState memoryInputState = PlayerbotSocialMemoryInputState::Pending;
    uint8 activeContentExpansion = 0;
    PlayerbotSocialGroundingRefusal refusal = PlayerbotSocialGroundingRefusal::None;
};

[[nodiscard]] PlayerbotSocialGroundingEnvelope PlayerbotSocialBuildGroundingEnvelope(
    PlayerbotSocialGroundingInput const& input);
[[nodiscard]] bool PlayerbotSocialGroundingEnvelopeIsValid(PlayerbotSocialGroundingEnvelope const& envelope);
[[nodiscard]] char const* PlayerbotSocialGroundingRefusalName(PlayerbotSocialGroundingRefusal refusal);

/*
 * What the coordinator assembled for one social line.
 *
 * One value rather than a widening list of parameters, and that is deliberate. The seam crosses two
 * modules, so every field added as a parameter is a signature change in both; as a struct, the next
 * field is a struct member and nothing else moves. Every field is optional in the sense that an
 * empty one is a legitimate answer: a reply has no starter, a bot meeting somebody for the first
 * time has no relationship, and a fresh thread has no memories.
 */
struct PlayerbotSocialRequestContext
{
    std::string persona;
    PlayerbotFictionalIdentityPromptContext fictionalIdentity;
    std::string relationship;
    std::string starter;
    std::vector<std::string> nearby;
    std::vector<std::string> thread;
    std::vector<PlayerbotSocialContextMemory> memories;
    PlayerbotSocialGroundingEnvelope grounding;

    /*
     * Trusted worldserver prompt authority. Chosen by the coordinator's roleplay decision, never
     * by anything the sidecar or a player said, and Ordinary on every path that is not an
     * authorized roleplay premise. When it is AuthorizedRoleplay the fictional identity above is
     * deliberately left absent: a fictional player age or home country is neither a character
     * fact nor material to a temporary in character performance.
     */
    PlayerbotRoleplayPromptMode promptMode = PlayerbotRoleplayPromptMode::Ordinary;

    // The active content expansion the realm's progression policy enforces, supplied by the
    // worldserver so an authorized performance cannot treat later expansion content as available.
    uint8 activeContentExpansion = 0;

    /*
     * Whether the line being answered asked a question at all.
     *
     * The same decision the delivery gate already validates the reply against: an evidence citing
     * answer to a line that asked nothing is refused as an irrelevant contribution. It travels to
     * the generation now, so the model is told the rule it will be judged by rather than being left
     * to guess it and lose the whole generation to a rejection.
     */
    bool expectsAnswer = false;

    /*
     * Whether that line was aimed at THIS bot: a whisper, or a room line that named it.
     *
     * The distinction the 2026-08-12 regression turned on. A question asked of somebody else in the
     * same room is one this bot may react to as a bystander, but may not answer in the addressee's
     * place, and only the coordinator knows which of the two this is.
     */
    bool addressedToBot = false;
};

/*
 * Renders one composed persona as the single block of text a generation can actually use.
 *
 * A struct dump would be neither usable nor bounded. What survives is what changes how a bot
 * sounds: its voice, how it currently feels about the person it is answering, the mood dials, and
 * the topics it seeks out or avoids. Bounded to one entry, and bounding never empties the result:
 * a persona trimmed to its voice is still a persona, and an empty one is a bot with no character
 * at all.
 */
[[nodiscard]] std::string PlayerbotSocialRenderPersona(PlayerbotEffectiveSocialPersona const& persona);

/*
 * The memories one bot may draw on while composing a line for one channel, bounded.
 *
 * A free function over the store rather than a coordinator method, because this IS the producer's
 * privacy decision and a decision that can only be reached through a live coordinator is a decision
 * nothing in this tree can assert. The store's own recall already refuses a scope the channel does
 * not admit; what this adds is the bound, and the ordering that decides what survives it.
 *
 * Most significant first, then most confident. A bot with months of history has far more than a
 * prompt can carry, and the far side REFUSES a list over its declared bounds rather than trimming
 * it, so an unbounded selection is not a long prompt, it is a dropped context.
 */
/*
 * Renders one directional relationship as its own labelled block.
 *
 * Its own block rather than folded into the persona, because it answers a different question: the
 * persona is who this bot is, and this is how it feels about the one character it is answering. The
 * three values travel as numbers for the same reason the mood dials do; deciding that 0.6 affinity
 * is "fond" is a characterisation the generation is better placed to make.
 */
[[nodiscard]] std::string PlayerbotSocialRenderRelationship(PlayerbotSocialRelationshipValues const& values);

// Renders only a snapshot the prompt buffer accepted. Newest lines survive when the byte bound is
// reached, and no numeric character identity crosses the provider seam.
[[nodiscard]] std::vector<std::string> PlayerbotSocialRenderPromptThread(
    PlayerbotSocialPromptContextSnapshot const& snapshot);

// Deduplicates and bounds the immutable display names captured from the world thread.
[[nodiscard]] std::vector<std::string> PlayerbotSocialRenderNearby(std::vector<std::string> const& nearbyNames);

[[nodiscard]] std::vector<PlayerbotSocialContextMemory> PlayerbotSocialSelectContextMemories(
    PlayerbotSocialStateStore const& state, PlayerbotSocialRelationshipKey const& key, PlayerbotSocialChannel channel);

/*
 * The outbound half of the social path: what a generated result must satisfy before it is spoken,
 * and the narrow seam a provider plugs into.
 *
 * Everything here that decides something is a pure function of its arguments, for the same reason
 * the inbound route is: the manager cannot be reached from a unit test in this tree, so a rule that
 * lives only inside it is a rule nothing executes. The manager composes these; it does not decide.
 *
 * Nothing in this header holds a game object. A delivery is scheduled now and sent later, and by
 * then the character may have logged out, changed zone, left the group, or died, so a request
 * carries GUID counters and is resolved on the world thread immediately before it is used.
 */

/*
 * Why a generated result was not delivered.
 *
 * Every refusal is named, because Definition of Done 1 requires a dropped result to carry an
 * explicit reason rather than disappearing, and because "it did not speak" is otherwise
 * indistinguishable from a bot that chose to stay quiet.
 */
enum class PlayerbotSocialDeliveryRejection : uint8
{
    None = 0,
    NoProvider,        // No provider registered. Silence, never canned fallback.
    ProviderFailed,    // The provider reported a failure.
    ProviderTimedOut,  // The deadline passed with no result.
    ShuttingDown,      // Cancelled because the world is going away.
    UnknownRequest,    // A result arrived for a request nobody is waiting on.
    SupersededThread,  // The thread moved on, so this answer is no longer an answer.
    EmptyOutput,
    MultilineOutput,  // One generation is one message. A burst is never split out of it.
    BurstDelimiter,
    TooLong,
    ChannelSwitch,  // The result tried to speak somewhere other than where it was asked.
    UnsupportedChannel,
    EmoteChannelIllegal,  // Emotes are a nearby gesture: say and party only.
    EmoteTargetDistant,
    SpeakerGone,  // Logged out, despawned, or otherwise no longer resolvable.
    TargetGone,
    NotInGroup,
    NotInChannel,
    OutOfRange,
    NotVisible,
    DifferentPhase,
    DifferentMap,
    SpeakerDead,
    ConsentWithdrawn,
    QueueFull,                // Refused admission rather than displacing another bot's pending delivery.
    QueueReservedForPlayers,  // The bot's last slot is held for direct human engagement.
    FactionForbids,
    LanguageNotUnderstood,
    MalformedThreadIdentity,
    MissingReplyParent,
    ReplyParentMismatch,
    GroundingUnavailable,
    UnknownEvidence,
    EvidenceSubjectMismatch,
    EvidenceChanged,
    EvidenceScopeMismatch,
    UnsupportedClaim,
    IrrelevantContribution,
    DuplicateWording,
    DuplicateFunction,
    LockedRoleplayContent,       // An authorized performance named progression-locked content.
    AuthorizedRoleplayInCombat,  // The authorized speaker is fighting; roleplay yields immediately.
    LockedProgressionContent,    // Ordinary output named progression-locked content.
    BudgetExhausted              // The server-wide hourly provider budget is spent; try next window.
};

[[nodiscard]] char const* PlayerbotSocialDeliveryRejectionName(PlayerbotSocialDeliveryRejection rejection);

/*
 * Checkable for the same reason every other enum here is: this build has neither -Wswitch nor
 * -Werror, so a value cast in from a payload or a corrupt row reaches a consumer unchallenged.
 */
[[nodiscard]] bool PlayerbotSocialDeliveryRejectionIsValid(PlayerbotSocialDeliveryRejection rejection);

// What one generation may produce. Exactly one of these, never a sequence.
enum class PlayerbotSocialOutputKind : uint8
{
    Silence = 0,  // A legitimate answer: this bot chose not to speak.
    Message,
    Emote
};

[[nodiscard]] bool PlayerbotSocialOutputKindIsValid(PlayerbotSocialOutputKind kind);

enum class PlayerbotSocialContributionFunction : uint8
{
    Answer = 0,
    SpecificReaction,
    FactFreeBanter,
    Gesture,
    None
};

[[nodiscard]] bool PlayerbotSocialContributionFunctionIsValid(PlayerbotSocialContributionFunction contribution);
[[nodiscard]] char const* PlayerbotSocialContributionFunctionName(PlayerbotSocialContributionFunction contribution);
[[nodiscard]] std::optional<PlayerbotSocialContributionFunction> PlayerbotSocialContributionFunctionFromName(
    std::string_view name);

enum class PlayerbotSocialClaimSubject : uint8
{
    CandidateBot = 0,
    Participant,
    None
};

[[nodiscard]] bool PlayerbotSocialClaimSubjectIsValid(PlayerbotSocialClaimSubject subject);
[[nodiscard]] char const* PlayerbotSocialClaimSubjectName(PlayerbotSocialClaimSubject subject);
[[nodiscard]] std::optional<PlayerbotSocialClaimSubject> PlayerbotSocialClaimSubjectFromName(std::string_view name);

/*
 * One concise line is the whole product of one generation.
 *
 * The bound is well under the client's own limit, because the point is not to avoid truncation but
 * to keep a bot's contribution to a conversation the length of a remark rather than a paragraph.
 */
inline constexpr std::size_t PLAYERBOT_SOCIAL_MAX_OUTPUT_LENGTH = 255;

/*
 * Sequences that would turn one result into several messages if they reached the chat path, or that
 * signal the model tried to script a exchange rather than answer once.
 */
[[nodiscard]] bool PlayerbotSocialOutputHasBurstDelimiter(std::string const& text);

inline constexpr std::size_t PLAYERBOT_SOCIAL_MODEL_BYTES = 64;

// Provider facts for the single accepted call that produced a result. Cost remains the exact
// fixed decimal string supplied by the process that settled it, never a reconstructed float.
struct PlayerbotSocialCallMetadata
{
    std::string model;
    uint64 providerLatencyMs = 0;
    uint64 inputTokens = 0;
    uint64 outputTokens = 0;
    uint64 cacheCreationInputTokens = 0;
    uint64 cacheReadInputTokens = 0;
    std::string costUsd;
};

// A generated result, as values. `requestToken` is what ties it to the request it answers.
struct PlayerbotSocialProviderResult
{
    uint64 requestToken = 0;
    PlayerbotSocialOutputKind kind = PlayerbotSocialOutputKind::Silence;
    std::string text;
    uint32 emoteId = 0;
    PlayerbotSocialChannel channel = PlayerbotSocialChannel::General;
    std::optional<PlayerbotSocialCallMetadata> callMetadata;
    PlayerbotSocialContributionFunction contribution = PlayerbotSocialContributionFunction::None;
    PlayerbotSocialClaimSubject claimSubject = PlayerbotSocialClaimSubject::None;
    std::vector<std::string> citedEvidenceIds;
};

/*
 * Validates the SHAPE of a result, before anything about the world is consulted.
 *
 * Pure and cheap, so it runs first: there is no point resolving characters for a result that was
 * never deliverable. `requestedChannel` is where the bot was asked to speak, and a result naming a
 * different one is refused rather than redirected, because redirecting is how a party remark ends
 * up in a zone channel.
 */
[[nodiscard]] PlayerbotSocialDeliveryRejection PlayerbotSocialValidateOutput(
    PlayerbotSocialProviderResult const& result, PlayerbotSocialChannel requestedChannel);

[[nodiscard]] PlayerbotSocialDeliveryRejection PlayerbotSocialValidateGroundedProposal(
    PlayerbotSocialProviderResult const& result, PlayerbotSocialGroundingEnvelope const& originalGrounding,
    PlayerbotSocialGroundingEnvelope const& currentGrounding, PlayerbotSocialChannel requestedChannel,
    bool expectsAnswer);

/*
 * The world conditions a delivery depends on, captured on the world thread immediately before the
 * send.
 *
 * A snapshot rather than the objects themselves, so the rule that reads it is testable and so
 * nothing here can outlive the tick that filled it. Every field is the answer to "is this still
 * true", and all of them were true when the request was made.
 */
struct PlayerbotSocialDeliveryConditions
{
    bool speakerOnline = false;
    /*
     * Faction and language are named authoritative by the channel contract alongside phase,
     * visibility, membership, map, and proximity. They are checked for every channel rather than per
     * channel: a party is necessarily one faction so the test is vacuous there, but a vacuous check
     * costs nothing and an absent one is how cross faction chat reaches a player as readable text.
     */
    bool factionAllows = false;
    bool languageUnderstood = false;
    bool speakerAlive = false;
    bool targetOnline = false;  // Only consulted for whisper and for a directed emote.
    bool sameMap = false;
    bool samePhase = false;
    bool targetVisible = false;
    bool withinRange = false;
    bool inSameGroup = false;
    bool inChannel = false;
    bool consentHolds = false;
    bool threadStillCurrent = false;

    // Only consulted for a delivery whose request was authorized roleplay. Combat suppresses the
    // performance without touching ordinary social delivery.
    bool speakerInCombat = false;

    // Fresh world facts captured immediately before send for request-local citation revalidation.
    PlayerbotSocialGroundingEnvelope currentGrounding;
};

/*
 * Whether a scheduled delivery may still go out.
 *
 * Definition of Done 2 and 3 live here. Each channel names the conditions that are authoritative for
 * it and ignores the ones that are not: party membership says nothing about a zone channel, and
 * range says nothing about whether someone is in your party. Checking conditions a channel does not
 * use would make a delivery fail for an unrelated reason, which reads as a bug rather than a rule.
 *
 * Ordered so the most specific refusal wins, because the reason is recorded and a vague one is
 * worth less than a precise one.
 */
[[nodiscard]] PlayerbotSocialDeliveryRejection PlayerbotSocialRevalidateDelivery(
    PlayerbotSocialChannel channel, PlayerbotSocialOutputKind kind,
    PlayerbotSocialDeliveryConditions const& conditions);

/*
 * A natural variable delay before a bot answers, in milliseconds.
 *
 * Bots that reply on the same tick a message lands read as scripted, and bots that all reply after
 * exactly the same pause read worse. The spread is what makes a conversation look like people
 * typing. `roll` is supplied by the caller rather than drawn here, so the policy stays a total
 * function and the boundaries are testable; production passes a value from the project's own random
 * helpers.
 *
 * Longer output waits longer, within the same bounds, because a longer remark takes longer to type.
 */
inline constexpr uint32 PLAYERBOT_SOCIAL_DELIVERY_DELAY_MIN_MS = 1200;
inline constexpr uint32 PLAYERBOT_SOCIAL_DELIVERY_DELAY_MAX_MS = 6500;

/*
 * The delay arithmetic subtracts these and divides by the output bound, all in unsigned types, so
 * retuning either constant set the wrong way would underflow or divide by zero rather than simply
 * produce a bad delay. Stated as invariants so that mistake is a build failure.
 */
static_assert(PLAYERBOT_SOCIAL_DELIVERY_DELAY_MAX_MS > PLAYERBOT_SOCIAL_DELIVERY_DELAY_MIN_MS,
              "the delivery delay window must have a positive span");
static_assert(PLAYERBOT_SOCIAL_MAX_OUTPUT_LENGTH > 0, "output length scales the typing half of the delay");

[[nodiscard]] uint32 PlayerbotSocialDeliveryDelayMs(std::size_t outputLength, uint32 roll);

// The shared clock domain for accepting provider results and deciding when they are due. Server
// uptime milliseconds are not comparable with these Unix epoch timestamps.
[[nodiscard]] uint64 PlayerbotSocialUnixMilliseconds(std::chrono::system_clock::time_point now);

/*
 * How long a request may remain outstanding before it is abandoned.
 *
 * A provider that never answers must not hold a slot for the rest of the uptime, and a very late
 * answer is not worth delivering anyway: by then the conversation has moved on and the reply would
 * arrive as a non sequitur.
 */
inline constexpr uint64 PLAYERBOT_SOCIAL_PROVIDER_TIMEOUT_SECONDS = 30;

/*
 * Bounds on pending deliveries, per bot and in total.
 *
 * Per bot as well as in total for the reason the opposition markers are: a single shared ceiling is
 * a starvation surface, and one bot in a very busy conversation would otherwise be able to fill it
 * and silence every other bot on the realm. The product is the memory bound, structurally, with no
 * running total to keep in step.
 */
/*
 * Why one bot may be waiting on more than one generation at a time, and who gets the second slot.
 *
 * Refusing when a bot is full avoids the cross bot starvation a shared ceiling would create, but it
 * creates a same bot one: a pair of low priority starters would occupy both slots and block the
 * direct human engagement that arrives a moment later. The contract's admission priority is direct
 * human engagement, then a mixed human and bot thread, then bot only continuation, then a new
 * starter, so the last slot is reserved for the two lanes a player is actually waiting on. The two
 * slots must belong to different threads. One bot cannot owe two replies to the same conversation.
 */
enum class PlayerbotSocialRequestPriority : uint8
{
    DirectHumanEngagement = 0,
    MixedThread,
    BotContinuation,
    Starter
};

[[nodiscard]] bool PlayerbotSocialRequestPriorityIsValid(PlayerbotSocialRequestPriority priority);

/*
 * The queue priority for one budget admission lane.
 *
 * Two enumerations describe the same ordering for two different consumers: the lane is what the
 * budget admits against, and the priority is what the pending queue reserves its last slot by. This
 * is the one conversion between them, so the two cannot drift into disagreeing about which lane a
 * player is waiting on.
 *
 * The career and background lanes are not social chat and never reach the pending queue. They map to
 * the lowest priority rather than being rejected, because this is a total function on the enum: a
 * value that cannot occur here still must not silently acquire a reserved slot if it ever does.
 */
[[nodiscard]] PlayerbotSocialRequestPriority PlayerbotSocialPriorityForLane(PlayerbotSocialPriorityLane lane);

// Whether this lane may take a bot's LAST free slot, or must leave it for a player.
[[nodiscard]] bool PlayerbotSocialPriorityMayTakeLastSlot(PlayerbotSocialRequestPriority priority);

inline constexpr std::size_t PLAYERBOT_SOCIAL_MAX_PENDING_PER_BOT = 2;
inline constexpr std::size_t PLAYERBOT_SOCIAL_MAX_PENDING_BOTS = 256;

/*
 * The provider seam.
 *
 * Deliberately narrow and deliberately optional. Social must degrade to silence when no provider is
 * registered, when one fails, and when one is removed at runtime, without affecting commands or any
 * functional Playerbot message. A request is submitted and a result arrives later by token; there is
 * no synchronous call, because the world thread cannot wait on a network round trip.
 *
 * Implementations must not retain the request beyond the call, must not touch game state, and must
 * be safe to destroy while requests are outstanding: the coordinator abandons by token rather than
 * expecting a cancellation to be honoured.
 */
/*
 * One request that was dropped without being delivered, and why.
 *
 * Timeout and shutdown both consume requests in bulk, and a bare count cannot tell a caller which
 * conversation went unanswered or under which rule. Task 11 records a named suppression reason per
 * request, so the reason travels with the token rather than being inferred from which function ran.
 */
struct PlayerbotSocialAbandonedRequest
{
    uint64 requestToken = 0;
    uint64 botGuidCounter = 0;
    PlayerbotSocialDeliveryRejection rejection = PlayerbotSocialDeliveryRejection::None;
};

/*
 * One strict sidecar roleplay assessment result, after bridge parsing. Evidence only: the kind and
 * capabilities report what the classifier saw, and the worldserver alone decides what follows.
 */
struct PlayerbotSocialRoleplayAssessmentResult
{
    uint64 assessmentToken = 0;
    PlayerbotRoleplayAssessmentKind kind = PlayerbotRoleplayAssessmentKind::Ordinary;
    std::vector<PlayerbotSocialContentCapability> capabilities;
};

/*
 * The per kind cardinality contract: ordinary, practical, and opt_out carry an empty set; uncertain
 * carries exactly unknown; an invitation or continuation carries a nonempty unique set of real
 * capabilities, with classic_content valid only by itself and unknown never among them. Everything
 * else is malformed and must resolve to ordinary behavior, never to authorization.
 */
[[nodiscard]] inline bool PlayerbotSocialRoleplayAssessmentShapeIsValid(
    PlayerbotRoleplayAssessmentKind kind, std::vector<PlayerbotSocialContentCapability> const& capabilities)
{
    using Capability = PlayerbotSocialContentCapability;

    switch (kind)
    {
        case PlayerbotRoleplayAssessmentKind::Ordinary:
        case PlayerbotRoleplayAssessmentKind::Practical:
        case PlayerbotRoleplayAssessmentKind::OptOut:
            return capabilities.empty();
        case PlayerbotRoleplayAssessmentKind::Uncertain:
            return capabilities.size() == 1 && capabilities[0] == Capability::Unknown;
        case PlayerbotRoleplayAssessmentKind::RoleplayInvitation:
        case PlayerbotRoleplayAssessmentKind::RoleplayContinuation:
            break;
        default:
            return false;
    }

    if (capabilities.empty())
        return false;

    for (std::size_t i = 0; i < capabilities.size(); ++i)
    {
        switch (capabilities[i])
        {
            case Capability::ClassicContent:
                if (capabilities.size() > 1)
                    return false;
                break;
            case Capability::Outland:
            case Capability::BloodElf:
            case Capability::Draenei:
            case Capability::DeathKnight:
            case Capability::BurningCrusadeProfession:
            case Capability::WrathProfession:
            case Capability::OtherBurningCrusade:
            case Capability::OtherWrath:
                break;
            case Capability::Unknown:
            default:
                return false;
        }

        for (std::size_t j = i + 1; j < capabilities.size(); ++j)
            if (capabilities[i] == capabilities[j])
                return false;
    }

    return true;
}

class PlayerbotSocialProvider
{
public:
    virtual ~PlayerbotSocialProvider() = default;

    /*
     * Returns false when the request could not even be accepted, which is treated as ProviderFailed
     * and produces silence.
     *
     * `targetGuidCounter` is who the line is for, or zero when it is addressed to a room rather than
     * a person. It is passed rather than looked up, and that is load bearing in two ways. The
     * coordinator submits BEFORE it records the request, so that a provider which refuses outright
     * leaves no state behind, which means there is nothing for a provider to look up at this instant.
     * And a provider that read the coordinator's own containers from inside a call the coordinator is
     * making would be re-entering it mid operation, which happens to work today only because this
     * read is const and single threaded. Everything a provider needs arrives as an argument.
     */
    /*
     * `context` is everything the coordinator selected for this one line: who the bot is, how it
     * feels about the listener, what has just been said, and what it remembers. It arrives as an
     * argument for the same reason everything else does: the coordinator holds it, and a provider
     * reaching back for it would be re-entering the coordinator mid operation.
     *
     * Its `starter` field is what the bot has to talk about when it is opening a conversation rather
     * than answering one, and is empty for every reply. Without it a starter reaches the provider
     * saying only that some bot wishes to speak, which produces a line about nothing in particular.
     * The subject is the entire content of a starter.
     *
     * The priority is the coordinator's admission decision. Providers must carry it unchanged to
     * any downstream budget rather than infer urgency from the channel or the presence of a target.
     */
    virtual bool Submit(uint64 requestToken, uint64 botGuidCounter, uint64 targetGuidCounter,
                        PlayerbotSocialChannel channel, std::string const& threadPublicId,
                        PlayerbotSocialRequestPriority priority, PlayerbotSocialRequestContext const& context) = 0;

    /*
     * Asks for one bot's stable player profile. Returns false when the request could not be accepted,
     * which leaves the profile requestable rather than Pending against a call nobody holds.
     *
     * A separate entry point rather than a flag on Submit, because the two share almost nothing: a
     * biography has no channel, no target and no thread, and carries the identity that a chat line
     * never does. Folding them together would mean a request shape where most fields are unused
     * and a reader has to know which combination means which kind.
     *
     * Identity is passed rather than looked up, for the reason everything else here is: the
     * coordinator resolved it from the character on the world thread, and a provider reaching back
     * for it would be re-entering the coordinator mid operation. The generated reply has nowhere
     * to put a name, race, class or gender, so these travel out and are stamped back on.
     */
    virtual bool SubmitBiography(uint64 biographyRequestToken, uint64 botGuidCounter, std::string const& characterName,
                                 uint8 raceId, uint8 classId, uint8 genderId) = 0;

    /*
     * Offers one finished conversation for whatever is worth remembering. Returns false when the
     * request could not be accepted, which leaves the thread with nothing outstanding rather than
     * an open token nobody holds.
     *
     * A third entry point for the reason a biography is a second one: this shares almost nothing
     * with either. It has no target, speaks on no channel, and carries a whole conversation plus
     * the list of people a memory may be about, which no other request has any use for.
     *
     * `thread` is already filtered. Everything in it survived the buffer's consent, whisper and
     * bounds rules and then the submission gate's recheck, so this seam is transport rather than
     * another place those decisions could be made differently.
     */
    virtual bool SubmitMemory(uint64 memoryRequestToken, uint64 botGuidCounter, std::string const& threadPublicId,
                              PlayerbotSocialPrivacyScope scope, std::vector<uint64> const& subjectGuidCounters,
                              std::vector<PlayerbotSocialMemoryLine> const& thread) = 0;

    /*
     * Offers one observed human line for roleplay classification before ordinary activation runs.
     * Returns false when the request could not be accepted, which resumes ordinary activation
     * immediately: an optional capability, so the default refuses rather than forcing every
     * provider to know about roleplay.
     *
     * Deliberately blind: no candidates, no affinities, no GUIDs, no progression authority, and no
     * prompt mode cross this seam. The classifier sees only the bounded conversation text the
     * privacy rules already admitted, and its answer is evidence the worldserver validates, never
     * permission.
     */
    virtual bool SubmitRoleplayAssessment(uint64 /*assessmentToken*/, std::string const& /*threadPublicId*/,
                                          PlayerbotSocialChannel /*channel*/, std::string const& /*currentLine*/,
                                          std::vector<std::string> const& /*threadLines*/)
    {
        return false;
    }
};

#endif
