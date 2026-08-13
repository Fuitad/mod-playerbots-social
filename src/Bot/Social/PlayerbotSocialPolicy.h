/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef PLAYERBOTS_PLAYERBOTSOCIALPOLICY_H
#define PLAYERBOTS_PLAYERBOTSOCIALPOLICY_H

#include <cstddef>
#include <deque>
#include <string>
#include <string_view>
#include <vector>

#include "Bot/Social/PlayerbotSocialTypes.h"

/*
 * Pure social policy. Every function here is a total function of its arguments: no clock, no
 * database, no game state, no randomness. That is what makes the coefficients below testable as
 * exact values and what lets the coordinator call them from the world thread without cost.
 *
 * Named policy values are deliberate. Values backed by live evidence are calibrated through the
 * plan rather than frozen as product level caps. Operator density multipliers are separately marked
 * as initial configurable defaults until Task 16 has live evidence. Every value is covered by a
 * boundary test.
 */

// Bumped whenever a coefficient or threshold below changes, so persisted decisions and telemetry
// can be attributed to the policy revision that produced them.
inline constexpr uint32 PLAYERBOT_SOCIAL_POLICY_VERSION = 2;

// Ambient events remain a Social starter source even when the legacy canned broadcast system is
// disabled. When both systems are off, the event producers keep their historical early exit.
[[nodiscard]] bool PlayerbotSocialAmbientSourceEnabled(bool legacyBroadcastsEnabled, bool socialChatEnabled);

// Social owns starter sampling once it is enabled. Otherwise the typed event keeps the exact legacy
// canned broadcast chance, including zero as disabled and equality as admitted.
[[nodiscard]] bool PlayerbotSocialAmbientSourcePassesChance(bool socialChatEnabled, uint32 legacyChance, uint32 roll);

// Social traits and the disposition score share this range with the base personality scores.
inline constexpr uint8 PLAYERBOT_SOCIAL_TRAIT_MIN = 0;
inline constexpr uint8 PLAYERBOT_SOCIAL_TRAIT_MAX = 100;

// How willing a bot is to engage one specific subject right now. Directional: the stance a bot
// holds toward a character says nothing about the stance that character's bot holds in return.
enum class PlayerbotSocialStance : uint8
{
    Dismissive = 0,  // Actively negative. May refuse or brush the subject off, in character.
    Reserved,        // Quiet or distant. Usually says nothing, without any hostility.
    Neutral,
    Receptive,
    Engaged  // Familiar and well disposed. Most likely to answer.
};

/*
 * Weights applied to the directional relationship when scoring engagement.
 *
 * The negative affinity weight is deliberately larger than the positive one. Dislike suppresses
 * conversation faster than liking encourages it, so a bot that has a reason to dislike someone
 * does not keep chatting simply because its base personality is talkative.
 */
inline constexpr float PLAYERBOT_SOCIAL_AFFINITY_POSITIVE_WEIGHT = 30.0f;
inline constexpr float PLAYERBOT_SOCIAL_AFFINITY_NEGATIVE_WEIGHT = 70.0f;
inline constexpr float PLAYERBOT_SOCIAL_TRUST_WEIGHT = 15.0f;
inline constexpr float PLAYERBOT_SOCIAL_FAMILIARITY_WEIGHT = 15.0f;

/*
 * At or below either threshold the bot holds an actively negative view and its stance is
 * Dismissive no matter how high its disposition score is. Hostility is a separate axis from
 * talkativeness: a chatty bot that dislikes someone declines, and a shy bot that dislikes nobody
 * is merely Reserved.
 */
inline constexpr float PLAYERBOT_SOCIAL_HOSTILE_AFFINITY = -0.5f;
inline constexpr float PLAYERBOT_SOCIAL_HOSTILE_TRUST = -0.5f;

// Lower bound of each non hostile stance band, expressed on the 0..100 disposition scale.
inline constexpr uint8 PLAYERBOT_SOCIAL_STANCE_NEUTRAL_MIN = 25;
inline constexpr uint8 PLAYERBOT_SOCIAL_STANCE_RECEPTIVE_MIN = 50;
inline constexpr uint8 PLAYERBOT_SOCIAL_STANCE_ENGAGED_MIN = 75;

/*
 * Bounded trait evolution.
 *
 * A social trait may move at most one step per accepted proposal, and at most one proposal may be
 * accepted per interval. Together these make an abrupt personality rewrite structurally impossible:
 * a model returning an absurd delta still only nudges the bot, and a burst of messages in one
 * conversation cannot compound into a visible change.
 */
inline constexpr int32 PLAYERBOT_SOCIAL_TRAIT_MAX_STEP = 2;
inline constexpr uint64 PLAYERBOT_SOCIAL_TRAIT_MIN_INTERVAL_SECONDS = 3600;

/*
 * Scores how willing this bot is to engage this subject, on 0..100.
 *
 * Trait arguments outside 0..100 and relationship values that are out of range or not finite are
 * brought back into range before any arithmetic, so a corrupt stored row cannot produce an out of
 * range score or poison the result with a NaN.
 */
[[nodiscard]] uint8 PlayerbotSocialEngagementDisposition(uint8 sociability, uint8 warmth,
                                                         PlayerbotSocialRelationshipValues const& toward);

// Classifies a disposition score into a stance. The relationship is consulted separately so that
// hostility can override an otherwise high score.
[[nodiscard]] PlayerbotSocialStance PlayerbotSocialStanceFor(uint8 disposition,
                                                             PlayerbotSocialRelationshipValues const& toward);

[[nodiscard]] char const* PlayerbotSocialStanceName(PlayerbotSocialStance stance);

/*
 * Checkable for the same reason the channel is: this build has neither -Wswitch nor -Werror, so a
 * stance cast in from a corrupt row or a payload reaches a consumer unchallenged. A consumer that
 * only tests for one specific stance would treat every unknown value as ordinary, which fails open.
 */
[[nodiscard]] inline constexpr bool PlayerbotSocialStanceIsValid(PlayerbotSocialStance stance)
{
    switch (stance)
    {
        case PlayerbotSocialStance::Dismissive:
        case PlayerbotSocialStance::Reserved:
        case PlayerbotSocialStance::Neutral:
        case PlayerbotSocialStance::Receptive:
        case PlayerbotSocialStance::Engaged:
            return true;
    }

    return false;
}

