/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#include <cstddef>
#include <iterator>
#include <set>
#include <string>
#include <vector>

#include "Bot/Engine/AiObjectContext.h"
#include "Bot/PlayerbotMgr.h"
#include "Bot/Social/PlayerbotSocialConfig.h"
#include "Bot/Social/PlayerbotSocialPolicy.h"
#include "Bot/Social/PlayerbotSocialRoute.h"
#include "BroadcastHelper.h"
#include "IntegrationTestFixture.h"
#include "ObjectAccessor.h"
#include "PlayerbotAI.h"
#include "gtest/gtest.h"

void AddPlayerbotsSocialScripts();

namespace
{
PlayerbotPersonalityProfile StoredPersonality()
{
    PlayerbotPersonalityProfile profile;
    profile.craftingAffinity = 50;
    profile.gatheringAffinity = 50;
    profile.explorationAffinity = 50;
    profile.sociability = 80;
    profile.voice = PlayerbotVoice::Wry;
    profile.fictionalAge = 36;
    profile.fictionalHomeCountry = "Ireland";
    profile.roleplayAffinity = 100;
    return profile;
}

PlayerbotSocialGate EnabledGate()
{
    PlayerbotSocialGate gate;
    gate.enabled = true;
    gate.stage = PlayerbotSocialRolloutStage::BoundedContinuation;
    return gate;
}

PlayerbotSocialGate DisabledGate() { return PlayerbotSocialGate(); }

// Ordinary chat: not something the feature delivered, and not heard inside a battleground.
PlayerbotSocialInboundContext HeardNormally() { return PlayerbotSocialInboundContext(); }

PlayerbotSocialInboundContext DeliveredBySocial()
{
    PlayerbotSocialInboundContext context;
    context.originatedFromSocialDelivery = true;
    return context;
}

PlayerbotSocialInboundContext HeardInsideABattleground()
{
    PlayerbotSocialInboundContext context;
    context.listenerInBattleground = true;
    return context;
}

PlayerbotSocialInboundContext MachineTraffic()
{
    PlayerbotSocialInboundContext context;
    context.machineTraffic = true;
    return context;
}

PlayerbotSocialInboundContext FunctionalTraffic()
{
    PlayerbotSocialInboundContext context;
    context.functionalTraffic = true;
    return context;
}

class PlayerbotSocialFunctionalTrafficTest : public IntegrationTestFixture
{
protected:
    void SetUp() override
    {
        IntegrationTestFixture::SetUp();

        static bool contextsBuilt = false;
        if (!contextsBuilt)
        {
            AiObjectContext::BuildAllSharedContexts();
            contextsBuilt = true;
        }

        _restoreEnabled = sPlayerbotAIConfig.enabled;
        sPlayerbotAIConfig.enabled = true;
        _bot = CreateTestPlayer(790, "CommandBot");
        sPlayerbotsMgr.AddPlayerbotData(_bot, true);
        _botAI = sPlayerbotsMgr.GetPlayerbotAI(_bot);
        ASSERT_NE(_botAI, nullptr);
    }

    void TearDown() override
    {
        delete _botAI;
        _botAI = nullptr;
        sPlayerbotAIConfig.enabled = _restoreEnabled;
        IntegrationTestFixture::TearDown();
    }

    PlayerbotAI* _botAI = nullptr;

private:
    TestPlayer* _bot = nullptr;
    bool _restoreEnabled = false;
};

// Every chat surface the module can resolve, so a routing claim covers the whole enum rather than
// the handful of values a test happened to name.
std::vector<ChatChannelSource> AllChatChannelSources()
{
    std::vector<ChatChannelSource> sources;
    for (int value = 0; value <= static_cast<int>(ChatChannelSource::SRC_UNDEFINED); ++value)
        sources.push_back(static_cast<ChatChannelSource>(value));

    return sources;
}

// The eight destinations the ambient broadcast funnel can be asked for.
std::vector<BroadcastHelper::ToChannel> AllBroadcastDestinations()
{
    return {BroadcastHelper::TO_GUILD,
            BroadcastHelper::TO_WORLD,
            BroadcastHelper::TO_GENERAL,
            BroadcastHelper::TO_TRADE,
            BroadcastHelper::TO_LOOKING_FOR_GROUP,
            BroadcastHelper::TO_LOCAL_DEFENSE,
            BroadcastHelper::TO_WORLD_DEFENSE,
            BroadcastHelper::TO_GUILD_RECRUITMENT};
}
}  // namespace

TEST(PlayerbotSocialRolloutStageTest, MissingAndInvalidConfigurationResolveToHumanReplies)
{
    EXPECT_EQ(PlayerbotSocialParseRolloutStage(""), PlayerbotSocialRolloutStage::HumanReplies);
    EXPECT_EQ(PlayerbotSocialParseRolloutStage("future"), PlayerbotSocialRolloutStage::HumanReplies);
    EXPECT_EQ(PlayerbotSocialParseRolloutStage("grounded_presence"), PlayerbotSocialRolloutStage::GroundedStarters);
    EXPECT_EQ(PlayerbotSocialParseRolloutStage("grounded_starters"), PlayerbotSocialRolloutStage::HumanReplies);
    EXPECT_EQ(PlayerbotSocialParseRolloutStage("bounded_continuation"),
              PlayerbotSocialRolloutStage::BoundedContinuation);
}

TEST(PlayerbotSocialRolloutStageTest, LaterBehaviorCannotLeakIntoAnEarlierStage)
{
    for (PlayerbotSocialRolloutStage const stage :
         {PlayerbotSocialRolloutStage::HumanReplies, PlayerbotSocialRolloutStage::GroundedStarters,
          PlayerbotSocialRolloutStage::BoundedContinuation})
    {
        PlayerbotSocialGate gate = EnabledGate();
        gate.stage = stage;

        EXPECT_EQ(PlayerbotSocialRouteInbound(ChatChannelSource::SRC_SAY, HeardNormally(), gate).route,
                  PlayerbotSocialInboundRoute::SocialOpportunity);

        PlayerbotSocialInboundRoute const generated =
            PlayerbotSocialRouteInbound(ChatChannelSource::SRC_SAY, DeliveredBySocial(), gate).route;
        EXPECT_EQ(generated, stage == PlayerbotSocialRolloutStage::BoundedContinuation
                                 ? PlayerbotSocialInboundRoute::SocialOpportunity
                                 : PlayerbotSocialInboundRoute::ThreadContinuationOnly);

        PlayerbotSocialBroadcastRoute const starter = PlayerbotSocialRouteBroadcast(BroadcastHelper::TO_GENERAL, gate);
        EXPECT_EQ(starter, stage == PlayerbotSocialRolloutStage::HumanReplies
                               ? PlayerbotSocialBroadcastRoute::SuppressCannedDelivery
                               : PlayerbotSocialBroadcastRoute::StarterContext);
    }
}

TEST(PlayerbotSocialNearbySnapshotTest, OnlyVisibleFactionCompatibleCharactersInPhysicalEarshotSurvive)
{
    auto names = [](std::vector<PlayerbotSocialNearbySnapshotEntry> const& entries)
    {
        std::vector<std::string> result;
        for (PlayerbotSocialNearbySnapshotEntry const& entry : entries)
            result.push_back(entry.name);
        return result;
    };

    PlayerbotSocialNearbyCharacter eligible;
    eligible.characterGuidCounter = 1;
    eligible.name = "Deszy";
    eligible.sameMap = true;
    eligible.samePhase = true;
    eligible.visible = true;
    eligible.factionMatches = true;
    eligible.consented = true;
    eligible.sameZone = true;
    eligible.sameParty = true;
    eligible.channelMember = true;
    eligible.withinRange = true;

    std::vector<PlayerbotSocialNearbyCharacter> characters;
    characters.push_back(eligible);

    for (int rejectedField = 0; rejectedField < 9; ++rejectedField)
    {
        PlayerbotSocialNearbyCharacter rejected = eligible;
        rejected.name = "rejected" + std::to_string(rejectedField);
        switch (rejectedField)
        {
            case 0:
                rejected.sameMap = false;
                break;
            case 1:
                rejected.samePhase = false;
                break;
            case 2:
                rejected.visible = false;
                break;
            case 3:
                rejected.factionMatches = false;
                break;
            case 4:
                rejected.sameZone = false;
                break;
            case 5:
                rejected.withinRange = false;
                break;
            case 6:
                rejected.isObserver = true;
                break;
            case 7:
                rejected.channelMember = false;
                break;
            case 8:
                rejected.consented = false;
                break;
        }
        characters.push_back(std::move(rejected));
    }

    EXPECT_EQ(names(PlayerbotSocialSelectNearby(PlayerbotSocialChannel::General, characters)),
              (std::vector<std::string>{"Deszy"}));
    EXPECT_EQ(names(PlayerbotSocialSelectNearby(PlayerbotSocialChannel::Say, characters)),
              (std::vector<std::string>{"Deszy", "rejected4", "rejected7"}))
        << "say is neither zone nor channel scoped";
}

TEST(PlayerbotSocialNearbySnapshotTest, PartyRequiresMembershipAndWhisperExposesNoBystanders)
{
    auto names = [](std::vector<PlayerbotSocialNearbySnapshotEntry> const& entries)
    {
        std::vector<std::string> result;
        for (PlayerbotSocialNearbySnapshotEntry const& entry : entries)
            result.push_back(entry.name);
        return result;
    };

    PlayerbotSocialNearbyCharacter member;
    member.characterGuidCounter = 1;
    member.name = "Barnek";
    member.sameMap = true;
    member.samePhase = true;
    member.visible = true;
    member.factionMatches = true;
    member.consented = true;
    member.sameZone = true;
    member.sameParty = true;
    member.channelMember = true;
    member.withinRange = true;

    PlayerbotSocialNearbyCharacter stranger = member;
    stranger.characterGuidCounter = 2;
    stranger.name = "Stranger";
    stranger.sameParty = false;

    EXPECT_EQ(names(PlayerbotSocialSelectNearby(PlayerbotSocialChannel::Party, {member, stranger})),
              (std::vector<std::string>{"Barnek"}));
    EXPECT_TRUE(PlayerbotSocialSelectNearby(PlayerbotSocialChannel::Whisper, {member, stranger}).empty())
        << "a private prompt must not enumerate unrelated bystanders";
}

TEST(PlayerbotSocialDispatchTest, ListenerFanoutObservesOneGameMessageOnlyOnce)
{
    PlayerbotSocialObservation observation;
    observation.key.channel = PlayerbotSocialChannel::Say;
    observation.key.scopeId = 0xF15C;
    observation.speakerGuidCounter = 900;
    observation.speakerName = "Barnek";
    observation.speakerIsHuman = false;
    observation.atUnixSeconds = 1000;
    observation.text = "the mine is busy tonight";

    PlayerbotSocialThreadHandle first;
    PlayerbotSocialThreadHandle second;
    {
        PlayerbotSocialDispatchScope const dispatch;
        first = PlayerbotSocialObserveOncePerDispatch(observation);
        second = PlayerbotSocialObserveOncePerDispatch(observation);
    }

    ASSERT_TRUE(first.valid);
    EXPECT_EQ(second.threadId, first.threadId);
    EXPECT_FALSE(second.duplicateOfRecentMessage) << "listener fanout is one game event, not a repeated chat line";

    PlayerbotSocialRequestContext const context = sPlayerbotSocialMgr.ComposeRequestContext(
        500, StoredPersonality(), 900, PlayerbotSocialChannel::Say, "", 1000, first.publicId);
    EXPECT_EQ(context.thread, (std::vector<std::string>{"Barnek: the mine is busy tonight"}));

    sPlayerbotSocialMgr.PruneStaleThreads(1000 + PLAYERBOT_SOCIAL_THREAD_STALE_SECONDS + 1);
}

TEST(PlayerbotSocialDispatchTest, HumanListenerFanoutMintsAndRecordsOneExactObservationEvent)
{
    PlayerbotSocialObservation observation;
    observation.key.channel = PlayerbotSocialChannel::General;
    observation.key.scopeId = 0xF160;
    observation.speakerGuidCounter = 901;
    observation.speakerName = "Elyse";
    observation.speakerIsHuman = true;
    observation.zoneId = 12;
    observation.atUnixSeconds = 1000;
    observation.text = "the mine is busy tonight";

    sPlayerbotSocialMgr.ApplyConsentSnapshot(observation.speakerGuidCounter, false);
    std::size_t const before = sPlayerbotSocialMgr.PendingEventCount();

    PlayerbotSocialThreadHandle first;
    PlayerbotSocialThreadHandle second;
    {
        PlayerbotSocialDispatchScope const dispatch;
        first = PlayerbotSocialObserveOncePerDispatch(observation);
        second = PlayerbotSocialObserveOncePerDispatch(observation);
    }

    ASSERT_TRUE(first.valid);
    EXPECT_EQ(second.threadId, first.threadId);
    ASSERT_EQ(sPlayerbotSocialMgr.PendingEventCount(), before + 1);
    std::vector<std::string> const eventIds = sPlayerbotSocialMgr.RecentEventIdsOf(first);
    ASSERT_EQ(eventIds.size(), 1u);
    EXPECT_TRUE(PlayerbotSocialPublicIdIsValid(PlayerbotSocialIdKind::Event, eventIds.front()));

    sPlayerbotSocialMgr.ForgetConsent(observation.speakerGuidCounter);
    sPlayerbotSocialMgr.PruneStaleThreads(1000 + PLAYERBOT_SOCIAL_THREAD_STALE_SECONDS + 1);
}

TEST(PlayerbotSocialDispatchTest, AListenerPacketOutsideTheChatDispatchCannotDuplicateTheObservation)
{
    std::size_t const trackedBefore = sPlayerbotSocialMgr.TrackedScopeCount();
    PlayerbotSocialObservation observation;
    observation.key.channel = PlayerbotSocialChannel::General;
    observation.key.scopeId = 0xF15E;
    observation.speakerGuidCounter = 900;
    observation.speakerName = "Barnek";
    observation.speakerIsHuman = false;
    observation.atUnixSeconds = 1000;
    observation.text = "the forge is busy tonight";

    EXPECT_FALSE(PlayerbotSocialObserveOncePerDispatch(observation).valid)
        << "packet fanout runs after the chat dispatch and must not observe the line again";
    EXPECT_EQ(sPlayerbotSocialMgr.TrackedScopeCount(), trackedBefore);
}

TEST(PlayerbotSocialDispatchTest, ARepeatedLineInAnotherDispatchRemainsARealPromptTurn)
{
    PlayerbotSocialObservation observation;
    observation.key.channel = PlayerbotSocialChannel::Say;
    observation.key.scopeId = 0xF15D;
    observation.speakerGuidCounter = 900;
    observation.speakerName = "Barnek";
    observation.speakerIsHuman = false;
    observation.atUnixSeconds = 1000;
    observation.text = "still waiting by the mine";

    PlayerbotSocialThreadHandle first;
    {
        PlayerbotSocialDispatchScope const dispatch;
        first = PlayerbotSocialObserveOncePerDispatch(observation);
    }

    observation.atUnixSeconds = 1001;
    PlayerbotSocialThreadHandle repeated;
    {
        PlayerbotSocialDispatchScope const dispatch;
        repeated = PlayerbotSocialObserveOncePerDispatch(observation);
    }

    ASSERT_TRUE(first.valid);
    EXPECT_TRUE(repeated.duplicateOfRecentMessage)
        << "the existing reply suppression still sees a genuinely repeated line";

    PlayerbotSocialRequestContext const context = sPlayerbotSocialMgr.ComposeRequestContext(
        500, StoredPersonality(), 900, PlayerbotSocialChannel::Say, "", 1001, first.publicId);
    EXPECT_EQ(context.thread,
              (std::vector<std::string>{"Barnek: still waiting by the mine", "Barnek: still waiting by the mine"}));

    sPlayerbotSocialMgr.PruneStaleThreads(1001 + PLAYERBOT_SOCIAL_THREAD_STALE_SECONDS + 1);
}

