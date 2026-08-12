/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#include "Bot/Social/PlayerbotSocialPolicy.h"

#include <algorithm>
#include <cmath>
#include <limits>

#include "Bot/Social/PlayerbotSocialPersonality.h"

bool PlayerbotSocialAmbientSourceEnabled(bool legacyBroadcastsEnabled, bool socialChatEnabled)
{
    return legacyBroadcastsEnabled || socialChatEnabled;
}

bool PlayerbotSocialAmbientSourcePassesChance(bool socialChatEnabled, uint32 legacyChance, uint32 roll)
{
    return socialChatEnabled || (legacyChance != 0 && roll != 0 && roll <= legacyChance);
}

namespace
{
uint8 ClampTrait(uint8 value) { return value > PLAYERBOT_SOCIAL_TRAIT_MAX ? PLAYERBOT_SOCIAL_TRAIT_MAX : value; }

// Rounds a bounded score to the nearest whole point. The input is already clamped to 0..100 by
// the caller, so adding a half never overflows the byte.
uint8 RoundBoundedScore(float score) { return static_cast<uint8>(score + 0.5f); }
}  // namespace

namespace
{
// Repeated multiplication rather than std::pow: the exponent is a turn or interval count that can
// be enormous for a corrupt row, and the loop is bounded above so it cannot spin on one.
float DecayFactor(float perStep, uint64 steps)
{
    /*
     * The step bound is load bearing, and its value is chosen against the PRODUCT of both decays
     * rather than either one alone. Reply pressure multiplies the bot only turn decay by the idle
     * decay, so the two exponents compound: at 64 steps each the product is about 1.2e-30, which
     * is a normal float, while at 100 steps each it is about 1.9e-47, which is below the float
     * denormal minimum and flushes to exactly zero.
     *
     * Exactly zero is the one value this policy must never report, because a zero probability is
     * the hard conversation cap the contract forbids. So decay is strict up to the bound and flat
     * beyond it. Flat is the safe direction to fail: it leaves a vanishing but real chance rather
     * than forbidding the conversation. By the bound the combined decay is already around 1e-30,
     * far below anything a roll will ever hit, so the flattening is unobservable in play and is
     * pinned by DecayFlattensAtTheStepBoundRatherThanReachingZero.
     */
    constexpr uint64 MAX_STEPS = 64;
    uint64 const bounded = steps > MAX_STEPS ? MAX_STEPS : steps;

    float factor = 1.0f;
    for (uint64 step = 0; step < bounded; ++step)
        factor *= perStep;

    return factor;
}

float ClampDensity(uint8 density)
{
    float const bounded = density > PLAYERBOT_SOCIAL_TRAIT_MAX ? static_cast<float>(PLAYERBOT_SOCIAL_TRAIT_MAX)
                                                               : static_cast<float>(density);
    return bounded / static_cast<float>(PLAYERBOT_SOCIAL_TRAIT_MAX);
}

// Elapsed time with a clock that moved backwards read as zero rather than as a huge interval.
// Decaying a thread to nothing because the clock stepped back would be the wrong direction to
// fail: it silently ends live conversations.
uint64 ElapsedSeconds(uint64 nowUnixSeconds, uint64 sinceUnixSeconds)
{
    return nowUnixSeconds < sinceUnixSeconds ? 0 : nowUnixSeconds - sinceUnixSeconds;
}

float HumanParticipationBonus(uint32 relevantHumanMessages)
{
    float const raw = static_cast<float>(relevantHumanMessages) * PLAYERBOT_SOCIAL_HUMAN_MESSAGE_BONUS;
    return raw > PLAYERBOT_SOCIAL_HUMAN_BONUS_MAX ? PLAYERBOT_SOCIAL_HUMAN_BONUS_MAX : raw;
}

// The decay shared by both lanes: bot only turns and idle time.
float ThreadDecay(PlayerbotSocialThreadPressure const& thread)
{
    uint64 const idleSeconds = ElapsedSeconds(thread.nowUnixSeconds, thread.lastActivityUnixSeconds);
    uint64 const idleIntervals = idleSeconds / PLAYERBOT_SOCIAL_IDLE_DECAY_INTERVAL_SECONDS;

    return DecayFactor(PLAYERBOT_SOCIAL_BOT_ONLY_TURN_DECAY, thread.consecutiveBotOnlyTurns) *
           DecayFactor(PLAYERBOT_SOCIAL_IDLE_DECAY_PER_INTERVAL, idleIntervals);
}

/*
 * Keeps a pressure inside (0, ceiling]. Both ends are contractual: certainty would force a reply,
 * and zero would be a hard cap.
 *
 * There is deliberately no lower clamp. Geometric decay at the step bound above is still a
 * positive float, so the value stays above zero on its own; clamping it to a floor would instead
 * flatten the curve and stop the decay the no hard cap rule is stated in terms of. The only lower
 * guard needed is against a NaN, which would otherwise pass every comparison.
 */
float BoundPressure(float value, float ceiling)
{
    float const finite = PlayerbotSocialDetail::NeutralIfNotANumber(value, 0.0f);
    if (finite > ceiling)
        return ceiling;

    return finite < 0.0f ? 0.0f : finite;
}

float ScaleByDensityProfile(float value, float multiplier)
{
    float const scaled = value * PlayerbotSocialNormalizeDensityMultiplier(multiplier, 1.0f);

    // A positive configured multiplier is not a hard stop. If floating point underflow would
    // turn a still eligible thread into zero, retain the smallest normal positive pressure.
    if (value > 0.0f && scaled == 0.0f)
        return std::numeric_limits<float>::min();

    return scaled;
}
}  // namespace

