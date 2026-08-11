/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#include "Bot/Social/PlayerbotSocialRoute.h"

#include <algorithm>
#include <cctype>
#include <ctime>
#include <map>
#include <vector>

#include "Bot/Personality/PlayerbotPersonalityMgr.h"
#include "Bot/Social/PlayerbotSocialConfig.h"
#include "Bot/Social/PlayerbotSocialMgr.h"
#include "Bot/Social/PlayerbotSocialPersonality.h"
#include "Channel.h"
#include "ChannelMgr.h"
#include "DBCStores.h"
#include "DBCStructure.h"
#include "Group.h"
#include "Map.h"
#include "ObjectAccessor.h"
#include "Playerbots.h"
#include "Random.h"
#include "SharedDefines.h"
#include "World.h"

namespace
{
/*
 * The channel a refused capture carries. It is outside the four supported values on purpose: the
 * coordinator's own validity check is what turns it into a refusal, so the rejection is enforced
 * by the same guard that protects against a corrupt payload rather than by a second convention.
 */
constexpr PlayerbotSocialChannel REFUSED_CHANNEL = static_cast<PlayerbotSocialChannel>(0xFF);

void CaptureGeneratedAudience(Player* speaker, PlayerbotSocialDeliveryRequest const& request);

std::string LocalizedName(char const* const* names)
{
    LocaleConstant const locale = sWorld->GetDefaultDbcLocale();
    if (names[locale] != nullptr && names[locale][0] != '\0')
        return names[locale];
    if (names[LOCALE_enUS] != nullptr)
        return names[LOCALE_enUS];
    return {};
}

std::string RaceName(uint8 race)
{
    ChrRacesEntry const* const entry = sChrRacesStore.LookupEntry(race);
    return entry == nullptr ? std::string() : LocalizedName(entry->name);
}

std::string ClassName(uint8 characterClass)
{
    ChrClassesEntry const* const entry = sChrClassesStore.LookupEntry(characterClass);
    return entry == nullptr ? std::string() : LocalizedName(entry->name);
}

std::string AreaName(uint32 areaId)
{
    AreaTableEntry const* const entry = sAreaTableStore.LookupEntry(areaId);
    return entry == nullptr ? std::string() : LocalizedName(entry->area_name);
}

std::string FactionName(Player const* player)
{
    if (player == nullptr)
        return {};
    return player->GetTeamId() == TEAM_ALLIANCE ? "alliance" : "horde";
}

PlayerbotSocialCharacterFacts CaptureCharacterFacts(Player const* character)
{
    PlayerbotSocialCharacterFacts facts;
    if (character == nullptr)
        return facts;

    facts.guidCounter = character->GetGUID().GetCounter();
    facts.name = character->GetName();
    facts.race = RaceName(character->getRace());
    facts.characterClass = ClassName(character->getClass());
    facts.level = character->GetLevel();
    facts.faction = FactionName(character);
    facts.zone = AreaName(character->GetZoneId());
    facts.area = AreaName(character->GetAreaId());
    facts.inCombat = character->IsInCombat();
    return facts;
}

void CaptureGroundingRelations(PlayerbotSocialGroundingInput& input, Player* bot, Player* participant)
{
    if (bot != nullptr && participant != nullptr)
    {
        bool const sameMap = bot->GetMap() != nullptr && bot->GetMap() == participant->GetMap();
        bool const samePhase = sameMap && (bot->GetPhaseMask() & participant->GetPhaseMask()) != 0;
        bool const visible = samePhase && bot->CanSeeOrDetect(participant);

        input.participant = CaptureCharacterFacts(participant);
        input.participant.visible = visible;
        input.participant.inRange =
            sameMap && bot->IsWithinDistInMap(participant, sWorld->getFloatConfig(CONFIG_LISTEN_RANGE_SAY));

        Group const* const botGroup = bot->GetGroup();
        input.bot.groupRelation =
            botGroup != nullptr && botGroup == participant->GetGroup() ? "same_party" : "not_same_party";
        uint32 const botGuildId = bot->GetGuildId();
        input.bot.guildRelation =
            botGuildId != 0 && botGuildId == participant->GetGuildId() ? "same_guild" : "not_same_guild";
    }

    if (bot != nullptr && !bot->GetTarget().IsEmpty())
        if (Unit* const target = ObjectAccessor::GetUnit(*bot, bot->GetTarget());
            target != nullptr && bot->CanSeeOrDetect(target))
            input.bot.visibleTarget = target->GetName();
}

PlayerbotSocialGroundingEnvelope CaptureCurrentGroundingEnvelope(Player* bot, Player* participant,
                                                                 PlayerbotSocialChannel channel, uint64 nowUnixSeconds)
{
    PlayerbotSocialGroundingInput input;
    input.bot = CaptureCharacterFacts(bot);
    input.evidenceScope = PlayerbotSocialChannelPrivacyScope(channel);
    input.activeContentExpansion = PlayerbotSocialActiveContentExpansion();
    input.nowUnixSeconds = nowUnixSeconds;
    CaptureGroundingRelations(input, bot, participant);
    return PlayerbotSocialBuildGroundingEnvelope(input);
}

PlayerbotSocialGroundingEnvelope CaptureGroundingEnvelope(Player* bot, Player* participant,
                                                          PlayerbotSocialThreadHandle const& thread,
                                                          PlayerbotSocialChannel channel,
                                                          PlayerbotSocialProfileLoadState profileLoadState,
                                                          uint64 nowUnixSeconds, std::string const& starterSubject = {})
{
    PlayerbotSocialGroundingInput input;
    input.bot = CaptureCharacterFacts(bot);
    input.profileLoadState = profileLoadState;
    input.memoryInputState = sPlayerbotSocialMgr.MemoryInputStateFor(input.bot.guidCounter, channel);
    input.evidenceScope = PlayerbotSocialChannelPrivacyScope(channel);
    input.activeContentExpansion = PlayerbotSocialActiveContentExpansion();
    input.nowUnixSeconds = nowUnixSeconds;
    input.transcriptEventPublicIds = sPlayerbotSocialMgr.RecentEventIdsOf(thread);
    Player* groundingParticipant = participant;
    if (!starterSubject.empty() && !PlayerbotSocialStarterParticipantIsPerceivable(channel))
        groundingParticipant = nullptr;
    CaptureGroundingRelations(input, bot, groundingParticipant);

    if (!starterSubject.empty() && input.bot.guidCounter != 0)
    {
        PlayerbotSocialEvidenceEntry source;
        source.subjectRole = PlayerbotSocialEvidenceSubjectRole::Source;
        source.subjectGuidCounter = input.bot.guidCounter;
        source.factKind = PlayerbotSocialEvidenceFactKind::Objective;
        source.value = starterSubject;
        source.provenance = PlayerbotSocialEvidenceProvenance::AuthoritativeSource;
        source.scope = input.evidenceScope;
        source.atUnixSeconds = nowUnixSeconds;
        input.sourceFacts.push_back(std::move(source));
    }

    return PlayerbotSocialBuildGroundingEnvelope(input);
}

// Trimmed and lowered so a configuration file may write Quiet, quiet, or " QUIET " and mean the
// same profile. Only ASCII space is trimmed; a profile name has no other legal whitespace.
std::string NormalizeProfileText(std::string_view text)
{
    std::size_t begin = 0;
    std::size_t end = text.size();
    while (begin < end && std::isspace(static_cast<unsigned char>(text[begin])))
        ++begin;
    while (end > begin && std::isspace(static_cast<unsigned char>(text[end - 1])))
        --end;

    std::string normalized;
    normalized.reserve(end - begin);
    for (std::size_t index = begin; index < end; ++index)
        normalized.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(text[index]))));

    return normalized;
}

std::vector<PlayerbotSocialNearbySnapshotEntry> CaptureNearbySnapshot(Player const* observer,
                                                                      PlayerbotSocialChannel channel)
{
    if (observer == nullptr || observer->GetMap() == nullptr || channel == PlayerbotSocialChannel::Whisper)
        return {};

    std::vector<PlayerbotSocialNearbyCharacter> characters;
    characters.reserve(PLAYERBOT_SOCIAL_CONTEXT_ENTRIES);

    Map::PlayerList const& mapPlayers = observer->GetMap()->GetPlayers();
    for (Map::PlayerList::const_iterator player = mapPlayers.begin(); player != mapPlayers.end(); ++player)
    {
        Player* const candidate = player->GetSource();
        if (candidate == nullptr)
            continue;

        PlayerbotSocialNearbyCharacter facts;
        facts.characterGuidCounter = candidate->GetGUID().GetCounter();
        facts.name = candidate->GetName();
        facts.characterIsHuman = GET_PLAYERBOT_AI(candidate) == nullptr;
        facts.isObserver = candidate == observer;
        facts.sameMap = candidate->GetMap() == observer->GetMap();
        facts.samePhase = (candidate->GetPhaseMask() & observer->GetPhaseMask()) != 0;
        facts.visible = observer->CanSeeOrDetect(candidate);
        facts.factionMatches = candidate->GetTeamId() == observer->GetTeamId();
        facts.consented = !facts.characterIsHuman || !sPlayerbotSocialMgr.IsOptedOut(candidate->GetGUID().GetCounter());
        facts.sameZone = candidate->GetZoneId() == observer->GetZoneId();
        facts.sameParty = observer->GetGroup() != nullptr && candidate->GetGroup() == observer->GetGroup();
        if (ChannelMgr* channelMgr = ChannelMgr::forTeam(candidate->GetTeamId()))
            for (auto const& channelEntry : channelMgr->GetChannels())
            {
                Channel* const joined = channelEntry.second;
                if (joined != nullptr && joined->GetChannelId() == ChatChannelId::GENERAL &&
                    candidate->IsInChannel(joined))
                {
                    facts.channelMember = true;
                    break;
                }
            }
        facts.withinRange =
            facts.sameMap && observer->IsWithinDistInMap(candidate, sWorld->getFloatConfig(CONFIG_LISTEN_RANGE_SAY));
        characters.push_back(std::move(facts));
    }

    return PlayerbotSocialSelectNearby(channel, characters);
}
}  // namespace

PlayerbotSocialDensityProfile PlayerbotSocialParseDensityProfile(std::string_view text)
{
    std::string const normalized = NormalizeProfileText(text);

    if (normalized == "quiet")
        return PlayerbotSocialDensityProfile::Quiet;
    if (normalized == "lively")
        return PlayerbotSocialDensityProfile::Lively;

    // Everything else, including an empty or misspelled value, resolves to the middle profile.
    return PlayerbotSocialDensityProfile::Normal;
}

bool PlayerbotSocialStarterParticipantIsPerceivable(PlayerbotSocialChannel channel)
{
    return channel == PlayerbotSocialChannel::Party || channel == PlayerbotSocialChannel::Say;
}

PlayerbotSocialRolloutStage PlayerbotSocialParseRolloutStage(std::string_view text)
{
    std::string const normalized = NormalizeProfileText(text);
    if (normalized == "grounded_presence")
        return PlayerbotSocialRolloutStage::GroundedStarters;
    if (normalized == "bounded_continuation")
        return PlayerbotSocialRolloutStage::BoundedContinuation;
    return PlayerbotSocialRolloutStage::HumanReplies;
}

char const* PlayerbotSocialRolloutStageName(PlayerbotSocialRolloutStage stage)
{
    switch (stage)
    {
        case PlayerbotSocialRolloutStage::HumanReplies:
            return "human_replies";
        case PlayerbotSocialRolloutStage::GroundedStarters:
            return "grounded_presence";
        case PlayerbotSocialRolloutStage::BoundedContinuation:
            return "bounded_continuation";
    }

    return "unknown";
}

