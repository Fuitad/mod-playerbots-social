/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#include <algorithm>
#include <cstdint>
#include <iterator>
#include <limits>
#include <string>
#include <vector>

#include "Bot/Social/PlayerbotSocialPolicy.h"
#include "gtest/gtest.h"

namespace
{
PlayerbotSocialRelationshipValues MakeRelationship(float familiarity, float affinity, float trust)
{
    PlayerbotSocialRelationshipValues values;
    values.familiarity = familiarity;
    values.affinity = affinity;
    values.trust = trust;
    return values;
}

constexpr uint8 NEUTRAL_TRAIT = 50;
}  // namespace

// Engagement disposition ---------------------------------------------------------------------------

TEST(PlayerbotSocialPolicyTest, NeutralBotTowardStrangerSitsAtTheMidpoint)
{
    // A stranger is neutral on every axis, so a middling bot lands exactly in the middle. This is
    // the reference point every other expectation below is measured against.
    uint8 const disposition =
        PlayerbotSocialEngagementDisposition(NEUTRAL_TRAIT, NEUTRAL_TRAIT, MakeRelationship(0.0f, 0.0f, 0.0f));
    EXPECT_EQ(disposition, 50u);
}

TEST(PlayerbotSocialPolicyTest, DispositionIsDirectional)
{
    // The same pair of characters can hold opposite views of each other. Only the values the bot
    // itself holds toward the subject may move its disposition.
    uint8 const warmView =
        PlayerbotSocialEngagementDisposition(NEUTRAL_TRAIT, NEUTRAL_TRAIT, MakeRelationship(1.0f, 1.0f, 1.0f));
    uint8 const coldView =
        PlayerbotSocialEngagementDisposition(NEUTRAL_TRAIT, NEUTRAL_TRAIT, MakeRelationship(1.0f, -1.0f, -1.0f));
    EXPECT_GT(warmView, coldView);
    EXPECT_EQ(warmView, 100u);
    EXPECT_EQ(coldView, 0u);
}

TEST(PlayerbotSocialPolicyTest, UnsociableBotStaysQuietTowardAStranger)
{
    uint8 const disposition = PlayerbotSocialEngagementDisposition(0, 0, MakeRelationship(0.0f, 0.0f, 0.0f));
    EXPECT_EQ(disposition, 0u);
    EXPECT_EQ(PlayerbotSocialStanceFor(disposition, MakeRelationship(0.0f, 0.0f, 0.0f)),
              PlayerbotSocialStance::Reserved);
}

TEST(PlayerbotSocialPolicyTest, FamiliaritySeparatesAcquaintanceFromStranger)
{
    // Same bot, same feelings, different amount of shared history. More history means more
    // willingness to speak up.
    uint8 const stranger =
        PlayerbotSocialEngagementDisposition(NEUTRAL_TRAIT, NEUTRAL_TRAIT, MakeRelationship(0.0f, 0.2f, 0.2f));
    uint8 const acquaintance =
        PlayerbotSocialEngagementDisposition(NEUTRAL_TRAIT, NEUTRAL_TRAIT, MakeRelationship(1.0f, 0.2f, 0.2f));
    EXPECT_GT(acquaintance, stranger);
}

TEST(PlayerbotSocialPolicyTest, DispositionStaysWithinBoundsForExtremeInputs)
{
    // Out of range and non finite relationship values are clamped before they reach the arithmetic,
    // so no combination can produce a disposition outside the byte range the callers rely on.
    float const probes[] = {-1000.0f,
                            -1.0f,
                            0.0f,
                            1.0f,
                            1000.0f,
                            std::numeric_limits<float>::quiet_NaN(),
                            std::numeric_limits<float>::infinity(),
                            -std::numeric_limits<float>::infinity()};
    uint8 const traits[] = {0u, 50u, 100u, 255u};

    for (uint8 sociability : traits)
    {
        for (uint8 warmth : traits)
        {
            for (float familiarity : probes)
            {
                for (float affinity : probes)
                {
                    for (float trust : probes)
                    {
                        uint8 const disposition = PlayerbotSocialEngagementDisposition(
                            sociability, warmth, MakeRelationship(familiarity, affinity, trust));
                        ASSERT_LE(disposition, 100u);
                    }
                }
            }
        }
    }
}

// Stance -------------------------------------------------------------------------------------------

TEST(PlayerbotSocialPolicyTest, HostileRelationshipDismissesRegardlessOfHowTalkativeTheBotIs)
{
    // Declining is in character. Even the most sociable bot dismisses someone it actively dislikes,
    // and that decision must not be washed out by a high disposition score.
    PlayerbotSocialRelationshipValues const hostile = MakeRelationship(1.0f, -0.9f, -0.9f);
    uint8 const disposition = PlayerbotSocialEngagementDisposition(100, 100, hostile);
    EXPECT_EQ(PlayerbotSocialStanceFor(disposition, hostile), PlayerbotSocialStance::Dismissive);
}

TEST(PlayerbotSocialPolicyTest, DistrustAloneIsEnoughToDismiss)
{
    PlayerbotSocialRelationshipValues const distrusted = MakeRelationship(1.0f, 0.4f, -0.9f);
    uint8 const disposition = PlayerbotSocialEngagementDisposition(100, 100, distrusted);
    EXPECT_EQ(PlayerbotSocialStanceFor(disposition, distrusted), PlayerbotSocialStance::Dismissive);
}

TEST(PlayerbotSocialPolicyTest, ReservedIsDistinctFromDismissive)
{
    // A quiet bot toward a stranger is Reserved, not Dismissive. Dismissive means the bot holds an
    // actively negative view, so a shy bot must never be reported as hostile.
    PlayerbotSocialRelationshipValues const stranger = MakeRelationship(0.0f, 0.0f, 0.0f);
    EXPECT_EQ(PlayerbotSocialStanceFor(PlayerbotSocialEngagementDisposition(0, 0, stranger), stranger),
              PlayerbotSocialStance::Reserved);
}

TEST(PlayerbotSocialPolicyTest, StanceRisesWithDispositionForAWellDisposedSubject)
{
    PlayerbotSocialRelationshipValues const friendly = MakeRelationship(0.5f, 0.5f, 0.5f);
    EXPECT_EQ(PlayerbotSocialStanceFor(0, friendly), PlayerbotSocialStance::Reserved);
    EXPECT_EQ(PlayerbotSocialStanceFor(25, friendly), PlayerbotSocialStance::Neutral);
    EXPECT_EQ(PlayerbotSocialStanceFor(50, friendly), PlayerbotSocialStance::Receptive);
    EXPECT_EQ(PlayerbotSocialStanceFor(75, friendly), PlayerbotSocialStance::Engaged);
    EXPECT_EQ(PlayerbotSocialStanceFor(100, friendly), PlayerbotSocialStance::Engaged);
}

// Bounded trait evolution --------------------------------------------------------------------------

TEST(PlayerbotSocialPolicyTest, OneProposalCannotRewriteATrait)
{
    // The single most important property of trait evolution: no single accepted proposal, however
    // large, may move a trait more than one bounded step. A model that returns an absurd delta
    // still only nudges the bot.
    EXPECT_EQ(PlayerbotSocialEvolveTrait(50, 1000), 50 + PLAYERBOT_SOCIAL_TRAIT_MAX_STEP);
    EXPECT_EQ(PlayerbotSocialEvolveTrait(50, -1000), 50 - PLAYERBOT_SOCIAL_TRAIT_MAX_STEP);
    EXPECT_EQ(PlayerbotSocialEvolveTrait(50, std::numeric_limits<int32>::max()), 50 + PLAYERBOT_SOCIAL_TRAIT_MAX_STEP);
    EXPECT_EQ(PlayerbotSocialEvolveTrait(50, std::numeric_limits<int32>::min()), 50 - PLAYERBOT_SOCIAL_TRAIT_MAX_STEP);
}

TEST(PlayerbotSocialPolicyTest, RepeatedInteractionsEvolveATraitSlowly)
{
    // Many independent accepted proposals do move the bot, which is the other half of the contract:
    // bounded per step, but not frozen.
    uint8 trait = 50;
    for (int i = 0; i < 10; ++i)
        trait = PlayerbotSocialEvolveTrait(trait, 5);

    EXPECT_EQ(trait, 50 + 10 * PLAYERBOT_SOCIAL_TRAIT_MAX_STEP);
}

TEST(PlayerbotSocialPolicyTest, TraitEvolutionSaturatesAtTheBounds)
{
    uint8 trait = 0;
    for (int i = 0; i < 200; ++i)
        trait = PlayerbotSocialEvolveTrait(trait, 100);
    EXPECT_EQ(trait, 100u);

    for (int i = 0; i < 200; ++i)
        trait = PlayerbotSocialEvolveTrait(trait, -100);
    EXPECT_EQ(trait, 0u);
}

TEST(PlayerbotSocialPolicyTest, ZeroDeltaLeavesTheTraitUntouched)
{
    for (uint32 value = 0; value <= 100; ++value)
        EXPECT_EQ(PlayerbotSocialEvolveTrait(static_cast<uint8>(value), 0), value);
}

TEST(PlayerbotSocialPolicyTest, OutOfRangeStoredTraitIsBroughtBackIntoRange)
{
    // A corrupt or hand edited stored trait must not stay out of range once it passes through the
    // evolution path.
    EXPECT_EQ(PlayerbotSocialEvolveTrait(200, 0), 100u);
}

TEST(PlayerbotSocialPolicyTest, TraitEvolutionRespectsItsMinimumInterval)
{
    uint64 const lastAccepted = 1000000;
    EXPECT_FALSE(PlayerbotSocialTraitEvolutionIsDue(lastAccepted, lastAccepted));
    EXPECT_FALSE(PlayerbotSocialTraitEvolutionIsDue(lastAccepted,
                                                    lastAccepted + PLAYERBOT_SOCIAL_TRAIT_MIN_INTERVAL_SECONDS - 1));
    EXPECT_TRUE(
        PlayerbotSocialTraitEvolutionIsDue(lastAccepted, lastAccepted + PLAYERBOT_SOCIAL_TRAIT_MIN_INTERVAL_SECONDS));
}

TEST(PlayerbotSocialPolicyTest, TraitEvolutionFailsClosedOnABackwardClock)
{
    // A clock that moved backwards must not be readable as a very old last change, which would
    // otherwise let a burst of proposals through.
    uint64 const lastAccepted = 1000000;
    EXPECT_FALSE(PlayerbotSocialTraitEvolutionIsDue(lastAccepted, lastAccepted - 1));
    EXPECT_FALSE(PlayerbotSocialTraitEvolutionIsDue(lastAccepted, 0));
}

TEST(PlayerbotSocialPolicyTest, NeverEvolvedTraitIsImmediatelyDue)
{
    // A profile that has never accepted a proposal stores zero, which must not be treated as a
    // recent change.
    EXPECT_TRUE(PlayerbotSocialTraitEvolutionIsDue(0, PLAYERBOT_SOCIAL_TRAIT_MIN_INTERVAL_SECONDS));
    EXPECT_TRUE(PlayerbotSocialTraitEvolutionIsDue(0, 1700000000));
}

// Transient context ---------------------------------------------------------------------------------

TEST(PlayerbotSocialPolicyTest, NeutralContextLeavesTheDispositionAlone)
{
    EXPECT_EQ(PlayerbotSocialApplyContextToDisposition(50, false, PLAYERBOT_SOCIAL_MOOD_NEUTRAL), 50u);
}

TEST(PlayerbotSocialPolicyTest, BeingAddressedDirectlyRaisesTheDisposition)
{
    EXPECT_EQ(PlayerbotSocialApplyContextToDisposition(50, true, PLAYERBOT_SOCIAL_MOOD_NEUTRAL),
              50 + PLAYERBOT_SOCIAL_DIRECT_ADDRESS_BONUS);
}

TEST(PlayerbotSocialPolicyTest, MoodShiftsTheDispositionSymmetrically)
{
    uint8 const high = PlayerbotSocialApplyContextToDisposition(50, false, 100);
    uint8 const low = PlayerbotSocialApplyContextToDisposition(50, false, 0);
    EXPECT_GT(high, 50u);
    EXPECT_LT(low, 50u);
    EXPECT_EQ(high - 50, 50 - low);
}

TEST(PlayerbotSocialPolicyTest, ContextCannotPushTheDispositionOutOfRange)
{
    EXPECT_EQ(PlayerbotSocialApplyContextToDisposition(100, true, 100), 100u);
    EXPECT_EQ(PlayerbotSocialApplyContextToDisposition(0, false, 0), 0u);

    // A corrupt mood value is normalized rather than allowed to distort the score.
    EXPECT_EQ(PlayerbotSocialApplyContextToDisposition(50, false, 255),
              PlayerbotSocialApplyContextToDisposition(50, false, 100));
    EXPECT_LE(PlayerbotSocialApplyContextToDisposition(200, true, 255), 100u);
}

// Bounded evolving topics ---------------------------------------------------------------------------

TEST(PlayerbotSocialPolicyTest, EvolvingTopicsAcceptUntilTheCap)
{
    std::vector<std::string> topics;
    for (std::size_t i = 0; i < PLAYERBOT_SOCIAL_MAX_EVOLVING_TOPICS; ++i)
        EXPECT_TRUE(PlayerbotSocialAddEvolvingTopic(topics, "topic" + std::to_string(i)));

    EXPECT_EQ(topics.size(), PLAYERBOT_SOCIAL_MAX_EVOLVING_TOPICS);
    EXPECT_FALSE(PlayerbotSocialAddEvolvingTopic(topics, "one too many"));
    EXPECT_EQ(topics.size(), PLAYERBOT_SOCIAL_MAX_EVOLVING_TOPICS);
}