char const* PlayerbotSocialOpportunityRejectionName(PlayerbotSocialOpportunityRejection rejection)
{
    switch (rejection)
    {
        case PlayerbotSocialOpportunityRejection::None:
            return "none";
        case PlayerbotSocialOpportunityRejection::UnsupportedChannel:
            return "unsupported_channel";
        case PlayerbotSocialOpportunityRejection::StarterNotAllowedOnChannel:
            return "starter_not_allowed_on_channel";
        case PlayerbotSocialOpportunityRejection::SelfReply:
            return "self_reply";
        case PlayerbotSocialOpportunityRejection::StarterSourceMismatch:
            return "starter_source_mismatch";
        case PlayerbotSocialOpportunityRejection::InitiationOptedOut:
            return "initiation_opted_out";
        case PlayerbotSocialOpportunityRejection::SpeakerOptedOut:
            return "speaker_opted_out";
        case PlayerbotSocialOpportunityRejection::FactionMismatch:
            return "faction_mismatch";
        case PlayerbotSocialOpportunityRejection::LanguageMismatch:
            return "language_mismatch";
        case PlayerbotSocialOpportunityRejection::ThreadStale:
            return "thread_stale";
        case PlayerbotSocialOpportunityRejection::CooldownActive:
            return "cooldown_active";
        case PlayerbotSocialOpportunityRejection::DuplicateSuppressed:
            return "duplicate_suppressed";
        case PlayerbotSocialOpportunityRejection::BotOnlyTurnLimit:
            return "bot_only_turn_limit";
        case PlayerbotSocialOpportunityRejection::ProfilePending:
            return "profile_pending";
        case PlayerbotSocialOpportunityRejection::ProfileRejected:
            return "profile_rejected";
        case PlayerbotSocialOpportunityRejection::ProfileUnavailable:
            return "profile_unavailable";
    }

    // Not unreachable: this build has neither -Wswitch nor -Werror, so an enumerator added later
    // arrives here silently. Naming it is what keeps a diagnostic readable instead of empty.
    return "unknown";
}

PlayerbotSocialOpportunityRejection PlayerbotSocialEvaluateOpportunity(PlayerbotSocialOpportunity const& opportunity)
{
    switch (opportunity.profileLoadState)
    {
        case PlayerbotSocialProfileLoadState::Pending:
            return PlayerbotSocialOpportunityRejection::ProfilePending;
        case PlayerbotSocialProfileLoadState::UnavailableUsingBase:
            return PlayerbotSocialOpportunityRejection::ProfileUnavailable;
        /*
         * Rejected is admitted, not gated. The load has already replaced the unusable row with a
         * profile seeded from the stable base personality, exactly as it does for a missing row, so
         * by the time an opportunity carries this state there is a fully usable profile behind it.
         * Refusing it would contradict that fallback and mute the bot permanently, because an
         * unsupported stored version is rejected again on every load. Unavailable stays gated: a
         * read that never answered says nothing about what the row contains, and admitting it could
         * speak over a profile that actually exists.
         */
        case PlayerbotSocialProfileLoadState::Loaded:
        case PlayerbotSocialProfileLoadState::AbsentUsingBase:
        case PlayerbotSocialProfileLoadState::RejectedUsingBase:
            break;
    }

    if (!PlayerbotSocialChannelIsValid(opportunity.channel))
        return PlayerbotSocialOpportunityRejection::UnsupportedChannel;

    // Whisper is reactive only. Every other supported channel accepts a spontaneous starter.
    if (opportunity.starter && opportunity.channel == PlayerbotSocialChannel::Whisper)
        return PlayerbotSocialOpportunityRejection::StarterNotAllowedOnChannel;

    // Opting out of initiation stops a bot addressing someone first, and nothing more. A speaker
    // level opt out suppresses public replies too. The one exception is a human directly whispering
    // a bot: that may receive a stateless reply whose request retains no history or telemetry.
    if (opportunity.starter && opportunity.botOptedOutOfInitiation)
        return PlayerbotSocialOpportunityRejection::InitiationOptedOut;

    bool const statelessDirectWhisper = opportunity.speakerOptedOut && opportunity.speakerIsHuman &&
                                        !opportunity.starter && opportunity.channel == PlayerbotSocialChannel::Whisper;
    if (opportunity.speakerOptedOut && !statelessDirectWhisper)
        return PlayerbotSocialOpportunityRejection::SpeakerOptedOut;

    if (!opportunity.factionMatches)
        return PlayerbotSocialOpportunityRejection::FactionMismatch;

    if (!opportunity.languageMatches)
        return PlayerbotSocialOpportunityRejection::LanguageMismatch;

    // Both elapsed checks fail closed on a clock that moved backwards. Treating that as a very large
    // elapsed time would expire every thread and release every cooldown at once.
    if (opportunity.nowUnixSeconds < opportunity.threadLastActivityUnixSeconds ||
        opportunity.nowUnixSeconds - opportunity.threadLastActivityUnixSeconds > PLAYERBOT_SOCIAL_THREAD_STALE_SECONDS)
        return PlayerbotSocialOpportunityRejection::ThreadStale;

    if (opportunity.nowUnixSeconds < opportunity.botLastSpokeUnixSeconds ||
        opportunity.nowUnixSeconds - opportunity.botLastSpokeUnixSeconds < PLAYERBOT_SOCIAL_REPLY_COOLDOWN_SECONDS)
        return PlayerbotSocialOpportunityRejection::CooldownActive;

    if (opportunity.duplicateOfRecentMessage)
        return PlayerbotSocialOpportunityRejection::DuplicateSuppressed;

    if (!opportunity.starter && !opportunity.speakerIsHuman &&
        opportunity.consecutiveBotOnlyTurns >= PLAYERBOT_SOCIAL_MAX_CONSECUTIVE_BOT_TURNS)
        return PlayerbotSocialOpportunityRejection::BotOnlyTurnLimit;

    return PlayerbotSocialOpportunityRejection::None;
}