char const* PlayerbotSocialDensityProfileName(PlayerbotSocialDensityProfile profile)
{
    switch (profile)
    {
        case PlayerbotSocialDensityProfile::Quiet:
            return "quiet";
        case PlayerbotSocialDensityProfile::Normal:
            return "normal";
        case PlayerbotSocialDensityProfile::Lively:
            return "lively";
    }

    return "unknown";
}

PlayerbotSocialGate PlayerbotSocialConfiguredGate()
{
    PlayerbotSocialGate gate;
    gate.enabled = sPlayerbotSocialConfig.socialChatEnable;
    gate.stage = PlayerbotSocialParseRolloutStage(sPlayerbotSocialConfig.socialChatStage);
    gate.density = PlayerbotSocialParseDensityProfile(sPlayerbotSocialConfig.socialChatDensity);
    gate.telemetryRetentionHours =
        PlayerbotSocialNormalizeRetentionHours(sPlayerbotSocialConfig.socialChatTelemetryRetentionHours);
    return gate;
}

PlayerbotSocialRuntimeControl PlayerbotSocialSeedRuntimeControl(PlayerbotSocialGate const& configured)
{
    PlayerbotSocialRuntimeControl control;
    control.paused = configured.paused;
    control.density = configured.density;
    control.channelEnabled = configured.channelEnabled;
    return control;
}

PlayerbotSocialGate PlayerbotSocialEffectiveGate()
{
    PlayerbotSocialGate const configured = PlayerbotSocialConfiguredGate();

    /*
     * The configuration alone until the stored controls have been read. Overlaying before then would
     * apply a DEFAULT rather than an answer, quietly replacing a configured density of quiet with the
     * middle profile for the length of startup.
     */
    if (!sPlayerbotSocialMgr.RuntimeControlLoaded())
        return configured;

    return PlayerbotSocialOverlayRuntimeControl(configured, sPlayerbotSocialMgr.RuntimeControl());
}

bool PlayerbotSocialGateIsLive(PlayerbotSocialGate const& gate) { return gate.enabled && !gate.paused; }

PlayerbotSocialGate PlayerbotSocialOverlayRuntimeControl(PlayerbotSocialGate const& configured,
                                                         PlayerbotSocialRuntimeControl const& control)
{
    /*
     * Starts from the configuration and replaces only what an operator owns, rather than building a
     * gate from the control and copying the configuration back in. The difference matters when a
     * field is added later: this way a new configuration field is carried by default and has to be
     * deliberately exposed to a control, instead of being silently dropped.
     */
    PlayerbotSocialGate gate = configured;
    gate.paused = control.paused;
    gate.density = control.density;
    gate.channelEnabled = control.channelEnabled;
    return gate;
}

char const* PlayerbotSocialInboundRouteName(PlayerbotSocialInboundRoute route)
{
    switch (route)
    {
        case PlayerbotSocialInboundRoute::LegacyOnly:
            return "legacy_only";
        case PlayerbotSocialInboundRoute::SocialOpportunity:
            return "social_opportunity";
        case PlayerbotSocialInboundRoute::ThreadContinuationOnly:
            return "thread_continuation_only";
        case PlayerbotSocialInboundRoute::SuppressedSurface:
            return "suppressed_surface";
    }

    return "unknown";
}

bool PlayerbotSocialChannelFromChatSource(ChatChannelSource source, PlayerbotSocialChannel& channel)
{
    switch (source)
    {
        case ChatChannelSource::SRC_GENERAL:
            channel = PlayerbotSocialChannel::General;
            return true;
        case ChatChannelSource::SRC_SAY:
            channel = PlayerbotSocialChannel::Say;
            return true;
        case ChatChannelSource::SRC_PARTY:
            channel = PlayerbotSocialChannel::Party;
            return true;
        case ChatChannelSource::SRC_WHISPER:
            channel = PlayerbotSocialChannel::Whisper;
            return true;
        default:
            /*
             * Guild, World, Trade, Looking For Group, both defense channels, guild recruitment, yell,
             * both emote forms, raid, and the undefined surface that battleground and raid warning
             * resolve to. A surface added upstream lands here too, which is the intended direction:
             * an unrecognized place to speak is refused rather than guessed at.
             */
            return false;
    }
}

PlayerbotSocialInboundDecision PlayerbotSocialRouteInbound(ChatChannelSource source,
                                                           PlayerbotSocialInboundContext const& context,
                                                           PlayerbotSocialGate const& gate)
{
    PlayerbotSocialInboundDecision decision;

    if (!gate.enabled || context.machineTraffic)
        return decision;

    PlayerbotSocialChannel channel = PlayerbotSocialChannel::General;
    if (PlayerbotSocialChannelFromChatSource(source, channel))
    {
        if (context.listenerInBattleground)
        {
            /*
             * Checked before the surface is honored rather than after. The chat types a battleground
             * owns never reach this router, but say, party, and the zone channels are all carried
             * inside one and resolve to their ordinary sources there, so a battleground can only be
             * recognized through the listener. It is silenced in both directions: no social input and
             * no canned reply either, because a stock line in the middle of a fight is exactly the
             * chatter this feature retires.
             */
            decision.route = PlayerbotSocialInboundRoute::SuppressedSurface;
            decision.suppressLegacyReply = true;
            return decision;
        }

        decision.channel = channel;
        decision.suppressLegacyReply = true;

        /*
         * An operator pause, or this channel switched off, silences the surface rather than handing
         * it back to the legacy canned reply queue. Pause is a stop, and a stop that made the bots
         * start saying stock lines instead would be the opposite of what it reads as. Rolling the
         * deployment back to pre-feature behaviour is what the configuration option above does, and
         * it keeps its own legacy route.
         *
         * A continuation is exempt: a line the feature already delivered is a real turn in a thread
         * that exists, and dropping it would leave that thread believing its own last message was
         * never said. It updates the thread and still opens nothing new.
         */
        bool const silenced = gate.paused || !gate.channelEnabled[static_cast<std::size_t>(channel)];
        if (silenced)
        {
            decision.route = context.originatedFromSocialDelivery ? PlayerbotSocialInboundRoute::ThreadContinuationOnly
                                                                  : PlayerbotSocialInboundRoute::SuppressedSurface;
            return decision;
        }

        /*
         * A generated bot line is still an opportunity. Bot-only conversations are stopped by the
         * ordinary reply cooldown and consecutive-bot-turn decay, not by making every starter a
         * one-line monologue. The continuation-only route above remains the narrow answer when the
         * operator silenced the channel between accepting and observing an already delivered line.
         */
        decision.route =
            context.originatedFromSocialDelivery && gate.stage != PlayerbotSocialRolloutStage::BoundedContinuation
                ? PlayerbotSocialInboundRoute::ThreadContinuationOnly
                : PlayerbotSocialInboundRoute::SocialOpportunity;
        return decision;
    }

    switch (source)
    {
        case ChatChannelSource::SRC_GUILD:
        case ChatChannelSource::SRC_EMOTE:
        case ChatChannelSource::SRC_TEXT_EMOTE:
            /*
             * Left alone on purpose. Guild is a conversation space this feature does not own yet, and
             * the two emote surfaces are not chat. Silencing either would be a behavior change nobody
             * asked for, so they keep the legacy path even while the feature is on.
             */
            return decision;
        default:
            /*
             * World, Trade, Looking For Group, both defense channels, guild recruitment, yell, raid,
             * and the undefined surface that battleground resolves to, plus anything added upstream.
             *
             * Suppressing by default rather than by list is the fail closed direction here: a new
             * server wide channel appearing in a later core version would otherwise start carrying
             * canned bot chatter that this feature is supposed to have retired.
             */
            decision.route = PlayerbotSocialInboundRoute::SuppressedSurface;
            decision.suppressLegacyReply = true;
            return decision;
    }
}

bool PlayerbotSocialSpeakerCanOpenOpportunity(bool speakerIsHuman, bool originatedFromSocialDelivery)
{
    return speakerIsHuman || originatedFromSocialDelivery;
}

char const* PlayerbotSocialCaptureRejectionName(PlayerbotSocialCaptureRejection rejection)
{
    switch (rejection)
    {
        case PlayerbotSocialCaptureRejection::None:
            return "none";
        case PlayerbotSocialCaptureRejection::UnsupportedChannel:
            return "unsupported_channel";
        case PlayerbotSocialCaptureRejection::MissingSpeaker:
            return "missing_speaker";
        case PlayerbotSocialCaptureRejection::MissingZone:
            return "missing_zone";
        case PlayerbotSocialCaptureRejection::MissingGroup:
            return "missing_group";
        case PlayerbotSocialCaptureRejection::MissingTarget:
            return "missing_target";
        case PlayerbotSocialCaptureRejection::UnexpectedTarget:
            return "unexpected_target";
        case PlayerbotSocialCaptureRejection::SpeakerIsTarget:
            return "speaker_is_target";
        case PlayerbotSocialCaptureRejection::OutOfHearingRange:
            return "out_of_hearing_range";
        case PlayerbotSocialCaptureRejection::MissingSayCohort:
            return "missing_say_cohort";
        case PlayerbotSocialCaptureRejection::IdentifierOutOfRange:
            return "identifier_out_of_range";
    }

    return "unknown";
}

PlayerbotSocialCaptureRejection PlayerbotSocialValidateCapture(PlayerbotSocialCapturedMessage const& captured)
{
    /*
     * Ordered so the reported reason is the most fundamental one. The channel decides which shape
     * applies, so nothing else can be judged before it; the speaker and the hearing check hold for
     * every channel; only then do the per channel fields matter.
     */
    if (!PlayerbotSocialChannelIsValid(captured.channel))
        return PlayerbotSocialCaptureRejection::UnsupportedChannel;

    if (captured.speakerGuidCounter == 0)
        return PlayerbotSocialCaptureRejection::MissingSpeaker;

    if (!captured.withinHearingRange)
        return PlayerbotSocialCaptureRejection::OutOfHearingRange;

    switch (captured.channel)
    {
        case PlayerbotSocialChannel::General:
            if (captured.targetGuidCounter != 0)
                return PlayerbotSocialCaptureRejection::UnexpectedTarget;
            if (captured.zoneId == 0)
                return PlayerbotSocialCaptureRejection::MissingZone;
            break;
        case PlayerbotSocialChannel::Say:
            if (captured.targetGuidCounter != 0)
                return PlayerbotSocialCaptureRejection::UnexpectedTarget;
            if (captured.zoneId == 0)
                return PlayerbotSocialCaptureRejection::MissingZone;
            if (captured.sayCohortScopeId == 0)
                return PlayerbotSocialCaptureRejection::MissingSayCohort;
            break;
        case PlayerbotSocialChannel::Party:
            if (captured.targetGuidCounter != 0)
                return PlayerbotSocialCaptureRejection::UnexpectedTarget;
            if (captured.groupId == 0)
                return PlayerbotSocialCaptureRejection::MissingGroup;
            break;
        case PlayerbotSocialChannel::Whisper:
            if (captured.targetGuidCounter == 0)
                return PlayerbotSocialCaptureRejection::MissingTarget;
            if (captured.targetGuidCounter == captured.speakerGuidCounter)
                return PlayerbotSocialCaptureRejection::SpeakerIsTarget;

            // Only a whisper packs both identities into its scope, so only a whisper needs them to
            // fit. A wider value would overflow its half and alias onto an unrelated pair.
            if (captured.speakerGuidCounter > PLAYERBOT_SOCIAL_MAX_CHARACTER_COUNTER ||
                captured.targetGuidCounter > PLAYERBOT_SOCIAL_MAX_CHARACTER_COUNTER)
                return PlayerbotSocialCaptureRejection::IdentifierOutOfRange;
            break;
        default:
            // Unreachable: the validity check above already refused anything outside the four.
            return PlayerbotSocialCaptureRejection::UnsupportedChannel;
    }

    return PlayerbotSocialCaptureRejection::None;
}