/*
 * Transient context, applied on top of the persisted disposition.
 *
 * Mood is a short lived value that never reaches storage, and being addressed by name is the
 * strongest ordinary reason for a bot to answer. Both are bounded so that context can colour a
 * reply without ever overriding what the relationship says.
 */
inline constexpr uint8 PLAYERBOT_SOCIAL_MOOD_NEUTRAL = 50;
inline constexpr int32 PLAYERBOT_SOCIAL_DIRECT_ADDRESS_BONUS = 15;
inline constexpr int32 PLAYERBOT_SOCIAL_MOOD_DIVISOR = 5;

[[nodiscard]] uint8 PlayerbotSocialApplyContextToDisposition(uint8 disposition, bool addressedDirectly, uint8 mood);

/*
 * Bounded evolving interests and aversions. These are stored beside the social traits and kept
 * deliberately separate from the biography: what a character has picked up over time is not part of
 * the player profile it was created with, and enriching one must never rewrite the other.
 */
inline constexpr std::size_t PLAYERBOT_SOCIAL_MAX_EVOLVING_TOPICS = 8;
inline constexpr std::size_t PLAYERBOT_SOCIAL_MAX_TOPIC_LENGTH = 32;

// Adds one topic if there is room and it is usable. Returns false when the topic is empty, longer
// than the bound, already present, or the list is full, so a stream of proposals cannot grow the
// row without limit or fill it with repetitions of a single interest.
[[nodiscard]] bool PlayerbotSocialAddEvolvingTopic(std::vector<std::string>& topics, std::string const& topic);

// Normalizes a stored topic list to the bound above. A row is not written through the add path, so
// loading has to apply the same rule: empty and oversized entries are dropped, duplicates are
// collapsed, order is preserved, and the result is truncated to the cap.
[[nodiscard]] std::vector<std::string> PlayerbotSocialBoundEvolvingTopics(std::vector<std::string> const& topics);

// Applies one accepted trait proposal. The delta is clamped to a single step and the result to the
// trait range, so this is safe to call with any value a proposal or a corrupt row can carry.
[[nodiscard]] uint8 PlayerbotSocialEvolveTrait(uint8 current, int32 proposedDelta);

// Whether enough time has passed since the last accepted proposal. Fails closed on a clock that
// moved backwards, which would otherwise read as a very old last change and let a burst through.
[[nodiscard]] bool PlayerbotSocialTraitEvolutionIsDue(uint64 lastAcceptedAtUnixSeconds, uint64 nowUnixSeconds);

struct PlayerbotSocialTraitEvolution
{
    bool applied = false;
    uint8 value = 0;
    uint64 lastAcceptedAtUnixSeconds = 0;
};

/*
 * The entry point a caller should use. Combining the interval check with the bounded step in one
 * operation is what actually enforces slow evolution: keeping them apart leaves the step callable
 * repeatedly inside a single conversation, which is exactly the abrupt rewrite the bound exists to
 * prevent. When the interval has not elapsed the value and the timestamp are returned untouched.
 */
[[nodiscard]] PlayerbotSocialTraitEvolution PlayerbotSocialEvolveTraitIfDue(uint8 current, int32 proposedDelta,
                                                                            uint64 lastAcceptedAtUnixSeconds,
                                                                            uint64 nowUnixSeconds);

// Opportunity eligibility --------------------------------------------------------------------------

/*
 * How long a thread may sit idle and still be continued, and how long a bot must wait after speaking
 * before it may speak again. Both are named policy values calibrated against live telemetry rather
 * than product level caps, and both are inclusive bounds: elapsed time equal to the value passes.
 */
inline constexpr uint64 PLAYERBOT_SOCIAL_THREAD_STALE_SECONDS = 300;
inline constexpr uint64 PLAYERBOT_SOCIAL_REPLY_COOLDOWN_SECONDS = 45;
inline constexpr uint32 PLAYERBOT_SOCIAL_MAX_CONSECUTIVE_BOT_TURNS = 2;

/*
 * Why an opportunity was refused before any provider request existed. Every one of these is reported
 * by name in telemetry and in the Medivh suppression feed, so the reason is part of the contract and
 * not a private detail of the gate.
 */
enum class PlayerbotSocialOpportunityRejection : uint8
{
    None = 0,
    UnsupportedChannel,
    StarterNotAllowedOnChannel,
    SelfReply,
    StarterSourceMismatch,
    InitiationOptedOut,
    SpeakerOptedOut,
    FactionMismatch,
    LanguageMismatch,
    ThreadStale,
    CooldownActive,
    DuplicateSuppressed,
    BotOnlyTurnLimit,
    ProfilePending,
    ProfileRejected,  // Retained for durable telemetry; the gate no longer produces it, because a
                      // rejected row already degraded to a usable base-personality profile at load.
    ProfileUnavailable
};

// Bump when an enumerator is added above, for the same reason as the suppression count below.
inline constexpr std::size_t PLAYERBOT_SOCIAL_OPPORTUNITY_REJECTION_COUNT = 16;

[[nodiscard]] char const* PlayerbotSocialOpportunityRejectionName(PlayerbotSocialOpportunityRejection rejection);

/*
 * A normalized opportunity. Every field is an immutable value captured on the world thread: no
 * pointers, no handles, nothing that can dangle if the character logs out while this is in flight.
 */
struct PlayerbotSocialOpportunity
{
    PlayerbotSocialChannel channel = PlayerbotSocialChannel::General;
    bool starter = false;
    bool speakerIsHuman = false;
    bool speakerOptedOut = false;
    bool botOptedOutOfInitiation = false;
    bool factionMatches = true;
    bool languageMatches = true;
    uint64 threadLastActivityUnixSeconds = 0;
    uint64 botLastSpokeUnixSeconds = 0;
    uint64 nowUnixSeconds = 0;
    bool duplicateOfRecentMessage = false;
    uint32 consecutiveBotOnlyTurns = 0;

    // The turn cap rides on the opportunity so the autonomous stage can raise it per activation
    // while every earlier stage keeps the default. Zero is treated as the default, not "no turns".
    uint32 maxConsecutiveBotOnlyTurns = PLAYERBOT_SOCIAL_MAX_CONSECUTIVE_BOT_TURNS;

