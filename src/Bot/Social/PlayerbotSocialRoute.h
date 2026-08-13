/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef PLAYERBOTS_PLAYERBOTSOCIALROUTE_H
#define PLAYERBOTS_PLAYERBOTSOCIALROUTE_H

#include <array>
#include <cstddef>
#include <map>
#include <mutex>
#include <string>
#include <string_view>
#include <vector>

#include "Bot/PlayerbotAI.h"
#include "Bot/Social/PlayerbotSocialMgr.h"
#include "Bot/Social/PlayerbotSocialTypes.h"
#include "Util/BroadcastHelper.h"

/*
 * The boundary between AzerothCore chat and the social core.
 *
 * Everything the feature is allowed to read or write passes through one of the two routers below, so
 * the set of surfaces it can reach is decided in a single place rather than at each call site. Both
 * routers are total functions of their arguments and hold no state.
 *
 * Two rules shape the whole file.
 *
 * The gate is authoritative and defaults to off. With the gate off every router returns the legacy
 * decision, so an untouched configuration behaves exactly as it did before this feature existed.
 *
 * Routing fails closed. An unrecognized chat surface, an unrecognized broadcast destination, or a
 * malformed captured message is never admitted to the social core. This build compiles the module
 * without -Wswitch and without -Werror, so a chat surface added upstream would not be caught by the
 * compiler; it must be rejected by the default arm instead.
 */

// The density profile moved to PlayerbotSocialTypes.h, which is where the shared value contract
// lives, so the control boundary can name it without pulling in PlayerbotAI.h through this header.

// The runtime gate, read from configuration once and passed by value into every routing decision.
struct PlayerbotSocialGate
{
    bool enabled = false;
    PlayerbotSocialRolloutStage stage = PlayerbotSocialRolloutStage::HumanReplies;
    PlayerbotSocialDensityProfile density = PlayerbotSocialDensityProfile::Normal;
    uint32 telemetryRetentionHours = PLAYERBOT_SOCIAL_MIN_RETENTION_HOURS;

    // Live operator stop. Distinct from `enabled` above: that is the deployment's answer and this is
    // the operator's, and only the deployment's can turn the feature on.
    bool paused = false;

    // The budget governor's hard backstop; silences the feature exactly as a pause does, and only an
    // operator closes it.
    bool budgetCircuitOpen = false;

    /*
     * Which surfaces are still carrying, indexed by PlayerbotSocialChannel.
     *
     * An array rather than four named booleans because every use is "the channel this message came
     * in on", and four names invite a transposition that silences the wrong surface while every test
     * that checks one channel at a time still passes.
     */
    std::array<bool, PLAYERBOT_SOCIAL_CHANNEL_COUNT> channelEnabled = {true, true, true, true};
};

[[nodiscard]] PlayerbotSocialRolloutStage PlayerbotSocialParseRolloutStage(std::string_view text);
[[nodiscard]] char const* PlayerbotSocialRolloutStageName(PlayerbotSocialRolloutStage stage);

// PlayerbotSocialRuntimeControl lives in PlayerbotSocialTypes.h, with the other shared value
// contracts, so the manager can store one without including this routing header.

/*
 * The gate as the running server is configured, before any live control is applied.
 *
 * Every call site reads the effective gate through PlayerbotSocialEffectiveGate below rather than
 * this, so there is one place where the raw settings become a routing decision and one place the
 * stored control overrides it.
 */
[[nodiscard]] PlayerbotSocialGate PlayerbotSocialConfiguredGate();

/*
 * The control that represents "nothing has been changed yet", taken from the configuration in
 * effect.
 *
 * Used to create the stored row the first time an operator sends a control. Seeding from the
 * configuration and overlaying the result back is the identity, so the first control writes only the
 * value it names and leaves everything else exactly as the deployment had it.
 */
[[nodiscard]] PlayerbotSocialRuntimeControl PlayerbotSocialSeedRuntimeControl(PlayerbotSocialGate const& configured);

// Applies the operator's stored values over the deployment's. Total, and never widens what the
// configuration permits.
[[nodiscard]] PlayerbotSocialGate PlayerbotSocialOverlayRuntimeControl(PlayerbotSocialGate const& configured,
                                                                       PlayerbotSocialRuntimeControl const& control);

/*
 * The gate as it stands right now: the configuration with the stored control applied.
 *
 * This is what every caller should read. PlayerbotSocialConfiguredGate is the input to it and is
 * public only so the control layer can seed a first row from the deployment's own values.
 */