uint64 PlayerbotSocialWhisperScopeId(uint64 firstGuidCounter, uint64 secondGuidCounter)
{
    // Ordered before packing, so the scope names the pair rather than the direction.
    uint64 const low = firstGuidCounter < secondGuidCounter ? firstGuidCounter : secondGuidCounter;
    uint64 const high = firstGuidCounter < secondGuidCounter ? secondGuidCounter : firstGuidCounter;

    /*
     * Packed, not hashed. Both halves are 32 bit character counters, so the pair survives whole and
     * two different pairs cannot produce one scope at all. Validation refuses a wider identifier
     * before it reaches here, which is what keeps that true.
     */
    return ((low & PLAYERBOT_SOCIAL_MAX_CHARACTER_COUNTER) << 32) | (high & PLAYERBOT_SOCIAL_MAX_CHARACTER_COUNTER);
}

uint64 PlayerbotSocialSayCohortRegistry::Resolve(std::vector<uint64> members, uint64 nowUnixSeconds)
{
    members.erase(std::remove(members.begin(), members.end(), 0), members.end());
    std::sort(members.begin(), members.end());
    members.erase(std::unique(members.begin(), members.end()), members.end());
    if (members.empty())
        return 0;

    Prune(nowUnixSeconds);
    auto const found = _entries.find(members);
    if (found != _entries.end())
    {
        found->second.lastUsedUnixSeconds = nowUnixSeconds;
        return found->second.scopeId;
    }

    if (_nextScopeId == 0)
        ++_nextScopeId;

    Entry entry;
    entry.scopeId = _nextScopeId++;
    entry.lastUsedUnixSeconds = nowUnixSeconds;
    uint64 const scopeId = entry.scopeId;
    _entries.emplace(std::move(members), entry);
    return scopeId;
}

void PlayerbotSocialSayCohortRegistry::Prune(uint64 nowUnixSeconds)
{
    for (auto entry = _entries.begin(); entry != _entries.end();)
    {
        bool const clockMovedBackwards = entry->second.lastUsedUnixSeconds > nowUnixSeconds;
        bool const stale = !clockMovedBackwards &&
                           nowUnixSeconds - entry->second.lastUsedUnixSeconds > PLAYERBOT_SOCIAL_THREAD_STALE_SECONDS;
        if (clockMovedBackwards || stale)
            entry = _entries.erase(entry);
        else
            ++entry;
    }
}

uint64 PlayerbotSocialResolveSayCohort(std::vector<uint64> members, uint64 nowUnixSeconds)
{
    static PlayerbotSocialSayCohortRegistry registry;
    return registry.Resolve(std::move(members), nowUnixSeconds);
}

uint64 PlayerbotSocialScopeIdFor(PlayerbotSocialCapturedMessage const& captured)
{
    switch (captured.channel)
    {
        case PlayerbotSocialChannel::General:
            return captured.zoneId;
        case PlayerbotSocialChannel::Say:
            return captured.sayCohortScopeId;
        case PlayerbotSocialChannel::Party:
            return captured.groupId;
        case PlayerbotSocialChannel::Whisper:
            return PlayerbotSocialWhisperScopeId(captured.speakerGuidCounter, captured.targetGuidCounter);
        default:
            return 0;
    }
}

PlayerbotSocialObservation PlayerbotSocialObservationFor(PlayerbotSocialCapturedMessage const& captured)
{
    PlayerbotSocialObservation observation;

    if (PlayerbotSocialValidateCapture(captured) != PlayerbotSocialCaptureRejection::None)
    {
        /*
         * Deliberately unusable rather than merely empty. A default constructed observation still
         * names General and would open a thread in scope zero, so a rejected message has to carry a
         * channel the coordinator refuses outright.
         */
        observation.key.channel = REFUSED_CHANNEL;
        return observation;
    }

    observation.key.channel = captured.channel;
    observation.key.scopeId = PlayerbotSocialScopeIdFor(captured);
    observation.eventPublicId = captured.eventPublicId;
    observation.role = captured.speakerIsHuman                 ? PlayerbotSocialPromptLineRole::HumanObservation
                       : captured.replyToEventPublicId.empty() ? PlayerbotSocialPromptLineRole::GeneratedStarter
                                                               : PlayerbotSocialPromptLineRole::GeneratedReply;
    observation.replyToEventPublicId = captured.replyToEventPublicId;
    observation.sourceEventPublicId = captured.sourceEventPublicId;
    observation.speakerGuidCounter = captured.speakerGuidCounter;
    observation.speakerName = captured.speakerName;
    observation.speakerIsHuman = captured.speakerIsHuman;
    observation.zoneId = captured.zoneId;
    observation.atUnixSeconds = captured.atUnixSeconds;
    observation.text = captured.text;

    return observation;
}

std::vector<PlayerbotSocialNearbySnapshotEntry> PlayerbotSocialSelectNearby(
    PlayerbotSocialChannel channel, std::vector<PlayerbotSocialNearbyCharacter> const& characters)
{
    std::vector<PlayerbotSocialNearbySnapshotEntry> selected;
    if (!PlayerbotSocialChannelIsValid(channel) || channel == PlayerbotSocialChannel::Whisper)
        return selected;

    for (PlayerbotSocialNearbyCharacter const& character : characters)
    {
        bool const channelEligible = channel == PlayerbotSocialChannel::General
                                         ? character.sameZone && character.channelMember
                                     : channel == PlayerbotSocialChannel::Party ? character.sameParty
                                                                                : true;
        if (character.name.empty() || character.isObserver || !character.sameMap || !character.samePhase ||
            !character.visible || !character.factionMatches || !character.consented || !character.withinRange ||
            !channelEligible)
            continue;

        auto const duplicate = std::find_if(selected.begin(), selected.end(),
                                            [&character](PlayerbotSocialNearbySnapshotEntry const& existing)
                                            { return existing.name == character.name; });
        if (duplicate == selected.end())
            selected.push_back({character.characterGuidCounter, character.name, character.characterIsHuman});

        if (selected.size() >= PLAYERBOT_SOCIAL_CONTEXT_ENTRIES)
            break;
    }

    return selected;
}

namespace
{
/*
 * The candidates one dispatch has collected, grouped by conversation scope.
 *
 * File scope rather than a member, because the collector's lifetime is a call stack rather than
 * an object's: the chat callback opens it, the two manager fan outs fill it, and it closes when
 * the callback returns. World thread only, which is what makes the absence of synchronization
 * correct rather than merely convenient.
 */
struct DispatchGroup
{
    PlayerbotSocialThreadHandle thread;
    PlayerbotSocialChannel channel = PlayerbotSocialChannel::General;
    uint64 speakerGuidCounter = 0;
    bool speakerIsHuman = false;
    uint32 zoneId = 0;
    uint64 atUnixSeconds = 0;
    PlayerbotSocialPromptLine currentLine;
    std::vector<PlayerbotSocialActivationCandidate> candidates;
};

uint32 g_dispatchDepth = 0;
std::map<PlayerbotSocialThreadKey, DispatchGroup> g_dispatchGroups;

/*
 * Runs one collected scope. Called only when the outermost dispatch closes.
 *
 * The thread pressure inputs are read back from the coordinator rather than accumulated here,
 * because every bot in this group has already observed the message: reading now sees the thread
 * as it stands AFTER those observations, which is the state the decision is actually about.
 */
void ActivateGroup(DispatchGroup const& group)
{
    if (group.candidates.empty())
        return;

    PlayerbotSocialThreadPressure const pressure = sPlayerbotSocialMgr.PressureFor(group.thread, group.atUnixSeconds);

    PlayerbotSocialActivation activation;
    activation.thread = group.thread;
    activation.channel = group.channel;
    activation.speakerGuidCounter = group.speakerGuidCounter;
    activation.speakerIsHuman = group.speakerIsHuman;
    activation.speakerOptedOut = sPlayerbotSocialMgr.State().IsOptedOut(group.speakerGuidCounter);
    activation.currentLine = group.currentLine;

    /*
     * Carried so a suppressed opportunity is filterable by zone alongside the deliveries it did
     * not produce. The scope id cannot stand in for it: for General it IS the zone, but for party
     * it is the group, so deriving one from the other would file party events under a zone that
     * happens to share the number.
     */
    activation.zoneId = group.zoneId;

    // This path is driven by an observed message and is reply only. Starters are activated by
    // PlayerbotSocialPumpStarters from pending General context.
    activation.starter = false;

    /*
     * Answered by the coordinator when it observed the line, not recomputed here. `Observe` is
     * the only place that sees the thread's history and the new line together, and it answers
     * before the line joins that history, so a message is never a duplicate of itself.
     */
    activation.duplicateOfRecentMessage = group.thread.duplicateOfRecentMessage;
    activation.channelDensity = pressure.channelDensity;
    activation.threadLastActivityUnixSeconds = pressure.lastActivityUnixSeconds;
    activation.relevantHumanMessages = pressure.relevantHumanMessages;
    activation.consecutiveBotOnlyTurns = pressure.consecutiveBotOnlyTurns;
    activation.nowUnixSeconds = group.atUnixSeconds;

    /*
     * Seeded from the thread and the moment rather than drawn from the global generator, so one
     * dispatch is reproducible from its own inputs. Two scopes in the same dispatch get different
     * seeds because the thread id differs, which is what stops every zone answering in lockstep.
     */
    activation.selectionSeed =
        PlayerbotPersonality::SplitMix64(group.thread.threadId ^ (group.atUnixSeconds << 1) ^ group.speakerGuidCounter);

    activation.candidates = group.candidates;

    /*
     * An observed human line goes through the roleplay assessment first; everything else keeps
     * the ordinary path. Fallbacks live inside AssessAndActivate, so a missing or refusing
     * provider still activates immediately and no conversation is lost to the classifier.
     */
    if (PlayerbotSocialOpportunityRequiresAssessment(activation))
        sPlayerbotSocialMgr.AssessAndActivate(activation, PlayerbotSocialEffectiveGate().density);
    else
        sPlayerbotSocialMgr.Activate(activation, PlayerbotSocialEffectiveGate().density);
}
}  // namespace

bool PlayerbotSocialOpportunityRequiresAssessment(PlayerbotSocialActivation const& activation)
{
    return activation.thread.valid && activation.speakerIsHuman && !activation.starter;
}

PlayerbotSocialDispatchScope::PlayerbotSocialDispatchScope() { ++g_dispatchDepth; }

PlayerbotSocialDispatchScope::~PlayerbotSocialDispatchScope()
{
    if (g_dispatchDepth > 0)
        --g_dispatchDepth;

    // Only the outermost scope activates, so a nested callback cannot flush a field the outer one is
    // still filling.
    if (g_dispatchDepth != 0)
        return;

    /*
     * Moved out before activating rather than iterated in place. Activation reaches the coordinator,
     * and a future producer that captures while delivering would otherwise mutate the container this
     * loop is walking. Clearing first makes that a no op instead of undefined behaviour.
     */
    std::map<PlayerbotSocialThreadKey, DispatchGroup> collected;
    collected.swap(g_dispatchGroups);

    for (auto const& [key, group] : collected)
        ActivateGroup(group);
}

bool PlayerbotSocialDispatchIsOpen() { return g_dispatchDepth > 0; }

