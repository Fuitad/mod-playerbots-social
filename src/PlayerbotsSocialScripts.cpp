#include <ctime>
#include <string>
#include <utility>
#include <vector>

#include "Bot/Extension/PlayerbotExtension.h"
#include "Bot/Social/PlayerbotSocialConfig.h"
#include "Bot/Social/PlayerbotSocialControlWork.h"
#include "Bot/Social/PlayerbotSocialMgr.h"
#include "Bot/Social/PlayerbotSocialRoute.h"
#include "Channel.h"
#include "GameTime.h"
#include "Group.h"
#include "ObjectAccessor.h"
#include "Player.h"
#include "PlayerScript.h"
#include "PlayerbotAI.h"
#include "Playerbots.h"
#include "ScriptMgr.h"
#include "World.h"
#include "WorldScript.h"

namespace
{
PlayerbotSocialStarterSource SocialStarterSource(PlayerbotEvent const& event)
{
    PlayerbotSocialStarterSource source;
    source.subjectId = event.subjectId;
    source.subject = event.subject;

    switch (event.type)
    {
        case PlayerbotEventType::Loot:
            source.kind = PlayerbotSocialStarterSourceKind::Loot;
            break;
        case PlayerbotEventType::Kill:
            source.kind = PlayerbotSocialStarterSourceKind::Kill;
            break;
        case PlayerbotEventType::Level:
            source.kind = PlayerbotSocialStarterSourceKind::Level;
            break;
        case PlayerbotEventType::QuestAccepted:
            source.kind = PlayerbotSocialStarterSourceKind::QuestTransition;
            source.questTransition = PlayerbotSocialQuestTransition::Accepted;
            break;
        case PlayerbotEventType::QuestObjectiveProgress:
            source.kind = PlayerbotSocialStarterSourceKind::QuestTransition;
            source.questTransition = PlayerbotSocialQuestTransition::ObjectiveProgress;
            break;
        case PlayerbotEventType::QuestObjectiveCompleted:
            source.kind = PlayerbotSocialStarterSourceKind::QuestTransition;
            source.questTransition = PlayerbotSocialQuestTransition::ObjectiveCompleted;
            break;
        case PlayerbotEventType::QuestFailed:
            source.kind = PlayerbotSocialStarterSourceKind::QuestTransition;
            source.questTransition = PlayerbotSocialQuestTransition::Failed;
            break;
        case PlayerbotEventType::QuestCompleted:
            source.kind = PlayerbotSocialStarterSourceKind::QuestTransition;
            source.questTransition = PlayerbotSocialQuestTransition::Completed;
            break;
        case PlayerbotEventType::QuestTurnedIn:
            source.kind = PlayerbotSocialStarterSourceKind::QuestTransition;
            source.questTransition = PlayerbotSocialQuestTransition::TurnedIn;
            break;
    }

    return source;
}

class PlayerbotsSocialExtension final : public PlayerbotExtension
{
public:
    bool HandleRemoteCommand(std::string_view command, std::string& response) override
    {
        return PlayerbotSocialControlHandleLine(command, response);
    }

    bool HandleBotEvent(PlayerbotAI* botAI, PlayerbotEvent const& event) override
    {
        PlayerbotSocialGate const gate = PlayerbotSocialEffectiveGate();
        if (!gate.enabled)
            return false;

        if (PlayerbotSocialGateIsLive(gate) && gate.stage != PlayerbotSocialRolloutStage::HumanReplies)
        {
            [[maybe_unused]] bool const queued = PlayerbotSocialQueueStarterSource(botAI, SocialStarterSource(event));
        }

        return true;
    }