TEST(PlayerbotSocialPolicyTest, EvolvingTopicsRejectDuplicatesAndUnusableValues)
{
    std::vector<std::string> topics;
    EXPECT_TRUE(PlayerbotSocialAddEvolvingTopic(topics, "fishing"));

    // Repetition must not let one interest occupy several slots.
    EXPECT_FALSE(PlayerbotSocialAddEvolvingTopic(topics, "fishing"));
    EXPECT_FALSE(PlayerbotSocialAddEvolvingTopic(topics, ""));
    EXPECT_FALSE(PlayerbotSocialAddEvolvingTopic(topics, std::string(PLAYERBOT_SOCIAL_MAX_TOPIC_LENGTH + 1, 'a')));
    EXPECT_EQ(topics.size(), 1u);

    EXPECT_TRUE(PlayerbotSocialAddEvolvingTopic(topics, std::string(PLAYERBOT_SOCIAL_MAX_TOPIC_LENGTH, 'a')));
    EXPECT_EQ(topics.size(), 2u);
}

// Diagnostic names ----------------------------------------------------------------------------------

TEST(PlayerbotSocialPolicyTest, StanceNamesAreStableForTelemetry)
{
    // These strings reach durable telemetry and the operator UI, so they are part of the contract
    // and not free to reword.
    EXPECT_STREQ(PlayerbotSocialStanceName(PlayerbotSocialStance::Dismissive), "dismissive");
    EXPECT_STREQ(PlayerbotSocialStanceName(PlayerbotSocialStance::Reserved), "reserved");
    EXPECT_STREQ(PlayerbotSocialStanceName(PlayerbotSocialStance::Neutral), "neutral");
    EXPECT_STREQ(PlayerbotSocialStanceName(PlayerbotSocialStance::Receptive), "receptive");
    EXPECT_STREQ(PlayerbotSocialStanceName(PlayerbotSocialStance::Engaged), "engaged");
}

TEST(PlayerbotSocialPolicyTest, UnknownStanceReportsAnUnknownName)
{
    // A value cast in from a corrupt payload must produce a safe label rather than read past the
    // end of the switch.
    EXPECT_STREQ(PlayerbotSocialStanceName(static_cast<PlayerbotSocialStance>(200)), "unknown");
}

// Evolution guarded by its own interval ---------------------------------------------------------------

TEST(PlayerbotSocialPolicyTest, GuardedEvolutionRefusesASecondChangeInTheSameInterval)
{
    // The interval and the step must not be usable apart. A burst of proposals inside one
    // conversation reaches this entry point and only the first of them can move the trait.
    uint64 const start = 1000000;
    PlayerbotSocialTraitEvolution first = PlayerbotSocialEvolveTraitIfDue(50, 5, 0, start);
    EXPECT_TRUE(first.applied);
    EXPECT_EQ(first.value, 50 + PLAYERBOT_SOCIAL_TRAIT_MAX_STEP);
    EXPECT_EQ(first.lastAcceptedAtUnixSeconds, start);

    PlayerbotSocialTraitEvolution second =
        PlayerbotSocialEvolveTraitIfDue(first.value, 5, first.lastAcceptedAtUnixSeconds, start);
    EXPECT_FALSE(second.applied);
    EXPECT_EQ(second.value, first.value);
    EXPECT_EQ(second.lastAcceptedAtUnixSeconds, first.lastAcceptedAtUnixSeconds);
}

TEST(PlayerbotSocialPolicyTest, GuardedEvolutionAllowsOneChangePerInterval)
{
    uint64 now = 1000000;
    uint8 trait = 50;
    uint64 lastAccepted = 0;

    for (int i = 0; i < 5; ++i)
    {
        PlayerbotSocialTraitEvolution const step = PlayerbotSocialEvolveTraitIfDue(trait, 100, lastAccepted, now);
        ASSERT_TRUE(step.applied) << "interval " << i;
        trait = step.value;
        lastAccepted = step.lastAcceptedAtUnixSeconds;
        now += PLAYERBOT_SOCIAL_TRAIT_MIN_INTERVAL_SECONDS;
    }

    // Five intervals, five bounded steps. Slow evolution, never a rewrite.
    EXPECT_EQ(trait, 50 + 5 * PLAYERBOT_SOCIAL_TRAIT_MAX_STEP);
}

TEST(PlayerbotSocialPolicyTest, GuardedEvolutionFailsClosedOnABackwardClock)
{
    PlayerbotSocialTraitEvolution const result = PlayerbotSocialEvolveTraitIfDue(50, 5, 1000000, 999999);
    EXPECT_FALSE(result.applied);
    EXPECT_EQ(result.value, 50u);
    EXPECT_EQ(result.lastAcceptedAtUnixSeconds, 1000000u);
}

TEST(PlayerbotSocialPolicyTest, StoredTopicListsAreBroughtWithinTheirBounds)
{
    // A stored row is not written through the add path, so loading has to apply the same bound.
    std::vector<std::string> stored;
    for (std::size_t i = 0; i < PLAYERBOT_SOCIAL_MAX_EVOLVING_TOPICS * 3; ++i)
        stored.push_back("topic" + std::to_string(i));
    stored.push_back("");
    stored.push_back(std::string(PLAYERBOT_SOCIAL_MAX_TOPIC_LENGTH + 1, 'a'));
    stored.push_back("topic0");

    std::vector<std::string> const bounded = PlayerbotSocialBoundEvolvingTopics(stored);

    EXPECT_LE(bounded.size(), PLAYERBOT_SOCIAL_MAX_EVOLVING_TOPICS);
    for (std::string const& topic : bounded)
    {
        EXPECT_FALSE(topic.empty());
        EXPECT_LE(topic.size(), PLAYERBOT_SOCIAL_MAX_TOPIC_LENGTH);
    }

    // Order is preserved and duplicates are collapsed.
    EXPECT_EQ(bounded.front(), "topic0");
    EXPECT_EQ(std::count(bounded.begin(), bounded.end(), std::string("topic0")), 1);
}

TEST(PlayerbotSocialPolicyTest, GuardedEvolutionReportsAnInRangeValueEvenWhenItRefuses)
{
    // The refused branch must not hand back a corrupt stored trait untouched. Both branches of the
    // guarded entry point report a value inside the trait range.
    PlayerbotSocialTraitEvolution const refused = PlayerbotSocialEvolveTraitIfDue(255, 5, 1000, 1000);
    EXPECT_FALSE(refused.applied);
    EXPECT_EQ(refused.value, PLAYERBOT_SOCIAL_TRAIT_MAX);

    PlayerbotSocialTraitEvolution const applied =
        PlayerbotSocialEvolveTraitIfDue(255, -5, 0, PLAYERBOT_SOCIAL_TRAIT_MIN_INTERVAL_SECONDS);
    EXPECT_TRUE(applied.applied);
    EXPECT_LE(applied.value, PLAYERBOT_SOCIAL_TRAIT_MAX);
}

// Opportunity eligibility ---------------------------------------------------------------------------

namespace
{
// A request that every gate accepts. Each eligibility test below changes exactly one field, so a
// failure names the gate that rejected rather than leaving the reader to diff whole structs.
PlayerbotSocialOpportunity EligibleReplyOpportunity()
{
    PlayerbotSocialOpportunity opportunity;
    opportunity.channel = PlayerbotSocialChannel::General;
    opportunity.starter = false;
    opportunity.speakerIsHuman = true;
    opportunity.speakerOptedOut = false;
    opportunity.botOptedOutOfInitiation = false;
    opportunity.factionMatches = true;
    opportunity.languageMatches = true;
    opportunity.threadLastActivityUnixSeconds = 1000;
    opportunity.botLastSpokeUnixSeconds = 0;
    opportunity.nowUnixSeconds = 1000;
    opportunity.duplicateOfRecentMessage = false;
    opportunity.profileLoadState = PlayerbotSocialProfileLoadState::AbsentUsingBase;
    return opportunity;
}
}  // namespace

TEST(PlayerbotSocialEligibilityTest, TheBaselineOpportunityIsAccepted)
{
    // Without this the rejection tests below could all pass against a gate that refuses everything.
    EXPECT_EQ(PlayerbotSocialEvaluateOpportunity(EligibleReplyOpportunity()),
              PlayerbotSocialOpportunityRejection::None);
}

TEST(PlayerbotSocialEligibilityTest, AnInvalidChannelValueIsRejected)
{
    // The module compiles without -Wswitch and without -Werror, so a value cast in from a payload or
    // an enumerator added later reaches this gate. It must be refused rather than defaulted.
    PlayerbotSocialOpportunity opportunity = EligibleReplyOpportunity();
    opportunity.channel = static_cast<PlayerbotSocialChannel>(200);

    EXPECT_EQ(PlayerbotSocialEvaluateOpportunity(opportunity), PlayerbotSocialOpportunityRejection::UnsupportedChannel);
}

TEST(PlayerbotSocialEligibilityTest, AWhisperStarterIsRejectedButAWhisperReplyIsNot)
{
    // Whisper supports reactive replies only. A spontaneous whisper is the one channel rule that
    // depends on the direction of the opportunity rather than on the channel alone.
    PlayerbotSocialOpportunity starter = EligibleReplyOpportunity();
    starter.channel = PlayerbotSocialChannel::Whisper;
    starter.starter = true;

    EXPECT_EQ(PlayerbotSocialEvaluateOpportunity(starter),
              PlayerbotSocialOpportunityRejection::StarterNotAllowedOnChannel);

    PlayerbotSocialOpportunity reply = starter;
    reply.starter = false;
    EXPECT_EQ(PlayerbotSocialEvaluateOpportunity(reply), PlayerbotSocialOpportunityRejection::None);
}

TEST(PlayerbotSocialEligibilityTest, GeneralSayAndPartyAcceptStarters)
{
    for (PlayerbotSocialChannel channel :
         {PlayerbotSocialChannel::General, PlayerbotSocialChannel::Say, PlayerbotSocialChannel::Party})
    {
        PlayerbotSocialOpportunity opportunity = EligibleReplyOpportunity();
        opportunity.channel = channel;
        opportunity.starter = true;

        EXPECT_EQ(PlayerbotSocialEvaluateOpportunity(opportunity), PlayerbotSocialOpportunityRejection::None)
            << "channel " << static_cast<uint32>(channel) << " should support a spontaneous starter";
    }
}

TEST(PlayerbotSocialEligibilityTest, OptedOutInitiationIsRejectedOnlyForStarters)
{
    // Opting out of initiation stops a bot addressing someone first. It is not a mute: a bot that
    // was spoken to may still answer.
    PlayerbotSocialOpportunity starter = EligibleReplyOpportunity();
    starter.starter = true;
    starter.botOptedOutOfInitiation = true;

    EXPECT_EQ(PlayerbotSocialEvaluateOpportunity(starter), PlayerbotSocialOpportunityRejection::InitiationOptedOut);

    PlayerbotSocialOpportunity reply = starter;
    reply.starter = false;
    EXPECT_EQ(PlayerbotSocialEvaluateOpportunity(reply), PlayerbotSocialOpportunityRejection::None);
}

TEST(PlayerbotSocialEligibilityTest, AnOptedOutHumanMayReceiveOnlyAReactiveWhisperReply)
{
    PlayerbotSocialOpportunity whisper = EligibleReplyOpportunity();
    whisper.channel = PlayerbotSocialChannel::Whisper;
    whisper.speakerOptedOut = true;

    EXPECT_EQ(PlayerbotSocialEvaluateOpportunity(whisper), PlayerbotSocialOpportunityRejection::None);

    for (PlayerbotSocialChannel const channel :
         {PlayerbotSocialChannel::General, PlayerbotSocialChannel::Say, PlayerbotSocialChannel::Party})
    {
        PlayerbotSocialOpportunity publicReply = whisper;
        publicReply.channel = channel;

        EXPECT_EQ(PlayerbotSocialEvaluateOpportunity(publicReply),
                  PlayerbotSocialOpportunityRejection::SpeakerOptedOut);
    }

    PlayerbotSocialOpportunity botSpeaker = whisper;
    botSpeaker.speakerIsHuman = false;
    EXPECT_EQ(PlayerbotSocialEvaluateOpportunity(botSpeaker), PlayerbotSocialOpportunityRejection::SpeakerOptedOut);

    PlayerbotSocialOpportunity starter = whisper;
    starter.starter = true;
    EXPECT_EQ(PlayerbotSocialEvaluateOpportunity(starter),
              PlayerbotSocialOpportunityRejection::StarterNotAllowedOnChannel);
}

TEST(PlayerbotSocialEligibilityTest, AnOptedOutSpeakerOnPublicChatIsRejected)
{
    PlayerbotSocialOpportunity opportunity = EligibleReplyOpportunity();
    opportunity.speakerOptedOut = true;

    EXPECT_EQ(PlayerbotSocialEvaluateOpportunity(opportunity), PlayerbotSocialOpportunityRejection::SpeakerOptedOut);
}

TEST(PlayerbotSocialEligibilityTest, FactionAndLanguageMismatchesAreRejectedSeparately)
{
    PlayerbotSocialOpportunity faction = EligibleReplyOpportunity();
    faction.factionMatches = false;
    EXPECT_EQ(PlayerbotSocialEvaluateOpportunity(faction), PlayerbotSocialOpportunityRejection::FactionMismatch);

    PlayerbotSocialOpportunity language = EligibleReplyOpportunity();
    language.languageMatches = false;
    EXPECT_EQ(PlayerbotSocialEvaluateOpportunity(language), PlayerbotSocialOpportunityRejection::LanguageMismatch);
}

TEST(PlayerbotSocialEligibilityTest, AStaleThreadIsRejected)
{
    PlayerbotSocialOpportunity opportunity = EligibleReplyOpportunity();
    opportunity.threadLastActivityUnixSeconds = 1000;
    opportunity.nowUnixSeconds = 1000 + PLAYERBOT_SOCIAL_THREAD_STALE_SECONDS + 1;

    EXPECT_EQ(PlayerbotSocialEvaluateOpportunity(opportunity), PlayerbotSocialOpportunityRejection::ThreadStale);
}

TEST(PlayerbotSocialEligibilityTest, AThreadExactlyAtTheStalenessBoundIsStillFresh)
{
    // The bound is inclusive. Stating it as a test keeps a later refactor from quietly moving it.
    PlayerbotSocialOpportunity opportunity = EligibleReplyOpportunity();
    opportunity.threadLastActivityUnixSeconds = 1000;
    opportunity.nowUnixSeconds = 1000 + PLAYERBOT_SOCIAL_THREAD_STALE_SECONDS;

    EXPECT_EQ(PlayerbotSocialEvaluateOpportunity(opportunity), PlayerbotSocialOpportunityRejection::None);
}

