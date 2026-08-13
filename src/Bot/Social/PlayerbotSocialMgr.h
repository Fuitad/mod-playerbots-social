/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef PLAYERBOTS_PLAYERBOTSOCIALMGR_H
#define PLAYERBOTS_PLAYERBOTSOCIALMGR_H

// The coordinator now owns profiles, so the persona types are part of its interface rather than
// only an implementation detail of the file that composes them.
#include <array>
#include <cstddef>
#include <deque>
#include <functional>
#include <map>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "Bot/Social/PlayerbotSocialControl.h"
#include "Bot/Social/PlayerbotSocialExtraction.h"
#include "Bot/Social/PlayerbotSocialModeration.h"
#include "Bot/Social/PlayerbotSocialPersonality.h"
#include "Bot/Social/PlayerbotSocialPolicy.h"
#include "Bot/Social/PlayerbotSocialPromptContext.h"
#include "Bot/Social/PlayerbotSocialProvider.h"
#include "Bot/Social/PlayerbotSocialRepository.h"

/*
 * The conversation coordinator.
 *
 * It infers threads from observed messages and keeps the bookkeeping the pressure policy reads. Two
 * properties shape everything here:
 *
 * Nothing it stores is a live game object. Characters are GUID counters and messages are opaque
 * public identities, so a logout, a despawn, or an instance unload cannot leave a dangling pointer
 * inside a thread. The coordinator resolves an identity back to a character on the world thread
 * immediately before it is used, never before.
 *
 * Every container is bounded. Threads per scope, participants per thread, and retained event
 * identities all have caps, so a busy zone degrades by forgetting the oldest state rather than by
 * growing without limit.
 *
 * SHORTCUT: coordinator state lives in one worldserver process and is not shared. Promote it to
 * shared storage when a realm runs more than one worldserver, which is the point at which two
 * processes would otherwise infer two disjoint halves of the same conversation.
 */

// How long after the previous message a new one is still treated as the same conversation. Beyond
// this the speaker is starting something new rather than replying very late.
inline constexpr uint64 PLAYERBOT_SOCIAL_THREAD_CONTINUATION_SECONDS = 90;

// How often the raw event retention purge runs, and how many rows one pass may remove. Bounded on
// both axes so a long accumulated backlog is worked off across ticks rather than in one statement
// that would hold the world update while it ran.
inline constexpr uint32 PLAYERBOT_SOCIAL_PURGE_INTERVAL_MS = 300000;
inline constexpr uint32 PLAYERBOT_SOCIAL_PURGE_BATCH = 1000;

inline constexpr std::size_t PLAYERBOT_SOCIAL_MAX_THREADS_PER_SCOPE = 6;
inline constexpr std::size_t PLAYERBOT_SOCIAL_MAX_THREAD_PARTICIPANTS = 8;
inline constexpr std::size_t PLAYERBOT_SOCIAL_MAX_THREAD_EVENTS = 12;

// How many durable warm pairs the startup preload reads into the snapshot: the same bound the
// whisper pump's scan applies, because the preload exists solely to feed that scan.
inline constexpr std::size_t PLAYERBOT_SOCIAL_WARM_RELATIONSHIP_PRELOAD_LIMIT = 64;

/*
 * How many recent lines a thread recognises as already said.
 *
 * Deliberately smaller than the event history. This answers "did someone just say this", not "has
 * this ever been said here": a line that comes back around after a dozen turns is a callback rather
 * than a repetition, and refusing it would make a thread quieter the longer it runs.
 */
inline constexpr std::size_t PLAYERBOT_SOCIAL_MAX_THREAD_RECENT_LINES = 6;
inline constexpr std::size_t PLAYERBOT_SOCIAL_MAX_STARTER_CONTEXTS_PER_SCOPE = 4;

/*
 * One conversation space: a zone's General channel, one party, or one whisper pair. Threads never
 * cross a scope, so one room's context cannot leak into another.
 *
 * `scopeId` is 64 bits because a whisper scope has to name a PAIR of characters. Zone and group ids
 * are 32 bit and would fit in half of this, but two 32 bit character counters need the whole width,
 * and anything narrower would have to fold or hash the pair. Either would let two unrelated private
 * conversations land on one scope, and that collision is not a lost message, it is one whisper thread
 * absorbing a stranger's. The width is chosen so the pair is carried exactly and the collision cannot
 * happen at all, which makes it a privacy guarantee rather than a capacity estimate.
 */
struct PlayerbotSocialThreadKey
{
    PlayerbotSocialChannel channel = PlayerbotSocialChannel::General;
    uint64 scopeId = 0;
};

[[nodiscard]] bool operator<(PlayerbotSocialThreadKey const& left, PlayerbotSocialThreadKey const& right);

// One memory as the provider returned it, before this coordinator has agreed to store it.
struct PlayerbotSocialExtractedMemory
{
    std::string paraphrase;
    uint64 aboutGuidCounter = 0;
    PlayerbotSocialPrivacyScope scope = PlayerbotSocialPrivacyScope::Public;
    std::string sourceEventPublicId;
};

// One idle thread with something to submit. The scope travels with it because a memory's privacy is
// decided by the surface it was learned on, and the identity is the opaque one telemetry carries.
struct PlayerbotSocialIdleThread
{
    std::string threadPublicId;
    PlayerbotSocialThreadKey key;
    PlayerbotSocialExtractionSnapshot snapshot;
};

// A normalized observation. Values only, captured on the world thread.
struct PlayerbotSocialObservation
{
    PlayerbotSocialThreadKey key;
    std::string eventPublicId;
    PlayerbotSocialPromptLineRole role = PlayerbotSocialPromptLineRole::HumanObservation;
    std::string replyToEventPublicId;
    std::string sourceEventPublicId;
    uint64 speakerGuidCounter = 0;
    std::string speakerName;
    bool speakerIsHuman = false;
    uint32 zoneId = 0;
    uint64 atUnixSeconds = 0;

    /*
     * What was said. Reduced to a hash for duplicate detection on every channel, and additionally
     * held as text for idle memory extraction on the three public surfaces, from consenting speakers
     * only. A whisper is never held as text on any terms.
     *
     * An empty text is not a duplicate of anything and is buffered nowhere, so a caller with nothing
     * to give suppresses nothing and retains nothing by omitting it.
     */
    std::string text;
};

/*
 * Something that happened to a bot which it may open a conversation about: the loot, quest, kill, and
 * level events that used to be announced as a canned line. Held rather than said, so the decision to
 * speak stays with the coordinator.
 */
inline constexpr std::size_t PLAYERBOT_SOCIAL_STARTER_SUBJECT_MAX_LENGTH = 160;

enum class PlayerbotSocialStarterSourceKind : uint8
{
    Loot = 0,
    QuestTransition,
    Kill,
    Level,

    // Ambient kinds, produced by the module's own player hooks rather than the bot event bus, so
    // lines stop reading uniformly as grind reports.
    ZoneArrival,
    Death
};

// Bump when an enumerator is added above; the starter picker's rotation array is sized by it.
inline constexpr std::size_t PLAYERBOT_SOCIAL_STARTER_SOURCE_KIND_COUNT = 6;

enum class PlayerbotSocialQuestTransition : uint8
{
    None = 0,
    Accepted,
    ObjectiveProgress,
    ObjectiveCompleted,
    Failed,
    Completed,
    TurnedIn
};

struct PlayerbotSocialStarterSource
{
    PlayerbotSocialStarterSourceKind kind = PlayerbotSocialStarterSourceKind::Loot;
    PlayerbotSocialQuestTransition questTransition = PlayerbotSocialQuestTransition::None;
    std::string sourceEventPublicId;
    uint32 subjectId = 0;
    std::string subject;
};

[[nodiscard]] bool PlayerbotSocialStarterSourceIsValid(PlayerbotSocialStarterSource const& source);
[[nodiscard]] char const* PlayerbotSocialStarterSourceKindName(PlayerbotSocialStarterSourceKind kind);
[[nodiscard]] char const* PlayerbotSocialQuestTransitionName(PlayerbotSocialQuestTransition transition);
[[nodiscard]] std::string PlayerbotSocialStarterGroundingSubject(PlayerbotSocialStarterSource const& source);

/*
 * Conversation relationship credits. Exchanging turns in a thread builds familiarity and a little
 * affinity, never trust: trust stays earned through assistance. Only a bot may own a credit, so a
 * ledger is never kept on a person's behalf; a human can appear only as the subject a bot warmed
 * toward. The consent gate and the per-pair window ceiling both live in ApplyRelationshipDelta,
 * which every credit is applied through.
 */
inline constexpr float PLAYERBOT_SOCIAL_CONVERSATION_FAMILIARITY_DELTA = 0.004f;
inline constexpr float PLAYERBOT_SOCIAL_CONVERSATION_AFFINITY_DELTA = 0.003f;

struct PlayerbotSocialConversationCredit
{
    uint64 botGuidCounter = 0;
    uint64 subjectGuidCounter = 0;
    PlayerbotSocialRelationshipValues delta;
};

[[nodiscard]] std::vector<PlayerbotSocialConversationCredit> PlayerbotSocialConversationCredits(
    uint64 speakerGuidCounter, bool speakerIsHuman, uint64 previousSpeakerGuidCounter, bool previousSpeakerWasHuman);

struct PlayerbotSocialStarterAudience
{
    bool hasRealPartyMember = false;
    bool hasRealSayListener = false;
    bool hasRealGeneralMember = false;

    // Bot fallbacks, consulted only when the caller allows bot audiences (the autonomous society
    // stage). A real human anywhere still outranks every one of these.
    bool hasBotPartyMember = false;
    bool hasBotSayListener = false;
    bool hasBotGeneralMember = false;
};

[[nodiscard]] bool PlayerbotSocialSelectStarterChannel(PlayerbotSocialStarterAudience const& audience,
                                                       PlayerbotSocialChannel& channel, bool allowBotAudiences = false);

struct PlayerbotSocialStarterContext
{
    PlayerbotSocialThreadKey key;
    uint64 botGuidCounter = 0;
    PlayerbotSocialStarterSource source;
    uint64 audienceGuidCounter = 0;
    std::vector<uint64> sayCohortGuidCounters;
    uint32 zoneId = 0;
    uint64 atUnixSeconds = 0;
};

/*
 * Which pending starter a scope speaks about this pass. Kill and loot events vastly outnumber
 * everything else, so chasing freshness alone converges on every line being a grind report: the
 * kind spoken about longest ago wins first, and freshness only breaks ties inside that choice.
 * The caller guarantees a non-empty list.
 */
[[nodiscard]] std::size_t PlayerbotSocialPickStarterContext(
    std::vector<PlayerbotSocialStarterContext> const& starters,
    std::array<uint64, PLAYERBOT_SOCIAL_STARTER_SOURCE_KIND_COUNT> const& lastSpokenAtByKind);

/*
 * A handle to an inferred thread. `threadId` is the process local key and `publicId` is the opaque
 * identity telemetry and Medivh carry. A handle can outlive the thread it names once pruning has
 * run, which the coordinator treats as an unknown thread rather than as an error.
 */
struct PlayerbotSocialThreadHandle
{
    bool valid = false;
    uint64 threadId = 0;
    std::string publicId;
    std::string observedEventPublicId;
    std::string sourceEventPublicId;
    std::string rootSubject;

    /*
     * Whether the observed line repeats one this thread has heard recently.
     *
     * Answered here because this is the only place that sees both the thread's history and the new
     * line, and it is answered BEFORE the new line joins that history, so a message is never found
     * to be a duplicate of itself.
     */
    bool duplicateOfRecentMessage = false;
};

/*
 * What one encounter sweep closed and what it actually applied.
 *
 * The two are deliberately separate. `completed` counts the encounters that ended; `applied` is the
 * credit that survived each pair's window ceiling, which is smaller whenever a pair has already been
 * paid this window. Reporting only the count would make a rationed sweep look identical to a
 * generous one.
 */
struct PlayerbotSocialEncounterSweepResult
{
    std::size_t completed = 0;
    PlayerbotSocialRelationshipValues applied;
};

// Ceiling for the bounded grounding snapshot, its 24-entry maximum, and fixed JSON labels.
inline constexpr std::size_t PLAYERBOT_SOCIAL_OPERATOR_EVIDENCE_MAX_BYTES = 8 * 1024;