[[nodiscard]] PlayerbotSocialGate PlayerbotSocialEffectiveGate();

/*
 * Whether the feature may act at all: enabled by the deployment AND not stopped by an operator.
 *
 * Named rather than written out at each site because the two halves are easy to check singly and
 * wrong that way. A producer that tested only `enabled` would keep pumping conversation through a
 * pause, and the pause would appear to work only on the surfaces the router happens to cover.
 */
[[nodiscard]] bool PlayerbotSocialGateIsLive(PlayerbotSocialGate const& gate);

// Inbound: what to do with a chat message the bot just heard ----------------------------------------

enum class PlayerbotSocialInboundRoute : uint8
{
    /*
     * Hand the message to the legacy canned reply queue and do not tell the social core about it.
     * This is the gate off decision for every surface, and with the gate on it is what guild chat and
     * the two emote surfaces keep: a space the feature does not own yet, left exactly as it is.
     */
    LegacyOnly = 0,

    // A supported surface with the gate on: the legacy reply is skipped and the social core decides.
    SocialOpportunity,

    /*
     * A message the social core itself just delivered after its channel was silenced. It still
     * updates the thread it belongs to, because a delivered line is a real turn in the conversation,
     * but the operator's stop prevents it from opening another opportunity.
     */
    ThreadContinuationOnly,

    /*
     * Say nothing at all. With the gate on, World, Trade, Looking For Group, both defense channels,
     * guild recruitment, yell, raid, and battleground get neither social input nor a canned reply.
     *
     * Suppressing the outbound broadcast alone would not be enough: the reply path can speak on these
     * same surfaces on its own, and a stock answer to World chat is the nonfunctional chatter this
     * feature exists to replace, whichever direction it came from.
     *
     * A battleground reaches this route through the listener rather than the surface: the chat types
     * a battleground owns never get this far, but say, party, and the zone channels are still carried
     * inside one, and a bot fighting in a battleground is not somewhere this feature converses.
     */
    SuppressedSurface
};

inline constexpr std::size_t PLAYERBOT_SOCIAL_INBOUND_ROUTE_COUNT = 4;

[[nodiscard]] char const* PlayerbotSocialInboundRouteName(PlayerbotSocialInboundRoute route);

struct PlayerbotSocialInboundDecision
{
    PlayerbotSocialInboundRoute route = PlayerbotSocialInboundRoute::LegacyOnly;

    // Only meaningful when the route is not LegacyOnly.
    PlayerbotSocialChannel channel = PlayerbotSocialChannel::General;

    // True exactly when the legacy canned reply must not be queued for this message.
    bool suppressLegacyReply = false;
};

/*
 * Maps an AzerothCore chat surface onto the four channels the feature supports.
 *
 * `ChatChannelSource` is the module's own resolution of where a message came from, and it resolves a
 * custom channel by its channel id rather than by its localized name, so a localized "General - Elwynn
 * Forest" is recognized and a player made channel that happens to be named General is not.
 */
[[nodiscard]] bool PlayerbotSocialChannelFromChatSource(ChatChannelSource source, PlayerbotSocialChannel& channel);

[[nodiscard]] bool PlayerbotSocialIsFunctionalTraffic(PlayerbotAI* botAI, ChatChannelSource source, bool machineTraffic,
                                                      std::string const& message);

/*
 * What the router needs to know about the moment a message arrived, as opposed to the surface it
 * arrived on. These facts belong to the message or listening bot rather than to the channel, and all
 * default to the answer that admits the message, so a caller that names none gets ordinary chat heard
 * in an ordinary place. Naming them rather than passing bare booleans keeps a transposed call from
 * meaning the opposite of what it reads as.
 */
struct PlayerbotSocialInboundContext
{
    std::string eventPublicId;

    // True only for a message the feature itself just delivered, seen coming back through chat.
    bool originatedFromSocialDelivery = false;

    // True while the listening bot is inside a battleground.
    bool listenerInBattleground = false;

    // True for LANG_ADDON protocol payloads. Commands still consume them, but Social never does.
    bool machineTraffic = false;

    // True for a Playerbot command recognized without executing it. The command path still owns it.
    bool functionalTraffic = false;
};

[[nodiscard]] PlayerbotSocialInboundDecision PlayerbotSocialRouteInbound(ChatChannelSource source,
                                                                         PlayerbotSocialInboundContext const& context,
                                                                         PlayerbotSocialGate const& gate);

