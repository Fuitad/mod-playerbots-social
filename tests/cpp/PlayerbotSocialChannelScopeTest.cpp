/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#include <algorithm>
#include <barrier>
#include <set>
#include <string>
#include <thread>
#include <vector>

// Ahead of IntegrationTestFixture.h deliberately: that header does `using namespace testing`, and
// the playerbots engine declares its own Value and Action, which become ambiguous if gmock's are
// already in scope. The Route test orders its includes the same way for the same reason.
#include "Bot/Engine/AiObjectContext.h"
#include "Bot/PlayerbotMgr.h"
#include "Bot/Social/PlayerbotSocialChannelScope.h"
#include "Channel.h"
#include "ChannelMgr.h"
#include "DBCStores.h"
#include "IntegrationTestFixture.h"
#include "Player.h"
#include "PlayerbotAI.h"
#include "gtest/gtest.h"

namespace
{
constexpr uint32 SCOPE_GENERAL = 1;
constexpr uint32 SCOPE_LOCAL_DEFENSE = 22;
constexpr uint32 SCOPE_WORLD_DEFENSE = 23;
constexpr uint32 SCOPE_TRADE = 2;

std::vector<uint32> ZoneLocalIds() { return {SCOPE_GENERAL, SCOPE_LOCAL_DEFENSE}; }

bool Contains(std::vector<std::string> const& names, std::string const& name)
{
    return std::find(names.begin(), names.end(), name) != names.end();
}
}  // namespace

/*
 * The accumulation case, which is the whole reason this feature exists. Core's own routine takes the
 * first membership matching a channel id and stops, so a bot that has visited four zones keeps three
 * memberships no zone change will ever drain.
 */
TEST(PlayerbotSocialChannelScopeTest, EveryMembershipOutsideTheCurrentZoneIsLeftAtOnce)
{
    std::vector<PlayerbotSocialChannelMembership> const current = {
        {"General - Elwynn Forest", SCOPE_GENERAL},
        {"General - Westfall", SCOPE_GENERAL},
        {"General - Duskwood", SCOPE_GENERAL},
    };

    PlayerbotSocialChannelReconciliation const plan =
        PlayerbotSocialReconcileZoneChannels(current, {"General - Stormwind City"}, ZoneLocalIds());

    EXPECT_EQ(plan.leave.size(), 3u);
    EXPECT_TRUE(Contains(plan.leave, "General - Elwynn Forest"));
    EXPECT_TRUE(Contains(plan.leave, "General - Westfall"));
    EXPECT_TRUE(Contains(plan.leave, "General - Duskwood"));
    ASSERT_EQ(plan.join.size(), 1u);
    EXPECT_EQ(plan.join.front(), "General - Stormwind City");
}

TEST(PlayerbotSocialChannelScopeTest, AMembershipSetAlreadyEqualToTheExpectedSetIsLeftAlone)
{
    std::vector<PlayerbotSocialChannelMembership> const current = {
        {"General - Stormwind City", SCOPE_GENERAL},
        {"LocalDefense - Stormwind City", SCOPE_LOCAL_DEFENSE},
    };

    PlayerbotSocialChannelReconciliation const plan = PlayerbotSocialReconcileZoneChannels(
        current, {"General - Stormwind City", "LocalDefense - Stormwind City"}, ZoneLocalIds());

    EXPECT_TRUE(plan.leave.empty());
    EXPECT_TRUE(plan.join.empty());
}

/*
 * A channel the reconciler was not given authority over is never touched, however wrong its name
 * looks. World, LookingForGroup and the city scoped channels are deliberately outside its reach.
 */
TEST(PlayerbotSocialChannelScopeTest, AChannelOutsideTheZoneLocalSetIsNeverLeft)
{
    std::vector<PlayerbotSocialChannelMembership> const current = {
        {"WorldDefense", SCOPE_WORLD_DEFENSE},
        {"Trade - City", SCOPE_TRADE},
        {"General - Elwynn Forest", SCOPE_GENERAL},
    };

    PlayerbotSocialChannelReconciliation const plan =
        PlayerbotSocialReconcileZoneChannels(current, {"General - Stormwind City"}, ZoneLocalIds());

    EXPECT_FALSE(Contains(plan.leave, "WorldDefense"));
    EXPECT_FALSE(Contains(plan.leave, "Trade - City"));
    ASSERT_EQ(plan.leave.size(), 1u);
    EXPECT_EQ(plan.leave.front(), "General - Elwynn Forest");
}