    // True only for the relationship-driven whisper check-in, the one whisper a bot may open. Every
    // other whisper starter stays refused, keeping the channel reactive by default.
    bool relationshipDriven = false;
    PlayerbotSocialProfileLoadState profileLoadState = PlayerbotSocialProfileLoadState::Pending;

    /*
     * Reply cooldown carried per activation, seconds. Zero means the built-in cooldown. The
     * autonomous stage sets a short one for BOT-ONLY replies so two bots can alternate turns (the
     * built-in 45s made A-B-A structurally impossible at a ~5s turn latency in the small audiences
     * that actually exist); starters ignore it, and any thread with human participation never
     * carries one, so a bot still cannot pepper a player.
     */
    uint32 replyCooldownSeconds = 0;
};

/*
 * The gate that runs before anything is spent. Returns the first failing reason, so a rejection
 * always names one cause rather than a set, and `None` only when every rule passed.
 */
[[nodiscard]] PlayerbotSocialOpportunityRejection PlayerbotSocialEvaluateOpportunity(
    PlayerbotSocialOpportunity const& opportunity);

// Conversation pressure ----------------------------------------------------------------------------

/*
 * Pressure is a probability, not a quota. Reply pressure never reaches certainty, so a bot may
 * always stay silent. The separate eligibility gate applies the product's hard two bot turn ceiling
 * before this curve or provider admission is reached.
 */
inline constexpr float PLAYERBOT_SOCIAL_REPLY_PRESSURE_CEILING = 0.95f;

// Pressure with one relevant human message, no decay and an empty channel. Everything else scales
// this base up toward the ceiling or decays it toward zero.
inline constexpr float PLAYERBOT_SOCIAL_REPLY_PRESSURE_BASE = 0.45f;

// A starter competes for the same channel from a deliberately lower base, which is half of why
// starters throttle first. Density pressing harder on the starter lane is the other half.
inline constexpr float PLAYERBOT_SOCIAL_STARTER_PRESSURE_BASE = 0.18f;

// Geometric decay per consecutive bot only turn and per full idle interval. Geometric rather than
// linear so the value falls steeply at first and then approaches zero without ever arriving.
// Applies to the REPLY lane only: replies answer a live thread, so a stale or bot-saturated thread
// deserves less of them. Starters follow the ambient fill below instead.
inline constexpr float PLAYERBOT_SOCIAL_BOT_ONLY_TURN_DECAY = 0.62f;
inline constexpr float PLAYERBOT_SOCIAL_IDLE_DECAY_PER_INTERVAL = 0.55f;
inline constexpr uint64 PLAYERBOT_SOCIAL_IDLE_DECAY_INTERVAL_SECONDS = 60;

/*
 * Ambient cadence. A quiet scope FILLS toward a starter instead of decaying into permanent silence:
 * starter pressure scales with idle time as idle/cadence, from the floor just after a line to the
 * full pressure at or past the cadence point. The default is the fallback when the configured
 * option is absent or unusable, exactly like the density multipliers above.
 */
inline constexpr uint32 PLAYERBOT_SOCIAL_AMBIENT_CADENCE_DEFAULT_SECONDS = 300;
inline constexpr float PLAYERBOT_SOCIAL_AMBIENT_MIN_FILL = 0.05f;
inline constexpr float PLAYERBOT_SOCIAL_STARTER_FULL_PRESSURE = 0.85f;

// How much each relevant human message adds, and the most that participation alone can contribute.
// Bounded so a long human conversation approaches the ceiling instead of saturating at it.
inline constexpr float PLAYERBOT_SOCIAL_HUMAN_MESSAGE_BONUS = 0.06f;
inline constexpr float PLAYERBOT_SOCIAL_HUMAN_BONUS_MAX = 0.42f;

// Density throttling. The starter coefficient is the larger of the two, which is what makes starters
// give way before active replies when a channel gets busy.
inline constexpr float PLAYERBOT_SOCIAL_REPLY_DENSITY_THROTTLE = 0.45f;
inline constexpr float PLAYERBOT_SOCIAL_STARTER_DENSITY_THROTTLE = 0.85f;

/*
 * Initial operator profile defaults. The running values come from configuration; these are only
 * the fallback used when the corresponding option is absent or unusable, and Task 16 may adjust
 * them without a source change.
 */
struct PlayerbotSocialDensityMultipliers
{
    float quiet = 0.55f;
    float normal = 1.00f;
    float lively = 1.60f;
};

// A multiplier must be finite and positive so it cannot create either certainty or a hard zero.
[[nodiscard]] float PlayerbotSocialNormalizeDensityMultiplier(float value, float fallback);

// Selects the configured multiplier for one operator profile. An invalid profile fails to normal.
[[nodiscard]] float PlayerbotSocialDensityMultiplier(PlayerbotSocialDensityProfile profile,
                                                     PlayerbotSocialDensityMultipliers const& multipliers);

/*
 * The server-wide provider call budget, shaped as a token bucket rather than a windowed count: a
 * windowed count admits the whole hour's budget as one opening burst and then starves the rest of
 * the hour. Tokens refill at hourlyBudget per hour and cap at a small burst, so the spend is
 * steady. Refusals are still tallied on a sliding hour, and only a pathological volume of them,
 * many multiples of the budget, trips the durable circuit: with hundreds of bots the refusal lane
 * runs warm all day, and that is the ceiling working, not an emergency. Pure state plus a pure
 * decision, so the whole ladder is testable without a database.
 */
inline constexpr uint64 PLAYERBOT_SOCIAL_PROVIDER_BUDGET_WINDOW_SECONDS = 3600;
inline constexpr uint32 PLAYERBOT_SOCIAL_PROVIDER_BURST_DIVISOR = 12;
inline constexpr uint64 PLAYERBOT_SOCIAL_BUDGET_TRIP_MULTIPLE = 20;

/*
 * The bottom quarter of the burst is reserved for continuations. Starter demand runs at many times
 * the budget, so without a reserve starters win every token and a reply to a bot's line never
 * opens: threads stall at one turn and the conversation half of the feature starves. Integer
 * division, so a degenerate burst of one to three tokens reserves nothing.
 */