TEST(PlayerbotSocialEligibilityTest, ABotStillInCooldownIsRejected)
{
    PlayerbotSocialOpportunity opportunity = EligibleReplyOpportunity();
    opportunity.botLastSpokeUnixSeconds = 1000;
    opportunity.nowUnixSeconds = 1000 + PLAYERBOT_SOCIAL_REPLY_COOLDOWN_SECONDS - 1;

    EXPECT_EQ(PlayerbotSocialEvaluateOpportunity(opportunity), PlayerbotSocialOpportunityRejection::CooldownActive);
}

TEST(PlayerbotSocialEligibilityTest, ABotExactlyAtTheCooldownBoundMaySpeak)
{
    PlayerbotSocialOpportunity opportunity = EligibleReplyOpportunity();
    opportunity.botLastSpokeUnixSeconds = 1000;
    opportunity.nowUnixSeconds = 1000 + PLAYERBOT_SOCIAL_REPLY_COOLDOWN_SECONDS;

    EXPECT_EQ(PlayerbotSocialEvaluateOpportunity(opportunity), PlayerbotSocialOpportunityRejection::None);
}

TEST(PlayerbotSocialEligibilityTest, ACooldownIsNotBypassedByAClockThatMovedBackwards)
{
    // A backwards clock would otherwise read as a very large elapsed time and release every bot at
    // once. The same fail closed rule the trait evolution interval already uses.
    PlayerbotSocialOpportunity opportunity = EligibleReplyOpportunity();
    opportunity.botLastSpokeUnixSeconds = 5000;
    opportunity.nowUnixSeconds = 1000;

    EXPECT_EQ(PlayerbotSocialEvaluateOpportunity(opportunity), PlayerbotSocialOpportunityRejection::CooldownActive);
}

TEST(PlayerbotSocialEligibilityTest, AStalenessCheckIsNotBypassedByAClockThatMovedBackwards)
{
    PlayerbotSocialOpportunity opportunity = EligibleReplyOpportunity();
    opportunity.threadLastActivityUnixSeconds = 5000;
    opportunity.nowUnixSeconds = 1000;

    EXPECT_EQ(PlayerbotSocialEvaluateOpportunity(opportunity), PlayerbotSocialOpportunityRejection::ThreadStale);
}

TEST(PlayerbotSocialEligibilityTest, ADuplicateOfARecentMessageIsRejected)
{
    PlayerbotSocialOpportunity opportunity = EligibleReplyOpportunity();
    opportunity.duplicateOfRecentMessage = true;

    EXPECT_EQ(PlayerbotSocialEvaluateOpportunity(opportunity),
              PlayerbotSocialOpportunityRejection::DuplicateSuppressed);
}

TEST(PlayerbotSocialEligibilityTest, AThirdConsecutiveBotTurnIsRejectedBeforeProviderAdmission)
{
    PlayerbotSocialOpportunity continuation = EligibleReplyOpportunity();
    continuation.speakerIsHuman = false;
    continuation.consecutiveBotOnlyTurns = PLAYERBOT_SOCIAL_MAX_CONSECUTIVE_BOT_TURNS;

    EXPECT_EQ(PlayerbotSocialEvaluateOpportunity(continuation), PlayerbotSocialOpportunityRejection::BotOnlyTurnLimit);
}

TEST(PlayerbotSocialEligibilityTest, AStarterIsNeverRejectedForThreadStaleness)
{
    /*
     * Staleness protects replies from answering a conversation that ended; a starter is new speech
     * about a quiet scope, and the quieter the scope the MORE the ambient cadence wants the line.
     * The starter's own subject freshness is enforced where the contexts age out, not here.
     */
    PlayerbotSocialOpportunity starter = EligibleReplyOpportunity();
    starter.starter = true;
    starter.speakerIsHuman = false;
    starter.threadLastActivityUnixSeconds = 0;
    starter.nowUnixSeconds = 1000 + PLAYERBOT_SOCIAL_THREAD_STALE_SECONDS * 10;
    starter.botLastSpokeUnixSeconds = 0;

    EXPECT_EQ(PlayerbotSocialEvaluateOpportunity(starter), PlayerbotSocialOpportunityRejection::None);

    // A reply to the same silence stays refused: nobody is waiting on an answer to a dead thread.
    PlayerbotSocialOpportunity reply = starter;
    reply.starter = false;
    reply.speakerIsHuman = true;
    EXPECT_EQ(PlayerbotSocialEvaluateOpportunity(reply), PlayerbotSocialOpportunityRejection::ThreadStale);
}

TEST(PlayerbotSocialEligibilityTest, AWhisperStarterIsAdmittedOnlyWhenRelationshipDriven)
{
    // Whisper stays reactive for every spontaneous starter; the one whisper a bot may open is the
    // relationship-driven check-in, which carries its justification on the opportunity.
    PlayerbotSocialOpportunity starter = EligibleReplyOpportunity();
    starter.starter = true;
    starter.speakerIsHuman = false;
    starter.channel = PlayerbotSocialChannel::Whisper;

    EXPECT_EQ(PlayerbotSocialEvaluateOpportunity(starter),
              PlayerbotSocialOpportunityRejection::StarterNotAllowedOnChannel);

    starter.relationshipDriven = true;
    EXPECT_EQ(PlayerbotSocialEvaluateOpportunity(starter), PlayerbotSocialOpportunityRejection::None);
}

TEST(PlayerbotSocialEligibilityTest, ARelationshipDrivenWhisperStillHonoursInitiationOptOut)
{
    // Criterion: an opted-out bot never opens a whisper, however warm the relationship reads.
    PlayerbotSocialOpportunity starter = EligibleReplyOpportunity();
    starter.starter = true;
    starter.speakerIsHuman = false;
    starter.channel = PlayerbotSocialChannel::Whisper;
    starter.relationshipDriven = true;
    starter.botOptedOutOfInitiation = true;

    EXPECT_EQ(PlayerbotSocialEvaluateOpportunity(starter), PlayerbotSocialOpportunityRejection::InitiationOptedOut);
}

TEST(PlayerbotSocialEligibilityTest, TheBotOnlyTurnCapIsCarriedByTheOpportunity)
{
    // Autonomous threads raise the cap through the opportunity rather than a global: a fourth bot
    // turn is admitted when the cap says six, and the raised cap still rejects at its own bound.
    PlayerbotSocialOpportunity continuation = EligibleReplyOpportunity();
    continuation.speakerIsHuman = false;
    continuation.consecutiveBotOnlyTurns = 4;
    continuation.maxConsecutiveBotOnlyTurns = 6;

    EXPECT_EQ(PlayerbotSocialEvaluateOpportunity(continuation), PlayerbotSocialOpportunityRejection::None);

    continuation.consecutiveBotOnlyTurns = 6;
    EXPECT_EQ(PlayerbotSocialEvaluateOpportunity(continuation), PlayerbotSocialOpportunityRejection::BotOnlyTurnLimit);

    continuation.consecutiveBotOnlyTurns = PLAYERBOT_SOCIAL_MAX_CONSECUTIVE_BOT_TURNS - 1;
    EXPECT_EQ(PlayerbotSocialEvaluateOpportunity(continuation), PlayerbotSocialOpportunityRejection::None);

    continuation.speakerIsHuman = true;
    continuation.consecutiveBotOnlyTurns = PLAYERBOT_SOCIAL_MAX_CONSECUTIVE_BOT_TURNS;
    EXPECT_EQ(PlayerbotSocialEvaluateOpportunity(continuation), PlayerbotSocialOpportunityRejection::None);
}

TEST(PlayerbotSocialEligibilityTest, EveryRejectionReasonHasItsOwnExactName)
{
    // Same contract and same reasoning as the suppression names: exact strings, because a length
    // check cannot tell a named reason from one that fell through to "unknown".
    struct NamedRejection
    {
        PlayerbotSocialOpportunityRejection reason;
        char const* name;
    };

    NamedRejection const expected[] = {
        {PlayerbotSocialOpportunityRejection::None, "none"},
        {PlayerbotSocialOpportunityRejection::UnsupportedChannel, "unsupported_channel"},
        {PlayerbotSocialOpportunityRejection::StarterNotAllowedOnChannel, "starter_not_allowed_on_channel"},
        {PlayerbotSocialOpportunityRejection::SelfReply, "self_reply"},
        {PlayerbotSocialOpportunityRejection::StarterSourceMismatch, "starter_source_mismatch"},
        {PlayerbotSocialOpportunityRejection::InitiationOptedOut, "initiation_opted_out"},
        {PlayerbotSocialOpportunityRejection::SpeakerOptedOut, "speaker_opted_out"},
        {PlayerbotSocialOpportunityRejection::FactionMismatch, "faction_mismatch"},
        {PlayerbotSocialOpportunityRejection::LanguageMismatch, "language_mismatch"},
        {PlayerbotSocialOpportunityRejection::ThreadStale, "thread_stale"},
        {PlayerbotSocialOpportunityRejection::CooldownActive, "cooldown_active"},
        {PlayerbotSocialOpportunityRejection::DuplicateSuppressed, "duplicate_suppressed"},
        {PlayerbotSocialOpportunityRejection::BotOnlyTurnLimit, "bot_only_turn_limit"},
        {PlayerbotSocialOpportunityRejection::ProfilePending, "profile_pending"},
        {PlayerbotSocialOpportunityRejection::ProfileRejected, "profile_rejected"},
        {PlayerbotSocialOpportunityRejection::ProfileUnavailable, "profile_unavailable"}};

    static_assert(std::size(expected) == PLAYERBOT_SOCIAL_OPPORTUNITY_REJECTION_COUNT,
                  "every opportunity rejection reason needs an exact expected name here");

    std::vector<std::string> names;
    for (NamedRejection const& pair : expected)
    {
        EXPECT_STREQ(PlayerbotSocialOpportunityRejectionName(pair.reason), pair.name);
        names.emplace_back(PlayerbotSocialOpportunityRejectionName(pair.reason));
    }

    std::sort(names.begin(), names.end());
    EXPECT_EQ(std::adjacent_find(names.begin(), names.end()), names.end()) << "rejection names must be distinct";

    EXPECT_STREQ(PlayerbotSocialOpportunityRejectionName(static_cast<PlayerbotSocialOpportunityRejection>(200)),
                 "unknown");
}

// Conversation pressure -----------------------------------------------------------------------------

namespace
{
// A thread that has just seen one human message and no bot only turns yet.
PlayerbotSocialThreadPressure FreshHumanThread()
{
    PlayerbotSocialThreadPressure thread;
    thread.consecutiveBotOnlyTurns = 0;
    thread.relevantHumanMessages = 1;
    thread.lastActivityUnixSeconds = 1000;
    thread.nowUnixSeconds = 1000;
    thread.channelDensity = 0;
    return thread;
}
}  // namespace

TEST(PlayerbotSocialPressureTest, ReplyPressureNeverReachesCertainty)
{
    // Contract: relevant human participation strongly boosts continuation without guaranteeing it.
    // Even the most favourable thread this policy can describe must leave room to stay silent.
    PlayerbotSocialThreadPressure thread = FreshHumanThread();
    thread.relevantHumanMessages = 1000;

    float const pressure = PlayerbotSocialReplyPressure(thread);

    EXPECT_GT(pressure, 0.0f);
    EXPECT_LT(pressure, 1.0f);
    EXPECT_LE(pressure, PLAYERBOT_SOCIAL_REPLY_PRESSURE_CEILING);
}

TEST(PlayerbotSocialPressureTest, HumanParticipationRaisesReplyPressureMonotonically)
{
    // Monotonic, not merely "higher at the extremes": every additional relevant human message must
    // weakly raise the pressure, so no interior dip can hide in the curve.
    float previous = -1.0f;
    for (uint32 humanMessages = 0; humanMessages <= 40; ++humanMessages)
    {
        PlayerbotSocialThreadPressure thread = FreshHumanThread();
        thread.relevantHumanMessages = humanMessages;

        float const pressure = PlayerbotSocialReplyPressure(thread);
        EXPECT_GE(pressure, previous) << "reply pressure dipped at " << humanMessages << " human messages";
        EXPECT_LT(pressure, 1.0f);
        previous = pressure;
    }

    // And the effect is real, not a flat line that trivially satisfies monotonicity.
    PlayerbotSocialThreadPressure quiet = FreshHumanThread();
    quiet.relevantHumanMessages = 0;
    PlayerbotSocialThreadPressure busy = FreshHumanThread();
    busy.relevantHumanMessages = 10;
    EXPECT_GT(PlayerbotSocialReplyPressure(busy), PlayerbotSocialReplyPressure(quiet));
}

TEST(PlayerbotSocialPressureTest, ReplyPressureDecaysWithBotOnlyTurnsAndStaysAboveZero)
{
    // Contract: no hard conversation length cap. Probability decays with bot only turns and remains
    // nonzero, so the thread ends because a low roll fails or a cooldown bites, never because a
    // counter hit a limit.
    float previous = 2.0f;
    for (uint32 turns = 0; turns <= 50; ++turns)
    {
        PlayerbotSocialThreadPressure thread = FreshHumanThread();
        thread.consecutiveBotOnlyTurns = turns;

        float const pressure = PlayerbotSocialReplyPressure(thread);
        EXPECT_LT(pressure, previous) << "reply pressure failed to decay at turn " << turns;
        EXPECT_GT(pressure, 0.0f) << "reply pressure hit zero at turn " << turns << ", which is a hard cap";
        previous = pressure;
    }
}

TEST(PlayerbotSocialPressureTest, ReplyPressureDecaysWithIdleTimeAndStaysAboveZero)
{
    float previous = 2.0f;
    for (uint64 idleSeconds = 0; idleSeconds <= PLAYERBOT_SOCIAL_THREAD_STALE_SECONDS; idleSeconds += 30)
    {
        PlayerbotSocialThreadPressure thread = FreshHumanThread();
        thread.nowUnixSeconds = thread.lastActivityUnixSeconds + idleSeconds;

        float const pressure = PlayerbotSocialReplyPressure(thread);
        EXPECT_LE(pressure, previous);
        EXPECT_GT(pressure, 0.0f);
        previous = pressure;
    }
}

