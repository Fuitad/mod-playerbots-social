/*
 * This file is part of the mod-playerbots-social module.
 */

#include "Bot/Social/PlayerbotSocialChannelScope.h"

#include <algorithm>
#include <cstdio>
#include <map>

#include "Bot/Social/PlayerbotSocialConfig.h"
#include "Channel.h"
#include "ChannelMgr.h"
#include "DBCStores.h"
#include "Log.h"
#include "ObjectAccessor.h"
#include "Player.h"
#include "PlayerbotAI.h"
#include "Playerbots.h"
#include "Util/BroadcastHelper.h"

namespace
{
bool NameIsPresent(std::vector<std::string> const& names, std::string const& name)
{
    return std::find(names.begin(), names.end(), name) != names.end();
}

/*
 * A channel is the reconciler's business when its name carries the zone. Global channels name no
 * zone, and the city-scoped ones resolve to a constant "City" name that does not change as the bot
 * moves, so core's own note at PlayerUpdates.cpp:557 is that they are already stable. Reconciling
 * either would mean leaving a channel the bot is correctly on.
 */
bool ChannelIsZoneLocal(ChatChannelsEntry const* entry)
{
    return entry != nullptr && (entry->flags & CHANNEL_DBC_FLAG_GLOBAL) == 0 &&
           (entry->flags & CHANNEL_DBC_FLAG_CITY_ONLY) == 0;
}
}  // namespace

PlayerbotSocialChannelReconciliation PlayerbotSocialReconcileZoneChannels(
    std::vector<PlayerbotSocialChannelMembership> const& current, std::vector<std::string> const& expected,
    std::vector<uint32> const& zoneLocalChannelIds)
{
    PlayerbotSocialChannelReconciliation reconciliation;

    for (PlayerbotSocialChannelMembership const& membership : current)
    {
        // Authority first. A channel this reconciler was not given is not a candidate for anything,
        // so a WorldDefense or city-scoped membership is passed over before its name is considered.
        if (std::find(zoneLocalChannelIds.begin(), zoneLocalChannelIds.end(), membership.channelId) ==
            zoneLocalChannelIds.end())
            continue;

        if (NameIsPresent(expected, membership.name) || NameIsPresent(reconciliation.leave, membership.name))
            continue;

        reconciliation.leave.push_back(membership.name);
    }

    for (std::string const& name : expected)
    {
        bool const alreadyOn =
            std::any_of(current.begin(), current.end(), [&name](PlayerbotSocialChannelMembership const& membership)
                        { return membership.name == name; });
        if (alreadyOn || NameIsPresent(reconciliation.join, name))
            continue;

        reconciliation.join.push_back(name);
    }

    return reconciliation;
}

void PlayerbotSocialChannelScopeQueue::Mark(uint64 botGuidCounter)
{
    // Concurrent by nature: the zone hook runs on the map update threads, so several bots changing
    // zone on different maps land here at once.
    std::lock_guard<std::mutex> const guard(_lock);

    if (botGuidCounter == 0 || _order.size() >= PLAYERBOT_SOCIAL_CHANNEL_SCOPE_PENDING_MAX)
        return;

    // The set decides membership, the deque decides order. Marking a bot already waiting keeps its
    // original place rather than pushing it to the back, so a bot crossing borders repeatedly
    // cannot starve the ones queued behind it.
    if (!_pending.insert(botGuidCounter).second)
        return;

    _order.push_back(botGuidCounter);
}

std::vector<uint64> PlayerbotSocialChannelScopeQueue::Drain(std::size_t budget)
{
    std::lock_guard<std::mutex> const guard(_lock);

    std::vector<uint64> drained;
    drained.reserve(std::min(budget, _order.size()));

    while (!_order.empty() && drained.size() < budget)
    {
        uint64 const botGuidCounter = _order.front();
        _order.pop_front();
        _pending.erase(botGuidCounter);
        drained.push_back(botGuidCounter);
    }

    return drained;
}

std::size_t PlayerbotSocialChannelScopeQueue::PendingCount() const
{
    std::lock_guard<std::mutex> const guard(_lock);
    return _order.size();
}

bool PlayerbotSocialChannelScopeQueue::FullRescanDue(uint32 elapsedMs)
{
    std::lock_guard<std::mutex> const guard(_lock);

    _sinceRescanMs += elapsedMs;
    if (_sinceRescanMs < PLAYERBOT_SOCIAL_CHANNEL_SCOPE_RESCAN_MS)
        return false;

    _sinceRescanMs = 0;
    return true;
}

void PlayerbotSocialLeaveChannelCompletely(Player* bot, Channel* channel)
{
    if (bot == nullptr || channel == nullptr)
        return;

    channel->LeaveChannel(bot, false);
    bot->LeftChannel(channel);
}

bool PlayerbotSocialChannelScopeAcceptsBot(Player* bot)
{
    if (bot == nullptr || bot->GetSession() == nullptr)
        return false;

    // A selfbot is the human's own character under bot control; its channels are the client's to
    // manage, exactly as mod-playerbots skips it for the teleport acks at PlayerbotAI.cpp:794.
    if (GET_PLAYERBOT_AI(bot) == nullptr || IsRealPlayer(bot) || IsSelfBot(bot))
        return false;

    /*
     * Leaving a channel before the zone has settled crashes under mtmap, which is the reason core
     * updates the zone first at MovementHandler.cpp:271. A bot that fails this is not finished
     * with, it is simply not ready this tick.
     */
    return bot->IsInWorld() && !bot->IsBeingTeleported();
}