PlayerbotSocialThreadHandle PlayerbotSocialObserveOncePerDispatch(PlayerbotSocialObservation const& observation)
{
    if (!PlayerbotSocialDispatchIsOpen())
        return {};

    DispatchGroup& group = g_dispatchGroups[observation.key];
    if (!group.thread.valid)
    {
        PlayerbotSocialObservation prepared = observation;
        if (prepared.speakerIsHuman && !sPlayerbotSocialMgr.PrepareHumanObservation(prepared))
            return {};

        group.thread = sPlayerbotSocialMgr.Observe(prepared);
        if (prepared.speakerIsHuman)
            sPlayerbotSocialMgr.RecordHumanObservation(prepared, group.thread);

        group.currentLine.eventPublicId = prepared.eventPublicId;
        group.currentLine.role = prepared.role;
        group.currentLine.replyToEventPublicId = prepared.replyToEventPublicId;
        group.currentLine.sourceEventPublicId = prepared.sourceEventPublicId;
        group.currentLine.speakerGuidCounter = prepared.speakerGuidCounter;
        group.currentLine.speakerName = prepared.speakerName;
        group.currentLine.speakerIsHuman = prepared.speakerIsHuman;
        group.currentLine.atUnixSeconds = prepared.atUnixSeconds;
        group.currentLine.text = prepared.text;
    }

    return group.thread;
}

namespace
{
/*
 * The world's answer to every condition the revalidation asks about, for one pending delivery.
 *
 * Both characters are resolved once, here, and neither pointer outlives this call. A condition
 * whose counterpart does not exist is reported as satisfiable rather than failed: a room
 * addressed line has no target, and treating its absent target as "gone" would refuse every
 * General, say and party answer the feature produces.
 */
PlayerbotSocialDeliveryConditions ObserveDeliveryConditions(Player* bot, PlayerbotAI* botAI, Player* target,
                                                            Player* subject,
                                                            PlayerbotSocialPendingDelivery const& pending)
{
    PlayerbotSocialDeliveryConditions conditions;

    /*
     * A character with no bot AI cannot be made to speak, and a silenced one is refused by the
     * server whatever it tries to say. Both are folded into the speaker being unavailable rather
     * than given new refusal reasons, because the delivery contract's reasons are frozen and
     * "this bot cannot speak right now" is what SpeakerGone already means for delivery.
     */
    conditions.speakerOnline = bot != nullptr && botAI != nullptr && bot->CanSpeak();
    conditions.speakerAlive = bot != nullptr && bot->IsAlive();
    conditions.speakerInCombat = bot != nullptr && bot->IsInCombat();
    conditions.currentGrounding =
        CaptureCurrentGroundingEnvelope(bot, subject, pending.channel, static_cast<uint64>(time(nullptr)));

    conditions.consentHolds = bot != nullptr && !sPlayerbotSocialMgr.State().IsOptedOut(pending.botGuidCounter) &&
                              (pending.statelessDirectReply || pending.targetGuidCounter == 0 ||
                               !sPlayerbotSocialMgr.State().IsOptedOut(pending.targetGuidCounter));

    /*
     * The thread has to still be the one this answers. A thread pruned while the answer was in
     * flight makes this a superseded conversation, and the reply a non sequitur.
     */
    conditions.threadStillCurrent = sPlayerbotSocialMgr.ThreadIsCurrent(pending.threadPublicId);

    if (bot == nullptr)
        return conditions;

    if (target == nullptr)
    {
        /*
         * No addressee, so the conversation itself is the counterparty. Answering "is the target
         * still nearby" with a vacuous true would mean a room addressed line is never checked
         * against anything at all: a bot that walked to another zone, or left the party, would
         * still deliver into a conversation it is no longer part of.
         *
         * The scope is the authority instead. It is the zone for General and say and the group
         * for party, which is exactly what the conversation was keyed by when it opened.
         */
        conditions.factionAllows = true;
        conditions.languageUnderstood = true;
        conditions.targetOnline = pending.targetGuidCounter == 0;

        PlayerbotSocialThreadKey scope;
        bool const scopeKnown = sPlayerbotSocialMgr.ThreadScopeFor(pending.threadPublicId, scope);

        // An unrecoverable scope fails closed. A thread whose scope cannot be read is one this
        // answer can no longer be shown to belong to.
        if (!scopeKnown)
        {
            conditions.sameMap = false;
            conditions.samePhase = false;
            conditions.targetVisible = false;
            conditions.withinRange = false;
            conditions.inSameGroup = false;
            conditions.inChannel = false;
            return conditions;
        }

        bool const stillInScope =
            pending.channel == PlayerbotSocialChannel::Party
                ? bot->GetGroup() != nullptr && bot->GetGroup()->GetGUID().GetCounter() == scope.scopeId
                : static_cast<uint64>(bot->GetZoneId()) == scope.scopeId;

        conditions.inSameGroup = stillInScope;

        /*
         * Membership rather than location for a channel. Standing in the zone does not put a bot
         * on that zone's General channel: it can leave the channel and stay put, and speaking
         * then produces a "not a member" notice rather than a message anyone reads.
         */
        conditions.inChannel = pending.channel == PlayerbotSocialChannel::General
                                   ? stillInScope && botAI != nullptr && botAI->IsOnChannel(ChatChannelId::GENERAL)
                                   : stillInScope;

        /*
         * A room addressed EMOTE has nowhere to point. The party revalidator requires the target
         * to be on the same map and phase, in range and visible, and with no target there is
         * nobody for those to be true of. Reporting them as satisfied because the bot is still in
         * the group would let a gesture aimed at nobody be performed across a continent, which is
         * the distant directed emote the contract forbids. Refused instead.
         */
        bool const emoteWithoutTarget = pending.result.kind == PlayerbotSocialOutputKind::Emote;

        conditions.sameMap = stillInScope && !emoteWithoutTarget;
        conditions.samePhase = stillInScope && !emoteWithoutTarget;
        conditions.targetVisible = stillInScope && !emoteWithoutTarget;
        conditions.withinRange = stillInScope && !emoteWithoutTarget;
        return conditions;
    }

    conditions.targetOnline = true;
    conditions.factionAllows = bot->GetTeamId() == target->GetTeamId();
    conditions.languageUnderstood = conditions.factionAllows;
    conditions.sameMap = bot->GetMapId() == target->GetMapId();
    conditions.samePhase = bot->GetPhaseMask() & target->GetPhaseMask();
    conditions.targetVisible = bot->CanSeeOrDetect(target);
    conditions.withinRange =
        conditions.sameMap && bot->IsWithinDistInMap(target, sWorld->getFloatConfig(CONFIG_LISTEN_RANGE_SAY));
    conditions.inSameGroup = bot->GetGroup() != nullptr && bot->GetGroup() == target->GetGroup();
    conditions.inChannel = true;

    return conditions;
}

/*
 * The lines the feature is about to speak through PlayerScript, so each callback can recognize
 * its bot speaker as Social rather than functional output.
 *
 * Keyed by bot and matched on the exact text. World thread only, like the dispatch collector,
 * and bounded by the echo window rather than by a capacity: a delivery is followed by its echo
 * within a tick or two, so nothing accumulates.
 */
struct DeliveredLine
{
    std::string text;
    std::string eventPublicId;
    std::string replyToEventPublicId;
    std::string sourceEventPublicId;
    uint64 atUnixSeconds = 0;
};

/*
 * Several lines per bot, not one.
 *
 * A bot may hold PLAYERBOT_SOCIAL_MAX_PENDING_PER_BOT requests, and one pass of the delivery pump
 * can send all of them before any echo comes back. Keeping only the newest would let the second
 * delivery overwrite the first, so the first callback would classify a generated line as
 * functional speech and the conversation would stop despite having eligible listeners.
 */
std::map<uint64, std::vector<DeliveredLine>> g_deliveredLines;

// When each bot last spoke socially. Pruned to the cooldown window on write.
std::map<uint64, uint64> g_lastSpoke;

// A record whose echo window has passed, or whose stamp is in the future because the wall clock
// moved backwards. Both are unusable: a stamp ahead of now can never be shown to have expired,
// so it would suppress a genuine repetition indefinitely.
bool DeliveredLineIsStale(DeliveredLine const& record, uint64 nowUnixSeconds)
{
    if (nowUnixSeconds < record.atUnixSeconds)
        return true;

    return nowUnixSeconds - record.atUnixSeconds > PLAYERBOT_SOCIAL_DELIVERY_ECHO_WINDOW_SECONDS;
}
}  // namespace

void PlayerbotSocialRecordDelivery(PlayerbotSocialDelivery const& delivery)
{
    // With the gate off nothing about this feature runs, including its telemetry. An untouched
    // configuration must behave exactly as it did before the feature existed.
    if (!PlayerbotSocialGateIsLive(PlayerbotSocialEffectiveGate()))
        return;

    if (delivery.botGuidCounter == 0)
        return;

    // Key Decision 6. Unsupported channels do not enter the Social feed, and refusing here rather
    // than at each producer is what stops one producer's mistake from filing a raid line as social.
    if (!PlayerbotSocialChannelIsValid(delivery.channel))
        return;

    sPlayerbotSocialMgr.RecordEvent(PlayerbotSocialMakeDeliveryEvent(delivery));
}

bool PlayerbotSocialDeliveryRecordFor(PlayerbotSocialDeliveryRequest const& request,
                                      PlayerbotSocialSpeaker const& speaker, bool accepted, uint64 nowUnixSeconds,
                                      PlayerbotSocialDelivery& record)
{
    // The one rule this function exists for. Everything below describes a line a player heard.
    if (!accepted)
        return false;

    record = PlayerbotSocialDelivery();
    record.eventPublicId = request.eventPublicId;
    record.replyToEventPublicId = request.replyToEventPublicId;
    record.sourceEventPublicId = request.sourceEventPublicId;
    record.botGuidCounter = speaker.botGuidCounter;
    record.channel = request.channel;
    record.origin = request.origin;
    record.targetGuidCounter = speaker.targetGuidCounter;
    record.threadPublicId = request.threadPublicId;
    record.zoneId = speaker.zoneId;
    if (request.origin == PlayerbotSocialEventOrigin::Social)
    {
        record.callMetadata = request.callMetadata;
        record.operatorEvidence = request.operatorEvidence;
    }
    record.occurredAtUnixSeconds = request.occurredAtUnixSeconds != 0 ? request.occurredAtUnixSeconds : nowUnixSeconds;

    // A gesture REPLACES the line rather than accompanying it. Carrying both would let the feed show
    // words beside an emote that was performed in silence.
    if (request.isEmote)
    {
        record.isEmote = true;
        record.emoteId = request.emoteId;
    }
    else
    {
        record.text = request.text;
    }

    return true;
}

uint32 PlayerbotSocialSpokenLanguageFor(TeamId team)
{
    return team == TEAM_ALLIANCE ? static_cast<uint32>(LANG_COMMON) : static_cast<uint32>(LANG_ORCISH);
}