struct PlayerbotSocialOperatorEvidence
{
    PlayerbotSocialGroundingEnvelope grounding;
    PlayerbotSocialProfileLoad profileLoad;
    PlayerbotSocialRolloutStage rolloutStage = PlayerbotSocialRolloutStage::HumanReplies;
    PlayerbotSocialContributionFunction contribution = PlayerbotSocialContributionFunction::None;
    std::vector<std::string> citedEvidenceIds;
};

[[nodiscard]] std::optional<std::string> PlayerbotSocialSerializeOperatorEvidence(
    PlayerbotSocialOperatorEvidence const& evidence);

/*
 * One outstanding social generation.
 *
 * Values only, because a request is made now and answered later: by then the bot may have logged
 * out, the target may be gone, and the thread may have moved on. Everything needed to decide whether
 * it may still be spoken is either here or resolved on the world thread at delivery time.
 */
struct PlayerbotSocialPendingDelivery
{
    uint64 requestToken = 0;
    uint64 botGuidCounter = 0;
    uint64 targetGuidCounter = 0;
    uint64 subjectGuidCounter = 0;
    PlayerbotSocialChannel channel = PlayerbotSocialChannel::General;
    std::string threadPublicId;
    std::string replyToEventPublicId;
    std::string sourceEventPublicId;
    PlayerbotSocialGroundingEnvelope grounding;
    std::optional<PlayerbotSocialOperatorEvidence> operatorEvidence;
    uint64 requestedAtUnixSeconds = 0;
    PlayerbotSocialRequestPriority priority = PlayerbotSocialRequestPriority::Starter;

    /*
     * The zone the opportunity was observed in, carried rather than re-read.
     *
     * A request concludes later, by which time the bot may have moved or logged out, so resolving a
     * zone at conclusion time would either fail or report somewhere the conversation never happened.
     * Telemetry wants where the exchange took place, which is fixed when the request opens.
     */
    uint32 zoneId = 0;

    // A direct reply to an opted-out human may use only the current whisper and the bot's persona.
    // The flag lets delivery preserve that narrow consent exception without retaining the whisper.
    bool statelessDirectReply = false;

    // Set when the provider answers. Until then there is nothing to schedule.
    bool resultArrived = false;
    PlayerbotSocialProviderResult result;
    uint64 deliverAtUnixMilliseconds = 0;

    /*
     * Whether the worldserver authorized a temporary roleplay performance for this request. What
     * gates the two roleplay-specific delivery refusals: locked content in the generated line, and
     * a speaker who is fighting by the time the delivery is due. Never set by a provider result.
     */
    bool authorizedRoleplay = false;
    bool expectsAnswer = false;
};

/*
 * One bot considered for answering an observed message.
 *
 * The eligibility half and the scoring half of a candidate are carried together, because the two are
 * decided from the same snapshot and separating them invites the gate and the scorer to disagree
 * about the same bot.
 */
struct PlayerbotSocialActivationCandidate
{
    uint64 botGuidCounter = 0;
    PlayerbotPersonalityProfile personality;
    PlayerbotSocialProfileLoadState profileLoadState = PlayerbotSocialProfileLoadState::Pending;
    PlayerbotSocialGroundingEnvelope grounding;

    // Immutable display names physically near this responder when the opportunity was captured.
    // No GUID or live object crosses the provider seam.
    std::vector<PlayerbotSocialNearbySnapshotEntry> nearby;

    // Scoring. Meanings are PlayerbotSocialCandidate's, which this reduces to once eligibility passes.
    uint8 effectiveDisposition = 0;
    PlayerbotSocialStance stance = PlayerbotSocialStance::Neutral;
    bool addressedByName = false;
    bool askedQuestion = false;
    bool participatedInThread = false;
    uint8 contentRelevance = 0;

    // Eligibility. Per candidate rather than per thread: faction and language are relations between
    // this bot and the speaker, and one bot opting out says nothing about the next.
    bool optedOutOfInitiation = false;
    bool factionMatches = true;
    bool languageMatches = true;
    uint64 lastSpokeUnixSeconds = 0;

    // Captured from the live bot when the opportunity was collected. A fighting bot never receives
    // an authorized roleplay generation; ordinary social behavior is untouched by it.
    bool inCombat = false;
};

/*
 * Everything one activation decision needs, captured on the world thread as values.
 *
 * The thread state fields duplicate what the coordinator already holds for this thread. They are
 * passed rather than read back because a caller has usually just observed the message that changed
 * them, and re-reading would decide against state from before its own observation.
 */
struct PlayerbotSocialActivation
{
    PlayerbotSocialThreadHandle thread;
    PlayerbotSocialChannel channel = PlayerbotSocialChannel::General;
    uint64 speakerGuidCounter = 0;
    bool speakerIsHuman = false;
    bool speakerOptedOut = false;
    bool starter = false;

    // The bot whose authoritative event supplied a starter's subject. It is the only character
    // allowed to speak about that event in first person. Zero for replies.
    uint64 starterSourceBotGuidCounter = 0;
    uint64 starterAudienceGuidCounter = 0;
    std::string starterSourceEventPublicId;

    // True only for the relationship-driven whisper check-in; carried onto every candidate's
    // opportunity so the whisper starter gate can admit it.
    bool relationshipDriven = false;

    bool duplicateOfRecentMessage = false;

    // The line that opened this reply opportunity. Normally the thread owns it. For an opted-out
    // whisperer it is carried only through this stack-bound activation so the reply can be stateless.
    PlayerbotSocialPromptLine currentLine;

    /*
     * What the bot has to talk about, for a starter. Empty for every reply, where the observed
     * message is the subject.
     *
     * Carried on the activation rather than looked up, because the pending starter context that
     * holds it is consumed by the caller before this arrives: by the time the request is opened
     * there is nothing left to read it from.
     */
    std::string starterSubject;

    // Where the line was heard. Zero when the caller could not resolve one, which the event layer
    // stores as an absent zone rather than as zone zero.
    uint32 zoneId = 0;

    uint8 channelDensity = 0;  // 0..100, clamped on use
    uint64 threadLastActivityUnixSeconds = 0;
    uint32 relevantHumanMessages = 0;
    uint32 consecutiveBotOnlyTurns = 0;
    uint64 nowUnixSeconds = 0;

    /*
     * The single source of randomness for the whole decision, supplied rather than drawn here so
     * activation is a total function and its boundaries are testable. Production passes a value from
     * the project's own random helpers.
     *
     * There is deliberately no second roll input. Responder selection already draws the reply
     * pressure from this seed, and a roll here as well would apply the same probability twice.
     */
    uint64 selectionSeed = 0;

    std::vector<PlayerbotSocialActivationCandidate> candidates;
};

/*
 * What one activation decided, and why.
 *
 * Every refusal is named rather than implied by an empty token list, because a silent opportunity
 * that cannot be explained is indistinguishable from a broken one.
 */
struct PlayerbotSocialActivationResult
{
    std::vector<uint64> openedTokens;

    // The first thread level reason nothing could be opened, or None when the thread itself was fine.
    PlayerbotSocialOpportunityRejection rejection = PlayerbotSocialOpportunityRejection::None;

    // Why each candidate that was considered did not become eligible. Bounded by the candidate list.
    std::vector<std::pair<uint64, PlayerbotSocialOpportunityRejection>> refusedCandidates;

    // Set when every rule passed and the roll still declined. Not a failure: pressure is a
    // probability, and a conversation that always answers is the thing this feature must not become.
    bool pressureDeclined = false;

    // The probability selection was given. Recorded so a silent opportunity can be told apart from a
    // thread that never had a chance, without re-deriving it from state that has since moved on.
    float pressure = 0.0f;

    // Populated only when selection ran. Carries responders, alternates, suppressions, and factors.
    PlayerbotSocialSelection selection;

    // Why an opened responder's request was refused by the coordinator, keyed by bot. A selected bot
    // whose request never opened is reported here rather than silently missing from `openedTokens`.
    std::vector<std::pair<uint64, PlayerbotSocialDeliveryRejection>> refusedRequests;

    /*
     * The trusted prompt mode each opened responder speaks under, keyed by bot. Ordinary on every
     * path that is not an authorized roleplay premise; only the worldserver decision in
     * ApplyRoleplayAssessment can ever place AuthorizedRoleplay here.
     */
    std::vector<std::pair<uint64, PlayerbotRoleplayPromptMode>> promptModes;
};

/*
 * What the worldserver decided about one assessed opportunity before activation ran. Ordinary by
 * default: only a validated, progression-allowed invitation or continuation sets roleplayEligible,
 * and everything else runs the ordinary path unchanged.
 */
struct PlayerbotSocialRoleplayDirective
{
    PlayerbotRoleplayAssessmentKind kind = PlayerbotRoleplayAssessmentKind::Ordinary;
    bool roleplayEligible = false;
};

// Roleplay assessment lifecycle --------------------------------------------------------------------

/*
 * Transient per thread roleplay state bounds. Both containers live inside the thread and die with
 * it, so pruning is the retention rule and these are only the cardinality bounds.
 */
inline constexpr std::size_t PLAYERBOT_SOCIAL_MAX_ROLEPLAY_PARTICIPANTS = 8;
inline constexpr std::size_t PLAYERBOT_SOCIAL_MAX_ROLEPLAY_OPTOUTS = 32;

/*
 * Why an assessment result was not applied. Every discard is named so a silent conversation can be
 * told apart from a broken lifecycle, and a stale answer never reaches a conversation that moved on.
 */
enum class PlayerbotSocialRoleplayAssessmentDiscard : uint8
{
    None = 0,
    UnknownToken,     // No pending assessment holds this token: late, duplicate, or invented.
    MalformedResult,  // Invalid kind or capability shape. Falls back to ordinary activation.
    StaleThread,      // The thread was pruned while the assessment was in flight.
    StaleLine         // The thread moved past the assessed line. Answering now is a non sequitur.
};

[[nodiscard]] char const* PlayerbotSocialRoleplayAssessmentDiscardName(
    PlayerbotSocialRoleplayAssessmentDiscard discard);

// What AssessAndActivate decided: either the assessment is in flight, or activation already ran.
struct PlayerbotSocialAssessmentDisposition
{
    bool assessmentPending = false;
    uint64 assessmentToken = 0;
    PlayerbotSocialActivationResult immediate;  // Meaningful only when assessmentPending is false.
};

// What applying one provider assessment result did.
struct PlayerbotSocialAssessmentApplication
{
    PlayerbotSocialRoleplayAssessmentDiscard discard = PlayerbotSocialRoleplayAssessmentDiscard::None;
    bool activated = false;
    PlayerbotSocialActivationResult activation;  // Meaningful only when activated is true.
};

// Event vocabulary for the activation seam. Named here rather than spelled at each producer, so the
// feed's grouping cannot drift from one call site to the next.
inline constexpr std::string_view PLAYERBOT_SOCIAL_EVENT_TYPE_OPPORTUNITY = "social.opportunity";
inline constexpr std::string_view PLAYERBOT_SOCIAL_EVENT_TYPE_SELECTION = "social.selection";
inline constexpr std::string_view PLAYERBOT_SOCIAL_EVENT_TYPE_PROVIDER_ATTEMPT = "social.provider.attempt";
inline constexpr std::string_view PLAYERBOT_SOCIAL_EVENT_TYPE_OBSERVATION = "social.observation";
inline constexpr std::string_view PLAYERBOT_SOCIAL_REASON_PRESSURE_DECLINED = "pressure_declined";

/*
 * Silence, named.
 *
 * The delivery vocabulary reports a deliberately silent provider as `None`, because silence is an
 * answer rather than a refusal, and there is no rejection to borrow a word from. Without a name of
 * its own it would be written as an unqualified success, which reads in the feed exactly like a line
 * that was spoken.
 */
inline constexpr std::string_view PLAYERBOT_SOCIAL_REASON_PROVIDER_SILENCE = "provider_silence";

/*
 * What one opportunity's diagnostics may cost.
 *
 * The column is JSON, but the budget is what stops a busy zone from writing a blob per observed
 * line: every bot in a zone is a candidate, so anything proportional to the candidate list grows
 * without bound exactly when the server is busiest.
 */
inline constexpr std::size_t PLAYERBOT_SOCIAL_MAX_OPPORTUNITY_DIAGNOSTICS_LENGTH = 512;
inline constexpr std::size_t PLAYERBOT_SOCIAL_MAX_REPORTED_ALTERNATES = 3;
inline constexpr std::size_t PLAYERBOT_SOCIAL_MAX_REPORTED_FACTORS = 3;