    void OnBotPurge(std::vector<std::uint32_t> const& botGuids) override
    {
        std::vector<uint64> cohort;
        cohort.reserve(botGuids.size());
        for (std::uint32_t guid : botGuids)
            cohort.push_back(guid);
        sPlayerbotSocialMgr.ForgetBotCohort(cohort);
    }
};

bool IsSocialBotListener(Player* player)
{
    if (!player || !player->IsInWorld())
        return false;
    PlayerbotAI* const botAI = GET_PLAYERBOT_AI(player);
    return botAI && !botAI->IsRealPlayer();
}

void CaptureSocialListener(PlayerbotAI* botAI, Player* speaker, uint32 type, uint32 language,
                           std::string const& message, std::string const& channelName,
                           bool originatedFromSocialDelivery, std::string_view eventPublicId,
                           PlayerbotSocialGate const& gate, uint64 sayCohortScopeId = 0,
                           std::string_view replyToEventPublicId = {}, std::string_view sourceEventPublicId = {})
{
    if (!botAI || botAI->IsRealPlayer() || !speaker)
        return;

    PlayerbotAI* const speakerAI = GET_PLAYERBOT_AI(speaker);
    if (!PlayerbotSocialSpeakerCanOpenOpportunity(!speakerAI || speakerAI->IsRealPlayer(),
                                                  originatedFromSocialDelivery))
        return;

    Player* const bot = botAI->GetBot();
    if (!bot)
        return;

    PlayerbotSocialInboundContext context;
    context.eventPublicId = eventPublicId;
    context.listenerInBattleground = bot->InBattleground();
    context.originatedFromSocialDelivery = originatedFromSocialDelivery;
    context.machineTraffic = language == LANG_ADDON;
    ChatChannelSource const source = botAI->GetChatChannelSource(bot, type, channelName);
    PlayerbotSocialInboundDecision const decision = PlayerbotSocialRouteInbound(source, context, gate);
    PlayerbotSocialCaptureChat(botAI, decision, speaker->GetGUID(), language, message, context.eventPublicId,
                               sayCohortScopeId, replyToEventPublicId, sourceEventPublicId);
}

class PlayerbotsSocialPlayerScript final : public PlayerScript
{
public:
    PlayerbotsSocialPlayerScript()
        : PlayerScript("PlayerbotsSocialPlayerScript",
                       {PLAYERHOOK_ON_LOGIN, PLAYERHOOK_ON_LOGOUT, PLAYERHOOK_CAN_PLAYER_USE_CHAT,
                        PLAYERHOOK_CAN_PLAYER_USE_PRIVATE_CHAT, PLAYERHOOK_CAN_PLAYER_USE_GROUP_CHAT,
                        PLAYERHOOK_CAN_PLAYER_USE_CHANNEL_CHAT})
    {
    }

    void OnPlayerLogin(Player* player) override
    {
        if (!player || !player->GetSession())
            return;
        sPlayerbotSocialMgr.LoadConsent(player->GetGUID().GetCounter());
        sPlayerbotSocialMgr.TouchActor(player->GetGUID().GetCounter(), player->GetName(),
                                       player->GetSession()->IsBot());
        if (player->GetSession()->IsBot())
            sPlayerbotSocialMgr.LoadProfile(player->GetGUID().GetCounter());
    }

    void OnPlayerLogout(Player* player) override
    {
        if (player)
            sPlayerbotSocialMgr.ForgetConsent(player->GetGUID().GetCounter());
    }

    bool OnPlayerCanUseChat(Player* player, uint32 type, uint32 language, std::string& message) override
    {
        if (!player || type != CHAT_MSG_SAY)
            return true;
        PlayerbotSocialGate const gate = PlayerbotSocialEffectiveGate();
        if (!gate.enabled)
            return true;

        uint64 const now = static_cast<uint64>(std::time(nullptr));
        std::string eventPublicId;
        std::string replyToEventPublicId;
        std::string sourceEventPublicId;
        bool const delivered = PlayerbotSocialWasDeliveredLine(
            player->GetGUID().GetCounter(), message, now, &eventPublicId, &replyToEventPublicId, &sourceEventPublicId);
        std::vector<uint64> hearingCohort;
        for (auto const& [guid, listener] : ObjectAccessor::GetPlayers())
            if (listener && listener->IsInWorld() &&
                listener->IsWithinDistInMap(player, sWorld->getFloatConfig(CONFIG_LISTEN_RANGE_SAY)))
                hearingCohort.push_back(listener->GetGUID().GetCounter());
        uint64 const scope = PlayerbotSocialResolveSayCohort(std::move(hearingCohort), now);
        PlayerbotSocialDispatchScope const dispatch;
        for (auto const& [guid, listener] : ObjectAccessor::GetPlayers())
            if (IsSocialBotListener(listener))
                CaptureSocialListener(GET_PLAYERBOT_AI(listener), player, type, language, message, "", delivered,
                                      eventPublicId, gate, scope, replyToEventPublicId, sourceEventPublicId);
        return true;
    }