TEST(PlayerbotSocialPressureTest, ABotOnlyThreadBecomesImprobableWithoutEverBeingForbidden)
{
    // The stated shape of a bot only conversation: it fades rather than stopping. A thread with no
    // human at all is already well under an even chance early, and keeps falling.
    PlayerbotSocialThreadPressure thread = FreshHumanThread();
    thread.relevantHumanMessages = 0;
    thread.consecutiveBotOnlyTurns = 8;

    float const pressure = PlayerbotSocialReplyPressure(thread);
    EXPECT_LT(pressure, 0.1f);
    EXPECT_GT(pressure, 0.0f);
}

TEST(PlayerbotSocialPressureTest, StarterPressureThrottlesBeforeReplyPressureAsDensityRises)
{
    // Contract: when density rises, starters throttle before active replies. Asserted as a ratio at
    // each density step so it holds across the range rather than at one convenient point.
    for (uint8 density = 0; density <= 100; density = static_cast<uint8>(density + 10))
    {
        PlayerbotSocialThreadPressure thread = FreshHumanThread();
        thread.channelDensity = density;

        float const starter = PlayerbotSocialStarterPressure(thread);
        float const reply = PlayerbotSocialReplyPressure(thread);

        EXPECT_LT(starter, reply) << "starter pressure must stay below reply pressure at density "
                                  << static_cast<uint32>(density);
        EXPECT_GE(starter, 0.0f);
    }
}

TEST(PlayerbotSocialPressureTest, GeneralStarterPressureIsStrictlyLowerThanSay)
{
    PlayerbotSocialThreadPressure thread;
    thread.lastActivityUnixSeconds = 1000;
    thread.nowUnixSeconds = 1000;

    float const say = PlayerbotSocialStarterPressureForChannel(PlayerbotSocialChannel::Say, thread);
    float const general = PlayerbotSocialStarterPressureForChannel(PlayerbotSocialChannel::General, thread);

    EXPECT_GT(general, 0.0f);
    EXPECT_LT(general, say);
    EXPECT_FLOAT_EQ(PlayerbotSocialStarterPressureForChannel(PlayerbotSocialChannel::Party, thread), say);
}

TEST(PlayerbotSocialPressureTest, RisingDensityLowersBothPressuresAndStartersFasterThanReplies)
{
    PlayerbotSocialThreadPressure quiet = FreshHumanThread();
    quiet.channelDensity = 0;
    PlayerbotSocialThreadPressure busy = FreshHumanThread();
    busy.channelDensity = 100;

    float const quietStarter = PlayerbotSocialStarterPressure(quiet);
    float const busyStarter = PlayerbotSocialStarterPressure(busy);
    float const quietReply = PlayerbotSocialReplyPressure(quiet);
    float const busyReply = PlayerbotSocialReplyPressure(busy);

    EXPECT_LT(busyStarter, quietStarter);
    EXPECT_LT(busyReply, quietReply);

    // The starter lane loses proportionally more of its pressure than the reply lane does.
    EXPECT_LT(busyStarter / quietStarter, busyReply / quietReply);
}

TEST(PlayerbotSocialBudgetTest, TheBudgetAdmitsABurstThenRefillsAtTheHourlyRate)
{
    /*
     * A token bucket, not a windowed count: a windowed count admits the whole hour's budget as one
     * opening burst and then starves the rest of the hour, which reads exactly like the silence
     * this build removes. Budget 120 means a burst of ten and one new token every thirty seconds,
     * so lively is steady rather than a flood followed by a graveyard. Continuations draw the
     * whole bucket.
     */
    PlayerbotSocialProviderBudgetState state;

    for (uint64 call = 0; call < 10; ++call)
        EXPECT_EQ(PlayerbotSocialGovernProviderCall(state, 1000, 120, /*continuation=*/true),
                  PlayerbotSocialBudgetDecision::Admitted);

    EXPECT_EQ(PlayerbotSocialGovernProviderCall(state, 1000, 120, true), PlayerbotSocialBudgetDecision::Refused);

    // Thirty seconds refills exactly one token at 120 per hour.
    EXPECT_EQ(PlayerbotSocialGovernProviderCall(state, 1030, 120, true), PlayerbotSocialBudgetDecision::Admitted);
    EXPECT_EQ(PlayerbotSocialGovernProviderCall(state, 1030, 120, true), PlayerbotSocialBudgetDecision::Refused);
}

TEST(PlayerbotSocialBudgetTest, StartersCannotDrainTheContinuationReserve)
{
    /*
     * Observed live: starter demand runs at many times the budget, so starters won every token and
     * replies to bot lines never opened, which is why no thread ever reached a real length. The
     * bottom of the bucket is reserved for continuations: starters stop above it, and a reply that
     * arrives when starters have spent the rest still gets its line.
     */
    PlayerbotSocialProviderBudgetState state;

    // Budget 120: burst ten, reserve two. Starters take the bucket from ten down to the reserve.
    for (uint64 call = 0; call < 8; ++call)
        EXPECT_EQ(PlayerbotSocialGovernProviderCall(state, 2000, 120, /*continuation=*/false),
                  PlayerbotSocialBudgetDecision::Admitted);

    EXPECT_EQ(PlayerbotSocialGovernProviderCall(state, 2000, 120, false), PlayerbotSocialBudgetDecision::Refused);

    // The reserve still carries continuations, and only continuations.
    EXPECT_EQ(PlayerbotSocialGovernProviderCall(state, 2000, 120, true), PlayerbotSocialBudgetDecision::Admitted);
    EXPECT_EQ(PlayerbotSocialGovernProviderCall(state, 2000, 120, true), PlayerbotSocialBudgetDecision::Admitted);
    EXPECT_EQ(PlayerbotSocialGovernProviderCall(state, 2000, 120, true), PlayerbotSocialBudgetDecision::Refused);
}

TEST(PlayerbotSocialBudgetTest, OnlyPathologicalOverrunTripsTheCircuit)
{
    /*
     * Organic demand above the budget is the ceiling WORKING, not an emergency: with hundreds of
     * bots the refusal lane runs warm all day. The durable circuit is for a runaway loop, so it
     * trips only when refusals inside one hour reach many multiples of the budget.
     */
    PlayerbotSocialProviderBudgetState state;

    EXPECT_EQ(PlayerbotSocialGovernProviderCall(state, 5000, 1), PlayerbotSocialBudgetDecision::Admitted);

    for (uint64 refusal = 0; refusal + 1 < PLAYERBOT_SOCIAL_BUDGET_TRIP_MULTIPLE; ++refusal)
        EXPECT_EQ(PlayerbotSocialGovernProviderCall(state, 5000, 1), PlayerbotSocialBudgetDecision::Refused);

    EXPECT_EQ(PlayerbotSocialGovernProviderCall(state, 5000, 1), PlayerbotSocialBudgetDecision::RefusedCircuitTrip);
}

TEST(PlayerbotSocialBudgetTest, AZeroBudgetMeansUnlimited)
{
    // Zero is the operator saying "no ceiling", which must never mean "nothing may speak".
    PlayerbotSocialProviderBudgetState state;

    for (uint64 call = 0; call < 500; ++call)
        EXPECT_EQ(PlayerbotSocialGovernProviderCall(state, 3000, 0), PlayerbotSocialBudgetDecision::Admitted);
}

TEST(PlayerbotSocialPressureTest, ReplyPressureTurnDecayIsConfigurablePerThread)
{
    /*
     * Autonomous threads soften the per-turn decay so a bot conversation can reach four turns and
     * beyond. The decay rides on the thread state, so earlier stages keep the default untouched.
     */
    PlayerbotSocialThreadPressure defaultDecay;
    defaultDecay.consecutiveBotOnlyTurns = 4;
    defaultDecay.lastActivityUnixSeconds = 1000;
    defaultDecay.nowUnixSeconds = 1000;

    PlayerbotSocialThreadPressure softenedDecay = defaultDecay;
    softenedDecay.botOnlyTurnDecay = 0.85f;

    float const hard = PlayerbotSocialReplyPressure(defaultDecay);
    float const soft = PlayerbotSocialReplyPressure(softenedDecay);

    EXPECT_GT(soft, hard);

    // At the softened decay a fourth turn still has a workable chance of being answered rather than
    // a token one: 0.45 * 0.85^4 is above a fifth.
    EXPECT_GE(soft, 0.2f);
}

TEST(PlayerbotSocialPressureTest, StarterPressureRisesTowardCadenceAsAScopeStaysQuiet)
{
    /*
     * The ambient contract: silence fills instead of decaying. A scope that just heard a line stays
     * near-quiet, and starter pressure climbs with idle time until it saturates at the configured
     * cadence, so a quiet channel converges on roughly one ambient line per cadence interval.
     */
    PlayerbotSocialThreadPressure justSpoke;
    justSpoke.lastActivityUnixSeconds = 10000;
    justSpoke.nowUnixSeconds = 10000;

    PlayerbotSocialThreadPressure halfCadence = justSpoke;
    halfCadence.nowUnixSeconds = 10000 + PLAYERBOT_SOCIAL_AMBIENT_CADENCE_DEFAULT_SECONDS / 2;

    PlayerbotSocialThreadPressure atCadence = justSpoke;
    atCadence.nowUnixSeconds = 10000 + PLAYERBOT_SOCIAL_AMBIENT_CADENCE_DEFAULT_SECONDS;

    float const justSpokePressure = PlayerbotSocialStarterPressure(justSpoke);
    float const halfCadencePressure = PlayerbotSocialStarterPressure(halfCadence);
    float const atCadencePressure = PlayerbotSocialStarterPressure(atCadence);

    EXPECT_LT(justSpokePressure, halfCadencePressure);
    EXPECT_LT(halfCadencePressure, atCadencePressure);

    // At the cadence point a quiet channel must actually be likely to speak, not just trend upward.
    EXPECT_GE(atCadencePressure, 0.5f);
}

TEST(PlayerbotSocialPressureTest, StarterPressureAtCadenceStillThrottlesInABusyChannel)
{
    // The fill raises quiet scopes; it must not override the density throttle that keeps a busy
    // channel from being talked over.
    PlayerbotSocialThreadPressure quietAtCadence;
    quietAtCadence.lastActivityUnixSeconds = 10000;
    quietAtCadence.nowUnixSeconds = 10000 + PLAYERBOT_SOCIAL_AMBIENT_CADENCE_DEFAULT_SECONDS;
    quietAtCadence.channelDensity = 0;

    PlayerbotSocialThreadPressure busyAtCadence = quietAtCadence;
    busyAtCadence.channelDensity = 100;

    EXPECT_LT(PlayerbotSocialStarterPressure(busyAtCadence), PlayerbotSocialStarterPressure(quietAtCadence) / 2.0f);
}

TEST(PlayerbotSocialPressureTest, StarterPressureAfterBotTurnsRecoversWithIdleTime)
{
    /*
     * Bot speech must not poison future ambient starters. A scope whose last thread ran several
     * bot-only turns still refills toward cadence once it goes quiet, otherwise bot conversation
     * suppresses the next conversation and the channel converges back on silence.
     */
    PlayerbotSocialThreadPressure afterBotThread;
    afterBotThread.consecutiveBotOnlyTurns = 4;
    afterBotThread.lastActivityUnixSeconds = 10000;
    afterBotThread.nowUnixSeconds = 10000 + PLAYERBOT_SOCIAL_AMBIENT_CADENCE_DEFAULT_SECONDS;

    EXPECT_GE(PlayerbotSocialStarterPressure(afterBotThread), 0.5f);
}

TEST(PlayerbotSocialPressureTest, AFullyCongestedChannelStillLeavesADirectReplyPossible)
{
    // Density is throttling pressure, not a mute. A human talking into a busy channel can still be
    // answered, which is what keeps the priority order meaningful under load.
    PlayerbotSocialThreadPressure thread = FreshHumanThread();
    thread.channelDensity = 255;  // deliberately out of range: a corrupt or rescaled input
    thread.relevantHumanMessages = 5;

    float const pressure = PlayerbotSocialReplyPressure(thread);
    EXPECT_GT(pressure, 0.0f);
    EXPECT_LT(pressure, 1.0f);
}

TEST(PlayerbotSocialPressureTest, PressureIsBoundedForCorruptAndBackwardsInputs)
{
    // A stored row or a clock can hand this function anything. Both pressures stay inside their
    // documented range for every one of these rather than producing a NaN or a value above 1.
    std::vector<PlayerbotSocialThreadPressure> corrupt;

    PlayerbotSocialThreadPressure backwards = FreshHumanThread();
    backwards.nowUnixSeconds = 0;
    backwards.lastActivityUnixSeconds = 5000;
    corrupt.push_back(backwards);

    PlayerbotSocialThreadPressure enormousTurns = FreshHumanThread();
    enormousTurns.consecutiveBotOnlyTurns = std::numeric_limits<uint32>::max();
    corrupt.push_back(enormousTurns);

    PlayerbotSocialThreadPressure enormousHumans = FreshHumanThread();
    enormousHumans.relevantHumanMessages = std::numeric_limits<uint32>::max();
    corrupt.push_back(enormousHumans);

    PlayerbotSocialThreadPressure enormousIdle = FreshHumanThread();
    enormousIdle.nowUnixSeconds = std::numeric_limits<uint64>::max();
    corrupt.push_back(enormousIdle);

    for (std::size_t index = 0; index < corrupt.size(); ++index)
    {
        float const reply = PlayerbotSocialReplyPressure(corrupt[index]);
        float const starter = PlayerbotSocialStarterPressure(corrupt[index]);

        EXPECT_EQ(reply, reply) << "reply pressure produced a NaN for corrupt input " << index;
        EXPECT_EQ(starter, starter) << "starter pressure produced a NaN for corrupt input " << index;
        EXPECT_GE(reply, 0.0f) << "corrupt input " << index;
        EXPECT_LT(reply, 1.0f) << "corrupt input " << index;
        EXPECT_GE(starter, 0.0f) << "corrupt input " << index;
        EXPECT_LT(starter, 1.0f) << "corrupt input " << index;
    }
}

