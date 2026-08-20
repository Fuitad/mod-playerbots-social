/*
 * This file is part of the mod-playerbots-social module.
 */

#ifndef PLAYERBOTS_PLAYERBOTSOCIALCHANNELSCOPE_H
#define PLAYERBOTS_PLAYERBOTSOCIALCHANNELSCOPE_H

#include <cstddef>
#include <cstdint>
#include <deque>
#include <mutex>
#include <set>
#include <string>
#include <vector>

using uint32 = std::uint32_t;
using uint64 = std::uint64_t;

/*
 * How many bots one tick may reconcile, and how often every online bot is swept regardless of
 * whether a zone change was noticed. Sized against roughly 160 characters online: a full re-scan
 * costs at most seven ticks, and the interval is long enough that the sweep is a backstop rather
 * than the main path, which is the marked queue.
 */
constexpr std::size_t PLAYERBOT_SOCIAL_CHANNEL_SCOPE_BUDGET = 25;
constexpr uint32 PLAYERBOT_SOCIAL_CHANNEL_SCOPE_RESCAN_MS = 300000;

/*
 * A bound rather than an unbounded set. A bot crossing a zone border repeatedly, or a re-scan
 * queueing every online character, must not let this grow without limit; past the bound the marks
 * are dropped because the periodic re-scan will pick those bots up anyway.
 */
constexpr std::size_t PLAYERBOT_SOCIAL_CHANNEL_SCOPE_PENDING_MAX = 512;

/*
 * How many stale memberships one reporting interval describes in detail.
 *
 * The detail lines exist to answer one question, which shape the drift takes, and a handful of
 * examples answers it as well as thousands would. The first sweep after a restart sees every stale
 * membership on the server at once, so an unbounded sample would be a log flood exactly when the
 * server is busiest.
 */
constexpr std::size_t PLAYERBOT_SOCIAL_CHANNEL_SCOPE_DIAGNOSTIC_BUDGET = 8;

/*
 * One zone-local channel a bot currently belongs to.
 *
 * The name is the identity, not the channel object: ChannelMgr keys its map on the name, so one
 * zone's General is a wholly different channel from another's. The id is carried alongside only to
 * decide whether the reconciler has authority over it at all.
 */
struct PlayerbotSocialChannelMembership
{
    std::string name;
    uint32 channelId = 0;
};

struct PlayerbotSocialChannelReconciliation
{
    std::vector<std::string> leave;
    std::vector<std::string> join;
};

/*
 * How many of a bot's memberships belong to one channel id.
 *
 * This is the classification the drift diagnostic turns on. Two memberships of a single id means
 * core's break-on-first-match (PlayerUpdates.cpp:542) left behind a duplicate that no further zone
 * change can drain. One means the bot merely holds the wrong zone's channel, which is an update that
 * was dropped rather than one that cannot complete. The two have different causes and different
 * fixes, and the count is the only thing that separates them from outside.
 */
[[nodiscard]] std::size_t PlayerbotSocialCountMembershipsOfChannel(
    std::vector<PlayerbotSocialChannelMembership> const& current, uint32 channelId);

/*
 * What it takes to make a bot's zone-local memberships exactly those of the zone it is standing in.
 *
 * Every membership outside the expected set is returned, not merely the first one. Core's own
 * routine finds the first channel matching a given id and stops, which is why a bot that has moved
 * through several zones accumulates memberships no zone change can drain; reconciling one at a time
 * would inherit that defect. A channel whose id is absent from `zoneLocalChannelIds` is never
 * returned to leave, whatever its name looks like, because the global and city-scoped channels are
 * deliberately outside this function's authority.
 *
 * Leaves come back separately from joins so the caller can order them, leaving before joining, and
 * a name repeated in `current` collapses to a single entry in either list.
 */
[[nodiscard]] PlayerbotSocialChannelReconciliation PlayerbotSocialReconcileZoneChannels(
    std::vector<PlayerbotSocialChannelMembership> const& current, std::vector<std::string> const& expected,
    std::vector<uint32> const& zoneLocalChannelIds);

/*
 * Which bots are waiting to be reconciled, and when the next full sweep is due.
 *
 * Marking is separated from draining because the zone hook fires while the session is still loading,
 * a window in which every channel call is skipped by core's own gate. The mark is therefore recorded
 * there and acted on from the world tick, which is also the only phase with no map thread running.
 *
 * Every operation takes the lock, because marking and draining do NOT share a thread. The zone hook
 * reaches this from `Player::Update` (PlayerUpdates.cpp:300), which runs on the map update threads,
 * so several bots changing zone on different maps mark concurrently; the drain then runs on the
 * world tick. The channel work is what the world tick makes safe, never the bookkeeping that feeds
 * it, and an unsynchronized deque and set torn by four map threads is process corruption rather
 * than a missed reconciliation.
 */