namespace
{
// Asks the world to carry one line, and reports whether it took it. Nothing here decides anything:
// the channel was chosen by the producer and the language came with it.
bool PerformSend(Player* bot, Player* target, PlayerbotSocialDeliveryRequest const& request)
{
    PlayerbotAI* const botAI = GET_PLAYERBOT_AI(bot);

    if (request.isEmote)
    {
        /*
         * `PlayEmote` returns false unconditionally, so its result carries no information, and the
         * opcode handler behind it can still refuse a dead or silenced character. The preconditions
         * are checked rather than assumed, and what is reported is "issued in a state the server
         * accepts" rather than a delivery confirmation this code cannot give.
         */
        if (botAI == nullptr || !bot->IsAlive() || !bot->CanSpeak())
            return false;

        botAI->PlayEmote(request.emoteId);
        return true;
    }

    switch (request.channel)
    {
        case PlayerbotSocialChannel::Say:
        {
            /*
             * `Player::Say` returns void and can still drop the line: the say chat filter refuses
             * it outright. That is asked here, on the same predicate the core uses, rather than
             * assumed away, because a helper that answered "sent" for a filtered line would file
             * speech nobody heard into a feed of what players heard.
             *
             * Asked BESIDE the send rather than instead of it, so the core still does exactly
             * what it did before, including the notice it sends the speaker.
             *
             * One refusal remains unobservable: a `PlayerScript` implementing
             * `OnPlayerCanUseChat` can veto the message inside the core call. It cannot be
             * pre-checked here, because that hook takes the text by reference and may rewrite
             * it, so asking it twice would run its side effects twice. The only implementation
             * in this tree, `chat_log.cpp`, always allows.
             */
            bool const filtered = sWorld->getBoolConfig(CONFIG_CHAT_FILTER_SAY) && Player::IsChatFiltered(request.text);

            bot->Say(request.text, static_cast<Language>(request.languageId));
            return !filtered;
        }
        case PlayerbotSocialChannel::Whisper:
        {
            if (target == nullptr)
                return false;

            // A filtered whisper never reaches the addressee: the core sends the speaker a
            // `CHAT_MSG_FILTERED` notice instead and the target gets nothing.
            bool const filtered =
                sWorld->getBoolConfig(CONFIG_CHAT_FILTER_WHISPER) && Player::IsChatFiltered(request.text);

            bot->Whisper(request.text, static_cast<Language>(request.languageId), target);
            return !filtered;
        }
        case PlayerbotSocialChannel::Party:
            return botAI != nullptr && botAI->SayToParty(request.text);
        case PlayerbotSocialChannel::General:
            return botAI != nullptr && botAI->SayToChannel(request.text, ChatChannelId::GENERAL);
    }

    return false;
}
}  // namespace

bool PlayerbotSocialDeliver(Player* bot, Player* target, PlayerbotSocialDeliveryRequest const& request)
{
    if (bot == nullptr)
        return false;

    PlayerbotSocialDeliveryRequest prepared = request;
    bool const live = PlayerbotSocialGateIsLive(PlayerbotSocialEffectiveGate());
    if (live && prepared.retainTelemetry && prepared.origin == PlayerbotSocialEventOrigin::Social &&
        prepared.eventPublicId.empty())
        prepared.eventPublicId = sPlayerbotSocialMgr.ReserveDeliveryEventPublicId(bot->GetGUID().GetCounter());

    uint64 const nowUnixSeconds =
        prepared.occurredAtUnixSeconds != 0 ? prepared.occurredAtUnixSeconds : static_cast<uint64>(time(nullptr));
    bool const captureThroughPlayerScript =
        live && prepared.retainTelemetry && !prepared.isEmote &&
        prepared.origin == PlayerbotSocialEventOrigin::Social &&
        (prepared.channel == PlayerbotSocialChannel::Say || prepared.channel == PlayerbotSocialChannel::Whisper);
    if (captureThroughPlayerScript)
        PlayerbotSocialRememberDeliveredLine(bot->GetGUID().GetCounter(), prepared.text, nowUnixSeconds,
                                             prepared.eventPublicId, prepared.replyToEventPublicId,
                                             prepared.sourceEventPublicId);

    bool const accepted = PerformSend(bot, target, prepared);

    if (!accepted && captureThroughPlayerScript)
    {
        std::string unusedEventPublicId;
        bool const markerWasPending = PlayerbotSocialWasDeliveredLine(bot->GetGUID().GetCounter(), prepared.text,
                                                                      nowUnixSeconds, &unusedEventPublicId);
        (void)markerWasPending;
    }

    /*
     * Asked here as well as inside the recorder, and not as a matter of trust. With the feature off
     * this helper now sits on every bot chat line in the module, and building a record only for the
     * recorder to drop it would spend a copy of every line a server that never enabled the feature
     * speaks. Producers still do not read the gate; the one seam they all go through does.
     */
    if (!live)
        return accepted;

    if (!prepared.retainTelemetry)
        return accepted;

    PlayerbotSocialSpeaker speaker;
    speaker.botGuidCounter = bot->GetGUID().GetCounter();
    speaker.targetGuidCounter = target != nullptr ? target->GetGUID().GetCounter() : 0;

    // Read AFTER the send, because the send is the moment the line existed and a bot can have moved
    // during whatever delay preceded it.
    speaker.zoneId = bot->GetZoneId();

    PlayerbotSocialDelivery record;
    if (PlayerbotSocialDeliveryRecordFor(prepared, speaker, accepted, nowUnixSeconds, record))
        PlayerbotSocialRecordDelivery(record);

    if (accepted && !prepared.isEmote &&
        (prepared.channel == PlayerbotSocialChannel::General || prepared.channel == PlayerbotSocialChannel::Party))
        CaptureGeneratedAudience(bot, prepared);

    return accepted;
}

bool PlayerbotSocialSay(Player* bot, std::string const& text, uint32 languageId, PlayerbotSocialEventOrigin origin)
{
    PlayerbotSocialDeliveryRequest request;
    request.channel = PlayerbotSocialChannel::Say;
    request.origin = origin;
    request.languageId = languageId;
    request.text = text;
    return PlayerbotSocialDeliver(bot, nullptr, request);
}

bool PlayerbotSocialChannelFromChatMsg(ChatMsg type, PlayerbotSocialChannel& channel)
{
    switch (type)
    {
        case CHAT_MSG_SAY:
            channel = PlayerbotSocialChannel::Say;
            return true;
        case CHAT_MSG_WHISPER:
        case CHAT_MSG_WHISPER_INFORM:
            channel = PlayerbotSocialChannel::Whisper;
            return true;
        case CHAT_MSG_PARTY:
        case CHAT_MSG_PARTY_LEADER:
            channel = PlayerbotSocialChannel::Party;
            return true;
        default:
            /*
             * Everything else, including `CHAT_MSG_ADDON`, raid, guild, yell, and `CHAT_MSG_CHANNEL`.
             * A channel message names no channel here, so General cannot be told apart from Trade or
             * a defense channel, and guessing is how a trade advert lands in the Social feed.
             */
            return false;
    }
}

bool PlayerbotSocialDeliverDirect(Player* bot, Player* target, ChatMsg type, uint32 languageId, std::string const& text,
                                  PlayerbotSocialEventOrigin origin)
{
    if (bot == nullptr || target == nullptr)
        return false;

    WorldPacket data;
    ChatHandler::BuildChatPacket(data, type, static_cast<Language>(languageId), bot, nullptr, text);
    target->SendDirectMessage(&data);

    // The send is a packet the module built and handed to one session. There is no filter and no
    // hook between here and the client, so unlike `Player::Say` this one is genuinely delivered.
    if (!PlayerbotSocialGateIsLive(PlayerbotSocialEffectiveGate()))
        return true;

    PlayerbotSocialDeliveryRequest request;
    request.origin = origin;
    request.languageId = languageId;
    request.text = text;

    if (!PlayerbotSocialChannelFromChatMsg(type, request.channel))
        return true;

    PlayerbotSocialSpeaker speaker;
    speaker.botGuidCounter = bot->GetGUID().GetCounter();
    speaker.targetGuidCounter = target->GetGUID().GetCounter();
    speaker.zoneId = bot->GetZoneId();

    PlayerbotSocialDelivery record;
    if (PlayerbotSocialDeliveryRecordFor(request, speaker, true, static_cast<uint64>(time(nullptr)), record))
        PlayerbotSocialRecordDelivery(record);

    return true;
}

bool PlayerbotSocialWhisper(Player* bot, std::string const& text, uint32 languageId, Player* target,
                            PlayerbotSocialEventOrigin origin)
{
    PlayerbotSocialDeliveryRequest request;
    request.channel = PlayerbotSocialChannel::Whisper;
    request.origin = origin;
    request.languageId = languageId;
    request.text = text;
    return PlayerbotSocialDeliver(bot, target, request);
}

void PlayerbotSocialRememberDeliveredLine(uint64 botGuidCounter, std::string const& text, uint64 nowUnixSeconds,
                                          std::string const& eventPublicId, std::string const& replyToEventPublicId,
                                          std::string const& sourceEventPublicId)
{
    if (botGuidCounter == 0 || text.empty())
        return;

    std::vector<DeliveredLine>& lines = g_deliveredLines[botGuidCounter];

    // Stale records are dropped on the way in, so a bot that stopped speaking does not leave an entry
    // behind and the bound below is never spent on records that can no longer match.
    lines.erase(std::remove_if(lines.begin(), lines.end(), [nowUnixSeconds](DeliveredLine const& record)
                               { return DeliveredLineIsStale(record, nowUnixSeconds); }),
                lines.end());

    // Bounded by what a bot can actually have outstanding. Dropping the oldest keeps the newest
    // recognisable, which is the one whose echo has not arrived yet.
    if (lines.size() >= PLAYERBOT_SOCIAL_MAX_UNECHOED_LINES_PER_BOT)
        lines.erase(lines.begin());

    DeliveredLine record;
    record.text = text;
    record.eventPublicId = eventPublicId;
    record.replyToEventPublicId = replyToEventPublicId;
    record.sourceEventPublicId = sourceEventPublicId;
    record.atUnixSeconds = nowUnixSeconds;
    lines.push_back(std::move(record));
}

bool PlayerbotSocialWasDeliveredLine(uint64 botGuidCounter, std::string const& text, uint64 nowUnixSeconds,
                                     std::string* eventPublicId, std::string* replyToEventPublicId,
                                     std::string* sourceEventPublicId)
{
    auto const found = g_deliveredLines.find(botGuidCounter);
    if (found == g_deliveredLines.end())
        return false;

    std::vector<DeliveredLine>& lines = found->second;

    // Expired records are dropped on sight rather than swept on a timer.
    lines.erase(std::remove_if(lines.begin(), lines.end(), [nowUnixSeconds](DeliveredLine const& record)
                               { return DeliveredLineIsStale(record, nowUnixSeconds); }),
                lines.end());

    auto const match =
        std::find_if(lines.begin(), lines.end(), [&text](DeliveredLine const& record) { return record.text == text; });

    if (match == lines.end())
    {
        if (lines.empty())
            g_deliveredLines.erase(found);

        return false;
    }

    /*
     * ONE record consumed, not the whole entry. One delivery suppresses one echo, so a bot that said
     * the same thing twice has both echoes recognised and a genuine repetition later is still a real
     * opportunity rather than being silently swallowed.
     */
    if (eventPublicId != nullptr)
        *eventPublicId = match->eventPublicId;
    if (replyToEventPublicId != nullptr)
        *replyToEventPublicId = match->replyToEventPublicId;
    if (sourceEventPublicId != nullptr)
        *sourceEventPublicId = match->sourceEventPublicId;

    lines.erase(match);
    if (lines.empty())
        g_deliveredLines.erase(found);

    return true;
}

void PlayerbotSocialForgetDeliveredLines()
{
    g_deliveredLines.clear();
    g_lastSpoke.clear();
}

uint64 PlayerbotSocialLastSpokeAt(uint64 botGuidCounter)
{
    auto const found = g_lastSpoke.find(botGuidCounter);

    return found == g_lastSpoke.end() ? 0 : found->second;
}

void PlayerbotSocialRememberSpoke(uint64 botGuidCounter, uint64 nowUnixSeconds)
{
    if (botGuidCounter == 0)
        return;

    /*
     * Pruned on write rather than swept. Only the cooldown window matters, so an entry older than it
     * answers the same as no entry at all, and keeping it would grow this map by one row per bot that
     * has ever spoken for the life of the process.
     *
     * A stamp in the future is dropped for the same reason it is in the echo record: a clock that
     * stepped back leaves an entry that can never age out, and here that would silence the bot
     * indefinitely rather than merely fail to suppress an echo.
     */
    for (auto entry = g_lastSpoke.begin(); entry != g_lastSpoke.end();)
    {
        bool const inFuture = entry->second > nowUnixSeconds;
        bool const expired = !inFuture && nowUnixSeconds - entry->second > PLAYERBOT_SOCIAL_REPLY_COOLDOWN_SECONDS;

        if (inFuture || expired)
            entry = g_lastSpoke.erase(entry);
        else
            ++entry;
    }

    g_lastSpoke[botGuidCounter] = nowUnixSeconds;
}