inline constexpr uint32 PLAYERBOT_SOCIAL_BUDGET_CONTINUATION_RESERVE_DIVISOR = 4;

struct PlayerbotSocialProviderBudgetState
{
    // Negative means unseeded; the first decision seeds the bucket at its burst capacity.
    double tokens = -1.0;
    uint64 lastRefillUnixSeconds = 0;
    std::deque<uint64> refusedAtUnixSeconds;
};

enum class PlayerbotSocialBudgetDecision : uint8
{
    Admitted = 0,
    Refused,

    // Refused, and the refusal rate says the backstop must open: the caller owns flipping the
    // durable circuit, this decision only names the moment.
    RefusedCircuitTrip
};

/*
 * `continuation` marks a request that answers an existing thread rather than opening a new one;
 * continuations may draw the reserved bottom of the bucket, starters stop above it. `circuitOpen`
 * is the durable budget circuit: while it is open every call is refused before the bucket is
 * touched, so admission honours the same hard stop the pumps and delivery already do, and the
 * refusals never escalate into a second trip.
 */
[[nodiscard]] PlayerbotSocialBudgetDecision PlayerbotSocialGovernProviderCall(PlayerbotSocialProviderBudgetState& state,
                                                                              uint64 nowUnixSeconds,
                                                                              uint32 hourlyBudget,
                                                                              bool continuation = false,
                                                                              bool circuitOpen = false);

/*
 * Whether a request may draw the continuation reserve: any reply (its empty starter subject is
 * what marks it), and anything on the whisper channel. The one whisper starter is the
 * relationship check-in, paced by its per-pair cooldown and one-per-scan cadence, so it can never
 * flood the reserve the way ambient starters flood the bucket; at the starter floor it simply
 * lost the admission race to that flood on every scan.
 */
[[nodiscard]] bool PlayerbotSocialProviderCallDrawsReserve(PlayerbotSocialChannel channel, bool starterSubjectEmpty);

/*
 * The state of one inferred thread, as values. Counters rather than history: the coordinator keeps
 * bounded identities and participant state, and nothing here can outlive the character it describes.
 */
struct PlayerbotSocialThreadPressure
{
    uint32 consecutiveBotOnlyTurns = 0;
    uint32 relevantHumanMessages = 0;
    uint64 lastActivityUnixSeconds = 0;
    uint64 nowUnixSeconds = 0;
    uint8 channelDensity = 0;  // 0..100, clamped on use

    // Per-turn decay applied to the reply lane. The autonomous stage softens it so a bot-only
    // thread survives to a real length; earlier stages keep the default. Out-of-range values are
    // clamped back to the default on use.
    float botOnlyTurnDecay = PLAYERBOT_SOCIAL_BOT_ONLY_TURN_DECAY;

    /*
     * Participation base for a BOT-ONLY continuation. The autonomous stage raises it so a bot
     * conversation's early turns continue with real probability (at the default base a three-reply
     * chain almost never formed live); a thread with human participation always uses the default
     * base, and earlier stages keep this at the default. Out-of-range values fall back on use.
     */
    float botOnlyContinuationBase = PLAYERBOT_SOCIAL_REPLY_PRESSURE_BASE;

    /*
     * When set, a BOT-ONLY continuation skips the channel-density reply throttle. Each delivered
     * turn raises the scope's density, so the throttle acted as a second decay stacked on the
     * turn decay (window 8 measured 0.37 where base x decay was 0.68) and bot threads wound down
     * twice as fast as designed. The turn decay and the consecutive-turn cap own the wind-down in
     * the autonomous stage; a thread with human participation always keeps the throttle.
     */
    bool botOnlyDensityThrottleExempt = false;
};

// Probability that an eligible bot should answer in this thread right now. Strictly inside (0, 1).
[[nodiscard]] float PlayerbotSocialReplyPressure(PlayerbotSocialThreadPressure const& thread,
                                                 float densityProfileMultiplier = 1.0f);

/*
 * Probability that a bot should start speaking here. Below the reply pressure in a freshly active
 * thread, and it falls faster as the channel fills up; in a scope that has stayed quiet it FILLS
 * toward the full starter pressure at the configured cadence, so silence invites the next line
 * rather than suppressing it.
 */
[[nodiscard]] float PlayerbotSocialStarterPressure(
    PlayerbotSocialThreadPressure const& thread, float densityProfileMultiplier = 1.0f,
    uint32 ambientCadenceSeconds = PLAYERBOT_SOCIAL_AMBIENT_CADENCE_DEFAULT_SECONDS);

// General uses a separately configured multiplier. Say and Party retain the ordinary starter curve.
// Invalid General multipliers fail to the established quiet density multiplier.
[[nodiscard]] float PlayerbotSocialStarterPressureForChannel(
    PlayerbotSocialChannel channel, PlayerbotSocialThreadPressure const& thread, float densityProfileMultiplier = 1.0f,
    float generalMultiplier = PlayerbotSocialDensityMultipliers{}.quiet,
    uint32 ambientCadenceSeconds = PLAYERBOT_SOCIAL_AMBIENT_CADENCE_DEFAULT_SECONDS);

/*
 * Which budget lane this opportunity competes in. Derived from thread state rather than stored, so
 * the coordinator and the budget admission path cannot disagree about what a thread counts as.
 *
 * Direct address raises interest but never promotes a thread no human is in: a bot naming another
 * bot is not human engagement and must not reach the protected reserve.
 */
[[nodiscard]] PlayerbotSocialPriorityLane PlayerbotSocialAdmissionLane(PlayerbotSocialThreadPressure const& thread,
                                                                       bool starter, bool addressedDirectly);

// Responder selection ------------------------------------------------------------------------------

/*
 * Everything a decision records is bounded. A busy zone can present a field of any size, and one
 * opportunity must not be able to write an unbounded telemetry row because of it.
 */
inline constexpr std::size_t PLAYERBOT_SOCIAL_MAX_SELECTION_FACTORS = 4;
inline constexpr std::size_t PLAYERBOT_SOCIAL_MAX_SELECTION_ALTERNATES = 4;
inline constexpr std::size_t PLAYERBOT_SOCIAL_MAX_SELECTION_SUPPRESSIONS = 8;