TEST(PlayerbotSocialChannelScopeTest, AnExpectedChannelTheBotIsNotOnIsJoinedWithoutLeavingAnything)
{
    std::vector<PlayerbotSocialChannelMembership> const current = {
        {"General - Stormwind City", SCOPE_GENERAL},
    };

    PlayerbotSocialChannelReconciliation const plan = PlayerbotSocialReconcileZoneChannels(
        current, {"General - Stormwind City", "LocalDefense - Stormwind City"}, ZoneLocalIds());

    EXPECT_TRUE(plan.leave.empty());
    ASSERT_EQ(plan.join.size(), 1u);
    EXPECT_EQ(plan.join.front(), "LocalDefense - Stormwind City");
}

/*
 * Duplicate rows for one name collapse to a single leave. The membership scan reads the live channel
 * map, and a name that appears twice there is one channel to leave, not two.
 */
TEST(PlayerbotSocialChannelScopeTest, ARepeatedMembershipNameProducesOneLeaveRatherThanSeveral)
{
    std::vector<PlayerbotSocialChannelMembership> const current = {
        {"General - Elwynn Forest", SCOPE_GENERAL},
        {"General - Elwynn Forest", SCOPE_GENERAL},
    };

    PlayerbotSocialChannelReconciliation const plan =
        PlayerbotSocialReconcileZoneChannels(current, {"General - Stormwind City"}, ZoneLocalIds());

    ASSERT_EQ(plan.leave.size(), 1u);
    EXPECT_EQ(plan.leave.front(), "General - Elwynn Forest");
}

TEST(PlayerbotSocialChannelScopeTest, MarkingTheSameBotRepeatedlyLeavesOnePendingEntry)
{
    PlayerbotSocialChannelScopeQueue queue;
    queue.Mark(11);
    queue.Mark(11);
    queue.Mark(11);

    EXPECT_EQ(queue.PendingCount(), 1u);
}

TEST(PlayerbotSocialChannelScopeTest, ThePendingSetStopsAtItsBoundRatherThanGrowingWithoutLimit)
{
    PlayerbotSocialChannelScopeQueue queue;
    for (uint64 bot = 1; bot <= PLAYERBOT_SOCIAL_CHANNEL_SCOPE_PENDING_MAX + 50; ++bot)
        queue.Mark(bot);

    EXPECT_EQ(queue.PendingCount(), PLAYERBOT_SOCIAL_CHANNEL_SCOPE_PENDING_MAX);
}

/*
 * The budget is what keeps a full re-scan off a single tick. Draining must hand back at most the
 * budget and keep the remainder, rather than quietly discarding what it could not reach.
 */
TEST(PlayerbotSocialChannelScopeTest, ADrainTakesAtMostTheBudgetAndKeepsTheRest)
{
    PlayerbotSocialChannelScopeQueue queue;
    for (uint64 bot = 1; bot <= 40; ++bot)
        queue.Mark(bot);

    std::vector<uint64> const drained = queue.Drain(25);

    EXPECT_EQ(drained.size(), 25u);
    EXPECT_EQ(queue.PendingCount(), 15u);
}

TEST(PlayerbotSocialChannelScopeTest, ADrainSmallerThanTheBudgetEmptiesTheQueue)
{
    PlayerbotSocialChannelScopeQueue queue;
    queue.Mark(1);
    queue.Mark(2);

    EXPECT_EQ(queue.Drain(25).size(), 2u);
    EXPECT_EQ(queue.PendingCount(), 0u);
}

/*
 * A bot that could not be reconciled this tick, because it was mid-teleport or not yet in world,
 * goes back rather than being dropped: dropping it would strand the very membership this feature
 * exists to correct until the next full re-scan.
 */