/*
 * Whether speech is conversational input rather than functional bot output.
 *
 * Human speech is always eligible on a supported surface. Bot speech is eligible only when the
 * Social provider produced it; combat status, commands, and other functional lines remain telemetry
 * only and never spend another provider request.
 */
[[nodiscard]] bool PlayerbotSocialSpeakerCanOpenOpportunity(bool speakerIsHuman, bool originatedFromSocialDelivery);

// Inbound: the normalized message ------------------------------------------------------------------

/*
 * One captured message, normalized to values on the world thread. Nothing here is a live game object,
 * so the record stays valid after a logout, a despawn, or an instance unload.
 */
struct PlayerbotSocialCapturedMessage
{
    std::string eventPublicId;
    std::string replyToEventPublicId;
    std::string sourceEventPublicId;
    PlayerbotSocialChannel channel = PlayerbotSocialChannel::General;
    uint64 speakerGuidCounter = 0;
    std::string speakerName;
    bool speakerIsHuman = false;

    // The addressed character. Set for a whisper, zero everywhere else.
    uint64 targetGuidCounter = 0;

    // The party the message was said in. Set for party, zero everywhere else.
    uint32 groupId = 0;

    // The zone the message was said in. Set for General and Say, zero everywhere else.
    uint32 zoneId = 0;

    // Process local identity of the exact characters who could hear this Say dispatch.
    uint64 sayCohortScopeId = 0;

    uint32 languageId = 0;

    /*
     * Whether the listening bot could actually hear the speaker. Say is a proximity surface, so a
     * message it could not have heard must not reach the social core. General is zone wide and party
     * and whisper are not positional, so this is always true for them.
     */
    bool withinHearingRange = true;

    uint64 atUnixSeconds = 0;

    /*
     * What was said. Carried only so the coordinator can recognise a line the thread has just heard,
     * and reduced to a hash the moment it gets there. Nothing downstream retains it, and nothing
     * validates it, because it is never interpreted: a line is compared for equality or not at all.
     */
    std::string text;
};

enum class PlayerbotSocialCaptureRejection : uint8
{
    None = 0,
    UnsupportedChannel,
    MissingSpeaker,
    MissingZone,
    MissingGroup,
    MissingTarget,
    UnexpectedTarget,
    SpeakerIsTarget,
    OutOfHearingRange,
    MissingSayCohort,

    /*
     * A character identifier too wide to be one. A whisper scope carries both of its characters
     * exactly (see below), which holds only while each identifier is the 32 bit counter the core
     * actually issues. A wider value is refused rather than folded down, because folding is what
     * would let two unrelated private conversations land in one scope.
     */
    IdentifierOutOfRange
};

inline constexpr std::size_t PLAYERBOT_SOCIAL_CAPTURE_REJECTION_COUNT = 11;

[[nodiscard]] char const* PlayerbotSocialCaptureRejectionName(PlayerbotSocialCaptureRejection rejection);

/*
 * Validates a captured message against the shape its channel requires. Returns the first failing
 * reason so a rejection always names one cause, matching how the opportunity gate reports itself.
 */
[[nodiscard]] PlayerbotSocialCaptureRejection PlayerbotSocialValidateCapture(
    PlayerbotSocialCapturedMessage const& captured);

// The widest character identifier a scope can carry: ObjectGuid::LowType is 32 bits, so an ordered
// pair of them is exactly the width of a scope id.
inline constexpr uint64 PLAYERBOT_SOCIAL_MAX_CHARACTER_COUNTER = 0xFFFFFFFFull;

/*
 * The conversation space a captured message belongs to.
 *
 * General is keyed by zone, Say by its exact hearing cohort, party by group, and a whisper by the
 * unordered pair of its two characters.
 */
[[nodiscard]] uint64 PlayerbotSocialScopeIdFor(PlayerbotSocialCapturedMessage const& captured);

/*
 * The scope for one whisper pair, in either order.
 *
 * The two identifiers are packed rather than hashed, so the guarantee above is structural instead of
 * probabilistic: a hash of the same width would leave a small but real chance of two unrelated pairs
 * sharing a scope, and that collision is not a lost message but one private conversation absorbing a
 * stranger's. Both arguments must be within PLAYERBOT_SOCIAL_MAX_CHARACTER_COUNTER, which validation
 * enforces before a captured whisper ever gets here.
 *
 * Because it is lossless it is also readable: it is an internal key, and anything that publishes a
 * scope outside the server has to derive an opaque identifier from it rather than expose it.
 */