float PlayerbotSocialNormalizeDensityMultiplier(float value, float fallback)
{
    float const usableFallback = std::isfinite(fallback) && fallback > 0.0f ? fallback : 1.0f;
    return std::isfinite(value) && value > 0.0f ? value : usableFallback;
}

float PlayerbotSocialDensityMultiplier(PlayerbotSocialDensityProfile profile,
                                       PlayerbotSocialDensityMultipliers const& multipliers)
{
    PlayerbotSocialDensityMultipliers const defaults;
    switch (profile)
    {
        case PlayerbotSocialDensityProfile::Quiet:
            return PlayerbotSocialNormalizeDensityMultiplier(multipliers.quiet, defaults.quiet);
        case PlayerbotSocialDensityProfile::Normal:
            return PlayerbotSocialNormalizeDensityMultiplier(multipliers.normal, defaults.normal);
        case PlayerbotSocialDensityProfile::Lively:
            return PlayerbotSocialNormalizeDensityMultiplier(multipliers.lively, defaults.lively);
    }

    return PlayerbotSocialNormalizeDensityMultiplier(multipliers.normal, defaults.normal);
}

float PlayerbotSocialReplyPressure(PlayerbotSocialThreadPressure const& thread, float densityProfileMultiplier)
{
    float const participation =
        PLAYERBOT_SOCIAL_REPLY_PRESSURE_BASE + HumanParticipationBonus(thread.relevantHumanMessages);
    float const throttled =
        participation * (1.0f - PLAYERBOT_SOCIAL_REPLY_DENSITY_THROTTLE * ClampDensity(thread.channelDensity));

    return BoundPressure(ScaleByDensityProfile(throttled * ThreadDecay(thread), densityProfileMultiplier),
                         PLAYERBOT_SOCIAL_REPLY_PRESSURE_CEILING);
}

float PlayerbotSocialStarterPressure(PlayerbotSocialThreadPressure const& thread, float densityProfileMultiplier)
{
    // A starter gets no human participation bonus. Someone else's conversation being lively is not a
    // reason to begin a separate one, and leaving the bonus out is what keeps the starter lane below
    // the reply lane even in the most active thread.
    float const throttled = PLAYERBOT_SOCIAL_STARTER_PRESSURE_BASE *
                            (1.0f - PLAYERBOT_SOCIAL_STARTER_DENSITY_THROTTLE * ClampDensity(thread.channelDensity));

    return BoundPressure(ScaleByDensityProfile(throttled * ThreadDecay(thread), densityProfileMultiplier),
                         PLAYERBOT_SOCIAL_REPLY_PRESSURE_CEILING);
}

float PlayerbotSocialStarterPressureForChannel(PlayerbotSocialChannel channel,
                                               PlayerbotSocialThreadPressure const& thread,
                                               float densityProfileMultiplier, float generalMultiplier)
{
    float const pressure = PlayerbotSocialStarterPressure(thread, densityProfileMultiplier);
    if (channel != PlayerbotSocialChannel::General)
        return pressure;

    float const normalized = std::isfinite(generalMultiplier) && generalMultiplier > 0.0f && generalMultiplier < 1.0f
                                 ? generalMultiplier
                                 : PlayerbotSocialDensityMultipliers{}.quiet;
    return pressure * normalized;
}

PlayerbotSocialPriorityLane PlayerbotSocialAdmissionLane(PlayerbotSocialThreadPressure const& thread, bool starter,
                                                         bool addressedDirectly)
{
    // Checked first: a starter is new speech regardless of who has been talking, so it competes in
    // the starter lane even in a thread a human is active in.
    if (starter)
        return PlayerbotSocialPriorityLane::NewStarter;

    if (thread.relevantHumanMessages == 0)
        return PlayerbotSocialPriorityLane::BotOnlyContinuation;

    return addressedDirectly ? PlayerbotSocialPriorityLane::DirectHuman : PlayerbotSocialPriorityLane::MixedHumanBot;
}

char const* PlayerbotSocialSuppressionReasonName(PlayerbotSocialSuppressionReason reason)
{
    switch (reason)
    {
        case PlayerbotSocialSuppressionReason::HostileStance:
            return "hostile_stance";
        case PlayerbotSocialSuppressionReason::Uninterested:
            return "uninterested";
        case PlayerbotSocialSuppressionReason::DuplicateCandidate:
            return "duplicate_candidate";
        case PlayerbotSocialSuppressionReason::LostToHigherScore:
            return "lost_to_higher_score";
        case PlayerbotSocialSuppressionReason::InvalidStance:
            return "invalid_stance";
    }

    return "unknown";
}

std::string_view PlayerbotSocialCombatContextName(PlayerbotSocialCombatContext context)
{
    switch (context)
    {
        case PlayerbotSocialCombatContext::Cooperative:
            return "cooperative";
        case PlayerbotSocialCombatContext::Duel:
            return "duel";
        case PlayerbotSocialCombatContext::Arena:
            return "arena";
        case PlayerbotSocialCombatContext::Battleground:
            return "battleground";
        case PlayerbotSocialCombatContext::OpenWorldPvp:
            return "open_world_pvp";
    }

    // Empty rather than a placeholder, so a corrupt context reaches the event binding as a value it
    // refuses instead of one it stores under a name the schema does not define.
    return {};
}