TEST(PlayerbotSocialChannelScopeTest, ABotDeferredDuringItsDrainIsRequeuedRatherThanDropped)
{
    PlayerbotSocialChannelScopeQueue queue;
    queue.Mark(7);

    std::vector<uint64> const drained = queue.Drain(25);
    ASSERT_EQ(drained.size(), 1u);
    EXPECT_EQ(queue.PendingCount(), 0u);

    queue.Mark(drained.front());
    EXPECT_EQ(queue.PendingCount(), 1u);
}

/*
 * The zone hook runs on the map update threads, so marks arrive concurrently. Without a lock the
 * deque and set behind the queue are torn by parallel writers, which is process corruption rather
 * than a missed reconciliation. Every distinct bot must survive exactly once.
 */
TEST(PlayerbotSocialChannelScopeTest, ConcurrentMarksFromSeveralMapThreadsKeepTheQueueIntact)
{
    constexpr int WRITERS = 4;
    constexpr uint64 PER_WRITER = 60;

    PlayerbotSocialChannelScopeQueue queue;
    std::barrier start(WRITERS);
    std::vector<std::thread> writers;

    for (int writer = 0; writer < WRITERS; ++writer)
        writers.emplace_back(
            [&queue, &start, writer]()
            {
                start.arrive_and_wait();
                // Overlapping ranges, so the writers contend on dedup as well as on insertion.
                for (uint64 bot = 1; bot <= PER_WRITER; ++bot)
                    queue.Mark(bot + static_cast<uint64>(writer) * 10);
            });

    for (std::thread& writer : writers)
        writer.join();

    std::vector<uint64> const drained = queue.Drain(PLAYERBOT_SOCIAL_CHANNEL_SCOPE_PENDING_MAX);
    std::set<uint64> const distinct(drained.begin(), drained.end());

    // 1..60 from writer 0, shifted by 10 per writer, so the union is 1..90.
    EXPECT_EQ(drained.size(), 90u);
    EXPECT_EQ(distinct.size(), drained.size());
    EXPECT_EQ(*distinct.begin(), 1u);
    EXPECT_EQ(*distinct.rbegin(), 90u);
    EXPECT_EQ(queue.PendingCount(), 0u);
}

TEST(PlayerbotSocialChannelScopeTest, TheFullRescanIntervalIsRespectedBetweenSweeps)
{
    PlayerbotSocialChannelScopeQueue queue;

    // Nothing is due before the interval has elapsed, whatever the diff pattern.
    EXPECT_FALSE(queue.FullRescanDue(PLAYERBOT_SOCIAL_CHANNEL_SCOPE_RESCAN_MS - 1));
    EXPECT_TRUE(queue.FullRescanDue(1));

    // The clock restarts after a sweep rather than firing on every subsequent tick.
    EXPECT_FALSE(queue.FullRescanDue(1));
}

namespace
{
constexpr uint32 TEST_ZONE_CHANNEL_ID = 1;

/*
 * A built-in channel entry, which is what makes Channel treat the object as constant: the custom
 * branch of JoinChannel writes channel usage to the database, which no unit test may touch.
 */
void EnsureZoneChannelEntry()
{
    if (sChatChannelsStore.LookupEntry(TEST_ZONE_CHANNEL_ID))
        return;

    auto* entry = new ChatChannelsEntry{};
    entry->ChannelID = TEST_ZONE_CHANNEL_ID;
    entry->flags = CHANNEL_DBC_FLAG_ZONE_DEP;
    // Only enUS is filled, deliberately: a DBC row need not carry every locale, and the reconciler
    // has to fall back rather than hand a null format to snprintf.
    entry->pattern[LOCALE_enUS] = "General - %s";
    sChatChannelsStore.SetEntry(TEST_ZONE_CHANNEL_ID, entry);
}

class PlayerbotSocialChannelScopeLeaveTest : public IntegrationTestFixture
{
protected:
    void SetUp() override
    {
        IntegrationTestFixture::SetUp();
        EnsureZoneChannelEntry();
    }
};
}  // namespace