// The most responders one opportunity may ever produce, on the explicitly tested coherent path.
inline constexpr std::size_t PLAYERBOT_SOCIAL_MAX_RESPONDERS = 2;

// Score contributions. Direct address is the strongest ordinary reason to answer, which is why it
// outweighs a materially higher base disposition rather than merely breaking a tie.
inline constexpr int32 PLAYERBOT_SOCIAL_SELECTION_NAMED_BONUS = 40;
inline constexpr int32 PLAYERBOT_SOCIAL_SELECTION_PARTICIPATED_BONUS = 15;
inline constexpr int32 PLAYERBOT_SOCIAL_SELECTION_QUESTION_BONUS = PLAYERBOT_SOCIAL_SELECTION_PARTICIPATED_BONUS;

// A bot whose interest sits under this after scoring will not speak even when the thread is loud.
inline constexpr int32 PLAYERBOT_SOCIAL_SELECTION_INTEREST_FLOOR = 30;

// Why a bot that was considered did not speak. Reported so a silent opportunity is explainable.
enum class PlayerbotSocialSuppressionReason : uint8
{
    HostileStance = 0,
    Uninterested,
    DuplicateCandidate,
    LostToHigherScore,
    // A stance outside the enumeration. Reported separately from hostility so a corrupt row is
    // visible as a data problem in telemetry rather than looking like an in character refusal.
    InvalidStance
};

/*
 * Bump when an enumerator is added above. The name test pairs every reason with its exact expected
 * string and asserts its table is this long, so a reason added without a name fails on the count and
 * a reason that falls through to "unknown" fails on the string. The count alone is not enough: a
 * fall through is neither empty nor a duplicate, so a length check passes straight over it.
 */
inline constexpr std::size_t PLAYERBOT_SOCIAL_SUPPRESSION_REASON_COUNT = 5;

[[nodiscard]] char const* PlayerbotSocialSuppressionReasonName(PlayerbotSocialSuppressionReason reason);

struct PlayerbotSocialSuppression
{
    uint64 botGuidCounter = 0;
    PlayerbotSocialSuppressionReason reason = PlayerbotSocialSuppressionReason::Uninterested;
};

// A named contribution to one bot's score. The name is a literal, so this carries no ownership.
struct PlayerbotSocialSelectionFactor
{
    char const* name = nullptr;
    int32 contribution = 0;
};

/*
 * One scored bot. Values only: the coordinator resolves a GUID counter back to a live character on
 * the world thread immediately before delivery, so nothing here can dangle.
 */
struct PlayerbotSocialCandidate
{
    uint64 botGuidCounter = 0;
    uint8 effectiveDisposition = 0;
    PlayerbotSocialStance stance = PlayerbotSocialStance::Neutral;
    bool addressedByName = false;
    bool askedQuestion = false;
    bool participatedInThread = false;
    uint8 contentRelevance = 0;  // 0..100, clamped on use
};

struct PlayerbotSocialSelectionInput
{
    std::vector<PlayerbotSocialCandidate> candidates;
    float replyPressure = 0.0f;
    bool secondResponderAllowed = false;
    uint64 selectionSeed = 0;
};

struct PlayerbotSocialSelection
{
    std::vector<uint64> responders;
    std::vector<uint64> alternates;
    std::vector<PlayerbotSocialSuppression> suppressions;
    std::vector<PlayerbotSocialSelectionFactor> leadingFactors;
};

/*
 * Chooses zero, one, or occasionally two responders before any provider request exists.
 *
 * Deterministic in its seed: the same input selects the same bots every time, which is what makes
 * the simulation tests meaningful. The seed varies per opportunity in production, so equally
 * disposed bots do not collapse onto one permanent speaker.
 */
[[nodiscard]] PlayerbotSocialSelection PlayerbotSocialSelectResponders(PlayerbotSocialSelectionInput const& input);

// A question is a direct conversational invitation even when it names nobody.
[[nodiscard]] bool PlayerbotSocialMessageIsQuestion(std::string_view message);

// Assistance evidence ------------------------------------------------------------------------------

/*
 * What one bot did for one other character across a single encounter.
 *
 * The combat hooks accumulate this; the policy below turns a finished encounter into a bounded
 * relationship delta. Values only and no pointers, because an encounter outlives the tick that
 * opened it and a `Unit*` does not.
 *
 * `effectiveHealing` is health actually restored, which is what `UnitScript::OnHeal` reports: a
 * heal landing on a full health character arrives as zero, so overheal never reaches this struct.
 * `meaningfulDamage` is damage dealt to an enemy that was engaged with the beneficiary, so splash
 * onto something nobody was fighting is excluded at the hook rather than discounted here.
 */
struct PlayerbotSocialAssistanceTally
{
    uint64 effectiveHealing = 0;
    uint64 meaningfulDamage = 0;
    // Times the beneficiary was healed while below the rescue threshold. Counted at the hook, which
    // is the only place that can see the health that made it a rescue.
    uint32 rescueCount = 0;
    uint32 contributionEvents = 0;

    /*
     * The beneficiary's maximum health, captured when the encounter opened.
     *
     * Stored rather than resolved at completion, because an encounter is completed by a sweep on the
     * world update and the character may be gone by then. A stale maximum from a level up mid fight
     * is a rounding error; a `Player*` held across ticks is a crash.
     */
    uint32 beneficiaryMaxHealth = 0;

    // When the last event landed. What the sweep uses to decide a fight is over, and what stops an
    // encounter that never completes from living forever.
    uint64 lastEventAtUnixSeconds = 0;
};

/*
 * Adds without wrapping.
 *
 * The totals are bounded by the policy caps only AFTER the division, so an encounter that ran long
 * enough to wrap a `uint64` would come out the other side reading as almost no help at all. That is
 * the one arithmetic path where overflow silently inverts the answer instead of exaggerating it.
 */
[[nodiscard]] inline constexpr uint64 PlayerbotSocialSaturatingAdd(uint64 total, uint64 addend)
{
    uint64 const remaining = UINT64_MAX - total;
    return addend > remaining ? UINT64_MAX : total + addend;
}