namespace
{
// Namespace constant so a selection roll cannot correlate with any other seeded decision that
// shares the same opportunity inputs.
constexpr uint64 SELECTION_NAMESPACE = 0x53454C4543544F52ULL;

struct ScoredCandidate
{
    uint64 botGuidCounter = 0;
    int32 score = 0;
    uint64 tiebreak = 0;
    bool addressedByName = false;
    bool askedQuestion = false;
    bool participatedInThread = false;
};

// A roll in [0, 1) derived from the seed. Deterministic, and independent per salt so the reply
// roll and the second responder roll do not move together.
float SeededRoll(uint64 seed, uint64 salt)
{
    uint64 const bits = PlayerbotPersonality::SplitMix64(seed ^ salt ^ SELECTION_NAMESPACE);

    // 24 bits keeps the quotient exactly representable in a float.
    return static_cast<float>(bits >> 40) / static_cast<float>(1u << 24);
}

int32 InterestScore(PlayerbotSocialCandidate const& candidate)
{
    uint8 const disposition = candidate.effectiveDisposition > PLAYERBOT_SOCIAL_TRAIT_MAX
                                  ? PLAYERBOT_SOCIAL_TRAIT_MAX
                                  : candidate.effectiveDisposition;
    uint8 const relevance = candidate.contentRelevance > PLAYERBOT_SOCIAL_TRAIT_MAX ? PLAYERBOT_SOCIAL_TRAIT_MAX
                                                                                    : candidate.contentRelevance;

    int32 score = static_cast<int32>(disposition) + static_cast<int32>(relevance) / 4;
    if (candidate.addressedByName)
        score += PLAYERBOT_SOCIAL_SELECTION_NAMED_BONUS;

    if (candidate.askedQuestion)
        score += PLAYERBOT_SOCIAL_SELECTION_QUESTION_BONUS;

    if (candidate.participatedInThread)
        score += PLAYERBOT_SOCIAL_SELECTION_PARTICIPATED_BONUS;

    return score;
}

void RecordSuppression(PlayerbotSocialSelection& selection, uint64 botGuidCounter,
                       PlayerbotSocialSuppressionReason reason)
{
    if (selection.suppressions.size() >= PLAYERBOT_SOCIAL_MAX_SELECTION_SUPPRESSIONS)
        return;

    PlayerbotSocialSuppression suppression;
    suppression.botGuidCounter = botGuidCounter;
    suppression.reason = reason;
    selection.suppressions.push_back(suppression);
}

void RecordFactor(PlayerbotSocialSelection& selection, char const* name, int32 contribution)
{
    if (selection.leadingFactors.size() >= PLAYERBOT_SOCIAL_MAX_SELECTION_FACTORS)
        return;

    PlayerbotSocialSelectionFactor factor;
    factor.name = name;
    factor.contribution = contribution;
    selection.leadingFactors.push_back(factor);
}
}  // namespace