// Density profile ----------------------------------------------------------------------------------

TEST(PlayerbotSocialDensityProfileTest, EachProfileNameRoundTripsThroughTheParser)
{
    struct Case
    {
        char const* text;
        PlayerbotSocialDensityProfile expected;
    };

    constexpr Case CASES[] = {{"quiet", PlayerbotSocialDensityProfile::Quiet},
                              {"normal", PlayerbotSocialDensityProfile::Normal},
                              {"lively", PlayerbotSocialDensityProfile::Lively}};

    static_assert(std::size(CASES) == PLAYERBOT_SOCIAL_DENSITY_PROFILE_COUNT,
                  "every density profile needs a parse case");

    for (Case const& testCase : CASES)
    {
        EXPECT_EQ(PlayerbotSocialParseDensityProfile(testCase.text), testCase.expected) << testCase.text;
        EXPECT_STREQ(PlayerbotSocialDensityProfileName(testCase.expected), testCase.text);
    }
}

TEST(PlayerbotSocialDensityProfileTest, ProfileNamesAreCaseInsensitiveAndIgnoreSurroundingSpace)
{
    EXPECT_EQ(PlayerbotSocialParseDensityProfile("QUIET"), PlayerbotSocialDensityProfile::Quiet);
    EXPECT_EQ(PlayerbotSocialParseDensityProfile("Lively"), PlayerbotSocialDensityProfile::Lively);
    EXPECT_EQ(PlayerbotSocialParseDensityProfile("  normal  "), PlayerbotSocialDensityProfile::Normal);
}

TEST(PlayerbotSocialDensityProfileTest, AnUnreadableProfileFallsBackToNormalRatherThanToAnExtreme)
{
    // A typo must not silently mute the bots, and it must not silently make them loud either.
    EXPECT_EQ(PlayerbotSocialParseDensityProfile(""), PlayerbotSocialDensityProfile::Normal);
    EXPECT_EQ(PlayerbotSocialParseDensityProfile("liveley"), PlayerbotSocialDensityProfile::Normal);
    EXPECT_EQ(PlayerbotSocialParseDensityProfile("silent"), PlayerbotSocialDensityProfile::Normal);
    EXPECT_EQ(PlayerbotSocialParseDensityProfile("2"), PlayerbotSocialDensityProfile::Normal);
}

TEST(PlayerbotSocialDensityProfileTest, ACorruptProfileValueIsNotMistakenForAValidOne)
{
    EXPECT_TRUE(PlayerbotSocialDensityProfileIsValid(PlayerbotSocialDensityProfile::Quiet));
    EXPECT_TRUE(PlayerbotSocialDensityProfileIsValid(PlayerbotSocialDensityProfile::Lively));
    EXPECT_FALSE(PlayerbotSocialDensityProfileIsValid(static_cast<PlayerbotSocialDensityProfile>(3)));
    EXPECT_FALSE(PlayerbotSocialDensityProfileIsValid(static_cast<PlayerbotSocialDensityProfile>(200)));
    EXPECT_STREQ(PlayerbotSocialDensityProfileName(static_cast<PlayerbotSocialDensityProfile>(3)), "unknown");
}

// Inbound routing ----------------------------------------------------------------------------------

TEST(PlayerbotSocialInboundRouteTest, TheFourSupportedSurfacesMapOntoTheFourSupportedChannels)
{
    struct Case
    {
        ChatChannelSource source;
        PlayerbotSocialChannel expected;
    };

    constexpr Case CASES[] = {{ChatChannelSource::SRC_GENERAL, PlayerbotSocialChannel::General},
                              {ChatChannelSource::SRC_SAY, PlayerbotSocialChannel::Say},
                              {ChatChannelSource::SRC_PARTY, PlayerbotSocialChannel::Party},
                              {ChatChannelSource::SRC_WHISPER, PlayerbotSocialChannel::Whisper}};

    for (Case const& testCase : CASES)
    {
        PlayerbotSocialChannel channel = PlayerbotSocialChannel::Whisper;
        EXPECT_TRUE(PlayerbotSocialChannelFromChatSource(testCase.source, channel));
        EXPECT_EQ(channel, testCase.expected);
    }
}

TEST(PlayerbotSocialInboundRouteTest, EverySurfaceOutsideTheSupportedFourIsRefused)
{
    std::set<ChatChannelSource> const supported = {ChatChannelSource::SRC_GENERAL, ChatChannelSource::SRC_SAY,
                                                   ChatChannelSource::SRC_PARTY, ChatChannelSource::SRC_WHISPER};

    // Guild, World, Trade, Looking For Group, both defense channels, guild recruitment, yell, both
    // emote forms, raid, and the undefined surface that battleground and raid warning resolve to.
    for (ChatChannelSource source : AllChatChannelSources())
    {
        if (supported.count(source))
            continue;

        PlayerbotSocialChannel channel = PlayerbotSocialChannel::General;
        EXPECT_FALSE(PlayerbotSocialChannelFromChatSource(source, channel))
            << "source " << static_cast<int>(source) << " must not reach the social core";
    }
}

TEST(PlayerbotSocialInboundRouteTest, TheEnumerationOfChatSourcesIsStillComplete)
{
    /*
     * The refusal test above walks the enum by integer, so it only stays exhaustive while
     * SRC_UNDEFINED remains the last enumerator. A surface appended after it would be walked past and
     * silently escape the check.
     */
    EXPECT_EQ(ChatChannelSourceStr.size(), static_cast<std::size_t>(ChatChannelSource::SRC_UNDEFINED) + 1);
    EXPECT_EQ(ChatChannelSourceStr.rbegin()->first, ChatChannelSource::SRC_UNDEFINED);
}

TEST(PlayerbotSocialInboundRouteTest, WithTheGateOffEverySurfaceKeepsItsLegacyReply)
{
    for (ChatChannelSource source : AllChatChannelSources())
    {
        PlayerbotSocialInboundDecision const decision =
            PlayerbotSocialRouteInbound(source, HeardNormally(), DisabledGate());
        EXPECT_EQ(decision.route, PlayerbotSocialInboundRoute::LegacyOnly) << "source " << static_cast<int>(source);
        EXPECT_FALSE(decision.suppressLegacyReply);
    }
}

TEST(PlayerbotSocialInboundRouteTest, WithTheGateOffEvenTheFeaturesOwnDeliveryStaysOnTheLegacyPath)
{
    PlayerbotSocialInboundDecision const decision =
        PlayerbotSocialRouteInbound(ChatChannelSource::SRC_GENERAL, DeliveredBySocial(), DisabledGate());

    EXPECT_EQ(decision.route, PlayerbotSocialInboundRoute::LegacyOnly);
    EXPECT_FALSE(decision.suppressLegacyReply);
}

TEST(PlayerbotSocialInboundRouteTest, WithTheGateOnASupportedSurfaceBecomesAnOpportunityAndDropsTheCannedReply)
{
    struct Case
    {
        ChatChannelSource source;
        PlayerbotSocialChannel expected;
    };

    constexpr Case CASES[] = {{ChatChannelSource::SRC_GENERAL, PlayerbotSocialChannel::General},
                              {ChatChannelSource::SRC_SAY, PlayerbotSocialChannel::Say},
                              {ChatChannelSource::SRC_PARTY, PlayerbotSocialChannel::Party},
                              {ChatChannelSource::SRC_WHISPER, PlayerbotSocialChannel::Whisper}};

    for (Case const& testCase : CASES)
    {
        PlayerbotSocialInboundDecision const decision =
            PlayerbotSocialRouteInbound(testCase.source, HeardNormally(), EnabledGate());

        EXPECT_EQ(decision.route, PlayerbotSocialInboundRoute::SocialOpportunity);
        EXPECT_EQ(decision.channel, testCase.expected);
        EXPECT_TRUE(decision.suppressLegacyReply);
    }
}

TEST(PlayerbotSocialInboundRouteTest, AddonTrafficKeepsItsCommandPathAndNeverBecomesConversation)
{
    for (ChatChannelSource source : {ChatChannelSource::SRC_GENERAL, ChatChannelSource::SRC_SAY,
                                     ChatChannelSource::SRC_PARTY, ChatChannelSource::SRC_WHISPER})
    {
        PlayerbotSocialInboundDecision const decision =
            PlayerbotSocialRouteInbound(source, MachineTraffic(), EnabledGate());

        EXPECT_EQ(decision.route, PlayerbotSocialInboundRoute::LegacyOnly) << "source " << static_cast<int>(source);
        EXPECT_FALSE(decision.suppressLegacyReply)
            << "the caller still has to hand addon traffic to PlayerbotAI::HandleCommand";
    }
}

TEST(PlayerbotSocialInboundRouteTest, PlayerbotCommandsKeepTheirCommandPathAndNeverBecomeConversation)
{
    for (ChatChannelSource source : {ChatChannelSource::SRC_PARTY, ChatChannelSource::SRC_WHISPER})
    {
        PlayerbotSocialInboundDecision const decision =
            PlayerbotSocialRouteInbound(source, FunctionalTraffic(), EnabledGate());

        EXPECT_EQ(decision.route, PlayerbotSocialInboundRoute::LegacyOnly) << "source " << static_cast<int>(source);
        EXPECT_FALSE(decision.suppressLegacyReply)
            << "the caller still has to hand recognized commands to PlayerbotAI::HandleCommand";
    }
}

TEST_F(PlayerbotSocialFunctionalTrafficTest, RealCommandResolutionAppliesOnlyToConversationalCommandSurfaces)
{
    for (ChatChannelSource source : {ChatChannelSource::SRC_PARTY, ChatChannelSource::SRC_WHISPER})
        EXPECT_TRUE(PlayerbotSocialIsFunctionalTraffic(_botAI, source, false, "maintenance"));

    EXPECT_FALSE(PlayerbotSocialIsFunctionalTraffic(_botAI, ChatChannelSource::SRC_WHISPER, false,
                                                     "What are you doing?"));
    EXPECT_FALSE(PlayerbotSocialIsFunctionalTraffic(_botAI, ChatChannelSource::SRC_SAY, false, "maintenance"));
    EXPECT_FALSE(PlayerbotSocialIsFunctionalTraffic(_botAI, ChatChannelSource::SRC_WHISPER, true, "maintenance"));
}

TEST(PlayerbotSocialInboundRouteTest, WithTheGateOnTheBroadcastSurfacesStopProducingCannedReplies)
{
    /*
     * The inbound half of the suppression requirement. These surfaces produce neither social input
     * nor a canned reply once the feature is on: a bot answering World or Trade chat with a stock
     * line is exactly the nonfunctional chatter this feature replaces, and refusing it at the
     * broadcast funnel alone would leave the reply path still speaking there.
     */
    constexpr ChatChannelSource SUPPRESSED[] = {
        ChatChannelSource::SRC_WORLD, ChatChannelSource::SRC_TRADE, ChatChannelSource::SRC_LOOKING_FOR_GROUP,
        ChatChannelSource::SRC_LOCAL_DEFENSE, ChatChannelSource::SRC_WORLD_DEFENSE,
        ChatChannelSource::SRC_GUILD_RECRUITMENT, ChatChannelSource::SRC_YELL, ChatChannelSource::SRC_RAID,
        // Battleground and raid warning both resolve here, as does any surface added upstream.
        ChatChannelSource::SRC_UNDEFINED};

    for (ChatChannelSource source : SUPPRESSED)
    {
        PlayerbotSocialInboundDecision const decision =
            PlayerbotSocialRouteInbound(source, HeardNormally(), EnabledGate());
        EXPECT_EQ(decision.route, PlayerbotSocialInboundRoute::SuppressedSurface)
            << "source " << static_cast<int>(source);
        EXPECT_TRUE(decision.suppressLegacyReply);
    }
}

TEST(PlayerbotSocialInboundRouteTest, WithTheGateOnGuildAndEmoteSurfacesKeepTheirExistingBehavior)
{
    /*
     * Guild is a real conversation space this feature does not own yet, and the two emote surfaces are
     * not chat at all. Neither is in the suppression list, so enabling the feature must leave both
     * exactly as they are rather than quietly silencing them.
     */
    for (ChatChannelSource source :
         {ChatChannelSource::SRC_GUILD, ChatChannelSource::SRC_EMOTE, ChatChannelSource::SRC_TEXT_EMOTE})
    {
        PlayerbotSocialInboundDecision const decision =
            PlayerbotSocialRouteInbound(source, HeardNormally(), EnabledGate());
        EXPECT_EQ(decision.route, PlayerbotSocialInboundRoute::LegacyOnly) << "source " << static_cast<int>(source);
        EXPECT_FALSE(decision.suppressLegacyReply);
    }
}

TEST(PlayerbotSocialInboundRouteTest, InsideABattlegroundASupportedSurfaceIsSilencedRatherThanCaptured)
{
    /*
     * A battleground owes the player a fight, not conversation. The requirement names it alongside
     * World and yell: no social request and no canned line either. It arrives through the listener
     * rather than the surface, because say, party, and the zone channels are all still carried inside
     * a battleground and resolve to their ordinary sources there.
     */
    for (ChatChannelSource source : {ChatChannelSource::SRC_GENERAL, ChatChannelSource::SRC_SAY,
                                     ChatChannelSource::SRC_PARTY, ChatChannelSource::SRC_WHISPER})
    {
        PlayerbotSocialInboundDecision const decision =
            PlayerbotSocialRouteInbound(source, HeardInsideABattleground(), EnabledGate());

        EXPECT_EQ(decision.route, PlayerbotSocialInboundRoute::SuppressedSurface)
            << "source " << static_cast<int>(source);
        EXPECT_TRUE(decision.suppressLegacyReply) << "source " << static_cast<int>(source);
    }
}

TEST(PlayerbotSocialInboundRouteTest, NoSurfaceAtAllReachesTheSocialCoreFromInsideABattleground)
{
    // The sweep, so the claim covers every surface rather than the four that motivated it, and both
    // for fresh chat and for a line the feature itself delivered.
    for (ChatChannelSource source : AllChatChannelSources())
    {
        for (bool delivered : {false, true})
        {
            PlayerbotSocialInboundContext context = HeardInsideABattleground();
            context.originatedFromSocialDelivery = delivered;

            PlayerbotSocialInboundDecision const decision = PlayerbotSocialRouteInbound(source, context, EnabledGate());

            EXPECT_NE(decision.route, PlayerbotSocialInboundRoute::SocialOpportunity)
                << "source " << static_cast<int>(source);
            EXPECT_NE(decision.route, PlayerbotSocialInboundRoute::ThreadContinuationOnly)
                << "source " << static_cast<int>(source);
        }
    }
}