/*
 * Turns one activation decision into the single event that explains it.
 *
 * ONE event, not one per candidate. Key Decision 4: the selected bot, the leading factors, and the
 * top alternatives are what make a silence explainable; a row per bot considered is what makes the
 * feed unreadable and the table unbounded.
 *
 * Pure, so every rule below is provable without a world: which of the several ways an opportunity
 * can go quiet is named, and that a suppressed opportunity never carries a bot it did not select.
 */
[[nodiscard]] PlayerbotSocialEventDraft PlayerbotSocialMakeOpportunityEvent(
    PlayerbotSocialActivation const& activation, PlayerbotSocialActivationResult const& result);

/*
 * One selected responder's own record of being chosen.
 *
 * Distinct from the opportunity event, which is one row for the whole decision and names only the
 * FIRST responder. On the coherent path two bots answer, so without this the second bot's delivery
 * would correlate back to nothing. This is still not a row per candidate: only a bot that selection
 * actually chose gets one, so the bound is the two responder ceiling rather than the zone's
 * population.
 *
 * Pure. `responderGuidCounter` is the bot this event is about; its rank is read off the ranked
 * field rather than assumed from the responder order.
 */
[[nodiscard]] PlayerbotSocialEventDraft PlayerbotSocialMakeSelectionEvent(PlayerbotSocialActivation const& activation,
                                                                          PlayerbotSocialActivationResult const& result,
                                                                          uint64 responderGuidCounter);

/*
 * How one provider request ended.
 *
 * Three outcomes, because the delivery rejection vocabulary covers two of them and has no word for
 * the third. A refusal of any kind, a timeout and a shutdown included, is `Refused` with the
 * rejection carrying the name. Silence is its own outcome precisely because it is NOT a refusal.
 */
enum class PlayerbotSocialProviderAttemptOutcome : uint8
{
    Answered = 0,
    Silent,
    Refused
};

/*
 * What became of one provider request, assembled at the point its fate is decided.
 *
 * Values only, and deliberately not a reference to the pending delivery: the request is erased from
 * the coordinator's map in the same step that decides its outcome, so anything reading the map
 * afterwards would find nothing.
 *
 * A `requestToken` of zero means the request never opened, which is the honest reading of a refusal
 * that happened before a token was minted. No sentinel is invented for it.
 */
struct PlayerbotSocialProviderAttempt
{
    uint64 requestToken = 0;
    uint64 botGuidCounter = 0;
    uint64 targetGuidCounter = 0;
    PlayerbotSocialChannel channel = PlayerbotSocialChannel::General;
    std::string threadPublicId;
    uint32 zoneId = 0;
    uint64 occurredAtUnixSeconds = 0;

    PlayerbotSocialProviderAttemptOutcome outcome = PlayerbotSocialProviderAttemptOutcome::Answered;

    // Present only for a complete answer accepted from the provider. Provider silence and every
    // refusal path have no call document to report.
    std::optional<PlayerbotSocialCallMetadata> callMetadata;
    std::optional<PlayerbotSocialOperatorEvidence> operatorEvidence;

    // Read only for `Refused`, where it supplies the reason. Left `None` otherwise, because neither
    // an answer nor a silence was refused by anything.
    PlayerbotSocialDeliveryRejection rejection = PlayerbotSocialDeliveryRejection::None;
};

[[nodiscard]] PlayerbotSocialEventDraft PlayerbotSocialMakeProviderAttemptEvent(
    PlayerbotSocialProviderAttempt const& attempt);

inline constexpr std::string_view PLAYERBOT_SOCIAL_EVENT_TYPE_ASSISTANCE = "social.assistance";
inline constexpr std::string_view PLAYERBOT_SOCIAL_EVENT_TYPE_PVP = "social.pvp";

/*
 * One assistance encounter that ended, and what it was worth.
 *
 * `earned` and `applied` are both carried because they can differ. The per window ceiling can refuse
 * part of what an encounter earned, and reporting only one number would make a capped encounter
 * indistinguishable from a small one.
 *
 * No channel and no thread. A fight is not a conversation, and inventing a thread identity for it
 * would correlate a heal to a chat that never mentioned it.
 */
struct PlayerbotSocialAssistanceCompletion
{
    uint64 beneficiaryGuidCounter = 0;
    uint64 helperGuidCounter = 0;
    PlayerbotSocialRelationshipValues earned;
    PlayerbotSocialRelationshipValues applied;
    uint64 occurredAtUnixSeconds = 0;
};

[[nodiscard]] PlayerbotSocialEventDraft PlayerbotSocialMakeAssistanceEvent(
    PlayerbotSocialAssistanceCompletion const& completion);

/*
 * One PVP opposition that cost the attacker something.
 *
 * The combat context is the reason the delta was what it was, so it is carried as the event's
 * reason rather than buried in diagnostics: a gank and a duel produce very different rows and the
 * feed has to say which one this was.
 */
struct PlayerbotSocialPvpOpposition
{
    uint64 victimGuidCounter = 0;
    uint64 attackerGuidCounter = 0;
    PlayerbotSocialCombatContext context = PlayerbotSocialCombatContext::Cooperative;

    /*
     * What the opposition EARNED, which is what the producer knows.
     *
     * `RecordPvpOpposition` decides the delta and the caller applies it, so only one triple is
     * available here, unlike an assistance completion where the same pass does both. Naming it
     * `earned` rather than `applied` is what stops this row claiming a write it never observed.
     */
    PlayerbotSocialRelationshipValues earned;
    uint64 occurredAtUnixSeconds = 0;
};

[[nodiscard]] PlayerbotSocialEventDraft PlayerbotSocialMakePvpEvent(PlayerbotSocialPvpOpposition const& opposition);

inline constexpr std::string_view PLAYERBOT_SOCIAL_EVENT_TYPE_DELIVERY = "social.delivery";

/*
 * One bot line the world actually accepted, on one supported surface.
 *
 * Only a delivery that SUCCEEDED is ever described by this value object. Coordinator suppression
 * uses a separate builder that carries no message text, so the feed can name why nothing was heard
 * without presenting refused output as speech.
 *
 * The origin is stated by the producer rather than inferred from the current world thread context.
 * A scope would be inherited by nested calls and bypassed by direct callers, so it cannot be the
 * source of truth for a property that belongs to the individual delivery.
 */
struct PlayerbotSocialDelivery
{
    std::string eventPublicId;
    std::string replyToEventPublicId;
    std::string sourceEventPublicId;
    uint64 botGuidCounter = 0;
    PlayerbotSocialChannel channel = PlayerbotSocialChannel::General;
    PlayerbotSocialEventOrigin origin = PlayerbotSocialEventOrigin::Legacy;

    // The addressed character, for a whisper. Zero on every surface addressed to a room.
    uint64 targetGuidCounter = 0;

    // Empty for functional output, which belongs to no conversation. Correlating a status
    // announcement to whichever chat happened to be open would be a fabrication.
    std::string threadPublicId;

    uint32 zoneId = 0;
    std::string text;

    // An emote is a gesture with no line. The id travels in diagnostics so the feed can show what
    // the bot did without inventing words for it.
    bool isEmote = false;
    uint32 emoteId = 0;

    std::optional<PlayerbotSocialCallMetadata> callMetadata;
    std::optional<PlayerbotSocialOperatorEvidence> operatorEvidence;

    uint64 occurredAtUnixSeconds = 0;
};

[[nodiscard]] PlayerbotSocialEventDraft PlayerbotSocialMakeDeliveryEvent(PlayerbotSocialDelivery const& delivery);

[[nodiscard]] PlayerbotSocialEventDraft PlayerbotSocialMakeDeliverySuppressionEvent(
    PlayerbotSocialPendingDelivery const& pending, PlayerbotSocialDeliveryRejection rejection,
    uint64 occurredAtUnixSeconds);

// Key Decision 5: extraction telemetry rides this event model rather than a new one.
inline constexpr std::string_view PLAYERBOT_SOCIAL_EVENT_TYPE_EXTRACTION = "social.memory.extraction";

/*
 * A conversation that supported nothing, named.
 *
 * This is the COMMONEST outcome and it is a success: most talk is not worth remembering. Without a
 * name of its own it would be written as an unqualified success indistinguishable from one that
 * stored something, and an operator could not tell "working, nothing to store" from "quietly
 * broken", which is the question a memory feature goes wrong at.
 */
inline constexpr std::string_view PLAYERBOT_SOCIAL_REASON_NOTHING_TO_REMEMBER = "nothing_to_remember";
inline constexpr std::string_view PLAYERBOT_SOCIAL_REASON_MEMORY_WRITE_REFUSED = "memory_write_refused";
inline constexpr std::string_view PLAYERBOT_SOCIAL_REASON_MEMORY_ALREADY_GONE = "memory_already_gone";
inline constexpr std::string_view PLAYERBOT_SOCIAL_REASON_MEMORY_STATE_RESET = "memory_state_reset";

/*
 * One extraction, as the feed will describe it.
 *
 * Deliberately carries NO chat. An operator needs to know that a conversation was read, by which
 * bot, on which surface, how much was in scope and what came of it, and none of those questions
 * need the words. The event table outlives the buffer's retention window by design, so putting the
 * thread in it would defeat the bound the buffer exists to enforce.
 */
struct PlayerbotSocialExtractionAttempt
{
    std::string threadPublicId;
    uint64 botGuidCounter = 0;
    PlayerbotSocialChannel channel = PlayerbotSocialChannel::General;
    std::size_t subjectCount = 0;
    std::size_t lineCount = 0;

    // Why nothing was submitted, when nothing was. `Accepted` means the request went out.
    PlayerbotSocialSnapshotRefusal refusal = PlayerbotSocialSnapshotRefusal::Accepted;

    // Whether the provider answered at all, and how many memories survived every gate.
    bool answered = false;
    std::size_t written = 0;

    uint64 occurredAtUnixSeconds = 0;
};

[[nodiscard]] PlayerbotSocialEventDraft PlayerbotSocialMakeExtractionEvent(
    PlayerbotSocialExtractionAttempt const& attempt);

inline constexpr std::string_view PLAYERBOT_SOCIAL_EVENT_TYPE_MEMORY_PERSISTENCE = "social.memory.persistence";

/*
 * What became of the cached copy when the database refused the write behind it.
 *
 * Three outcomes rather than a boolean, because the thing an operator does about them differs.
 * `Dropped` is the ordinary refusal and the only one that removed anything. `AlreadyGone` means a
 * reset or a cohort purge reached the record first, which is the same end state by another route.
 * `StaleEpoch` means the cache was erased and rebuilt between the write and its answer, so whatever
 * matches now is somebody else's record and this callback must not touch it.
 */
enum class PlayerbotSocialMemoryWriteFailureAction : uint8
{
    Dropped = 0,
    AlreadyGone,
    StaleEpoch
};

/*
 * Applies the complete world-thread response to a refused memory write. The two effects are passed
 * in because the database-backed manager cannot be constructed by the unit harness, while this
 * operation's cache, snapshot, and telemetry contract still needs one executable proof.
 * `writtenAtEpoch` fences a callback that outlived an erasure, while the record's write token
 * distinguishes fully identical writes that received different database outcomes.
 */
[[nodiscard]] PlayerbotSocialMemoryWriteFailureAction PlayerbotSocialHandleMemoryWriteFailure(
    PlayerbotSocialStateStore& state, PlayerbotSocialMemoryRecord const& record, uint64 writtenAtEpoch,
    uint64 currentEpoch, std::function<void(uint64)> const& invalidateSnapshots,
    std::function<void(PlayerbotSocialEventDraft)> const& recordEvent);

/*
 * The diagnostic Task 5 Key Decision 8 requires when the database refuses a memory the cache had
 * already accepted.
 *
 * Marked `Critical`, because an event announcing that memory persistence is failing is the last
 * thing that should be dropped when the queue is under pressure.
 */
[[nodiscard]] PlayerbotSocialEventDraft PlayerbotSocialMakeMemoryPersistenceFailureEvent(
    uint64 botGuidCounter, uint64 subjectGuidCounter, PlayerbotSocialMemoryWriteFailureAction action);