PlayerbotSocialSelection PlayerbotSocialSelectResponders(PlayerbotSocialSelectionInput const& input)
{
    PlayerbotSocialSelection selection;

    std::vector<ScoredCandidate> scored;
    scored.reserve(input.candidates.size());

    for (PlayerbotSocialCandidate const& candidate : input.candidates)
    {
        // A candidate list assembled from overlapping sources can repeat a bot. Dropping the repeat
        // here is what keeps one bot from answering itself on the second responder path.
        bool duplicate = false;
        for (ScoredCandidate const& existing : scored)
            if (existing.botGuidCounter == candidate.botGuidCounter)
                duplicate = true;

        if (duplicate)
        {
            RecordSuppression(selection, candidate.botGuidCounter,
                              PlayerbotSocialSuppressionReason::DuplicateCandidate);
            continue;
        }

        /*
         * Validity is checked before anything reads the stance. Testing only for Dismissive would
         * fail open: an unknown value is not equal to it, so a corrupt candidate would fall straight
         * through into scoring and could be selected to speak.
         */
        if (!PlayerbotSocialStanceIsValid(candidate.stance))
        {
            RecordSuppression(selection, candidate.botGuidCounter, PlayerbotSocialSuppressionReason::InvalidStance);
            continue;
        }

        // Hostility is checked before interest. A bot that dislikes the subject declines in
        // character however loud the thread is, and the reason has to say so.
        if (candidate.stance == PlayerbotSocialStance::Dismissive)
        {
            RecordSuppression(selection, candidate.botGuidCounter, PlayerbotSocialSuppressionReason::HostileStance);
            continue;
        }

        int32 const score = InterestScore(candidate);
        if (score < PLAYERBOT_SOCIAL_SELECTION_INTEREST_FLOOR)
        {
            RecordSuppression(selection, candidate.botGuidCounter, PlayerbotSocialSuppressionReason::Uninterested);
            continue;
        }

        ScoredCandidate entry;
        entry.botGuidCounter = candidate.botGuidCounter;
        entry.score = score;
        entry.tiebreak =
            PlayerbotPersonality::SplitMix64(input.selectionSeed ^ candidate.botGuidCounter ^ SELECTION_NAMESPACE);
        entry.addressedByName = candidate.addressedByName;
        entry.askedQuestion = candidate.askedQuestion;
        entry.participatedInThread = candidate.participatedInThread;
        scored.push_back(entry);
    }

    // Highest score first, then a seeded tiebreak. The tiebreak is what stops equally disposed bots
    // resolving to the same one forever, without making the outcome unreproducible.
    std::sort(scored.begin(), scored.end(),
              [](ScoredCandidate const& left, ScoredCandidate const& right)
              {
                  if (left.score != right.score)
                      return left.score > right.score;

                  return left.tiebreak > right.tiebreak;
              });

    for (ScoredCandidate const& entry : scored)
    {
        if (selection.alternates.size() >= PLAYERBOT_SOCIAL_MAX_SELECTION_ALTERNATES)
            break;

        selection.alternates.push_back(entry.botGuidCounter);
    }

    if (scored.empty())
        return selection;

    // Pressure gates the roll, not the scoring: the field is recorded either way, which is what lets
    // the Medivh feed show a considered but silent opportunity.
    float const pressure = PlayerbotSocialDetail::ClampToRange(
        PlayerbotSocialDetail::NeutralIfNotANumber(input.replyPressure, 0.0f), 0.0f, 1.0f);

    if (SeededRoll(input.selectionSeed, 1) >= pressure)
        return selection;

    ScoredCandidate const& leader = scored.front();
    selection.responders.push_back(leader.botGuidCounter);

    RecordFactor(selection, "disposition", leader.score);
    if (leader.addressedByName)
        RecordFactor(selection, "addressed_by_name", PLAYERBOT_SOCIAL_SELECTION_NAMED_BONUS);

    if (leader.askedQuestion)
        RecordFactor(selection, "asked_question", PLAYERBOT_SOCIAL_SELECTION_QUESTION_BONUS);

    if (leader.participatedInThread)
        RecordFactor(selection, "participated_in_thread", PLAYERBOT_SOCIAL_SELECTION_PARTICIPATED_BONUS);

    /*
     * The second responder is occasional and coherent: only on a path the caller has already
     * confirmed shares the thread and delivery context, and only when a separate roll succeeds
     * against a fraction of the same pressure. It stays bounded at two responders.
     */
    if (input.secondResponderAllowed && scored.size() > 1 &&
        selection.responders.size() < PLAYERBOT_SOCIAL_MAX_RESPONDERS)
    {
        constexpr float SECOND_RESPONDER_SHARE = 0.35f;
        if (SeededRoll(input.selectionSeed, 2) < pressure * SECOND_RESPONDER_SHARE)
            selection.responders.push_back(scored[1].botGuidCounter);
    }

    for (std::size_t index = selection.responders.size(); index < scored.size(); ++index)
        RecordSuppression(selection, scored[index].botGuidCounter, PlayerbotSocialSuppressionReason::LostToHigherScore);

    return selection;
}

uint8 PlayerbotSocialEngagementDisposition(uint8 sociability, uint8 warmth,
                                           PlayerbotSocialRelationshipValues const& toward)
{
    // A corrupt stored trait or relationship row must not be able to move the score outside the
    // documented range, so both are normalized before any arithmetic runs.
    float const ownWillingness =
        (static_cast<float>(ClampTrait(sociability)) + static_cast<float>(ClampTrait(warmth))) * 0.5f;

    PlayerbotSocialRelationshipValues const values = PlayerbotSocialClampRelationship(toward);
    float const affinityWeight =
        values.affinity >= 0.0f ? PLAYERBOT_SOCIAL_AFFINITY_POSITIVE_WEIGHT : PLAYERBOT_SOCIAL_AFFINITY_NEGATIVE_WEIGHT;

    float const relationshipShift = values.affinity * affinityWeight + values.trust * PLAYERBOT_SOCIAL_TRUST_WEIGHT +
                                    values.familiarity * PLAYERBOT_SOCIAL_FAMILIARITY_WEIGHT;

    float const bounded = PlayerbotSocialDetail::ClampToRange(ownWillingness + relationshipShift,
                                                              static_cast<float>(PLAYERBOT_SOCIAL_TRAIT_MIN),
                                                              static_cast<float>(PLAYERBOT_SOCIAL_TRAIT_MAX));

    return RoundBoundedScore(bounded);
}

PlayerbotSocialStance PlayerbotSocialStanceFor(uint8 disposition, PlayerbotSocialRelationshipValues const& toward)
{
    PlayerbotSocialRelationshipValues const values = PlayerbotSocialClampRelationship(toward);

    // Hostility is checked first and on its own axis. A bot that dislikes or distrusts the subject
    // declines in character, however talkative its base personality happens to be.
    if (values.affinity <= PLAYERBOT_SOCIAL_HOSTILE_AFFINITY || values.trust <= PLAYERBOT_SOCIAL_HOSTILE_TRUST)
        return PlayerbotSocialStance::Dismissive;

    if (disposition < PLAYERBOT_SOCIAL_STANCE_NEUTRAL_MIN)
        return PlayerbotSocialStance::Reserved;

    if (disposition < PLAYERBOT_SOCIAL_STANCE_RECEPTIVE_MIN)
        return PlayerbotSocialStance::Neutral;

    if (disposition < PLAYERBOT_SOCIAL_STANCE_ENGAGED_MIN)
        return PlayerbotSocialStance::Receptive;

    return PlayerbotSocialStance::Engaged;
}