    bool OnPlayerCanUseChat(Player* player, uint32 type, uint32 language, std::string& message,
                            Player* receiver) override
    {
        if (type != CHAT_MSG_WHISPER || !receiver)
            return true;
        PlayerbotAI* const botAI = PlayerbotsMgr::instance().GetPlayerbotAI(receiver);
        if (!botAI)
            return true;
        PlayerbotSocialGate const gate = PlayerbotSocialEffectiveGate();
        std::string eventPublicId;
        std::string replyToEventPublicId;
        std::string sourceEventPublicId;
        bool const delivered =
            player && PlayerbotSocialWasDeliveredLine(player->GetGUID().GetCounter(), message,
                                                      static_cast<uint64>(std::time(nullptr)), &eventPublicId,
                                                      &replyToEventPublicId, &sourceEventPublicId);
        PlayerbotSocialDispatchScope const dispatch;
        CaptureSocialListener(botAI, player, type, language, message, "", delivered, eventPublicId, gate, 0,
                              replyToEventPublicId, sourceEventPublicId);
        return true;
    }

    bool OnPlayerCanUseChat(Player* player, uint32 type, uint32 language, std::string& message, Group* group) override
    {
        if (!group)
            return true;
        PlayerbotSocialGate const gate = PlayerbotSocialEffectiveGate();
        std::string eventPublicId;
        std::string replyToEventPublicId;
        std::string sourceEventPublicId;
        bool const delivered =
            player && PlayerbotSocialWasDeliveredLine(player->GetGUID().GetCounter(), message,
                                                      static_cast<uint64>(std::time(nullptr)), &eventPublicId,
                                                      &replyToEventPublicId, &sourceEventPublicId);
        PlayerbotSocialDispatchScope const dispatch;
        for (GroupReference* reference = group->GetFirstMember(); reference; reference = reference->next())
        {
            Player* const member = reference->GetSource();
            if (PlayerbotAI* botAI = member ? PlayerbotsMgr::instance().GetPlayerbotAI(member) : nullptr)
                CaptureSocialListener(botAI, player, type, language, message, "", delivered, eventPublicId, gate, 0,
                                      replyToEventPublicId, sourceEventPublicId);
        }
        return true;
    }

    bool OnPlayerCanUseChat(Player* player, uint32 type, uint32 language, std::string& message,
                            Channel* channel) override
    {
        if (!channel || channel->GetChannelId() != ChatChannelId::GENERAL)
            return true;
        PlayerbotSocialGate const gate = PlayerbotSocialEffectiveGate();
        std::string eventPublicId;
        std::string replyToEventPublicId;
        std::string sourceEventPublicId;
        bool const delivered =
            player && PlayerbotSocialWasDeliveredLine(player->GetGUID().GetCounter(), message,
                                                      static_cast<uint64>(std::time(nullptr)), &eventPublicId,
                                                      &replyToEventPublicId, &sourceEventPublicId);
        PlayerbotSocialDispatchScope const dispatch;
        for (auto const& [guid, listener] : ObjectAccessor::GetPlayers())
            if (IsSocialBotListener(listener) && listener->IsInChannel(channel))
                CaptureSocialListener(GET_PLAYERBOT_AI(listener), player, type, language, message, channel->GetName(),
                                      delivered, eventPublicId, gate, 0, replyToEventPublicId, sourceEventPublicId);
        return true;
    }
};

class PlayerbotsSocialWorldScript final : public WorldScript
{
public:
    PlayerbotsSocialWorldScript()
        : WorldScript("PlayerbotsSocialWorldScript",
                      {WORLDHOOK_ON_AFTER_CONFIG_LOAD, WORLDHOOK_ON_BEFORE_WORLD_INITIALIZED, WORLDHOOK_ON_UPDATE,
                       WORLDHOOK_ON_SHUTDOWN})
    {
    }

    void OnAfterConfigLoad(bool) override { ReloadPlayerbotSocialConfig(); }

    void OnBeforeWorldInitialized() override
    {
        sPlayerbotSocialMgr.LoadRuntimeControl();
        SetPlayerbotSocialControlAcceptingRequests(true);
    }

    void OnUpdate(uint32 diff) override
    {
        sPlayerbotSocialMgr.UpdateDatabaseWork(diff);
        PlayerbotSocialPumpStarters();
        PlayerbotSocialPumpBiographies(diff);
        PlayerbotSocialDeliverDue();
    }

    void OnShutdown() override { SetPlayerbotSocialControlAcceptingRequests(false); }
};
}  // namespace

void AddPlayerbotsSocialScripts()
{
    static PlayerbotsSocialExtension extension;
    GetPlayerbotExtensionRegistry().Register(extension);
    new PlayerbotsSocialPlayerScript();
    new PlayerbotsSocialWorldScript();
}