TEST(PlayerbotSocialInboundRouteTest, ABattlegroundDoesNotSilenceTheSurfacesTheFeatureDoesNotOwn)
{
    // Guild and the emote surfaces are outside the feature everywhere, so a battleground must not
    // become a reason to change behavior this feature was never given.
    for (ChatChannelSource source :
         {ChatChannelSource::SRC_GUILD, ChatChannelSource::SRC_EMOTE, ChatChannelSource::SRC_TEXT_EMOTE})
    {
        PlayerbotSocialInboundDecision const decision =
            PlayerbotSocialRouteInbound(source, HeardInsideABattleground(), EnabledGate());

        EXPECT_EQ(decision.route, PlayerbotSocialInboundRoute::LegacyOnly) << "source " << static_cast<int>(source);
        EXPECT_FALSE(decision.suppressLegacyReply) << "source " << static_cast<int>(source);
    }
}

TEST(PlayerbotSocialInboundRouteTest, WithTheGateOffABattlegroundChangesNothing)
{
    // The gate is authoritative: with the feature off, the battleground fact must not start altering
    // a path this feature is not participating in.
    for (ChatChannelSource source : AllChatChannelSources())
    {
        PlayerbotSocialInboundDecision const decision =
            PlayerbotSocialRouteInbound(source, HeardInsideABattleground(), DisabledGate());

        EXPECT_EQ(decision.route, PlayerbotSocialInboundRoute::LegacyOnly) << "source " << static_cast<int>(source);
        EXPECT_FALSE(decision.suppressLegacyReply) << "source " << static_cast<int>(source);
    }
}

TEST(PlayerbotSocialInboundRouteTest, NoSurfaceOutsideTheSupportedFourEverReachesTheSocialCore)
{
    // Whatever a surface's route is, only the four supported channels may become social input.
    std::set<ChatChannelSource> const supported = {ChatChannelSource::SRC_GENERAL, ChatChannelSource::SRC_SAY,
                                                   ChatChannelSource::SRC_PARTY, ChatChannelSource::SRC_WHISPER};

    for (ChatChannelSource source : AllChatChannelSources())
    {
        if (supported.count(source))
            continue;

        for (bool delivered : {false, true})
        {
            PlayerbotSocialInboundDecision const decision =
                PlayerbotSocialRouteInbound(source, delivered ? DeliveredBySocial() : HeardNormally(), EnabledGate());

            EXPECT_NE(decision.route, PlayerbotSocialInboundRoute::SocialOpportunity)
                << "source " << static_cast<int>(source);
            EXPECT_NE(decision.route, PlayerbotSocialInboundRoute::ThreadContinuationOnly)
                << "source " << static_cast<int>(source);
        }
    }
}

TEST(PlayerbotSocialInboundRouteTest, ADeliveredBotLineCanEarnOneDecayingReplyOpportunity)
{
    /*
     * Bot-only conversation is part of the product contract. The generated line is observed once
     * by the enclosing chat dispatch, then ordinary reply pressure and the consecutive bot-turn
     * decay decide whether another bot answers. Refusing the opportunity here makes every ambient
     * starter a one-line monologue, because no other production path activates a response to it.
     */
    for (ChatChannelSource source : {ChatChannelSource::SRC_GENERAL, ChatChannelSource::SRC_SAY,
                                     ChatChannelSource::SRC_PARTY, ChatChannelSource::SRC_WHISPER})
    {
        PlayerbotSocialInboundDecision const decision =
            PlayerbotSocialRouteInbound(source, DeliveredBySocial(), EnabledGate());

        EXPECT_EQ(decision.route, PlayerbotSocialInboundRoute::SocialOpportunity)
            << "source " << static_cast<int>(source);
        EXPECT_TRUE(decision.suppressLegacyReply);
    }
}

TEST(PlayerbotSocialInboundRouteTest, FunctionalBotSpeechCannotBecomeAProviderConversation)
{
    EXPECT_TRUE(PlayerbotSocialSpeakerCanOpenOpportunity(true, false));
    EXPECT_TRUE(PlayerbotSocialSpeakerCanOpenOpportunity(false, true));
    EXPECT_FALSE(PlayerbotSocialSpeakerCanOpenOpportunity(false, false));
}

TEST(PlayerbotSocialInboundRouteTest, ADeliveredSocialMessageOnAnUnsupportedSurfaceIsNotTreatedAsSocialAtAll)
{
    EXPECT_EQ(PlayerbotSocialRouteInbound(ChatChannelSource::SRC_GUILD, DeliveredBySocial(), EnabledGate()).route,
              PlayerbotSocialInboundRoute::LegacyOnly);
    EXPECT_EQ(PlayerbotSocialRouteInbound(ChatChannelSource::SRC_WORLD, DeliveredBySocial(), EnabledGate()).route,
              PlayerbotSocialInboundRoute::SuppressedSurface);
}

TEST(PlayerbotSocialInboundRouteTest, EachInboundRouteReportsItsOwnName)
{
    struct Case
    {
        PlayerbotSocialInboundRoute route;
        char const* expected;
    };

    constexpr Case CASES[] = {{PlayerbotSocialInboundRoute::LegacyOnly, "legacy_only"},
                              {PlayerbotSocialInboundRoute::SocialOpportunity, "social_opportunity"},
                              {PlayerbotSocialInboundRoute::ThreadContinuationOnly, "thread_continuation_only"},
                              {PlayerbotSocialInboundRoute::SuppressedSurface, "suppressed_surface"}};

    static_assert(std::size(CASES) == PLAYERBOT_SOCIAL_INBOUND_ROUTE_COUNT, "every inbound route needs a name case");

    for (Case const& testCase : CASES)
        EXPECT_STREQ(PlayerbotSocialInboundRouteName(testCase.route), testCase.expected);

    EXPECT_STREQ(PlayerbotSocialInboundRouteName(static_cast<PlayerbotSocialInboundRoute>(7)), "unknown");
}

// Capture normalization ----------------------------------------------------------------------------

namespace
{
PlayerbotSocialCapturedMessage GeneralCapture()
{
    PlayerbotSocialCapturedMessage captured;
    captured.channel = PlayerbotSocialChannel::General;
    captured.speakerGuidCounter = 4001;
    captured.speakerIsHuman = true;
    captured.zoneId = 12;
    captured.languageId = 7;
    captured.atUnixSeconds = 1000;
    return captured;
}

PlayerbotSocialCapturedMessage SayCapture()
{
    PlayerbotSocialCapturedMessage captured = GeneralCapture();
    captured.channel = PlayerbotSocialChannel::Say;
    captured.sayCohortScopeId = 0xC012;
    return captured;
}

PlayerbotSocialCapturedMessage PartyCapture()
{
    PlayerbotSocialCapturedMessage captured;
    captured.channel = PlayerbotSocialChannel::Party;
    captured.speakerGuidCounter = 4002;
    captured.groupId = 88;
    captured.zoneId = 12;
    captured.languageId = 0;
    captured.atUnixSeconds = 1000;
    return captured;
}

PlayerbotSocialCapturedMessage WhisperCapture()
{
    PlayerbotSocialCapturedMessage captured;
    captured.channel = PlayerbotSocialChannel::Whisper;
    captured.speakerGuidCounter = 4003;
    captured.targetGuidCounter = 4004;
    captured.zoneId = 12;
    captured.atUnixSeconds = 1000;
    return captured;
}
}  // namespace

TEST(PlayerbotSocialCaptureTest, AWellFormedMessageOnEachSupportedChannelIsAccepted)
{
    EXPECT_EQ(PlayerbotSocialValidateCapture(GeneralCapture()), PlayerbotSocialCaptureRejection::None);
    EXPECT_EQ(PlayerbotSocialValidateCapture(SayCapture()), PlayerbotSocialCaptureRejection::None);
    EXPECT_EQ(PlayerbotSocialValidateCapture(PartyCapture()), PlayerbotSocialCaptureRejection::None);
    EXPECT_EQ(PlayerbotSocialValidateCapture(WhisperCapture()), PlayerbotSocialCaptureRejection::None);
}

TEST(PlayerbotSocialCaptureTest, ACorruptChannelIsRefusedBeforeAnyShapeRuleRuns)
{
    PlayerbotSocialCapturedMessage captured = GeneralCapture();
    captured.channel = static_cast<PlayerbotSocialChannel>(9);
    captured.speakerGuidCounter = 0;
    captured.zoneId = 0;

    // Several rules are broken at once; the channel is the one reported, because nothing else can be
    // judged without knowing which shape applies.
    EXPECT_EQ(PlayerbotSocialValidateCapture(captured), PlayerbotSocialCaptureRejection::UnsupportedChannel);
}

TEST(PlayerbotSocialCaptureTest, AMessageWithNoSpeakerIsRefused)
{
    PlayerbotSocialCapturedMessage captured = GeneralCapture();
    captured.speakerGuidCounter = 0;

    EXPECT_EQ(PlayerbotSocialValidateCapture(captured), PlayerbotSocialCaptureRejection::MissingSpeaker);
}

TEST(PlayerbotSocialCaptureTest, AMessageTheBotCouldNotHearIsRefusedOnEverySupportedChannel)
{
    for (PlayerbotSocialCapturedMessage captured : {GeneralCapture(), SayCapture(), PartyCapture(), WhisperCapture()})
    {
        captured.withinHearingRange = false;
        EXPECT_EQ(PlayerbotSocialValidateCapture(captured), PlayerbotSocialCaptureRejection::OutOfHearingRange)
            << "channel " << static_cast<int>(captured.channel);
    }
}

TEST(PlayerbotSocialCaptureTest, AZoneScopedMessageWithoutAZoneIsRefused)
{
    PlayerbotSocialCapturedMessage general = GeneralCapture();
    general.zoneId = 0;
    EXPECT_EQ(PlayerbotSocialValidateCapture(general), PlayerbotSocialCaptureRejection::MissingZone);

    PlayerbotSocialCapturedMessage say = SayCapture();
    say.zoneId = 0;
    EXPECT_EQ(PlayerbotSocialValidateCapture(say), PlayerbotSocialCaptureRejection::MissingZone);
}

TEST(PlayerbotSocialCaptureTest, ASayMessageWithoutAHearingCohortIsRefused)
{
    PlayerbotSocialCapturedMessage say = SayCapture();
    say.sayCohortScopeId = 0;

    EXPECT_EQ(PlayerbotSocialValidateCapture(say), PlayerbotSocialCaptureRejection::MissingSayCohort);
}

TEST(PlayerbotSocialCaptureTest, APartyMessageWithoutAGroupIsRefused)
{
    PlayerbotSocialCapturedMessage captured = PartyCapture();
    captured.groupId = 0;

    EXPECT_EQ(PlayerbotSocialValidateCapture(captured), PlayerbotSocialCaptureRejection::MissingGroup);
}

TEST(PlayerbotSocialCaptureTest, AWhisperWithoutARecipientIsRefused)
{
    PlayerbotSocialCapturedMessage captured = WhisperCapture();
    captured.targetGuidCounter = 0;

    EXPECT_EQ(PlayerbotSocialValidateCapture(captured), PlayerbotSocialCaptureRejection::MissingTarget);
}

TEST(PlayerbotSocialCaptureTest, AWhisperAddressedToItsOwnSpeakerIsRefused)
{
    PlayerbotSocialCapturedMessage captured = WhisperCapture();
    captured.targetGuidCounter = captured.speakerGuidCounter;

    EXPECT_EQ(PlayerbotSocialValidateCapture(captured), PlayerbotSocialCaptureRejection::SpeakerIsTarget);
}

TEST(PlayerbotSocialCaptureTest, ARecipientOnANonWhisperChannelIsRefused)
{
    // A single addressee on a public or party surface means the normalization at the call site mixed
    // two shapes up. Admitting it would file a broadcast under a private conversation.
    for (PlayerbotSocialCapturedMessage captured : {GeneralCapture(), SayCapture(), PartyCapture()})
    {
        captured.targetGuidCounter = 5555;
        EXPECT_EQ(PlayerbotSocialValidateCapture(captured), PlayerbotSocialCaptureRejection::UnexpectedTarget)
            << "channel " << static_cast<int>(captured.channel);
    }
}

TEST(PlayerbotSocialCaptureTest, ZoneAndGroupContextAreCarriedWhereTheyAreNotTheScope)
{
    // A party message still knows which zone it happened in, and a zone message still knows the
    // speaker's group. Neither is the scope key, and neither is a reason to reject the message.
    PlayerbotSocialCapturedMessage party = PartyCapture();
    party.zoneId = 41;
    EXPECT_EQ(PlayerbotSocialValidateCapture(party), PlayerbotSocialCaptureRejection::None);

    PlayerbotSocialCapturedMessage general = GeneralCapture();
    general.groupId = 77;
    EXPECT_EQ(PlayerbotSocialValidateCapture(general), PlayerbotSocialCaptureRejection::None);
}

TEST(PlayerbotSocialCaptureTest, EachRejectionReportsItsOwnName)
{
    struct Case
    {
        PlayerbotSocialCaptureRejection rejection;
        char const* expected;
    };

    constexpr Case CASES[] = {{PlayerbotSocialCaptureRejection::None, "none"},
                              {PlayerbotSocialCaptureRejection::UnsupportedChannel, "unsupported_channel"},
                              {PlayerbotSocialCaptureRejection::MissingSpeaker, "missing_speaker"},
                              {PlayerbotSocialCaptureRejection::MissingZone, "missing_zone"},
                              {PlayerbotSocialCaptureRejection::MissingGroup, "missing_group"},
                              {PlayerbotSocialCaptureRejection::MissingTarget, "missing_target"},
                              {PlayerbotSocialCaptureRejection::UnexpectedTarget, "unexpected_target"},
                              {PlayerbotSocialCaptureRejection::SpeakerIsTarget, "speaker_is_target"},
                              {PlayerbotSocialCaptureRejection::OutOfHearingRange, "out_of_hearing_range"},
                              {PlayerbotSocialCaptureRejection::MissingSayCohort, "missing_say_cohort"},
                              {PlayerbotSocialCaptureRejection::IdentifierOutOfRange, "identifier_out_of_range"}};

    static_assert(std::size(CASES) == PLAYERBOT_SOCIAL_CAPTURE_REJECTION_COUNT,
                  "every capture rejection needs a name case");

    for (Case const& testCase : CASES)
        EXPECT_STREQ(PlayerbotSocialCaptureRejectionName(testCase.rejection), testCase.expected);

    EXPECT_STREQ(PlayerbotSocialCaptureRejectionName(static_cast<PlayerbotSocialCaptureRejection>(40)), "unknown");
}

// Scope derivation ---------------------------------------------------------------------------------

TEST(PlayerbotSocialScopeTest, ZoneScopedChannelsAreKeyedByZoneAndPartyByGroup)
{
    EXPECT_EQ(PlayerbotSocialScopeIdFor(GeneralCapture()), 12u);
    EXPECT_EQ(PlayerbotSocialScopeIdFor(SayCapture()), 0xC012u);
    EXPECT_EQ(PlayerbotSocialScopeIdFor(PartyCapture()), 88u);
}