/*
 * One bot a biography may be generated for, with its identity already resolved.
 *
 * Values only, like every other structure here. The identity is read from the character on the
 * world thread and then travels as data, so nothing in the biography path holds a Player.
 */
struct PlayerbotSocialBiographyCandidate
{
    uint64 botGuidCounter = 0;
    std::string characterName;
    uint8 raceId = 0;
    uint8 classId = 0;
    uint8 genderId = 0;
};

/*
 * One profile row as it will be written.
 *
 * The state is already the schema's own spelling rather than the enum, for the reason the event
 * bindings are: a test that asserts on this reads the value the database will hold, so a producer
 * that queued the right row in the wrong state cannot pass.
 *
 * The biography itself is carried as the assembled document rather than as JSON text. Turning it
 * into JSON is the writer's job, and doing it here would put the storage format in the decision
 * path where a test would have to assert on escaped text to check a state transition.
 */
struct PlayerbotSocialProfileBinding
{
    uint64 botGuidCounter = 0;
    uint32 schemaVersion = PLAYERBOT_SOCIAL_PERSONA_VERSION;

    // Carried so an insert creates a complete row. The traits themselves are NOT written by the
    // biography path: the column belongs to the bounded evolution path, and a biography write that
    // touched it would reset traits to whatever this caller happened to be holding.
    uint32 traitsVersion = PLAYERBOT_SOCIAL_PERSONA_VERSION;
    char const* biographyState = "absent";
    uint64 biographyRequestToken = 0;
    uint64 biographyAttemptedAtUnixSeconds = 0;
    PlayerbotBiography biography;
};

struct PlayerbotSocialTraitsBinding
{
    uint64 botGuidCounter = 0;
    uint32 schemaVersion = PLAYERBOT_SOCIAL_PERSONA_VERSION;
    PlayerbotSocialTraits traits;
};

// The schema's own spelling for a biography state, or nullptr for an unrecognized one. Never
// reword an existing value: it is what the ENUM column stores.
[[nodiscard]] char const* PlayerbotSocialBiographyStateColumn(PlayerbotBiographyState state);

class PlayerbotSocialMgr
{
public:
    /*
     * One coordinator per worldserver process, reached through sPlayerbotSocialMgr.
     *
     * Every caller is on the world thread, which is what makes the unsynchronized containers below
     * safe. Nothing here may be touched from a database callback or any other thread.
     */
    static PlayerbotSocialMgr& instance()
    {
        static PlayerbotSocialMgr instance;

        return instance;
    }

    // Attributes a message to a thread, opening one when nothing fits. Returns an invalid handle for
    // an unsupported channel, without creating state for it.
    PlayerbotSocialThreadHandle Observe(PlayerbotSocialObservation const& observation);

    // When anyone last actually spoke in this scope, or zero for a scope nobody has spoken in.
    // The starter pumps anchor the ambient fill to this rather than to thread coherence stamps.
    [[nodiscard]] uint64 ScopeLastSpokenAt(PlayerbotSocialThreadKey const& key) const;

    /*
     * Takes what the quiet threads have to say, and forgets it here.
     *
     * Every thread this considers has its buffer CLEARED, accepted or refused. Two reasons, and the
     * second is the important one: a refused thread would otherwise be reconsidered on every tick
     * forever, and holding a player's words past the moment they were judged unusable serves nothing
     * this feature needs.
     *
     * Bounded per sweep, so one quiet moment on a busy realm is not hundreds of simultaneous provider
     * requests. Threads past the bound keep their buffers and are collected by a later sweep.
     */
    [[nodiscard]] std::vector<PlayerbotSocialIdleThread> CollectIdleExtractionSnapshots(uint64 nowUnixSeconds);

    /*
     * Sweeps the quiet threads and asks the provider to read them. Returns how many requests were
     * accepted, which is what the caller counts; a refusal leaves nothing outstanding.
     *
     * The lines are rendered here rather than in the provider, because the speaker NAMES have to be
     * resolved from live characters and that may only happen on the world thread. A speaker whose
     * character has gone is rendered by guid rather than dropped: removing their turn would leave
     * the others answering nobody.
     */
    std::size_t RequestIdleExtractions(uint64 nowUnixSeconds);

    /*
     * Applies one extraction answer. Returns how many memories were actually written.
     *
     * The token is the fence. An answer for a request this coordinator is not holding is discarded
     * without a write, which covers a duplicate reply, one for a thread already answered, and one
     * for a request abandoned while it was in flight.
     *
     * Every candidate is checked again here even though the sidecar checked it: the subject must
     * still be one this request named, and the scope must still be the surface it was learned on.
     * The far side is not trusted to have been correct about either, because both are the
     * difference between a memory and a leak.
     */
    std::size_t ApplyExtractedMemories(uint64 memoryRequestToken, uint64 botGuidCounter,
                                       std::string const& threadPublicId,
                                       std::vector<PlayerbotSocialExtractedMemory> const& memories);

    // Releases requests the provider never answered, so a bridge that goes away does not leave
    // tokens outstanding for the life of the process. Returns how many were abandoned.
    std::size_t AbandonStaleMemoryRequests(uint64 nowUnixSeconds);

    [[nodiscard]] std::size_t OutstandingMemoryRequestCount() const { return _memoryRequests.size(); }

    // The pressure inputs for a thread. An unknown thread reports an inert, maximally stale state so
    // a stale handle cannot read as a lively conversation.
    [[nodiscard]] PlayerbotSocialThreadPressure PressureFor(PlayerbotSocialThreadHandle const& thread,
                                                            uint64 nowUnixSeconds) const;

    [[nodiscard]] std::vector<uint64> ParticipantsOf(PlayerbotSocialThreadHandle const& thread) const;

    /*
     * Whether a thread with this public identity is still open.
     *
     * Answered from the PUBLIC identity rather than the internal id because that is what a pending
     * delivery carries across the seam: the internal id never leaves this class, while the public id
     * is what telemetry and Medivh already hold. Fails closed on an unknown, malformed or empty
     * identity, which is what makes a thread pruned while an answer was in flight read as a
     * superseded conversation rather than a live one.
     */
    [[nodiscard]] bool ThreadIsCurrent(std::string const& threadPublicId) const;

    /*
     * The conversation space a thread belongs to, by public identity.
     *
     * Delivery needs this and cannot derive it. A pending request carries the thread identity but
     * not the scope, and without the scope a party answer would be revalidated against whatever
     * group the bot is in NOW rather than the one the conversation happened in, and a say answer
     * against nowhere at all. Both are the same defect: the answer is checked against the bot's
     * present circumstances instead of the conversation's.
     *
     * Returns false for an unknown, malformed or wrong-kind identity, leaving `key` untouched.
     */
    [[nodiscard]] bool ThreadScopeFor(std::string const& threadPublicId, PlayerbotSocialThreadKey& key) const;
    [[nodiscard]] std::vector<std::string> RecentEventIdsOf(PlayerbotSocialThreadHandle const& thread) const;

    [[nodiscard]] std::size_t ActiveThreadCount(PlayerbotSocialThreadKey const& key) const;

    /*
     * How many scopes are currently tracked. `ActiveThreadCount` cannot answer this: it reports zero
     * both for a scope that was never seen and for one still held with no threads left in it, so the
     * difference between forgetting a scope and merely emptying it is invisible without this. That
     * difference is the whole memory bound on a long lived coordinator.
     */
    [[nodiscard]] std::size_t TrackedScopeCount() const;

    // How busy a scope is, on the 0..100 scale the pressure policy expects.
    [[nodiscard]] uint8 ChannelDensity(PlayerbotSocialThreadKey const& key) const;

    /*
     * Records something a bot could open a conversation about, in place of the canned line that used
     * to be broadcast. Accepts General only: that is the single destination the broadcast funnel
     * converts, so anything else arriving here is a call site that routed something it should have
     * suppressed. Returns false without storing on any refusal.
     */
    bool NoteStarterContext(PlayerbotSocialStarterContext const& starter);

    [[nodiscard]] std::vector<PlayerbotSocialStarterContext> PendingStarterContextsFor(
        PlayerbotSocialThreadKey const& key) const;

    /*
     * Takes the pending starter contexts for a scope, leaving none behind.
     *
     * Consumption is separate from ageing out. A starter that merely ages out would be reopened on
     * every tick until its window closed, spending a provider request each time to say the same
     * thing, so the tick takes what it acts on.
     */
    std::vector<PlayerbotSocialStarterContext> TakeStarterContextsFor(PlayerbotSocialThreadKey const& key);

    /*
     * The scopes holding something a bot could open a conversation about, at most `limit` of them,
     * resuming strictly AFTER `after` and wrapping once around.
     *
     * Bounded and snapshot rather than a view onto the map. The number of scopes is driven by the
     * world rather than by any cap this feature sets, so an unbounded answer would let one world tick
     * scale with the whole server.
     *
     * The cursor is what makes the bound fair, and consuming the starters is NOT a substitute for it.
     * Scanning from the beginning every tick starves the far end of an ordered map: a busy low keyed
     * zone refills its starters between ticks and takes the whole quota again, so a high keyed zone
     * can stay pending indefinitely while the queue in front of it never empties. Wrapping once means
     * excess is deferred to the next tick rather than dropped, and every scope is eventually reached.
     */
    [[nodiscard]] std::vector<PlayerbotSocialThreadKey> ScopesWithPendingStarters(
        std::size_t limit, PlayerbotSocialThreadKey const& after) const;

    /*
     * Opens or reuses a thread for a scope that is about to be started in, with no speaker.
     *
     * Deliberately NOT `Observe`. That path is driven by a message and would need a speaker; a zero
     * one would enter the participant list and count as a bot-only turn, and both feed the pressure
     * that decides whether the conversation should happen at all. Opening one would then change the
     * answer to the question it was opening it to ask.
     *
     * Records no speaker, no participant, no bot-only turn, and no observed event. What it does
     * establish is the freshness anchor: `Activate` refuses an invalid handle, and the opportunity
     * gate measures staleness from the thread's last activity, so without this every starter is
     * refused as `ThreadStale` before a single starter rule is consulted.
     */
    PlayerbotSocialThreadHandle OpenStarterThread(PlayerbotSocialThreadKey const& key, uint64 nowUnixSeconds);
    PlayerbotSocialThreadHandle OpenStarterThread(PlayerbotSocialStarterContext const& starter, uint64 nowUnixSeconds);

    // Drops threads that have gone stale. Fails closed on a clock that moved backwards: a backwards
    // step must never read as a huge elapsed time and wipe every live conversation.
    void PruneStaleThreads(uint64 nowUnixSeconds);

    // Durable consent -----------------------------------------------------------------------------

    /*
     * Begins reading a character's consent. The read is asynchronous and its result is applied on
     * the world thread, so the answer is not available when this returns. Call it at login, before
     * the character can say anything, because until it lands the character is treated as opted out.
     */
    void LoadConsent(uint64 characterGuidCounter);

    // Drops a character's loaded consent at logout, so a long lived server does not accumulate one
    // entry per character ever seen. The stored row is untouched.
    void ForgetConsent(uint64 characterGuidCounter);

    // Applies an already authoritative storage snapshot without issuing another write.
    void ApplyConsentSnapshot(uint64 characterGuidCounter, bool optedOut);

    /*
     * Records a consent change. Applied in memory immediately so the next message already obeys it,
     * and written asynchronously. The order matters: writing first and applying on the callback
     * would leave a window in which a character who just opted out is still being listened to.
     */
    void SetOptedOut(uint64 characterGuidCounter, bool optedOut);

    /*
     * Fails closed. A character whose consent has not been read yet reads as opted out, not as
     * participating, because the cost of the two mistakes is not symmetric: withholding social state
     * for a moment is a missed conversation, while assuming consent is a privacy breach.
     */
    [[nodiscard]] bool IsOptedOut(uint64 characterGuidCounter) const;
    [[nodiscard]] PlayerbotSocialMemoryInputState MemoryInputStateFor(uint64 botGuidCounter,
                                                                      PlayerbotSocialChannel channel) const;

    /*
     * Erases the character from every bot, in memory and in the database. Consent is deliberately
     * left alone, matching PlayerbotSocialResetDeletes for a SubjectCharacter reset: clearing what
     * bots remember is not a statement about whether the character wants to keep taking part.
     */
    void ResetCharacter(uint64 characterGuidCounter);

    // Durable relationships and memories -----------------------------------------------------------