[[nodiscard]] inline constexpr uint32 PlayerbotSocialSaturatingIncrement(uint32 count)
{
    return count == UINT32_MAX ? UINT32_MAX : count + 1;
}

/*
 * Fraction of the beneficiary's maximum health at or below which a heal counts as a rescue rather
 * than as topping someone up. Being pulled off the floor is what a character remembers.
 */
inline constexpr float PLAYERBOT_SOCIAL_RESCUE_HEALTH_FRACTION = 0.25f;

/*
 * Per encounter ceilings. These are what make the credit unfarmable: an encounter contributes at
 * most this much no matter how many events fed it, so splitting one heal into a thousand ticks
 * earns exactly what one heal of the same size earns.
 */
inline constexpr float PLAYERBOT_SOCIAL_ASSISTANCE_FAMILIARITY_CAP = 0.05f;
inline constexpr float PLAYERBOT_SOCIAL_ASSISTANCE_AFFINITY_CAP = 0.04f;
inline constexpr float PLAYERBOT_SOCIAL_ASSISTANCE_TRUST_CAP = 0.06f;

// Rescues past this many in one encounter add nothing further. One rescue is the memorable event;
// the second confirms it was not luck; beyond that the encounter is simply a hard fight.
inline constexpr uint32 PLAYERBOT_SOCIAL_ASSISTANCE_RESCUE_CAP = 2;

/*
 * Whether a heal landing on a character at `currentHealth` counts as pulling them off the floor.
 *
 * Asked at the hook, because it is the only place that can see the health that made it a rescue.
 * A character already at zero was dead when the heal landed, which is a resurrection question this
 * task does not answer, and an unknown maximum gives nothing to compare against. Both earn nothing.
 */
[[nodiscard]] inline constexpr bool PlayerbotSocialHealIsRescue(uint32 currentHealth, uint32 maxHealth)
{
    if (maxHealth == 0 || currentHealth == 0)
        return false;

    return currentHealth <=
           static_cast<uint32>(static_cast<float>(maxHealth) * PLAYERBOT_SOCIAL_RESCUE_HEALTH_FRACTION);
}

/*
 * Whether damage dealt counts as helping the beneficiary rather than as incidental splash.
 *
 * The engagement question is resolved live at the hook, where the enemy's threat and target are
 * readable. Damage to something nobody was fighting is worth nothing here however large it was.
 */
[[nodiscard]] inline constexpr bool PlayerbotSocialDamageIsMeaningful(bool victimEngagedWithBeneficiary, uint32 damage)
{
    return victimEngagedWithBeneficiary && damage > 0;
}

/*
 * Turns a finished encounter into a bounded, deterministic relationship delta.
 *
 * Pure and total like everything else in this header: the same tally always produces the same
 * delta. A provider interpretation may suggest what a bot felt about an encounter, but the numeric
 * relationship change is decided here and nowhere else.
 *
 * `beneficiaryMaxHealth` normalises the contribution, because 500 healing is a rescue at level ten
 * and a rounding error at level eighty. A zero or missing maximum earns nothing rather than
 * dividing by zero.
 */
[[nodiscard]] PlayerbotSocialRelationshipValues PlayerbotSocialAssistanceDelta(
    PlayerbotSocialAssistanceTally const& tally, uint32 beneficiaryMaxHealth);

/*
 * How long an encounter may go without an event before the sweep decides the fight is over.
 *
 * Long enough to span a pull, a run between packs, and a resurrection; short enough that a bot that
 * logged out mid fight does not hold its tally for the rest of the uptime.
 */
inline constexpr uint64 PLAYERBOT_SOCIAL_ENCOUNTER_IDLE_SECONDS = 60;

/*
 * How long the per pair credit ceiling covers.
 *
 * The per encounter caps alone do not bound what a pair can earn, because the sweep decides an
 * encounter ended by elapsed idle time rather than by an authoritative combat end. A fight that
 * carries on with sparse healing is therefore chopped into several encounters, and each one would
 * otherwise pay the full ceiling again. An authoritative combat end was considered and rejected: no
 * signal arrives when a participant logs out or the mob despawns, and even a perfect one would still
 * let a pair farm the ceiling by entering and leaving combat repeatedly.
 *
 * The window is what actually bounds it. Whatever the encounter boundaries turn out to be, one pair
 * cannot gain more than the caps within this span.
 */
inline constexpr uint64 PLAYERBOT_SOCIAL_ASSISTANCE_PAYOUT_WINDOW_SECONDS = 3600;

/*
 * How often the encounter sweep runs.
 *
 * Well below PLAYERBOT_SOCIAL_ENCOUNTER_IDLE_SECONDS, so the delay it adds to completing a finished
 * fight is small, and far above one world update, so the map is not re-walked thousands of times
 * between the two ticks where the answer can actually change.
 */
inline constexpr uint32 PLAYERBOT_SOCIAL_ENCOUNTER_SWEEP_INTERVAL_MS = 5000;

/*
 * Hard ceiling on how many pairs any one combat map may track at once.
 *
 * Age pruning bounds nothing inside a single idle window: enough distinct pairs within sixty
 * seconds grow every one of these maps without limit, and a world boss or a battleground zone is
 * exactly that shape. This is the bound memory cannot exceed regardless of traffic.
 *
 * Sized as headroom rather than as a working limit. A realm running a few hundred bots reaches a few
 * hundred concurrent pairs, so this is roughly an order of magnitude above anything realistic, and
 * at well under a megabyte per map it costs little to be generous.
 *
 * Reaching it means one of two things, and which one depends on whether the map holds WORK or a
 * BOUND.
 *
 * The encounter map holds work: a tally waiting to be paid. Its oldest entry is evicted, because
 * that entry is closest to completing anyway and losing it costs one unpaid encounter.
 *
 * The credit ledger and the opposition marker map both hold bounds. Evicting from either REMOVES a
 * limit rather than some pending work, and anyone able to churn this many pairs could evict their
 * own entry on demand: a fresh hourly ceiling from the ledger, or a second aggression delta inside
 * one fight from the marker map. Both therefore refuse new entries when full, and refuse the thing
 * the missing entry would have bounded along with it. Saturation is self healing in every case,
 * because the sweep drops elapsed windows and idle markers.
 */