char const* PlayerbotSocialStanceName(PlayerbotSocialStance stance)
{
    switch (stance)
    {
        case PlayerbotSocialStance::Dismissive:
            return "dismissive";
        case PlayerbotSocialStance::Reserved:
            return "reserved";
        case PlayerbotSocialStance::Neutral:
            return "neutral";
        case PlayerbotSocialStance::Receptive:
            return "receptive";
        case PlayerbotSocialStance::Engaged:
            return "engaged";
    }

    return "unknown";
}

uint8 PlayerbotSocialApplyContextToDisposition(uint8 disposition, bool addressedDirectly, uint8 mood)
{
    int32 adjusted = static_cast<int32>(ClampTrait(disposition));

    if (addressedDirectly)
        adjusted += PLAYERBOT_SOCIAL_DIRECT_ADDRESS_BONUS;

    // A corrupt mood is normalized to the same range as every other bounded score, so an out of
    // range value shifts the result no further than a legitimate extreme would.
    adjusted += (static_cast<int32>(ClampTrait(mood)) - static_cast<int32>(PLAYERBOT_SOCIAL_MOOD_NEUTRAL)) /
                PLAYERBOT_SOCIAL_MOOD_DIVISOR;

    if (adjusted < PLAYERBOT_SOCIAL_TRAIT_MIN)
        return PLAYERBOT_SOCIAL_TRAIT_MIN;

    if (adjusted > PLAYERBOT_SOCIAL_TRAIT_MAX)
        return PLAYERBOT_SOCIAL_TRAIT_MAX;

    return static_cast<uint8>(adjusted);
}

bool PlayerbotSocialAddEvolvingTopic(std::vector<std::string>& topics, std::string const& topic)
{
    if (topic.empty() || topic.size() > PLAYERBOT_SOCIAL_MAX_TOPIC_LENGTH)
        return false;

    if (topics.size() >= PLAYERBOT_SOCIAL_MAX_EVOLVING_TOPICS)
        return false;

    for (std::string const& existing : topics)
    {
        if (existing == topic)
            return false;
    }

    topics.push_back(topic);
    return true;
}

std::vector<std::string> PlayerbotSocialBoundEvolvingTopics(std::vector<std::string> const& topics)
{
    std::vector<std::string> bounded;
    bounded.reserve(topics.size() < PLAYERBOT_SOCIAL_MAX_EVOLVING_TOPICS ? topics.size()
                                                                         : PLAYERBOT_SOCIAL_MAX_EVOLVING_TOPICS);

    // Reuse the add rule rather than restating it, so there is exactly one definition of what a
    // usable topic is and the load path cannot drift from the write path.
    for (std::string const& topic : topics)
    {
        if (bounded.size() >= PLAYERBOT_SOCIAL_MAX_EVOLVING_TOPICS)
            break;

        (void)PlayerbotSocialAddEvolvingTopic(bounded, topic);
    }

    return bounded;
}

uint8 PlayerbotSocialEvolveTrait(uint8 current, int32 proposedDelta)
{
    // Clamp the proposal before it is used in any arithmetic. Doing it here rather than negating
    // the delta later also keeps the most negative int32 from overflowing on negation.
    int32 const step = proposedDelta > PLAYERBOT_SOCIAL_TRAIT_MAX_STEP    ? PLAYERBOT_SOCIAL_TRAIT_MAX_STEP
                       : proposedDelta < -PLAYERBOT_SOCIAL_TRAIT_MAX_STEP ? -PLAYERBOT_SOCIAL_TRAIT_MAX_STEP
                                                                          : proposedDelta;

    int32 const evolved = static_cast<int32>(ClampTrait(current)) + step;

    if (evolved < PLAYERBOT_SOCIAL_TRAIT_MIN)
        return PLAYERBOT_SOCIAL_TRAIT_MIN;

    if (evolved > PLAYERBOT_SOCIAL_TRAIT_MAX)
        return PLAYERBOT_SOCIAL_TRAIT_MAX;

    return static_cast<uint8>(evolved);
}

bool PlayerbotSocialTraitEvolutionIsDue(uint64 lastAcceptedAtUnixSeconds, uint64 nowUnixSeconds)
{
    // Unsigned subtraction wraps, so the backward clock case is rejected explicitly rather than
    // being allowed to produce an enormous elapsed time.
    if (nowUnixSeconds < lastAcceptedAtUnixSeconds)
        return false;

    return nowUnixSeconds - lastAcceptedAtUnixSeconds >= PLAYERBOT_SOCIAL_TRAIT_MIN_INTERVAL_SECONDS;
}

PlayerbotSocialTraitEvolution PlayerbotSocialEvolveTraitIfDue(uint8 current, int32 proposedDelta,
                                                              uint64 lastAcceptedAtUnixSeconds, uint64 nowUnixSeconds)
{
    PlayerbotSocialTraitEvolution evolution;

    if (!PlayerbotSocialTraitEvolutionIsDue(lastAcceptedAtUnixSeconds, nowUnixSeconds))
    {
        // A refused proposal must not advance the clock, or a steady stream of them would keep
        // pushing the next opportunity out. The value is still normalized: both branches of this
        // entry point report a trait inside the range, so a corrupt stored value cannot circulate
        // just because the proposal that carried it was refused.
        evolution.applied = false;
        evolution.value = ClampTrait(current);
        evolution.lastAcceptedAtUnixSeconds = lastAcceptedAtUnixSeconds;
        return evolution;
    }

    evolution.applied = true;
    evolution.value = PlayerbotSocialEvolveTrait(current, proposedDelta);
    evolution.lastAcceptedAtUnixSeconds = nowUnixSeconds;
    return evolution;
}

bool PlayerbotSocialMessageIsQuestion(std::string_view message) { return message.find('?') != std::string_view::npos; }