    /*
     * Records that a character exists as a social actor and resolves its row id.
     *
     * Nothing else here can run until this has landed, because every durable social row is keyed by
     * actor id rather than by character GUID. Call it when a character becomes socially visible; it
     * is idempotent, and a character already resolved costs nothing.
     */
    void TouchActor(uint64 characterGuidCounter, std::string const& displayName, bool isBot);

    [[nodiscard]] bool ActorIdKnown(uint64 characterGuidCounter) const;

    // The durable actor id back to the guid counter the caches are keyed by, or zero when this
    // process has never resolved that actor.
    [[nodiscard]] uint64 ActorGuidFor(uint32 actorId) const;

    /*
     * Begins an asynchronous read of one directional pair into the snapshot.
     *
     * Does nothing while the current snapshot is still fresh, while either actor is unresolved, or
     * while either end has opted out. The answer is not available when this returns, and a caller
     * that finds no snapshot must behave as though the pair is a stranger rather than wait: a
     * database that is slow or down degrades to stateless chat, never to invented memory.
     */
    void LoadRelationship(uint64 botGuidCounter, uint64 subjectGuidCounter, uint64 nowUnixSeconds);

    /*
     * Begins one asynchronous read of the warmest durable relationships into the snapshot, once
     * per uptime, at world initialization. The whisper pump reads only the in-memory store, so
     * without this every restart hid the durable warm pairs until each happened to re-converse,
     * and a relationship-driven whisper could never open inside a fresh uptime.
     */
    void PreloadWarmRelationships();

    /*
     * Applies one preloaded pair to the snapshot, clamped. Split from the query callback so the
     * seam the whisper pump depends on is provable without a database: a pair applied here must
     * come back out of WarmRelationships. Consent KNOWN to be withdrawn refuses the apply (the
     * store enforces that itself); a pair whose consent is merely unloaded is cached but unusable,
     * because every consumer applies the fail-closed consent check before acting on it.
     */
    void ApplyPreloadedRelationship(uint64 botGuidCounter, uint64 subjectGuidCounter,
                                    PlayerbotSocialRelationshipValues const& values);

    /*
     * Begins an asynchronous read of one bot's memories, restricted to the scopes `channel` may
     * read. The restriction is carried by the choice of statement, each of which has its scope list
     * written as a literal, so no binding mistake can widen it. An invalid channel reads nothing.
     */
    void LoadMemories(uint64 botGuidCounter, PlayerbotSocialChannel channel, uint64 nowUnixSeconds);

    /*
     * Writes one directional relationship through to the database, clamped.
     *
     * Write-through rather than write-then-invalidate: the row and the snapshot are given the same
     * clamped values in the same call, so they cannot disagree, and the bot does not lose what it
     * just learned while a re-read is in flight. The snapshot still expires on its own TTL, which is
     * what bounds divergence from a writer outside this process. Returns false when consent, an
     * unresolved actor, or the store refuses the write.
     */
    bool PersistRelationship(uint64 botGuidCounter, uint64 subjectGuidCounter,
                             PlayerbotSocialRelationshipValues const& values, uint64 nowUnixSeconds);

    /*
     * Adds a delta to the stored relationship, atomically, in the database.
     *
     * The counterpart to PersistRelationship for the case where the caller knows a CHANGE rather than
     * a value. Nothing here reads the stored row first: this process does not load relationships, so
     * a value computed from its own snapshot would replace accumulated history with a number derived
     * from an empty one. The statement adds and clamps in one statement instead, which is also what
     * makes two writers safe. Returns false when consent, an unresolved actor, or the store refuses.
     */
    bool PersistRelationshipDelta(uint64 botGuidCounter, uint64 subjectGuidCounter,
                                  PlayerbotSocialRelationshipValues const& delta, uint64 nowUnixSeconds);

    /*
     * Validates, stores, and writes one memory through to the database. Returns the rejection reason
     * when it is refused, so the caller can count it by name without copying the refused text.
     */
    PlayerbotSocialMemoryRejection PersistMemory(PlayerbotSocialMemoryRecord const& record);

    /*
     * Erases the state owned by a cohort of deleted bots, in memory and in the database.
     *
     * Takes only what those bots owned, leaving what surviving bots knew about them. See
     * PlayerbotSocialStateStore::ForgetBotCohort for why that asymmetry with ResetCharacter is
     * deliberate. An empty cohort does nothing at all.
     */
    void ForgetBotCohort(std::vector<uint64> const& botGuidCounters);

    /*
     * Drives the asynchronous database work and the periodic retention purge. Must be called from
     * the world update, because the callbacks it dispatches touch the containers above.
     *
     * `diff` is the world tick in milliseconds. The purge runs on its own interval rather than every
     * tick, and deletes a bounded batch, so a large backlog is worked off gradually instead of
     * stalling a tick with one enormous statement.
     */
    void UpdateDatabaseWork(uint32 diff);

    // The administrative controls in force -----------------------------------------------------

    /*
     * What an operator has changed, or the configuration's own values when nothing has been changed
     * yet.
     *
     * Returned by value and read from the world thread. Cheap: three words and an array of four
     * bytes, so there is no reason for a caller to cache it and go stale.
     */
    [[nodiscard]] PlayerbotSocialRuntimeControl RuntimeControl() const { return _runtimeControl; }

    /*
     * Whether the stored controls have been read yet.
     *
     * Until they have, the in memory control is a default rather than an answer, and overlaying a
     * default would replace a configured density with the middle one. Callers read the configuration
     * alone in that window, which is startup, before any bot can speak.
     */
    [[nodiscard]] bool RuntimeControlLoaded() const { return _runtimeControlLoaded; }

    /*
     * Loads the stored controls and puts them into force. Called once during startup, before any bot
     * can speak.
     *
     * Synchronous on purpose, unlike everything else the manager reads. An asynchronous load would
     * leave a window in which the feature runs with the configuration's values while the operator's
     * pause is still in flight, which is exactly the window a pause exists to close. It is one row
     * read once at startup.
     */
    void LoadRuntimeControl();

    /*
     * Applies one authenticated control and persists it. World thread only.
     *
     * Persisting the whole row rather than the changed field is deliberate: the table is the
     * authority that Definition of Done 3 requires to survive a restart, and a partial write would
     * leave it describing a state the server was never in.
     */
    PlayerbotSocialControlOutcome ApplyRuntimeControl(PlayerbotSocialControlRequest const& request);

    /*
     * Queues one event for durable storage.
     *
     * The manager assigns the sequence, overwriting whatever the draft carried, so no producer can
     * write an event under an identity it chose. Nothing here touches the database: the draft is
     * validated and parked, and `UpdateDatabaseWork` performs the write on its own tick.
     *
     * A draft that does not survive validation is dropped rather than stored malformed, and does not
     * count against the queue's loss counter. Callers do not check the outcome: telemetry must never
     * be able to change what the game did.
     */
    [[nodiscard]] std::string ReserveDeliveryEventPublicId(uint64 botGuidCounter);
    [[nodiscard]] bool PrepareHumanObservation(PlayerbotSocialObservation& observation);
    void RecordHumanObservation(PlayerbotSocialObservation const& observation,
                                PlayerbotSocialThreadHandle const& thread);
    void RecordEvent(PlayerbotSocialEventDraft draft);

    /*
     * How deep the backlog is, and what it has already cost.
     *
     * Definition of Done 2 requires queue pressure to be OBSERVABLE rather than merely survivable.
     * The gap marker records it in the feed after the fact; these two report it while it is
     * happening, which is the difference between noticing a saturated queue and reading about it
     * later. Read only, and cheap enough to poll on a tick.
     *
     * Stated plainly: nothing outside the tests reads them yet. Task 11B owns the authenticated
     * telemetry surface that reports them, and putting that surface here would move its deliverable
     * into this task.
     */
    [[nodiscard]] std::size_t PendingEventCount() const { return _events.PendingCount(); }
    [[nodiscard]] uint64 LostEventCount() const { return _events.LostSinceLastDrain(); }

    /*
     * What is waiting, as the rows it will be written as.
     *
     * A count alone cannot tell one producer's event from another's, so a wiring test asserting only
     * a count passes just as happily when a producer queues the WRONG event. This returns the
     * validated bindings, where the origin, outcome and channel are already resolved to their schema
     * spellings, so an assertion reads the same values the database will hold.
     */
    [[nodiscard]] std::vector<PlayerbotSocialEventBinding> PendingEvents() const { return _events.Pending(); }

    [[nodiscard]] PlayerbotSocialStateStore const& State() const { return _state; }

    // Assistance encounters --------------------------------------------------------------------

    /*
     * Records that `helperGuidCounter` healed `beneficiaryGuidCounter` for `effectiveHealing`.
     *
     * `effectiveHealing` is what `UnitScript::OnHeal` reports, which is health actually restored, so
     * overheal arrives as zero and opens nothing. `beneficiaryHealthBeforeHeal` is what decides
     * whether this was a rescue or a top up; a caller that cannot read it passes the maximum, which
     * is the reading that earns no rescue credit.
     *
     * Guid counters and integers only. An encounter outlives the tick that opened it, so nothing
     * here may hold a `Unit*`.
     */
    void RecordAssistanceHealing(uint64 beneficiaryGuidCounter, uint64 helperGuidCounter, uint32 effectiveHealing,
                                 uint32 beneficiaryMaxHealth, uint32 beneficiaryHealthBeforeHeal,
                                 uint64 nowUnixSeconds);

    /*
     * Records that `helperGuidCounter` damaged an enemy on the beneficiary's behalf.
     *
     * `victimEngagedWithBeneficiary` is resolved live at the hook, where the enemy's target is
     * readable. Damage to something nobody was fighting is incidental and opens nothing.
     */
    void RecordAssistanceDamage(uint64 beneficiaryGuidCounter, uint64 helperGuidCounter, uint32 damage,
                                bool victimEngagedWithBeneficiary, uint64 nowUnixSeconds);

    [[nodiscard]] bool EncounterIsOpen(uint64 beneficiaryGuidCounter, uint64 helperGuidCounter) const;

    [[nodiscard]] std::size_t OpenEncounterCount() const { return _openEncounters.size(); }

    // Pairs currently holding an opposition marker. Bounded the same way an encounter is, and worth
    // reading for the same reason: an unbounded combat map is a slow leak, not a wrong answer.
    [[nodiscard]] std::size_t TrackedOppositionCount() const
    {
        std::size_t total = 0;
        for (auto const& [attacker, victims] : _appliedOpposition)
            total += victims.size();
        return total;
    }

    /*
     * Drops every open encounter, spent ledger entry, and opposition marker touching this character,
     * in both directions.
     *
     * The in-memory half of an erasure, and the half that has to be immediate. An open tally is state
     * that has not been written yet, so an erasure that leaves it behind does not hold: the sweep
     * would complete it a minute later and write a fresh relationship row for the pair the character
     * just asked to have forgotten. ResetCharacter and ForgetBotCohort both call it before they queue
     * their durable deletes.
     */
    void ForgetOpenEncountersOf(uint64 characterGuidCounter);

    // Activation ---------------------------------------------------------------------------------

    /*
     * Turns one observed message into zero or more opened requests.
     *
     * This is the seam the feature was missing. Eligibility, pressure, and responder selection were
     * all built and tested in Task 3, and nothing called any of them, so pressure accumulated in
     * threads that could never produce a request. This composes them in the one order that is
     * defensible: refuse per candidate first, then decide whether anyone answers at all, then choose
     * who, then open the requests.
     *
     * Values only, on the world thread. The density profile is the effective configuration plus
     * persisted operator control captured by the route for this activation, so a test can exercise
     * the same input the production path uses without reaching into singleton state. Nothing here
     * holds a character, so a bot that logs out between this call and its delivery is caught at the
     * delivery revalidation rather than dereferenced here.
     */
    PlayerbotSocialActivationResult Activate(PlayerbotSocialActivation const& activation,
                                             PlayerbotSocialDensityProfile densityProfile,
                                             PlayerbotSocialRoleplayDirective const& roleplay = {});

    // Roleplay assessment ------------------------------------------------------------------------