inline constexpr std::size_t PLAYERBOT_SOCIAL_MAX_TRACKED_PAIRS = 8192;

/*
 * Opposition markers are bounded per attacker, not out of one shared pool.
 *
 * A single global ceiling is a starvation surface even once only storable pairs may allocate: one
 * consented player who fights enough distinct bots fills it, and every other pair then goes
 * unattributed until markers expire. Whoever is loudest wins, which is backwards.
 *
 * Bounding each attacker separately removes that. An attacker cannot take more than their own share
 * no matter how many bots they engage, so filling the structure needs many independent attackers
 * rather than one determined one. The product of the two is the memory bound, and because it is
 * structural there is no running total to keep in step with the inserts and erasures.
 *
 * Both are sized as headroom. Thirty two simultaneous distinct victims is far beyond one fight, and
 * five hundred simultaneous distinct attackers is far beyond a realm of a few hundred characters.
 */
inline constexpr std::size_t PLAYERBOT_SOCIAL_MAX_OPPOSITION_ATTACKERS = 512;
inline constexpr std::size_t PLAYERBOT_SOCIAL_MAX_OPPOSITION_PER_ATTACKER = 32;

/*
 * What one pair has already been credited inside the current window.
 *
 * Positive movement only. A penalty is never rationed: repeated ganking should keep costing the
 * aggressor, and the stored value is bounded at its floor by the database anyway.
 */
struct PlayerbotSocialAssistanceCredit
{
    /*
     * Explicit, because a timestamp alone cannot say "no window yet". Zero is a valid Unix second,
     * so a ledger whose first gain landed at zero would still read as untouched and the next call
     * would open a fresh window, handing the pair the ceiling twice.
     */
    bool windowStarted = false;
    uint64 windowStartedAtUnixSeconds = 0;
    float familiarity = 0.0f;
    float affinity = 0.0f;
    float trust = 0.0f;
};

/*
 * Returns the part of `delta` that may still be paid, and records it against the window.
 *
 * A window that has fully elapsed restarts at `nowUnixSeconds` with nothing spent. A clock that
 * moves backwards does NOT restart it, because restarting on a rewind is exactly the move that would
 * let the ceiling be paid twice.
 */
[[nodiscard]] PlayerbotSocialRelationshipValues PlayerbotSocialAdmitAssistanceCredit(
    PlayerbotSocialAssistanceCredit& credit, PlayerbotSocialRelationshipValues const& delta, uint64 nowUnixSeconds);

/*
 * Whether a credit entry is old enough that dropping it changes no future answer, which is what lets
 * the sweep prune the ledger instead of holding one entry per pair for the rest of the uptime.
 */
[[nodiscard]] bool PlayerbotSocialAssistanceCreditIsExpired(PlayerbotSocialAssistanceCredit const& credit,
                                                            uint64 nowUnixSeconds);

/*
 * What may be applied for a pair that could not be given a ledger slot at all.
 *
 * Only the part that does not need rationing: penalties. A gain with no ledger behind it is
 * unbounded by construction, so it is refused rather than granted.
 *
 * This is the case that makes the ceiling hold under cardinality pressure. Evicting a ledger entry
 * to make room would hand that pair a fresh ceiling early, and anyone able to churn
 * PLAYERBOT_SOCIAL_MAX_TRACKED_PAIRS other pairs could evict their own entry on demand and collect
 * a new ceiling as often as they liked. Refusing is what keeps the per window bound a bound, and it
 * is self healing: the sweep drops every elapsed window, so slots return within the hour.
 */
[[nodiscard]] PlayerbotSocialRelationshipValues PlayerbotSocialAdmitWithoutLedger(
    PlayerbotSocialRelationshipValues const& delta);

/*
 * Whether an opposition event belongs to a fight that was already answered.
 *
 * "They attacked me" does not get truer the more swings it took, and the damage hook sees every
 * swing. Two events belong to the same fight while fewer than PLAYERBOT_SOCIAL_ENCOUNTER_IDLE_SECONDS
 * separate them, using the same threshold an assistance encounter uses.
 *
 * A clock that moves backwards reads as no elapsed time rather than as a large interval, so a
 * correction cannot end a fight early and let it be answered twice.
 */
[[nodiscard]] bool PlayerbotSocialOppositionIsSameFight(uint64 markerAtUnixSeconds, uint64 nowUnixSeconds);

/*
 * Moves an opposition marker forward, never backward.
 *
 * A continuing fight keeps refreshing its marker so it stays one fight. Writing an earlier timestamp
 * would let the idle threshold elapse against the rewound value, which answers the same fight a
 * second time.
 */
[[nodiscard]] uint64 PlayerbotSocialAdvanceOppositionMarker(uint64 markerAtUnixSeconds, uint64 nowUnixSeconds);

// Combat context -----------------------------------------------------------------------------------

/*
 * Why two characters were fighting. Assistance and hostility are separate policies over the same
 * encounter, and this is the axis that decides whether the hostile half applies at all.
 */
enum class PlayerbotSocialCombatContext : uint8
{
    Cooperative = 0,  // PVE, or two characters fighting the same enemy.
    Duel,
    Arena,
    Battleground,
    OpenWorldPvp
};

[[nodiscard]] inline constexpr bool PlayerbotSocialCombatContextIsValid(PlayerbotSocialCombatContext context)
{
    switch (context)
    {
        case PlayerbotSocialCombatContext::Cooperative:
        case PlayerbotSocialCombatContext::Duel:
        case PlayerbotSocialCombatContext::Arena:
        case PlayerbotSocialCombatContext::Battleground:
        case PlayerbotSocialCombatContext::OpenWorldPvp:
            return true;
        default:
            break;
    }

    return false;
}

/*
 * The stable label a PVP event reports as its reason.
 *
 * Empty for a value outside the enum, matching every other schema spelling in this feature, so a
 * corrupt context is refused by the event binding rather than filed under the first member. This
 * module compiles without -Wswitch, so the default arm is the guard rather than the compiler.
 */
[[nodiscard]] std::string_view PlayerbotSocialCombatContextName(PlayerbotSocialCombatContext context);