// Assistance evidence ------------------------------------------------------------------------------

PlayerbotSocialRelationshipValues PlayerbotSocialAssistanceDelta(PlayerbotSocialAssistanceTally const& tally,
                                                                 uint32 beneficiaryMaxHealth)
{
    PlayerbotSocialRelationshipValues delta;

    // No scale to measure against, so no defensible number to produce. Earning nothing is the only
    // answer that cannot be wrong.
    if (beneficiaryMaxHealth == 0)
        return delta;

    // The contribution is expressed in units of "one full health bar's worth of help", so that the
    // same real assistance earns the same credit at every level. Both halves are capped at one
    // before they are added, which is what stops a long fight from paying out without bound.
    float const scale = static_cast<float>(beneficiaryMaxHealth);
    float const healingShare = std::min(1.0f, static_cast<float>(tally.effectiveHealing) / scale);
    float const damageShare = std::min(1.0f, static_cast<float>(tally.meaningfulDamage) / scale);
    float const contribution = std::min(1.0f, healingShare + damageShare);

    if (contribution <= 0.0f && tally.rescueCount == 0)
        return delta;

    delta.familiarity = PLAYERBOT_SOCIAL_ASSISTANCE_FAMILIARITY_CAP * contribution;
    delta.affinity = PLAYERBOT_SOCIAL_ASSISTANCE_AFFINITY_CAP * contribution;

    uint32 const rescues = std::min(tally.rescueCount, PLAYERBOT_SOCIAL_ASSISTANCE_RESCUE_CAP);
    delta.trust = PLAYERBOT_SOCIAL_ASSISTANCE_TRUST_CAP *
                  (static_cast<float>(rescues) / static_cast<float>(PLAYERBOT_SOCIAL_ASSISTANCE_RESCUE_CAP));

    return delta;
}

PlayerbotSocialRelationshipValues PlayerbotSocialOpenWorldAggressionDelta()
{
    PlayerbotSocialRelationshipValues delta;

    // Being attacked makes someone memorable, so familiarity rises while regard falls. A bot that
    // has been ganked knows exactly who did it.
    delta.familiarity = PLAYERBOT_SOCIAL_AGGRESSION_FAMILIARITY_GAIN;
    delta.affinity = -PLAYERBOT_SOCIAL_AGGRESSION_AFFINITY_PENALTY;
    delta.trust = -PLAYERBOT_SOCIAL_AGGRESSION_TRUST_PENALTY;

    return delta;
}

namespace
{
/*
 * Admits one axis against its ceiling and records what was spent.
 *
 * A penalty passes straight through: the ledger rations gains, because gains are what an
 * encounter boundary could be made to repeat. Letting a penalty through unrationed is also the
 * safer direction, since the stored value is held at its floor by the database.
 */
float AdmitAxis(float& spent, float cap, float requested)
{
    if (requested <= 0.0f)
        return requested;

    float const remaining = cap - spent;
    if (remaining <= 0.0f)
        return 0.0f;

    float const admitted = std::min(requested, remaining);
    spent += admitted;
    return admitted;
}
}  // namespace

PlayerbotSocialRelationshipValues PlayerbotSocialAdmitAssistanceCredit(PlayerbotSocialAssistanceCredit& credit,
                                                                       PlayerbotSocialRelationshipValues const& delta,
                                                                       uint64 nowUnixSeconds)
{
    /*
     * A window that has fully elapsed starts again with nothing spent. A clock that moves backwards
     * does not, because opening a fresh window on a rewind is precisely the move that would let the
     * ceiling be paid twice. A ledger that has never paid anything says so with its own flag rather
     * than with a zero timestamp, because zero is a valid second.
     */
    if (!credit.windowStarted ||
        (nowUnixSeconds >= credit.windowStartedAtUnixSeconds &&
         nowUnixSeconds - credit.windowStartedAtUnixSeconds >= PLAYERBOT_SOCIAL_ASSISTANCE_PAYOUT_WINDOW_SECONDS))
    {
        credit.windowStarted = true;
        credit.windowStartedAtUnixSeconds = nowUnixSeconds;
        credit.familiarity = 0.0f;
        credit.affinity = 0.0f;
        credit.trust = 0.0f;
    }

    PlayerbotSocialRelationshipValues admitted;
    admitted.familiarity =
        AdmitAxis(credit.familiarity, PLAYERBOT_SOCIAL_ASSISTANCE_FAMILIARITY_CAP, delta.familiarity);
    admitted.affinity = AdmitAxis(credit.affinity, PLAYERBOT_SOCIAL_ASSISTANCE_AFFINITY_CAP, delta.affinity);
    admitted.trust = AdmitAxis(credit.trust, PLAYERBOT_SOCIAL_ASSISTANCE_TRUST_CAP, delta.trust);

    return admitted;
}

bool PlayerbotSocialAssistanceCreditIsExpired(PlayerbotSocialAssistanceCredit const& credit, uint64 nowUnixSeconds)
{
    // A ledger that has never paid anything holds no ceiling, so it is droppable on sight. Otherwise
    // it stops mattering once its window has fully elapsed, and a rewound clock keeps it rather than
    // expiring it early.
    if (!credit.windowStarted)
        return true;

    if (nowUnixSeconds < credit.windowStartedAtUnixSeconds)
        return false;

    return nowUnixSeconds - credit.windowStartedAtUnixSeconds >= PLAYERBOT_SOCIAL_ASSISTANCE_PAYOUT_WINDOW_SECONDS;
}