/*
 * The half-removal this feature has to avoid. `Channel::LeaveChannel(bot, false)` clears the
 * channel's member store but not the bot's own list, so a reconciler using it alone would leave the
 * bot believing it is still on every channel it just left, and the next pass would read that stale
 * list as truth.
 */
TEST_F(PlayerbotSocialChannelScopeLeaveTest, AReconciledRemovalClearsTheChannelAndTheBotsOwnList)
{
    TestPlayer* const bot = CreateTestPlayer(880, "ScopeBot");
    ASSERT_NE(bot, nullptr);

    Channel channel("General - Elwynn Forest", TEST_ZONE_CHANNEL_ID, 0, TEAM_ALLIANCE);
    channel.JoinChannel(bot, "");
    ASSERT_TRUE(channel.IsOn(bot->GetGUID()));
    ASSERT_TRUE(bot->IsInChannel(&channel));

    PlayerbotSocialLeaveChannelCompletely(bot, &channel);

    EXPECT_FALSE(channel.IsOn(bot->GetGUID()));
    // Core's IsInChannel compares channel IDS rather than identity (Player.cpp:5090). It is a valid
    // read of the player side only because this bot is on exactly one channel; a test with several
    // channels sharing an id must count memberships instead.
    EXPECT_FALSE(bot->IsInChannel(&channel));
}

namespace
{
class PlayerbotSocialChannelScopeReconcileTest : public IntegrationTestFixture
{
protected:
    void SetUp() override
    {
        IntegrationTestFixture::SetUp();
        EnsureZoneChannelEntry();
        _restoreEnabled = sPlayerbotAIConfig.enabled;
        sPlayerbotAIConfig.enabled = true;
    }

    void TearDown() override
    {
        delete _botAI;
        _botAI = nullptr;
        sPlayerbotAIConfig.enabled = _restoreEnabled;
        IntegrationTestFixture::TearDown();
    }

    // A zone entry at whatever id the fixture's player actually reports, so the test does not
    // depend on the map mock choosing any particular zone.
    static void EnsureAreaEntry(uint32 zoneId, char const* name)
    {
        if (sAreaTableStore.LookupEntry(zoneId))
            return;

        auto* entry = new AreaTableEntry{};
        entry->ID = zoneId;
        for (auto& localized : entry->area_name)
            localized = name;
        sAreaTableStore.SetEntry(zoneId, entry);
    }

    PlayerbotAI* _botAI = nullptr;
    bool _restoreEnabled = false;
};
}  // namespace

/*
 * The reconciler against real ChannelMgr and DBC state: a bot carrying General memberships for two
 * zones it is no longer in ends up on exactly one, the zone it is standing in, with both the channel
 * and the bot's own list agreeing.
 */