void PlayerbotSocialPumpBiographies(uint32 diff)
{
    if (!PlayerbotSocialGateIsLive(PlayerbotSocialEffectiveGate()))
        return;

    /*
     * On its own interval rather than every tick. The whole point of this pump is that it is lazy:
     * scanning the online population sixty times a second to answer a question whose answer changes
     * once an hour is work the world thread should not be doing.
     *
     * Static for the same reason the starter cursor is: the world update is the only caller and is
     * world thread only.
     */
    static uint32 sinceLastPass = 0;
    sinceLastPass += diff;
    if (sinceLastPass < PLAYERBOT_SOCIAL_BIOGRAPHY_PUMP_INTERVAL_MS)
        return;

    sinceLastPass = 0;

    uint64 const nowUnixSeconds = static_cast<uint64>(time(nullptr));

    // Abandoned first, so a bot whose request was lost becomes eligible again in the same pass
    // rather than waiting a further interval for the sweep to catch up with the requester.
    sPlayerbotSocialMgr.ExpireTimedOutBiographyRequests(nowUnixSeconds);

    std::size_t requested = 0;
    for (auto const& entry : ObjectAccessor::GetPlayers())
    {
        if (requested >= PLAYERBOT_SOCIAL_BIOGRAPHY_REQUESTS_PER_PASS)
            break;

        Player* const bot = entry.second;
        if (bot == nullptr || !bot->IsInWorld())
            continue;

        /*
         * Machine bots only. A real player driving their own character through Playerbot has a
         * person behind them, and generating a player profile for that character would invent a
         * personality for somebody who did not ask for one.
         */
        PlayerbotAI* const botAI = GET_PLAYERBOT_AI(bot);
        if (botAI == nullptr || botAI->IsRealPlayer())
            continue;

        // Consent is checked here rather than inside the coordinator because it is the same
        // question the rest of the social path asks before doing anything on a character's behalf.
        if (sPlayerbotSocialMgr.State().IsOptedOut(bot->GetGUID().GetCounter()))
            continue;

        PlayerbotSocialBiographyCandidate candidate;
        candidate.botGuidCounter = bot->GetGUID().GetCounter();
        candidate.characterName = bot->GetName();
        candidate.raceId = bot->getRace();
        candidate.classId = bot->getClass();
        candidate.genderId = bot->getGender();

        if (sPlayerbotSocialMgr.RequestBiographyFor(candidate, nowUnixSeconds) != 0)
            ++requested;
    }
}

bool PlayerbotSocialQueueStarterSource(PlayerbotAI* sourceAI, PlayerbotSocialStarterSource source)
{
    PlayerbotSocialGate const gate = PlayerbotSocialEffectiveGate();
    if (sourceAI == nullptr || !PlayerbotSocialGateIsLive(gate) ||
        gate.stage == PlayerbotSocialRolloutStage::HumanReplies)
        return false;

    Player* const sourceBot = sourceAI->GetBot();
    if (sourceBot == nullptr || !sourceBot->IsInWorld() || sourceBot->InBattleground())
        return false;

    uint64 const nowUnixSeconds = static_cast<uint64>(time(nullptr));
    source.sourceEventPublicId = sPlayerbotSocialMgr.ReserveDeliveryEventPublicId(sourceBot->GetGUID().GetCounter());
    if (!PlayerbotSocialStarterSourceIsValid(source))
        return false;

    PlayerbotSocialStarterAudience audience;
    uint64 partyAudienceGuidCounter = 0;
    uint64 sayAudienceGuidCounter = 0;
    uint64 generalAudienceGuidCounter = 0;
    std::vector<uint64> sayCohortGuidCounters;
    sayCohortGuidCounters.push_back(sourceBot->GetGUID().GetCounter());

    for (auto const& [guid, candidate] : ObjectAccessor::GetPlayers())
    {
        if (candidate == nullptr || candidate == sourceBot || !candidate->IsInWorld() ||
            candidate->GetTeamId() != sourceBot->GetTeamId())
            continue;

        PlayerbotAI* const candidateAI = GET_PLAYERBOT_AI(candidate);
        bool const characterIsHuman = candidateAI == nullptr || candidateAI->IsRealPlayer();
        bool const sameMap = candidate->GetMap() == sourceBot->GetMap();
        bool const samePhase = (candidate->GetPhaseMask() & sourceBot->GetPhaseMask()) != 0;
        bool const visible = sameMap && samePhase && sourceBot->CanSeeOrDetect(candidate);
        bool const withinSay =
            visible && sourceBot->IsWithinDistInMap(candidate, sWorld->getFloatConfig(CONFIG_LISTEN_RANGE_SAY));
        if (withinSay)
            sayCohortGuidCounters.push_back(guid.GetCounter());

        if (!characterIsHuman || sPlayerbotSocialMgr.IsOptedOut(guid.GetCounter()))
            continue;

        if (sourceBot->GetGroup() != nullptr && candidate->GetGroup() == sourceBot->GetGroup() &&
            partyAudienceGuidCounter == 0)
            partyAudienceGuidCounter = guid.GetCounter();

        if (withinSay && sayAudienceGuidCounter == 0)
            sayAudienceGuidCounter = guid.GetCounter();

        if (candidate->GetZoneId() == sourceBot->GetZoneId() && generalAudienceGuidCounter == 0)
        {
            ChannelMgr* const channelMgr = ChannelMgr::forTeam(candidate->GetTeamId());
            if (channelMgr != nullptr)
                for (auto const& channelEntry : channelMgr->GetChannels())
                {
                    Channel* const joined = channelEntry.second;
                    if (joined != nullptr && joined->GetChannelId() == ChatChannelId::GENERAL &&
                        candidate->IsInChannel(joined))
                    {
                        generalAudienceGuidCounter = guid.GetCounter();
                        break;
                    }
                }
        }
    }

    audience.hasRealPartyMember = partyAudienceGuidCounter != 0;
    audience.hasRealSayListener = sayAudienceGuidCounter != 0;
    audience.hasRealGeneralMember = generalAudienceGuidCounter != 0;

    PlayerbotSocialStarterContext starter;
    if (!PlayerbotSocialSelectStarterChannel(audience, starter.key.channel))
        return false;

    starter.botGuidCounter = sourceBot->GetGUID().GetCounter();
    starter.source = std::move(source);
    starter.zoneId = sourceBot->GetZoneId();
    starter.atUnixSeconds = nowUnixSeconds;

    switch (starter.key.channel)
    {
        case PlayerbotSocialChannel::Party:
            starter.key.scopeId = sourceBot->GetGroup() == nullptr ? 0 : sourceBot->GetGroup()->GetGUID().GetCounter();
            starter.audienceGuidCounter = partyAudienceGuidCounter;
            break;
        case PlayerbotSocialChannel::Say:
            starter.key.scopeId = PlayerbotSocialResolveSayCohort(sayCohortGuidCounters, nowUnixSeconds);
            starter.audienceGuidCounter = sayAudienceGuidCounter;
            starter.sayCohortGuidCounters = std::move(sayCohortGuidCounters);
            break;
        case PlayerbotSocialChannel::General:
            starter.key.scopeId = sourceBot->GetZoneId();
            starter.audienceGuidCounter = generalAudienceGuidCounter;
            break;
        case PlayerbotSocialChannel::Whisper:
            return false;
    }

    PlayerbotSocialEventDraft sourceEvent;
    sourceEvent.eventPublicId = starter.source.sourceEventPublicId;
    sourceEvent.eventType = "social.source";
    sourceEvent.origin = PlayerbotSocialEventOrigin::Social;
    sourceEvent.outcome = PlayerbotSocialEventOutcome::Recorded;
    sourceEvent.channel = starter.key.channel;
    sourceEvent.hasChannel = true;
    sourceEvent.botGuidCounter = starter.botGuidCounter;
    sourceEvent.targetGuidCounter = starter.audienceGuidCounter;
    sourceEvent.zoneId = sourceBot->GetZoneId();
    sourceEvent.reason = PlayerbotSocialStarterSourceKindName(starter.source.kind);
    std::string diagnostics = "{\"subject_id\":" + std::to_string(starter.source.subjectId);
    if (starter.source.kind == PlayerbotSocialStarterSourceKind::QuestTransition)
    {
        diagnostics += ",\"quest_transition\":\"";
        diagnostics += PlayerbotSocialQuestTransitionName(starter.source.questTransition);
        diagnostics += '"';
    }
    diagnostics += '}';
    sourceEvent.diagnosticsJson = std::move(diagnostics);
    sourceEvent.occurredAtUnixSeconds = nowUnixSeconds;
    sPlayerbotSocialMgr.RecordEvent(std::move(sourceEvent));

    return sPlayerbotSocialMgr.NoteStarterContext(starter);
}