TEST(PlayerbotSocialScopeTest, SimultaneousSayCohortsInOneZoneNeverShareAThread)
{
    PlayerbotSocialCapturedMessage first = SayCapture();
    PlayerbotSocialCapturedMessage second = SayCapture();
    first.text = "first cohort only";
    second.text = "second cohort only";
    first.speakerIsHuman = false;
    second.speakerIsHuman = false;
    first.speakerName = "First";
    second.speakerName = "Second";
    second.sayCohortScopeId = first.sayCohortScopeId + 1;

    PlayerbotSocialMgr coordinator;
    PlayerbotSocialThreadHandle const firstThread = coordinator.Observe(PlayerbotSocialObservationFor(first));
    PlayerbotSocialThreadHandle const secondThread = coordinator.Observe(PlayerbotSocialObservationFor(second));

    ASSERT_TRUE(firstThread.valid);
    ASSERT_TRUE(secondThread.valid);
    EXPECT_NE(firstThread.publicId, secondThread.publicId);

    PlayerbotSocialRequestContext const firstContext =
        coordinator.ComposeRequestContext(500, StoredPersonality(), first.speakerGuidCounter,
                                          PlayerbotSocialChannel::Say, "", 1000, firstThread.publicId);
    PlayerbotSocialRequestContext const secondContext =
        coordinator.ComposeRequestContext(500, StoredPersonality(), second.speakerGuidCounter,
                                          PlayerbotSocialChannel::Say, "", 1000, secondThread.publicId);
    EXPECT_EQ(firstContext.thread.size(), 1u);
    EXPECT_EQ(secondContext.thread.size(), 1u);
    EXPECT_EQ(firstContext.thread.front(), "First: first cohort only");
    EXPECT_EQ(secondContext.thread.front(), "Second: second cohort only");
}

TEST(PlayerbotSocialScopeTest, HearingCohortsAreCanonicalAndExpireWithConversationState)
{
    PlayerbotSocialSayCohortRegistry registry;

    uint64 const first = registry.Resolve({30, 10, 20, 20}, 1000);
    EXPECT_NE(first, 0u);
    EXPECT_EQ(registry.Resolve({20, 30, 10}, 1001), first)
        << "ordering and duplicate listeners do not change the exact cohort";

    uint64 const changed = registry.Resolve({10, 20}, 1002);
    EXPECT_NE(changed, first) << "losing one listener opens a new Say scope";

    registry.Prune(1000 + PLAYERBOT_SOCIAL_THREAD_STALE_SECONDS + 2);
    EXPECT_NE(registry.Resolve({10, 20, 30}, 1000 + PLAYERBOT_SOCIAL_THREAD_STALE_SECONDS + 3), first)
        << "a cohort identity cannot outlive the thread retention window";
}

TEST(PlayerbotSocialScopeTest, BothDirectionsOfOneWhisperShareOneScope)
{
    PlayerbotSocialCapturedMessage outgoing = WhisperCapture();

    PlayerbotSocialCapturedMessage incoming = WhisperCapture();
    incoming.speakerGuidCounter = outgoing.targetGuidCounter;
    incoming.targetGuidCounter = outgoing.speakerGuidCounter;

    EXPECT_EQ(PlayerbotSocialScopeIdFor(outgoing), PlayerbotSocialScopeIdFor(incoming));
    EXPECT_EQ(PlayerbotSocialWhisperScopeId(11, 22), PlayerbotSocialWhisperScopeId(22, 11));
}

TEST(PlayerbotSocialScopeTest, UnrelatedWhisperPairsDoNotShareAScope)
{
    /*
     * The reason the scope is 64 bits. Two 32 bit character counters do not fit in 32, and a pair
     * folded down into 32 bits would eventually put a stranger's private conversation into someone
     * else's thread.
     */
    std::set<uint64> scopes;
    for (uint64 first = 1; first <= 40; ++first)
        for (uint64 second = first + 1; second <= 40; ++second)
            scopes.insert(PlayerbotSocialWhisperScopeId(first, second));

    EXPECT_EQ(scopes.size(), static_cast<std::size_t>(40 * 39 / 2));
}

TEST(PlayerbotSocialScopeTest, AWhisperScopeCarriesBothIdentitiesRatherThanFoldingThemTogether)
{
    /*
     * What makes the claim above structural instead of probabilistic. The scope holds the ordered
     * pair exactly, so no two pairs can land on one scope at all. A value of the same width derived
     * by hashing would satisfy the sweep above and still leave a real, if small, chance of one
     * private conversation absorbing a stranger's, which is a privacy failure rather than a lost
     * message.
     */
    uint64 const scope = PlayerbotSocialWhisperScopeId(4242u, 77u);

    EXPECT_EQ(scope >> 32, static_cast<uint64>(77u));
    EXPECT_EQ(scope & PLAYERBOT_SOCIAL_MAX_CHARACTER_COUNTER, static_cast<uint64>(4242u));
    EXPECT_EQ(PlayerbotSocialWhisperScopeId(77u, 4242u), scope);
}

TEST(PlayerbotSocialScopeTest, TheWidestPairARealmCanIssueStillFitsOneScope)
{
    /*
     * The top of the identifier range packs as losslessly as the bottom, which is where a fold would
     * have failed first. The smaller of the two always takes the high half, so a scope stays the same
     * whichever way the whisper went.
     */
    uint64 const widest = PlayerbotSocialWhisperScopeId(PLAYERBOT_SOCIAL_MAX_CHARACTER_COUNTER,
                                                        PLAYERBOT_SOCIAL_MAX_CHARACTER_COUNTER - 1);

    EXPECT_EQ(widest >> 32, PLAYERBOT_SOCIAL_MAX_CHARACTER_COUNTER - 1);
    EXPECT_EQ(widest & PLAYERBOT_SOCIAL_MAX_CHARACTER_COUNTER, PLAYERBOT_SOCIAL_MAX_CHARACTER_COUNTER);
    EXPECT_NE(widest, PlayerbotSocialWhisperScopeId(PLAYERBOT_SOCIAL_MAX_CHARACTER_COUNTER,
                                                    PLAYERBOT_SOCIAL_MAX_CHARACTER_COUNTER - 2));
}

TEST(PlayerbotSocialCaptureTest, AWhisperCarryingAnImpossiblyWideIdentifierIsRefused)
{
    // The guard that keeps the packing lossless. A counter wider than the core can issue would
    // overflow the half of the scope it belongs to and quietly alias onto another pair.
    PlayerbotSocialCapturedMessage overflowingSpeaker = WhisperCapture();
    overflowingSpeaker.speakerGuidCounter = PLAYERBOT_SOCIAL_MAX_CHARACTER_COUNTER + 1;
    EXPECT_EQ(PlayerbotSocialValidateCapture(overflowingSpeaker),
              PlayerbotSocialCaptureRejection::IdentifierOutOfRange);

    PlayerbotSocialCapturedMessage overflowingTarget = WhisperCapture();
    overflowingTarget.targetGuidCounter = PLAYERBOT_SOCIAL_MAX_CHARACTER_COUNTER + 1;
    EXPECT_EQ(PlayerbotSocialValidateCapture(overflowingTarget), PlayerbotSocialCaptureRejection::IdentifierOutOfRange);

    // And it is a refusal, so nothing the coordinator can act on comes out of it.
    PlayerbotSocialMgr coordinator;
    EXPECT_FALSE(coordinator.Observe(PlayerbotSocialObservationFor(overflowingSpeaker)).valid);
}

TEST(PlayerbotSocialCaptureTest, TheWidestIdentifierARealmCanIssueIsStillAccepted)
{
    PlayerbotSocialCapturedMessage widest = WhisperCapture();
    widest.speakerGuidCounter = PLAYERBOT_SOCIAL_MAX_CHARACTER_COUNTER;
    widest.targetGuidCounter = PLAYERBOT_SOCIAL_MAX_CHARACTER_COUNTER - 1;

    EXPECT_EQ(PlayerbotSocialValidateCapture(widest), PlayerbotSocialCaptureRejection::None);
}

TEST(PlayerbotSocialScopeTest, AWhisperScopeIsNotDerivedFromOneEndpointAlone)
{
    // Keying on the bot alone would merge every partner who whispered it inside the continuation
    // window into one thread, which is precisely the cross partner bleed the pair key prevents.
    EXPECT_NE(PlayerbotSocialWhisperScopeId(11, 22), PlayerbotSocialWhisperScopeId(11, 23));
    EXPECT_NE(PlayerbotSocialWhisperScopeId(11, 22), PlayerbotSocialWhisperScopeId(12, 22));
}

TEST(PlayerbotSocialScopeTest, TheSamePairAlwaysProducesTheSameScope)
{
    EXPECT_EQ(PlayerbotSocialWhisperScopeId(90001, 90002), PlayerbotSocialWhisperScopeId(90001, 90002));
}

// Observation derivation ---------------------------------------------------------------------------

TEST(PlayerbotSocialObservationTest, AValidCaptureBecomesTheObservationTheCoordinatorExpects)
{
    PlayerbotSocialCapturedMessage captured = GeneralCapture();
    captured.eventPublicId = PlayerbotSocialMakeEventPublicId(61, captured.speakerGuidCounter);
    captured.speakerIsHuman = false;
    captured.atUnixSeconds = 4242;

    PlayerbotSocialObservation const observation = PlayerbotSocialObservationFor(captured);

    EXPECT_EQ(observation.key.channel, PlayerbotSocialChannel::General);
    EXPECT_EQ(observation.key.scopeId, 12u);
    EXPECT_EQ(observation.eventPublicId, captured.eventPublicId);
    EXPECT_EQ(observation.speakerGuidCounter, captured.speakerGuidCounter);
    EXPECT_FALSE(observation.speakerIsHuman);
    EXPECT_EQ(observation.atUnixSeconds, 4242u);
}

TEST(PlayerbotSocialObservationTest, EveryGeneratedSurfaceKeepsItsEventIdentityInTheObservedThread)
{
    std::vector<PlayerbotSocialCapturedMessage> captures = {GeneralCapture(), SayCapture(), PartyCapture(),
                                                            WhisperCapture()};

    for (std::size_t index = 0; index < captures.size(); ++index)
    {
        PlayerbotSocialCapturedMessage& captured = captures[index];
        captured.eventPublicId = PlayerbotSocialMakeEventPublicId(62 + index, captured.speakerGuidCounter);
        captured.speakerIsHuman = false;
        captured.atUnixSeconds = 4242 + index;

        PlayerbotSocialMgr coordinator;
        PlayerbotSocialThreadHandle const thread = coordinator.Observe(PlayerbotSocialObservationFor(captured));

        ASSERT_TRUE(thread.valid) << "channel " << static_cast<uint32>(captured.channel);
        EXPECT_EQ(coordinator.RecentEventIdsOf(thread), (std::vector<std::string>{captured.eventPublicId}))
            << "channel " << static_cast<uint32>(captured.channel);
    }
}

TEST(PlayerbotSocialObservationTest, AnInvalidCaptureYieldsAnObservationTheCoordinatorRefuses)
{
    PlayerbotSocialCapturedMessage captured = GeneralCapture();
    captured.zoneId = 0;

    PlayerbotSocialObservation const observation = PlayerbotSocialObservationFor(captured);

    // Not merely empty. An empty observation still names a supported channel and would open a thread
    // in scope zero; this one has to be refused outright.
    EXPECT_FALSE(PlayerbotSocialChannelIsValid(observation.key.channel));

    PlayerbotSocialMgr coordinator;
    EXPECT_FALSE(coordinator.Observe(observation).valid);
    EXPECT_EQ(coordinator.TrackedScopeCount(), 0u);
}

TEST(PlayerbotSocialObservationTest, TwoWhisperPairsInOneZoneStayInSeparateThreads)
{
    PlayerbotSocialMgr coordinator;

    PlayerbotSocialCapturedMessage first = WhisperCapture();
    first.speakerGuidCounter = 700;
    first.targetGuidCounter = 701;

    PlayerbotSocialCapturedMessage second = WhisperCapture();
    second.speakerGuidCounter = 800;
    second.targetGuidCounter = 801;

    PlayerbotSocialThreadHandle const firstThread = coordinator.Observe(PlayerbotSocialObservationFor(first));
    PlayerbotSocialThreadHandle const secondThread = coordinator.Observe(PlayerbotSocialObservationFor(second));

    ASSERT_TRUE(firstThread.valid);
    ASSERT_TRUE(secondThread.valid);
    EXPECT_NE(firstThread.threadId, secondThread.threadId);
    EXPECT_EQ(coordinator.TrackedScopeCount(), 2u);
}

// Outbound broadcast routing -----------------------------------------------------------------------

TEST(PlayerbotSocialBroadcastRouteTest, WithTheGateOffEveryDestinationIsDeliveredAsItIsToday)
{
    for (BroadcastHelper::ToChannel destination : AllBroadcastDestinations())
        EXPECT_EQ(PlayerbotSocialRouteBroadcast(destination, DisabledGate()),
                  PlayerbotSocialBroadcastRoute::DeliverAsToday)
            << "destination " << static_cast<int>(destination);
}

TEST(PlayerbotSocialBroadcastRouteTest, SocialStarterSourcesDoNotDependOnTheLegacyBroadcastSwitch)
{
    EXPECT_FALSE(PlayerbotSocialAmbientSourceEnabled(false, false));
    EXPECT_TRUE(PlayerbotSocialAmbientSourceEnabled(true, false));
    EXPECT_TRUE(PlayerbotSocialAmbientSourceEnabled(false, true));
    EXPECT_TRUE(PlayerbotSocialAmbientSourceEnabled(true, true));
}

TEST(PlayerbotSocialBroadcastRouteTest, SocialAuthoritativeSourcesBypassLegacyBroadcastChance)
{
    EXPECT_TRUE(PlayerbotSocialAmbientSourcePassesChance(true, 0, 30000));

    EXPECT_TRUE(PlayerbotSocialAmbientSourcePassesChance(false, 1, 1));
    EXPECT_TRUE(PlayerbotSocialAmbientSourcePassesChance(false, 150, 150));
    EXPECT_FALSE(PlayerbotSocialAmbientSourcePassesChance(false, 0, 1));
    EXPECT_FALSE(PlayerbotSocialAmbientSourcePassesChance(false, 149, 150));
}

TEST(PlayerbotSocialBroadcastRouteTest, WithTheGateOnOnlyGeneralConvertsAndEveryOtherDestinationIsSuppressed)
{
    for (BroadcastHelper::ToChannel destination : AllBroadcastDestinations())
    {
        PlayerbotSocialBroadcastRoute const expected = destination == BroadcastHelper::TO_GENERAL
                                                           ? PlayerbotSocialBroadcastRoute::StarterContext
                                                           : PlayerbotSocialBroadcastRoute::SuppressCannedDelivery;

        EXPECT_EQ(PlayerbotSocialRouteBroadcast(destination, EnabledGate()), expected)
            << "destination " << static_cast<int>(destination);
    }
}

TEST(PlayerbotSocialBroadcastRouteTest, NoDestinationEverProducesBothADeliveryAndAConversion)
{
    /*
     * The suppression requirement stated as a property rather than as a list. World, Trade, Looking
     * For Group, both defense channels, guild recruitment, and the ambient guild line must produce
     * neither a canned delivery nor social input once the feature is on.
     */
    for (BroadcastHelper::ToChannel destination : AllBroadcastDestinations())
    {
        if (destination == BroadcastHelper::TO_GENERAL)
            continue;

        PlayerbotSocialBroadcastRoute const route = PlayerbotSocialRouteBroadcast(destination, EnabledGate());
        EXPECT_NE(route, PlayerbotSocialBroadcastRoute::DeliverAsToday);
        EXPECT_NE(route, PlayerbotSocialBroadcastRoute::StarterContext);
    }
}