TEST_F(PlayerbotSocialChannelScopeReconcileTest, ABotOnForeignZoneGeneralsEndsUpOnOnlyItsOwn)
{
    TestPlayer* const bot = CreateTestPlayer(881, "ScopeReconcileBot");
    ASSERT_NE(bot, nullptr);

    // The fixture's player carries no race, and ChannelMgr::forTeam refuses a neutral team
    // (ChannelMgr.cpp:41-47). Give it a real side rather than reaching into the world mock.
    bot->setTeamId(TEAM_ALLIANCE);
    sPlayerbotsMgr.AddPlayerbotData(bot, true);
    _botAI = sPlayerbotsMgr.GetPlayerbotAI(bot);
    ASSERT_NE(_botAI, nullptr);

    EnsureAreaEntry(bot->GetZoneId(), "Testshire");

    ChannelMgr* const channelMgr = ChannelMgr::forTeam(bot->GetTeamId());
    ASSERT_NE(channelMgr, nullptr);

    Channel* const foreignA = channelMgr->GetJoinChannel("General - Elsewhere", TEST_ZONE_CHANNEL_ID);
    Channel* const foreignB = channelMgr->GetJoinChannel("General - Otherplace", TEST_ZONE_CHANNEL_ID);
    ASSERT_NE(foreignA, nullptr);
    ASSERT_NE(foreignB, nullptr);
    foreignA->JoinChannel(bot, "");
    foreignB->JoinChannel(bot, "");
    ASSERT_TRUE(foreignA->IsOn(bot->GetGUID()));
    ASSERT_TRUE(foreignB->IsOn(bot->GetGUID()));

    std::size_t const corrected = PlayerbotSocialReconcileBotChannels(bot);

    EXPECT_EQ(corrected, 2u);
    EXPECT_FALSE(foreignA->IsOn(bot->GetGUID()));
    EXPECT_FALSE(foreignB->IsOn(bot->GetGUID()));
    Channel* const home = channelMgr->GetChannel("General - Testshire", bot, false);
    ASSERT_NE(home, nullptr);
    EXPECT_TRUE(home->IsOn(bot->GetGUID()));

    /*
     * Counted over the channel map rather than asked of the player: core's Player::IsInChannel
     * compares channel IDS, not identity (Player.cpp:5090), so it answers true for any General once
     * the bot is on its own. Exactly one membership at this id is the invariant that matters.
     */
    std::size_t generals = 0;
    for (auto const& [name, channel] : channelMgr->GetChannels())
    {
        (void)name;
        if (channel != nullptr && channel->GetChannelId() == TEST_ZONE_CHANNEL_ID && channel->IsOn(bot->GetGUID()))
            ++generals;
    }
    EXPECT_EQ(generals, 1u);

    // Converged: a second pass has nothing left to correct.
    EXPECT_EQ(PlayerbotSocialReconcileBotChannels(bot), 0u);

    // ChannelMgr holds its managers in function-local statics, so the channels outlive this test.
    // Leaving them keeps a deleted player out of a live channel's member store.
    bot->CleanupChannels();
}

/*
 * The branch that first surfaced as a segfault: a zone-local DBC row carrying no name pattern for
 * any locale. The reconciler cannot know what the correct channel for this zone is called, so it
 * must renounce authority over the id entirely rather than leave every membership it finds there.
 * Asserting survival, not absence, is the point: skipping the join while still leaving is the
 * plausible wrong fix, and it would strip a membership the bot should keep.
 */
TEST_F(PlayerbotSocialChannelScopeReconcileTest, AChannelWhoseNameCannotBeBuiltIsLeftCompletelyAlone)
{
    constexpr uint32 NAMELESS_CHANNEL_ID = 22;
    if (!sChatChannelsStore.LookupEntry(NAMELESS_CHANNEL_ID))
    {
        auto* entry = new ChatChannelsEntry{};
        entry->ChannelID = NAMELESS_CHANNEL_ID;
        entry->flags = CHANNEL_DBC_FLAG_ZONE_DEP;
        // Every locale left null on purpose.
        sChatChannelsStore.SetEntry(NAMELESS_CHANNEL_ID, entry);
    }

    TestPlayer* const bot = CreateTestPlayer(882, "ScopeNamelessBot");
    ASSERT_NE(bot, nullptr);
    bot->setTeamId(TEAM_ALLIANCE);
    sPlayerbotsMgr.AddPlayerbotData(bot, true);
    _botAI = sPlayerbotsMgr.GetPlayerbotAI(bot);
    ASSERT_NE(_botAI, nullptr);

    EnsureAreaEntry(bot->GetZoneId(), "Testshire");

    ChannelMgr* const channelMgr = ChannelMgr::forTeam(bot->GetTeamId());
    ASSERT_NE(channelMgr, nullptr);

    Channel* const nameless = channelMgr->GetJoinChannel("LocalDefense - Elsewhere", NAMELESS_CHANNEL_ID);
    ASSERT_NE(nameless, nullptr);
    nameless->JoinChannel(bot, "");
    ASSERT_TRUE(nameless->IsOn(bot->GetGUID()));

    // Does not crash, and reports no correction for the nameless id.
    PlayerbotSocialReconcileBotChannels(bot);

    EXPECT_TRUE(nameless->IsOn(bot->GetGUID()));

    bot->CleanupChannels();
}

/*
 * The liveness report. A sweep that corrects nothing must still be visible, because a silent
 * reconciler and an absent one are otherwise indistinguishable from outside the process: that
 * ambiguity is what made the first live deployment unverifiable.
 */