void PlayerbotSocialPumpStarters()
{
    PlayerbotSocialGate const gate = PlayerbotSocialEffectiveGate();
    if (!PlayerbotSocialGateIsLive(gate) || gate.stage == PlayerbotSocialRolloutStage::HumanReplies)
        return;

    uint64 const nowUnixSeconds = static_cast<uint64>(time(nullptr));

    /*
     * Where the last tick stopped. Static because the rotation has to survive between ticks to mean
     * anything, and the world update is the only caller and is world thread only.
     *
     * Consuming the starters is NOT a substitute for this. A busy low keyed zone refills between
     * ticks and takes the whole quota again, so scanning from the beginning every time can leave a
     * high keyed zone pending indefinitely behind a queue that never empties.
     */
    static PlayerbotSocialThreadKey servedThrough;

    for (PlayerbotSocialThreadKey const& key :
         sPlayerbotSocialMgr.ScopesWithPendingStarters(PLAYERBOT_SOCIAL_STARTER_SCOPES_PER_TICK, servedThrough))
    {
        // Advanced for every scope VISITED, not only the ones that manage to speak. A scope that
        // cannot produce a line this pass must not pin the cursor and block the ones behind it.
        servedThrough = key;

        /*
         * Taken before anything else can fail. Every exit below leaves this scope without its
         * starters, which is deliberate: retrying them would reopen the same subject on every tick
         * for as long as the window lasts, spending a provider request each time to say one thing.
         *
         * SHORTCUT: a scope where nobody could speak this pass loses its subjects rather than
         * keeping them. Carry them forward instead if zones are observed going quiet because their
         * bots happened to share a cooldown, which this cannot distinguish from having nothing to say.
         */
        std::vector<PlayerbotSocialStarterContext> const starters = sPlayerbotSocialMgr.TakeStarterContextsFor(key);
        if (starters.empty())
            continue;

        // The same freshest subject drives selection and generation, so the selected bot is
        // interested in the thing it will actually be asked to discuss.
        PlayerbotSocialStarterContext const& starter = starters.back();
        std::string const starterSubject = PlayerbotSocialStarterGroundingSubject(starter.source);

        PlayerbotSocialThreadHandle const thread = sPlayerbotSocialMgr.OpenStarterThread(starter, nowUnixSeconds);
        if (!thread.valid)
            continue;

        uint32 const zoneId = starter.zoneId;
        Player* const audience = ObjectAccessor::FindPlayerByLowGUID(starter.audienceGuidCounter);
        PlayerbotAI* const audienceAI = audience == nullptr ? nullptr : GET_PLAYERBOT_AI(audience);
        if (audience == nullptr || !audience->IsInWorld() || (audienceAI != nullptr && !audienceAI->IsRealPlayer()) ||
            sPlayerbotSocialMgr.IsOptedOut(starter.audienceGuidCounter))
            continue;

        /*
         * An authoritative starter is owned by the bot whose event created it. Letting another bot
         * compete would permit the selected responder to claim the source bot's event as its own.
         * Resolve the owner again at use time and fail closed if it is no longer a managed bot in
         * the exact scope where the source was admitted.
         */
        Player* const sourceBot =
            ObjectAccessor::FindPlayer(ObjectGuid::Create<HighGuid::Player>(starter.botGuidCounter));
        if (sourceBot == nullptr || !sourceBot->IsInWorld())
            continue;

        PlayerbotAI* const sourceAI = GET_PLAYERBOT_AI(sourceBot);
        if (sourceAI == nullptr || sourceAI->IsRealPlayer())
            continue;

        bool sourceInScope = false;
        switch (key.channel)
        {
            case PlayerbotSocialChannel::General:
                sourceInScope = sourceBot->GetZoneId() == zoneId;
                break;
            case PlayerbotSocialChannel::Party:
                sourceInScope =
                    sourceBot->GetGroup() != nullptr && sourceBot->GetGroup()->GetGUID().GetCounter() == key.scopeId;
                break;
            case PlayerbotSocialChannel::Say:
                sourceInScope = std::find(starter.sayCohortGuidCounters.begin(), starter.sayCohortGuidCounters.end(),
                                          starter.botGuidCounter) != starter.sayCohortGuidCounters.end();
                break;
            case PlayerbotSocialChannel::Whisper:
                break;
        }
        if (!sourceInScope)
            continue;

        std::vector<PlayerbotSocialActivationCandidate> candidates;
        std::vector<uint64> const participants = sPlayerbotSocialMgr.ParticipantsOf(thread);
        std::vector<uint64> const present = {starter.botGuidCounter};

        for (uint64 const botGuidCounter : present)
        {
            /*
             * Resolved again rather than carried as a pointer. Nothing between the walk above and
             * here can log a bot out, but the module's own rule is that a Player* is not held past
             * the call that produced it, and a starter tick is not the place to make an exception.
             */
            Player* const bot = ObjectAccessor::FindPlayer(ObjectGuid::Create<HighGuid::Player>(botGuidCounter));
            if (bot == nullptr || !bot->IsInWorld())
                continue;

            PlayerbotSocialActivationCandidate candidate;
            candidate.botGuidCounter = botGuidCounter;
            candidate.profileLoadState = sPlayerbotSocialMgr.ProfileLoadFor(botGuidCounter).state;
            std::optional<PlayerbotPersonalityProfile> const personality =
                sPlayerbotPersonalityMgr.GetOrCreate(botGuidCounter);
            if (!personality.has_value())
                continue;
            candidate.personality = *personality;

            /*
             * Scored against nobody. A starter has no speaker, so there is no relationship to read
             * and no stance to take toward one: the disposition falls back to the bot's own
             * sociability, which is exactly what "would this character speak up unprompted" asks.
             */
            uint8 const sociability = personality->sociability;
            PlayerbotSocialProfile const& profile = sPlayerbotSocialMgr.ProfileFor(botGuidCounter);
            PlayerbotSocialRelationshipValues const toward;
            candidate.effectiveDisposition =
                PlayerbotSocialEngagementDisposition(sociability, profile.traits.warmth, toward);
            candidate.stance = PlayerbotSocialStanceFor(candidate.effectiveDisposition, toward);

            // Nobody was addressed and nothing was said, so neither can be true of a starter.
            candidate.addressedByName = false;
            candidate.contentRelevance = PlayerbotPersonality::SocialContentRelevance(profile, starterSubject);

            candidate.optedOutOfInitiation = sPlayerbotSocialMgr.State().IsOptedOut(botGuidCounter);
            candidate.lastSpokeUnixSeconds = PlayerbotSocialLastSpokeAt(botGuidCounter);
            candidate.participatedInThread =
                std::find(participants.begin(), participants.end(), botGuidCounter) != participants.end();

            /*
             * Both true with no speaker to disagree with. Faction and language are relations between
             * two characters, and a starter addresses the room: the delivery path revalidates the
             * surface against the world before anything is actually said.
             */
            candidate.factionMatches = true;
            candidate.languageMatches = true;
            if (!candidate.optedOutOfInitiation)
                candidate.nearby = CaptureNearbySnapshot(bot, key.channel);
            candidate.grounding = CaptureGroundingEnvelope(bot, audience, thread, key.channel,
                                                           candidate.profileLoadState, nowUnixSeconds, starterSubject);

            candidates.push_back(candidate);
        }

        if (candidates.empty())
            continue;

        PlayerbotSocialThreadPressure const pressure = sPlayerbotSocialMgr.PressureFor(thread, nowUnixSeconds);

        PlayerbotSocialActivation activation;
        activation.thread = thread;
        activation.channel = key.channel;
        activation.starter = true;
        activation.starterSourceBotGuidCounter = starter.botGuidCounter;
        activation.starterAudienceGuidCounter = starter.audienceGuidCounter;
        activation.starterSourceEventPublicId = starter.source.sourceEventPublicId;

        /*
         * No speaker, and none invented. Zero is what says nobody spoke, and a starter must not be
         * refused by the speaker opt-out rule for a speaker that does not exist.
         */
        activation.speakerGuidCounter = 0;
        activation.speakerIsHuman = false;
        activation.speakerOptedOut = false;

        // Nothing was said, so nothing was repeated. Duplicate suppression answers about an observed
        // line, and there is none.
        activation.duplicateOfRecentMessage = false;

        /*
         * The freshest pending subject, which is the whole content of a starter: without it the
         * provider is told only that some bot wishes to speak and answers about nothing in
         * particular. Freshest rather than oldest because the contexts age out together and the most
         * recent thing that happened is the one worth remarking on.
         */
        activation.starterSubject = starterSubject;

        activation.zoneId = zoneId;
        activation.channelDensity = pressure.channelDensity;
        activation.threadLastActivityUnixSeconds = pressure.lastActivityUnixSeconds;
        activation.relevantHumanMessages = pressure.relevantHumanMessages;
        activation.consecutiveBotOnlyTurns = pressure.consecutiveBotOnlyTurns;
        activation.nowUnixSeconds = nowUnixSeconds;

        // Seeded from the thread and the moment, exactly as the reply path is, so two zones starting
        // in the same tick do not roll in lockstep.
        activation.selectionSeed =
            PlayerbotPersonality::SplitMix64(thread.threadId ^ (nowUnixSeconds << 1) ^ key.scopeId);

        activation.candidates = candidates;

        sPlayerbotSocialMgr.Activate(activation, gate.density);
    }
}

void PlayerbotSocialDeliverDue()
{
    uint64 const nowUnixSeconds = static_cast<uint64>(time(nullptr));

    /*
     * Abandoned first. A request the provider never answered holds one of a bot's few pending slots,
     * and clearing them before speaking means a bot that timed out can take part in this tick rather
     * than the next one.
     */
    sPlayerbotSocialMgr.ExpireTimedOutRequests(nowUnixSeconds);

    /*
     * Unanswered assessments in the same breath, and for a stronger reason: each one HOLDS an
     * activation. A sidecar that never answers must cost latency, not the conversation, and this
     * pump is the only production driver of that promise; the sweep resumes every still-current
     * held activation in ordinary mode.
     */
    sPlayerbotSocialMgr.ExpireTimedOutAssessments(nowUnixSeconds);

    uint64 const nowUnixMilliseconds = PlayerbotSocialUnixMilliseconds(GameTime::GetSystemTime());

    for (uint64 const token : sPlayerbotSocialMgr.DueDeliveries(nowUnixMilliseconds))
    {
        PlayerbotSocialPendingDelivery pending;
        if (!sPlayerbotSocialMgr.PendingDeliveryFor(token, pending))
            continue;

        Player* const bot = ObjectAccessor::FindPlayerByLowGUID(pending.botGuidCounter);
        Player* const target =
            pending.targetGuidCounter == 0 ? nullptr : ObjectAccessor::FindPlayerByLowGUID(pending.targetGuidCounter);
        Player* const subject =
            pending.subjectGuidCounter == 0 ? nullptr : ObjectAccessor::FindPlayerByLowGUID(pending.subjectGuidCounter);

        /*
         * Resolved BEFORE the request is consumed, and folded into the conditions rather than
         * checked after them. `CompleteDelivery` consumes the request whichever way it decides, so a
         * check that ran afterwards could only drop the line silently: the coordinator would have
         * recorded a completed delivery that nothing spoke.
         */
        PlayerbotAI* const botAI = bot != nullptr ? GET_PLAYERBOT_AI(bot) : nullptr;

        PlayerbotSocialDeliveryConditions const conditions =
            ObserveDeliveryConditions(bot, botAI, target, subject, pending);

        /*
         * The coordinator decides. Speaking before asking would deliver a line the world had already
         * refused, and asking without speaking on None would drop an answer that was still good.
         */
        if (sPlayerbotSocialMgr.CompleteDelivery(token, conditions) != PlayerbotSocialDeliveryRejection::None)
            continue;

        /*
         * The canonical seam, and the last link in the correlation chain. It carries the thread the
         * opportunity, selection and provider attempt all recorded, so a delivered line can be traced
         * back to the decision that produced it.
         *
         * Speaking and recording are ONE act here, through the same helper every direct producer
         * uses. They were two statements until Task 11C, which is exactly the shape that let fifty
         * nine other sites speak without recording: a rule written out beside a send is a rule the
         * next send forgets.
         *
         * The tick's own clock is passed rather than left to the helper, so a line and the echo
         * record below agree to the second.
         */
        PlayerbotSocialDeliveryRequest request;
        request.channel = pending.channel;
        request.origin = PlayerbotSocialEventOrigin::Social;
        request.languageId =
            bot != nullptr ? PlayerbotSocialSpokenLanguageFor(bot->GetTeamId()) : static_cast<uint32>(LANG_UNIVERSAL);
        request.text = pending.result.text;
        request.threadPublicId = pending.threadPublicId;
        request.replyToEventPublicId = pending.replyToEventPublicId;
        request.sourceEventPublicId = pending.sourceEventPublicId;
        request.isEmote = pending.result.kind == PlayerbotSocialOutputKind::Emote;
        request.emoteId = pending.result.emoteId;
        request.callMetadata = pending.result.callMetadata;
        request.operatorEvidence = pending.operatorEvidence;
        request.retainTelemetry = !pending.statelessDirectReply;
        request.occurredAtUnixSeconds = nowUnixSeconds;

        if (!PlayerbotSocialDeliver(bot, target, request))
        {
            /*
             * The world took the request and then refused the send: not in the channel, no group, a
             * whisper target that just vanished. The request is already consumed, which is correct
             * because a result is delivered once or not at all, but a silent drop here would look
             * identical to a line that was spoken.
             */
            LOG_DEBUG("playerbots", "Social delivery {} for bot {} was revalidated but the send failed on channel {}",
                      token, pending.botGuidCounter, static_cast<uint32>(pending.channel));
            continue;
        }

        // Recorded only on a send the world accepted, so a refusal cannot start a speech cooldown.
        PlayerbotSocialRememberSpoke(pending.botGuidCounter, nowUnixSeconds);
    }
}