TEST(PlayerbotSocialPressureTest, AdmissionLaneFollowsTheStatedPriorityOrder)
{
    // The lane decides how an opportunity competes for budget. It is derived here so the coordinator
    // and the budget admission path cannot disagree about what a thread counts as.
    PlayerbotSocialThreadPressure direct = FreshHumanThread();
    direct.relevantHumanMessages = 1;

    EXPECT_EQ(PlayerbotSocialAdmissionLane(direct, /*starter=*/false, /*addressedDirectly=*/true),
              PlayerbotSocialPriorityLane::DirectHuman);
    EXPECT_EQ(PlayerbotSocialAdmissionLane(direct, /*starter=*/false, /*addressedDirectly=*/false),
              PlayerbotSocialPriorityLane::MixedHumanBot);

    PlayerbotSocialThreadPressure botOnly = FreshHumanThread();
    botOnly.relevantHumanMessages = 0;
    EXPECT_EQ(PlayerbotSocialAdmissionLane(botOnly, /*starter=*/false, /*addressedDirectly=*/false),
              PlayerbotSocialPriorityLane::BotOnlyContinuation);
    EXPECT_EQ(PlayerbotSocialAdmissionLane(botOnly, /*starter=*/true, /*addressedDirectly=*/false),
              PlayerbotSocialPriorityLane::NewStarter);

    // A starter is a starter even in a thread a human once spoke in: it is new speech, not a reply.
    EXPECT_EQ(PlayerbotSocialAdmissionLane(direct, /*starter=*/true, /*addressedDirectly=*/false),
              PlayerbotSocialPriorityLane::NewStarter);

    // And the order the lanes imply is the one the frozen contract states.
    EXPECT_TRUE(PlayerbotSocialLaneIsHigherPriority(PlayerbotSocialPriorityLane::DirectHuman,
                                                    PlayerbotSocialPriorityLane::MixedHumanBot));
    EXPECT_TRUE(PlayerbotSocialLaneIsHigherPriority(PlayerbotSocialPriorityLane::MixedHumanBot,
                                                    PlayerbotSocialPriorityLane::BotOnlyContinuation));
    EXPECT_TRUE(PlayerbotSocialLaneIsHigherPriority(PlayerbotSocialPriorityLane::BotOnlyContinuation,
                                                    PlayerbotSocialPriorityLane::NewStarter));
}

TEST(PlayerbotSocialPressureTest, AddressingSomeoneDirectlyDoesNotPromoteABotOnlyThread)
{
    // Direct address is the strongest ordinary reason to answer, but a bot naming another bot is not
    // human engagement and must not reach the protected human lane.
    PlayerbotSocialThreadPressure botOnly = FreshHumanThread();
    botOnly.relevantHumanMessages = 0;

    PlayerbotSocialPriorityLane const lane =
        PlayerbotSocialAdmissionLane(botOnly, /*starter=*/false, /*addressedDirectly=*/true);

    EXPECT_EQ(lane, PlayerbotSocialPriorityLane::BotOnlyContinuation);
    EXPECT_FALSE(PlayerbotSocialLaneMayUseHumanReserve(lane));
}

// Responder selection -------------------------------------------------------------------------------

namespace
{
PlayerbotSocialCandidate Candidate(uint64 guidCounter, uint8 disposition, PlayerbotSocialStance stance)
{
    PlayerbotSocialCandidate candidate;
    candidate.botGuidCounter = guidCounter;
    candidate.effectiveDisposition = disposition;
    candidate.stance = stance;
    candidate.addressedByName = false;
    candidate.participatedInThread = false;
    candidate.contentRelevance = 50;
    return candidate;
}

PlayerbotSocialSelectionInput SelectionInput(std::vector<PlayerbotSocialCandidate> candidates)
{
    PlayerbotSocialSelectionInput input;
    input.candidates = std::move(candidates);
    input.replyPressure = 0.9f;
    input.secondResponderAllowed = false;
    input.selectionSeed = 12345;
    return input;
}

std::size_t CountResponder(PlayerbotSocialSelection const& selection, uint64 guidCounter)
{
    return static_cast<std::size_t>(std::count(selection.responders.begin(), selection.responders.end(), guidCounter));
}
}  // namespace

TEST(PlayerbotSocialSelectionTest, ANormalOpportunitySelectsAtMostOneBot)
{
    // The swarm risk stated in the plan: a single human message must never produce a chorus. The
    // seed is varied so this is a property of the selector, not of one lucky seed.
    for (uint64 seed = 0; seed < 200; ++seed)
    {
        PlayerbotSocialSelectionInput input = SelectionInput(
            {Candidate(1, 90, PlayerbotSocialStance::Engaged), Candidate(2, 85, PlayerbotSocialStance::Engaged),
             Candidate(3, 80, PlayerbotSocialStance::Receptive), Candidate(4, 75, PlayerbotSocialStance::Receptive),
             Candidate(5, 70, PlayerbotSocialStance::Receptive)});
        input.selectionSeed = seed;

        PlayerbotSocialSelection const selection = PlayerbotSocialSelectResponders(input);
        EXPECT_LE(selection.responders.size(), 1u) << "seed " << seed << " produced a chorus";
    }
}

TEST(PlayerbotSocialSelectionTest, TheCoherentSecondResponderPathSelectsAtMostTwo)
{
    // The one path allowed to produce a second voice, and it is still bounded at two.
    bool sawTwo = false;
    for (uint64 seed = 0; seed < 200; ++seed)
    {
        PlayerbotSocialSelectionInput input = SelectionInput(
            {Candidate(1, 95, PlayerbotSocialStance::Engaged), Candidate(2, 92, PlayerbotSocialStance::Engaged),
             Candidate(3, 90, PlayerbotSocialStance::Engaged), Candidate(4, 88, PlayerbotSocialStance::Engaged)});
        input.selectionSeed = seed;
        input.secondResponderAllowed = true;

        PlayerbotSocialSelection const selection = PlayerbotSocialSelectResponders(input);
        EXPECT_LE(selection.responders.size(), 2u) << "seed " << seed;
        if (selection.responders.size() == 2)
            sawTwo = true;
    }

    EXPECT_TRUE(sawTwo) << "the second responder path never fired, so its bound is untested";
}

TEST(PlayerbotSocialSelectionTest, TheSameSeededInputProducesTheSameOutcome)
{
    PlayerbotSocialSelectionInput input = SelectionInput({Candidate(11, 80, PlayerbotSocialStance::Receptive),
                                                          Candidate(12, 78, PlayerbotSocialStance::Receptive),
                                                          Candidate(13, 76, PlayerbotSocialStance::Engaged)});
    input.secondResponderAllowed = true;

    PlayerbotSocialSelection const first = PlayerbotSocialSelectResponders(input);
    PlayerbotSocialSelection const second = PlayerbotSocialSelectResponders(input);

    EXPECT_EQ(first.responders, second.responders);
    EXPECT_EQ(first.alternates, second.alternates);
    EXPECT_EQ(first.responders.size(), second.responders.size());
}

TEST(PlayerbotSocialSelectionTest, ADifferentSeedCanProduceADifferentResponder)
{
    // Determinism must not collapse into always picking the same bot: that would be a fairness
    // quota by accident and would make one bot the voice of the server.
    std::vector<uint64> chosen;
    for (uint64 seed = 0; seed < 60; ++seed)
    {
        PlayerbotSocialSelectionInput input = SelectionInput({Candidate(21, 70, PlayerbotSocialStance::Receptive),
                                                              Candidate(22, 70, PlayerbotSocialStance::Receptive),
                                                              Candidate(23, 70, PlayerbotSocialStance::Receptive)});
        input.selectionSeed = seed;

        PlayerbotSocialSelection const selection = PlayerbotSocialSelectResponders(input);
        if (!selection.responders.empty())
            chosen.push_back(selection.responders.front());
    }

    ASSERT_FALSE(chosen.empty());
    std::sort(chosen.begin(), chosen.end());
    chosen.erase(std::unique(chosen.begin(), chosen.end()), chosen.end());
    EXPECT_GT(chosen.size(), 1u) << "equally disposed bots must not always resolve to the same one";
}

TEST(PlayerbotSocialSelectionTest, ADismissiveBotIsNeverSelectedAndIsReportedAsSuppressed)
{
    // Declining is a valid personality outcome, and the reason has to reach telemetry rather than
    // the bot silently vanishing from the candidate list.
    PlayerbotSocialSelectionInput input = SelectionInput({Candidate(31, 99, PlayerbotSocialStance::Dismissive)});
    input.replyPressure = PLAYERBOT_SOCIAL_REPLY_PRESSURE_CEILING;

    PlayerbotSocialSelection const selection = PlayerbotSocialSelectResponders(input);

    EXPECT_EQ(CountResponder(selection, 31), 0u);
    ASSERT_EQ(selection.suppressions.size(), 1u);
    EXPECT_EQ(selection.suppressions.front().botGuidCounter, 31u);
    EXPECT_EQ(selection.suppressions.front().reason, PlayerbotSocialSuppressionReason::HostileStance);
}

TEST(PlayerbotSocialSelectionTest, ZeroRepliesIsAValidOutcomeForAnUninterestedField)
{
    // No fairness quota: a field of reserved bots that do not care produces silence, however high
    // the thread pressure is.
    PlayerbotSocialSelectionInput input = SelectionInput({Candidate(41, 5, PlayerbotSocialStance::Reserved),
                                                          Candidate(42, 4, PlayerbotSocialStance::Reserved),
                                                          Candidate(43, 3, PlayerbotSocialStance::Reserved)});
    input.replyPressure = PLAYERBOT_SOCIAL_REPLY_PRESSURE_CEILING;

    std::size_t spoke = 0;
    for (uint64 seed = 0; seed < 200; ++seed)
    {
        input.selectionSeed = seed;
        if (!PlayerbotSocialSelectResponders(input).responders.empty())
            ++spoke;
    }

    EXPECT_LT(spoke, 200u) << "a field of uninterested bots always replied, which is a forced quota";
}

TEST(PlayerbotSocialSelectionTest, AnEmptyCandidateFieldSelectsNobodyWithoutReportingAFailure)
{
    PlayerbotSocialSelection const selection = PlayerbotSocialSelectResponders(SelectionInput({}));

    EXPECT_TRUE(selection.responders.empty());
    EXPECT_TRUE(selection.alternates.empty());
    EXPECT_TRUE(selection.suppressions.empty());
    EXPECT_TRUE(selection.leadingFactors.empty());
}

TEST(PlayerbotSocialSelectionTest, BeingAddressedByNameOutweighsAHigherBaseDisposition)
{
    // Direct address is the strongest ordinary reason to answer. Asserted against a rival with a
    // materially higher disposition so the bonus is doing the work, not a tie break.
    PlayerbotSocialCandidate named = Candidate(51, 55, PlayerbotSocialStance::Receptive);
    named.addressedByName = true;

    PlayerbotSocialSelectionInput input = SelectionInput({named, Candidate(52, 75, PlayerbotSocialStance::Receptive)});
    input.replyPressure = PLAYERBOT_SOCIAL_REPLY_PRESSURE_CEILING;

    PlayerbotSocialSelection const selection = PlayerbotSocialSelectResponders(input);

    ASSERT_FALSE(selection.responders.empty());
    EXPECT_EQ(selection.responders.front(), 51u);
}

TEST(PlayerbotSocialSelectionTest, AQuestionCanMakeAnOtherwiseUninterestingLineWorthAnswering)
{
    PlayerbotSocialCandidate statement = Candidate(53, 3, PlayerbotSocialStance::Reserved);
    PlayerbotSocialSelectionInput input = SelectionInput({statement});
    input.replyPressure = PLAYERBOT_SOCIAL_REPLY_PRESSURE_CEILING;

    EXPECT_TRUE(PlayerbotSocialSelectResponders(input).responders.empty());

    PlayerbotSocialCandidate question = statement;
    question.askedQuestion = true;
    input.candidates = {question};

    PlayerbotSocialSelection const selection = PlayerbotSocialSelectResponders(input);

    ASSERT_FALSE(selection.responders.empty());
    EXPECT_EQ(selection.responders.front(), 53u);
}

TEST(PlayerbotSocialSelectionTest, AQuestionMarkMakesTheObservedLineAQuestion)
{
    EXPECT_TRUE(PlayerbotSocialMessageIsQuestion("can anyone help with this?"));
    EXPECT_TRUE(PlayerbotSocialMessageIsQuestion("really?!"));
    EXPECT_FALSE(PlayerbotSocialMessageIsQuestion("we should head north"));
    EXPECT_FALSE(PlayerbotSocialMessageIsQuestion(""));
}

TEST(PlayerbotSocialSelectionTest, TheLeadingFactorsExplainTheChoiceAndAreBounded)
{
    // Telemetry has to be able to say why a bot was chosen. Bounded so a large field cannot make one
    // decision record grow without limit.
    PlayerbotSocialCandidate named = Candidate(61, 80, PlayerbotSocialStance::Engaged);
    named.addressedByName = true;
    named.participatedInThread = true;

    PlayerbotSocialSelectionInput input = SelectionInput({named, Candidate(62, 40, PlayerbotSocialStance::Neutral)});
    input.replyPressure = PLAYERBOT_SOCIAL_REPLY_PRESSURE_CEILING;

    PlayerbotSocialSelection const selection = PlayerbotSocialSelectResponders(input);

    ASSERT_FALSE(selection.responders.empty());
    ASSERT_FALSE(selection.leadingFactors.empty());
    EXPECT_LE(selection.leadingFactors.size(), PLAYERBOT_SOCIAL_MAX_SELECTION_FACTORS);

    for (PlayerbotSocialSelectionFactor const& factor : selection.leadingFactors)
    {
        ASSERT_NE(factor.name, nullptr);
        EXPECT_STRNE(factor.name, "");
    }

    // The named bonus is one of the reported reasons, not an invisible thumb on the scale.
    bool namedFactorReported = false;
    for (PlayerbotSocialSelectionFactor const& factor : selection.leadingFactors)
        if (std::string(factor.name) == "addressed_by_name")
            namedFactorReported = true;

    EXPECT_TRUE(namedFactorReported);
}