    /*
     * The single asynchronous entry before Activate, for observed human reply opportunities.
     *
     * When a provider accepts the assessment, the activation is held as an immutable snapshot until
     * the result, the timeout, or shutdown consumes it exactly once. Provider absence, provider
     * refusal, and capacity saturation all run the existing activation path immediately in ordinary
     * mode: the assessment is an upgrade path, never a gate ordinary conversation can be lost to.
     */
    PlayerbotSocialAssessmentDisposition AssessAndActivate(PlayerbotSocialActivation const& activation,
                                                           PlayerbotSocialDensityProfile densityProfile);

    /*
     * Applies one provider assessment result. Validates the token, the kind, the capability shape,
     * and that the assessed thread and line are still current, before anything is decided from it.
     * A malformed result falls back to ordinary activation; a stale one is discarded by name.
     */
    PlayerbotSocialAssessmentApplication ApplyRoleplayAssessment(PlayerbotSocialRoleplayAssessmentResult const& result);

    /*
     * Abandons assessments the provider never answered, returning their tokens. An expired
     * assessment whose thread and line are still current resumes ordinary activation, so a silent
     * sidecar costs latency rather than a conversation; a superseded one is dropped.
     */
    std::vector<uint64> ExpireTimedOutAssessments(uint64 nowUnixSeconds);

    // Drops every pending assessment without activating anything. Shutdown only.
    std::vector<uint64> CancelPendingAssessments();

    [[nodiscard]] std::size_t PendingAssessmentCount() const { return _pendingAssessments.size(); }

    /*
     * Transient per thread roleplay state. Participants are the bots an invitation authorized into
     * the thread's roleplay; opt outs are the humans whose explicit stop suppresses roleplay for
     * the remainder of the thread. Both are bounded, deduplicated, and destroyed with the thread.
     */
    bool NoteRoleplayParticipant(std::string const& threadPublicId, uint64 botGuidCounter);
    [[nodiscard]] std::vector<uint64> RoleplayParticipants(std::string const& threadPublicId) const;
    void ClearRoleplayParticipants(std::string const& threadPublicId);
    bool NoteRoleplayOptOut(std::string const& threadPublicId, uint64 humanGuidCounter);
    [[nodiscard]] bool IsRoleplayOptedOut(std::string const& threadPublicId, uint64 humanGuidCounter) const;

    // Delivery -----------------------------------------------------------------------------------

    /*
     * Registers the one optional provider, or clears it.
     *
     * Absence is a supported state, not a degraded one: with no provider the social path produces
     * silence and diagnostics, and commands and every functional Playerbot message are untouched.
     * Clearing while requests are outstanding is safe, because the coordinator abandons by token
     * rather than expecting a cancellation to be honoured.
     */
    void SetSocialProvider(PlayerbotSocialProvider* provider);
    [[nodiscard]] bool HasSocialProvider() const { return _provider != nullptr; }

    /*
     * Opens a request and returns its token, or zero with the reason it was refused.
     *
     * The token is what ties a later result back to this request. It is never reused and never zero,
     * so a result carrying a token nobody is waiting on is refused rather than matched to whatever
     * happens to occupy that slot now.
     */
    uint64 BeginSocialRequest(uint64 botGuidCounter, PlayerbotPersonalityProfile const& personality,
                              uint64 targetGuidCounter, PlayerbotSocialChannel channel,
                              std::string const& threadPublicId, PlayerbotSocialRequestPriority priority,
                              uint64 nowUnixSeconds, uint32 zoneId, std::string const& starterSubject,
                              PlayerbotSocialDeliveryRejection& rejection,
                              std::vector<PlayerbotSocialNearbySnapshotEntry> const& nearby = {});

    uint64 BeginSocialRequest(uint64 botGuidCounter, PlayerbotPersonalityProfile const& personality,
                              uint64 targetGuidCounter, PlayerbotSocialChannel channel,
                              std::string const& threadPublicId, PlayerbotSocialRequestPriority priority,
                              uint64 nowUnixSeconds, uint32 zoneId, std::string const& starterSubject,
                              PlayerbotSocialDeliveryRejection& rejection,
                              std::vector<PlayerbotSocialNearbySnapshotEntry> const& nearby, uint64 subjectGuidCounter,
                              bool addressedDirectly, PlayerbotSocialPromptLine const& currentLine = {},
                              bool statelessDirectReply = false,
                              PlayerbotRoleplayPromptMode promptMode = PlayerbotRoleplayPromptMode::Ordinary,
                              PlayerbotSocialGroundingEnvelope const& grounding = {}, bool expectsAnswer = false);

    /*
     * Takes a provider result and schedules its delivery, or refuses it with a reason.
     *
     * The shape of the output is checked here, before anything about the world is consulted, because
     * there is no point resolving characters for a result that was never deliverable. `roll` supplies
     * the variation in the natural delay.
     */
    PlayerbotSocialDeliveryRejection AcceptSocialResult(PlayerbotSocialProviderResult const& result,
                                                        uint64 nowUnixMilliseconds, uint32 roll);

    // Tokens whose delay has elapsed and which are ready to be revalidated and sent.
    [[nodiscard]] std::vector<uint64> DueDeliveries(uint64 nowUnixMilliseconds) const;

    [[nodiscard]] bool PendingDeliveryFor(uint64 requestToken, PlayerbotSocialPendingDelivery& out) const;

    /*
     * Revalidates one due delivery against the world as it is NOW and consumes it either way.
     *
     * Returns None when it may be spoken. Any other value is the reason it was dropped, and the
     * request is gone in both cases: a result is delivered once or not at all, never retried into a
     * conversation that has moved on.
     */
    PlayerbotSocialDeliveryRejection CompleteDelivery(uint64 requestToken,
                                                      PlayerbotSocialDeliveryConditions const& conditions);

    // Biography ------------------------------------------------------------------------------

    /*
     * Asks the provider for one bot's player profile and returns the token it was issued, or zero.
     *
     * The identity arrives resolved. This runs on the world thread from the coordinator lifecycle,
     * where the character is readable, and a biography is generated from authoritative character
     * data alone: no chat, no memory, and nothing a player typed. That is what makes it the one
     * request in this file with no untrusted input at all.
     *
     * Zero on every refusal, and the profile is left exactly as it was. Marking the profile before
     * the provider accepted would leave it Pending against a request nobody holds whenever the
     * provider refused, and that bot would then wait out the whole abandonment window for nothing.
     */
    uint64 RequestBiographyFor(PlayerbotSocialBiographyCandidate const& candidate, uint64 nowUnixSeconds);

    /*
     * Reads one bot's stored profile back, so a restart does not regenerate a biography it already
     * paid for. Does nothing for a bot already loaded or already decided about.
     *
     * Called at bot login beside LoadConsent. Without it the write path is a write-only log: every
     * profile would read as Absent after a restart and every bot would be generated again.
     */
    void LoadProfile(uint64 botGuidCounter);

    /*
     * Applies one generated biography to the profile that asked for it.
     *
     * `fields` are name and value pairs exactly as they arrived, unassembled, because the whitelist
     * can only be enforced while the field NAMES still exist: once a payload has been copied into a
     * typed biography the unknown names are already gone. `authoritative` is the character's real
     * identity, read from the world rather than from the payload, which carries none.
     *
     * Returns None when it was applied. Every other value is the reason it was discarded, and the
     * fence is evaluated before the payload is looked at: a completion nobody is waiting on must
     * not be able to decide anything about a profile, or a bad answer to an old request would
     * demote a finished biography.
     */
    PlayerbotBiographyCompletionRejection AcceptBiographyResult(uint64 biographyRequestToken, uint64 botGuidCounter,
                                                                std::vector<PlayerbotBiographyFieldValue> const& fields,
                                                                PlayerbotSocialBiographyCandidate const& authoritative,
                                                                uint64 nowUnixSeconds);

    /*
     * Abandons biography requests the provider never answered, returning their tokens.
     *
     * Separate from ExpireTimedOutRequests because the two expire different things under different
     * rules: a social request holds a delivery slot and a conversation that has moved on, while a
     * biography holds neither and only needs its retry opened. Without this the bounded retry
     * backoff is unreachable in production, since a lost request would otherwise sit Pending until
     * the much longer abandonment window elapsed on its own.
     */
    std::vector<uint64> ExpireTimedOutBiographyRequests(uint64 nowUnixSeconds);

    /*
     * The profile this coordinator owns for one bot.
     *
     * Returns the default profile for a bot with none, rather than an optional or a throw. Every
     * caller wants "what does this bot look like now", and a bot with no stored row looks exactly
     * like a bot with a default one: that is already how PlayerbotSocialProfileLoad degrades a
     * missing or unusable row, and disagreeing with it here would create a second answer.
     */
    [[nodiscard]] PlayerbotSocialProfile const& ProfileFor(uint64 botGuidCounter) const;

    // Health is separate from the fallback profile. A base profile is usable for composition, but
    // only a successful absent read authorizes first use; pending, rejected and unavailable reads
    // fail closed before provider admission.
    [[nodiscard]] PlayerbotSocialProfileLoad const& ProfileLoadFor(uint64 botGuidCounter) const;

    /*
     * Assembles everything one generation is allowed to know, from state this coordinator already
     * owns.
     *
     * Public so a test can assert what a bot is actually told without driving a whole activation,
     * and because the assertion that matters most here is a NEGATIVE one: that a memory learned in
     * a whisper is absent from the context built for a zone line. A rule that can only be observed
     * through a full activation is a rule whose violations are hard to see.
     *
     * Nothing here touches the world. Every input is a value this coordinator already holds or is
     * given from the persistent personality cache, which keeps it callable from a unit test and safe
     * to call while a request is being opened.
     */
    [[nodiscard]] PlayerbotSocialRequestContext ComposeRequestContext(
        uint64 botGuidCounter, PlayerbotPersonalityProfile const& personality, uint64 targetGuidCounter,
        PlayerbotSocialChannel channel, std::string const& starterSubject, uint64 nowUnixSeconds,
        std::string const& threadPublicId = {},
        std::vector<PlayerbotSocialNearbySnapshotEntry> const& nearby = {}) const;

    /*
     * Profile rows waiting to be written, as the values the row will hold.
     *
     * Readable for the reason PendingEvents is: a count cannot tell a correct write from a wrong
     * one, so a wiring test asserting only that something was queued passes just as happily when
     * the profile was persisted in the wrong state. The state is already resolved to its schema
     * spelling here, so an assertion reads what the database will.
     */
    [[nodiscard]] std::vector<PlayerbotSocialProfileBinding> PendingProfileWrites() const
    {
        return _pendingProfileWrites;
    }

    /*
     * Abandons requests a provider never answered, returning each with its reason.
     *
     * The reason travels with the token rather than being inferred from which function ran, because
     * Task 11 records a named suppression reason per request and a bare count cannot say which
     * conversation went unanswered.
     */
    std::vector<PlayerbotSocialAbandonedRequest> ExpireTimedOutRequests(uint64 nowUnixSeconds);

    /*
     * Drops every outstanding request without touching a game object.
     *
     * Definition of Done 4. At shutdown the characters these name are being removed, so the only
     * safe thing to do with a pending delivery is forget it.
     */
    std::vector<PlayerbotSocialAbandonedRequest> CancelPendingDeliveries();

    [[nodiscard]] std::size_t PendingDeliveryCount() const;

    /*
     * Closes an encounter and returns the bounded relationship delta it earned.
     *
     * Returns the neutral zero delta for a pair with no open encounter, so a completion that arrives
     * twice, or for a fight that produced nothing, is not an error. The tally is dropped either way.
     * The health scale comes from the tally, captured when the encounter opened, because by the time
     * this runs the character may be offline.
     */
    PlayerbotSocialRelationshipValues CompleteEncounter(uint64 beneficiaryGuidCounter, uint64 helperGuidCounter);

    /*
     * Closes every encounter idle for longer than PLAYERBOT_SOCIAL_ENCOUNTER_IDLE_SECONDS and applies
     * what each earned. Returns how many it closed and the total it actually applied.
     *
     * This is the production path that turns a tally into a relationship, and it is also what bounds
     * the map: a fight that simply stops, because the participants logged out or the mob despawned,
     * is completed by elapsed time rather than by an event that may never arrive. Called from
     * UpdateDatabaseWork, so it runs on the world thread like everything else that touches these
     * containers.
     *
     * The applied total is what survived the per pair window ceiling, which is not the same as what
     * the encounters were worth. Task 11 records that difference; a caller that only wants to know
     * whether work happened reads `completed`.
     */
    PlayerbotSocialEncounterSweepResult CompleteStaleEncounters(uint64 nowUnixSeconds);