[[nodiscard]] uint64 PlayerbotSocialWhisperScopeId(uint64 firstGuidCounter, uint64 secondGuidCounter);

class PlayerbotSocialSayCohortRegistry
{
public:
    [[nodiscard]] uint64 Resolve(std::vector<uint64> members, uint64 nowUnixSeconds);
    void Prune(uint64 nowUnixSeconds);

private:
    struct Entry
    {
        uint64 scopeId = 0;
        uint64 lastUsedUnixSeconds = 0;
    };

    void PruneLocked(uint64 nowUnixSeconds);

    std::mutex _mutex;
    std::map<std::vector<uint64>, Entry> _entries;
    uint64 _nextScopeId = 1;
};

[[nodiscard]] uint64 PlayerbotSocialResolveSayCohort(std::vector<uint64> members, uint64 nowUnixSeconds);

/*
 * Derives the coordinator observation for a captured message. The caller must have validated the
 * message first; an invalid one yields an observation the coordinator rejects rather than a
 * plausible looking one.
 */
[[nodiscard]] PlayerbotSocialObservation PlayerbotSocialObservationFor(PlayerbotSocialCapturedMessage const& captured);

/*
 * A starter may ground its selected listener only when the bot can identify that listener from the
 * surface itself. Party exposes its roster, and Say already requires a visible in range listener.
 * General proves only that somebody real is in the zone channel, so the selected audience remains
 * a delivery witness and never becomes provider evidence.
 */
[[nodiscard]] bool PlayerbotSocialStarterParticipantIsPerceivable(PlayerbotSocialChannel channel);

/*
 * One character considered for a request's immutable nearby snapshot.
 *
 * The live world is reduced to these booleans before selection. This keeps the privacy decision
 * executable without a Map and guarantees the provider receives display names rather than GUIDs.
 */
struct PlayerbotSocialNearbyCharacter
{
    uint64 characterGuidCounter = 0;
    std::string name;
    bool characterIsHuman = false;
    bool isObserver = false;
    bool sameMap = false;
    bool samePhase = false;
    bool visible = false;
    bool factionMatches = false;
    bool consented = false;
    bool sameZone = false;
    bool sameParty = false;
    bool channelMember = false;
    bool withinRange = false;
};

[[nodiscard]] std::vector<PlayerbotSocialNearbySnapshotEntry> PlayerbotSocialSelectNearby(
    PlayerbotSocialChannel channel, std::vector<PlayerbotSocialNearbyCharacter> const& characters);

/*
 * Normalizes a chat message the given bot just heard and hands it to the coordinator.
 *
 * This is the one place AzerothCore chat becomes a social observation: it resolves the speaker, the
 * zone, the group, and the hearing distance from live objects on the world thread and reduces them to
 * the value record above, so no caller holds a Player pointer past this call. Returns false when the
 * message was refused, which the caller may treat as "nothing further to do" rather than as an error.
 */
bool PlayerbotSocialCaptureChat(PlayerbotAI* botAI, PlayerbotSocialInboundDecision const& decision,
                                ObjectGuid speakerGuid, uint32 languageId, std::string_view message,
                                std::string_view eventPublicId = {}, uint64 sayCohortScopeId = 0,
                                std::string_view replyToEventPublicId = {}, std::string_view sourceEventPublicId = {});

/*
 * One chat dispatch, open from the outer chat callback until it returns.
 *
 * Capture runs once per bot that heard the message, because zone, group and hearing distance are
 * properties of the LISTENING bot. Activation must NOT run there. Handing responder selection a
 * single candidate would make alternates, second responders and cross candidate suppression
 * unreachable, and N bots each rolling the reply pressure independently answer at one minus (1-p) to
 * the N rather than at most twice. That amplification is exactly what the selection layer exists to
 * prevent, so per bot activation would quietly turn all of it into dead code.
 *
 * Capture therefore records candidates here, and activation runs once per conversation scope when
 * the outermost scope closes. Per scope rather than per message, because one line reaching three
 * zones is three unrelated conversations and each may legitimately answer.
 *
 * Scoped rather than a begin and end pair because the chat callbacks return early in several places
 * and one missed end would spill candidates into the next dispatch. World thread only, like
 * everything else the coordinator owns, which is what makes the unsynchronized collector safe.
 * Nesting is tolerated and only the outermost scope activates, so a callback reached from inside
 * another cannot flush a half collected field.
 */