TEST(PlayerbotSocialSelectionTest, AlternatesAndSuppressionsAreBounded)
{
    // A busy zone can present a large field. Everything the decision records stays bounded so one
    // opportunity cannot write an unbounded row.
    std::vector<PlayerbotSocialCandidate> field;
    for (uint64 index = 0; index < 60; ++index)
        field.push_back(
            Candidate(100 + index, static_cast<uint8>(30 + index % 50),
                      index % 4 == 0 ? PlayerbotSocialStance::Dismissive : PlayerbotSocialStance::Receptive));

    PlayerbotSocialSelection const selection = PlayerbotSocialSelectResponders(SelectionInput(field));

    EXPECT_LE(selection.alternates.size(), PLAYERBOT_SOCIAL_MAX_SELECTION_ALTERNATES);
    EXPECT_LE(selection.suppressions.size(), PLAYERBOT_SOCIAL_MAX_SELECTION_SUPPRESSIONS);
    EXPECT_LE(selection.leadingFactors.size(), PLAYERBOT_SOCIAL_MAX_SELECTION_FACTORS);
}

TEST(PlayerbotSocialSelectionTest, TheSameBotIsNeverSelectedTwiceFromADuplicatedField)
{
    // Server side duplicate suppression. A candidate list assembled from two overlapping sources
    // must not let one bot answer itself.
    PlayerbotSocialCandidate repeated = Candidate(71, 95, PlayerbotSocialStance::Engaged);

    PlayerbotSocialSelectionInput input = SelectionInput({repeated, repeated, repeated});
    input.secondResponderAllowed = true;
    input.replyPressure = PLAYERBOT_SOCIAL_REPLY_PRESSURE_CEILING;

    for (uint64 seed = 0; seed < 100; ++seed)
    {
        input.selectionSeed = seed;
        PlayerbotSocialSelection const selection = PlayerbotSocialSelectResponders(input);
        EXPECT_LE(CountResponder(selection, 71), 1u) << "seed " << seed << " selected one bot twice";
    }
}

TEST(PlayerbotSocialSelectionTest, ZeroPressureSelectsNobodyButStillReportsTheField)
{
    // Pressure gates the roll, not the scoring. Even when nothing is selected the alternates are
    // recorded, which is what lets the Medivh feed show a considered but silent opportunity.
    PlayerbotSocialSelectionInput input = SelectionInput(
        {Candidate(81, 90, PlayerbotSocialStance::Engaged), Candidate(82, 85, PlayerbotSocialStance::Engaged)});
    input.replyPressure = 0.0f;

    PlayerbotSocialSelection const selection = PlayerbotSocialSelectResponders(input);

    EXPECT_TRUE(selection.responders.empty());
    EXPECT_FALSE(selection.alternates.empty());
}

TEST(PlayerbotSocialSelectionTest, EverySuppressionReasonHasItsOwnExactName)
{
    /*
     * Pairs, not a bare list. A length check alone is not enough: an enumerator can be added to both
     * the list and the count while the name function still falls through to "unknown", and that
     * passes, because "unknown" is neither empty nor a duplicate until a second reason falls through
     * too. Pinning the exact string is what makes the fall through fail. Telemetry and the Medivh
     * suppression feed key off these strings, so they are contract, not debug text.
     */
    struct NamedReason
    {
        PlayerbotSocialSuppressionReason reason;
        char const* name;
    };

    NamedReason const expected[] = {{PlayerbotSocialSuppressionReason::HostileStance, "hostile_stance"},
                                    {PlayerbotSocialSuppressionReason::Uninterested, "uninterested"},
                                    {PlayerbotSocialSuppressionReason::DuplicateCandidate, "duplicate_candidate"},
                                    {PlayerbotSocialSuppressionReason::LostToHigherScore, "lost_to_higher_score"},
                                    {PlayerbotSocialSuppressionReason::InvalidStance, "invalid_stance"}};

    static_assert(std::size(expected) == PLAYERBOT_SOCIAL_SUPPRESSION_REASON_COUNT,
                  "every suppression reason needs an exact expected name here");

    std::vector<std::string> names;
    for (NamedReason const& pair : expected)
    {
        EXPECT_STREQ(PlayerbotSocialSuppressionReasonName(pair.reason), pair.name);
        names.emplace_back(PlayerbotSocialSuppressionReasonName(pair.reason));
    }

    std::sort(names.begin(), names.end());
    EXPECT_EQ(std::adjacent_find(names.begin(), names.end()), names.end());

    // An out of range value is named rather than reported as an empty string, and its name is not
    // one a declared reason uses.
    EXPECT_STREQ(PlayerbotSocialSuppressionReasonName(static_cast<PlayerbotSocialSuppressionReason>(200)), "unknown");
}

TEST(PlayerbotSocialPressureTest, DecayFlattensAtTheStepBoundRatherThanReachingZero)
{
    /*
     * The documented edge of the decay curve. Past the step bound the value stops falling instead of
     * continuing, because continuing would underflow the product of the two decays to exactly zero,
     * and zero is the hard cap the contract forbids. Pinned here so the trade cannot be silently
     * changed in either direction: the bound must not shrink into the range a real conversation
     * reaches, and it must not be removed and take the nonzero property with it.
     */
    PlayerbotSocialThreadPressure early = FreshHumanThread();
    early.consecutiveBotOnlyTurns = 60;

    PlayerbotSocialThreadPressure atBound = FreshHumanThread();
    atBound.consecutiveBotOnlyTurns = 64;

    PlayerbotSocialThreadPressure farBeyond = FreshHumanThread();
    farBeyond.consecutiveBotOnlyTurns = std::numeric_limits<uint32>::max();

    float const earlyPressure = PlayerbotSocialReplyPressure(early);
    float const boundPressure = PlayerbotSocialReplyPressure(atBound);
    float const beyondPressure = PlayerbotSocialReplyPressure(farBeyond);

    // Still strictly decaying right up to the bound.
    EXPECT_LT(boundPressure, earlyPressure);

    // Flat past it, and never zero.
    EXPECT_EQ(beyondPressure, boundPressure);
    EXPECT_GT(beyondPressure, 0.0f);

    // Even with both decays saturated at once, the product is a real positive float rather than an
    // underflow to zero. This is the case that decides the bound's value.
    PlayerbotSocialThreadPressure bothSaturated = FreshHumanThread();
    bothSaturated.consecutiveBotOnlyTurns = std::numeric_limits<uint32>::max();
    bothSaturated.nowUnixSeconds = bothSaturated.lastActivityUnixSeconds + 1000000;

    EXPECT_GT(PlayerbotSocialReplyPressure(bothSaturated), 0.0f);
    EXPECT_GT(PlayerbotSocialStarterPressure(bothSaturated), 0.0f);
}

TEST(PlayerbotSocialSelectionTest, ACandidateWithAnInvalidStanceIsRefusedRatherThanScored)
{
    // The module has neither -Wswitch nor -Werror, so a stance cast in from a corrupt row reaches
    // the selector. Testing only for Dismissive would fail open, because an unknown value is not
    // equal to it and would fall through into scoring with a high disposition.
    PlayerbotSocialCandidate corrupt = Candidate(91, 100, PlayerbotSocialStance::Engaged);
    corrupt.stance = static_cast<PlayerbotSocialStance>(200);
    corrupt.addressedByName = true;

    PlayerbotSocialSelectionInput input = SelectionInput({corrupt});
    input.replyPressure = PLAYERBOT_SOCIAL_REPLY_PRESSURE_CEILING;

    for (uint64 seed = 0; seed < 100; ++seed)
    {
        input.selectionSeed = seed;
        PlayerbotSocialSelection const selection = PlayerbotSocialSelectResponders(input);

        EXPECT_TRUE(selection.responders.empty()) << "seed " << seed << " let a corrupt stance speak";
    }

    PlayerbotSocialSelection const selection = PlayerbotSocialSelectResponders(input);
    ASSERT_EQ(selection.suppressions.size(), 1u);
    EXPECT_EQ(selection.suppressions.front().reason, PlayerbotSocialSuppressionReason::InvalidStance);
}

TEST(PlayerbotSocialSelectionTest, AValidStanceIsNotMistakenForACorruptOne)
{
    // The guard must refuse only what is actually outside the enumeration. Every declared stance
    // still reaches scoring, so the fix cannot have silenced the whole selector.
    for (PlayerbotSocialStance stance : {PlayerbotSocialStance::Reserved, PlayerbotSocialStance::Neutral,
                                         PlayerbotSocialStance::Receptive, PlayerbotSocialStance::Engaged})
    {
        EXPECT_TRUE(PlayerbotSocialStanceIsValid(stance));

        PlayerbotSocialSelectionInput input = SelectionInput({Candidate(92, 90, stance)});
        input.replyPressure = PLAYERBOT_SOCIAL_REPLY_PRESSURE_CEILING;
        input.selectionSeed = 3;

        PlayerbotSocialSelection const selection = PlayerbotSocialSelectResponders(input);
        EXPECT_TRUE(selection.suppressions.empty())
            << "stance " << static_cast<uint32>(stance) << " was wrongly refused as corrupt";
    }

    EXPECT_TRUE(PlayerbotSocialStanceIsValid(PlayerbotSocialStance::Dismissive));
    EXPECT_FALSE(PlayerbotSocialStanceIsValid(static_cast<PlayerbotSocialStance>(200)));
}

// Assistance evidence ------------------------------------------------------------------------------

TEST(PlayerbotSocialAssistanceTest, OverhealEarnsNoCredit)
{
    // The OnHeal hook reports the health actually restored, so a heal landing on a character who
    // was already full arrives here as zero. Nothing was done for them, and nothing is owed.
    PlayerbotSocialAssistanceTally tally;
    tally.effectiveHealing = 0;
    tally.contributionEvents = 40;

    PlayerbotSocialRelationshipValues const delta = PlayerbotSocialAssistanceDelta(tally, 10000);

    EXPECT_FLOAT_EQ(delta.familiarity, 0.0f);
    EXPECT_FLOAT_EQ(delta.affinity, 0.0f);
    EXPECT_FLOAT_EQ(delta.trust, 0.0f);
}

TEST(PlayerbotSocialAssistanceTest, SplittingOneHealIntoManyEarnsTheSameAsOne)
{
    // The anti farming property. Credit is a function of health restored, never of how many events
    // carried it, so a thousand one point ticks and a single thousand point heal are worth the same.
    PlayerbotSocialAssistanceTally spread;
    spread.effectiveHealing = 5000;
    spread.contributionEvents = 5000;

    PlayerbotSocialAssistanceTally single;
    single.effectiveHealing = 5000;
    single.contributionEvents = 1;

    PlayerbotSocialRelationshipValues const spreadDelta = PlayerbotSocialAssistanceDelta(spread, 10000);
    PlayerbotSocialRelationshipValues const singleDelta = PlayerbotSocialAssistanceDelta(single, 10000);

    EXPECT_FLOAT_EQ(spreadDelta.familiarity, singleDelta.familiarity);
    EXPECT_FLOAT_EQ(spreadDelta.affinity, singleDelta.affinity);
    EXPECT_FLOAT_EQ(spreadDelta.trust, singleDelta.trust);
}

TEST(PlayerbotSocialAssistanceTest, OneEncounterCannotExceedItsCeilings)
{
    // Absurd totals from a very long fight still land on the cap. Without this a raid night would
    // move a relationship further than years of conversation.
    PlayerbotSocialAssistanceTally enormous;
    enormous.effectiveHealing = 100000000;
    enormous.meaningfulDamage = 100000000;
    enormous.rescueCount = 500;
    enormous.contributionEvents = 100000;

    PlayerbotSocialRelationshipValues const delta = PlayerbotSocialAssistanceDelta(enormous, 10000);

    // Literal, not the constants. An expectation written as the constant it is testing passes no
    // matter what that constant becomes, which is exactly the bug this suite exists to catch.
    EXPECT_FLOAT_EQ(delta.familiarity, 0.05f);
    EXPECT_FLOAT_EQ(delta.affinity, 0.04f);
    EXPECT_FLOAT_EQ(delta.trust, 0.06f);

    // And the constants themselves are what the rest of the module reads, so pin them too.
    EXPECT_FLOAT_EQ(PLAYERBOT_SOCIAL_ASSISTANCE_FAMILIARITY_CAP, 0.05f);
    EXPECT_FLOAT_EQ(PLAYERBOT_SOCIAL_ASSISTANCE_AFFINITY_CAP, 0.04f);
    EXPECT_FLOAT_EQ(PLAYERBOT_SOCIAL_ASSISTANCE_TRUST_CAP, 0.06f);
    EXPECT_EQ(PLAYERBOT_SOCIAL_ASSISTANCE_RESCUE_CAP, 2u);
}

TEST(PlayerbotSocialAssistanceTest, RescuesPastTheCapAddNothing)
{
    PlayerbotSocialAssistanceTally atCap;
    atCap.effectiveHealing = 1;
    atCap.rescueCount = 2;

    PlayerbotSocialAssistanceTally beyondCap = atCap;
    beyondCap.rescueCount = 99;

    EXPECT_FLOAT_EQ(PlayerbotSocialAssistanceDelta(beyondCap, 10000).trust,
                    PlayerbotSocialAssistanceDelta(atCap, 10000).trust);
}

TEST(PlayerbotSocialAssistanceTest, TheSameHelpIsWorthTheSameAtEveryLevel)
{
    // Normalising by the beneficiary's maximum health is what makes this true. Healing someone from
    // half to full is the same favour whether the bar is 500 points or 50000.
    PlayerbotSocialAssistanceTally lowLevel;
    lowLevel.effectiveHealing = 250;

    PlayerbotSocialAssistanceTally highLevel;
    highLevel.effectiveHealing = 25000;

    PlayerbotSocialRelationshipValues const lowDelta = PlayerbotSocialAssistanceDelta(lowLevel, 500);
    PlayerbotSocialRelationshipValues const highDelta = PlayerbotSocialAssistanceDelta(highLevel, 50000);

    EXPECT_FLOAT_EQ(lowDelta.familiarity, highDelta.familiarity);
    EXPECT_FLOAT_EQ(lowDelta.affinity, highDelta.affinity);
}