    /*
     * Adds a bounded delta to a stored relationship and writes it through, and returns the part that
     * was actually admitted.
     *
     * The single production entry point for "this happened, adjust what they think of each other".
     * The returned value is what the pair's window ceiling allowed, so an encounter worth the full
     * cap can still be admitted as nothing when the pair already earned it this window. A zero return
     * therefore means "nothing was applied", never "nothing was worth applying".
     *
     * The write is an atomic addition in the database rather than a read, add, and replace here. This
     * process does not load stored relationships, so anything it computed from its own snapshot would
     * overwrite accumulated history with a value derived from an empty one.
     */
    /*
     * The server-wide provider budget. True admits the call and stamps the window; false refuses
     * it, and a sustained overrun opens the durable budget circuit as the backstop. Continuations
     * may draw the reserved bottom of the bucket; starters stop above it. World thread only, like
     * every other admission decision here.
     */
    bool AdmitProviderCall(uint64 nowUnixSeconds, bool continuation = false);

    // Opens the durable backstop: flips the runtime control's circuit, persists the circuit columns
    // (and only those), and says so loudly. Idempotent while already open.
    void OpenBudgetCircuit(std::string_view reason, uint64 nowUnixSeconds);

    /*
     * Rations relationship-driven whisper starters to one attempt per pair per cooldown window.
     * Returns true and stamps the window when the pair may attempt now. The map is transient and
     * evicts like the encounter maps do: losing a stamp costs at most one early whisper, never a
     * bound that matters.
     */
    bool NoteWhisperStarterAttempt(PlayerbotSocialRelationshipKey const& key, uint64 nowUnixSeconds,
                                   uint64 cooldownSeconds);

    /*
     * Un-stamps a pair whose check-in never opened a request (a budget refusal, a full queue), so
     * the next scan retries instead of the pair resting its whole cooldown over nothing. The stamp
     * must mean "a whisper happened", never "a whisper was considered".
     */
    void ClearWhisperStarterAttempt(PlayerbotSocialRelationshipKey const& key);

    PlayerbotSocialRelationshipValues ApplyRelationshipDelta(uint64 botGuidCounter, uint64 subjectGuidCounter,
                                                             PlayerbotSocialRelationshipValues const& delta,
                                                             uint64 nowUnixSeconds);

    /*
     * Returns what being opposed by `opponentGuidCounter` in this context costs them.
     *
     * Zero for every consented context and for an unrecognized one, which is what keeps a
     * battleground from making every bot hate half the server. Opposition is not accumulated the way
     * assistance is: it is one bounded answer per encounter, because "they attacked me" does not get
     * truer the more swings it took.
     */
    PlayerbotSocialRelationshipValues RecordPvpOpposition(uint64 victimGuidCounter, uint64 opponentGuidCounter,
                                                          PlayerbotSocialCombatContext context, uint64 nowUnixSeconds);

private:
    [[nodiscard]] PlayerbotSocialRequestContext ComposeRequestContextForSubject(
        uint64 botGuidCounter, PlayerbotPersonalityProfile const& personality, uint64 subjectGuidCounter,
        PlayerbotSocialChannel channel, std::string const& starterSubject, uint64 nowUnixSeconds,
        std::string const& threadPublicId, std::vector<PlayerbotSocialNearbySnapshotEntry> const& nearby,
        bool addressedDirectly, PlayerbotSocialPromptLine const& currentLine, bool statelessDirectReply,
        PlayerbotRoleplayPromptMode promptMode = PlayerbotRoleplayPromptMode::Ordinary) const;

    struct Thread
    {
        uint64 threadId = 0;
        std::string publicId;
        std::string sourceEventPublicId;
        std::string rootSubject;
        uint64 lastActivityUnixSeconds = 0;
        uint32 consecutiveBotOnlyTurns = 0;
        uint32 relevantHumanMessages = 0;
        std::deque<uint64> participants;
        std::deque<std::string> recentEventIds;

        // Who spoke last, for the conversation relationship credit: an observed turn pairs its
        // speaker with the previous one. Zero until the thread has heard anyone.
        uint64 lastSpeakerGuidCounter = 0;
        bool lastSpeakerWasHuman = false;

        /*
         * Recent lines, as hashes of their normalized text rather than the text itself.
         *
         * A hash is all duplicate detection needs, since the question is equality and never content,
         * so this half retains nothing readable on any surface including whispers.
         *
         * Privacy scope needs no separate rule. A thread lives under a scope keyed BY channel, so
         * every line compared here was said on the same surface by construction, and a whisper can
         * never be matched against zone General.
         *
         * Retention needs no separate timer either. These die with the thread, which `PruneStaleThreads`
         * already drops on the staleness window. That window is far tighter than the durable memory
         * retention floor, so this is the stricter of the two bounds rather than an exemption.
         */
        std::deque<uint64> recentLineHashes;
        std::deque<uint64> recentGeneratedLineHashes;

        /*
         * Who said a generated line and what it was doing, in order. The duplicate-function rail
         * compares BOTH: one bot repeating its own function on consecutive generated lines is a
         * monologue and refused, while two bots exchanging the same function is a conversation
         * and delivers.
         */
        struct GeneratedContribution
        {
            uint64 speakerGuidCounter = 0;
            PlayerbotSocialContributionFunction function = PlayerbotSocialContributionFunction::None;
        };
        std::deque<GeneratedContribution> recentContributionFunctions;

        /*
         * Raw chat, for idle memory extraction only, and the ONE place this class holds any.
         *
         * It exists because extraction fires at an idle boundary: a thread is eligible precisely
         * because nobody has spoken for the staleness window, so the words must have been kept from
         * the moment they arrived. The narrowings that make that acceptable are enforced by the
         * buffer itself rather than here, and two are worth naming at the member: a WHISPER is never
         * buffered on any terms, so the private surfaces still leave nothing readable behind, and a
         * human's line is kept only while they consent, with an opt out purging what was held.
         */
        PlayerbotSocialExtractionBuffer extraction;

        // Raw chat retained only for immediate generation context. Separate from extraction because
        // the purposes, whisper rules, and retention lifecycles are different.
        PlayerbotSocialPromptContextBuffer promptContext;

        /*
         * Transient roleplay state. Bots an invitation authorized into this thread's roleplay, and
         * humans whose explicit opt out suppresses roleplay here. Bounded, deduplicated, never
         * persisted, and destroyed with the thread by the same pruning that owns everything else.
         */
        std::deque<uint64> roleplayParticipants;
        std::deque<uint64> roleplayOptOuts;
    };

    /*
     * One conversation space's whole state. Threads and starter contexts share the entry because they
     * share a lifetime: a scope is forgotten only once it holds neither, so a zone that still has a
     * pending thing to talk about is not dropped just because nobody has spoken in it yet.
     */
    struct Scope
    {
        std::vector<Thread> threads;
        std::deque<PlayerbotSocialStarterContext> starters;

        /*
         * When anyone last actually said something here. The ambient starter fill anchors to this,
         * NOT to a thread's coherence stamp: the starter pump touches threads on every pass, so a
         * thread-based anchor never accumulates idle time and pins starter pressure at the floor.
         * Zero means nobody has spoken since this scope was created, which the fill reads as
         * maximally quiet, exactly right for bootstrapping a silent world.
         */
        uint64 lastSpokenAtUnixSeconds = 0;

        [[nodiscard]] bool Empty() const { return threads.empty() && starters.empty(); }
    };

    [[nodiscard]] Thread const* FindThread(uint64 threadId) const;
    [[nodiscard]] Thread const* FindThread(std::string const& threadPublicId) const;

    // Drops every snapshot stamp this character appears in, on either side, so the next opportunity
    // re-reads instead of being told a snapshot from before an erasure is still fresh.
    void ForgetSnapshotsOf(uint64 characterGuidCounter);

    /*
     * Drops every line this character spoke, from every thread on the server.
     *
     * The purge half of consent. Called wherever the answer stops being yes, which is an opt out, a
     * consent read being discarded, and a durable reset. Other participants keep their lines: they
     * consented, and dropping theirs as well would be a second privacy decision on their behalf.
     */
    void ForgetBufferedChatOf(uint64 characterGuidCounter);

    /*
     * One extraction this coordinator is waiting on.
     *
     * The subjects are held rather than re-derived because the thread they came from is cleared the
     * moment the request goes out, and re-deriving them from a thread that has since moved on would
     * check the answer against a different conversation.
     */
    struct OutstandingExtraction
    {
        uint64 botGuidCounter = 0;
        std::string threadPublicId;
        PlayerbotSocialPrivacyScope scope = PlayerbotSocialPrivacyScope::Public;
        std::vector<uint64> subjects;
        std::vector<PlayerbotSocialBufferedLine> sources;
        uint64 issuedAtUnixSeconds = 0;
    };

    std::map<uint64, OutstandingExtraction> _memoryRequests;

    /*
     * One assessment this coordinator is waiting on. The activation snapshot is immutable values
     * only, so nothing here can dangle while the sidecar thinks; the line hash is what proves the
     * conversation has not moved on before a late answer is applied.
     */
    struct PendingRoleplayAssessment
    {
        PlayerbotSocialActivation activation;
        PlayerbotSocialDensityProfile densityProfile = PlayerbotSocialDensityProfile::Normal;
        std::string threadPublicId;
        uint64 currentLineHash = 0;
        uint64 issuedAtUnixSeconds = 0;
    };

    /*
     * Whether the assessed thread still exists and still ends on the assessed line. The named
     * discard reason for the failing case, so a stale answer is explainable in diagnostics.
     */
    [[nodiscard]] PlayerbotSocialRoleplayAssessmentDiscard AssessmentStaleness(
        PendingRoleplayAssessment const& pending) const;

    // The bounded, consent-filtered thread lines the classifier may see, and the worldserver's own
    // indicator scan reads. One producer, so the two cannot see different conversations.
    [[nodiscard]] std::vector<std::string> AssessmentThreadLines(std::string const& threadPublicId,
                                                                 uint64 nowUnixSeconds) const;

    /*
     * The trusted prompt mode one selected responder speaks under, from the directive, the bot's
     * stable affinity band, its deterministic willingness roll, and the thread's participant set.
     */
    [[nodiscard]] PlayerbotRoleplayPromptMode RoleplayPromptModeFor(PlayerbotSocialActivation const& activation,
                                                                    PlayerbotSocialRoleplayDirective const& roleplay,
                                                                    uint64 botGuidCounter,
                                                                    uint8 roleplayAffinity) const;

    [[nodiscard]] Thread* FindThreadMutable(std::string const& threadPublicId);

    std::map<uint64, PendingRoleplayAssessment> _pendingAssessments;
    uint64 _nextAssessmentToken = 1;

    // Just this bot's memory stamps, across every scope query. Used after a memory write, which
    // changes nothing about any relationship and so has no business forcing those to be re-read.
    void ForgetMemorySnapshotsOf(uint64 botGuidCounter);

    /*
     * Makes any outstanding read for one pair obsolete, after a write that read cannot have seen.
     *
     * Narrower than the epoch on purpose. A relationship write concerns one pair, and advancing the
     * epoch would void every unrelated read on the realm to protect it.
     */
    void InvalidateRelationshipRead(PlayerbotSocialRelationshipKey const& key);

    /*
     * Whether durable state may be read or written for this pair. The single fail-closed consent
     * gate for the persistence path: it consults this class's IsOptedOut rather than the store's,
     * because the store answers "not opted out" for a character whose consent was never read, which
     * is everyone offline.
     */
    [[nodiscard]] bool PairMayBeStored(uint64 botGuidCounter, uint64 subjectGuidCounter) const;

    std::map<PlayerbotSocialThreadKey, Scope> _scopes;
    uint64 _nextThreadId = 1;
    uint64 _nextEventSequence = 1;

    PlayerbotSocialStateStore _state;

    // Which characters' consent has actually been read. Separate from the store because the store
    // cannot distinguish "participates" from "not asked yet", and that difference is what makes
    // IsOptedOut able to fail closed.
    std::set<uint64> _consentLoaded;