/*
 * Whether opposition in this context says anything about the opponent.
 *
 * Only open world aggression does. A duel, an arena match, and a battleground are all fights the
 * character agreed to, and treating a battleground opponent as an enemy would make every bot hate
 * half the server for playing the game as intended. Something said or done inside those fights can
 * still move a relationship, but it travels through the chat and assistance paths, not through the
 * mere fact of being on the other side.
 *
 * An unrecognized context is refused, because this module compiles without `-Wswitch` and a later
 * enumerator that nobody wired here must not inherit a hostile default.
 */
[[nodiscard]] inline constexpr bool PlayerbotSocialCombatContextAppliesHostility(PlayerbotSocialCombatContext context)
{
    if (!PlayerbotSocialCombatContextIsValid(context))
        return false;

    return context == PlayerbotSocialCombatContext::OpenWorldPvp;
}

/*
 * What unprovoked open world aggression costs the attacker, per encounter.
 *
 * Deliberately smaller in magnitude than a full encounter of assistance is worth, because one
 * ganking should not permanently define a character while repeated help can still outweigh it.
 */
inline constexpr float PLAYERBOT_SOCIAL_AGGRESSION_AFFINITY_PENALTY = 0.03f;
inline constexpr float PLAYERBOT_SOCIAL_AGGRESSION_TRUST_PENALTY = 0.05f;
inline constexpr float PLAYERBOT_SOCIAL_AGGRESSION_FAMILIARITY_GAIN = 0.02f;

// The bounded, deterministic delta for one open world aggression encounter. Only callers that have
// already established the context through `PlayerbotSocialCombatContextAppliesHostility` apply it.
[[nodiscard]] PlayerbotSocialRelationshipValues PlayerbotSocialOpenWorldAggressionDelta();

// Roleplay affinity --------------------------------------------------------------------------------

/*
 * The willingness version owns the transient per opportunity invitation and continuation roll.
 * It is independent from the stored affinity and from PLAYERBOT_SOCIAL_POLICY_VERSION.
 */
inline constexpr uint32 PLAYERBOT_ROLEPLAY_WILLINGNESS_VERSION = 1;

// Fixed PRD bands over the 1..100 affinity score: averse 1..50, neutral 51..75, receptive 76..89,
// enthusiast 90..100. Out of range scores fail closed to the averse band.
enum class PlayerbotRoleplayAffinityBand : uint8
{
    Averse = 0,
    Neutral,
    Receptive,
    Enthusiast
};

// Strict result kinds of one sidecar roleplay assessment. Wire values are the snake_case names the
// bridge parses; anything else is malformed and resolves to ordinary behavior.
enum class PlayerbotRoleplayAssessmentKind : uint8
{
    Ordinary = 0,
    RoleplayInvitation,
    RoleplayContinuation,
    Practical,
    OptOut,
    Uncertain
};

// Trusted worldserver prompt authority. Only AuthorizedRoleplay may lift the ordinary player voice
// premise; every unknown or invalid value must resolve to Ordinary, never to authorization.
enum class PlayerbotRoleplayPromptMode : uint8
{
    Ordinary = 0,
    DeclineRoleplay,
    AcknowledgeRoleplay,
    AuthorizedRoleplay
};

[[nodiscard]] PlayerbotRoleplayAffinityBand PlayerbotRoleplayAffinityBandFor(uint8 affinity);

/*
 * Deterministic per opportunity roll on 1..25, derived only from the activation seed, the bot GUID
 * counter, and the willingness version. Uniform by the same rejection sampling as the affinity.
 */
[[nodiscard]] uint8 PlayerbotRoleplayWillingnessRoll(
    uint64 activationSeed, uint64 botGuidCounter, uint32 willingnessVersion = PLAYERBOT_ROLEPLAY_WILLINGNESS_VERSION);

/*
 * Whether a receptive or enthusiast score enters roleplay for this roll: scores 76..100 pass when
 * roll <= score - 75, which gives exactly 1 through 25 passing roll values, so a higher score can
 * never reduce willingness. Scores at or below 75 and out of range inputs always fail.
 */
[[nodiscard]] bool PlayerbotRoleplayWillingnessPasses(uint8 affinity, uint8 roll);

[[nodiscard]] inline constexpr bool PlayerbotRoleplayAffinityBandIsValid(PlayerbotRoleplayAffinityBand band)
{
    switch (band)
    {
        case PlayerbotRoleplayAffinityBand::Averse:
        case PlayerbotRoleplayAffinityBand::Neutral:
        case PlayerbotRoleplayAffinityBand::Receptive:
        case PlayerbotRoleplayAffinityBand::Enthusiast:
            return true;
    }

    return false;
}

[[nodiscard]] inline constexpr bool PlayerbotRoleplayAssessmentKindIsValid(PlayerbotRoleplayAssessmentKind kind)
{
    switch (kind)
    {
        case PlayerbotRoleplayAssessmentKind::Ordinary:
        case PlayerbotRoleplayAssessmentKind::RoleplayInvitation:
        case PlayerbotRoleplayAssessmentKind::RoleplayContinuation:
        case PlayerbotRoleplayAssessmentKind::Practical:
        case PlayerbotRoleplayAssessmentKind::OptOut:
        case PlayerbotRoleplayAssessmentKind::Uncertain:
            return true;
    }

    return false;
}

[[nodiscard]] inline constexpr bool PlayerbotRoleplayPromptModeIsValid(PlayerbotRoleplayPromptMode mode)
{
    switch (mode)
    {
        case PlayerbotRoleplayPromptMode::Ordinary:
        case PlayerbotRoleplayPromptMode::DeclineRoleplay:
        case PlayerbotRoleplayPromptMode::AcknowledgeRoleplay:
        case PlayerbotRoleplayPromptMode::AuthorizedRoleplay:
            return true;
    }

    return false;
}

[[nodiscard]] char const* PlayerbotRoleplayAffinityBandName(PlayerbotRoleplayAffinityBand band);
[[nodiscard]] char const* PlayerbotRoleplayAssessmentKindName(PlayerbotRoleplayAssessmentKind kind);
[[nodiscard]] char const* PlayerbotRoleplayPromptModeName(PlayerbotRoleplayPromptMode mode);

#endif  // PLAYERBOTS_PLAYERBOTSOCIALPOLICY_H