TEST(PlayerbotSocialAssistanceTest, AnUnknownHealthScaleEarnsNothing)
{
    // A beneficiary whose maximum health could not be read gives nothing to measure against, and a
    // guess would be a number nobody can defend.
    PlayerbotSocialAssistanceTally tally;
    tally.effectiveHealing = 5000;
    tally.meaningfulDamage = 5000;
    tally.rescueCount = 3;

    PlayerbotSocialRelationshipValues const delta = PlayerbotSocialAssistanceDelta(tally, 0);

    EXPECT_FLOAT_EQ(delta.familiarity, 0.0f);
    EXPECT_FLOAT_EQ(delta.affinity, 0.0f);
    EXPECT_FLOAT_EQ(delta.trust, 0.0f);
}

// Combat context -----------------------------------------------------------------------------------

TEST(PlayerbotSocialCombatContextTest, ConsentedPvpIsNotHatred)
{
    // Agreeing to fight someone is not a reason to dislike them. A duel, an arena match, and a
    // battleground are all opposition the character opted into, so none of them applies a hostile
    // delta on their own. Something said or done inside them still can, through the chat paths.
    for (PlayerbotSocialCombatContext context :
         {PlayerbotSocialCombatContext::Duel, PlayerbotSocialCombatContext::Arena,
          PlayerbotSocialCombatContext::Battleground})
    {
        EXPECT_FALSE(PlayerbotSocialCombatContextAppliesHostility(context))
            << "context " << static_cast<uint32>(context) << " wrongly applied a hostile delta";
    }
}

TEST(PlayerbotSocialCombatContextTest, OpenWorldAggressionIsTheOneHostileContext)
{
    // Being attacked in the open world was not agreed to, so it is the only opposition that says
    // anything about the attacker.
    EXPECT_TRUE(PlayerbotSocialCombatContextAppliesHostility(PlayerbotSocialCombatContext::OpenWorldPvp));
    EXPECT_FALSE(PlayerbotSocialCombatContextAppliesHostility(PlayerbotSocialCombatContext::Cooperative));
}

TEST(PlayerbotSocialCombatContextTest, AnUnrecognizedContextIsRefusedRatherThanAssumedSafe)
{
    // This module compiles without -Wswitch, so a later enumerator added without updating the
    // switch would otherwise fall through to some default. Validity is checked explicitly, and an
    // unrecognized context applies no delta of either sign.
    PlayerbotSocialCombatContext const unknown = static_cast<PlayerbotSocialCombatContext>(200);

    EXPECT_FALSE(PlayerbotSocialCombatContextIsValid(unknown));
    EXPECT_FALSE(PlayerbotSocialCombatContextAppliesHostility(unknown));

    for (PlayerbotSocialCombatContext context :
         {PlayerbotSocialCombatContext::Cooperative, PlayerbotSocialCombatContext::Duel,
          PlayerbotSocialCombatContext::Arena, PlayerbotSocialCombatContext::Battleground,
          PlayerbotSocialCombatContext::OpenWorldPvp})
    {
        EXPECT_TRUE(PlayerbotSocialCombatContextIsValid(context));
    }
}

TEST(PlayerbotSocialCombatContextTest, AssistanceIsCreditedInEveryContextIncludingHostileOnes)
{
    // Teammate assistance and open world aggression are separate policies over the same encounter.
    // Someone healing you in a battleground earns the same credit they would earn in a dungeon; the
    // enemy attacking you there earns no hostility. The two questions never share an answer.
    PlayerbotSocialAssistanceTally tally;
    tally.effectiveHealing = 5000;

    PlayerbotSocialRelationshipValues const delta = PlayerbotSocialAssistanceDelta(tally, 10000);

    EXPECT_GT(delta.familiarity, 0.0f);
    EXPECT_GT(delta.affinity, 0.0f);
    EXPECT_FALSE(PlayerbotSocialCombatContextAppliesHostility(PlayerbotSocialCombatContext::Battleground));
}

TEST(PlayerbotSocialCombatContextTest, OpenWorldHostilityIsBoundedAndNegativeOnBothAxes)
{
    PlayerbotSocialRelationshipValues const delta = PlayerbotSocialOpenWorldAggressionDelta();

    EXPECT_LT(delta.affinity, 0.0f);
    EXPECT_LT(delta.trust, 0.0f);
    EXPECT_FLOAT_EQ(delta.affinity, -0.03f);
    EXPECT_FLOAT_EQ(delta.trust, -0.05f);
    EXPECT_FLOAT_EQ(delta.familiarity, 0.02f);

    // Being attacked makes someone memorable, so familiarity rises even as regard falls.
    EXPECT_GT(delta.familiarity, 0.0f);
}

// What the hooks are allowed to count -------------------------------------------------------------

TEST(PlayerbotSocialAssistanceTest, ARescueIsHelpGivenNearDeathNotATopUp)
{
    // A quarter of 10000 is 2500, worked out here rather than recomputed from the constant. If the
    // threshold moves, these four lines are supposed to fail.
    uint32 const maxHealth = 10000;

    EXPECT_FLOAT_EQ(PLAYERBOT_SOCIAL_RESCUE_HEALTH_FRACTION, 0.25f);

    EXPECT_TRUE(PlayerbotSocialHealIsRescue(1, maxHealth));
    EXPECT_TRUE(PlayerbotSocialHealIsRescue(2500, maxHealth));
    EXPECT_FALSE(PlayerbotSocialHealIsRescue(2501, maxHealth));
    EXPECT_FALSE(PlayerbotSocialHealIsRescue(maxHealth, maxHealth));
}

TEST(PlayerbotSocialAssistanceTest, ARescueNeedsAHealthScaleAndALivingTarget)
{
    // No maximum means nothing to compare against. Zero current health means the character was
    // already dead when the heal landed, which is a resurrection question this task does not answer.
    EXPECT_FALSE(PlayerbotSocialHealIsRescue(1, 0));
    EXPECT_FALSE(PlayerbotSocialHealIsRescue(0, 10000));
}

TEST(PlayerbotSocialAssistanceTest, DamageCountsOnlyAgainstAnEnemyTheBeneficiaryWasFighting)
{
    // The difference between helping someone and happening to hit the same target. Splash onto a
    // mob nobody in the group was engaged with is incidental and earns nothing, however large.
    EXPECT_TRUE(PlayerbotSocialDamageIsMeaningful(true, 1));
    EXPECT_FALSE(PlayerbotSocialDamageIsMeaningful(false, 1000000));
    EXPECT_FALSE(PlayerbotSocialDamageIsMeaningful(true, 0));
}

// The per pair credit window -----------------------------------------------------------------------

TEST(PlayerbotSocialAssistanceTest, SaturatingAdditionStopsAtTheCeilingRatherThanWrapping)
{
    // The accumulation test in the coordinator suite adds three UINT32_MAX heals, which is nowhere
    // near UINT64_MAX and so would pass with plain wrapping arithmetic. This is the boundary itself.
    EXPECT_EQ(PlayerbotSocialSaturatingAdd(0, 5), 5u);
    EXPECT_EQ(PlayerbotSocialSaturatingAdd(UINT64_MAX - 5, 5), UINT64_MAX);
    EXPECT_EQ(PlayerbotSocialSaturatingAdd(UINT64_MAX - 5, 6), UINT64_MAX);
    EXPECT_EQ(PlayerbotSocialSaturatingAdd(UINT64_MAX, 1), UINT64_MAX);
    EXPECT_EQ(PlayerbotSocialSaturatingIncrement(UINT32_MAX), UINT32_MAX);
}

TEST(PlayerbotSocialAssistanceTest, TheFirstPayoutInAWindowIsAdmittedInFull)
{
    PlayerbotSocialAssistanceCredit credit;
    PlayerbotSocialRelationshipValues delta;
    delta.familiarity = 0.02f;
    delta.affinity = 0.01f;
    delta.trust = 0.03f;

    PlayerbotSocialRelationshipValues const admitted = PlayerbotSocialAdmitAssistanceCredit(credit, delta, 1000);

    EXPECT_FLOAT_EQ(admitted.familiarity, 0.02f);
    EXPECT_FLOAT_EQ(admitted.affinity, 0.01f);
    EXPECT_FLOAT_EQ(admitted.trust, 0.03f);
    EXPECT_EQ(credit.windowStartedAtUnixSeconds, 1000u);
}

TEST(PlayerbotSocialAssistanceTest, RepeatedPayoutsInOneWindowCannotExceedTheCaps)
{
    // Definition of Done 2. Whatever the encounter boundaries turn out to be, the pair cannot earn
    // more than the ceilings inside one window. The literals are the caps, written out rather than
    // read from the constants, so moving a cap fails this test.
    PlayerbotSocialAssistanceCredit credit;
    PlayerbotSocialRelationshipValues delta;
    delta.familiarity = 0.05f;
    delta.affinity = 0.04f;
    delta.trust = 0.06f;

    PlayerbotSocialRelationshipValues total;
    for (int i = 0; i < 20; ++i)
    {
        PlayerbotSocialRelationshipValues const admitted =
            PlayerbotSocialAdmitAssistanceCredit(credit, delta, 1000 + i);
        total.familiarity += admitted.familiarity;
        total.affinity += admitted.affinity;
        total.trust += admitted.trust;
    }

    EXPECT_FLOAT_EQ(total.familiarity, 0.05f);
    EXPECT_FLOAT_EQ(total.affinity, 0.04f);
    EXPECT_FLOAT_EQ(total.trust, 0.06f);
}

TEST(PlayerbotSocialAssistanceTest, AnElapsedWindowRestoresTheFullCeiling)
{
    PlayerbotSocialAssistanceCredit credit;
    PlayerbotSocialRelationshipValues delta;
    delta.familiarity = 0.05f;

    EXPECT_FLOAT_EQ(PlayerbotSocialAdmitAssistanceCredit(credit, delta, 1000).familiarity, 0.05f);
    EXPECT_FLOAT_EQ(PlayerbotSocialAdmitAssistanceCredit(credit, delta, 1500).familiarity, 0.0f);

    uint64 const afterWindow = 1000 + PLAYERBOT_SOCIAL_ASSISTANCE_PAYOUT_WINDOW_SECONDS;
    EXPECT_FLOAT_EQ(PlayerbotSocialAdmitAssistanceCredit(credit, delta, afterWindow).familiarity, 0.05f);
    EXPECT_EQ(credit.windowStartedAtUnixSeconds, afterWindow);
}

TEST(PlayerbotSocialAssistanceTest, ARewoundClockDoesNotRestoreTheCeiling)
{
    // Restarting the window on a backwards clock is exactly the move that would pay the ceiling
    // twice, so a rewind holds the current window rather than opening a new one.
    PlayerbotSocialAssistanceCredit credit;
    PlayerbotSocialRelationshipValues delta;
    delta.familiarity = 0.05f;

    EXPECT_FLOAT_EQ(PlayerbotSocialAdmitAssistanceCredit(credit, delta, 5000).familiarity, 0.05f);
    EXPECT_FLOAT_EQ(PlayerbotSocialAdmitAssistanceCredit(credit, delta, 1000).familiarity, 0.0f);
    EXPECT_EQ(credit.windowStartedAtUnixSeconds, 5000u);
}

TEST(PlayerbotSocialAssistanceTest, APenaltyIsNeverRationedByTheWindow)
{
    // The ledger bounds gains. Repeated aggression should keep costing the aggressor, and the stored
    // value is held at its floor by the database rather than by rationing here.
    PlayerbotSocialAssistanceCredit credit;
    PlayerbotSocialRelationshipValues penalty;
    penalty.affinity = -0.03f;
    penalty.trust = -0.05f;

    for (int i = 0; i < 5; ++i)
    {
        PlayerbotSocialRelationshipValues const admitted =
            PlayerbotSocialAdmitAssistanceCredit(credit, penalty, 1000 + i);
        EXPECT_FLOAT_EQ(admitted.affinity, -0.03f);
        EXPECT_FLOAT_EQ(admitted.trust, -0.05f);
    }
}

TEST(PlayerbotSocialAssistanceTest, ACreditEntryExpiresOnceItsWindowIsFullyElapsed)
{
    // What lets the sweep prune the ledger instead of holding one entry per pair forever.
    PlayerbotSocialAssistanceCredit credit;
    credit.windowStarted = true;
    credit.windowStartedAtUnixSeconds = 1000;

    EXPECT_FALSE(PlayerbotSocialAssistanceCreditIsExpired(credit, 1000));
    EXPECT_FALSE(
        PlayerbotSocialAssistanceCreditIsExpired(credit, 1000 + PLAYERBOT_SOCIAL_ASSISTANCE_PAYOUT_WINDOW_SECONDS - 1));
    EXPECT_TRUE(
        PlayerbotSocialAssistanceCreditIsExpired(credit, 1000 + PLAYERBOT_SOCIAL_ASSISTANCE_PAYOUT_WINDOW_SECONDS));
}

TEST(PlayerbotSocialAssistanceTest, ADeltaMustItselfFitTheStorableRange)
{
    /*
     * The additive write adds on a duplicate key, but on a pair's first write the delta becomes the
     * inserted value and MySQL checks the column constraints against it before it notices the
     * duplicate. So the guard on a delta is the value range, not a separate one. Familiarity floors
     * at zero, which makes a negative familiarity delta inexpressible rather than merely large.
     */
    PlayerbotSocialRelationshipValues realistic;
    realistic.familiarity = 0.05f;
    realistic.affinity = -0.03f;
    realistic.trust = -0.05f;
    EXPECT_TRUE(PlayerbotSocialRelationshipIsInRange(realistic));

    PlayerbotSocialRelationshipValues negativeFamiliarity;
    negativeFamiliarity.familiarity = -0.01f;
    EXPECT_FALSE(PlayerbotSocialRelationshipIsInRange(negativeFamiliarity));

    PlayerbotSocialRelationshipValues tooLarge;
    tooLarge.affinity = -5.0f;
    EXPECT_FALSE(PlayerbotSocialRelationshipIsInRange(tooLarge));

    // The boundary itself is storable, so the guard rejects only what the column would reject.
    PlayerbotSocialRelationshipValues atTheBound;
    atTheBound.affinity = -1.0f;
    atTheBound.trust = -1.0f;
    EXPECT_TRUE(PlayerbotSocialRelationshipIsInRange(atTheBound));
}