bool PlayerbotSocialCaptureChat(PlayerbotAI* botAI, PlayerbotSocialInboundDecision const& decision,
                                ObjectGuid speakerGuid, uint32 languageId, std::string_view message,
                                std::string_view eventPublicId, uint64 sayCohortScopeId,
                                std::string_view replyToEventPublicId, std::string_view sourceEventPublicId)
{
    if (botAI == nullptr)
        return false;

    // Allowed rather than excluded. A suppressed surface also skips the legacy reply, so testing for
    // that alone would let World or yell chat into the coordinator through the back door.
    if (decision.route != PlayerbotSocialInboundRoute::SocialOpportunity &&
        decision.route != PlayerbotSocialInboundRoute::ThreadContinuationOnly)
        return false;

    /*
     * Chat packets reach bot sessions after the authoritative PlayerScript callback has closed its
     * listener fanout. Observing them here would add the same game line once per recipient and still
     * produce no candidates, because activation is intentionally dispatch scoped. Only the callback
     * that owns the complete listener set may capture.
     */
    if (!PlayerbotSocialDispatchIsOpen())
        return false;

    Player* const bot = botAI->GetBot();
    if (bot == nullptr)
        return false;

    // Resolved once, here, and never stored. Everything below reduces to plain values.
    Player* const speaker = ObjectAccessor::FindPlayer(speakerGuid);
    if (speaker == nullptr)
        return false;

    PlayerbotSocialCapturedMessage captured;
    captured.eventPublicId = eventPublicId;
    captured.replyToEventPublicId = replyToEventPublicId;
    captured.sourceEventPublicId = sourceEventPublicId;
    captured.channel = decision.channel;
    captured.speakerGuidCounter = speakerGuid.GetCounter();
    captured.speakerName = speaker->GetName();

    // A character driven by the module is a bot however it was created, which is a broader and more
    // accurate test than asking whether its account is one of the random bot accounts.
    captured.speakerIsHuman = GET_PLAYERBOT_AI(speaker) == nullptr;

    captured.languageId = languageId;
    captured.atUnixSeconds = static_cast<uint64>(time(nullptr));
    captured.text = message;

    switch (decision.channel)
    {
        case PlayerbotSocialChannel::General:
            captured.zoneId = bot->GetZoneId();
            break;
        case PlayerbotSocialChannel::Say:
            captured.zoneId = bot->GetZoneId();
            captured.sayCohortScopeId = sayCohortScopeId;

            /*
             * Say is the one positional surface. The server already limits who receives it, but the
             * distance is recomputed here rather than assumed: a message the bot could not have heard
             * must not become something it knows, and a speaker who has since moved away or is on
             * another map fails this check rather than slipping through.
             */
            captured.withinHearingRange =
                bot->IsWithinDistInMap(speaker, sWorld->getFloatConfig(CONFIG_LISTEN_RANGE_SAY));
            break;
        case PlayerbotSocialChannel::Party:
            /*
             * SHORTCUT: a party message is captured as conversation only. Nothing here distinguishes
             * "pull the next group" from "nice pull", so a party line that is really a request to act
             * produces talk rather than the action. Split the two once gameplay action requests are
             * recognized, which is the point at which answering in words alone becomes wrong.
             */
            captured.groupId = bot->GetGroup() ? bot->GetGroup()->GetGUID().GetCounter() : 0;
            captured.zoneId = bot->GetZoneId();
            break;
        case PlayerbotSocialChannel::Whisper:
            // The bot is the addressed side of a whisper it received.
            captured.targetGuidCounter = bot->GetGUID().GetCounter();
            captured.zoneId = bot->GetZoneId();
            break;
        default:
            return false;
    }

    if (PlayerbotSocialValidateCapture(captured) != PlayerbotSocialCaptureRejection::None)
        return false;

    PlayerbotSocialObservation const observation = PlayerbotSocialObservationFor(captured);
    PlayerbotSocialThreadHandle const thread = PlayerbotSocialObserveOncePerDispatch(observation);
    if (!thread.valid)
        return false;

    /*
     * A continuation updates the thread and stops. This is the narrow race where the feature spoke
     * immediately before an operator silenced the channel: the delivered line remains real history,
     * but the newly effective stop prevents another opportunity.
     */
    if (decision.route == PlayerbotSocialInboundRoute::ThreadContinuationOnly)
        return true;

    uint64 const botGuidCounter = bot->GetGUID().GetCounter();

    /*
     * A bot never answers itself. The speaker is one of the listeners on every broadcast surface, so
     * without this a bot's own line would put it in its own candidate field.
     */
    if (botGuidCounter == captured.speakerGuidCounter)
        return true;

    PlayerbotSocialRelationshipKey relationship;
    relationship.botGuidCounter = botGuidCounter;
    relationship.subjectGuidCounter = captured.speakerGuidCounter;

    // Async and non-blocking. The first encounter may use the neutral snapshot, while later
    // opportunities use the durable directional relationship after its callback lands.
    sPlayerbotSocialMgr.LoadRelationship(botGuidCounter, captured.speakerGuidCounter, captured.atUnixSeconds);

    PlayerbotSocialRelationshipValues const toward = sPlayerbotSocialMgr.State().RecallRelationship(relationship);

    // A personality row is required before this bot may enter Social. The manager serves the cached
    // row on the normal path and creates one only for a genuinely new bot.
    std::optional<PlayerbotPersonalityProfile> const personality = sPlayerbotPersonalityMgr.GetOrCreate(botGuidCounter);
    if (!personality.has_value())
        return true;

    uint8 const sociability = personality->sociability;
    PlayerbotSocialProfile const& profile = sPlayerbotSocialMgr.ProfileFor(botGuidCounter);

    PlayerbotSocialActivationCandidate candidate;
    candidate.botGuidCounter = botGuidCounter;
    candidate.profileLoadState = sPlayerbotSocialMgr.ProfileLoadFor(botGuidCounter).state;
    candidate.personality = *personality;
    candidate.effectiveDisposition = PlayerbotSocialEngagementDisposition(sociability, profile.traits.warmth, toward);
    candidate.stance = PlayerbotSocialStanceFor(candidate.effectiveDisposition, toward);
    candidate.addressedByName = message.find(bot->GetName()) != std::string_view::npos;
    candidate.askedQuestion = PlayerbotSocialMessageIsQuestion(message);
    candidate.optedOutOfInitiation = sPlayerbotSocialMgr.State().IsOptedOut(botGuidCounter);

    /*
     * Without this the opportunity gate compares against zero, which is always outside the reply
     * cooldown, so the cooldown never fires and a bot answers every message it hears. That is the
     * behaviour the cooldown exists to prevent, and it fails in the direction that looks like the
     * feature working rather than like it being broken.
     */
    candidate.lastSpokeUnixSeconds = PlayerbotSocialLastSpokeAt(botGuidCounter);

    // Captured live while the bot is resolved. A fighting bot never receives an authorized
    // roleplay generation; its ordinary social behavior is unchanged.
    candidate.inCombat = bot->IsInCombat();

    candidate.contentRelevance = PlayerbotPersonality::SocialContentRelevance(profile, message);

    std::vector<uint64> const participants = sPlayerbotSocialMgr.ParticipantsOf(thread);
    candidate.participatedInThread =
        std::find(participants.begin(), participants.end(), botGuidCounter) != participants.end();

    /*
     * Faction and language are relations between this bot and the speaker, decided from the live
     * objects while both are still resolved. Only universal speech crosses the faction line, which is
     * the game's own rule rather than a policy this feature invents.
     */
    candidate.factionMatches = bot->GetTeamId() == speaker->GetTeamId();
    candidate.languageMatches = languageId == LANG_UNIVERSAL || candidate.factionMatches;
    if (!candidate.optedOutOfInitiation && candidate.factionMatches && candidate.languageMatches)
        candidate.nearby = CaptureNearbySnapshot(bot, captured.channel);
    candidate.grounding = CaptureGroundingEnvelope(bot, speaker, thread, captured.channel, candidate.profileLoadState,
                                                   captured.atUnixSeconds);

    PlayerbotSocialThreadKey const key = observation.key;
    DispatchGroup& group = g_dispatchGroups[key];
    group.thread = thread;
    group.channel = captured.channel;
    group.speakerGuidCounter = captured.speakerGuidCounter;
    group.speakerIsHuman = captured.speakerIsHuman;
    group.zoneId = captured.zoneId;
    group.atUnixSeconds = captured.atUnixSeconds;
    group.candidates.push_back(candidate);

    return true;
}

namespace
{
void CaptureGeneratedAudience(Player* speaker, PlayerbotSocialDeliveryRequest const& request)
{
    if (speaker == nullptr || request.origin != PlayerbotSocialEventOrigin::Social)
        return;

    PlayerbotSocialGate const gate = PlayerbotSocialEffectiveGate();
    PlayerbotSocialDispatchScope const dispatch;

    for (auto const& [guid, listener] : ObjectAccessor::GetPlayers())
    {
        if (listener == nullptr || !listener->IsInWorld())
            continue;

        PlayerbotAI* const listenerAI = GET_PLAYERBOT_AI(listener);
        if (listenerAI == nullptr || listenerAI->IsRealPlayer())
            continue;

        bool const eligibleAudience =
            request.channel == PlayerbotSocialChannel::General
                ? listener->GetZoneId() == speaker->GetZoneId() && listenerAI->IsOnChannel(ChatChannelId::GENERAL)
                : speaker->GetGroup() != nullptr && listener->GetGroup() == speaker->GetGroup();
        if (!eligibleAudience)
            continue;

        PlayerbotSocialInboundContext context;
        context.eventPublicId = request.eventPublicId;
        context.listenerInBattleground = listener->InBattleground();
        context.originatedFromSocialDelivery = true;

        ChatChannelSource const source = request.channel == PlayerbotSocialChannel::General
                                             ? ChatChannelSource::SRC_GENERAL
                                             : ChatChannelSource::SRC_PARTY;
        PlayerbotSocialInboundDecision const decision = PlayerbotSocialRouteInbound(source, context, gate);
        PlayerbotSocialCaptureChat(listenerAI, decision, speaker->GetGUID(), request.languageId, request.text,
                                   context.eventPublicId, 0, request.replyToEventPublicId, request.sourceEventPublicId);
    }
}
}  // namespace

char const* PlayerbotSocialBroadcastRouteName(PlayerbotSocialBroadcastRoute route)
{
    switch (route)
    {
        case PlayerbotSocialBroadcastRoute::DeliverAsToday:
            return "deliver_as_today";
        case PlayerbotSocialBroadcastRoute::SuppressCannedDelivery:
            return "suppress_canned_delivery";
        case PlayerbotSocialBroadcastRoute::StarterContext:
            return "starter_context";
    }

    return "unknown";
}

PlayerbotSocialBroadcastRoute PlayerbotSocialRouteBroadcast(BroadcastHelper::ToChannel destination,
                                                            PlayerbotSocialGate const& gate)
{
    if (!gate.enabled)
        return PlayerbotSocialBroadcastRoute::DeliverAsToday;

    /*
     * Paused, or General silenced: the canned line is still dropped, but nothing converts it into a
     * conversation starter either. Both halves of silence, and in this order deliberately. Returning
     * DeliverAsToday here would make an operator pause turn the canned broadcast chatter back on,
     * which is the behaviour the pause was almost certainly reaching for the opposite of.
     */
    bool const generalSilenced =
        gate.paused || !gate.channelEnabled[static_cast<std::size_t>(PlayerbotSocialChannel::General)];

    if (destination == BroadcastHelper::TO_GENERAL)
    {
        return generalSilenced || gate.stage == PlayerbotSocialRolloutStage::HumanReplies
                   ? PlayerbotSocialBroadcastRoute::SuppressCannedDelivery
                   : PlayerbotSocialBroadcastRoute::StarterContext;
    }

    /*
     * Everything else, including a destination this build does not know about. Suppressing an
     * unrecognized destination is the fail closed direction: the alternative would let a channel
     * added upstream keep broadcasting canned lines that this feature is supposed to have replaced.
     */
    return PlayerbotSocialBroadcastRoute::SuppressCannedDelivery;
}