class PlayerbotSocialChannelScopeQueue
{
public:
    // Ignored once the queue is at its bound; the periodic re-scan is what catches those bots.
    void Mark(uint64 botGuidCounter);

    // At most `budget` entries, removed from the queue. A bot that could not be reconciled is the
    // caller's to Mark again.
    [[nodiscard]] std::vector<uint64> Drain(std::size_t budget);

    // Advances the interval clock and reports whether a full sweep is due, restarting it when so.
    [[nodiscard]] bool FullRescanDue(uint32 elapsedMs);

    [[nodiscard]] std::size_t PendingCount() const;

private:
    mutable std::mutex _lock;
    std::deque<uint64> _order;
    std::set<uint64> _pending;
    uint32 _sinceRescanMs = 0;
};

struct PlayerbotSocialChannelScopeReport
{
    std::size_t reconciled = 0;
    std::size_t corrected = 0;
};

/*
 * What the reconciler did over one reporting interval.
 *
 * It exists so a sweep that corrects nothing is still visible. Reporting only corrections made a
 * converged server and a reconciler that never ran produce identical silence, which is exactly what
 * left the first live deployment impossible to verify from outside the process.
 */
class PlayerbotSocialChannelScopeActivity
{
public:
    void Record(std::size_t reconciled, std::size_t corrected);

    // Totals since the previous call, which are then cleared: each interval reports its own work
    // rather than an ever-growing running total. Also refills the diagnostic sample below, so the
    // sample tracks the reporting interval rather than running dry for the life of the process.
    [[nodiscard]] PlayerbotSocialChannelScopeReport TakeReport();

    // True at most PLAYERBOT_SOCIAL_CHANNEL_SCOPE_DIAGNOSTIC_BUDGET times per interval. Callers log
    // a detail line only when it is granted.
    [[nodiscard]] bool ClaimDiagnosticSlot();

private:
    mutable std::mutex _lock;
    PlayerbotSocialChannelScopeReport _pending;
    std::size_t _diagnosticsUsed = 0;
};

class Player;
class Channel;

/*
 * Remove a bot from a channel so that BOTH sides forget it.
 *
 * `Channel::LeaveChannel(bot, false)` alone is a half-removal: it calls `Player::LeftChannel` only
 * inside its `if (send)` branch (Channel.cpp:264), so the silent form clears the channel's member
 * store and leaves a stale pointer in the bot's own list. Core pairs the two calls for exactly this
 * reason at PlayerUpdates.cpp:597. The pair is named here so the reconciler cannot half-do it and so
 * the invariant is testable on its own.
 */
void PlayerbotSocialLeaveChannelCompletely(Player* bot, Channel* channel);

/*
 * Bring one bot's zone-local memberships in line with the zone it is standing in, and report how
 * many stale memberships had to be removed to get there.
 *
 * Call from `WorldScript::OnUpdate` and nowhere else. It manipulates shared Channel objects
 * without the `static std::mutex channelsLock` core holds for the same work, which is only safe
 * because the world tick runs after `MapMgr::Update` has joined every map thread. A per-player or
 * per-map caller reintroduces exactly the race that lock exists to prevent.
 *
 * Returns 0 for a bot it declines to touch: a real player, a selfbot, one not yet in world, one
 * mid-teleport, or one already correctly scoped.
 */
[[nodiscard]] std::size_t PlayerbotSocialReconcileBotChannels(Player* bot);

/*
 * Whether this bot is one the reconciler owns. Separated so the caller can requeue a bot it
 * declined for a transient reason rather than dropping it.
 */
[[nodiscard]] bool PlayerbotSocialChannelScopeAcceptsBot(Player* bot);

// Records that a bot's zone changed. Cheap and safe to call from a hook; does no channel work.
void PlayerbotSocialMarkChannelScope(uint64 botGuidCounter);

/*
 * Drains the marked bots within the per-tick budget and runs the periodic full sweep.
 *
 * `WorldScript::OnUpdate` only. See PlayerbotSocialReconcileBotChannels for why the phase is a
 * correctness requirement rather than a preference.
 */
void PlayerbotSocialPumpChannelScope(uint32 diff);

#endif