PlayerbotSocialRelationshipValues PlayerbotSocialAdmitWithoutLedger(PlayerbotSocialRelationshipValues const& delta)
{
    /*
     * Penalties through, gains refused.
     *
     * A gain needs a ledger entry to be bounded, and there is none, so granting it would be
     * unbounded credit. A penalty is never rationed by the ledger in the first place, so withholding
     * it here would change behaviour for no benefit and would let cardinality pressure become a way
     * to avoid consequences.
     */
    PlayerbotSocialRelationshipValues admitted;
    admitted.familiarity = std::min(0.0f, delta.familiarity);
    admitted.affinity = std::min(0.0f, delta.affinity);
    admitted.trust = std::min(0.0f, delta.trust);
    return admitted;
}

bool PlayerbotSocialOppositionIsSameFight(uint64 markerAtUnixSeconds, uint64 nowUnixSeconds)
{
    uint64 const elapsed = nowUnixSeconds > markerAtUnixSeconds ? nowUnixSeconds - markerAtUnixSeconds : 0;
    return elapsed < PLAYERBOT_SOCIAL_ENCOUNTER_IDLE_SECONDS;
}

uint64 PlayerbotSocialAdvanceOppositionMarker(uint64 markerAtUnixSeconds, uint64 nowUnixSeconds)
{
    return std::max(markerAtUnixSeconds, nowUnixSeconds);
}

// Roleplay affinity --------------------------------------------------------------------------------

namespace
{
// A distinct namespace so this transient roll does not share the social selection stream.
constexpr uint64 ROLEPLAY_WILLINGNESS_NAMESPACE = 0x524C505F57494C31ULL;  // "RLP_WIL1"

/*
 * Uniform sample on [0, range) by rejection: accept a 64 bit output only when it is below the
 * largest representable multiple of the range, advancing SplitMix64 on every rejection. This is
 * what gives every score equal selection opportunity; a bare modulo would bias the low scores.
 */
uint64 UniformRoleplaySample(uint64 state, uint64 range)
{
    uint64 const rejected = (UINT64_MAX % range + 1) % range;
    uint64 sample = PlayerbotPersonality::SplitMix64(state);
    while (rejected != 0 && sample > UINT64_MAX - rejected)
        sample = PlayerbotPersonality::SplitMix64(sample);
    return sample % range;
}
}  // namespace

PlayerbotRoleplayAffinityBand PlayerbotRoleplayAffinityBandFor(uint8 affinity)
{
    if (affinity < 1 || affinity > 100)
        return PlayerbotRoleplayAffinityBand::Averse;

    if (affinity <= 50)
        return PlayerbotRoleplayAffinityBand::Averse;
    if (affinity <= 75)
        return PlayerbotRoleplayAffinityBand::Neutral;
    if (affinity <= 89)
        return PlayerbotRoleplayAffinityBand::Receptive;

    return PlayerbotRoleplayAffinityBand::Enthusiast;
}

uint8 PlayerbotRoleplayWillingnessRoll(uint64 activationSeed, uint64 botGuidCounter, uint32 willingnessVersion)
{
    uint64 const base = PlayerbotPersonality::SplitMix64(ROLEPLAY_WILLINGNESS_NAMESPACE ^ willingnessVersion);
    uint64 const mixed = PlayerbotPersonality::SplitMix64(base ^ activationSeed) ^ botGuidCounter;
    return static_cast<uint8>(UniformRoleplaySample(mixed, 25) + 1);
}

bool PlayerbotRoleplayWillingnessPasses(uint8 affinity, uint8 roll)
{
    if (affinity <= 75 || affinity > 100 || roll < 1 || roll > 25)
        return false;

    return roll <= affinity - 75;
}

char const* PlayerbotRoleplayAffinityBandName(PlayerbotRoleplayAffinityBand band)
{
    switch (band)
    {
        case PlayerbotRoleplayAffinityBand::Averse:
            return "averse";
        case PlayerbotRoleplayAffinityBand::Neutral:
            return "neutral";
        case PlayerbotRoleplayAffinityBand::Receptive:
            return "receptive";
        case PlayerbotRoleplayAffinityBand::Enthusiast:
            return "enthusiast";
    }

    return "unknown";
}

char const* PlayerbotRoleplayAssessmentKindName(PlayerbotRoleplayAssessmentKind kind)
{
    switch (kind)
    {
        case PlayerbotRoleplayAssessmentKind::Ordinary:
            return "ordinary";
        case PlayerbotRoleplayAssessmentKind::RoleplayInvitation:
            return "roleplay_invitation";
        case PlayerbotRoleplayAssessmentKind::RoleplayContinuation:
            return "roleplay_continuation";
        case PlayerbotRoleplayAssessmentKind::Practical:
            return "practical";
        case PlayerbotRoleplayAssessmentKind::OptOut:
            return "opt_out";
        case PlayerbotRoleplayAssessmentKind::Uncertain:
            return "uncertain";
    }

    return "unknown";
}

char const* PlayerbotRoleplayPromptModeName(PlayerbotRoleplayPromptMode mode)
{
    switch (mode)
    {
        case PlayerbotRoleplayPromptMode::Ordinary:
            return "ordinary";
        case PlayerbotRoleplayPromptMode::DeclineRoleplay:
            return "decline_roleplay";
        case PlayerbotRoleplayPromptMode::AcknowledgeRoleplay:
            return "acknowledge_roleplay";
        case PlayerbotRoleplayPromptMode::AuthorizedRoleplay:
            return "authorized_roleplay";
    }

    return "unknown";
}