class PlayerbotSocialDispatchScope
{
public:
    PlayerbotSocialDispatchScope();
    ~PlayerbotSocialDispatchScope();

    PlayerbotSocialDispatchScope(PlayerbotSocialDispatchScope const&) = delete;
    PlayerbotSocialDispatchScope& operator=(PlayerbotSocialDispatchScope const&) = delete;
    PlayerbotSocialDispatchScope(PlayerbotSocialDispatchScope&&) = delete;
    PlayerbotSocialDispatchScope& operator=(PlayerbotSocialDispatchScope&&) = delete;
};

/*
 * Whether a dispatch is open.
 *
 * Capture consults this rather than assuming one. The authoritative PlayerScript callback opens the
 * dispatch around the complete listener set. Later per-session packet echoes are outside it and are
 * ignored, so one game line cannot be observed again for every recipient.
 */
[[nodiscard]] bool PlayerbotSocialDispatchIsOpen();

/*
 * Observes the game message once for the whole listener fanout.
 *
 * A dispatch scope is one chat callback. Every bot listener reaches the capture path, but only the
 * first listener may advance the thread. Later listeners reuse that handle while contributing their
 * own activation candidate.
 */
[[nodiscard]] PlayerbotSocialThreadHandle PlayerbotSocialObserveOncePerDispatch(
    PlayerbotSocialObservation const& observation);

/*
 * Speaks every answer whose natural delay has elapsed, and abandons every request the provider never
 * answered. Driven from the world update, on the world thread.
 *
 * Characters are resolved HERE and nowhere earlier. A decided line waits out a delay of a second or
 * more, and inside that window a bot can log out, die, change map, leave the group or be erased, so
 * a pointer taken at decision time would be exactly the dangling pointer the project's own rule
 * forbids. Everything upstream carries GUID counters and this is where they become characters again.
 *
 * A refusal here is a completed request rather than a retried one: the coordinator consumes the
 * result either way, because re-offering it later would deliver into a conversation that has moved
 * on, which is the stale delivery the whole revalidation path exists to prevent.
 */
void PlayerbotSocialDeliverDue();

/*
 * How much starter work one world tick may do.
 *
 * The number of scopes is set by the world, so an unbounded pass would scale a single tick with the
 * whole server. Each scope resolves exactly one authoritative source bot.
 *
 * Excess is deferred rather than dropped. A scope is served only by CONSUMING its starters, so a
 * served scope leaves the pending set and the next tick reaches the ones behind it. That is the
 * fairness mechanism: no cursor is kept, because consumption already rotates the queue.
 */
inline constexpr std::size_t PLAYERBOT_SOCIAL_STARTER_SCOPES_PER_TICK = 2;

/*
 * Lets bots open a conversation about something that happened, rather than only answer one.
 *
 * The source context already names the only bot authorized to speak about the event. The pump
 * resolves that bot at use time and stays silent if it is no longer an eligible participant in the
 * retained scope.
 *
 * Runs on the world update beside the delivery pump, and for the same reason: it resolves characters
 * and hands GUID counters back to the coordinator, both of which are world thread only.
 */
void PlayerbotSocialPumpStarters();

// How many warm pairs one whisper scan considers. A bound on the walk, not a fairness guarantee:
// the per-pair cooldown is what rotates attention across pairs between scans.
inline constexpr std::size_t PLAYERBOT_SOCIAL_WHISPER_SCAN_PAIR_LIMIT = 64;

// Relationship-driven whisper check-ins. Runs only in the autonomous_society stage, on a slow scan,
// rationed per pair by the configured cooldown.
void PlayerbotSocialPumpWhisperStarters();

/*
 * Records one typed authoritative gameplay source and queues it only when a real current audience
 * exists on Say, Party, or General. The source event is recorded before the starter enters the
 * coordinator. Rendered legacy broadcast text never crosses this seam.
 */
[[nodiscard]] bool PlayerbotSocialQueueStarterSource(PlayerbotAI* sourceAI, PlayerbotSocialStarterSource source);

/*
 * Whether one collected opportunity goes through the roleplay assessment before ordinary
 * activation. Only an observed HUMAN reply on a live thread does: starters stay ordinary, and a
 * bot's line is never classified because bot initiated roleplay is outside this slice entirely.
 */