TEST(PlayerbotSocialBroadcastRouteTest, AnUnrecognizedDestinationIsSuppressedRatherThanDelivered)
{
    // The funnel has no -Wswitch backstop either. A destination added upstream must not fall through
    // into the delivering arm.
    EXPECT_EQ(PlayerbotSocialRouteBroadcast(static_cast<BroadcastHelper::ToChannel>(99), EnabledGate()),
              PlayerbotSocialBroadcastRoute::SuppressCannedDelivery);
    EXPECT_EQ(PlayerbotSocialRouteBroadcast(static_cast<BroadcastHelper::ToChannel>(0), EnabledGate()),
              PlayerbotSocialBroadcastRoute::SuppressCannedDelivery);
}

TEST(PlayerbotSocialBroadcastRouteTest, EachBroadcastRouteReportsItsOwnName)
{
    struct Case
    {
        PlayerbotSocialBroadcastRoute route;
        char const* expected;
    };

    constexpr Case CASES[] = {{PlayerbotSocialBroadcastRoute::DeliverAsToday, "deliver_as_today"},
                              {PlayerbotSocialBroadcastRoute::SuppressCannedDelivery, "suppress_canned_delivery"},
                              {PlayerbotSocialBroadcastRoute::StarterContext, "starter_context"}};

    static_assert(std::size(CASES) == PLAYERBOT_SOCIAL_BROADCAST_ROUTE_COUNT,
                  "every broadcast route needs a name case");

    for (Case const& testCase : CASES)
        EXPECT_STREQ(PlayerbotSocialBroadcastRouteName(testCase.route), testCase.expected);

    EXPECT_STREQ(PlayerbotSocialBroadcastRouteName(static_cast<PlayerbotSocialBroadcastRoute>(12)), "unknown");
}

TEST(PlayerbotSocialGateTest, TheGateDefaultsToOffSoAnUntouchedConfigurationBehavesAsBefore)
{
    PlayerbotSocialGate const gate;

    EXPECT_FALSE(gate.enabled);
    EXPECT_EQ(gate.density, PlayerbotSocialDensityProfile::Normal);
    EXPECT_EQ(gate.telemetryRetentionHours, PLAYERBOT_SOCIAL_MIN_RETENTION_HOURS);
    EXPECT_EQ(PLAYERBOT_SOCIAL_MIN_RETENTION_HOURS, 48u);
}

// Starter context ----------------------------------------------------------------------------------

namespace
{
PlayerbotSocialStarterSource StarterSource(PlayerbotSocialStarterSourceKind kind,
                                           std::string_view subject = "a rare drop")
{
    PlayerbotSocialStarterSource source;
    source.kind = kind;
    if (kind == PlayerbotSocialStarterSourceKind::QuestTransition)
        source.questTransition = PlayerbotSocialQuestTransition::Accepted;
    source.sourceEventPublicId = PlayerbotSocialMakeEventPublicId(11, 900);
    source.subjectId = 12;
    source.subject = subject;
    return source;
}

PlayerbotSocialStarterContext ZoneStarter(uint64 botGuidCounter, uint64 atUnixSeconds)
{
    PlayerbotSocialStarterContext starter;
    starter.key.channel = PlayerbotSocialChannel::General;
    starter.key.scopeId = 12;
    starter.botGuidCounter = botGuidCounter;
    starter.source = StarterSource(PlayerbotSocialStarterSourceKind::Loot, "looted something");
    starter.audienceGuidCounter = 901;
    starter.zoneId = 12;
    starter.atUnixSeconds = atUnixSeconds;
    return starter;
}
}  // namespace

TEST(PlayerbotSocialStarterSourceTest, OnlyBoundedTypedAuthoritativeSourcesAreAccepted)
{
    for (PlayerbotSocialStarterSourceKind const kind :
         {PlayerbotSocialStarterSourceKind::Loot, PlayerbotSocialStarterSourceKind::QuestTransition,
          PlayerbotSocialStarterSourceKind::Kill, PlayerbotSocialStarterSourceKind::Level})
        EXPECT_TRUE(PlayerbotSocialStarterSourceIsValid(StarterSource(kind))) << static_cast<int>(kind);

    PlayerbotSocialStarterSource empty = StarterSource(PlayerbotSocialStarterSourceKind::Loot, "");
    EXPECT_FALSE(PlayerbotSocialStarterSourceIsValid(empty));

    PlayerbotSocialStarterSource oversized = StarterSource(
        PlayerbotSocialStarterSourceKind::Loot, std::string(PLAYERBOT_SOCIAL_STARTER_SUBJECT_MAX_LENGTH + 1, 'x'));
    EXPECT_FALSE(PlayerbotSocialStarterSourceIsValid(oversized));

    PlayerbotSocialStarterSource wrongId = StarterSource(PlayerbotSocialStarterSourceKind::Kill);
    wrongId.sourceEventPublicId.replace(0, 3, "thr");
    EXPECT_FALSE(PlayerbotSocialStarterSourceIsValid(wrongId));

    PlayerbotSocialStarterSource unknown = StarterSource(static_cast<PlayerbotSocialStarterSourceKind>(99));
    EXPECT_FALSE(PlayerbotSocialStarterSourceIsValid(unknown));
}

TEST(PlayerbotSocialStarterSourceTest, EveryQuestTransitionKeepsItsExactGroundingSemantics)
{
    struct ExpectedTransition
    {
        PlayerbotSocialQuestTransition transition;
        char const* name;
    };

    for (ExpectedTransition const expected :
         {ExpectedTransition{PlayerbotSocialQuestTransition::Accepted, "accepted"},
          ExpectedTransition{PlayerbotSocialQuestTransition::ObjectiveProgress, "objective_progress"},
          ExpectedTransition{PlayerbotSocialQuestTransition::ObjectiveCompleted, "objective_completed"},
          ExpectedTransition{PlayerbotSocialQuestTransition::Failed, "failed"},
          ExpectedTransition{PlayerbotSocialQuestTransition::Completed, "completed"},
          ExpectedTransition{PlayerbotSocialQuestTransition::TurnedIn, "turned_in"}})
    {
        PlayerbotSocialStarterSource source =
            StarterSource(PlayerbotSocialStarterSourceKind::QuestTransition, "Selling Fish");
        source.questTransition = expected.transition;

        EXPECT_STREQ(PlayerbotSocialQuestTransitionName(expected.transition), expected.name);
        EXPECT_EQ(PlayerbotSocialStarterGroundingSubject(source),
                  std::string("quest_transition.") + expected.name + ": Selling Fish");
    }

    EXPECT_EQ(
        PlayerbotSocialStarterGroundingSubject(StarterSource(PlayerbotSocialStarterSourceKind::Loot, "a rare drop")),
        "loot: a rare drop");
}

TEST(PlayerbotSocialStarterSourceTest, ACurrentRealAudienceSelectsOnlySupportedStarterChannels)
{
    PlayerbotSocialChannel channel = PlayerbotSocialChannel::Whisper;

    PlayerbotSocialStarterAudience party;
    party.hasRealPartyMember = true;
    EXPECT_TRUE(PlayerbotSocialSelectStarterChannel(party, channel));
    EXPECT_EQ(channel, PlayerbotSocialChannel::Party);

    PlayerbotSocialStarterAudience say;
    say.hasRealSayListener = true;
    EXPECT_TRUE(PlayerbotSocialSelectStarterChannel(say, channel));
    EXPECT_EQ(channel, PlayerbotSocialChannel::Say);

    PlayerbotSocialStarterAudience general;
    general.hasRealGeneralMember = true;
    EXPECT_TRUE(PlayerbotSocialSelectStarterChannel(general, channel));
    EXPECT_EQ(channel, PlayerbotSocialChannel::General);

    EXPECT_FALSE(PlayerbotSocialSelectStarterChannel(PlayerbotSocialStarterAudience(), channel));
}

TEST(PlayerbotSocialStarterSourceTest, OnlyPerceivableStarterAudienceMayEnterGrounding)
{
    EXPECT_TRUE(PlayerbotSocialStarterParticipantIsPerceivable(PlayerbotSocialChannel::Party));
    EXPECT_TRUE(PlayerbotSocialStarterParticipantIsPerceivable(PlayerbotSocialChannel::Say));
    EXPECT_FALSE(PlayerbotSocialStarterParticipantIsPerceivable(PlayerbotSocialChannel::General));
    EXPECT_FALSE(PlayerbotSocialStarterParticipantIsPerceivable(PlayerbotSocialChannel::Whisper));
    EXPECT_FALSE(PlayerbotSocialStarterParticipantIsPerceivable(static_cast<PlayerbotSocialChannel>(255)));
}

TEST(PlayerbotSocialStarterContextTest, AConvertedGeneralBroadcastIsHeldForTheZoneItHappenedIn)
{
    PlayerbotSocialMgr coordinator;
    PlayerbotSocialThreadKey key;
    key.channel = PlayerbotSocialChannel::General;
    key.scopeId = 12;

    EXPECT_TRUE(coordinator.NoteStarterContext(ZoneStarter(900, 1000)));

    std::vector<PlayerbotSocialStarterContext> const pending = coordinator.PendingStarterContextsFor(key);
    ASSERT_EQ(pending.size(), 1u);
    EXPECT_EQ(pending.front().botGuidCounter, 900u);
    EXPECT_EQ(pending.front().source.subject, "looted something");
    EXPECT_TRUE(
        PlayerbotSocialPublicIdIsValid(PlayerbotSocialIdKind::Event, pending.front().source.sourceEventPublicId));
    EXPECT_EQ(pending.front().audienceGuidCounter, 901u);
}

TEST(PlayerbotSocialStarterContextTest, StarterContextIsScopedToItsZoneAndDoesNotLeakIntoAnother)
{
    PlayerbotSocialMgr coordinator;
    coordinator.NoteStarterContext(ZoneStarter(900, 1000));

    PlayerbotSocialThreadKey elsewhere;
    elsewhere.channel = PlayerbotSocialChannel::General;
    elsewhere.scopeId = 13;

    EXPECT_TRUE(coordinator.PendingStarterContextsFor(elsewhere).empty());
}

TEST(PlayerbotSocialStarterContextTest, SayPartyAndGeneralCanCarryAStarterButWhisperCannot)
{
    PlayerbotSocialMgr coordinator;

    for (PlayerbotSocialChannel channel :
         {PlayerbotSocialChannel::General, PlayerbotSocialChannel::Say, PlayerbotSocialChannel::Party})
    {
        PlayerbotSocialStarterContext starter = ZoneStarter(900, 1000);
        starter.key.channel = channel;
        EXPECT_TRUE(coordinator.NoteStarterContext(starter)) << "channel " << static_cast<int>(channel);
    }

    PlayerbotSocialStarterContext whisper = ZoneStarter(900, 1000);
    whisper.key.channel = PlayerbotSocialChannel::Whisper;
    EXPECT_FALSE(coordinator.NoteStarterContext(whisper));

    PlayerbotSocialStarterContext corrupt = ZoneStarter(900, 1000);
    corrupt.key.channel = static_cast<PlayerbotSocialChannel>(0xFF);
    EXPECT_FALSE(coordinator.NoteStarterContext(corrupt));
}

TEST(PlayerbotSocialStarterContextTest, AStarterWithNothingToTalkAboutIsRefused)
{
    PlayerbotSocialMgr coordinator;

    PlayerbotSocialStarterContext emptySubject = ZoneStarter(900, 1000);
    emptySubject.source.subject.clear();
    EXPECT_FALSE(coordinator.NoteStarterContext(emptySubject));

    PlayerbotSocialStarterContext noBot = ZoneStarter(0, 1000);
    EXPECT_FALSE(coordinator.NoteStarterContext(noBot));

    PlayerbotSocialStarterContext noAudience = ZoneStarter(900, 1000);
    noAudience.audienceGuidCounter = 0;
    EXPECT_FALSE(coordinator.NoteStarterContext(noAudience));
}

TEST(PlayerbotSocialStarterContextTest, AZoneKeepsOnlyItsMostRecentStarters)
{
    PlayerbotSocialMgr coordinator;
    PlayerbotSocialThreadKey key;
    key.channel = PlayerbotSocialChannel::General;
    key.scopeId = 12;

    for (uint64 index = 0; index < PLAYERBOT_SOCIAL_MAX_STARTER_CONTEXTS_PER_SCOPE + 3; ++index)
        coordinator.NoteStarterContext(ZoneStarter(900 + index, 1000 + index));

    std::vector<PlayerbotSocialStarterContext> const pending = coordinator.PendingStarterContextsFor(key);
    ASSERT_EQ(pending.size(), PLAYERBOT_SOCIAL_MAX_STARTER_CONTEXTS_PER_SCOPE);

    // The oldest three were dropped, not the newest three.
    EXPECT_EQ(pending.back().botGuidCounter, 900u + PLAYERBOT_SOCIAL_MAX_STARTER_CONTEXTS_PER_SCOPE + 2);
    EXPECT_EQ(pending.front().botGuidCounter, 903u);
}

TEST(PlayerbotSocialStarterContextTest, StaleStartersArePrunedWithTheThreadsTheyBelongedTo)
{
    PlayerbotSocialMgr coordinator;
    PlayerbotSocialThreadKey key;
    key.channel = PlayerbotSocialChannel::General;
    key.scopeId = 12;

    coordinator.NoteStarterContext(ZoneStarter(900, 1000));
    ASSERT_EQ(coordinator.PendingStarterContextsFor(key).size(), 1u);

    coordinator.PruneStaleThreads(1000 + PLAYERBOT_SOCIAL_THREAD_STALE_SECONDS);
    EXPECT_EQ(coordinator.PendingStarterContextsFor(key).size(), 1u);

    coordinator.PruneStaleThreads(1000 + PLAYERBOT_SOCIAL_THREAD_STALE_SECONDS + 1);
    EXPECT_TRUE(coordinator.PendingStarterContextsFor(key).empty());
}

TEST(PlayerbotSocialStarterContextTest, PruningStartersDoesNotStrandTheScopeItEmptied)
{
    PlayerbotSocialMgr coordinator;

    coordinator.NoteStarterContext(ZoneStarter(900, 1000));
    ASSERT_EQ(coordinator.TrackedScopeCount(), 1u);

    coordinator.PruneStaleThreads(1000 + PLAYERBOT_SOCIAL_THREAD_STALE_SECONDS + 1);
    EXPECT_EQ(coordinator.TrackedScopeCount(), 0u);
}

// Recognising the feature's own delivered lines ----------------------------------------------------

TEST(PlayerbotSocialRouteTest, ALineTheFeatureJustSpokeIsRecognisedComingBack)
{
    /*
     * A bot's generated say or whisper enters PlayerScript before the send returns. The marker lets
     * that authoritative callback distinguish Social output, which may earn a decaying bot reply,
     * from functional bot speech, which must remain telemetry only.
     *
     * Matched on the speaking bot AND the exact text, because a bot speaking a line the feature did
     * not produce is a real opportunity and must not be swallowed.
     */
    PlayerbotSocialForgetDeliveredLines();

    PlayerbotSocialRememberDeliveredLine(500, "Aye, that pack hits hard.", 1000);

    EXPECT_TRUE(PlayerbotSocialWasDeliveredLine(500, "Aye, that pack hits hard.", 1000));

    // A different bot saying the same words is not the same delivery.
    EXPECT_FALSE(PlayerbotSocialWasDeliveredLine(501, "Aye, that pack hits hard.", 1000));

    // The same bot saying something else is a real opportunity.
    EXPECT_FALSE(PlayerbotSocialWasDeliveredLine(500, "Something else entirely.", 1000));
}