    /*
     * The read that is currently authoritative for each character's consent, and the counter it is
     * drawn from.
     *
     * A consent read is asynchronous, and anything that changes consent while one is in flight makes
     * that read obsolete rather than merely old. Without this, the sequence "log in, opt out, the
     * login read lands" ends with the character opted back in. Each issue of a read or a change takes
     * the next token; a callback applies its result only if its token is still the current one.
     *
     * The counter is global rather than per character so that a reconnect cannot reuse a token a
     * previous session's in-flight read is still carrying.
     */
    std::map<uint64, uint64> _consentToken;
    uint64 _nextConsentToken = 1;

    // Character GUID counter to durable actor row id, and back. The reverse direction is what lets a
    // memory row's subject actor be named as a character; a row whose subject cannot be mapped is
    // skipped rather than guessed at.
    std::map<uint64, uint32> _actorIds;
    std::map<uint32, uint64> _actorGuids;
    std::set<uint64> _actorLookupPending;

    // When each snapshot was read. A pair or a bot absent here has no snapshot at all, which reads as
    // a stranger with nothing remembered rather than as an error.
    std::map<PlayerbotSocialRelationshipKey, uint64> _relationshipSnapshotAt;

    /*
     * Keys with a read already outstanding, and whether something invalidated that read after it
     * went out.
     *
     * PRESENCE is the in flight marker. The freshness stamp is written by the CALLBACK, so it
     * cannot suppress a second read issued while the first is still in flight. Two overlapping
     * reads for one key can land in either order, and the older answer would then overwrite the
     * newer one. Holding at most one read per key removes the reordering rather than trying to
     * resolve it: with nothing to race against, the last answer applied is always the last one
     * asked for.
     *
     * The FLAG is what a write or an erasure sets so the answer already in the air is discarded
     * rather than applied over it. It is a flag rather than a counter precisely because presence
     * bounds the map to one entry per outstanding read: only one read exists per key at a time, so
     * there is never a second one to tell this answer apart from.
     *
     * Erased by the callback on EVERY path, including both fences. An entry left behind by a
     * discarded answer would silence that key's reads for the life of the process, and the map
     * would grow one entry per key ever read rather than one per read in flight.
     */
    std::map<PlayerbotSocialRelationshipKey, bool> _relationshipLoadsInFlight;

    /*
     * Open assistance encounters, keyed by beneficiary and helper in that order.
     *
     * Directional on purpose: what someone did for a bot is not evidence of what the bot did for
     * them. An absent key is an encounter that never opened, which is why an event that earns
     * nothing is dropped at the door rather than stored as an empty tally.
     *
     * SHORTCUT: healing and damage only. Buffs, dispels, crowd control, taunts, and resurrection are
     * real assistance this cannot see, because none of them passes through OnHeal or OnDamage.
     * Upgrade trigger: when the aura and spell cast hooks are wired for Task 7's delivery path, feed
     * them into the same tally rather than adding a second accumulator.
     */
    std::map<PlayerbotSocialRelationshipKey, PlayerbotSocialAssistanceTally> _openEncounters;

    /*
     * What each pair has already been credited inside its current window.
     *
     * The per encounter caps do not bound a pair on their own, because the sweep decides an encounter
     * ended by elapsed idle time rather than by an authoritative combat end, so one long fight with
     * sparse healing becomes several encounters and each would pay the ceiling again. This is what
     * makes the ceiling hold whatever the encounter boundaries turn out to be.
     *
     * Pruned by the sweep once a window has fully elapsed, so it does not become one entry per pair
     * for the rest of the uptime.
     */
    std::map<PlayerbotSocialRelationshipKey, PlayerbotSocialAssistanceCredit> _assistanceCredit;

    // Last relationship-driven whisper attempt per pair. Transient and evicting: see
    // NoteWhisperStarterAttempt.
    std::map<PlayerbotSocialRelationshipKey, uint64> _whisperStarterAttempts;

    // The sliding-window provider budget ledger AdmitProviderCall rules from.
    PlayerbotSocialProviderBudgetState _providerBudget;

    // Hostile-line tallies per (abused bot, category). Transient and evicting like the whisper
    // stamps: losing one costs a campaign a fresh count, never a durable record.
    std::map<std::pair<uint64, PlayerbotSocialModerationCategory>, PlayerbotSocialModerationTally> _moderationTallies;

    /*
     * One classified hostile line aimed at a bot. Tallies it, and at the category's opening
     * threshold opens or bumps the durable moderation case with bounded evidence. The subject is
     * the bot the line was aimed at; the speaker is who said it.
     */
    void NoteHostileLine(uint64 subjectGuidCounter, uint64 speakerGuidCounter,
                         PlayerbotSocialModerationCategory category, std::string const& text, uint64 nowUnixSeconds);

    /*
     * When each pair's opposition was last answered, so a fight costs the attacker once rather than
     * once per swing. Expired by the same idle threshold an encounter uses, so a second fight later
     * is opposed again. Pruned by the sweep and cleared by an erasure.
     *
     * Nested by attacker rather than flat, because the bound has to be per attacker: one player who
     * fights enough distinct bots would otherwise exhaust a shared ceiling and leave everyone else
     * unattributed. The nesting makes that structural, so the two caps multiply to the memory bound
     * with no running total to keep in step.
     */
    std::map<uint64, std::map<uint64, uint64>> _appliedOpposition;

    /*
     * Outstanding social requests, nested by bot.
     *
     * Nested for the same reason the opposition markers are: a single shared ceiling is a starvation
     * surface, and one bot in a very busy conversation could otherwise fill it and silence every
     * other bot on the realm. The two caps multiply to the memory bound, structurally.
     */
    std::map<uint64, std::map<uint64, PlayerbotSocialPendingDelivery>> _pendingDeliveries;

    // Never reused and never zero, so a stale result cannot be matched to a live request that
    // happens to occupy the same slot now.
    uint64 _nextRequestToken = 1;

    // Not owned. Absence is a supported state.
    PlayerbotSocialProvider* _provider = nullptr;

    /*
     * The profiles this coordinator owns, by bot GUID counter.
     *
     * Sparse on purpose: an entry appears when a bot first has a biography requested for it or a
     * stored row is loaded, not for every bot on the realm. A bot with no entry reads as the
     * default profile, which is the same thing a missing row already degrades to.
     */
    std::map<uint64, PlayerbotSocialProfile> _profiles;
    std::map<uint64, PlayerbotSocialProfileLoad> _profileLoads;

    // Which bot each outstanding biography token belongs to, and when it was issued. Separate from
    // the profile so an answer can be matched by token alone, without a scan.
    struct OutstandingBiography
    {
        uint64 botGuidCounter = 0;
        uint64 requestedAtUnixSeconds = 0;
    };

    std::map<uint64, OutstandingBiography> _biographyRequests;

    // Never reused and never zero, and drawn from its own sequence rather than the social one so a
    // biography token can never collide with a chat request token on the same bridge.
    uint64 _nextBiographyRequestToken = 1;

    // Its own sequence for the same reason, so an extraction token can never be mistaken for a
    // biography or a chat request on the same bridge.
    uint64 _nextMemoryRequestToken = 1;

    /*
     * How long an unanswered extraction is held before its token is released.
     *
     * Generous, because nothing is waiting on the answer and a slow one is still the right one: the
     * conversation is over and its buffer is already cleared, so there is nothing to be stale
     * against. This exists so a bridge that goes away does not leave tokens outstanding for the
     * life of the process, not to time a request out on quality grounds.
     */
    static constexpr uint64 MEMORY_REQUEST_TIMEOUT_SECONDS = 600;

    std::vector<PlayerbotSocialProfileBinding> _pendingProfileWrites;
    std::vector<PlayerbotSocialTraitsBinding> _pendingTraitsWrites;

    // Records the profile as it now stands so UpdateDatabaseWork can write it. Replaces any earlier
    // pending write for the same bot: the row is the whole state, so an older one is not a change
    // to apply first, it is a value that was already superseded.
    void QueueProfileWrite(uint64 botGuidCounter, PlayerbotSocialProfile const& profile);

    void QueueTraitsWrite(uint64 botGuidCounter, PlayerbotSocialProfile const& profile);

    // Issues the queued profile rows. Called from UpdateDatabaseWork, on the world thread, like
    // everything else that reads `_actorIds`.
    void FlushProfileWrites();

    // Counts down to the next encounter sweep. The idle threshold is measured in whole minutes, so
    // scanning the map on every world update would be re-deciding the same answer thousands of times
    // between the two ticks where it can change.
    uint32 _millisecondsSinceEncounterSweep = 0;

    /*
     * Keyed by bot AND by the scope query, not by bot alone.
     *
     * Each channel reads a different set of scopes, so one stamp per bot would let a General read,
     * which returns public memory only, mark the bot fresh and silence the party and whisper reads
     * for the rest of the interval. The bot would then appear to have forgotten everything private
     * it knew, for as long as the snapshot lasted.
     */
    std::map<std::pair<uint64, PlayerbotSocialMemoryScopeQuery>, uint64> _memorySnapshotAt;

    /*
     * The same in-flight guard and invalidation flag as _relationshipLoadsInFlight, keyed by bot and
     * scope query.
     *
     * The guard matters more here: a memory read is applied as a whole-snapshot REPLACEMENT, so an
     * older answer landing second does not merely age one value, it reinstates a set of memories
     * that a newer read had already replaced.
     *
     * The flag covers the narrower case the epoch must not be spent on. A single memory write
     * invalidates one bot's snapshot, and a read issued before that write would otherwise land
     * afterwards, replace the set without the new memory, and stamp itself fresh, so a bot would
     * forget what it just learned for a whole snapshot lifetime. Bumping the epoch for one write
     * would void every unrelated read on the realm instead, which is the wrong price.
     */
    std::map<std::pair<uint64, PlayerbotSocialMemoryScopeQuery>, bool> _memoryLoadsInFlight;
    std::map<std::pair<uint64, PlayerbotSocialMemoryScopeQuery>, PlayerbotSocialMemoryInputState> _memoryInputStates;

    /*
     * Characters whose reset has been requested but whose deletes have not been issued yet.
     *
     * The deletes cannot go out until an asynchronous lookup resolves the actor id, and a write
     * accepted in that window would be erased by deletes that were decided before it existed.
     * Refusing durable writes for the character until the reset completes is the fail closed answer,
     * and it matches what the player asked for: nothing new is remembered while being forgotten.
     */
    std::set<uint64> _resetPending;

    /*
     * The controls in force. Seeded from the configuration at construction so the feature behaves as
     * configured before LoadRuntimeControl has run, and replaced wholesale by the stored row if
     * there is one.
     */
    PlayerbotSocialRuntimeControl _runtimeControl;

    // False until LoadRuntimeControl has run, so a default is never mistaken for a stored answer.
    bool _runtimeControlLoaded = false;

    // One warm-relationship preload per uptime; the flag makes a repeated world-init call free.
    bool _warmRelationshipPreloadIssued = false;

    // Writes the whole control row. Split out only because both the apply path and the startup path
    // that has to create a first row need it.
    void PersistRuntimeControl();

    uint64 _nextMemorySequence = 1;

    /*
     * Events waiting to be written.
     *
     * Bounded, so a burst costs a counted, visible gap rather than an unbounded backlog on a tick
     * that is already the world's critical path.
     *
     * Identities come from the existing `_nextEventSequence` rather than from a counter of this
     * queue's own. Both draw on the same event id namespace, so a second counter would mint public
     * ids that collide with the ones the in memory thread history already hands out.
     */
    PlayerbotSocialEventQueue _events;
    PlayerbotSocialEventPersistenceTracker _eventPersistence;

    // Drains the event queue and issues one observed transaction. Called from UpdateDatabaseWork,
    // on the world thread, like everything else that reads `_actorIds`.
    void FlushEvents();

    /*
     * Bumped by anything that erases durable state: a character reset, or a deleted bot cohort.
     *
     * A read issued before the erasure is still in flight afterwards, and applying its result would
     * restore exactly what was just deleted. Each read captures the epoch it was issued under and
     * discards itself if the epoch has moved, which makes an erasure win over every read that could
     * not have seen it.
     *
     * One counter for the whole manager rather than one per pair. It discards more reads than
     * strictly necessary, but erasure is rare and a discarded read costs one refresh, while a
     * per-key scheme has more ways to miss a case that matters.
     */
    uint64 _stateEpoch = 1;

    uint32 _millisecondsSincePurge = 0;
};

#define sPlayerbotSocialMgr PlayerbotSocialMgr::instance()

#endif  // PLAYERBOTS_PLAYERBOTSOCIALMGR_H