[[nodiscard]] bool PlayerbotSocialOpportunityRequiresAssessment(PlayerbotSocialActivation const& activation);

/*
 * How often the biography pump looks at all, and how many bots it may ask about in one pass.
 *
 * Biography generation is one time and lazy, so this is deliberately slow. A bot that has no
 * player profile today will have one within the hour. Anything faster would spend provider requests
 * competing with the lines players are actually waiting on.
 */
inline constexpr uint32 PLAYERBOT_SOCIAL_BIOGRAPHY_PUMP_INTERVAL_MS = 60000;
inline constexpr std::size_t PLAYERBOT_SOCIAL_BIOGRAPHY_REQUESTS_PER_PASS = 1;

/*
 * Asks for one bot's player profile, and abandons requests the provider never answered.
 *
 * Runs on the world update beside the database work rather than from the starter pump or from
 * login. It reads a character's authoritative name, race, class and gender, which is world thread
 * only, and it is the only thing that makes the biography path reachable in production at all.
 *
 * Login deliberately does not trigger it. A bot logging in says nothing about whether a generation
 * is due, and tying the two would make every login storm a burst of provider requests.
 */
void PlayerbotSocialPumpBiographies(uint32 diff);

// Recording what was actually delivered --------------------------------------------------------------

/*
 * Records one delivery the world accepted, under the origin its producer states.
 *
 * The gate is checked here rather than at each producer, so a functional announcement instrumented
 * for the feed stays completely inert with the feature off. Producers call this unconditionally and
 * do not read the gate themselves; a producer that had to remember to would eventually forget.
 *
 * Only supported surfaces are recorded, per Key Decision 6. A raid, guild or world line is refused
 * here rather than at the caller, so a producer cannot file an unsupported channel into the Social
 * feed by getting its own check wrong.
 *
 * Call this ONLY after the send succeeded. A refused send reached nobody and produces no event.
 */
void PlayerbotSocialRecordDelivery(PlayerbotSocialDelivery const& delivery);

// Speaking and recording as one act -----------------------------------------------------------------

/*
 * What a producer states about one line before the world is asked to carry it.
 *
 * Scalars only, and no pointers. The record a delivery becomes has to be decidable without a world,
 * because that is what makes the rule below testable at all; the caller resolves the bot and the
 * target, and the helper fills the parts that only a live character can answer.
 *
 * `languageId` is the CALLER's language and is never a faction default. Most direct producers speak
 * `LANG_UNIVERSAL` while `PlayerbotAI::Say` and `PlayerbotAI::Whisper` pick `LANG_COMMON` or
 * `LANG_ORCISH` by team, so a helper that chose for them would silently change which characters can
 * understand a line. That is a gameplay change, and this task is forbidden to make one.
 */
struct PlayerbotSocialDeliveryRequest
{
    std::string eventPublicId;
    std::string replyToEventPublicId;
    std::string sourceEventPublicId;
    PlayerbotSocialChannel channel = PlayerbotSocialChannel::Say;

    /*
     * Stated, never inferred. A scoped ambient origin is bypassed by direct callers, inherited
     * wrongly by nested calls, and describes the world thread rather than the individual delivery.
     */
    PlayerbotSocialEventOrigin origin = PlayerbotSocialEventOrigin::Legacy;

    uint32 languageId = 0;
    std::string text;

    // Empty for functional output, which belongs to no conversation.
    std::string threadPublicId;

    bool isEmote = false;
    uint32 emoteId = 0;

    // Present only for provider backed Social output. Direct and functional producers leave it
    // absent, so their existing delivery diagnostics remain unchanged.
    std::optional<PlayerbotSocialCallMetadata> callMetadata;
    std::optional<PlayerbotSocialOperatorEvidence> operatorEvidence;

    // False only for the stateless direct reply an opted-out human explicitly requested. The world
    // still carries the line, but no event about that character enters the Social telemetry queue.
    bool retainTelemetry = true;

    // Zero means "stamp it now". The delivery pump passes the tick's own clock so a line and its
    // echo record agree to the second.
    uint64 occurredAtUnixSeconds = 0;
};

// What only a live character can answer, resolved by the caller and handed to the pure rule below.
struct PlayerbotSocialSpeaker
{
    uint64 botGuidCounter = 0;

    // The addressed character, for a whisper. Zero on every surface addressed to a room.
    uint64 targetGuidCounter = 0;