TEST(PlayerbotSocialRouteTest, AGeneratedReplyEchoRetainsItsExactParentIdentity)
{
    PlayerbotSocialForgetDeliveredLines();
    std::string const eventPublicId = PlayerbotSocialMakeEventPublicId(81, 500);
    std::string const parentEventPublicId = PlayerbotSocialMakeEventPublicId(80, 900);
    PlayerbotSocialRememberDeliveredLine(500, "Aye, that pack hits hard.", 1000, eventPublicId, parentEventPublicId);

    std::string observedEventPublicId;
    std::string observedParentPublicId;
    ASSERT_TRUE(PlayerbotSocialWasDeliveredLine(500, "Aye, that pack hits hard.", 1000, &observedEventPublicId,
                                                &observedParentPublicId));
    EXPECT_EQ(observedEventPublicId, eventPublicId);
    EXPECT_EQ(observedParentPublicId, parentEventPublicId);
}

TEST(PlayerbotSocialRouteTest, ARecognisedLineIsConsumedSoItCannotClassifyTwice)
{
    /*
     * One delivery marks one callback. Leaving the record in place would let a bot repeating itself
     * later be mistaken for another generated Social line.
     */
    PlayerbotSocialForgetDeliveredLines();
    PlayerbotSocialRememberDeliveredLine(500, "Careful, caster on the left.", 1000);

    EXPECT_TRUE(PlayerbotSocialWasDeliveredLine(500, "Careful, caster on the left.", 1000));
    EXPECT_FALSE(PlayerbotSocialWasDeliveredLine(500, "Careful, caster on the left.", 1000));
}

TEST(PlayerbotSocialRouteTest, ADeliveredLineIsForgottenOnceItsEchoWindowPasses)
{
    /*
     * The echo arrives in the same tick or the next. A record that outlived that window would
     * suppress a genuine repetition minutes later, so the window is short and expiry is checked
     * rather than assumed.
     */
    PlayerbotSocialForgetDeliveredLines();
    PlayerbotSocialRememberDeliveredLine(500, "On my way.", 1000);

    EXPECT_FALSE(
        PlayerbotSocialWasDeliveredLine(500, "On my way.", 1000 + PLAYERBOT_SOCIAL_DELIVERY_ECHO_WINDOW_SECONDS + 1));
}

TEST(PlayerbotSocialRouteTest, TwoLinesDeliveredBeforeEitherEchoesAreBothRecognised)
{
    /*
     * A bot may hold PLAYERBOT_SOCIAL_MAX_PENDING_PER_BOT requests, and one pass of the delivery
     * pump can send all of them before any echo returns. Keeping a single line per bot let the
     * second delivery overwrite the first, so the first callback lost its Social origin and the
     * generated conversation stopped early.
     */
    PlayerbotSocialForgetDeliveredLines();

    PlayerbotSocialRememberDeliveredLine(500, "Pulling now.", 1000);
    PlayerbotSocialRememberDeliveredLine(500, "Watch the adds.", 1000);

    // Recognised in either order: the echoes come back in whatever order the chat path emits them.
    EXPECT_TRUE(PlayerbotSocialWasDeliveredLine(500, "Watch the adds.", 1000));
    EXPECT_TRUE(PlayerbotSocialWasDeliveredLine(500, "Pulling now.", 1000));

    // Both consumed, so a later repetition of either is a real opportunity again.
    EXPECT_FALSE(PlayerbotSocialWasDeliveredLine(500, "Pulling now.", 1000));
}

TEST(PlayerbotSocialRouteTest, TheSameLineSpokenTwiceHasBothEchoesRecognised)
{
    // Consuming ONE record per match rather than the whole entry. A bot that repeated itself
    // deliberately produces two echoes, and swallowing only the first would leave the second to be
    // answered as though a human had said it.
    PlayerbotSocialForgetDeliveredLines();

    std::string const firstEvent = PlayerbotSocialMakeEventPublicId(91, 500);
    std::string const secondEvent = PlayerbotSocialMakeEventPublicId(92, 500);
    PlayerbotSocialRememberDeliveredLine(500, "Ready.", 1000, firstEvent);
    PlayerbotSocialRememberDeliveredLine(500, "Ready.", 1000, secondEvent);

    std::string consumedEvent;
    EXPECT_TRUE(PlayerbotSocialWasDeliveredLine(500, "Ready.", 1000, &consumedEvent));
    EXPECT_EQ(consumedEvent, firstEvent);
    EXPECT_TRUE(PlayerbotSocialWasDeliveredLine(500, "Ready.", 1000, &consumedEvent));
    EXPECT_EQ(consumedEvent, secondEvent);
    EXPECT_FALSE(PlayerbotSocialWasDeliveredLine(500, "Ready.", 1000));
}

TEST(PlayerbotSocialRouteTest, ARecordStampedInTheFutureIsDiscardedRatherThanTrusted)
{
    /*
     * A wall clock that steps backwards leaves a record stamped ahead of now. It can never be shown
     * to have expired, so trusting it would suppress a genuine repetition of that line for as long
     * as the clock stayed behind. Discarded instead, which costs one unsuppressed echo.
     */
    PlayerbotSocialForgetDeliveredLines();

    PlayerbotSocialRememberDeliveredLine(500, "Moving out.", 5000);

    EXPECT_FALSE(PlayerbotSocialWasDeliveredLine(500, "Moving out.", 1000));
}

TEST(PlayerbotSocialRouteTest, ABotThatJustSpokeIsRememberedSoTheReplyCooldownCanFire)
{
    /*
     * The opportunity gate refuses a bot that answered too recently, and it reads this. Production
     * capture left the field at its default of zero, which is always outside the cooldown, so the
     * cooldown never fired and a bot answered every message it heard. That failure looks like the
     * feature working enthusiastically rather than like it being broken, which is why it needs a
     * test rather than a comment.
     */
    PlayerbotSocialForgetDeliveredLines();

    EXPECT_EQ(PlayerbotSocialLastSpokeAt(500), 0u);

    PlayerbotSocialRememberSpoke(500, 1000);
    EXPECT_EQ(PlayerbotSocialLastSpokeAt(500), 1000u);

    // One bot speaking says nothing about another.
    EXPECT_EQ(PlayerbotSocialLastSpokeAt(501), 0u);
}

TEST(PlayerbotSocialRouteTest, ASpeakerIsForgottenOnceItsCooldownCannotBind)
{
    /*
     * Pruned rather than kept, because an entry older than the cooldown answers the same as no entry
     * and keeping it would add one row per bot that has ever spoken for the life of the process.
     * Pruning happens on write, so a later write is what clears an earlier speaker.
     */
    PlayerbotSocialForgetDeliveredLines();

    PlayerbotSocialRememberSpoke(500, 1000);
    PlayerbotSocialRememberSpoke(501, 1000 + PLAYERBOT_SOCIAL_REPLY_COOLDOWN_SECONDS + 1);

    EXPECT_EQ(PlayerbotSocialLastSpokeAt(500), 0u);
    EXPECT_EQ(PlayerbotSocialLastSpokeAt(501), 1000u + PLAYERBOT_SOCIAL_REPLY_COOLDOWN_SECONDS + 1);
}

TEST(PlayerbotSocialRouteTest, ASpeakerStampedInTheFutureIsDroppedRatherThanSilencedForever)
{
    /*
     * A wall clock that stepped back leaves an entry ahead of now. It can never age out, so trusting
     * it would hold the bot silent indefinitely. Here the safe direction is the opposite of the echo
     * record's: dropping it costs one uncooled reply, keeping it costs the bot its voice.
     */
    PlayerbotSocialForgetDeliveredLines();

    PlayerbotSocialRememberSpoke(500, 9000);
    PlayerbotSocialRememberSpoke(501, 1000);

    EXPECT_EQ(PlayerbotSocialLastSpokeAt(500), 0u);
}

// Runtime administrative control -------------------------------------------------------------------

namespace
{
// The four surfaces the feature owns, which are exactly the ones a control may silence.
constexpr ChatChannelSource OWNED_SOURCES[] = {ChatChannelSource::SRC_GENERAL, ChatChannelSource::SRC_SAY,
                                               ChatChannelSource::SRC_PARTY, ChatChannelSource::SRC_WHISPER};
}  // namespace

TEST(PlayerbotSocialRuntimeControlTest, APausedFeatureFallsSilentRatherThanBackToCannedReplies)
{
    /*
     * Pause is an operator stop, not a rollback to the pre-feature server. Routing the four owned
     * surfaces to LegacyOnly would make pressing pause START the canned chatter this feature exists
     * to replace, which is the opposite of what an operator reaching for pause wants. Deployment
     * rollback is the configuration option, and it keeps its own legacy behaviour.
     */
    PlayerbotSocialGate gate = EnabledGate();
    gate.paused = true;

    for (ChatChannelSource source : OWNED_SOURCES)
    {
        PlayerbotSocialInboundDecision const decision = PlayerbotSocialRouteInbound(source, HeardNormally(), gate);

        EXPECT_EQ(decision.route, PlayerbotSocialInboundRoute::SuppressedSurface)
            << "source " << static_cast<uint32>(source);
        EXPECT_TRUE(decision.suppressLegacyReply) << "source " << static_cast<uint32>(source);
    }
}

TEST(PlayerbotSocialRuntimeControlTest, ADisabledChannelSilencesOnlyItself)
{
    // A per-channel control has to be exactly per channel. Silencing General must leave a party
    // conversation running, or the toggle is a second pause under a misleading name.
    for (PlayerbotSocialChannel silenced : {PlayerbotSocialChannel::General, PlayerbotSocialChannel::Say,
                                            PlayerbotSocialChannel::Party, PlayerbotSocialChannel::Whisper})
    {
        PlayerbotSocialGate gate = EnabledGate();
        gate.channelEnabled[static_cast<std::size_t>(silenced)] = false;

        for (ChatChannelSource source : OWNED_SOURCES)
        {
            PlayerbotSocialChannel channel = PlayerbotSocialChannel::General;
            ASSERT_TRUE(PlayerbotSocialChannelFromChatSource(source, channel));

            PlayerbotSocialInboundDecision const decision = PlayerbotSocialRouteInbound(source, HeardNormally(), gate);

            if (channel == silenced)
            {
                EXPECT_EQ(decision.route, PlayerbotSocialInboundRoute::SuppressedSurface);
                EXPECT_TRUE(decision.suppressLegacyReply);
            }
            else
            {
                EXPECT_EQ(decision.route, PlayerbotSocialInboundRoute::SocialOpportunity)
                    << "silencing " << static_cast<uint32>(silenced) << " also hit " << static_cast<uint32>(channel);
            }
        }
    }
}

TEST(PlayerbotSocialRuntimeControlTest, ASilencedChannelStillTracksAConversationItAlreadyDelivered)
{
    /*
     * A line the feature itself delivered before the channel was silenced is still a turn in a
     * thread that exists. Dropping it would leave that thread believing its own last message was
     * never said, so the continuation is recorded even though nothing new may be opened.
     */
    PlayerbotSocialGate gate = EnabledGate();
    gate.channelEnabled[static_cast<std::size_t>(PlayerbotSocialChannel::Party)] = false;

    PlayerbotSocialInboundDecision const decision =
        PlayerbotSocialRouteInbound(ChatChannelSource::SRC_PARTY, DeliveredBySocial(), gate);

    EXPECT_EQ(decision.route, PlayerbotSocialInboundRoute::ThreadContinuationOnly);
    EXPECT_TRUE(decision.suppressLegacyReply);
}

TEST(PlayerbotSocialRuntimeControlTest, TheConfigurationGateStillWinsOverEveryRuntimeControl)
{
    /*
     * A control arriving over a socket must not be able to turn on a feature the deployment chose to
     * leave off. Effective enablement is the configuration AND not paused, never the control alone,
     * so the worst a stolen token buys is silence rather than an unwanted feature.
     */
    PlayerbotSocialGate gate = DisabledGate();
    gate.paused = false;
    for (bool& enabled : gate.channelEnabled)
        enabled = true;

    for (ChatChannelSource source : OWNED_SOURCES)
    {
        PlayerbotSocialInboundDecision const decision = PlayerbotSocialRouteInbound(source, HeardNormally(), gate);

        EXPECT_EQ(decision.route, PlayerbotSocialInboundRoute::LegacyOnly) << "source " << static_cast<uint32>(source);
        EXPECT_FALSE(decision.suppressLegacyReply);
    }
}

TEST(PlayerbotSocialRuntimeControlTest, AFreshGateIsUnpausedWithEveryChannelCarrying)
{
    // The default has to be the behaviour of a deployment that never touched a control, or enabling
    // the feature for the first time would land in a half silenced state nobody configured.
    PlayerbotSocialGate const gate;

    EXPECT_FALSE(gate.paused);
    ASSERT_EQ(std::size(gate.channelEnabled), PLAYERBOT_SOCIAL_CHANNEL_COUNT);
    for (bool const enabled : gate.channelEnabled)
        EXPECT_TRUE(enabled);
}

TEST(PlayerbotSocialRuntimeControlTest, AnOverlayTakesTheOperatorsValuesAndLeavesConfigurationFacts)
{
    /*
     * The split that makes restart survival correct: the stored row owns what an operator can
     * change, and the configuration owns what only a deployment can. Overwriting `enabled` or the
     * retention floor from a stored row would let a control quietly outlive the configuration that
     * is supposed to bound it.
     */
    PlayerbotSocialGate configured;
    configured.enabled = true;
    configured.density = PlayerbotSocialDensityProfile::Quiet;
    configured.telemetryRetentionHours = 96;

    PlayerbotSocialRuntimeControl control;
    control.paused = true;
    control.density = PlayerbotSocialDensityProfile::Lively;
    control.channelEnabled[static_cast<std::size_t>(PlayerbotSocialChannel::Whisper)] = false;

    PlayerbotSocialGate const overlaid = PlayerbotSocialOverlayRuntimeControl(configured, control);

    // The operator's values.
    EXPECT_TRUE(overlaid.paused);
    EXPECT_EQ(overlaid.density, PlayerbotSocialDensityProfile::Lively);
    EXPECT_FALSE(overlaid.channelEnabled[static_cast<std::size_t>(PlayerbotSocialChannel::Whisper)]);
    EXPECT_TRUE(overlaid.channelEnabled[static_cast<std::size_t>(PlayerbotSocialChannel::General)]);

    // The deployment's facts, untouched.
    EXPECT_TRUE(overlaid.enabled);
    EXPECT_EQ(overlaid.telemetryRetentionHours, 96u);
}

TEST(PlayerbotSocialRuntimeControlTest, AnOverlayCannotEnableADisabledDeployment)
{
    // The same rule stated as the property rather than through the router, because this is the one
    // an authentication bypass would be trying to reach.
    PlayerbotSocialGate configured;
    configured.enabled = false;

    PlayerbotSocialRuntimeControl control;
    control.paused = false;

    EXPECT_FALSE(PlayerbotSocialOverlayRuntimeControl(configured, control).enabled);
}