TEST(PlayerbotSocialChannelScopeTest, TheActivityReportCarriesTotalsAndThenResets)
{
    PlayerbotSocialChannelScopeActivity activity;

    activity.Record(10, 3);
    activity.Record(5, 0);

    PlayerbotSocialChannelScopeReport const first = activity.TakeReport();
    EXPECT_EQ(first.reconciled, 15u);
    EXPECT_EQ(first.corrected, 3u);

    // Reset on read, so each interval reports its own work rather than a running total.
    PlayerbotSocialChannelScopeReport const second = activity.TakeReport();
    EXPECT_EQ(second.reconciled, 0u);
    EXPECT_EQ(second.corrected, 0u);
}

/*
 * A quiet interval still reports. Zero is the informative case here, not the one to suppress.
 */
TEST(PlayerbotSocialChannelScopeTest, AnIntervalThatCorrectedNothingStillReportsWhatItSwept)
{
    PlayerbotSocialChannelScopeActivity activity;
    activity.Record(25, 0);

    PlayerbotSocialChannelScopeReport const report = activity.TakeReport();
    EXPECT_EQ(report.reconciled, 25u);
    EXPECT_EQ(report.corrected, 0u);
}

/*
 * The diagnostic sample is bounded per interval rather than per correction.
 *
 * The question these lines exist to answer is which shape the drift takes, and a handful of examples
 * answers it as well as thousands would. Without a bound, the first sweep after a restart could log
 * a line for every stale membership on the server at once.
 */
TEST(PlayerbotSocialChannelScopeTest, TheDiagnosticSampleStopsAtItsBudgetWithinAnInterval)
{
    PlayerbotSocialChannelScopeActivity activity;

    std::size_t granted = 0;
    for (std::size_t attempt = 0; attempt < PLAYERBOT_SOCIAL_CHANNEL_SCOPE_DIAGNOSTIC_BUDGET * 3; ++attempt)
        if (activity.ClaimDiagnosticSlot())
            ++granted;

    EXPECT_EQ(granted, PLAYERBOT_SOCIAL_CHANNEL_SCOPE_DIAGNOSTIC_BUDGET);
}

/*
 * Refilled by the same read that resets the totals, so the sample tracks the reporting interval
 * rather than running dry for the life of the process.
 */
TEST(PlayerbotSocialChannelScopeTest, TheDiagnosticSampleRefillsWhenTheIntervalReportIsTaken)
{
    PlayerbotSocialChannelScopeActivity activity;

    while (activity.ClaimDiagnosticSlot())
        ;
    ASSERT_FALSE(activity.ClaimDiagnosticSlot());

    (void)activity.TakeReport();

    EXPECT_TRUE(activity.ClaimDiagnosticSlot());
}

/*
 * The classification the diagnostic exists to make.
 *
 * Two memberships of one channel id means core's break-on-first-match left a duplicate that no zone
 * change can drain. One means the bot simply holds the wrong zone's channel, which is a dropped
 * update rather than an undrainable one. The count is what separates them, so it is what gets
 * logged, and the two causes lead to different fixes.
 */
TEST(PlayerbotSocialChannelScopeTest, MembershipsOfOneChannelAreCountedApartFromOtherChannels)
{
    std::vector<PlayerbotSocialChannelMembership> const current = {
        {"General - Elwynn Forest", SCOPE_GENERAL},
        {"General - Westfall", SCOPE_GENERAL},
        {"LocalDefense - Westfall", SCOPE_LOCAL_DEFENSE},
        {"Trade - City", SCOPE_TRADE},
    };

    EXPECT_EQ(PlayerbotSocialCountMembershipsOfChannel(current, SCOPE_GENERAL), 2u);
    EXPECT_EQ(PlayerbotSocialCountMembershipsOfChannel(current, SCOPE_LOCAL_DEFENSE), 1u);
    EXPECT_EQ(PlayerbotSocialCountMembershipsOfChannel(current, SCOPE_WORLD_DEFENSE), 0u);
}