    // Where the line was SPOKEN. Read at delivery rather than carried from the decision, because a
    // bot can move during a natural delay and the feed wants the place a player heard it.
    uint32 zoneId = 0;
};

/*
 * Whether an attempted delivery is one the feed carries, and what it should say.
 *
 * Pure, and the whole reason the helper below is split in two: a refused send reached nobody, and
 * the rule that it produces no event has to be provable without a world to refuse anything in.
 *
 * Returns false when nothing is to be recorded, leaving `record` untouched.
 */
[[nodiscard]] bool PlayerbotSocialDeliveryRecordFor(PlayerbotSocialDeliveryRequest const& request,
                                                    PlayerbotSocialSpeaker const& speaker, bool accepted,
                                                    uint64 nowUnixSeconds, PlayerbotSocialDelivery& record);

/*
 * The language a bot's own voice uses, by team.
 *
 * One function because the same ternary was written out at every site that speaks in the bot's own
 * voice, and two of those sites are wrappers this task routes through the helper. A caller that
 * means `LANG_UNIVERSAL` still passes `LANG_UNIVERSAL`: this is the faction voice, not a default.
 */
[[nodiscard]] uint32 PlayerbotSocialSpokenLanguageFor(TeamId team);

/*
 * Speaks one line and records it, as one act.
 *
 * Key Decision 1. Instrumenting each of the sites that bypass the `PlayerbotAI` wrappers by hand
 * would leave the same rule in fifty nine places, and the copy that drifts is the one nobody
 * re-reads. Every supported surface delivery goes through here instead, so a producer cannot speak
 * without recording or record without speaking.
 *
 * Returns whether the world took the send. The result is the same one the underlying send reported,
 * so a caller that branches on it keeps branching on exactly what it did before.
 */
bool PlayerbotSocialDeliver(Player* bot, Player* target, PlayerbotSocialDeliveryRequest const& request);

// The two shapes almost every direct producer needs, so a one line send stays a one line send.
bool PlayerbotSocialSay(Player* bot, std::string const& text, uint32 languageId, PlayerbotSocialEventOrigin origin);
bool PlayerbotSocialWhisper(Player* bot, std::string const& text, uint32 languageId, Player* target,
                            PlayerbotSocialEventOrigin origin);

/*
 * Maps a chat packet's own message type onto the four channels the feature supports.
 *
 * A second mapping beside `PlayerbotSocialChannelFromChatSource` because the two answer different
 * questions from different inputs: that one resolves where a message the bot HEARD came from, this
 * one names the surface a packet the bot is about to SEND will arrive on. The producers that build
 * their own packet know the `ChatMsg` and never compute a `ChatChannelSource`.
 *
 * Fails closed, like every other routing decision here. An addon message, a channel message whose
 * channel this cannot identify, and every unsupported surface return false.
 */
[[nodiscard]] bool PlayerbotSocialChannelFromChatMsg(ChatMsg type, PlayerbotSocialChannel& channel);

/*
 * Delivers one line to a single character by building the chat packet directly, and records it.
 *
 * A second canonical entry point because `PlayerbotAI` answers its owner this way rather than
 * through `Player::Whisper`: the reply has to arrive on the surface the owner is already using,
 * which `Player::Whisper` would force to a whisper. Both entry points share the same record rule and
 * the same gate, so there is still one answer to "what does the feed get told".
 *
 * The caller states the message type and language it would have built the packet with, because the
 * two producers differ in how they treat an addon message and neither substitution belongs here.
 * A type that names no supported surface is still SENT and simply not recorded, per Key Decision 6.
 */
bool PlayerbotSocialDeliverDirect(Player* bot, Player* target, ChatMsg type, uint32 languageId, std::string const& text,
                                  PlayerbotSocialEventOrigin origin);

// Recognising the feature's own delivered lines ----------------------------------------------------

/*
 * How long a delivered line stays recognisable as it echoes back through chat.
 *
 * The echo arrives on the same world tick or the next one, so this is generous rather than tuned.
 * It is deliberately short: a record that outlived the echo would suppress a bot genuinely repeating
 * itself later, which is a real remark being silently dropped.
 */
inline constexpr uint64 PLAYERBOT_SOCIAL_DELIVERY_ECHO_WINDOW_SECONDS = 5;