std::size_t PlayerbotSocialReconcileBotChannels(Player* bot)
{
    if (!PlayerbotSocialChannelScopeAcceptsBot(bot))
        return 0;

    AreaTableEntry const* const zone = sAreaTableStore.LookupEntry(bot->GetZoneId());
    ChannelMgr* const channelMgr = ChannelMgr::forTeam(bot->GetTeamId());
    if (zone == nullptr || channelMgr == nullptr)
        return 0;

    /*
     * Names are built exactly as mod-playerbots builds them when it joins at login
     * (PlayerbotMgr.cpp:640), same locale source and same helper. A name that differed by even one
     * byte would read as a stale membership every tick and the reconciler would churn forever.
     */
    uint8 const locale = BroadcastHelper::GetLocale();
    std::string const zoneName = PlayerbotAI::GetLocalizedAreaName(zone);

    std::vector<uint32> zoneLocalChannelIds;
    std::vector<std::string> expected;
    std::map<std::string, uint32> expectedChannelIds;

    for (uint32 row = 0; row < sChatChannelsStore.GetNumRows(); ++row)
    {
        ChatChannelsEntry const* const entry = sChatChannelsStore.LookupEntry(row);
        if (!ChannelIsZoneLocal(entry))
            continue;

        /*
         * The name pattern is resolved BEFORE authority is granted, and a row without one is passed
         * over entirely rather than merely skipped for joining. A DBC row need not carry a string
         * for every locale, so fall back to enUS the way PlayerbotAI::GetLocalizedAreaName does; if
         * even that is absent the correct channel name is unknowable, and claiming authority anyway
         * would leave every membership of this id including the one the bot should keep. Passing a
         * null format to snprintf is undefined behaviour besides.
         */
        char const* const pattern =
            entry->pattern[locale] != nullptr ? entry->pattern[locale] : entry->pattern[LOCALE_enUS];
        if (pattern == nullptr)
            continue;

        zoneLocalChannelIds.push_back(entry->ChannelID);

        // Authority over the channel does not imply the bot belongs on it here. A zone that forbids
        // the channel yields a leave with no matching join, which is the correct outcome.
        if (!bot->CanJoinConstantChannelInZone(entry, zone))
            continue;

        char name[100] = {};
        std::snprintf(name, sizeof(name), pattern, zoneName.c_str());
        expected.emplace_back(name);
        expectedChannelIds[name] = entry->ChannelID;
    }

    /*
     * Membership is read from the channel side, never from Player::m_channels. The player-side list
     * is the half that goes stale, and repairing it is the point of this function, so it cannot
     * also be the source of truth.
     */
    std::vector<PlayerbotSocialChannelMembership> current;
    for (auto const& [channelName, channel] : channelMgr->GetChannels())
    {
        (void)channelName;
        if (channel != nullptr && channel->IsOn(bot->GetGUID()))
            current.push_back({channel->GetName(), channel->GetChannelId()});
    }

    PlayerbotSocialChannelReconciliation const reconciliation =
        PlayerbotSocialReconcileZoneChannels(current, expected, zoneLocalChannelIds);

    for (std::string const& name : reconciliation.leave)
        if (Channel* const channel = channelMgr->GetChannel(name, bot, false))
            PlayerbotSocialLeaveChannelCompletely(bot, channel);

    for (std::string const& name : reconciliation.join)
    {
        auto const channelId = expectedChannelIds.find(name);
        if (channelId == expectedChannelIds.end())
            continue;

        // JoinChannel records the player side itself (Channel.cpp:197), so no second call here.
        if (Channel* const channel = channelMgr->GetJoinChannel(name, channelId->second))
            channel->JoinChannel(bot, "");
    }

    return reconciliation.leave.size();
}

namespace
{
PlayerbotSocialChannelScopeQueue& ScopeQueue()
{
    static PlayerbotSocialChannelScopeQueue queue;
    return queue;
}
}  // namespace

void PlayerbotSocialMarkChannelScope(uint64 botGuidCounter) { ScopeQueue().Mark(botGuidCounter); }

void PlayerbotSocialPumpChannelScope(uint32 diff)
{
    // The kill switch, checked before anything is drained or swept, so turning it off leaves every
    // membership exactly as it stands rather than half-reconciled.
    if (!sPlayerbotSocialConfig.socialChatChannelScopeEnable)
        return;

    PlayerbotSocialChannelScopeQueue& queue = ScopeQueue();

    /*
     * The backstop. Marks alone would miss any drift that never announced itself as a zone change,
     * which is precisely the part of this defect that stayed undiagnosed, so every online bot is
     * swept on an interval whether or not it was seen to move.
     */
    if (queue.FullRescanDue(diff))
        for (auto const& [guid, player] : ObjectAccessor::GetPlayers())
            if (PlayerbotSocialChannelScopeAcceptsBot(player))
                queue.Mark(guid.GetCounter());

    std::size_t corrected = 0;
    std::size_t reconciled = 0;
    for (uint64 const botGuidCounter : queue.Drain(PLAYERBOT_SOCIAL_CHANNEL_SCOPE_BUDGET))
    {
        Player* const bot = ObjectAccessor::FindPlayerByLowGUID(botGuidCounter);
        if (bot == nullptr)
            continue;  // Logged out between the mark and the drain; nothing to reconcile.

        // Not ready rather than not wanted: mid-teleport or not yet in world, so it goes back.
        if (!PlayerbotSocialChannelScopeAcceptsBot(bot))
        {
            queue.Mark(botGuidCounter);
            continue;
        }

        corrected += PlayerbotSocialReconcileBotChannels(bot);
        ++reconciled;
    }

    /*
     * Quiet once converged. A first sweep correcting many and later sweeps correcting none is the
     * signature that the accumulated backlog was real; a first sweep correcting none refutes it.
     */
    if (corrected > 0)
        LOG_INFO("playerbots", "Social channel scope: removed {} stale memberships across {} bots", corrected,
                 reconciled);
}