TEST(PlayerbotSocialRuntimeControlTest, ControlSeededFromTheConfigurationChangesNothingByItself)
{
    /*
     * The first control an operator sends creates the stored row, and every value it does not name
     * has to be written as whatever was already in effect. Seeding from the configuration and
     * overlaying it back must be the identity, or sending "pause off" would silently also reset the
     * density the configuration asked for.
     */
    PlayerbotSocialGate configured;
    configured.enabled = true;
    configured.density = PlayerbotSocialDensityProfile::Quiet;
    configured.telemetryRetentionHours = 72;

    PlayerbotSocialGate const round =
        PlayerbotSocialOverlayRuntimeControl(configured, PlayerbotSocialSeedRuntimeControl(configured));

    EXPECT_EQ(round.enabled, configured.enabled);
    EXPECT_EQ(round.density, configured.density);
    EXPECT_EQ(round.telemetryRetentionHours, configured.telemetryRetentionHours);
    EXPECT_FALSE(round.paused);
    for (bool const enabled : round.channelEnabled)
        EXPECT_TRUE(enabled);
}

TEST(PlayerbotSocialRuntimeControlTest, ASilencedGeneralDropsTheCannedLineWithoutConvertingIt)
{
    /*
     * Silence has two halves on the outbound side and both are needed. The canned broadcast still
     * does not go out, because the deployment enabled this feature and rolling that back is the
     * configuration's job; and it does not become a conversation starter either, because the channel
     * an operator silenced must not be where a bot opens a new subject.
     *
     * Returning DeliverAsToday here instead would make a pause switch the canned broadcast chatter
     * back ON, which is close to the opposite of what an operator reaching for pause wants.
     */
    for (bool const viaPause : {true, false})
    {
        PlayerbotSocialGate gate = EnabledGate();
        if (viaPause)
            gate.paused = true;
        else
            gate.channelEnabled[static_cast<std::size_t>(PlayerbotSocialChannel::General)] = false;

        EXPECT_EQ(PlayerbotSocialRouteBroadcast(BroadcastHelper::TO_GENERAL, gate),
                  PlayerbotSocialBroadcastRoute::SuppressCannedDelivery)
            << (viaPause ? "paused" : "channel off");
    }

    // Unsilenced, it still converts, so the suppression above is the control's doing and not a
    // regression in the ordinary path.
    EXPECT_EQ(PlayerbotSocialRouteBroadcast(BroadcastHelper::TO_GENERAL, EnabledGate()),
              PlayerbotSocialBroadcastRoute::StarterContext);

    // And with the feature configured off, a control cannot stop the canned line reaching players.
    PlayerbotSocialGate configuredOff = DisabledGate();
    configuredOff.paused = true;
    EXPECT_EQ(PlayerbotSocialRouteBroadcast(BroadcastHelper::TO_GENERAL, configuredOff),
              PlayerbotSocialBroadcastRoute::DeliverAsToday);
}

// The canonical delivery helper ---------------------------------------------------------------------

namespace
{
/*
 * One line a direct producer is about to speak. Say and `LANG_UNIVERSAL`, because that is what
 * almost every site this task migrates actually passes; the cases that differ say so.
 */
PlayerbotSocialDeliveryRequest SpokenRequest()
{
    PlayerbotSocialDeliveryRequest request;
    request.channel = PlayerbotSocialChannel::Say;
    request.origin = PlayerbotSocialEventOrigin::Legacy;
    request.languageId = LANG_UNIVERSAL;
    request.text = "Let's fish a bit.";
    request.occurredAtUnixSeconds = 8000;
    return request;
}

PlayerbotSocialSpeaker Speaker()
{
    PlayerbotSocialSpeaker speaker;
    speaker.botGuidCounter = 41;
    speaker.zoneId = 12;
    return speaker;
}
}  // namespace

TEST(PlayerbotSocialRouteTest, ARefusedSendIsRecordedNowhere)
{
    /*
     * Key Decision 5, and the rule Task 11A adopted for the canonical seam. The feed is a record of
     * what players HEARD. A line the world refused reached nobody, so a row for it would read as
     * speech that happened. This is the single rule that made the helper worth splitting in two:
     * with the send behind a world the unit harness cannot build, the refusal case is otherwise
     * unprovable.
     */
    PlayerbotSocialDelivery record;

    EXPECT_FALSE(PlayerbotSocialDeliveryRecordFor(SpokenRequest(), Speaker(), false, 9000, record))
        << "a send the world would not carry produces no event at all";
}

TEST(PlayerbotSocialRouteTest, AnAcceptedLineCarriesTheOriginItsProducerStated)
{
    // Definition of Done 2: each migrated site states its own origin, and the helper carries it
    // rather than inferring one from whatever the world thread happens to be doing.
    PlayerbotSocialDeliveryRequest request = SpokenRequest();
    request.origin = PlayerbotSocialEventOrigin::CombatStatus;
    PlayerbotSocialDelivery record;

    ASSERT_TRUE(PlayerbotSocialDeliveryRecordFor(request, Speaker(), true, 9000, record));

    EXPECT_EQ(record.origin, PlayerbotSocialEventOrigin::CombatStatus);
    EXPECT_EQ(record.channel, PlayerbotSocialChannel::Say);
    EXPECT_EQ(record.botGuidCounter, 41u);
    EXPECT_EQ(record.zoneId, 12u) << "where it was spoken, not where the decision was made";
    EXPECT_EQ(record.text, "Let's fish a bit.");
    EXPECT_FALSE(record.isEmote);
}

TEST(PlayerbotSocialRouteTest, AWhisperNamesTheCharacterItWasSaidTo)
{
    PlayerbotSocialDeliveryRequest request = SpokenRequest();
    request.channel = PlayerbotSocialChannel::Whisper;
    PlayerbotSocialSpeaker speaker = Speaker();
    speaker.targetGuidCounter = 900;
    PlayerbotSocialDelivery record;

    ASSERT_TRUE(PlayerbotSocialDeliveryRecordFor(request, speaker, true, 9000, record));

    EXPECT_EQ(record.channel, PlayerbotSocialChannel::Whisper);
    EXPECT_EQ(record.targetGuidCounter, 900u) << "a whisper without its addressee is not a whisper";
}

TEST(PlayerbotSocialRouteTest, AnEmoteCarriesItsGestureAndNoWords)
{
    // An emote is a gesture with no line. Carrying the text field forward would make the feed invent
    // words for something the bot never said.
    PlayerbotSocialDeliveryRequest request = SpokenRequest();
    request.isEmote = true;
    request.emoteId = 4;
    PlayerbotSocialDelivery record;

    ASSERT_TRUE(PlayerbotSocialDeliveryRecordFor(request, Speaker(), true, 9000, record));

    EXPECT_TRUE(record.isEmote);
    EXPECT_EQ(record.emoteId, 4u);
    EXPECT_TRUE(record.text.empty()) << "the gesture replaces the line rather than accompanying it";
}

TEST(PlayerbotSocialRouteTest, FunctionalOutputBelongsToNoConversation)
{
    /*
     * A status announcement is not an answer to anything. Correlating one to whichever chat happened
     * to be open would be a fabrication, and every site this task migrates is functional output.
     */
    PlayerbotSocialDelivery record;

    ASSERT_TRUE(PlayerbotSocialDeliveryRecordFor(SpokenRequest(), Speaker(), true, 9000, record));
    EXPECT_TRUE(record.threadPublicId.empty());

    PlayerbotSocialDeliveryRequest generated = SpokenRequest();
    generated.origin = PlayerbotSocialEventOrigin::Social;
    generated.threadPublicId = "thr_00000000000000000000000000000001";

    ASSERT_TRUE(PlayerbotSocialDeliveryRecordFor(generated, Speaker(), true, 9000, record));
    EXPECT_EQ(record.threadPublicId, "thr_00000000000000000000000000000001")
        << "generated speech keeps the thread that traces it back to its opportunity";
}

TEST(PlayerbotSocialRouteTest, EveryGeneratedSurfaceKeepsItsOwnAndParentEventIdentitiesThroughFinalBinding)
{
    std::string const eventPublicId = "evt_00000000000000000000000000000081";
    std::string const parentEventPublicId = "evt_00000000000000000000000000000080";

    for (PlayerbotSocialChannel const channel : {PlayerbotSocialChannel::Say, PlayerbotSocialChannel::Whisper,
                                                 PlayerbotSocialChannel::General, PlayerbotSocialChannel::Party})
    {
        PlayerbotSocialDeliveryRequest request = SpokenRequest();
        request.origin = PlayerbotSocialEventOrigin::Social;
        request.channel = channel;
        request.eventPublicId = eventPublicId;
        request.replyToEventPublicId = parentEventPublicId;

        PlayerbotSocialSpeaker speaker = Speaker();
        if (channel == PlayerbotSocialChannel::Whisper)
            speaker.targetGuidCounter = 900;

        PlayerbotSocialDelivery delivery;
        ASSERT_TRUE(PlayerbotSocialDeliveryRecordFor(request, speaker, true, 9000, delivery));
        EXPECT_EQ(delivery.eventPublicId, eventPublicId);
        EXPECT_EQ(delivery.replyToEventPublicId, parentEventPublicId);

        PlayerbotSocialEventBinding binding;
        ASSERT_TRUE(PlayerbotSocialBuildEventBinding(PlayerbotSocialMakeDeliveryEvent(delivery), binding));
        EXPECT_EQ(binding.publicId, eventPublicId);
        EXPECT_EQ(binding.replyToEventPublicId, parentEventPublicId);
    }
}

TEST(PlayerbotSocialRouteTest, ARequestKeepsTheClockItWasGivenAndIsStampedWhenItHasNone)
{
    /*
     * The delivery pump stamps a whole tick from one clock so a line and its echo record agree to
     * the second. A direct producer has no such clock and gets the caller's.
     */
    PlayerbotSocialDelivery record;

    ASSERT_TRUE(PlayerbotSocialDeliveryRecordFor(SpokenRequest(), Speaker(), true, 9000, record));
    EXPECT_EQ(record.occurredAtUnixSeconds, 8000u) << "the tick's clock wins when it supplies one";

    PlayerbotSocialDeliveryRequest unstamped = SpokenRequest();
    unstamped.occurredAtUnixSeconds = 0;

    ASSERT_TRUE(PlayerbotSocialDeliveryRecordFor(unstamped, Speaker(), true, 9000, record));
    EXPECT_EQ(record.occurredAtUnixSeconds, 9000u);
}

TEST(PlayerbotSocialRouteTest, ABotsOwnVoiceIsItsFactionLanguage)
{
    /*
     * Key Decision 2. Most direct producers speak `LANG_UNIVERSAL` and must keep doing so, while the
     * two `PlayerbotAI` wrappers speak in the bot's own voice. Routing the first group through the
     * second would change which characters can understand those lines, which is a gameplay change
     * this task is forbidden to make. The faction voice is one function so the wrappers cannot
     * drift, and every other site keeps passing the language it always passed.
     */
    EXPECT_EQ(PlayerbotSocialSpokenLanguageFor(TEAM_ALLIANCE), static_cast<uint32>(LANG_COMMON));
    EXPECT_EQ(PlayerbotSocialSpokenLanguageFor(TEAM_HORDE), static_cast<uint32>(LANG_ORCISH));
}

// A packet's own surface -----------------------------------------------------------------------------

TEST(PlayerbotSocialRouteTest, AChatMsgNamesTheSurfaceAPacketWillArriveOn)
{
    /*
     * The producers that build their own chat packet know a `ChatMsg` and never compute a
     * `ChatChannelSource`, so the feed needs this second mapping to know what surface a bot answered
     * its owner on.
     */
    PlayerbotSocialChannel channel = PlayerbotSocialChannel::General;

    ASSERT_TRUE(PlayerbotSocialChannelFromChatMsg(CHAT_MSG_SAY, channel));
    EXPECT_EQ(channel, PlayerbotSocialChannel::Say);

    ASSERT_TRUE(PlayerbotSocialChannelFromChatMsg(CHAT_MSG_WHISPER, channel));
    EXPECT_EQ(channel, PlayerbotSocialChannel::Whisper);

    ASSERT_TRUE(PlayerbotSocialChannelFromChatMsg(CHAT_MSG_PARTY, channel));
    EXPECT_EQ(channel, PlayerbotSocialChannel::Party);

    ASSERT_TRUE(PlayerbotSocialChannelFromChatMsg(CHAT_MSG_PARTY_LEADER, channel));
    EXPECT_EQ(channel, PlayerbotSocialChannel::Party);
}

TEST(PlayerbotSocialRouteTest, PartyLeaderSpeechUsesThePartyIntakeSource)
{
    PlayerbotAI botAI;

    EXPECT_EQ(botAI.GetChatChannelSource(nullptr, CHAT_MSG_PARTY, ""), ChatChannelSource::SRC_PARTY);
    EXPECT_EQ(botAI.GetChatChannelSource(nullptr, CHAT_MSG_PARTY_LEADER, ""), ChatChannelSource::SRC_PARTY);
}

TEST(PlayerbotSocialRouteTest, ASurfaceThisCannotNameIsRefusedRatherThanGuessed)
{
    /*
     * `CHAT_MSG_CHANNEL` is the one worth stating: it names no channel here, so General cannot be
     * told apart from Trade or a defense channel, and guessing is how a trade advert lands in a feed
     * of conversation. An addon message is not speech at all.
     */
    PlayerbotSocialChannel channel = PlayerbotSocialChannel::General;

    for (ChatMsg const refused :
         {CHAT_MSG_ADDON, CHAT_MSG_CHANNEL, CHAT_MSG_RAID, CHAT_MSG_GUILD, CHAT_MSG_YELL, CHAT_MSG_EMOTE})
        EXPECT_FALSE(PlayerbotSocialChannelFromChatMsg(refused, channel))
            << "a surface this cannot identify must not be filed under one it can";
}

// The helper against a real character ----------------------------------------------------------------

namespace
{
/*
 * The pure rule above decides WHAT the feed is told. This fixture covers the wiring around it,
 * which the rule cannot: that the helper actually reaches the world, that a refused send
 * propagates as no record at all, and that a migrated producer's line arrives once rather than
 * twice or not at all.
 *
 * It borrows the core's integration fixture, so `bot->Say` runs the real `Player::Say` against a
 * real session and map. The gate is opened for the duration and restored afterwards, because it
 * is process wide and every other test expects it shut.
 */
class PlayerbotSocialDeliveryWiringTest : public IntegrationTestFixture
{
protected:
    void SetUp() override
    {
        IntegrationTestFixture::SetUp();
        static bool scriptsAdded = false;
        if (!scriptsAdded)
        {
            AddPlayerbotsSocialScripts();
            scriptsAdded = true;
        }
        _restoreEnabled = sPlayerbotSocialConfig.socialChatEnable;
        _restoreStage = sPlayerbotSocialConfig.socialChatStage;
        _restoreBroadcasts = sPlayerbotAIConfig.enableBroadcasts;
        _restoreChanceMax = sPlayerbotAIConfig.broadcastChanceMaxValue;
        _restoreGenericLevelChance = sPlayerbotAIConfig.broadcastChanceLevelupGeneric;
        _restoreTenLevelChance = sPlayerbotAIConfig.broadcastChanceLevelupTenX;
        _restoreMaxLevelChance = sPlayerbotAIConfig.broadcastChanceLevelupMaxLevel;
        _restoreRandomBotMaxLevel = sPlayerbotAIConfig.randomBotMaxLevel;

        sPlayerbotSocialConfig.socialChatEnable = true;
    }