TEST(PlayerbotSocialAssistanceTest, WithNoLedgerSlotAGainIsRefusedAndAPenaltyStillLands)
{
    /*
     * The ledger entry is what bounds a pair, not what credits it. Evicting one to make room would
     * hand that pair a fresh ceiling, so anyone able to churn PLAYERBOT_SOCIAL_MAX_TRACKED_PAIRS
     * other pairs could evict their own entry on demand and collect a new ceiling whenever they
     * liked. Refusing the gain is what keeps the per window bound a bound.
     */
    PlayerbotSocialRelationshipValues mixed;
    mixed.familiarity = 0.05f;
    mixed.affinity = -0.03f;
    mixed.trust = -0.05f;

    PlayerbotSocialRelationshipValues const admitted = PlayerbotSocialAdmitWithoutLedger(mixed);

    EXPECT_FLOAT_EQ(admitted.familiarity, 0.0f);
    EXPECT_FLOAT_EQ(admitted.affinity, -0.03f);
    EXPECT_FLOAT_EQ(admitted.trust, -0.05f);
}

TEST(PlayerbotSocialAssistanceTest, WithNoLedgerSlotAPurelyPositiveDeltaEarnsNothingAtAll)
{
    PlayerbotSocialRelationshipValues gain;
    gain.familiarity = 0.05f;
    gain.affinity = 0.04f;
    gain.trust = 0.06f;

    PlayerbotSocialRelationshipValues const admitted = PlayerbotSocialAdmitWithoutLedger(gain);

    EXPECT_FLOAT_EQ(admitted.familiarity, 0.0f);
    EXPECT_FLOAT_EQ(admitted.affinity, 0.0f);
    EXPECT_FLOAT_EQ(admitted.trust, 0.0f);
}

TEST(PlayerbotSocialAssistanceTest, AWindowStartingAtTimeZeroIsStillAStartedWindow)
{
    /*
     * Zero has to mean "no window yet" and nothing else. If it also reads as a valid start time, the
     * first gain at time zero leaves the ledger looking untouched, so the very next call opens a
     * fresh window and the pair collects the ceiling twice.
     */
    PlayerbotSocialAssistanceCredit credit;
    PlayerbotSocialRelationshipValues delta;
    delta.familiarity = 0.05f;

    EXPECT_FLOAT_EQ(PlayerbotSocialAdmitAssistanceCredit(credit, delta, 0).familiarity, 0.05f);
    EXPECT_FLOAT_EQ(PlayerbotSocialAdmitAssistanceCredit(credit, delta, 1).familiarity, 0.0f);

    // And it still expires normally once its window genuinely elapses from zero.
    EXPECT_FALSE(PlayerbotSocialAssistanceCreditIsExpired(credit, 1));
    EXPECT_TRUE(PlayerbotSocialAssistanceCreditIsExpired(credit, PLAYERBOT_SOCIAL_ASSISTANCE_PAYOUT_WINDOW_SECONDS));
}

TEST(PlayerbotSocialAssistanceTest, AnUntouchedLedgerIsDroppableOnSight)
{
    // Nothing has been spent, so keeping it changes no future answer.
    PlayerbotSocialAssistanceCredit untouched;
    EXPECT_TRUE(PlayerbotSocialAssistanceCreditIsExpired(untouched, 0));
    EXPECT_TRUE(PlayerbotSocialAssistanceCreditIsExpired(untouched, 5000));
}

// Opposition dedup ---------------------------------------------------------------------------------

TEST(PlayerbotSocialCombatContextTest, OppositionIsAnsweredOncePerFightNotOncePerSwing)
{
    // The damage hook fires on every hit, and a fight is dozens of them. Answering each one applies
    // the full aggression delta forty times over, which drives affinity to its floor on one gank.
    uint64 marker = 1000;

    for (uint64 swing = 1; swing < 40; ++swing)
    {
        EXPECT_TRUE(PlayerbotSocialOppositionIsSameFight(marker, 1000 + swing)) << "swing " << swing;
        marker = PlayerbotSocialAdvanceOppositionMarker(marker, 1000 + swing);
    }
}

TEST(PlayerbotSocialCombatContextTest, ASeparateFightLaterIsOpposedAgain)
{
    // The dedup is per fight, not permanent. Coming back an hour later to gank the same bot is a
    // second thing done, and it costs a second time.
    EXPECT_FALSE(PlayerbotSocialOppositionIsSameFight(1000, 1000 + PLAYERBOT_SOCIAL_ENCOUNTER_IDLE_SECONDS));
    EXPECT_TRUE(PlayerbotSocialOppositionIsSameFight(1000, 1000 + PLAYERBOT_SOCIAL_ENCOUNTER_IDLE_SECONDS - 1));
}

TEST(PlayerbotSocialCombatContextTest, ARewoundClockDoesNotReopenAFight)
{
    // Writing an earlier timestamp would let the idle threshold elapse against the rewound value and
    // answer the same fight twice, so the marker only ever moves forward.
    EXPECT_EQ(PlayerbotSocialAdvanceOppositionMarker(5000, 1000), 5000u);
    EXPECT_EQ(PlayerbotSocialAdvanceOppositionMarker(1000, 5000), 5000u);

    // And a backwards reading is no elapsed time rather than an enormous interval.
    EXPECT_TRUE(PlayerbotSocialOppositionIsSameFight(5000, 1000));
}

// Roleplay affinity --------------------------------------------------------------------------------

TEST(PlayerbotRoleplayPolicyTest, WillingnessVersionOwnsItsTransientNamespace)
{
    bool anyRollChanged = false;
    for (uint64 guid = 1; guid <= 200; ++guid)
    {
        if (PlayerbotRoleplayWillingnessRoll(0xABCDEFu, guid, 1) !=
            PlayerbotRoleplayWillingnessRoll(0xABCDEFu, guid, 2))
            anyRollChanged = true;
    }
    EXPECT_TRUE(anyRollChanged);
}

TEST(PlayerbotRoleplayPolicyTest, AffinityBandBoundariesMatchThePrd)
{
    EXPECT_EQ(PlayerbotRoleplayAffinityBandFor(1), PlayerbotRoleplayAffinityBand::Averse);
    EXPECT_EQ(PlayerbotRoleplayAffinityBandFor(50), PlayerbotRoleplayAffinityBand::Averse);
    EXPECT_EQ(PlayerbotRoleplayAffinityBandFor(51), PlayerbotRoleplayAffinityBand::Neutral);
    EXPECT_EQ(PlayerbotRoleplayAffinityBandFor(75), PlayerbotRoleplayAffinityBand::Neutral);
    EXPECT_EQ(PlayerbotRoleplayAffinityBandFor(76), PlayerbotRoleplayAffinityBand::Receptive);
    EXPECT_EQ(PlayerbotRoleplayAffinityBandFor(89), PlayerbotRoleplayAffinityBand::Receptive);
    EXPECT_EQ(PlayerbotRoleplayAffinityBandFor(90), PlayerbotRoleplayAffinityBand::Enthusiast);
    EXPECT_EQ(PlayerbotRoleplayAffinityBandFor(100), PlayerbotRoleplayAffinityBand::Enthusiast);

    // Out of range affinities fail closed to the averse band.
    EXPECT_EQ(PlayerbotRoleplayAffinityBandFor(0), PlayerbotRoleplayAffinityBand::Averse);
    EXPECT_EQ(PlayerbotRoleplayAffinityBandFor(101), PlayerbotRoleplayAffinityBand::Averse);
    EXPECT_EQ(PlayerbotRoleplayAffinityBandFor(255), PlayerbotRoleplayAffinityBand::Averse);
}

TEST(PlayerbotRoleplayPolicyTest, WillingnessRollIsDeterministicAndBounded)
{
    for (uint64 seed = 1; seed <= 500; ++seed)
    {
        uint8 const roll = PlayerbotRoleplayWillingnessRoll(seed, seed * 7919);
        EXPECT_GE(roll, 1u) << "seed " << seed;
        EXPECT_LE(roll, 25u) << "seed " << seed;
        EXPECT_EQ(roll, PlayerbotRoleplayWillingnessRoll(seed, seed * 7919)) << "seed " << seed;
    }
}

TEST(PlayerbotRoleplayPolicyTest, WillingnessPassingCountsAreExactAndMonotonic)
{
    uint32 previousPassing = 0;
    for (uint32 score = 1; score <= 100; ++score)
    {
        uint32 passing = 0;
        for (uint8 roll = 1; roll <= 25; ++roll)
            if (PlayerbotRoleplayWillingnessPasses(static_cast<uint8>(score), roll))
                ++passing;

        uint32 const expected = score <= 75 ? 0u : score - 75;
        EXPECT_EQ(passing, expected) << "score " << score;
        EXPECT_GE(passing, previousPassing) << "score " << score;
        previousPassing = passing;
    }
}

TEST(PlayerbotRoleplayPolicyTest, WillingnessBandAnchorsAreExplicit)
{
    auto passingCount = [](uint8 score)
    {
        uint32 passing = 0;
        for (uint8 roll = 1; roll <= 25; ++roll)
            if (PlayerbotRoleplayWillingnessPasses(score, roll))
                ++passing;
        return passing;
    };

    EXPECT_EQ(passingCount(76), 1u);
    EXPECT_EQ(passingCount(89), 14u);
    EXPECT_EQ(passingCount(90), 15u);
    EXPECT_EQ(passingCount(100), 25u);
}

TEST(PlayerbotRoleplayPolicyTest, WillingnessFailsClosedOnOutOfRangeInputs)
{
    EXPECT_FALSE(PlayerbotRoleplayWillingnessPasses(0, 1));
    EXPECT_FALSE(PlayerbotRoleplayWillingnessPasses(75, 1));
    EXPECT_FALSE(PlayerbotRoleplayWillingnessPasses(101, 1));
    EXPECT_FALSE(PlayerbotRoleplayWillingnessPasses(100, 0));
    EXPECT_FALSE(PlayerbotRoleplayWillingnessPasses(100, 26));
}

TEST(PlayerbotRoleplayPolicyTest, EnumValidityAndNamesFailClosed)
{
    EXPECT_TRUE(PlayerbotRoleplayAffinityBandIsValid(PlayerbotRoleplayAffinityBand::Averse));
    EXPECT_TRUE(PlayerbotRoleplayAffinityBandIsValid(PlayerbotRoleplayAffinityBand::Enthusiast));
    EXPECT_FALSE(PlayerbotRoleplayAffinityBandIsValid(static_cast<PlayerbotRoleplayAffinityBand>(255)));

    EXPECT_TRUE(PlayerbotRoleplayAssessmentKindIsValid(PlayerbotRoleplayAssessmentKind::Ordinary));
    EXPECT_TRUE(PlayerbotRoleplayAssessmentKindIsValid(PlayerbotRoleplayAssessmentKind::Uncertain));
    EXPECT_FALSE(PlayerbotRoleplayAssessmentKindIsValid(static_cast<PlayerbotRoleplayAssessmentKind>(255)));

    EXPECT_TRUE(PlayerbotRoleplayPromptModeIsValid(PlayerbotRoleplayPromptMode::Ordinary));
    EXPECT_TRUE(PlayerbotRoleplayPromptModeIsValid(PlayerbotRoleplayPromptMode::AuthorizedRoleplay));
    EXPECT_FALSE(PlayerbotRoleplayPromptModeIsValid(static_cast<PlayerbotRoleplayPromptMode>(255)));

    EXPECT_STREQ(PlayerbotRoleplayAffinityBandName(PlayerbotRoleplayAffinityBand::Averse), "averse");
    EXPECT_STREQ(PlayerbotRoleplayAffinityBandName(PlayerbotRoleplayAffinityBand::Neutral), "neutral");
    EXPECT_STREQ(PlayerbotRoleplayAffinityBandName(PlayerbotRoleplayAffinityBand::Receptive), "receptive");
    EXPECT_STREQ(PlayerbotRoleplayAffinityBandName(PlayerbotRoleplayAffinityBand::Enthusiast), "enthusiast");
    EXPECT_STREQ(PlayerbotRoleplayAffinityBandName(static_cast<PlayerbotRoleplayAffinityBand>(255)), "unknown");

    EXPECT_STREQ(PlayerbotRoleplayAssessmentKindName(PlayerbotRoleplayAssessmentKind::Ordinary), "ordinary");
    EXPECT_STREQ(PlayerbotRoleplayAssessmentKindName(PlayerbotRoleplayAssessmentKind::RoleplayInvitation),
                 "roleplay_invitation");
    EXPECT_STREQ(PlayerbotRoleplayAssessmentKindName(PlayerbotRoleplayAssessmentKind::RoleplayContinuation),
                 "roleplay_continuation");
    EXPECT_STREQ(PlayerbotRoleplayAssessmentKindName(PlayerbotRoleplayAssessmentKind::Practical), "practical");
    EXPECT_STREQ(PlayerbotRoleplayAssessmentKindName(PlayerbotRoleplayAssessmentKind::OptOut), "opt_out");
    EXPECT_STREQ(PlayerbotRoleplayAssessmentKindName(PlayerbotRoleplayAssessmentKind::Uncertain), "uncertain");
    EXPECT_STREQ(PlayerbotRoleplayAssessmentKindName(static_cast<PlayerbotRoleplayAssessmentKind>(255)), "unknown");

    EXPECT_STREQ(PlayerbotRoleplayPromptModeName(PlayerbotRoleplayPromptMode::Ordinary), "ordinary");
    EXPECT_STREQ(PlayerbotRoleplayPromptModeName(PlayerbotRoleplayPromptMode::DeclineRoleplay), "decline_roleplay");
    EXPECT_STREQ(PlayerbotRoleplayPromptModeName(PlayerbotRoleplayPromptMode::AcknowledgeRoleplay),
                 "acknowledge_roleplay");
    EXPECT_STREQ(PlayerbotRoleplayPromptModeName(PlayerbotRoleplayPromptMode::AuthorizedRoleplay),
                 "authorized_roleplay");
    EXPECT_STREQ(PlayerbotRoleplayPromptModeName(static_cast<PlayerbotRoleplayPromptMode>(255)), "unknown");
}