/*
 * Records that the feature just made this bot say this, so the same line can be recognised when it
 * comes back through the chat hooks.
 *
 * The authoritative chat callback consumes this marker once and applies the same generated-line
 * fact to its complete listener fanout. The line remains eligible for one reply opportunity, while
 * ordinary cooldown and consecutive-bot-turn decay stop the conversation naturally.
 */
/*
 * The most delivered lines one bot can have awaiting their echo.
 *
 * What actually bounds this is the pending request cap, not the delivery delay. An earlier version
 * of this constant divided the echo window by the minimum delay on the assumption that the delay
 * spaces a bot's deliveries apart. It does not: each result is given its own due time, and one pass
 * of the pump returns every request that is due, so a bot holding two answers can speak both in the
 * same pass with no spacing at all. The cap on requests is therefore the real ceiling on lines in
 * flight, and the reply cooldown keeps a bot from refilling within the window.
 *
 * Kept deliberately above that ceiling. The cost of overshooting is a few bytes; the cost of
 * undershooting is an evicted record, which lets the bot's own line be answered as though a stranger
 * had said it, and that is the failure this whole mechanism exists to prevent.
 */
inline constexpr std::size_t PLAYERBOT_SOCIAL_MAX_UNECHOED_LINES_PER_BOT = PLAYERBOT_SOCIAL_MAX_PENDING_PER_BOT + 2;

void PlayerbotSocialRememberDeliveredLine(uint64 botGuidCounter, std::string const& text, uint64 nowUnixSeconds,
                                          std::string const& eventPublicId = {},
                                          std::string const& replyToEventPublicId = {},
                                          std::string const& sourceEventPublicId = {});

/*
 * When this bot last spoke socially, or zero if it has not within the reply cooldown.
 *
 * The opportunity gate refuses a bot that answered too recently, and it reads this. Without it the
 * gate compares against zero, which is always outside the cooldown, so the cooldown never fires and
 * a bot answers every message it hears.
 */
[[nodiscard]] uint64 PlayerbotSocialLastSpokeAt(uint64 botGuidCounter);

// Records that this bot just spoke socially. Called only on a send the world accepted.
void PlayerbotSocialRememberSpoke(uint64 botGuidCounter, uint64 nowUnixSeconds);

/*
 * Whether this exact line from this exact bot is one the feature just delivered. CONSUMES the match:
 * one delivery suppresses one echo, so a later genuine repetition is still a real opportunity.
 *
 * Matched on the bot AND the text. Matching on the bot alone would swallow the next thing that bot
 * said of its own accord.
 */
[[nodiscard]] bool PlayerbotSocialWasDeliveredLine(uint64 botGuidCounter, std::string const& text,
                                                   uint64 nowUnixSeconds, std::string* eventPublicId = nullptr,
                                                   std::string* replyToEventPublicId = nullptr,
                                                   std::string* sourceEventPublicId = nullptr);

// Drops every remembered line. For tests and for shutdown.
void PlayerbotSocialForgetDeliveredLines();

// Outbound: what to do with a canned broadcast ------------------------------------------------------

enum class PlayerbotSocialBroadcastRoute : uint8
{
    // Gate off: deliver the canned line exactly as before.
    DeliverAsToday = 0,

    /*
     * Gate on, non General destination: the canned line is dropped and nothing replaces it. These are
     * server wide or cross zone surfaces that this feature is not allowed to speak on at all.
     */
    SuppressCannedDelivery,

    /*
     * Gate on, General: the canned line is dropped and its subject becomes context a bot may open a
     * real conversation from. This is the only destination that converts rather than suppresses.
     */
    StarterContext
};

inline constexpr std::size_t PLAYERBOT_SOCIAL_BROADCAST_ROUTE_COUNT = 3;

[[nodiscard]] char const* PlayerbotSocialBroadcastRouteName(PlayerbotSocialBroadcastRoute route);

/*
 * Routes one destination of a canned broadcast.
 *
 * This covers only the ambient broadcast funnel: loot, quest, kill, level, and suggestion lines. The
 * guild lifecycle announcements do not pass through the funnel at all, so promotion, demotion, and
 * group or raid invitations are preserved by construction and can never be converted into social
 * context by any value this function returns.
 */
[[nodiscard]] PlayerbotSocialBroadcastRoute PlayerbotSocialRouteBroadcast(BroadcastHelper::ToChannel destination,
                                                                          PlayerbotSocialGate const& gate);

#endif  // PLAYERBOTS_PLAYERBOTSOCIALROUTE_H