    void TearDown() override
    {
        for (uint64 guidCounter : _consentedGuidCounters)
            sPlayerbotSocialMgr.ForgetConsent(guidCounter);
        _consentedGuidCounters.clear();

        for (TestPlayer* player : _registeredPlayers)
            ObjectAccessor::RemoveObject(static_cast<Player*>(player));
        _registeredPlayers.clear();

        sPlayerbotSocialConfig.socialChatEnable = _restoreEnabled;
        sPlayerbotSocialConfig.socialChatStage = _restoreStage;
        sPlayerbotAIConfig.enableBroadcasts = _restoreBroadcasts;
        sPlayerbotAIConfig.broadcastChanceMaxValue = _restoreChanceMax;
        sPlayerbotAIConfig.broadcastChanceLevelupGeneric = _restoreGenericLevelChance;
        sPlayerbotAIConfig.broadcastChanceLevelupTenX = _restoreTenLevelChance;
        sPlayerbotAIConfig.broadcastChanceLevelupMaxLevel = _restoreMaxLevelChance;
        sPlayerbotAIConfig.randomBotMaxLevel = _restoreRandomBotMaxLevel;
        IntegrationTestFixture::TearDown();
    }

    void RegisterPlayer(TestPlayer* player)
    {
        ObjectAccessor::AddObject(static_cast<Player*>(player));
        _registeredPlayers.push_back(player);
    }

    void ConsentPlayer(TestPlayer* player)
    {
        uint64 const guidCounter = player->GetGUID().GetCounter();
        sPlayerbotSocialMgr.ApplyConsentSnapshot(guidCounter, false);
        _consentedGuidCounters.push_back(guidCounter);
    }

    // A delta rather than an absolute, because the coordinator is a process wide singleton and
    // this fixture has no business asserting what else a suite run has left in it.
    [[nodiscard]] std::size_t RecordedCount() const { return sPlayerbotSocialMgr.PendingEventCount(); }

    [[nodiscard]] PlayerbotSocialEventBinding NewestRecord() const
    {
        std::vector<PlayerbotSocialEventBinding> const pending = sPlayerbotSocialMgr.PendingEvents();
        return pending.empty() ? PlayerbotSocialEventBinding() : pending.back();
    }

private:
    bool _restoreEnabled = false;
    bool _restoreBroadcasts = false;
    std::string _restoreStage;
    uint32 _restoreChanceMax = 0;
    uint32 _restoreGenericLevelChance = 0;
    uint32 _restoreTenLevelChance = 0;
    uint32 _restoreMaxLevelChance = 0;
    uint32 _restoreRandomBotMaxLevel = 0;
    std::vector<uint64> _consentedGuidCounters;
    std::vector<TestPlayer*> _registeredPlayers;
};
}  // namespace

TEST_F(PlayerbotSocialDeliveryWiringTest, ALevelSourceReachesSocialWhenLegacyChanceIsZero)
{
    TestPlayer* const bot = CreateTestPlayer(710, "Relar");
    TestPlayer* const listener = CreateTestPlayer(711, "Pierre");
    bot->SetLevel(5);
    bot->setRace(RACE_HUMAN);
    bot->SetByteValue(UNIT_FIELD_BYTES_0, 1, CLASS_WARRIOR);
    listener->setRace(RACE_HUMAN);
    listener->SetByteValue(UNIT_FIELD_BYTES_0, 1, CLASS_WARRIOR);
    RegisterPlayer(bot);
    RegisterPlayer(listener);
    ConsentPlayer(listener);

    ON_CALL(*GetWorldMock(), getFloatConfig(CONFIG_LISTEN_RANGE_SAY)).WillByDefault(Return(50.0f));
    sPlayerbotSocialConfig.socialChatStage = "grounded_presence";
    sPlayerbotAIConfig.enableBroadcasts = false;
    sPlayerbotAIConfig.broadcastChanceMaxValue = 30000;
    sPlayerbotAIConfig.broadcastChanceLevelupGeneric = 0;
    sPlayerbotAIConfig.broadcastChanceLevelupTenX = 0;
    sPlayerbotAIConfig.broadcastChanceLevelupMaxLevel = 0;
    sPlayerbotAIConfig.randomBotMaxLevel = 80;

    PlayerbotAI botAI(bot);
    std::size_t const before = RecordedCount();

    PlayerbotSocialGate const gate = PlayerbotSocialEffectiveGate();
    ASSERT_TRUE(PlayerbotSocialGateIsLive(gate));
    ASSERT_EQ(gate.stage, PlayerbotSocialRolloutStage::GroundedStarters);
    ASSERT_TRUE(bot->IsInWorld());
    ASSERT_TRUE(listener->IsInWorld());
    ASSERT_EQ(bot->GetMap(), listener->GetMap());
    ASSERT_EQ(bot->GetTeamId(), listener->GetTeamId());
    ASSERT_TRUE(bot->CanSeeOrDetect(listener));
    ASSERT_TRUE(bot->IsWithinDistInMap(listener, 50.0f));
    ASSERT_FALSE(sPlayerbotSocialMgr.IsOptedOut(listener->GetGUID().GetCounter()));

    EXPECT_FALSE(BroadcastHelper::BroadcastLevelup(&botAI, bot));
    ASSERT_EQ(RecordedCount(), before + 1)
        << "the typed level source must reach Social even when its canned chance is zero";

    PlayerbotSocialEventBinding const source = NewestRecord();
    EXPECT_EQ(source.eventType, "social.source");
    EXPECT_EQ(source.reason, "level");
    EXPECT_EQ(source.botGuidCounter, 710u);
    EXPECT_EQ(source.targetGuidCounter, 711u);
}

TEST_F(PlayerbotSocialDeliveryWiringTest, AQuestSourceRecordsTheExactAuthoritativeTransition)
{
    TestPlayer* const bot = CreateTestPlayer(712, "Relar");
    TestPlayer* const listener = CreateTestPlayer(713, "Pierre");
    bot->setRace(RACE_HUMAN);
    bot->SetByteValue(UNIT_FIELD_BYTES_0, 1, CLASS_WARRIOR);
    listener->setRace(RACE_HUMAN);
    listener->SetByteValue(UNIT_FIELD_BYTES_0, 1, CLASS_WARRIOR);
    RegisterPlayer(bot);
    RegisterPlayer(listener);
    ConsentPlayer(listener);

    ON_CALL(*GetWorldMock(), getFloatConfig(CONFIG_LISTEN_RANGE_SAY)).WillByDefault(Return(50.0f));
    sPlayerbotSocialConfig.socialChatStage = "grounded_presence";

    PlayerbotSocialStarterSource source;
    source.kind = PlayerbotSocialStarterSourceKind::QuestTransition;
    source.questTransition = PlayerbotSocialQuestTransition::TurnedIn;
    source.subjectId = 127;
    source.subject = "Selling Fish";

    PlayerbotAI botAI(bot);
    std::size_t const before = RecordedCount();

    ASSERT_TRUE(PlayerbotSocialQueueStarterSource(&botAI, std::move(source)));
    ASSERT_EQ(RecordedCount(), before + 1);

    PlayerbotSocialEventBinding const recorded = NewestRecord();
    EXPECT_EQ(recorded.eventType, "social.source");
    EXPECT_EQ(recorded.reason, "quest_transition");
    EXPECT_NE(recorded.diagnosticsJson.find("\"quest_transition\":\"turned_in\""), std::string::npos)
        << "telemetry must retain the event semantics that grounding receives";
}

TEST_F(PlayerbotSocialDeliveryWiringTest, ASpokenLineIsRecordedOnceThroughTheHelper)
{
    TestPlayer* const bot = CreateTestPlayer(700, "Relar");
    std::size_t const before = RecordedCount();

    EXPECT_TRUE(PlayerbotSocialSay(bot, "Storm coming.", LANG_UNIVERSAL, PlayerbotSocialEventOrigin::CombatStatus));

    ASSERT_EQ(RecordedCount(), before + 1) << "exactly one event, not none and not one per layer";

    PlayerbotSocialEventBinding const record = NewestRecord();
    EXPECT_EQ(record.messageText, "Storm coming.");
    EXPECT_EQ(record.botGuidCounter, 700u);
    EXPECT_EQ(record.origin, "combat_status") << "the origin the producer stated, not one inferred here";
    EXPECT_EQ(record.channel, "say");
    EXPECT_EQ(record.outcome, "delivered");
}

TEST_F(PlayerbotSocialDeliveryWiringTest, AGeneratedLineStoresItsReservedIdentityAndExactReplyParent)
{
    TestPlayer* const bot = CreateTestPlayer(706, "Relar");
    std::size_t const before = RecordedCount();

    PlayerbotSocialDeliveryRequest request;
    request.channel = PlayerbotSocialChannel::Say;
    request.origin = PlayerbotSocialEventOrigin::Social;
    request.languageId = LANG_UNIVERSAL;
    request.text = "The mine is clear now.";
    request.threadPublicId = "thr_00000000000000000000000000000001";
    request.eventPublicId = PlayerbotSocialMakeEventPublicId(81, 706);
    request.replyToEventPublicId = PlayerbotSocialMakeEventPublicId(80, 900);

    EXPECT_TRUE(PlayerbotSocialDeliver(bot, nullptr, request));
    ASSERT_EQ(RecordedCount(), before + 1);

    PlayerbotSocialEventBinding const record = NewestRecord();
    EXPECT_EQ(record.publicId, request.eventPublicId);
    EXPECT_EQ(record.replyToEventPublicId, request.replyToEventPublicId);
}

TEST_F(PlayerbotSocialDeliveryWiringTest, AGeneratedWhisperStoresTheIdentityItsEchoCarries)
{
    TestPlayer* const bot = CreateTestPlayer(708, "Relar");
    TestPlayer* const target = CreateTestPlayer(709, "Pierre");
    std::size_t const before = RecordedCount();

    PlayerbotSocialDeliveryRequest request;
    request.channel = PlayerbotSocialChannel::Whisper;
    request.origin = PlayerbotSocialEventOrigin::Social;
    request.languageId = LANG_UNIVERSAL;
    request.text = "The road is safe.";
    request.threadPublicId = "thr_00000000000000000000000000000001";
    request.eventPublicId = PlayerbotSocialMakeEventPublicId(82, 708);
    request.replyToEventPublicId = PlayerbotSocialMakeEventPublicId(81, 900);

    EXPECT_TRUE(PlayerbotSocialDeliver(bot, target, request));
    ASSERT_EQ(RecordedCount(), before + 1);

    PlayerbotSocialEventBinding const record = NewestRecord();
    EXPECT_EQ(record.publicId, request.eventPublicId);
    EXPECT_EQ(record.replyToEventPublicId, request.replyToEventPublicId);
}

TEST_F(PlayerbotSocialDeliveryWiringTest, AWhisperWithNoAddresseeSpeaksToNobodyAndRecordsNothing)
{
    /*
     * The refusal rule reaching all the way out to a producer. A whisper whose addressee has gone
     * reached nobody, so the helper must report the failure to its caller AND leave the feed alone;
     * a row here would read as speech a player heard.
     */
    TestPlayer* const bot = CreateTestPlayer(701, "Relar");
    std::size_t const before = RecordedCount();

    EXPECT_FALSE(PlayerbotSocialWhisper(bot, "Meet me in Goldshire.", LANG_UNIVERSAL, nullptr,
                                        PlayerbotSocialEventOrigin::Legacy));

    EXPECT_EQ(RecordedCount(), before);
}

TEST_F(PlayerbotSocialDeliveryWiringTest, ARefusedGeneratedWhisperLeavesNoEventOrEchoMarker)
{
    TestPlayer* const bot = CreateTestPlayer(707, "Relar");
    std::size_t const before = RecordedCount();

    PlayerbotSocialDeliveryRequest request;
    request.channel = PlayerbotSocialChannel::Whisper;
    request.origin = PlayerbotSocialEventOrigin::Social;
    request.languageId = LANG_UNIVERSAL;
    request.text = "Are you still there?";
    request.threadPublicId = "thr_00000000000000000000000000000001";

    EXPECT_FALSE(PlayerbotSocialDeliver(bot, nullptr, request));
    EXPECT_EQ(RecordedCount(), before);
    EXPECT_FALSE(PlayerbotSocialWasDeliveredLine(707, request.text, static_cast<uint64>(time(nullptr))));
}

TEST_F(PlayerbotSocialDeliveryWiringTest, AnAnswerBuiltAsAPacketIsRecordedOnTheSurfaceItArrivedOn)
{
    // `PlayerbotAI` answers its owner by building the packet itself so the reply lands on the surface
    // the owner is already using. The feed has to show it on that same surface.
    TestPlayer* const bot = CreateTestPlayer(702, "Relar");
    TestPlayer* const owner = CreateTestPlayer(703, "Pierre");
    std::size_t const before = RecordedCount();

    EXPECT_TRUE(PlayerbotSocialDeliverDirect(bot, owner, CHAT_MSG_PARTY, LANG_UNIVERSAL, "On my way.",
                                             PlayerbotSocialEventOrigin::Legacy));

    ASSERT_EQ(RecordedCount(), before + 1);

    PlayerbotSocialEventBinding const record = NewestRecord();
    EXPECT_EQ(record.channel, "party") << "the packet's own surface, not the whisper it defaults to";
    EXPECT_EQ(record.targetGuidCounter, 703u);
    EXPECT_EQ(record.messageText, "On my way.");
    EXPECT_TRUE(record.replyToEventPublicId.empty());
}

TEST_F(PlayerbotSocialDeliveryWiringTest, AnAddonAnswerIsStillSentAndStillStaysOutOfTheFeed)
{
    /*
     * Key Decision 6 from the other direction: an unsupported surface is refused at the recorder
     * rather than at the producer, and refusing it must not stop the send. An addon message is
     * machine traffic, and the reply the owner's client is waiting on has to go out regardless.
     */
    TestPlayer* const bot = CreateTestPlayer(704, "Relar");
    TestPlayer* const owner = CreateTestPlayer(705, "Pierre");
    std::size_t const before = RecordedCount();

    EXPECT_TRUE(PlayerbotSocialDeliverDirect(bot, owner, CHAT_MSG_ADDON, LANG_ADDON, "sync 1",
                                             PlayerbotSocialEventOrigin::Legacy));

    EXPECT_EQ(RecordedCount(), before) << "sent, and correctly absent from a feed of conversation";
}

// Roleplay assessment routing ----------------------------------------------------------------------

TEST(PlayerbotSocialAssessmentRoutingTest, OnlyObservedHumanRepliesRequireAssessment)
{
    PlayerbotSocialActivation activation;
    activation.thread.valid = true;
    activation.thread.threadId = 7;
    activation.thread.publicId = "thr_00000000000000000000000000000001";
    activation.channel = PlayerbotSocialChannel::General;
    activation.speakerGuidCounter = 900;
    activation.speakerIsHuman = true;
    activation.starter = false;

    // The one shape that goes through the classifier: an observed human reply opportunity.
    EXPECT_TRUE(PlayerbotSocialOpportunityRequiresAssessment(activation));

    // A bot's line is never assessed: bot initiated roleplay is out of this slice entirely.
    PlayerbotSocialActivation botLine = activation;
    botLine.speakerIsHuman = false;
    EXPECT_FALSE(PlayerbotSocialOpportunityRequiresAssessment(botLine));

    // Starters keep the ordinary path, whoever pumped them.
    PlayerbotSocialActivation starter = activation;
    starter.starter = true;
    EXPECT_FALSE(PlayerbotSocialOpportunityRequiresAssessment(starter));

    // An invalid thread is refused by Activate anyway; assessing it first would waste a call.
    PlayerbotSocialActivation invalid = activation;
    invalid.thread.valid = false;
    EXPECT_FALSE(PlayerbotSocialOpportunityRequiresAssessment(invalid));
}
