/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#include "Bot/Social/PlayerbotSocialMgr.h"

#include "Bot/Personality/PlayerbotPersonalityMgr.h"
#include "Bot/Social/PlayerbotSocialDatabase.h"
#include "Bot/Social/PlayerbotSocialPersonality.h"

// For the configured gate the stored controls are seeded from. The manager does not route, but the
// values it seeds a first control row with are exactly the ones the router already derives from
// configuration, and deriving them a second time here is how the two would come to disagree.
#include <algorithm>
#include <cctype>
#include <iterator>

#include "AsyncCallbackProcessor.h"
#include "Bot/Social/PlayerbotSocialConfig.h"
#include "Bot/Social/PlayerbotSocialRoute.h"
#include "DatabaseEnv.h"
#include "Field.h"
#include "GameTime.h"
#include "Log.h"
#include "QueryCallback.h"
#include "QueryResult.h"

namespace
{
void AppendJsonString(std::string& out, std::string const& value);

char const* EvidenceSubjectName(PlayerbotSocialEvidenceSubjectRole role)
{
    switch (role)
    {
        case PlayerbotSocialEvidenceSubjectRole::CandidateBot:
            return "candidate_bot";
        case PlayerbotSocialEvidenceSubjectRole::Participant:
            return "participant";
        case PlayerbotSocialEvidenceSubjectRole::Source:
            return "source";
    }

    return nullptr;
}

char const* EvidenceFactName(PlayerbotSocialEvidenceFactKind fact)
{
    switch (fact)
    {
        case PlayerbotSocialEvidenceFactKind::Name:
            return "name";
        case PlayerbotSocialEvidenceFactKind::Race:
            return "race";
        case PlayerbotSocialEvidenceFactKind::CharacterClass:
            return "character_class";
        case PlayerbotSocialEvidenceFactKind::Level:
            return "level";
        case PlayerbotSocialEvidenceFactKind::Faction:
            return "faction";
        case PlayerbotSocialEvidenceFactKind::Zone:
            return "zone";
        case PlayerbotSocialEvidenceFactKind::Area:
            return "area";
        case PlayerbotSocialEvidenceFactKind::GroupRelation:
            return "group_relation";
        case PlayerbotSocialEvidenceFactKind::GuildRelation:
            return "guild_relation";
        case PlayerbotSocialEvidenceFactKind::CombatState:
            return "combat_state";
        case PlayerbotSocialEvidenceFactKind::Target:
            return "target";
        case PlayerbotSocialEvidenceFactKind::Visibility:
            return "visibility";
        case PlayerbotSocialEvidenceFactKind::Proximity:
            return "proximity";
        case PlayerbotSocialEvidenceFactKind::Progression:
            return "progression";
        case PlayerbotSocialEvidenceFactKind::Quest:
            return "quest";
        case PlayerbotSocialEvidenceFactKind::Item:
            return "item";
        case PlayerbotSocialEvidenceFactKind::Creature:
            return "creature";
        case PlayerbotSocialEvidenceFactKind::Objective:
            return "objective";
        case PlayerbotSocialEvidenceFactKind::Achievement:
            return "achievement";
    }

    return nullptr;
}

char const* EvidenceProvenanceName(PlayerbotSocialEvidenceProvenance provenance)
{
    switch (provenance)
    {
        case PlayerbotSocialEvidenceProvenance::CurrentWorld:
            return "current_world";
        case PlayerbotSocialEvidenceProvenance::HumanObservation:
            return "human_observation";
        case PlayerbotSocialEvidenceProvenance::AuthoritativeSource:
            return "authoritative_source";
    }

    return nullptr;
}

char const* MemoryInputStateName(PlayerbotSocialMemoryInputState state)
{
    switch (state)
    {
        case PlayerbotSocialMemoryInputState::Pending:
            return "pending";
        case PlayerbotSocialMemoryInputState::Loaded:
            return "loaded";
        case PlayerbotSocialMemoryInputState::Absent:
            return "absent";
        case PlayerbotSocialMemoryInputState::Unavailable:
            return "unavailable";
    }

    return nullptr;
}

bool EvidenceIdIsSafe(std::string_view id)
{
    if (id.empty() || id.size() > 16)
        return false;

    return std::all_of(id.begin(), id.end(),
                       [](unsigned char symbol) { return std::isalnum(symbol) != 0 || symbol == '_'; });
}

void AppendOperatorEvidenceField(std::string& diagnostics, PlayerbotSocialOperatorEvidence const& evidence)
{
    if (diagnostics.size() > 1)
        diagnostics += ',';

    diagnostics += "\"evidence\":";
    std::optional<std::string> const encoded = PlayerbotSocialSerializeOperatorEvidence(evidence);
    diagnostics += encoded.value_or("{\"state\":\"unavailable\"}");
}

// Distinct namespaces so a thread identity and an event identity derived from the same counter
// cannot collide or be correlated with each other.
constexpr uint64 THREAD_ID_NAMESPACE = 0x54485245414449ULL;
constexpr uint64 EVENT_ID_NAMESPACE = 0x4556454E544944ULL;

// The frozen public identity shape is a prefix, an underscore, and exactly
// PLAYERBOT_SOCIAL_PUBLIC_ID_BODY_LENGTH lowercase hex characters. Two 64 bit words render to
// exactly 32, which is what that constant is.
std::string RenderPublicId(PlayerbotSocialIdKind kind, uint64 high, uint64 low)
{
    static constexpr char HEX[] = "0123456789abcdef";

    std::string_view const prefix = PlayerbotSocialPublicIdPrefix(kind);
    std::string text;
    text.reserve(prefix.size() + 1 + PLAYERBOT_SOCIAL_PUBLIC_ID_BODY_LENGTH);
    text.append(prefix);
    text.push_back('_');

    for (uint64 word : {high, low})
        for (int shift = 60; shift >= 0; shift -= 4)
            text.push_back(HEX[(word >> shift) & 0xF]);

    return text;
}

std::string MakeThreadPublicId(uint64 threadId, PlayerbotSocialThreadKey const& key)
{
    // Mixed rather than packed. The scope now uses the whole 64 bits, so there is no free field
    // left to lay the channel into beside it without one overwriting the other.
    uint64 const salt =
        PlayerbotPersonality::SplitMix64(key.scopeId ^ (static_cast<uint64>(static_cast<uint8>(key.channel)) + 1));
    uint64 const high = PlayerbotPersonality::SplitMix64(threadId ^ THREAD_ID_NAMESPACE);
    uint64 const low = PlayerbotPersonality::SplitMix64(high ^ salt);
    return RenderPublicId(PlayerbotSocialIdKind::Thread, high, low);
}

std::string MakeEventPublicId(uint64 sequence, uint64 speakerGuidCounter)
{
    uint64 const high = PlayerbotPersonality::SplitMix64(sequence ^ EVENT_ID_NAMESPACE);
    uint64 const low = PlayerbotPersonality::SplitMix64(high ^ speakerGuidCounter);
    return RenderPublicId(PlayerbotSocialIdKind::Event, high, low);
}

constexpr uint64 RECENT_LINE_NAMESPACE = 0x4C494E4548415348ULL;

/*
 * A line reduced to the value duplicate detection compares.
 *
 * Normalization is deliberately shallow: outer whitespace and ASCII case only. Those two are what
 * make the SAME line look different, and nothing more is claimed. It does not recognise a
 * rephrasing, and it is not meant to: a similarity metric here would start refusing lines that
 * merely share a subject, which is ordinary conversation.
 *
 * SHORTCUT: exact match after that normalization. Reach for near-duplicate matching only if bots
 * are observed rephrasing each other into the same line, which this cannot see.
 */
uint64 HashRecentLine(std::string_view text)
{
    std::size_t begin = 0;
    std::size_t end = text.size();
    while (begin < end && std::isspace(static_cast<unsigned char>(text[begin])) != 0)
        ++begin;
    while (end > begin && std::isspace(static_cast<unsigned char>(text[end - 1])) != 0)
        --end;

    // FNV-1a over the normalized bytes, then mixed, so the stored value does not reveal the run
    // of a plain byte hash over text that was never meant to be retained.
    uint64 hash = 0xCBF29CE484222325ULL;
    for (std::size_t at = begin; at < end; ++at)
    {
        unsigned char const folded = static_cast<unsigned char>(std::tolower(static_cast<unsigned char>(text[at])));
        hash ^= static_cast<uint64>(folded);
        hash *= 0x100000001B3ULL;
    }

    return PlayerbotPersonality::SplitMix64(hash ^ RECENT_LINE_NAMESPACE);
}

// Distinct namespaces again, and for the same reason: one character's actor identity must not be
// derivable from a relationship identity that happens to involve them.
constexpr uint64 ACTOR_ID_NAMESPACE = 0x4143544F52494400ULL;
constexpr uint64 RELATIONSHIP_ID_NAMESPACE = 0x52454C4154494F00ULL;
constexpr uint64 MEMORY_ID_NAMESPACE = 0x4D454D4F52594900ULL;
constexpr uint64 MODERATION_ID_NAMESPACE = 0x4D4F444341534500ULL;

/*
 * Actor and relationship identities are derived from what they name rather than from a counter,
 * so the same character or the same pair always renders the same public id. That is what lets the
 * upsert forms be idempotent across restarts: a second insert for a pair collides with the first
 * on public_id instead of creating a duplicate identity for the same relationship.
 */
std::string MakeActorPublicId(uint64 characterGuidCounter)
{
    uint64 const high = PlayerbotPersonality::SplitMix64(characterGuidCounter ^ ACTOR_ID_NAMESPACE);
    uint64 const low = PlayerbotPersonality::SplitMix64(high ^ characterGuidCounter);
    return RenderPublicId(PlayerbotSocialIdKind::Actor, high, low);
}

std::string MakeRelationshipPublicId(PlayerbotSocialRelationshipKey const& key)
{
    uint64 const high = PlayerbotPersonality::SplitMix64(key.botGuidCounter ^ RELATIONSHIP_ID_NAMESPACE);
    uint64 const low = PlayerbotPersonality::SplitMix64(high ^ key.subjectGuidCounter);
    return RenderPublicId(PlayerbotSocialIdKind::Relationship, high, low);
}

// Derived from (subject, category) like a relationship id is from its pair, so a repeated campaign
// collides with its own open case on public_id and bumps it instead of filing a duplicate.
std::string MakeModerationCasePublicId(uint64 subjectGuidCounter, PlayerbotSocialModerationCategory category)
{
    uint64 const high = PlayerbotPersonality::SplitMix64(subjectGuidCounter ^ MODERATION_ID_NAMESPACE);
    uint64 const low = PlayerbotPersonality::SplitMix64(high ^ (static_cast<uint64>(static_cast<uint8>(category)) + 1));
    return RenderPublicId(PlayerbotSocialIdKind::ModerationCase, high, low);
}

// A memory, unlike a relationship, is not unique per pair: a bot can hold many about the same
// character, so this one is drawn from a counter rather than from the pair.
std::string MakeMemoryPublicId(uint64 sequence, uint64 botGuidCounter)
{
    uint64 const high = PlayerbotPersonality::SplitMix64(sequence ^ MEMORY_ID_NAMESPACE);
    uint64 const low = PlayerbotPersonality::SplitMix64(high ^ botGuidCounter);
    return RenderPublicId(PlayerbotSocialIdKind::Memory, high, low);
}

// Which prepared statement carries a channel's allowed scopes. Each has its scope list written as
// a literal, so choosing the statement IS the privacy decision and no bound value can widen it.
PlayerbotSocialStatementId MemoryStatementFor(PlayerbotSocialMemoryScopeQuery query)
{
    switch (query)
    {
        case PlayerbotSocialMemoryScopeQuery::PublicOnly:
            return PLAYERBOT_SOCIAL_STMT_SEL_MEMORY_PUBLIC_SCOPE;
        case PlayerbotSocialMemoryScopeQuery::PublicAndParty:
            return PLAYERBOT_SOCIAL_STMT_SEL_MEMORY_PARTY_SCOPE;
        case PlayerbotSocialMemoryScopeQuery::Any:
            return PLAYERBOT_SOCIAL_STMT_SEL_MEMORY_ANY_SCOPE;
    }

    // Narrowest of the three. Unreachable through PlayerbotSocialMemoryScopeQueryFor, which
    // refuses an unknown channel outright, but the module compiles without -Wswitch and the
    // fallback that cannot leak is the only defensible one.
    return PLAYERBOT_SOCIAL_STMT_SEL_MEMORY_PUBLIC_SCOPE;
}

// How many memory rows one read may return. Bounded because a bot with a long history would
// otherwise pull its entire past into the world thread to answer one message.
constexpr uint32 MEMORY_READ_LIMIT = 32;

// Elapsed time that fails closed on a clock that moved backwards. Reading a backwards step as a
// huge interval would expire every live thread at once.
uint64 ElapsedSeconds(uint64 nowUnixSeconds, uint64 sinceUnixSeconds)
{
    return nowUnixSeconds < sinceUnixSeconds ? 0 : nowUnixSeconds - sinceUnixSeconds;
}
}  // namespace

bool operator<(PlayerbotSocialThreadKey const& left, PlayerbotSocialThreadKey const& right)
{
    if (left.channel != right.channel)
        return static_cast<uint8>(left.channel) < static_cast<uint8>(right.channel);

    return left.scopeId < right.scopeId;
}

PlayerbotSocialThreadHandle PlayerbotSocialMgr::Observe(PlayerbotSocialObservation const& observation)
{
    PlayerbotSocialThreadHandle handle;

    // Refused before any state exists. This build has neither -Wswitch nor -Werror, so a corrupt or
    // newly added channel value arrives here and must not be able to open a thread.
    if (!PlayerbotSocialChannelIsValid(observation.key.channel))
        return handle;

    Scope& scope = _scopes[observation.key];
    std::vector<Thread>& threads = scope.threads;

    // An observation IS somebody speaking, so it is what moves the scope's silence clock. Forward
    // only, for the same reason the thread stamp is.
    if (observation.atUnixSeconds > scope.lastSpokenAtUnixSeconds)
        scope.lastSpokenAtUnixSeconds = observation.atUnixSeconds;

    /*
     * Attribution follows the participants first, then recency.
     *
     * A speaker who is already in a thread rejoins it even when a louder conversation started more
     * recently, which is what keeps two conversations in one zone from merging. Only a speaker
     * nobody recognises falls back to the most recent thread, and only inside the continuation
     * window.
     */
    Thread* chosen = nullptr;
    bool const hasSourceEvent =
        PlayerbotSocialPublicIdIsValid(PlayerbotSocialIdKind::Event, observation.sourceEventPublicId);
    if (hasSourceEvent)
    {
        auto const sourceThread =
            std::find_if(threads.begin(), threads.end(), [&observation](Thread const& thread)
                         { return thread.sourceEventPublicId == observation.sourceEventPublicId; });
        if (sourceThread != threads.end())
            chosen = &*sourceThread;
    }

    if (chosen == nullptr && !hasSourceEvent)
        for (Thread& thread : threads)
        {
            bool const known = std::find(thread.participants.begin(), thread.participants.end(),
                                         observation.speakerGuidCounter) != thread.participants.end();
            if (!known)
                continue;

            if (ElapsedSeconds(observation.atUnixSeconds, thread.lastActivityUnixSeconds) >
                PLAYERBOT_SOCIAL_THREAD_STALE_SECONDS)
                continue;

            if (chosen == nullptr || thread.lastActivityUnixSeconds > chosen->lastActivityUnixSeconds)
                chosen = &thread;
        }

    if (chosen == nullptr && !hasSourceEvent)
    {
        for (Thread& thread : threads)
        {
            if (ElapsedSeconds(observation.atUnixSeconds, thread.lastActivityUnixSeconds) >
                PLAYERBOT_SOCIAL_THREAD_CONTINUATION_SECONDS)
                continue;

            if (chosen == nullptr || thread.lastActivityUnixSeconds > chosen->lastActivityUnixSeconds)
                chosen = &thread;
        }
    }

    if (chosen == nullptr)
    {
        // The oldest thread gives way when a scope is full, so a busy zone forgets rather than grows.
        if (threads.size() >= PLAYERBOT_SOCIAL_MAX_THREADS_PER_SCOPE)
        {
            auto const oldest =
                std::min_element(threads.begin(), threads.end(), [](Thread const& left, Thread const& right)
                                 { return left.lastActivityUnixSeconds < right.lastActivityUnixSeconds; });
            threads.erase(oldest);
        }

        Thread opened;
        opened.threadId = _nextThreadId++;
        opened.publicId = MakeThreadPublicId(opened.threadId, observation.key);
        if (hasSourceEvent)
            opened.sourceEventPublicId = observation.sourceEventPublicId;
        threads.push_back(opened);
        chosen = &threads.back();
    }

    if (observation.speakerIsHuman)
    {
        ++chosen->relevantHumanMessages;
        chosen->consecutiveBotOnlyTurns = 0;
    }
    else
    {
        ++chosen->consecutiveBotOnlyTurns;
    }

    /*
     * An exchanged pair of turns warms the pair a little. The credits are shaped by a pure function
     * and applied through ApplyRelationshipDelta, so consent, the per-pair window ceiling, and the
     * reset queue all rule here exactly as they do for assistance. This is what lets a society of
     * bots grow relationships out of nothing but talking, with no human in the loop.
     */
    if (chosen->lastSpeakerGuidCounter != 0 && chosen->lastSpeakerGuidCounter != observation.speakerGuidCounter)
    {
        for (PlayerbotSocialConversationCredit const& credit :
             PlayerbotSocialConversationCredits(observation.speakerGuidCounter, observation.speakerIsHuman,
                                                chosen->lastSpeakerGuidCounter, chosen->lastSpeakerWasHuman))
            ApplyRelationshipDelta(credit.botGuidCounter, credit.subjectGuidCounter, credit.delta,
                                   observation.atUnixSeconds);

        /*
         * The same adjacency answers "who was that aimed at": a hostile line in a thread targets
         * the previous speaker. Only a bot can be the abused subject here; what humans say to each
         * other is theirs, and what bots feel is the thing the moderation cases exist to notice.
         */
        if (!chosen->lastSpeakerWasHuman && !observation.text.empty())
            if (std::optional<PlayerbotSocialModerationCategory> const category =
                    PlayerbotSocialClassifyHostileLine(observation.text))
                NoteHostileLine(chosen->lastSpeakerGuidCounter, observation.speakerGuidCounter, *category,
                                observation.text, observation.atUnixSeconds);
    }

    if (observation.speakerGuidCounter != 0)
    {
        chosen->lastSpeakerGuidCounter = observation.speakerGuidCounter;
        chosen->lastSpeakerWasHuman = observation.speakerIsHuman;
    }

    /*
     * Activity only ever moves forward. Observations can arrive out of order, and letting a late one
     * rewind the timestamp would make a thread look fresher than it is: staleness, decay, and the
     * continuation window are all measured from this value, so a rewind quietly revives a thread that
     * should have been closing.
     */
    if (observation.atUnixSeconds > chosen->lastActivityUnixSeconds)
        chosen->lastActivityUnixSeconds = observation.atUnixSeconds;

    if (std::find(chosen->participants.begin(), chosen->participants.end(), observation.speakerGuidCounter) ==
        chosen->participants.end())
    {
        chosen->participants.push_back(observation.speakerGuidCounter);
        if (chosen->participants.size() > PLAYERBOT_SOCIAL_MAX_THREAD_PARTICIPANTS)
            chosen->participants.pop_front();
    }

    if (PlayerbotSocialPublicIdIsValid(PlayerbotSocialIdKind::Event, observation.eventPublicId))
    {
        chosen->recentEventIds.push_back(observation.eventPublicId);
        if (chosen->recentEventIds.size() > PLAYERBOT_SOCIAL_MAX_THREAD_EVENTS)
            chosen->recentEventIds.pop_front();
    }

    /*
     * Compared against the history BEFORE joining it, or every line would be a duplicate of itself.
     *
     * An empty text answers no rather than matching every other empty one. A caller with nothing to
     * give must suppress nothing: silence about the content is not evidence of repetition, and
     * folding the two together would let one text free path mute a whole thread.
     */
    bool duplicate = false;
    if (!observation.text.empty())
    {
        uint64 const hash = HashRecentLine(observation.text);
        duplicate = std::find(chosen->recentLineHashes.begin(), chosen->recentLineHashes.end(), hash) !=
                    chosen->recentLineHashes.end();

        chosen->recentLineHashes.push_back(hash);
        if (chosen->recentLineHashes.size() > PLAYERBOT_SOCIAL_MAX_THREAD_RECENT_LINES)
            chosen->recentLineHashes.pop_front();

        /*
         * And the text itself, for idle memory extraction, under the buffer's own rules.
         *
         * Consent is answered HERE, at the moment the line arrives, rather than deferred to
         * submission: a player who has not consented never has their words in this process at all,
         * which is a stronger promise than not sending them. `IsOptedOut` fails closed for anyone
         * whose consent has not been read, so the answer for an unknown character is no.
         *
         * A duplicate is offered like any other line. It is a real turn in the conversation, and the
         * flag above is about whether a BOT should repeat itself, not about what was said.
         */
        PlayerbotSocialBufferedLine line;
        line.speakerGuidCounter = observation.speakerGuidCounter;
        line.speakerIsHuman = observation.speakerIsHuman;
        line.sourceKind = observation.role == PlayerbotSocialPromptLineRole::AuthoritativeSource
                              ? PlayerbotSocialMemorySourceKind::AuthoritativeSource
                          : observation.speakerIsHuman ? PlayerbotSocialMemorySourceKind::HumanObservation
                                                       : PlayerbotSocialMemorySourceKind::GeneratedDelivery;
        line.sourceEventPublicId = observation.role == PlayerbotSocialPromptLineRole::AuthoritativeSource
                                       ? observation.sourceEventPublicId
                                       : observation.eventPublicId;
        line.atUnixSeconds = observation.atUnixSeconds;
        line.text = observation.text;

        bool const speakerConsented = !observation.speakerIsHuman || !IsOptedOut(observation.speakerGuidCounter);
        chosen->extraction.Offer(observation.key.channel, std::move(line), speakerConsented, observation.atUnixSeconds);

        /*
         * A duplicate is still a real turn. Listener fanout is collapsed at the route boundary, so
         * reaching this method twice means two game messages were spoken even when their text is
         * identical. The duplicate flag controls reply pressure and does not erase conversation.
         */
        PlayerbotSocialPromptLine promptLine;
        if (PlayerbotSocialPublicIdIsValid(PlayerbotSocialIdKind::Event, observation.eventPublicId))
            promptLine.eventPublicId = observation.eventPublicId;
        promptLine.role = observation.role;
        promptLine.replyToEventPublicId = observation.replyToEventPublicId;
        promptLine.sourceEventPublicId = observation.sourceEventPublicId;
        promptLine.speakerGuidCounter = observation.speakerGuidCounter;
        promptLine.speakerName = observation.speakerName;
        promptLine.speakerIsHuman = observation.speakerIsHuman;
        promptLine.atUnixSeconds = observation.atUnixSeconds;
        promptLine.text = observation.text;
        chosen->promptContext.Offer(observation.key.channel, std::move(promptLine), speakerConsented,
                                    observation.atUnixSeconds);
    }

    handle.valid = true;
    handle.threadId = chosen->threadId;
    handle.publicId = chosen->publicId;
    handle.observedEventPublicId = observation.eventPublicId;
    handle.sourceEventPublicId = chosen->sourceEventPublicId;
    handle.rootSubject = chosen->rootSubject;
    handle.duplicateOfRecentMessage = duplicate;
    return handle;
}

std::vector<PlayerbotSocialIdleThread> PlayerbotSocialMgr::CollectIdleExtractionSnapshots(uint64 nowUnixSeconds)
{
    std::vector<PlayerbotSocialIdleThread> collected;

    /*
     * Consent is read through this class rather than the store, so a character nobody has asked
     * about answers no. That is the same fail closed gate the persistence path uses, and it is what
     * makes an offline or newly connected player ineligible instead of silently included.
     */
    PlayerbotSocialExtractionConsent const consents = [this](uint64 guid) { return !IsOptedOut(guid); };

    for (auto& scope : _scopes)
    {
        for (Thread& thread : scope.second.threads)
        {
            if (collected.size() >= PLAYERBOT_SOCIAL_EXTRACTION_MAX_PER_SWEEP)
                return collected;

            if (!PlayerbotSocialThreadIsIdleForExtraction(thread.lastActivityUnixSeconds, nowUnixSeconds))
                continue;

            /*
             * The cheap gate first. This runs over every thread on the realm on a timer, while
             * building a snapshot copies text and scans it for markers, so a thread holding only bot
             * turns is refused by a deque walk rather than by doing that work and discarding it.
             */
            if (!thread.extraction.EligibleForExtraction())
                continue;

            PlayerbotSocialIdleThread idle;
            idle.threadPublicId = thread.publicId;
            idle.key = scope.first;
            idle.snapshot = PlayerbotSocialBuildExtractionSnapshot(thread.extraction, consents, nowUnixSeconds);

            // Cleared whether or not it was usable. A refusal is a decision about this conversation,
            // not an invitation to keep the words and ask again next tick.
            thread.extraction.Clear();

            if (idle.snapshot.Accepted())
                collected.push_back(std::move(idle));
        }
    }

    return collected;
}

std::size_t PlayerbotSocialMgr::RequestIdleExtractions(uint64 nowUnixSeconds)
{
    if (_provider == nullptr)
        return 0;

    std::size_t accepted = 0;

    for (PlayerbotSocialIdleThread& idle : CollectIdleExtractionSnapshots(nowUnixSeconds))
    {
        // Carried as guids and text. Turning a guid into a display name means touching a live
        // character, which is the provider layer's to do; doing it here would also leave this class
        // holding names it has no other use for.
        std::vector<PlayerbotSocialMemoryLine> lines;
        lines.reserve(idle.snapshot.lines.size());
        for (PlayerbotSocialBufferedLine const& line : idle.snapshot.lines)
        {
            PlayerbotSocialMemoryLine memoryLine;
            memoryLine.speakerGuidCounter = line.speakerGuidCounter;
            memoryLine.text = line.text;
            memoryLine.sourceKind = line.sourceKind;
            memoryLine.sourceEventPublicId = line.sourceEventPublicId;
            memoryLine.sourceChannel = idle.key.channel;
            memoryLine.sourceThreadPublicId = idle.threadPublicId;
            lines.push_back(std::move(memoryLine));
        }

        /*
         * The scope is the SURFACE, not a judgement. General and say are heard by anyone nearby so
         * they are public; a party is party. A whisper cannot reach here at all, because its text
         * is never buffered, and the provider seam refuses one again if it somehow did.
         */
        PlayerbotSocialPrivacyScope const scope = idle.key.channel == PlayerbotSocialChannel::Party
                                                      ? PlayerbotSocialPrivacyScope::Party
                                                      : PlayerbotSocialPrivacyScope::Public;

        uint64 const token = _nextMemoryRequestToken++;
        if (!_provider->SubmitMemory(token, idle.snapshot.holderGuidCounter, idle.threadPublicId, scope,
                                     idle.snapshot.subjects, lines))
        {
            // Never asked, so never answered. Reported as a failure rather than silently skipped:
            // a provider refusing every request is exactly what this feed exists to make visible.
            PlayerbotSocialExtractionAttempt refused;
            refused.threadPublicId = idle.threadPublicId;
            refused.botGuidCounter = idle.snapshot.holderGuidCounter;
            refused.channel = idle.key.channel;
            refused.subjectCount = idle.snapshot.subjects.size();
            refused.lineCount = idle.snapshot.lines.size();
            refused.occurredAtUnixSeconds = nowUnixSeconds;
            RecordEvent(PlayerbotSocialMakeExtractionEvent(refused));
            continue;
        }

        OutstandingExtraction outstanding;
        outstanding.botGuidCounter = idle.snapshot.holderGuidCounter;
        outstanding.threadPublicId = idle.threadPublicId;
        outstanding.scope = scope;
        outstanding.subjects = idle.snapshot.subjects;
        outstanding.sources = idle.snapshot.lines;
        outstanding.issuedAtUnixSeconds = nowUnixSeconds;

        // Recorded only after the provider accepted it, so a refusal leaves nothing outstanding.
        _memoryRequests[token] = std::move(outstanding);
        ++accepted;
    }

    return accepted;
}

std::size_t PlayerbotSocialMgr::ApplyExtractedMemories(uint64 memoryRequestToken, uint64 botGuidCounter,
                                                       std::string const& threadPublicId,
                                                       std::vector<PlayerbotSocialExtractedMemory> const& memories)
{
    auto const outstanding = _memoryRequests.find(memoryRequestToken);
    if (outstanding == _memoryRequests.end())
        return 0;

    OutstandingExtraction const request = outstanding->second;

    /*
     * Identity first, and the request survives a failure. An answer for a different bot or a
     * different conversation is somebody else's, and consuming this request on the strength of it
     * would leave the REAL answer arriving later to a token nobody holds: one wrong reply would
     * silently cost a right one. It stays outstanding until it is answered or ages out.
     */
    if (!PlayerbotSocialExtractionAnswerIsForRequest(request.botGuidCounter, request.threadPublicId, botGuidCounter,
                                                     threadPublicId))
        return 0;

    // Released once the answer is known to be this request's, and before anything is written, so a
    // correct answer sent twice finds nothing to answer the second time.
    _memoryRequests.erase(outstanding);

    std::size_t written = 0;

    for (PlayerbotSocialExtractedMemory const& memory : memories)
    {
        /*
         * Both rules are applied again here even though the sidecar applies them, because the far
         * side is what this gate exists to be wrong about. They live in the extraction unit rather
         * than inline: reaching this line from a test needs a consenting speaker, and granting
         * consent issues a prepared statement, so a rule written here could not be proven at all.
         */
        if (!PlayerbotSocialExtractedMemoryIsAdmissible(request.subjects, request.scope, memory.aboutGuidCounter,
                                                        memory.scope))
            continue;

        if (!PlayerbotSocialMemorySourceIsAdmissible(request.sources, memory.aboutGuidCounter,
                                                     memory.sourceEventPublicId))
            continue;

        auto const source = std::find_if(request.sources.begin(), request.sources.end(),
                                         [&memory](PlayerbotSocialBufferedLine const& candidate)
                                         { return candidate.sourceEventPublicId == memory.sourceEventPublicId; });
        if (source == request.sources.end())
            continue;

        PlayerbotSocialMemoryRecord record;
        record.botGuidCounter = request.botGuidCounter;
        record.subjectGuidCounter = memory.aboutGuidCounter;
        record.category = PlayerbotSocialMemoryCategory::Fact;

        // The bot was in the conversation, which is what makes this provenance true rather than a
        // default: the holder rule refuses a thread no bot took part in.
        record.provenance = PlayerbotSocialMemoryProvenance::Participated;
        record.scope = memory.scope;
        record.sourceEventPublicId = memory.sourceEventPublicId;
        record.sourceThreadPublicId = request.threadPublicId;
        record.sourceKind = source->sourceKind;
        record.confidence = PLAYERBOT_SOCIAL_EXTRACTION_CONFIDENCE;
        record.significance = PLAYERBOT_SOCIAL_EXTRACTION_SIGNIFICANCE;
        record.paraphrase = memory.paraphrase;

        // PersistMemory is the single gate on consent, the actor rows, and the content backstop, so
        // a refusal here is counted rather than worked around.
        if (PersistMemory(record) == PlayerbotSocialMemoryRejection::None)
            ++written;
    }

    auto const profile = _profiles.find(request.botGuidCounter);
    std::optional<PlayerbotPersonalityProfile> const personality =
        sPlayerbotPersonalityMgr.GetOrCreate(request.botGuidCounter);
    if (profile != _profiles.end() && personality.has_value())
    {
        PlayerbotSocialProfile const evolved = PlayerbotPersonality::EvolveSocialProfileAfterIndependentInteraction(
            *personality, profile->second, request.issuedAtUnixSeconds);

        if (evolved.traits.talkativeness != profile->second.traits.talkativeness ||
            evolved.traits.lastEvolvedAtUnixSeconds != profile->second.traits.lastEvolvedAtUnixSeconds)
        {
            profile->second = evolved;
            QueueTraitsWrite(request.botGuidCounter, evolved);
        }
    }

    /*
     * One event per extraction, emitted where it RESOLVES. A request and its answer are one thing
     * that happened to one conversation, and two rows would double the feed to say it.
     *
     * The stamp is the request's, not now: the event describes a conversation that ended, and
     * dating it to when the provider got round to answering would place it minutes after the talk
     * it is about.
     */
    PlayerbotSocialExtractionAttempt attempt;
    attempt.threadPublicId = request.threadPublicId;
    attempt.botGuidCounter = request.botGuidCounter;
    attempt.channel = request.scope == PlayerbotSocialPrivacyScope::Party ? PlayerbotSocialChannel::Party
                                                                          : PlayerbotSocialChannel::General;
    attempt.subjectCount = request.subjects.size();
    attempt.lineCount = memories.size();
    attempt.answered = true;
    attempt.written = written;
    attempt.occurredAtUnixSeconds = request.issuedAtUnixSeconds;
    RecordEvent(PlayerbotSocialMakeExtractionEvent(attempt));

    return written;
}

std::size_t PlayerbotSocialMgr::AbandonStaleMemoryRequests(uint64 nowUnixSeconds)
{
    std::size_t abandoned = 0;

    for (auto request = _memoryRequests.begin(); request != _memoryRequests.end();)
    {
        // A stamp in the future is abandoned too. A clock that stepped backwards would otherwise
        // leave a token outstanding forever, which is the leak this exists to prevent.
        bool const inFuture = request->second.issuedAtUnixSeconds > nowUnixSeconds;
        bool const expired =
            !inFuture && nowUnixSeconds - request->second.issuedAtUnixSeconds > MEMORY_REQUEST_TIMEOUT_SECONDS;

        if (!inFuture && !expired)
        {
            ++request;
            continue;
        }

        // Asked for and never answered, which is the Failed outcome. Recorded here rather than at
        // the request, so a bridge that has gone quiet shows up in the feed as requests that never
        // resolved rather than as nothing at all.
        PlayerbotSocialExtractionAttempt lost;
        lost.threadPublicId = request->second.threadPublicId;
        lost.botGuidCounter = request->second.botGuidCounter;
        lost.channel = request->second.scope == PlayerbotSocialPrivacyScope::Party ? PlayerbotSocialChannel::Party
                                                                                   : PlayerbotSocialChannel::General;
        lost.subjectCount = request->second.subjects.size();
        lost.occurredAtUnixSeconds = request->second.issuedAtUnixSeconds;
        RecordEvent(PlayerbotSocialMakeExtractionEvent(lost));

        request = _memoryRequests.erase(request);
        ++abandoned;
    }

    return abandoned;
}

PlayerbotSocialMgr::Thread const* PlayerbotSocialMgr::FindThread(uint64 threadId) const
{
    for (auto const& scope : _scopes)
        for (Thread const& thread : scope.second.threads)
            if (thread.threadId == threadId)
                return &thread;

    return nullptr;
}

PlayerbotSocialMgr::Thread const* PlayerbotSocialMgr::FindThread(std::string const& threadPublicId) const
{
    if (!PlayerbotSocialPublicIdIsValid(PlayerbotSocialIdKind::Thread, threadPublicId))
        return nullptr;

    for (auto const& scope : _scopes)
        for (Thread const& thread : scope.second.threads)
            if (thread.publicId == threadPublicId)
                return &thread;

    return nullptr;
}

PlayerbotSocialThreadPressure PlayerbotSocialMgr::PressureFor(PlayerbotSocialThreadHandle const& thread,
                                                              uint64 nowUnixSeconds) const
{
    PlayerbotSocialThreadPressure pressure;
    pressure.nowUnixSeconds = nowUnixSeconds;

    /*
     * The scope is located alongside the thread rather than the thread alone, because density is a
     * property of the conversation SPACE and this is the only place that assembles the full pressure
     * input. Returning it unset made every caller throttle against a density of zero, so the density
     * rules were inert while looking configured.
     */
    Thread const* found = nullptr;
    PlayerbotSocialThreadKey key;

    if (thread.valid)
        for (auto const& [scopeKey, scope] : _scopes)
        {
            for (Thread const& candidate : scope.threads)
                if (candidate.threadId == thread.threadId)
                {
                    found = &candidate;
                    key = scopeKey;
                    break;
                }

            if (found != nullptr)
                break;
        }

    if (found == nullptr)
    {
        // An unknown or pruned thread reports the most decayed state this type can express, so a
        // stale handle reads as a dead conversation rather than a fresh one.
        pressure.lastActivityUnixSeconds = 0;
        pressure.consecutiveBotOnlyTurns = 0;
        pressure.relevantHumanMessages = 0;
        pressure.channelDensity = 0;
        return pressure;
    }

    pressure.lastActivityUnixSeconds = found->lastActivityUnixSeconds;
    pressure.consecutiveBotOnlyTurns = found->consecutiveBotOnlyTurns;
    pressure.relevantHumanMessages = found->relevantHumanMessages;
    pressure.channelDensity = ChannelDensity(key);
    return pressure;
}

std::vector<uint64> PlayerbotSocialMgr::ParticipantsOf(PlayerbotSocialThreadHandle const& thread) const
{
    Thread const* const found = thread.valid ? FindThread(thread.threadId) : nullptr;
    if (found == nullptr)
        return {};

    return std::vector<uint64>(found->participants.begin(), found->participants.end());
}

bool PlayerbotSocialMgr::ThreadIsCurrent(std::string const& threadPublicId) const
{
    /*
     * Shape checked before the scan, so a malformed or wrong-kind identity is refused without
     * walking anything. An actor or event id must not be able to match a thread merely by being a
     * string nobody issued as a thread.
     */
    if (!PlayerbotSocialPublicIdIsValid(PlayerbotSocialIdKind::Thread, threadPublicId))
        return false;

    /*
     * A linear scan, deliberately. Both bounds are small and enforced elsewhere in this class, and
     * this runs only for a delivery that is already due, which is rare. An index would be a second
     * structure to keep in step with pruning for no measurable gain.
     */
    for (auto const& [key, scope] : _scopes)
        for (Thread const& thread : scope.threads)
            if (thread.publicId == threadPublicId)
                return true;

    return false;
}

bool PlayerbotSocialMgr::ThreadScopeFor(std::string const& threadPublicId, PlayerbotSocialThreadKey& key) const
{
    if (!PlayerbotSocialPublicIdIsValid(PlayerbotSocialIdKind::Thread, threadPublicId))
        return false;

    for (auto const& [scopeKey, scope] : _scopes)
        for (Thread const& thread : scope.threads)
            if (thread.publicId == threadPublicId)
            {
                key = scopeKey;
                return true;
            }

    return false;
}

std::vector<std::string> PlayerbotSocialMgr::RecentEventIdsOf(PlayerbotSocialThreadHandle const& thread) const
{
    Thread const* const found = thread.valid ? FindThread(thread.threadId) : nullptr;
    if (found == nullptr)
        return {};

    return std::vector<std::string>(found->recentEventIds.begin(), found->recentEventIds.end());
}

std::size_t PlayerbotSocialMgr::ActiveThreadCount(PlayerbotSocialThreadKey const& key) const
{
    auto const scope = _scopes.find(key);
    return scope == _scopes.end() ? 0 : scope->second.threads.size();
}

std::size_t PlayerbotSocialMgr::TrackedScopeCount() const { return _scopes.size(); }

uint8 PlayerbotSocialMgr::ChannelDensity(PlayerbotSocialThreadKey const& key) const
{
    std::size_t const active = ActiveThreadCount(key);
    if (active == 0)
        return 0;

    std::size_t const scaled = active * 100 / PLAYERBOT_SOCIAL_MAX_THREADS_PER_SCOPE;
    return static_cast<uint8>(scaled > 100 ? 100 : scaled);
}

void PlayerbotSocialMgr::PruneStaleThreads(uint64 nowUnixSeconds)
{
    /*
     * Scopes are erased once they hold neither a thread nor a starter, and that is what bounds the
     * map.
     *
     * Threads per scope, participants, retained events, and starters all have fixed caps, but the
     * number of scopes is driven by the world: every zone General channel, every party, and every
     * whisper pair is its own key. Whisper pairs in particular are effectively unlimited over a long
     * uptime, so keeping an empty entry for each one would leak steadily. With this, a scope survives
     * only while something inside it is still within the staleness window, which is a bound the
     * server's own activity sets rather than an arbitrary number.
     */
    for (auto scope = _scopes.begin(); scope != _scopes.end();)
    {
        std::vector<Thread>& threads = scope->second.threads;

        /*
         * Buffered chat ages out on its own window, independently of whether its thread survives
         * this sweep. Dropping a thread already releases what it held, but the reverse is what
         * matters: a thread kept alive by a trickle of conversation must not carry the opening lines
         * of a talk from an hour ago, and only this makes the retention bound true for one.
         */
        for (Thread& thread : threads)
        {
            thread.extraction.Expire(nowUnixSeconds);
            thread.promptContext.Expire(nowUnixSeconds);
        }

        threads.erase(std::remove_if(threads.begin(), threads.end(),
                                     [nowUnixSeconds](Thread const& thread) {
                                         return ElapsedSeconds(nowUnixSeconds, thread.lastActivityUnixSeconds) >
                                                PLAYERBOT_SOCIAL_THREAD_STALE_SECONDS;
                                     }),
                      threads.end());

        // Starters age out on the same window. Opening a conversation about something that happened
        // long enough ago to have been forgotten reads as stranger than saying nothing.
        std::deque<PlayerbotSocialStarterContext>& starters = scope->second.starters;
        while (!starters.empty() &&
               ElapsedSeconds(nowUnixSeconds, starters.front().atUnixSeconds) > PLAYERBOT_SOCIAL_THREAD_STALE_SECONDS)
            starters.pop_front();

        scope = scope->second.Empty() ? _scopes.erase(scope) : std::next(scope);
    }
}

char const* PlayerbotSocialStarterSourceKindName(PlayerbotSocialStarterSourceKind kind)
{
    switch (kind)
    {
        case PlayerbotSocialStarterSourceKind::Loot:
            return "loot";
        case PlayerbotSocialStarterSourceKind::QuestTransition:
            return "quest_transition";
        case PlayerbotSocialStarterSourceKind::Kill:
            return "kill";
        case PlayerbotSocialStarterSourceKind::Level:
            return "level";
        case PlayerbotSocialStarterSourceKind::ZoneArrival:
            return "zone_arrival";
        case PlayerbotSocialStarterSourceKind::Death:
            return "death";
    }

    return "unknown";
}

char const* PlayerbotSocialQuestTransitionName(PlayerbotSocialQuestTransition transition)
{
    switch (transition)
    {
        case PlayerbotSocialQuestTransition::None:
            return "none";
        case PlayerbotSocialQuestTransition::Accepted:
            return "accepted";
        case PlayerbotSocialQuestTransition::ObjectiveProgress:
            return "objective_progress";
        case PlayerbotSocialQuestTransition::ObjectiveCompleted:
            return "objective_completed";
        case PlayerbotSocialQuestTransition::Failed:
            return "failed";
        case PlayerbotSocialQuestTransition::Completed:
            return "completed";
        case PlayerbotSocialQuestTransition::TurnedIn:
            return "turned_in";
    }

    return "unknown";
}

std::string PlayerbotSocialStarterGroundingSubject(PlayerbotSocialStarterSource const& source)
{
    std::string subject = PlayerbotSocialStarterSourceKindName(source.kind);
    if (source.kind == PlayerbotSocialStarterSourceKind::QuestTransition)
    {
        subject += '.';
        subject += PlayerbotSocialQuestTransitionName(source.questTransition);
    }

    subject += ": ";
    subject += source.subject;
    return subject;
}

bool PlayerbotSocialStarterSourceIsValid(PlayerbotSocialStarterSource const& source)
{
    switch (source.kind)
    {
        case PlayerbotSocialStarterSourceKind::Loot:
        case PlayerbotSocialStarterSourceKind::Kill:
        case PlayerbotSocialStarterSourceKind::Level:
        case PlayerbotSocialStarterSourceKind::ZoneArrival:
        case PlayerbotSocialStarterSourceKind::Death:
            if (source.questTransition != PlayerbotSocialQuestTransition::None)
                return false;
            break;
        case PlayerbotSocialStarterSourceKind::QuestTransition:
            if (source.questTransition == PlayerbotSocialQuestTransition::None ||
                source.questTransition > PlayerbotSocialQuestTransition::TurnedIn)
                return false;
            break;
        default:
            return false;
    }

    return PlayerbotSocialPublicIdIsValid(PlayerbotSocialIdKind::Event, source.sourceEventPublicId) &&
           source.subjectId != 0 && !source.subject.empty() &&
           source.subject.size() <= PLAYERBOT_SOCIAL_STARTER_SUBJECT_MAX_LENGTH;
}

std::vector<PlayerbotSocialConversationCredit> PlayerbotSocialConversationCredits(uint64 speakerGuidCounter,
                                                                                  bool speakerIsHuman,
                                                                                  uint64 previousSpeakerGuidCounter,
                                                                                  bool previousSpeakerWasHuman)
{
    std::vector<PlayerbotSocialConversationCredit> credits;

    if (speakerGuidCounter == 0 || previousSpeakerGuidCounter == 0 || speakerGuidCounter == previousSpeakerGuidCounter)
        return credits;

    PlayerbotSocialRelationshipValues delta;
    delta.familiarity = PLAYERBOT_SOCIAL_CONVERSATION_FAMILIARITY_DELTA;
    delta.affinity = PLAYERBOT_SOCIAL_CONVERSATION_AFFINITY_DELTA;
    delta.trust = 0.0f;

    // The speaker's view of who they answered, then the answered party's view of the speaker. Only
    // bot owners: a ledger is never kept on a person's behalf.
    if (!speakerIsHuman)
        credits.push_back({speakerGuidCounter, previousSpeakerGuidCounter, delta});

    if (!previousSpeakerWasHuman)
        credits.push_back({previousSpeakerGuidCounter, speakerGuidCounter, delta});

    return credits;
}

bool PlayerbotSocialSelectStarterChannel(PlayerbotSocialStarterAudience const& audience,
                                         PlayerbotSocialChannel& channel, bool allowBotAudiences)
{
    // A real human anywhere outranks every bot audience: a person who can perceive the line is
    // always the preferred reason to speak, whatever stage admitted the starter.
    if (audience.hasRealPartyMember)
        channel = PlayerbotSocialChannel::Party;
    else if (audience.hasRealSayListener)
        channel = PlayerbotSocialChannel::Say;
    else if (audience.hasRealGeneralMember)
        channel = PlayerbotSocialChannel::General;
    else if (allowBotAudiences && audience.hasBotPartyMember)
        channel = PlayerbotSocialChannel::Party;
    else if (allowBotAudiences && audience.hasBotSayListener)
        channel = PlayerbotSocialChannel::Say;
    else if (allowBotAudiences && audience.hasBotGeneralMember)
        channel = PlayerbotSocialChannel::General;
    else
        return false;

    return true;
}

std::size_t PlayerbotSocialPickStarterContext(
    std::vector<PlayerbotSocialStarterContext> const& starters,
    std::array<uint64, PLAYERBOT_SOCIAL_STARTER_SOURCE_KIND_COUNT> const& lastSpokenAtByKind)
{
    std::size_t chosen = 0;

    for (std::size_t index = 1; index < starters.size(); ++index)
    {
        std::size_t const candidateKind = static_cast<std::size_t>(starters[index].source.kind);
        std::size_t const chosenKind = static_cast<std::size_t>(starters[chosen].source.kind);

        // An out-of-range kind cannot index the rotation and never displaces a valid choice.
        if (candidateKind >= PLAYERBOT_SOCIAL_STARTER_SOURCE_KIND_COUNT)
            continue;
        if (chosenKind >= PLAYERBOT_SOCIAL_STARTER_SOURCE_KIND_COUNT)
        {
            chosen = index;
            continue;
        }

        if (lastSpokenAtByKind[candidateKind] < lastSpokenAtByKind[chosenKind])
            chosen = index;
        else if (lastSpokenAtByKind[candidateKind] == lastSpokenAtByKind[chosenKind] &&
                 starters[index].atUnixSeconds >= starters[chosen].atUnixSeconds)
            chosen = index;
    }

    return chosen;
}

bool PlayerbotSocialMgr::NoteStarterContext(PlayerbotSocialStarterContext const& starter)
{
    if (starter.key.channel == PlayerbotSocialChannel::Whisper || !PlayerbotSocialChannelIsValid(starter.key.channel) ||
        starter.key.scopeId == 0)
        return false;

    if (starter.botGuidCounter == 0 || starter.audienceGuidCounter == 0 ||
        !PlayerbotSocialStarterSourceIsValid(starter.source))
        return false;

    std::deque<PlayerbotSocialStarterContext>& starters = _scopes[starter.key].starters;
    starters.push_back(starter);
    if (starters.size() > PLAYERBOT_SOCIAL_MAX_STARTER_CONTEXTS_PER_SCOPE)
        starters.pop_front();

    return true;
}

std::vector<PlayerbotSocialStarterContext> PlayerbotSocialMgr::PendingStarterContextsFor(
    PlayerbotSocialThreadKey const& key) const
{
    auto const scope = _scopes.find(key);
    if (scope == _scopes.end())
        return {};

    return std::vector<PlayerbotSocialStarterContext>(scope->second.starters.begin(), scope->second.starters.end());
}

std::vector<PlayerbotSocialStarterContext> PlayerbotSocialMgr::TakeStarterContextsFor(
    PlayerbotSocialThreadKey const& key)
{
    auto const scope = _scopes.find(key);
    if (scope == _scopes.end())
        return {};

    std::vector<PlayerbotSocialStarterContext> taken(scope->second.starters.begin(), scope->second.starters.end());
    scope->second.starters.clear();

    /*
     * The scope is left in place even when this empties it. `PruneStaleThreads` owns erasure and
     * already drops a scope holding neither a thread nor a starter; erasing here as well would race
     * that rule and could drop a scope whose thread is about to be opened by the very caller that
     * just took these.
     */
    return taken;
}

std::vector<PlayerbotSocialThreadKey> PlayerbotSocialMgr::ScopesWithPendingStarters(
    std::size_t limit, PlayerbotSocialThreadKey const& after) const
{
    std::vector<PlayerbotSocialThreadKey> keys;
    if (limit == 0 || _scopes.empty())
        return keys;

    keys.reserve(limit);

    /*
     * Two passes rather than a modular index: the tail from just past the cursor, then the head up to
     * it. The map changes shape between ticks as scopes are created and pruned, so a numeric position
     * would name a different scope each time. A key survives that, and `upper_bound` lands correctly
     * even when the scope the cursor names has since been erased.
     */
    auto const resume = _scopes.upper_bound(after);

    for (auto scope = resume; scope != _scopes.end() && keys.size() < limit; ++scope)
        if (!scope->second.starters.empty())
            keys.push_back(scope->first);

    for (auto scope = _scopes.begin(); scope != resume && keys.size() < limit; ++scope)
        if (!scope->second.starters.empty())
            keys.push_back(scope->first);

    return keys;
}

uint64 PlayerbotSocialMgr::ScopeLastSpokenAt(PlayerbotSocialThreadKey const& key) const
{
    auto const scope = _scopes.find(key);
    return scope == _scopes.end() ? 0 : scope->second.lastSpokenAtUnixSeconds;
}

PlayerbotSocialThreadHandle PlayerbotSocialMgr::OpenStarterThread(PlayerbotSocialThreadKey const& key,
                                                                  uint64 nowUnixSeconds)
{
    PlayerbotSocialThreadHandle handle;

    if (!PlayerbotSocialChannelIsValid(key.channel))
        return handle;

    std::vector<Thread>& threads = _scopes[key].threads;

    /*
     * Reuses the most recent thread inside the continuation window, mirroring what `Observe` does for
     * a speaker nobody recognises. A starter belongs to whatever is currently being talked about in
     * the scope when there is something; only genuine silence opens a new one.
     */
    Thread* chosen = nullptr;
    for (Thread& thread : threads)
    {
        if (ElapsedSeconds(nowUnixSeconds, thread.lastActivityUnixSeconds) >
            PLAYERBOT_SOCIAL_THREAD_CONTINUATION_SECONDS)
            continue;

        if (chosen == nullptr || thread.lastActivityUnixSeconds > chosen->lastActivityUnixSeconds)
            chosen = &thread;
    }

    if (chosen == nullptr)
    {
        // Same ceiling and same victim as an observed message. A scope full of threads forgets its
        // oldest rather than growing, whether the new conversation was heard or started.
        if (threads.size() >= PLAYERBOT_SOCIAL_MAX_THREADS_PER_SCOPE)
        {
            auto const oldest =
                std::min_element(threads.begin(), threads.end(), [](Thread const& left, Thread const& right)
                                 { return left.lastActivityUnixSeconds < right.lastActivityUnixSeconds; });
            threads.erase(oldest);
        }

        Thread opened;
        opened.threadId = _nextThreadId++;
        opened.publicId = MakeThreadPublicId(opened.threadId, key);
        threads.push_back(opened);
        chosen = &threads.back();
    }

    // Forward only, for the same reason an observation's timestamp is: staleness, decay, and the
    // continuation window are all measured from this, so a rewind quietly revives a closing thread.
    if (nowUnixSeconds > chosen->lastActivityUnixSeconds)
        chosen->lastActivityUnixSeconds = nowUnixSeconds;

    handle.valid = true;
    handle.threadId = chosen->threadId;
    handle.publicId = chosen->publicId;
    handle.sourceEventPublicId = chosen->sourceEventPublicId;
    handle.rootSubject = chosen->rootSubject;
    return handle;
}

PlayerbotSocialThreadHandle PlayerbotSocialMgr::OpenStarterThread(PlayerbotSocialStarterContext const& starter,
                                                                  uint64 nowUnixSeconds)
{
    PlayerbotSocialThreadHandle handle;
    if (!PlayerbotSocialStarterSourceIsValid(starter.source) || !PlayerbotSocialChannelIsValid(starter.key.channel) ||
        starter.key.channel == PlayerbotSocialChannel::Whisper || starter.key.scopeId == 0)
        return handle;

    std::vector<Thread>& threads = _scopes[starter.key].threads;
    if (threads.size() >= PLAYERBOT_SOCIAL_MAX_THREADS_PER_SCOPE)
    {
        auto const oldest = std::min_element(threads.begin(), threads.end(), [](Thread const& left, Thread const& right)
                                             { return left.lastActivityUnixSeconds < right.lastActivityUnixSeconds; });
        threads.erase(oldest);
    }

    Thread opened;
    opened.threadId = _nextThreadId++;
    opened.publicId = MakeThreadPublicId(opened.threadId, starter.key);
    opened.sourceEventPublicId = starter.source.sourceEventPublicId;
    opened.rootSubject =
        std::string(PlayerbotSocialStarterSourceKindName(starter.source.kind)) + ": " + starter.source.subject;
    opened.lastActivityUnixSeconds = nowUnixSeconds;
    threads.push_back(std::move(opened));

    Thread const& thread = threads.back();
    handle.valid = true;
    handle.threadId = thread.threadId;
    handle.publicId = thread.publicId;
    handle.sourceEventPublicId = thread.sourceEventPublicId;
    handle.rootSubject = thread.rootSubject;
    return handle;
}

// Durable consent -----------------------------------------------------------------------------

/*
 * The asynchronous read queue.
 *
 * Held here rather than as a member so the header does not have to pull in the database layer, and
 * so nothing outside this file can dispatch callbacks. AzerothCore processes these on the world
 * thread, which is what makes it safe for the lambdas below to touch the manager's containers; the
 * class comment forbids touching them from any other thread and that still holds.
 */
namespace
{
QueryCallbackProcessor& SocialQueryProcessor()
{
    static QueryCallbackProcessor processor;

    return processor;
}

/*
 * The same, for writes whose OUTCOME a caller has to see. Reads and transactions are separate
 * processor types in AzerothCore, so this is a second container rather than a second use of the
 * one above, and it is pumped from the same place on the same thread.
 */
AsyncCallbackProcessor<TransactionCallback>& SocialTransactionProcessor()
{
    static AsyncCallbackProcessor<TransactionCallback> processor;

    return processor;
}
}  // namespace

void PlayerbotSocialMgr::LoadConsent(uint64 characterGuidCounter)
{
    if (characterGuidCounter == 0)
        return;

    /*
     * Drop any previously loaded answer before asking again, so a reconnect fails closed until the
     * fresh read lands instead of being served the value the last session left behind. The logout
     * hook normally does this; doing it here as well means a missed logout, a crash, or a character
     * transfer cannot leave a stale answer standing.
     */
    _consentLoaded.erase(characterGuidCounter);

    uint64 const token = _nextConsentToken++;
    _consentToken[characterGuidCounter] = token;

    PlayerbotSocialPreparedStatement* statement = NewPlayerbotSocialStatement(PLAYERBOT_SOCIAL_STMT_SEL_CONSENT);
    statement->SetData(0, static_cast<uint32>(characterGuidCounter));

    SocialQueryProcessor().AddCallback(PlayerbotSocialAsyncQuery(statement).WithCallback(
        [this, characterGuidCounter, token](QueryResult result)
        {
            /*
             * Anything that touched this character's consent while the read was in flight replaced
             * the token, which makes this result obsolete rather than merely old. Applying it anyway
             * is how a login read lands after an explicit opt out and quietly opts the character
             * back in.
             */
            auto const current = _consentToken.find(characterGuidCounter);
            if (current == _consentToken.end() || current->second != token)
                return;

            /*
             * The statement counts as well as reads, so a successful query always returns exactly one
             * row. A null result therefore means the query FAILED, not that no preference is stored,
             * and the two must not be conflated: AzerothCore returns a null QueryResult for
             * both an empty result set and a failed query (PreparedStatementTask::Execute), so a
             * plain SELECT of the row cannot tell them apart. Leaving the character unloaded on
             * failure keeps IsOptedOut failing closed, which is the whole point of the flag.
             */
            if (!result)
                return;

            Field* fields = result->Fetch();
            bool const hasRow = fields[0].Get<uint64>() != 0;
            bool const optedOut = hasRow && fields[1].Get<uint32>() != 0;

            // No row means the character has never expressed a preference, which is participation.
            // The absence is still a loaded answer, so it must be recorded as one: leaving it
            // unloaded would keep IsOptedOut failing closed forever for everyone who never opted out.
            ApplyConsentSnapshot(characterGuidCounter, optedOut);
        }));
}

void PlayerbotSocialMgr::ApplyConsentSnapshot(uint64 characterGuidCounter, bool optedOut)
{
    if (characterGuidCounter == 0)
        return;

    _state.SetOptedOut(characterGuidCounter, optedOut);
    _consentLoaded.insert(characterGuidCounter);
    if (optedOut)
        ForgetBufferedChatOf(characterGuidCounter);
}

void PlayerbotSocialMgr::ForgetConsent(uint64 characterGuidCounter)
{
    _consentLoaded.erase(characterGuidCounter);

    // Invalidates any read still in flight for this character. Without it, a login read issued just
    // before logout could land afterwards and reinstate an answer for somebody no longer here.
    _consentToken.erase(characterGuidCounter);
    _state.SetOptedOut(characterGuidCounter, false);

    // Consent is now unread, which this class treats as a refusal, so the words held on the strength
    // of the old answer go with it rather than waiting for the thread to age out.
    ForgetBufferedChatOf(characterGuidCounter);
}

void PlayerbotSocialMgr::ForgetBufferedChatOf(uint64 characterGuidCounter)
{
    for (auto& scope : _scopes)
        for (Thread& thread : scope.second.threads)
        {
            thread.extraction.ForgetSpeaker(characterGuidCounter);
            thread.promptContext.ForgetSpeaker(characterGuidCounter);
        }
}

void PlayerbotSocialMgr::SetOptedOut(uint64 characterGuidCounter, bool optedOut)
{
    if (characterGuidCounter == 0)
        return;

    ApplyConsentSnapshot(characterGuidCounter, optedOut);

    /*
     * An opt out purges what was already buffered rather than only stopping what comes next. The
     * answer to "forget me" has to be that the words are gone now: filtering them at submission
     * would still mean they sat in the worldserver for the length of a staleness window after the
     * request to stop.
     */
    // This is now the authoritative answer, so any read still in flight is obsolete.
    _consentToken[characterGuidCounter] = _nextConsentToken++;

    PlayerbotSocialPreparedStatement* statement = NewPlayerbotSocialStatement(PLAYERBOT_SOCIAL_STMT_INS_CONSENT);
    statement->SetData(0, static_cast<uint32>(characterGuidCounter));
    statement->SetData(1, static_cast<uint8>(optedOut ? 1 : 0));
    PlayerbotSocialExecute(statement);
}

bool PlayerbotSocialMgr::IsOptedOut(uint64 characterGuidCounter) const
{
    if (_consentLoaded.find(characterGuidCounter) == _consentLoaded.end())
        return true;

    return _state.IsOptedOut(characterGuidCounter);
}

PlayerbotSocialMemoryInputState PlayerbotSocialMgr::MemoryInputStateFor(uint64 botGuidCounter,
                                                                        PlayerbotSocialChannel channel) const
{
    PlayerbotSocialMemoryScopeQuery query = PlayerbotSocialMemoryScopeQuery::PublicOnly;
    if (!PlayerbotSocialMemoryScopeQueryFor(channel, query))
        return PlayerbotSocialMemoryInputState::Unavailable;

    auto const found = _memoryInputStates.find({botGuidCounter, query});
    return found == _memoryInputStates.end() ? PlayerbotSocialMemoryInputState::Pending : found->second;
}

void PlayerbotSocialMgr::ResetCharacter(uint64 characterGuidCounter)
{
    if (characterGuidCounter == 0)
        return;

    _state.ResetCharacter(characterGuidCounter);

    /*
     * Everything that could put the erased state back has to go with it. The epoch discards any read
     * already in flight, and the stamps are dropped so the next opportunity re-reads rather than
     * being told a snapshot from before the reset is still fresh for another five minutes.
     */
    ++_stateEpoch;
    ForgetSnapshotsOf(characterGuidCounter);
    ForgetOpenEncountersOf(characterGuidCounter);

    // Erasing a character's durable state while their chat sat buffered would let the next extraction
    // write a fresh memory about them, out of words collected before the erasure.
    ForgetBufferedChatOf(characterGuidCounter);
    _resetPending.insert(characterGuidCounter);

    /*
     * The durable rows are keyed by actor id, not by character GUID, so the actor has to be resolved
     * before anything can be deleted. Resolving first and deleting in the callback keeps both sides
     * consistent even when no actor row exists: the character simply has nothing stored, and the
     * in-memory clear above was already the whole reset.
     */
    PlayerbotSocialPreparedStatement* lookup = NewPlayerbotSocialStatement(PLAYERBOT_SOCIAL_STMT_SEL_ACTOR_ID_BY_GUID);
    lookup->SetData(0, static_cast<uint32>(characterGuidCounter));

    SocialQueryProcessor().AddCallback(PlayerbotSocialAsyncQuery(lookup).WithCallback(
        [this, characterGuidCounter](QueryResult result)
        {
            /*
             * A null result from an aggregated read is a failed query, never an absent character, so
             * the suppression stays on. Lifting it here would let writes resume while the rows the
             * reset was supposed to erase are still in the database, and a later read would hand back
             * exactly the state the character asked to have deleted.
             *
             * The cost of staying closed is that this character stores nothing further until a reset
             * succeeds. That is the safe direction of the two, it is loud rather than silent, and it
             * is recoverable by the same command that set it: `.playerbots social reset confirm`
             * calls straight back into here and issues a fresh lookup.
             */
            if (!result)
            {
                LOG_ERROR("playerbots",
                          "Social reset for character {} could not read its actor row. Durable state "
                          "may remain and social writes stay suppressed for this character.",
                          characterGuidCounter);
                return;
            }

            /*
             * Read wide and narrowed here. `COUNT(*)` is a BIGINT and `COALESCE` promotes the
             * aggregated key, so asking the field layer for a uint32 would be asking it for a type
             * the column does not have.
             */
            Field* fields = result->Fetch();
            uint64 const actorCount = fields[0].Get<uint64>();
            uint32 const actorId = static_cast<uint32>(fields[1].Get<uint64>());

            if (actorCount != 0 && actorId != 0)
            {
                // Both directions, matching the in-memory reset and
                // PlayerbotSocialResetDeletes(SubjectCharacter, ...). Consent is not among them.
                for (PlayerbotSocialStatementId const statementId :
                     {PLAYERBOT_SOCIAL_STMT_DEL_RELATIONSHIP_BY_SUBJECT, PLAYERBOT_SOCIAL_STMT_DEL_RELATIONSHIP_BY_BOT,
                      PLAYERBOT_SOCIAL_STMT_DEL_MEMORY_BY_SUBJECT, PLAYERBOT_SOCIAL_STMT_DEL_MEMORY_BY_BOT})
                {
                    PlayerbotSocialPreparedStatement* deletion = NewPlayerbotSocialStatement(statementId);
                    deletion->SetData(0, actorId);
                    PlayerbotSocialExecute(deletion);
                }
            }

            // Lifted once the deletes are queued, or immediately when the character had no actor row
            // and so had nothing stored. Both are the point after which a new write is genuinely new
            // rather than something the reset was about to erase, because the database executes
            // queued statements in order.
            _resetPending.erase(characterGuidCounter);
        }));
}

void PlayerbotSocialMgr::ForgetBotCohort(std::vector<uint64> const& botGuidCounters)
{
    // An empty cohort selected nobody. Guarded here as well as in the store, because reaching the
    // loop below with an empty list would be harmless while reaching a differently written one
    // might not, and this is the destructive path.
    if (botGuidCounters.empty())
        return;

    _state.ForgetBotCohort(botGuidCounters);

    ++_stateEpoch;

    for (uint64 const botGuidCounter : botGuidCounters)
    {
        /*
         * The actor row itself is deleted below, unlike in a character reset, so the cached id stops
         * naming anything. Dropping it matters most if the GUID is ever handed to a new character:
         * a stale cache would bind that character's social state to the deleted bot's row. Clearing
         * it also lets TouchActor resolve again rather than returning early on the cached entry.
         */
        auto const cached = _actorIds.find(botGuidCounter);
        if (cached != _actorIds.end())
        {
            _actorGuids.erase(cached->second);
            _actorIds.erase(cached);
        }

        _actorLookupPending.erase(botGuidCounter);
        ForgetSnapshotsOf(botGuidCounter);
        ForgetOpenEncountersOf(botGuidCounter);

        PlayerbotSocialPreparedStatement* lookup =
            NewPlayerbotSocialStatement(PLAYERBOT_SOCIAL_STMT_SEL_ACTOR_ID_FOR_COHORT_PURGE);
        lookup->SetData(0, static_cast<uint32>(botGuidCounter));

        /*
         * This path runs during destructive startup cleanup and immediately stops the world. The
         * regular world tick never gets a chance to pump SocialQueryProcessor, so an async lookup
         * would leave every actor row behind. The indexed lookup is synchronous here for the same
         * reason LoadRuntimeControl is synchronous: its answer must be in force before startup can
         * continue. The writes remain queued and are drained by the cleanup path before shutdown.
         */
        QueryResult result = PlayerbotSocialQuery(lookup);
        if (!result)
        {
            LOG_ERROR("playerbots",
                      "Social cohort purge for bot {} could not read its actor row. Its durable "
                      "social state is left intact rather than partly deleted.",
                      botGuidCounter);
            continue;
        }

        // Same widths as the reset path, and for the same reason.
        Field* fields = result->Fetch();
        uint64 const actorCount = fields[0].Get<uint64>();
        uint32 const actorId = static_cast<uint32>(fields[1].Get<uint64>());

        if (actorCount == 0 || actorId == 0)
            continue;

        /*
         * Owner side only, unlike a character reset. A memory a surviving bot holds about one of
         * these bots belongs to that surviving bot, and Definition of Done 6 is that only the
         * supplied cohort loses bot owned state. The actor row goes with it, and the surviving
         * memory's reference to it resolves to nothing rather than to another character, because
         * the actor key is AUTO_INCREMENT and is never handed out twice.
         */
        for (PlayerbotSocialStatementId const statementId :
             {PLAYERBOT_SOCIAL_STMT_DEL_RELATIONSHIP_BY_BOT, PLAYERBOT_SOCIAL_STMT_DEL_MEMORY_BY_BOT,
              PLAYERBOT_SOCIAL_STMT_DEL_PROFILE_BY_BOT})
        {
            PlayerbotSocialPreparedStatement* deletion = NewPlayerbotSocialStatement(statementId);
            deletion->SetData(0, actorId);
            PlayerbotSocialExecute(deletion);
        }

        // Queued last, so the rows that reference it are already on their way out. The database
        // runs them in order, so no window exists where the actor is gone and its children are not.
        PlayerbotSocialPreparedStatement* actor = NewPlayerbotSocialStatement(PLAYERBOT_SOCIAL_STMT_DEL_ACTOR_BY_GUID);
        actor->SetData(0, static_cast<uint32>(botGuidCounter));
        PlayerbotSocialExecute(actor);
    }
}

// Durable relationships and memories -----------------------------------------------------------

void PlayerbotSocialMgr::ForgetOpenEncountersOf(uint64 characterGuidCounter)
{
    /*
     * An open tally is state that has not been written yet, so an erasure that leaves it behind does
     * not hold: the sweep would complete it a minute later and write a fresh relationship row for the
     * pair the character just asked to have forgotten. The erasure would have quietly undone itself.
     *
     * Both directions, and the spent ledger with them, matching what the reset and the cohort purge
     * actually delete.
     */
    auto const involves = [characterGuidCounter](PlayerbotSocialRelationshipKey const& key)
    { return key.botGuidCounter == characterGuidCounter || key.subjectGuidCounter == characterGuidCounter; };

    for (auto entry = _openEncounters.begin(); entry != _openEncounters.end();)
        entry = involves(entry->first) ? _openEncounters.erase(entry) : std::next(entry);

    for (auto entry = _assistanceCredit.begin(); entry != _assistanceCredit.end();)
        entry = involves(entry->first) ? _assistanceCredit.erase(entry) : std::next(entry);

    for (auto attacker = _appliedOpposition.begin(); attacker != _appliedOpposition.end();)
    {
        if (attacker->first == characterGuidCounter)
        {
            attacker = _appliedOpposition.erase(attacker);
            continue;
        }

        attacker->second.erase(characterGuidCounter);
        attacker = attacker->second.empty() ? _appliedOpposition.erase(attacker) : std::next(attacker);
    }
}

void PlayerbotSocialMgr::ForgetSnapshotsOf(uint64 characterGuidCounter)
{
    // Both sides of the pair, because a reset erases what this character knew and what was known
    // about them, and either stamp left behind would suppress a re-read of state that is now gone.
    for (auto entry = _relationshipSnapshotAt.begin(); entry != _relationshipSnapshotAt.end();)
    {
        if (entry->first.botGuidCounter == characterGuidCounter ||
            entry->first.subjectGuidCounter == characterGuidCounter)
        {
            entry = _relationshipSnapshotAt.erase(entry);
            continue;
        }

        ++entry;
    }

    ForgetMemorySnapshotsOf(characterGuidCounter);
}

void PlayerbotSocialMgr::ForgetMemorySnapshotsOf(uint64 botGuidCounter)
{
    // Every scope query for this bot, not just the one a caller happens to have in hand. Leaving a
    // sibling stamp behind is the defect that made the cache channel blind in the first place.
    for (auto entry = _memorySnapshotAt.begin(); entry != _memorySnapshotAt.end();)
    {
        if (entry->first.first == botGuidCounter)
        {
            entry = _memorySnapshotAt.erase(entry);
            continue;
        }

        ++entry;
    }

    /*
     * Dropping the stamps is not enough on its own. A read for this bot may already be in flight,
     * and its answer predates whatever invalidated the snapshot, so letting it land would replace
     * the set with a view from before the change and stamp that view fresh. Marked rather than
     * removed: the entry is what tells the callback a read of its own is still outstanding, and
     * removing it would let the next read start while this one is still in the air.
     */
    for (auto& outstanding : _memoryLoadsInFlight)
        if (outstanding.first.first == botGuidCounter)
            outstanding.second = true;
}

void PlayerbotSocialMgr::InvalidateRelationshipRead(PlayerbotSocialRelationshipKey const& key)
{
    _relationshipSnapshotAt.erase(key);

    // Marked rather than removed, for the reason the memory reads are. Nothing to mark when no read
    // is outstanding: the stamp above is then the whole invalidation, and the next read is issued
    // after this write and so already sees it.
    if (auto const outstanding = _relationshipLoadsInFlight.find(key); outstanding != _relationshipLoadsInFlight.end())
        outstanding->second = true;
}

bool PlayerbotSocialMgr::PairMayBeStored(uint64 botGuidCounter, uint64 subjectGuidCounter) const
{
    if (botGuidCounter == 0)
        return false;

    /*
     * The manager's IsOptedOut, not the store's. The store cannot tell "participates" from "not
     * asked yet" and answers false for both, so consulting it alone would treat a character whose
     * consent has never been read, which includes everyone currently offline, as having agreed.
     * This is the fail-closed check, and it is applied to every durable read and write.
     */
    if (IsOptedOut(botGuidCounter))
        return false;

    // A reset already decided this character is to be forgotten. Its deletes are waiting on an actor
    // lookup, and anything written in the meantime would be erased by them.
    if (_resetPending.find(botGuidCounter) != _resetPending.end())
        return false;

    // A subject of zero means the memory is about no one in particular, which no one can object to.
    if (subjectGuidCounter == 0)
        return true;

    return !IsOptedOut(subjectGuidCounter) && _resetPending.find(subjectGuidCounter) == _resetPending.end();
}

void PlayerbotSocialMgr::TouchActor(uint64 characterGuidCounter, std::string const& displayName, bool isBot)
{
    if (characterGuidCounter == 0 || displayName.empty())
        return;

    /*
     * The upsert and the lookup are separate statements because AzerothCore's async layer has no way
     * to read an auto increment key back from an insert. The insert is idempotent on both unique
     * keys, and the public identity is derived from the GUID rather than drawn from a counter, so a
     * repeat produces the same row rather than a second identity for the same character.
     */
    PlayerbotSocialPreparedStatement* upsert = NewPlayerbotSocialStatement(PLAYERBOT_SOCIAL_STMT_INS_ACTOR);
    upsert->SetData(0, MakeActorPublicId(characterGuidCounter));
    upsert->SetData(1, static_cast<uint32>(characterGuidCounter));
    upsert->SetData(2, displayName);
    upsert->SetData(3, isBot ? "bot" : "player");
    upsert->SetData(4, static_cast<uint64>(time(nullptr)));
    PlayerbotSocialExecute(upsert);

    if (_actorIds.find(characterGuidCounter) != _actorIds.end())
        return;

    // One lookup at a time per character. Without this, every social opportunity before the first
    // one lands would queue another identical read.
    if (!_actorLookupPending.insert(characterGuidCounter).second)
        return;

    PlayerbotSocialPreparedStatement* lookup = NewPlayerbotSocialStatement(PLAYERBOT_SOCIAL_STMT_SEL_ACTOR_BY_GUID);
    lookup->SetData(0, static_cast<uint32>(characterGuidCounter));

    uint64 const epoch = _stateEpoch;

    SocialQueryProcessor().AddCallback(PlayerbotSocialAsyncQuery(lookup).WithCallback(
        [this, characterGuidCounter, epoch](QueryResult result)
        {
            // Cleared before anything else can return, or a discarded lookup would leave the GUID
            // marked pending forever and no later TouchActor would ever issue another.
            _actorLookupPending.erase(characterGuidCounter);

            /*
             * A cohort purge landed while this lookup was in flight. It cleared the caches and is
             * deleting this very actor row, so caching the id now would put back exactly what the
             * purge removed, still pointing at a row that is going away.
             */
            if (epoch != _stateEpoch)
                return;

            if (!result)
                return;

            uint32 const actorId = result->Fetch()[0].Get<uint32>();
            _actorIds[characterGuidCounter] = actorId;
            _actorGuids[actorId] = characterGuidCounter;
        }));
}

bool PlayerbotSocialMgr::ActorIdKnown(uint64 characterGuidCounter) const
{
    return _actorIds.find(characterGuidCounter) != _actorIds.end();
}

uint64 PlayerbotSocialMgr::ActorGuidFor(uint32 actorId) const
{
    // Zero for an actor this process has never resolved, which is also the answer for a NULL column
    // read back as zero. Neither can have cached state here, because the cache is keyed by the guid
    // counter this map is the only source of, so a caller has nothing to erase either way.
    auto const known = _actorGuids.find(actorId);
    return known == _actorGuids.end() ? 0 : known->second;
}

void PlayerbotSocialMgr::LoadRelationship(uint64 botGuidCounter, uint64 subjectGuidCounter, uint64 nowUnixSeconds)
{
    PlayerbotSocialRelationshipKey const key{botGuidCounter, subjectGuidCounter};
    if (key.botGuidCounter == 0 || key.subjectGuidCounter == 0)
        return;

    // Opting out suppresses reads as well as writes, so a character who left the feature is not read
    // back out of storage by the next conversation.
    if (!PairMayBeStored(key.botGuidCounter, key.subjectGuidCounter))
        return;

    auto const bot = _actorIds.find(key.botGuidCounter);
    auto const subject = _actorIds.find(key.subjectGuidCounter);
    if (bot == _actorIds.end() || subject == _actorIds.end())
        return;

    auto const stamped = _relationshipSnapshotAt.find(key);
    if (stamped != _relationshipSnapshotAt.end() && PlayerbotSocialSnapshotIsFresh(stamped->second, nowUnixSeconds))
        return;

    // The stamp above is written by the callback, so it cannot suppress a second read issued while
    // the first is still outstanding. This can, and that is what keeps two answers for one key from
    // landing in the wrong order. The value starts false: nothing has invalidated this read yet.
    if (!_relationshipLoadsInFlight.emplace(key, false).second)
        return;

    PlayerbotSocialPreparedStatement* statement = NewPlayerbotSocialStatement(PLAYERBOT_SOCIAL_STMT_SEL_RELATIONSHIP);
    statement->SetData(0, bot->second);
    statement->SetData(1, subject->second);

    uint64 const epoch = _stateEpoch;

    SocialQueryProcessor().AddCallback(PlayerbotSocialAsyncQuery(statement).WithCallback(
        [this, key, nowUnixSeconds, epoch](QueryResult result)
        {
            /*
             * Taken out before anything else can return, including the fences below, and read out
             * in the same step. An entry left behind by a discarded answer would silence this key's
             * reads for the whole uptime, and would keep the map growing past the reads in flight.
             */
            bool invalidated = false;
            if (auto const outstanding = _relationshipLoadsInFlight.find(key);
                outstanding != _relationshipLoadsInFlight.end())
            {
                invalidated = outstanding->second;
                _relationshipLoadsInFlight.erase(outstanding);
            }

            // A reset or a cohort purge landed while this read was in flight, so its answer predates
            // an erasure it could not have seen. Applying it would restore what was just deleted.
            if (epoch != _stateEpoch)
                return;

            /*
             * A write to this pair landed after the read went out, so this answer is already
             * obsolete. Applying it would overwrite what was just written, and stamping it would
             * then serve that overwrite for a whole snapshot lifetime.
             */
            if (invalidated)
                return;

            /*
             * Stamped whether or not a row came back, and a null result is not treated as an error to
             * retry immediately. A pair with nothing stored and a pair the database failed to answer
             * for both read as the neutral stranger baseline, which is the safe answer either way:
             * the bot talks without history rather than inventing one, and the stamp bounds how often
             * a missing row is re-asked to once per snapshot TTL.
             */
            _relationshipSnapshotAt[key] = nowUnixSeconds;

            if (!result)
                return;

            Field* fields = result->Fetch();

            PlayerbotSocialRelationshipValues values;
            values.familiarity = fields[1].Get<float>();
            values.affinity = fields[2].Get<float>();
            values.trust = fields[3].Get<float>();

            // Clamped on the way in as well as on the way out. A row written by an older build, by a
            // server where the CHECK clauses did nothing, or by hand is not trusted to be in range.
            _state.RememberRelationship(key, PlayerbotSocialClampRelationship(values));
        }));
}

void PlayerbotSocialMgr::PreloadWarmRelationships()
{
    if (_warmRelationshipPreloadIssued)
        return;
    _warmRelationshipPreloadIssued = true;

    PlayerbotSocialPreparedStatement* statement =
        NewPlayerbotSocialStatement(PLAYERBOT_SOCIAL_STMT_SEL_WARM_RELATIONSHIPS);
    statement->SetData(0, sPlayerbotSocialConfig.socialChatWhisperMinFamiliarity);
    statement->SetData(1, static_cast<uint32>(PLAYERBOT_SOCIAL_WARM_RELATIONSHIP_PRELOAD_LIMIT));

    uint64 const epoch = _stateEpoch;

    SocialQueryProcessor().AddCallback(PlayerbotSocialAsyncQuery(statement).WithCallback(
        [this, epoch](QueryResult result)
        {
            // A reset or cohort purge landed while the read was in flight; its answer predates an
            // erasure it could not have seen.
            if (epoch != _stateEpoch)
                return;

            if (!result)
                return;

            do
            {
                Field* fields = result->Fetch();
                PlayerbotSocialRelationshipValues values;
                values.familiarity = fields[2].Get<float>();
                values.affinity = fields[3].Get<float>();
                values.trust = fields[4].Get<float>();
                ApplyPreloadedRelationship(fields[0].Get<uint32>(), fields[1].Get<uint32>(), values);
            } while (result->NextRow());
        }));
}

void PlayerbotSocialMgr::ApplyPreloadedRelationship(uint64 botGuidCounter, uint64 subjectGuidCounter,
                                                    PlayerbotSocialRelationshipValues const& values)
{
    if (botGuidCounter == 0 || subjectGuidCounter == 0)
        return;

    /*
     * A reset in flight wins over a preload, exactly as it wins over any other durable write. The
     * full fail-closed consent check is deliberately NOT applied here: at world initialization
     * nobody's consent is loaded yet, so it would refuse every pair and the preload would do
     * nothing. Consent KNOWN to be withdrawn still refuses the apply, inside the store's own
     * write check, and a pair whose consent is merely unloaded stays unusable until it is read,
     * because the pump and the request path both apply the fail-closed check before acting.
     */
    if (_resetPending.find(botGuidCounter) != _resetPending.end() ||
        _resetPending.find(subjectGuidCounter) != _resetPending.end())
        return;

    _state.RememberRelationship({botGuidCounter, subjectGuidCounter}, PlayerbotSocialClampRelationship(values));
}

void PlayerbotSocialMgr::LoadMemories(uint64 botGuidCounter, PlayerbotSocialChannel channel, uint64 nowUnixSeconds)
{
    if (!PairMayBeStored(botGuidCounter, 0))
        return;

    PlayerbotSocialMemoryScopeQuery query = PlayerbotSocialMemoryScopeQuery::PublicOnly;
    if (!PlayerbotSocialMemoryScopeQueryFor(channel, query))
        return;

    auto const bot = _actorIds.find(botGuidCounter);
    if (bot == _actorIds.end())
        return;

    auto const stamped = _memorySnapshotAt.find({botGuidCounter, query});
    if (stamped != _memorySnapshotAt.end() && PlayerbotSocialSnapshotIsFresh(stamped->second, nowUnixSeconds))
        return;

    /*
     * At most one read per bot and scope outstanding, for the reason the relationship loader holds
     * one: the stamp above is written by the callback and cannot suppress an overlapping read. It
     * matters more here, because a memory read is applied as a whole snapshot REPLACEMENT, so an
     * older answer landing second reinstates a set a newer answer had already replaced.
     */
    if (!_memoryLoadsInFlight.emplace(std::pair{botGuidCounter, query}, false).second)
        return;

    _memoryInputStates[{botGuidCounter, query}] = PlayerbotSocialMemoryInputState::Pending;

    PlayerbotSocialPreparedStatement* statement = NewPlayerbotSocialStatement(MemoryStatementFor(query));
    statement->SetData(0, bot->second);
    statement->SetData(1, nowUnixSeconds);
    statement->SetData(2, MEMORY_READ_LIMIT);

    uint64 const epoch = _stateEpoch;

    SocialQueryProcessor().AddCallback(PlayerbotSocialAsyncQuery(statement).WithCallback(
        [this, botGuidCounter, query, nowUnixSeconds, epoch](QueryResult result)
        {
            // Taken out on every path, including the two fences below, so a discarded answer cannot
            // silence this bot's memory reads for the rest of the uptime.
            bool invalidated = false;
            if (auto const outstanding = _memoryLoadsInFlight.find({botGuidCounter, query});
                outstanding != _memoryLoadsInFlight.end())
            {
                invalidated = outstanding->second;
                _memoryLoadsInFlight.erase(outstanding);
            }

            if (epoch != _stateEpoch)
                return;

            /*
             * A write invalidated this bot's snapshot after the read went out, so this answer is
             * already obsolete. Applying it would replace the set WITHOUT the memory that write
             * added, and stamping it would then hide that memory for a whole snapshot lifetime.
             */
            if (invalidated)
                return;

            _memorySnapshotAt[{botGuidCounter, query}] = nowUnixSeconds;

            if (!result)
            {
                _memoryInputStates[{botGuidCounter, query}] = PlayerbotSocialMemoryInputState::Unavailable;
                return;
            }

            _memoryInputStates[{botGuidCounter, query}] = PlayerbotSocialMemoryInputState::Loaded;

            /*
             * Collected first and applied as one replacement, rather than remembered row by row. A
             * snapshot read is the whole answer for this bot, so applying it additively would append
             * the same facts again on every refresh.
             */
            std::vector<PlayerbotSocialMemoryRecord> snapshot;

            do
            {
                Field* fields = result->Fetch();

                PlayerbotSocialMemoryRecord record;
                record.botGuidCounter = botGuidCounter;

                /*
                 * A subject actor that this process cannot name is skipped rather than guessed at. A
                 * NULL subject is legal and means the memory is about no one in particular; a
                 * non-NULL one that resolves to nothing means the row outlived its actor, and
                 * attaching it to an arbitrary character would be inventing a fact about them.
                 */
                uint32 const subjectActorId = fields[1].Get<uint32>();
                if (subjectActorId != 0)
                {
                    auto const subject = _actorGuids.find(subjectActorId);
                    if (subject == _actorGuids.end())
                        continue;

                    record.subjectGuidCounter = subject->second;
                }

                if (!PlayerbotSocialParseMemoryCategory(fields[2].Get<std::string>(), record.category))
                    continue;

                record.paraphrase = fields[3].Get<std::string>();

                if (!PlayerbotSocialParseMemoryProvenance(fields[4].Get<std::string>(), record.provenance))
                    continue;

                record.confidence = fields[5].Get<float>();
                record.significance = fields[6].Get<float>();

                if (!PlayerbotSocialParsePrivacyScope(fields[7].Get<std::string>(), record.scope))
                    continue;

                if (fields[8].IsNull() || fields[9].IsNull() || fields[10].IsNull())
                    continue;

                record.sourceEventPublicId = fields[8].Get<std::string>();
                record.sourceThreadPublicId = fields[9].Get<std::string>();
                PlayerbotSocialMemorySourceKind sourceKind;
                if (!PlayerbotSocialParseMemorySourceKind(fields[10].Get<std::string>(), sourceKind))
                    continue;
                record.sourceKind = sourceKind;

                // Same fail-closed rule as the write path. A subject who opted out while this read
                // was in flight, or who is simply not online to have had their consent read, does
                // not come back out of storage.
                if (!PairMayBeStored(botGuidCounter, record.subjectGuidCounter))
                    continue;

                snapshot.push_back(record);
            } while (result->NextRow());

            /*
             * Each record is revalidated as it is applied. A row passed this gate when it was
             * written, but the marker lists change, and one that would be refused today must not be
             * readable just because it predates the rule.
             *
             * The same rule bounds what the replacement is allowed to remove. It is written once,
             * here, as the two conditions the loop above skips on: a subject this process cannot name
             * and a pair it may not store. A memory whose subject was offline while this read ran is
             * therefore kept rather than dropped, because this read had no opinion about it.
             */
            auto const visibleToReader = [this, botGuidCounter](PlayerbotSocialMemoryRecord const& record)
            {
                if (record.subjectGuidCounter != 0 && _actorIds.find(record.subjectGuidCounter) == _actorIds.end())
                    return false;

                return PairMayBeStored(botGuidCounter, record.subjectGuidCounter);
            };

            _state.ReplaceMemoriesOwnedBy(botGuidCounter, query, snapshot, visibleToReader);
        }));
}

bool PlayerbotSocialMgr::PersistRelationship(uint64 botGuidCounter, uint64 subjectGuidCounter,
                                             PlayerbotSocialRelationshipValues const& values, uint64 nowUnixSeconds)
{
    PlayerbotSocialRelationshipKey const key{botGuidCounter, subjectGuidCounter};
    if (key.botGuidCounter == 0 || key.subjectGuidCounter == 0)
        return false;

    if (!PairMayBeStored(key.botGuidCounter, key.subjectGuidCounter))
        return false;

    auto const bot = _actorIds.find(key.botGuidCounter);
    auto const subject = _actorIds.find(key.subjectGuidCounter);
    if (bot == _actorIds.end() || subject == _actorIds.end())
        return false;

    // The store applies its own consent rule as well. It is the weaker of the two, because it cannot
    // tell an absent answer from a positive one, so the check above is the one that has to hold.
    if (!_state.RememberRelationship(key, values))
        return false;

    /*
     * One interaction is contributed per write and the database adds it to whatever is already
     * there, rather than this process deciding the running total. Two reasons: the count survives a
     * restart without being read back first, and a caller cannot reset somebody's history to zero by
     * passing a total it computed from an empty snapshot.
     */
    PlayerbotSocialRelationshipBinding const binding =
        PlayerbotSocialBuildRelationshipBinding(key, values, 1, nowUnixSeconds);

    PlayerbotSocialPreparedStatement* statement = NewPlayerbotSocialStatement(PLAYERBOT_SOCIAL_STMT_INS_RELATIONSHIP);
    statement->SetData(0, MakeRelationshipPublicId(key));
    statement->SetData(1, bot->second);
    statement->SetData(2, subject->second);
    statement->SetData(3, binding.familiarity);
    statement->SetData(4, binding.affinity);
    statement->SetData(5, binding.trust);
    statement->SetData(6, binding.interactionCount);
    statement->SetData(7, binding.lastInteractionAtUnixSeconds);
    PlayerbotSocialExecute(statement);

    // A read for this pair issued before this write would land afterwards and overwrite it.
    InvalidateRelationshipRead(key);

    return true;
}

PlayerbotSocialMemoryRejection PlayerbotSocialMgr::PersistMemory(PlayerbotSocialMemoryRecord const& record)
{
    if (!PairMayBeStored(record.botGuidCounter, record.subjectGuidCounter))
        return PlayerbotSocialMemoryRejection::CharacterOptedOut;

    auto const bot = _actorIds.find(record.botGuidCounter);
    if (bot == _actorIds.end())
        return PlayerbotSocialMemoryRejection::UnresolvedActor;

    uint32 subjectActorId = 0;
    if (record.subjectGuidCounter != 0)
    {
        auto const subject = _actorIds.find(record.subjectGuidCounter);
        if (subject == _actorIds.end())
            return PlayerbotSocialMemoryRejection::UnresolvedActor;

        subjectActorId = subject->second;
    }

    /*
     * Checked ahead of the store rather than after it. The three validators the store applies
     * enumerate exactly the enumerators these three namers spell, so while the two lists agree
     * nothing here can fire. Should one gain an enumerator the other does not, a value the store
     * accepts would name nothing below, and running this check afterwards would already have left
     * that memory in the cache with no row behind it. Each rejection is the one the store itself
     * returns for the same value, and they are tested in the store's order, so the answer a caller
     * sees is unchanged for every value both lists know.
     */
    std::string_view const category = PlayerbotSocialMemoryCategoryName(record.category);
    if (category.empty())
        return PlayerbotSocialMemoryRejection::UnknownCategory;

    std::string_view const provenance = PlayerbotSocialMemoryProvenanceName(record.provenance);
    if (provenance.empty())
        return PlayerbotSocialMemoryRejection::UnknownProvenance;

    std::string_view const scope = PlayerbotSocialPrivacyScopeName(record.scope);
    if (scope.empty())
        return PlayerbotSocialMemoryRejection::UnknownPrivacyScope;

    PlayerbotSocialMemoryRejection const candidateRejection = PlayerbotSocialValidateMemoryCandidate(record);
    if (candidateRejection != PlayerbotSocialMemoryRejection::None)
        return candidateRejection;

    std::string_view const sourceKind = PlayerbotSocialMemorySourceKindName(*record.sourceKind);

    // One token identifies this exact cached insertion while its database result is pending. Content
    // equality is insufficient because two fully identical memories can have different write outcomes.
    uint64 const writeToken = _nextMemorySequence++;
    PlayerbotSocialMemoryRecord cachedRecord = record;
    cachedRecord.writeToken = writeToken;

    // Validation, consent, and storage all happen here, so nothing reaches the statement below that
    // the in-memory store refused.
    PlayerbotSocialMemoryRejection const rejection = _state.RememberMemory(cachedRecord);
    if (rejection != PlayerbotSocialMemoryRejection::None)
        return rejection;

    PlayerbotSocialPreparedStatement* statement = NewPlayerbotSocialStatement(PLAYERBOT_SOCIAL_STMT_INS_MEMORY);
    statement->SetData(0, MakeMemoryPublicId(writeToken, record.botGuidCounter));
    statement->SetData(1, bot->second);
    if (subjectActorId != 0)
        statement->SetData(2, subjectActorId);
    else
        statement->SetData(2, nullptr);
    statement->SetData(3, category);
    statement->SetData(4, record.paraphrase);
    statement->SetData(5, provenance);
    statement->SetData(6, record.confidence);
    statement->SetData(7, record.significance);
    statement->SetData(8, scope);
    statement->SetData(9, record.sourceEventPublicId);
    statement->SetData(10, record.sourceThreadPublicId);
    statement->SetData(11, sourceKind);

    // No expiry. Retention deletes raw event text on a clock; a paraphrased memory is kept until the
    // character it concerns is reset or the bot holding it is deleted.
    statement->SetData(12, nullptr);

    /*
     * The one social write whose outcome is watched, rather than enqueued one way like the rest.
     *
     * Task 10B Definition of Done 3 says a repository failure path performs NO memory write, and
     * Task 5 Key Decision 8 says such a failure emits a diagnostic event and invents no remembered
     * fact. Neither can hold while the cache keeps a record the database refused, so this statement
     * is committed as a transaction and its result is read. Nothing else here changes: relationships,
     * actors, events, moderation and retention stay one way, because those two commitments are about
     * memory and only memory.
     */
    PlayerbotsDatabaseTransaction transaction = PlayerbotsDatabase.BeginTransaction();
    transaction->Append(ConsumePlayerbotSocialSql(statement));

    // Read before the commit is issued, so the callback can tell this write's cached record from an
    // identical one written after an erasure it never saw.
    uint64 const writtenAtEpoch = _stateEpoch;

    SocialTransactionProcessor()
        .AddCallback(PlayerbotsDatabase.AsyncCommitTransaction(transaction))
        .AfterComplete(
            [this, cachedRecord, writtenAtEpoch](bool success)
            {
                if (success)
                    return;

                /*
                 * Runs on the world thread, because this processor is pumped from
                 * `UpdateDatabaseWork` beside the read one. The decision itself lives in a free
                 * function so it can be tested; all that is left here is applying it.
                 */
                static_cast<void>(PlayerbotSocialHandleMemoryWriteFailure(
                    _state, cachedRecord, writtenAtEpoch, _stateEpoch,
                    [this](uint64 botGuidCounter) { ForgetMemorySnapshotsOf(botGuidCounter); },
                    [this](PlayerbotSocialEventDraft draft) { RecordEvent(std::move(draft)); }));
            });

    // The pair's snapshot no longer reflects the durable rows for this bot, so the next read refreshes
    // instead of serving a view that predates this write.
    ForgetMemorySnapshotsOf(record.botGuidCounter);

    return PlayerbotSocialMemoryRejection::None;
}

namespace
{
/*
 * Why this opportunity produced no request, or empty when it produced one.
 *
 * The order matters: the earliest stage that stopped it is the honest answer. A thread refused
 * outright never reached selection, so reporting a later stage's silence would name a decision
 * that was never made.
 */
std::string OpportunitySuppressionReason(PlayerbotSocialActivationResult const& result)
{
    if (!result.openedTokens.empty())
        return {};

    if (result.rejection != PlayerbotSocialOpportunityRejection::None)
        return PlayerbotSocialOpportunityRejectionName(result.rejection);

    if (result.pressureDeclined)
        return std::string(PLAYERBOT_SOCIAL_REASON_PRESSURE_DECLINED);

    // Selection ran and chose someone, and every one of those requests was refused. The first
    // refusal is a real cause rather than a summary invented over several.
    if (!result.refusedRequests.empty())
        return PlayerbotSocialDeliveryRejectionName(result.refusedRequests.front().second);

    if (!result.refusedCandidates.empty())
        return PlayerbotSocialOpportunityRejectionName(result.refusedCandidates.front().second);

    // Selection ran, suppressed everyone, and named why per bot.
    if (!result.selection.suppressions.empty())
        return PlayerbotSocialSuppressionReasonName(result.selection.suppressions.front().reason);

    return "no_responder";
}

void AppendUnsignedArray(std::string& out, char const* name, std::vector<uint64> const& values, std::size_t limit)
{
    out += ",\"";
    out += name;
    out += "\":[";

    std::size_t const count = std::min(values.size(), limit);
    for (std::size_t index = 0; index < count; ++index)
    {
        if (index != 0)
            out += ',';
        out += std::to_string(values[index]);
    }

    out += ']';
}
}  // namespace

PlayerbotSocialEventDraft PlayerbotSocialMakeOpportunityEvent(PlayerbotSocialActivation const& activation,
                                                              PlayerbotSocialActivationResult const& result)
{
    PlayerbotSocialEventDraft draft;
    draft.eventType = std::string(PLAYERBOT_SOCIAL_EVENT_TYPE_OPPORTUNITY);
    draft.origin = PlayerbotSocialEventOrigin::Social;
    draft.channel = activation.channel;
    draft.hasChannel = true;
    draft.threadPublicId = activation.thread.publicId;
    draft.actorGuidCounter = activation.speakerGuidCounter;
    draft.zoneId = activation.zoneId;
    draft.occurredAtUnixSeconds = activation.nowUnixSeconds;

    draft.reason = OpportunitySuppressionReason(result);
    draft.outcome =
        draft.reason.empty() ? PlayerbotSocialEventOutcome::Recorded : PlayerbotSocialEventOutcome::Suppressed;

    /*
     * The bot is named only when one was actually selected AND its request opened. Naming a
     * responder whose request was refused would attribute a silence to a bot that never got the
     * chance to speak, which reads in the feed as a bot choosing not to answer.
     */
    if (!result.openedTokens.empty() && !result.selection.responders.empty())
        draft.botGuidCounter = result.selection.responders.front();

    /*
     * Bounded by construction rather than by truncating a string that grew first. The totals are
     * always reported; only the detail is capped, so a busy zone still says how many bots it
     * considered without listing them.
     */
    /*
     * Counted from the activation's own candidate list, which is exact: every bot in it was
     * considered, and each one either appears in `refusedCandidates` or went to selection. Summing
     * the result's lists instead would be wrong twice over, because `alternates` is the whole ranked
     * field including the bots that became responders, and it is capped.
     */
    std::string diagnostics = "{\"considered\":";
    diagnostics += std::to_string(activation.candidates.size());
    diagnostics += ",\"pressure\":";
    diagnostics += std::to_string(static_cast<int32>(result.pressure * 100.0f));

    // The head of the ranked field. It includes the selected bot rather than excluding it, because
    // what makes a decision reviewable is seeing who the runner up was next to who won.
    AppendUnsignedArray(diagnostics, "top", result.selection.alternates, PLAYERBOT_SOCIAL_MAX_REPORTED_ALTERNATES);

    diagnostics += ",\"factors\":[";
    std::size_t const factorCount =
        std::min(result.selection.leadingFactors.size(), PLAYERBOT_SOCIAL_MAX_REPORTED_FACTORS);
    for (std::size_t index = 0; index < factorCount; ++index)
    {
        PlayerbotSocialSelectionFactor const& factor = result.selection.leadingFactors[index];
        if (index != 0)
            diagnostics += ',';

        diagnostics += "{\"n\":\"";
        diagnostics += factor.name != nullptr ? factor.name : "unknown";
        diagnostics += "\",\"v\":";
        diagnostics += std::to_string(factor.contribution);
        diagnostics += '}';
    }
    diagnostics += "]}";

    /*
     * A last resort, not the bound. Every list above is already capped, so reaching this means a
     * factor name is far longer than any literal in this module, and an unparseable tail is still
     * better than an oversized column.
     */
    if (diagnostics.size() > PLAYERBOT_SOCIAL_MAX_OPPORTUNITY_DIAGNOSTICS_LENGTH)
        diagnostics.resize(PLAYERBOT_SOCIAL_MAX_OPPORTUNITY_DIAGNOSTICS_LENGTH);

    draft.diagnosticsJson = std::move(diagnostics);
    return draft;
}

namespace
{
/*
 * Where a bot placed in the ranked field, one based, or zero when it is not in it at all.
 *
 * Zero rather than a guess. The field can be shorter than the decision that produced it, and a
 * bot missing from it reading as rank one would invent a standing it never had.
 */
uint32 RankWithin(std::vector<uint64> const& alternates, uint64 botGuidCounter)
{
    auto const found = std::find(alternates.begin(), alternates.end(), botGuidCounter);
    if (found == alternates.end())
        return 0;

    return static_cast<uint32>(std::distance(alternates.begin(), found)) + 1;
}

// The leading factors, capped and shaped the same way the opportunity event shapes them, so a
// consumer parses one form rather than two.
void AppendLeadingFactors(std::string& out, std::vector<PlayerbotSocialSelectionFactor> const& factors)
{
    out += ",\"factors\":[";

    std::size_t const count = std::min(factors.size(), PLAYERBOT_SOCIAL_MAX_REPORTED_FACTORS);
    for (std::size_t index = 0; index < count; ++index)
    {
        if (index != 0)
            out += ',';

        out += "{\"n\":\"";
        out += factors[index].name != nullptr ? factors[index].name : "unknown";
        out += "\",\"v\":";
        out += std::to_string(factors[index].contribution);
        out += '}';
    }

    out += "]}";
}
}  // namespace

PlayerbotSocialEventDraft PlayerbotSocialMakeSelectionEvent(PlayerbotSocialActivation const& activation,
                                                            PlayerbotSocialActivationResult const& result,
                                                            uint64 responderGuidCounter)
{
    PlayerbotSocialEventDraft draft;
    draft.eventType = std::string(PLAYERBOT_SOCIAL_EVENT_TYPE_SELECTION);
    draft.origin = PlayerbotSocialEventOrigin::Social;
    draft.outcome = PlayerbotSocialEventOutcome::Recorded;
    draft.channel = activation.channel;
    draft.hasChannel = true;
    draft.threadPublicId = activation.thread.publicId;
    draft.botGuidCounter = responderGuidCounter;
    draft.actorGuidCounter = activation.speakerGuidCounter;
    draft.zoneId = activation.zoneId;
    draft.occurredAtUnixSeconds = activation.nowUnixSeconds;

    /*
     * Correlation detail rather than feed content. Under queue pressure the ability to reconstruct
     * WHY this bot answered is what gives way first, so the delivery it explains still lands.
     */
    draft.priority = PlayerbotSocialEventPriority::Diagnostic;

    std::string diagnostics = "{\"rank\":";
    diagnostics += std::to_string(RankWithin(result.selection.alternates, responderGuidCounter));
    diagnostics += ",\"field\":";
    diagnostics += std::to_string(result.selection.alternates.size());
    diagnostics += ",\"pressure\":";
    diagnostics += std::to_string(static_cast<int32>(result.pressure * 100.0f));
    AppendLeadingFactors(diagnostics, result.selection.leadingFactors);

    // The same last resort the opportunity diagnostics carry: every list above is already capped, so
    // reaching this means a factor name is longer than any literal in this module.
    if (diagnostics.size() > PLAYERBOT_SOCIAL_MAX_OPPORTUNITY_DIAGNOSTICS_LENGTH)
        diagnostics.resize(PLAYERBOT_SOCIAL_MAX_OPPORTUNITY_DIAGNOSTICS_LENGTH);

    draft.diagnosticsJson = std::move(diagnostics);
    return draft;
}

namespace
{
void AppendSocialCallMetadata(std::string& diagnostics, PlayerbotSocialCallMetadata const& metadata)
{
    if (diagnostics.back() != '{')
        diagnostics += ',';
    diagnostics += "\"model\":";
    AppendJsonString(diagnostics, metadata.model);
    diagnostics += ",\"provider_latency_ms\":" + std::to_string(metadata.providerLatencyMs);
    diagnostics += ",\"input_tokens\":" + std::to_string(metadata.inputTokens);
    diagnostics += ",\"output_tokens\":" + std::to_string(metadata.outputTokens);
    diagnostics += ",\"cache_creation_input_tokens\":" + std::to_string(metadata.cacheCreationInputTokens);
    diagnostics += ",\"cache_read_input_tokens\":" + std::to_string(metadata.cacheReadInputTokens);
    diagnostics += ",\"cost_usd\":";
    AppendJsonString(diagnostics, metadata.costUsd);
}
}  // namespace

std::optional<std::string> PlayerbotSocialSerializeOperatorEvidence(PlayerbotSocialOperatorEvidence const& evidence)
{
    if (!PlayerbotSocialGroundingEnvelopeIsValid(evidence.grounding) ||
        !PlayerbotSocialContributionFunctionIsValid(evidence.contribution))
        return std::nullopt;

    char const* const profileState = PlayerbotSocialProfileLoadStateName(evidence.profileLoad.state);
    char const* const rejection = PlayerbotSocialProfileRejectionName(evidence.profileLoad.rejection);
    char const* const biographyState = PlayerbotSocialBiographyStateColumn(evidence.profileLoad.storedBiographyState);
    char const* const stage = PlayerbotSocialRolloutStageName(evidence.rolloutStage);
    char const* const memoryState = MemoryInputStateName(evidence.grounding.memoryInputState);
    if (std::string_view(profileState) == "unknown" || std::string_view(rejection) == "unknown" ||
        biographyState == nullptr || std::string_view(stage) == "unknown" || memoryState == nullptr)
        return std::nullopt;

    if (evidence.citedEvidenceIds.size() > PLAYERBOT_SOCIAL_EVIDENCE_MAX_ENTRIES)
        return std::nullopt;

    bool const hasCitations = !evidence.citedEvidenceIds.empty();
    if ((evidence.contribution == PlayerbotSocialContributionFunction::Answer && !hasCitations) ||
        ((evidence.contribution == PlayerbotSocialContributionFunction::FactFreeBanter ||
          evidence.contribution == PlayerbotSocialContributionFunction::Gesture ||
          evidence.contribution == PlayerbotSocialContributionFunction::None) &&
         hasCitations))
        return std::nullopt;

    for (std::size_t index = 0; index < evidence.citedEvidenceIds.size(); ++index)
    {
        if (!EvidenceIdIsSafe(evidence.citedEvidenceIds[index]))
            return std::nullopt;

        if (std::find(evidence.citedEvidenceIds.begin(), evidence.citedEvidenceIds.begin() + index,
                      evidence.citedEvidenceIds[index]) != evidence.citedEvidenceIds.begin() + index)
            return std::nullopt;

        if (std::none_of(evidence.grounding.entries.begin(), evidence.grounding.entries.end(),
                         [&evidence, index](PlayerbotSocialEvidenceEntry const& entry)
                         { return entry.id == evidence.citedEvidenceIds[index]; }))
            return std::nullopt;
    }

    std::string out = "{\"stage\":";
    AppendJsonString(out, stage);
    out += ",\"profile\":{\"state\":";
    AppendJsonString(out, profileState);
    out += ",\"row_present\":";
    out += evidence.profileLoad.storedRowPresent ? "true" : "false";
    out += ",\"schema_version\":" + std::to_string(evidence.profileLoad.storedSchemaVersion);
    out += ",\"traits_version\":" + std::to_string(evidence.profileLoad.storedTraitsVersion);
    out += ",\"biography_state\":";
    AppendJsonString(out, biographyState);
    out += ",\"biography_version\":" + std::to_string(evidence.profileLoad.storedBiographyVersion);
    out += ",\"rejection\":";
    AppendJsonString(out, rejection);
    out += "},\"memory\":{\"state\":";
    AppendJsonString(out, memoryState);
    out += "},\"active_expansion\":" + std::to_string(evidence.grounding.activeContentExpansion);
    out += ",\"facts\":[";

    for (std::size_t index = 0; index < evidence.grounding.entries.size(); ++index)
    {
        PlayerbotSocialEvidenceEntry const& entry = evidence.grounding.entries[index];
        char const* const subject = EvidenceSubjectName(entry.subjectRole);
        char const* const fact = EvidenceFactName(entry.factKind);
        char const* const provenance = EvidenceProvenanceName(entry.provenance);
        std::string_view const scope = PlayerbotSocialPrivacyScopeName(entry.scope);
        if (subject == nullptr || fact == nullptr || provenance == nullptr || scope.empty() || scope == "unknown")
            return std::nullopt;

        if (index != 0)
            out += ',';
        out += "{\"id\":";
        AppendJsonString(out, entry.id);
        out += ",\"subject\":";
        AppendJsonString(out, subject);
        out += ",\"fact\":";
        AppendJsonString(out, fact);
        out += ",\"value\":";
        AppendJsonString(out, entry.value);
        out += ",\"provenance\":";
        AppendJsonString(out, provenance);
        out += ",\"scope\":";
        AppendJsonString(out, std::string(scope));
        out += ",\"at\":" + std::to_string(entry.atUnixSeconds) + '}';
    }

    out += "],\"transcript_event_ids\":[";
    for (std::size_t index = 0; index < evidence.grounding.transcriptEventPublicIds.size(); ++index)
    {
        std::string const& eventId = evidence.grounding.transcriptEventPublicIds[index];
        if (!PlayerbotSocialPublicIdIsValid(PlayerbotSocialIdKind::Event, eventId))
            return std::nullopt;
        if (index != 0)
            out += ',';
        AppendJsonString(out, eventId);
    }

    out += "],\"response\":{\"function\":";
    AppendJsonString(out, PlayerbotSocialContributionFunctionName(evidence.contribution));
    out += ",\"cited_evidence_ids\":[";
    for (std::size_t index = 0; index < evidence.citedEvidenceIds.size(); ++index)
    {
        if (index != 0)
            out += ',';
        AppendJsonString(out, evidence.citedEvidenceIds[index]);
    }
    out += "]}}";

    if (out.size() > PLAYERBOT_SOCIAL_OPERATOR_EVIDENCE_MAX_BYTES)
        return std::nullopt;

    return out;
}

PlayerbotSocialEventDraft PlayerbotSocialMakeProviderAttemptEvent(PlayerbotSocialProviderAttempt const& attempt)
{
    PlayerbotSocialEventDraft draft;
    draft.eventType = std::string(PLAYERBOT_SOCIAL_EVENT_TYPE_PROVIDER_ATTEMPT);
    draft.origin = PlayerbotSocialEventOrigin::Social;
    draft.channel = attempt.channel;
    draft.hasChannel = true;
    draft.threadPublicId = attempt.threadPublicId;
    draft.botGuidCounter = attempt.botGuidCounter;
    draft.targetGuidCounter = attempt.targetGuidCounter;
    draft.zoneId = attempt.zoneId;
    draft.occurredAtUnixSeconds = attempt.occurredAtUnixSeconds;
    draft.priority = PlayerbotSocialEventPriority::Diagnostic;

    switch (attempt.outcome)
    {
        case PlayerbotSocialProviderAttemptOutcome::Answered:
            draft.outcome = PlayerbotSocialEventOutcome::Recorded;
            break;
        case PlayerbotSocialProviderAttemptOutcome::Silent:
            draft.outcome = PlayerbotSocialEventOutcome::Suppressed;
            draft.reason = std::string(PLAYERBOT_SOCIAL_REASON_PROVIDER_SILENCE);
            break;
        case PlayerbotSocialProviderAttemptOutcome::Refused:
            draft.outcome = PlayerbotSocialEventOutcome::Failed;
            draft.reason = std::string(PlayerbotSocialDeliveryRejectionName(attempt.rejection));
            break;
    }

    std::string diagnostics = "{\"token\":" + std::to_string(attempt.requestToken);
    if (attempt.callMetadata)
        AppendSocialCallMetadata(diagnostics, *attempt.callMetadata);
    if (attempt.operatorEvidence)
        AppendOperatorEvidenceField(diagnostics, *attempt.operatorEvidence);
    diagnostics += '}';
    draft.diagnosticsJson = std::move(diagnostics);
    return draft;
}

namespace
{
/*
 * One relationship triple, in hundredths.
 *
 * Integers rather than floats so the JSON is stable across platforms and short enough to fit the
 * diagnostics budget alongside a second triple. Two decimal places is finer than any single
 * encounter can earn, so nothing meaningful is lost to the rounding.
 */
void AppendRelationshipValues(std::string& out, char const* name, PlayerbotSocialRelationshipValues const& values)
{
    out += '"';
    out += name;
    out += "\":{\"f\":";
    out += std::to_string(static_cast<int32>(values.familiarity * 100.0f));
    out += ",\"a\":";
    out += std::to_string(static_cast<int32>(values.affinity * 100.0f));
    out += ",\"t\":";
    out += std::to_string(static_cast<int32>(values.trust * 100.0f));
    out += '}';
}
}  // namespace

PlayerbotSocialEventDraft PlayerbotSocialMakeAssistanceEvent(PlayerbotSocialAssistanceCompletion const& completion)
{
    PlayerbotSocialEventDraft draft;
    draft.eventType = std::string(PLAYERBOT_SOCIAL_EVENT_TYPE_ASSISTANCE);
    draft.origin = PlayerbotSocialEventOrigin::Assistance;
    draft.outcome = PlayerbotSocialEventOutcome::Recorded;

    /*
     * No channel and no thread, and the flag says so rather than leaving General to be inferred from
     * a defaulted enum. Both columns are nullable precisely for this class of event.
     */
    draft.hasChannel = false;

    // The bot whose relationship moved, matching the row ApplyRelationshipDelta writes, so the feed
    // and the relationship table name the same pair in the same order.
    draft.botGuidCounter = completion.beneficiaryGuidCounter;
    draft.actorGuidCounter = completion.helperGuidCounter;
    draft.occurredAtUnixSeconds = completion.occurredAtUnixSeconds;

    /*
     * No zone. An encounter completes on an idle sweep that can run long after the fight, by which
     * time either character may be offline, so there is no zone to read that would still be true.
     * Absent is honest; the zone the fight happened in is not recoverable here.
     */
    draft.priority = PlayerbotSocialEventPriority::Standard;

    std::string diagnostics = "{";
    AppendRelationshipValues(diagnostics, "earned", completion.earned);
    diagnostics += ',';
    AppendRelationshipValues(diagnostics, "applied", completion.applied);
    diagnostics += '}';

    draft.diagnosticsJson = std::move(diagnostics);
    return draft;
}

PlayerbotSocialEventDraft PlayerbotSocialMakePvpEvent(PlayerbotSocialPvpOpposition const& opposition)
{
    PlayerbotSocialEventDraft draft;
    draft.eventType = std::string(PLAYERBOT_SOCIAL_EVENT_TYPE_PVP);
    draft.origin = PlayerbotSocialEventOrigin::Pvp;
    draft.outcome = PlayerbotSocialEventOutcome::Recorded;
    draft.hasChannel = false;
    draft.botGuidCounter = opposition.victimGuidCounter;
    draft.actorGuidCounter = opposition.attackerGuidCounter;
    draft.occurredAtUnixSeconds = opposition.occurredAtUnixSeconds;
    draft.priority = PlayerbotSocialEventPriority::Standard;

    // The context IS the reason, not a diagnostic. A gank and a duel produce different rows, and
    // burying the difference in a JSON blob would make the feed unable to group by it.
    draft.reason = std::string(PlayerbotSocialCombatContextName(opposition.context));

    std::string diagnostics = "{";
    AppendRelationshipValues(diagnostics, "earned", opposition.earned);
    diagnostics += '}';

    draft.diagnosticsJson = std::move(diagnostics);
    return draft;
}

PlayerbotSocialEventDraft PlayerbotSocialMakeExtractionEvent(PlayerbotSocialExtractionAttempt const& attempt)
{
    PlayerbotSocialEventDraft draft;
    draft.eventType = std::string(PLAYERBOT_SOCIAL_EVENT_TYPE_EXTRACTION);
    draft.origin = PlayerbotSocialEventOrigin::Social;
    draft.channel = attempt.channel;
    draft.hasChannel = true;
    draft.threadPublicId = attempt.threadPublicId;
    draft.botGuidCounter = attempt.botGuidCounter;
    draft.occurredAtUnixSeconds = attempt.occurredAtUnixSeconds;

    /*
     * Four outcomes, and telling them apart is the whole value of this event.
     *
     * Suppressed: the submission gate refused the thread, and the refusal names itself so the feed
     * can be counted by cause. Delivered: something was written. Recorded: the provider answered
     * and the conversation supported nothing, which is the commonest result and a success. Failed:
     * the request went out and nothing ever came back.
     *
     * Collapsing any two of them leaves an operator unable to tell "working, nothing to store" from
     * "quietly broken", which is the question a memory feature goes wrong at.
     */
    if (attempt.refusal != PlayerbotSocialSnapshotRefusal::Accepted)
    {
        draft.outcome = PlayerbotSocialEventOutcome::Suppressed;
        draft.reason = PlayerbotSocialSnapshotRefusalName(attempt.refusal);
    }
    else if (attempt.written > 0)
    {
        draft.outcome = PlayerbotSocialEventOutcome::Delivered;
    }
    else if (attempt.answered)
    {
        draft.outcome = PlayerbotSocialEventOutcome::Recorded;
        draft.reason = std::string(PLAYERBOT_SOCIAL_REASON_NOTHING_TO_REMEMBER);
    }
    else
    {
        draft.outcome = PlayerbotSocialEventOutcome::Failed;
    }

    /*
     * Sizes only, never content. `messageText` is left empty deliberately rather than by omission:
     * it is the one field on this draft that could carry chat, and the buffer's retention window is
     * far shorter than the event table's, so a line written here would outlive the bound the buffer
     * exists to enforce.
     */
    draft.diagnosticsJson = "{\"lines\":" + std::to_string(attempt.lineCount) +
                            ",\"subjects\":" + std::to_string(attempt.subjectCount) +
                            ",\"written\":" + std::to_string(attempt.written) + "}";

    draft.priority = PlayerbotSocialEventPriority::Standard;
    return draft;
}

namespace
{
PlayerbotSocialMemoryWriteFailureAction ResolveMemoryWriteFailure(PlayerbotSocialStateStore& state,
                                                                  PlayerbotSocialMemoryRecord const& record,
                                                                  uint64 writtenAtEpoch, uint64 currentEpoch)
{
    /*
     * An erasure landed between the write and its refusal, so the cache no longer holds what this
     * write put there. Anything matching now was written after the erasure, by a statement whose
     * own outcome is not this one, and removing it would drop a memory that is durable.
     */
    if (writtenAtEpoch != currentEpoch)
        return PlayerbotSocialMemoryWriteFailureAction::StaleEpoch;

    bool const dropped = state.ForgetMemoryMatching(
        [&record](PlayerbotSocialMemoryRecord const& cached)
        {
            /*
             * The manager-lifetime token is the identity. The remaining fields are a defensive
             * check that the caller did not pair that token with different content.
             */
            return cached.writeToken == record.writeToken && cached.botGuidCounter == record.botGuidCounter &&
                   cached.subjectGuidCounter == record.subjectGuidCounter && cached.category == record.category &&
                   cached.provenance == record.provenance && cached.scope == record.scope &&
                   cached.confidence == record.confidence && cached.significance == record.significance &&
                   cached.paraphrase == record.paraphrase;
        });

    return dropped ? PlayerbotSocialMemoryWriteFailureAction::Dropped
                   : PlayerbotSocialMemoryWriteFailureAction::AlreadyGone;
}
}  // namespace

PlayerbotSocialMemoryWriteFailureAction PlayerbotSocialHandleMemoryWriteFailure(
    PlayerbotSocialStateStore& state, PlayerbotSocialMemoryRecord const& record, uint64 writtenAtEpoch,
    uint64 currentEpoch, std::function<void(uint64)> const& invalidateSnapshots,
    std::function<void(PlayerbotSocialEventDraft)> const& recordEvent)
{
    PlayerbotSocialMemoryWriteFailureAction const action =
        ResolveMemoryWriteFailure(state, record, writtenAtEpoch, currentEpoch);

    // Only a removal changes what a read would find. The other outcomes leave the cache untouched.
    if (action == PlayerbotSocialMemoryWriteFailureAction::Dropped && invalidateSnapshots)
        invalidateSnapshots(record.botGuidCounter);

    if (recordEvent)
        recordEvent(
            PlayerbotSocialMakeMemoryPersistenceFailureEvent(record.botGuidCounter, record.subjectGuidCounter, action));

    return action;
}

PlayerbotSocialEventDraft PlayerbotSocialMakeMemoryPersistenceFailureEvent(
    uint64 botGuidCounter, uint64 subjectGuidCounter, PlayerbotSocialMemoryWriteFailureAction action)
{
    PlayerbotSocialEventDraft draft;
    draft.eventType = std::string(PLAYERBOT_SOCIAL_EVENT_TYPE_MEMORY_PERSISTENCE);
    draft.origin = PlayerbotSocialEventOrigin::System;

    // Every case is a write the database refused, so none of them reports as anything but a failure.
    // What differs is what became of the cached copy, which is the reason and nothing else.
    draft.outcome = PlayerbotSocialEventOutcome::Failed;

    // No conversation surface. The refusal belongs to a statement, not to the channel whose words
    // happened to produce the memory, and General is a legitimate value that a default would mean.
    draft.hasChannel = false;

    draft.botGuidCounter = botGuidCounter;
    draft.targetGuidCounter = subjectGuidCounter;

    std::string_view actionName;
    switch (action)
    {
        case PlayerbotSocialMemoryWriteFailureAction::Dropped:
            draft.reason = std::string(PLAYERBOT_SOCIAL_REASON_MEMORY_WRITE_REFUSED);
            actionName = "dropped";
            break;
        case PlayerbotSocialMemoryWriteFailureAction::AlreadyGone:
            draft.reason = std::string(PLAYERBOT_SOCIAL_REASON_MEMORY_ALREADY_GONE);
            actionName = "already_gone";
            break;
        case PlayerbotSocialMemoryWriteFailureAction::StaleEpoch:
            draft.reason = std::string(PLAYERBOT_SOCIAL_REASON_MEMORY_STATE_RESET);
            actionName = "stale_epoch";
            break;
    }

    // No paraphrase, no subject name, no text of any kind. What failed is the operational fact; the
    // content of the memory that failed is exactly what a diagnostic feed must not carry.
    draft.diagnosticsJson = std::string("{\"action\":\"") + std::string(actionName) + "\"}";

    draft.priority = PlayerbotSocialEventPriority::Critical;
    return draft;
}

PlayerbotSocialEventDraft PlayerbotSocialMakeDeliveryEvent(PlayerbotSocialDelivery const& delivery)
{
    PlayerbotSocialEventDraft draft;
    draft.eventPublicId = delivery.eventPublicId;
    draft.replyToEventPublicId = delivery.replyToEventPublicId;
    draft.sourceEventPublicId = delivery.sourceEventPublicId;
    draft.eventType = std::string(PLAYERBOT_SOCIAL_EVENT_TYPE_DELIVERY);
    draft.origin = delivery.origin;

    /*
     * Always `Delivered`, because this type only ever describes a send the world accepted. That is
     * also what lets the binding keep the text: it drops the message for every other outcome, so a
     * line nobody heard could not be retained even if a producer tried.
     */
    draft.outcome = PlayerbotSocialEventOutcome::Delivered;

    draft.channel = delivery.channel;
    draft.hasChannel = true;
    draft.threadPublicId = delivery.threadPublicId;
    draft.botGuidCounter = delivery.botGuidCounter;
    draft.targetGuidCounter = delivery.targetGuidCounter;
    draft.zoneId = delivery.zoneId;
    draft.occurredAtUnixSeconds = delivery.occurredAtUnixSeconds;

    // What the feed is actually about, so it is the last thing to give way under queue pressure.
    draft.priority = PlayerbotSocialEventPriority::Standard;

    // A gesture has no line, and an emote carrying text would put words in the feed the bot never
    // said. The text is dropped here rather than trusted to be empty.
    if (delivery.isEmote)
    {
        std::string diagnostics = "{\"emote\":" + std::to_string(delivery.emoteId);
        if (delivery.callMetadata)
            AppendSocialCallMetadata(diagnostics, *delivery.callMetadata);
        if (delivery.operatorEvidence)
            AppendOperatorEvidenceField(diagnostics, *delivery.operatorEvidence);
        diagnostics += '}';
        draft.diagnosticsJson = std::move(diagnostics);
    }
    else
    {
        draft.messageText = delivery.text;
        if (delivery.callMetadata || delivery.operatorEvidence)
        {
            std::string diagnostics = "{";
            if (delivery.callMetadata)
                AppendSocialCallMetadata(diagnostics, *delivery.callMetadata);
            if (delivery.operatorEvidence)
                AppendOperatorEvidenceField(diagnostics, *delivery.operatorEvidence);
            diagnostics += '}';
            draft.diagnosticsJson = std::move(diagnostics);
        }
    }

    return draft;
}

PlayerbotSocialEventDraft PlayerbotSocialMakeDeliverySuppressionEvent(PlayerbotSocialPendingDelivery const& pending,
                                                                      PlayerbotSocialDeliveryRejection rejection,
                                                                      uint64 occurredAtUnixSeconds)
{
    PlayerbotSocialEventDraft draft;
    draft.eventType = std::string(PLAYERBOT_SOCIAL_EVENT_TYPE_DELIVERY);
    draft.origin = PlayerbotSocialEventOrigin::Social;
    draft.outcome = PlayerbotSocialEventOutcome::Suppressed;
    draft.channel = pending.channel;
    draft.hasChannel = true;
    draft.threadPublicId = pending.threadPublicId;
    draft.replyToEventPublicId = pending.replyToEventPublicId;
    draft.sourceEventPublicId = pending.sourceEventPublicId;
    draft.botGuidCounter = pending.botGuidCounter;
    draft.targetGuidCounter = pending.targetGuidCounter;
    draft.zoneId = pending.zoneId;
    draft.reason = PlayerbotSocialDeliveryRejectionName(rejection);
    draft.occurredAtUnixSeconds = occurredAtUnixSeconds;
    draft.priority = PlayerbotSocialEventPriority::Standard;
    if (pending.operatorEvidence)
    {
        std::string diagnostics = "{";
        AppendOperatorEvidenceField(diagnostics, *pending.operatorEvidence);
        diagnostics += '}';
        draft.diagnosticsJson = std::move(diagnostics);
    }
    return draft;
}

std::string PlayerbotSocialMgr::ReserveDeliveryEventPublicId(uint64 botGuidCounter)
{
    return PlayerbotSocialMakeEventPublicId(_nextEventSequence++, botGuidCounter);
}

bool PlayerbotSocialMgr::PrepareHumanObservation(PlayerbotSocialObservation& observation)
{
    if (!observation.speakerIsHuman || observation.speakerGuidCounter == 0 ||
        !PlayerbotSocialChannelIsValid(observation.key.channel) || observation.text.empty() ||
        IsOptedOut(observation.speakerGuidCounter))
        return false;

    observation.role = PlayerbotSocialPromptLineRole::HumanObservation;
    observation.replyToEventPublicId.clear();
    observation.sourceEventPublicId.clear();
    observation.eventPublicId = ReserveDeliveryEventPublicId(observation.speakerGuidCounter);
    return true;
}

void PlayerbotSocialMgr::RecordHumanObservation(PlayerbotSocialObservation const& observation,
                                                PlayerbotSocialThreadHandle const& thread)
{
    if (!observation.speakerIsHuman || !thread.valid || IsOptedOut(observation.speakerGuidCounter) ||
        !PlayerbotSocialPublicIdIsValid(PlayerbotSocialIdKind::Event, observation.eventPublicId))
        return;

    PlayerbotSocialEventDraft draft;
    draft.eventPublicId = observation.eventPublicId;
    draft.eventType = std::string(PLAYERBOT_SOCIAL_EVENT_TYPE_OBSERVATION);
    draft.origin = PlayerbotSocialEventOrigin::Social;
    draft.outcome = PlayerbotSocialEventOutcome::Recorded;
    draft.channel = observation.key.channel;
    draft.hasChannel = true;
    draft.threadPublicId = thread.publicId;
    draft.actorGuidCounter = observation.speakerGuidCounter;
    draft.zoneId = observation.zoneId;
    draft.messageText = observation.text;
    draft.occurredAtUnixSeconds = observation.atUnixSeconds;
    RecordEvent(std::move(draft));
}

void PlayerbotSocialMgr::RecordEvent(PlayerbotSocialEventDraft draft)
{
    if (draft.eventPublicId.empty())
        draft.eventSequence = _nextEventSequence++;

    _events.Push(draft);
}

void PlayerbotSocialMgr::FlushEvents()
{
    // New events remain in the bounded queue until the world thread applies the callback.
    if (_eventPersistence.InFlight())
        return;

    // Nothing waiting and nothing lost. Skipped rather than drained so an idle tick neither allocates
    // nor burns the sequence reserved for a gap marker that is not needed.
    if (_events.PendingCount() == 0 && _events.LostSinceLastDrain() == 0 && _eventPersistence.LostRows() == 0)
        return;

    uint64 const nowUnixSeconds = static_cast<uint64>(time(nullptr));
    std::vector<PlayerbotSocialEventBinding> drained;
    if (_events.PendingCount() > 0 || _events.LostSinceLastDrain() > 0)
    {
        uint64 const queueGapSequence = _events.LostSinceLastDrain() > 0 ? _nextEventSequence++ : 0;
        drained = _events.Drain(queueGapSequence, nowUnixSeconds);
    }

    uint64 const persistenceGapSequence = _eventPersistence.LostRows() > 0 ? _nextEventSequence++ : 0;
    if (!_eventPersistence.Prepare(drained, persistenceGapSequence, nowUnixSeconds))
        return;

    uint32 const retentionHours = sPlayerbotSocialConfig.socialChatTelemetryRetentionHours;
    PlayerbotsDatabaseTransaction transaction = PlayerbotsDatabase.BeginTransaction();

    for (PlayerbotSocialEventBinding const& binding : drained)
    {
        PlayerbotSocialEventRow const row = PlayerbotSocialBuildEventRow(binding, _actorIds, retentionHours);

        PlayerbotSocialPreparedStatement* statement = NewPlayerbotSocialStatement(PLAYERBOT_SOCIAL_STMT_INS_EVENT);
        statement->SetData(0, binding.publicId);

        // Every optional column is bound null rather than as an empty string or a zero. A row that
        // said actor zero or an empty thread would join to nothing while looking correlated.
        if (binding.threadPublicId.empty())
            statement->SetData(1, nullptr);
        else
            statement->SetData(1, binding.threadPublicId);

        if (binding.replyToEventPublicId.empty())
            statement->SetData(2, nullptr);
        else
            statement->SetData(2, binding.replyToEventPublicId);

        if (binding.sourceEventPublicId.empty())
            statement->SetData(3, nullptr);
        else
            statement->SetData(3, binding.sourceEventPublicId);

        statement->SetData(4, PLAYERBOT_SOCIAL_SCHEMA_VERSION);
        statement->SetData(5, binding.eventType);
        statement->SetData(6, binding.origin);

        if (binding.hasChannel)
            statement->SetData(7, binding.channel);
        else
            statement->SetData(7, nullptr);

        if (row.hasZone)
            statement->SetData(8, row.zoneId);
        else
            statement->SetData(8, nullptr);

        if (row.hasActor)
            statement->SetData(9, row.actorId);
        else
            statement->SetData(9, nullptr);

        if (row.hasTargetActor)
            statement->SetData(10, row.targetActorId);
        else
            statement->SetData(10, nullptr);

        if (row.hasBotActor)
            statement->SetData(11, row.botActorId);
        else
            statement->SetData(11, nullptr);

        statement->SetData(12, binding.outcome);

        if (binding.reason.empty())
            statement->SetData(13, nullptr);
        else
            statement->SetData(13, binding.reason);

        if (binding.messageText.empty())
            statement->SetData(14, nullptr);
        else
            statement->SetData(14, binding.messageText);

        if (binding.diagnosticsJson.empty())
            statement->SetData(15, nullptr);
        else
            statement->SetData(15, binding.diagnosticsJson);

        statement->SetData(16, binding.occurredAtUnixSeconds);
        statement->SetData(17, row.expiresAtUnixSeconds);

        transaction->Append(ConsumePlayerbotSocialSql(statement));
    }

    SocialTransactionProcessor()
        .AddCallback(PlayerbotsDatabase.AsyncCommitTransaction(transaction))
        .AfterComplete([this](bool success) { _eventPersistence.Complete(success); });
}

namespace
{
void AppendJsonString(std::string& out, std::string const& value)
{
    out += '"';
    for (char const character : value)
    {
        switch (character)
        {
            case '"':
                out += "\\\"";
                break;
            case '\\':
                out += "\\\\";
                break;
            case '\n':
                out += "\\n";
                break;
            case '\r':
                out += "\\r";
                break;
            case '\t':
                out += "\\t";
                break;
            default:
                // Every other control byte is escaped numerically rather than passed through.
                // MySQL rejects a raw control character inside a JSON string, so one reaching
                // the statement would fail the whole write rather than store an odd value.
                if (static_cast<unsigned char>(character) < 0x20)
                {
                    char escaped[7];
                    std::snprintf(escaped, sizeof(escaped), "\\u%04x",
                                  static_cast<unsigned>(static_cast<unsigned char>(character)));
                    out += escaped;
                }
                else
                {
                    out += character;
                }
                break;
        }
    }

    out += '"';
}

/*
 * The stored biography document.
 *
 * Written here rather than by the assembler, because this is the only place that cares what the
 * column looks like. Identity is included: the row is what a later load reads back, and a
 * biography whose identity had to be recovered from elsewhere would be a biography that could
 * disagree with itself after a rename.
 */
std::string SerializeBiographyDocument(PlayerbotBiography const& biography)
{
    std::string out = "{\"version\":";
    out += std::to_string(biography.version);

    auto const field = [&out](char const* name, std::string const& value)
    {
        out += ',';
        AppendJsonString(out, name);
        out += ':';
        AppendJsonString(out, value);
    };

    field("character_name", biography.identity.characterName);
    out += ",\"race_id\":" + std::to_string(biography.identity.raceId);
    out += ",\"class_id\":" + std::to_string(biography.identity.classId);
    out += ",\"gender_id\":" + std::to_string(biography.identity.genderId);
    field("origin", biography.origin);
    field("motivation", biography.motivation);
    field("formative_experience", biography.formativeExperience);
    field("interests", biography.interests);
    field("aversions", biography.aversions);
    field("preferred_topics", biography.preferredTopics);
    field("mannerisms", biography.mannerisms);
    field("values", biography.values);
    out += '}';
    return out;
}

std::string SerializeSocialTraits(PlayerbotSocialTraits const& traits)
{
    std::string out = "{\"warmth\":" + std::to_string(traits.warmth);
    out += ",\"talkativeness\":" + std::to_string(traits.talkativeness);
    out += ",\"curiosity\":" + std::to_string(traits.curiosity);
    out += ",\"humor\":" + std::to_string(traits.humor);
    out += ",\"formality\":" + std::to_string(traits.formality);
    out += ",\"last_evolved_at\":" + std::to_string(traits.lastEvolvedAtUnixSeconds);

    auto const array = [&out](char const* name, std::vector<std::string> const& values)
    {
        out += ',';
        AppendJsonString(out, name);
        out += ":[";
        for (std::size_t index = 0; index < values.size(); ++index)
        {
            if (index != 0)
                out += ',';
            AppendJsonString(out, values[index]);
        }
        out += ']';
    };

    array("interests", traits.interests);
    array("aversions", traits.aversions);
    out += '}';
    return out;
}
}  // namespace

void PlayerbotSocialMgr::FlushProfileWrites()
{
    if (_pendingProfileWrites.empty() && _pendingTraitsWrites.empty())
        return;

    std::vector<PlayerbotSocialProfileBinding> unresolved;

    for (PlayerbotSocialProfileBinding const& binding : _pendingProfileWrites)
    {
        /*
         * The profile table is keyed by actor id, not by character guid. A bot whose actor row has
         * not been resolved yet is RETAINED rather than dropped: the request that produced this
         * write already happened, and discarding the row would leave the database claiming a bot is
         * still Absent while this process believes a biography is in flight for it.
         *
         * Retaining is bounded, because a queued write is replaced per bot rather than appended.
         */
        auto const actor = _actorIds.find(binding.botGuidCounter);
        if (actor == _actorIds.end())
        {
            unresolved.push_back(binding);
            continue;
        }

        PlayerbotSocialPreparedStatement* statement = NewPlayerbotSocialStatement(PLAYERBOT_SOCIAL_STMT_INS_PROFILE);
        statement->SetData(0, actor->second);
        statement->SetData(1, binding.schemaVersion);
        statement->SetData(2, binding.traitsVersion);
        statement->SetData(3, binding.biographyState);
        statement->SetData(4, binding.biographyRequestToken);

        // Null rather than the epoch for a profile that has never been attempted. Zero would read
        // as 1970 and make every retry window look long expired.
        if (binding.biographyAttemptedAtUnixSeconds == 0)
            statement->SetData(5, nullptr);
        else
            statement->SetData(5, binding.biographyAttemptedAtUnixSeconds);

        // The document is written only for a state that has one. A pending or failed row carrying
        // a stale biography would be a biography nothing is entitled to read.
        if (binding.biography.origin.empty())
            statement->SetData(6, nullptr);
        else
            statement->SetData(6, SerializeBiographyDocument(binding.biography));

        // Same value as the state column, bound again for the generated-at expression. The
        // statement decides from it rather than from a second flag nobody else maintains.
        statement->SetData(7, binding.biographyState);
        PlayerbotSocialExecute(statement);
    }

    _pendingProfileWrites = std::move(unresolved);

    std::vector<PlayerbotSocialTraitsBinding> unresolvedTraits;
    for (PlayerbotSocialTraitsBinding const& binding : _pendingTraitsWrites)
    {
        auto const actor = _actorIds.find(binding.botGuidCounter);
        if (actor == _actorIds.end())
        {
            unresolvedTraits.push_back(binding);
            continue;
        }

        PlayerbotSocialPreparedStatement* statement =
            NewPlayerbotSocialStatement(PLAYERBOT_SOCIAL_STMT_UPD_PROFILE_TRAITS);
        statement->SetData(0, actor->second);
        statement->SetData(1, binding.schemaVersion);
        statement->SetData(2, binding.traits.version);
        statement->SetData(3, SerializeSocialTraits(binding.traits));
        PlayerbotSocialExecute(statement);
    }

    _pendingTraitsWrites = std::move(unresolvedTraits);
}

namespace
{
// The stored column order of PLAYERBOT_SOCIAL_STMT_SEL_RUNTIME_CONTROL's channel flags. Named so the
// load and the write agree by construction rather than by two hand written orderings that have to
// be compared to notice they disagree.
constexpr PlayerbotSocialChannel RUNTIME_CONTROL_CHANNEL_ORDER[] = {
    PlayerbotSocialChannel::General, PlayerbotSocialChannel::Say, PlayerbotSocialChannel::Party,
    PlayerbotSocialChannel::Whisper};

static_assert(std::size(RUNTIME_CONTROL_CHANNEL_ORDER) == PLAYERBOT_SOCIAL_CHANNEL_COUNT,
              "every channel needs a stored column");
}  // namespace

void PlayerbotSocialMgr::LoadRuntimeControl()
{
    /*
     * Seeded from the configuration first, so a deployment with no stored row runs exactly as it is
     * configured, and so a failed read leaves the configured behaviour rather than a struct of
     * defaults that might silence a channel nobody asked to silence.
     */
    _runtimeControl = PlayerbotSocialSeedRuntimeControl(PlayerbotSocialConfiguredGate());

    PlayerbotSocialPreparedStatement* statement =
        NewPlayerbotSocialStatement(PLAYERBOT_SOCIAL_STMT_SEL_RUNTIME_CONTROL);

    /*
     * Synchronous, unlike every other read in this manager. The controls have to be in force before
     * the first bot can speak: an asynchronous load would leave a window in which an operator's
     * pause is still in flight while the feature is already talking, which is the one window a pause
     * exists to close. One indexed single row read, once, during startup.
     */
    QueryResult result = PlayerbotSocialQuery(statement);
    if (!result)
    {
        // No row yet. The configured values stay in force and the first control an operator sends
        // creates the row, so nothing is written here for a deployment that never uses a control.
        LOG_INFO("playerbots", "Social runtime controls: no stored row, using the configured values.");
        _runtimeControlLoaded = true;
        return;
    }

    Field* fields = result->Fetch();

    _runtimeControl.paused = fields[0].Get<bool>();

    /*
     * Parsed leniently on purpose, and only here. The column is an ENUM so the database already
     * refused anything else, and a value this build does not recognise means the row was written by a
     * newer build: reading it as the middle profile is a better failure than refusing to start.
     */
    _runtimeControl.density = PlayerbotSocialParseDensityProfile(fields[1].Get<std::string>());

    for (std::size_t index = 0; index < PLAYERBOT_SOCIAL_CHANNEL_COUNT; ++index)
    {
        std::size_t const slot = static_cast<std::size_t>(RUNTIME_CONTROL_CHANNEL_ORDER[index]);
        _runtimeControl.channelEnabled[slot] = fields[2 + index].Get<bool>();
    }

    // The backstop survives a restart on purpose: a circuit the governor opened stays open until an
    // operator closes the row, and rebooting the server is not an operator closing the row.
    _runtimeControl.budgetCircuitOpen = fields[2 + PLAYERBOT_SOCIAL_CHANNEL_COUNT].Get<bool>();
    if (_runtimeControl.budgetCircuitOpen)
        LOG_WARN("playerbots",
                 "Social budget circuit is OPEN (reason={}); the feature stays silent until "
                 "an operator closes it.",
                 fields[3 + PLAYERBOT_SOCIAL_CHANNEL_COUNT].Get<std::string>());

    LOG_INFO("playerbots",
             "Social runtime controls loaded: paused={}, density={}, general={}, say={}, party={}, whisper={}.",
             _runtimeControl.paused, PlayerbotSocialDensityProfileName(_runtimeControl.density),
             _runtimeControl.channelEnabled[static_cast<std::size_t>(PlayerbotSocialChannel::General)],
             _runtimeControl.channelEnabled[static_cast<std::size_t>(PlayerbotSocialChannel::Say)],
             _runtimeControl.channelEnabled[static_cast<std::size_t>(PlayerbotSocialChannel::Party)],
             _runtimeControl.channelEnabled[static_cast<std::size_t>(PlayerbotSocialChannel::Whisper)]);

    _runtimeControlLoaded = true;
}

void PlayerbotSocialMgr::PersistRuntimeControl()
{
    PlayerbotSocialPreparedStatement* statement =
        NewPlayerbotSocialStatement(PLAYERBOT_SOCIAL_STMT_INS_RUNTIME_CONTROL);

    uint8 index = 0;
    statement->SetData(index++, _runtimeControl.paused);
    statement->SetData(index++, std::string(PlayerbotSocialDensityProfileName(_runtimeControl.density)));

    for (PlayerbotSocialChannel const channel : RUNTIME_CONTROL_CHANNEL_ORDER)
        statement->SetData(index++, _runtimeControl.channelEnabled[static_cast<std::size_t>(channel)]);

    /*
     * The budget circuit columns are bound to their closed defaults because this statement's INSERT
     * arm needs a value for every column it names, and a row being created here has no circuit open
     * by definition. The statement's ON DUPLICATE KEY UPDATE arm does NOT list them, so an existing
     * row keeps whatever the budget subsystem last wrote and a control can never close a circuit it
     * does not own.
     */
    statement->SetData(index++, false);

    /*
     * NULL, not an empty string and not the epoch. Both columns are nullable with no default, so NULL
     * is what "no circuit has ever opened" means there, and FROM_UNIXTIME(NULL) is NULL. Writing ''
     * and 1970-01-01 would make a row that has never had a circuit indistinguishable from one that
     * opened at the beginning of time, to anything that later reads these looking for a reason.
     */
    statement->SetData(index++, nullptr);
    statement->SetData(index++, nullptr);

    PlayerbotSocialExecute(statement);
}

namespace
{
/*
 * Whether a row with this opaque public id exists, asked synchronously.
 *
 * Deliberately synchronous, and deliberately only here. An operator deleting a memory or
 * acknowledging a case is entitled to be told whether it existed, and an asynchronous delete
 * cannot report that. The cost is one indexed single row lookup on a human triggered action,
 * bounded by the in flight control cap, on a path that runs at operator speed rather than at
 * tick speed.
 */
QueryResult SubjectRow(PlayerbotSocialStatementId query, std::string const& publicId)
{
    PlayerbotSocialPreparedStatement* statement = NewPlayerbotSocialStatement(query);
    statement->SetData(0, publicId);
    return PlayerbotSocialQuery(statement);
}

bool SubjectRowExists(PlayerbotSocialStatementId query, std::string const& publicId)
{
    return static_cast<bool>(SubjectRow(query, publicId));
}
}  // namespace

PlayerbotSocialControlOutcome PlayerbotSocialMgr::ApplyRuntimeControl(PlayerbotSocialControlRequest const& request)
{
    switch (request.operation)
    {
        case PlayerbotSocialControlOperation::Pause:
            _runtimeControl.paused = request.flag;
            PersistRuntimeControl();
            return PlayerbotSocialControlOutcome::Applied;

        case PlayerbotSocialControlOperation::Density:
            _runtimeControl.density = request.density;
            PersistRuntimeControl();
            return PlayerbotSocialControlOutcome::Applied;

        case PlayerbotSocialControlOperation::ChannelGeneral:
        case PlayerbotSocialControlOperation::ChannelSay:
        case PlayerbotSocialControlOperation::ChannelParty:
        case PlayerbotSocialControlOperation::ChannelWhisper:
        {
            PlayerbotSocialChannel channel = PlayerbotSocialChannel::General;
            if (!PlayerbotSocialControlChannelFor(request.operation, channel))
            {
                // Unreachable while the four cases above and the mapping agree. Reported rather than
                // defaulted to a channel, because silently toggling the wrong surface is worse than
                // refusing, and the caller may retry a refusal safely.
                LOG_ERROR("playerbots", "Social control: operation {} has no channel.",
                          PlayerbotSocialControlOperationName(request.operation));
                return PlayerbotSocialControlOutcome::InternalError;
            }

            _runtimeControl.channelEnabled[static_cast<std::size_t>(channel)] = request.flag;
            PersistRuntimeControl();
            return PlayerbotSocialControlOutcome::Applied;
        }

        case PlayerbotSocialControlOperation::ResetMemory:
        {
            QueryResult const row = SubjectRow(PLAYERBOT_SOCIAL_STMT_SEL_MEMORY_OWNER_BY_PUBLIC_ID, request.subject);
            if (!row)
                return PlayerbotSocialControlOutcome::SubjectNotFound;

            uint64 const ownerGuidCounter = ActorGuidFor(row->Fetch()[0].Get<uint32>());

            PlayerbotSocialPreparedStatement* deletion =
                NewPlayerbotSocialStatement(PLAYERBOT_SOCIAL_STMT_DEL_MEMORY_BY_PUBLIC_ID);
            deletion->SetData(0, request.subject);
            PlayerbotSocialExecute(deletion);

            /*
             * The owning bot's cached memories go with the row, not just the stamp that would have
             * caused them to be re-read. Dropping the stamp alone leaves the deleted memory in the
             * snapshot until something asks for it again, and a re-read that comes back empty is
             * not applied at all, so a bot whose last memory an operator deleted would quote it
             * indefinitely. Every scope is dropped because a memory id does not say which one it
             * was stored at.
             *
             * What is dropped is a cache of rows that were WRITTEN to MySQL, which is not quite the
             * same as rows MySQL holds: every social write is enqueued one way and reports no
             * result, so a statement the server rejects leaves the cache ahead of the table with
             * nothing here able to observe it. That is a standing property of this feature rather
             * than something this reset introduces, and the drop moves toward the durable answer
             * rather than away from it: a memory MySQL never accepted is one no reload would have
             * returned anyway.
             */
            if (ownerGuidCounter != 0)
                _state.ReplaceMemoriesOwnedBy(ownerGuidCounter, PlayerbotSocialMemoryScopeQuery::Any, {},
                                              [](PlayerbotSocialMemoryRecord const&) { return true; });

            /*
             * The epoch moves too, and dropping the stamps alone is not enough without it. A read
             * issued before this deletion is still in flight and its answer predates an erasure it
             * could not have seen, so it would put the deleted memory back and stamp the result
             * fresh. An operator's "forget this" has to survive a read that was already in the air.
             */
            ++_stateEpoch;
            _memorySnapshotAt.clear();
            return PlayerbotSocialControlOutcome::Applied;
        }

        case PlayerbotSocialControlOperation::ResetRelationship:
        {
            if (!SubjectRowExists(PLAYERBOT_SOCIAL_STMT_SEL_RELATIONSHIP_EXISTS_BY_PUBLIC_ID, request.subject))
                return PlayerbotSocialControlOutcome::SubjectNotFound;

            PlayerbotSocialPreparedStatement* deletion =
                NewPlayerbotSocialStatement(PLAYERBOT_SOCIAL_STMT_DEL_RELATIONSHIP_BY_PUBLIC_ID);
            deletion->SetData(0, request.subject);
            PlayerbotSocialExecute(deletion);

            /*
             * The cached pair goes with the row, for the reason the memory reset drops its snapshot:
             * a re-read that finds nothing is not applied, so dropping the stamp alone would leave
             * the deleted relationship live for the rest of the uptime rather than for a snapshot
             * lifetime. Only the named direction is erased, because that is what a row is.
             *
             * Found by recomputing the public id of each cached pair rather than by resolving the
             * row's actor ids. The id is a hash and cannot be inverted, and resolving is not an
             * alternative: a cohort purge drops the deleted bot's actor mapping while keeping what
             * surviving bots knew about it, so the survivor's pair would be exactly the one left
             * behind. One hash per cached pair, on a path that runs at operator speed and stops at
             * the first match.
             */
            _state.ForgetRelationshipPairMatching([&request](PlayerbotSocialRelationshipKey const& pair)
                                                  { return MakeRelationshipPublicId(pair) == request.subject; });

            /*
             * Both halves, for the reason the memory reset needs both. The stamps are keyed by pair
             * and this request names a row by public id, so there is no way to drop just the one:
             * they are a cache and rebuilding them is the correct cost of a deletion. The epoch
             * voids the read that was already in flight when the row went away.
             */
            ++_stateEpoch;
            _relationshipSnapshotAt.clear();
            return PlayerbotSocialControlOutcome::Applied;
        }

        case PlayerbotSocialControlOperation::AcknowledgeCase:
        {
            if (!SubjectRowExists(PLAYERBOT_SOCIAL_STMT_SEL_MODERATION_CASE_EXISTS_BY_PUBLIC_ID, request.subject))
                return PlayerbotSocialControlOutcome::SubjectNotFound;

            PlayerbotSocialPreparedStatement* acknowledgement =
                NewPlayerbotSocialStatement(PLAYERBOT_SOCIAL_STMT_UPD_MODERATION_CASE_ACK);
            acknowledgement->SetData(0, static_cast<uint64>(time(nullptr)));

            /*
             * Recorded as the control surface rather than as a named person. The command port
             * authenticates a shared token, not an operator, so naming one here would be inventing an
             * identity the server never established.
             */
            acknowledgement->SetData(1, std::string("social_control"));
            acknowledgement->SetData(2, request.subject);
            PlayerbotSocialExecute(acknowledgement);
            return PlayerbotSocialControlOutcome::Applied;
        }
    }

    // An operation added to the enum without a case here. Refused rather than fallen through, so a
    // control can never report success for something nothing applied.
    LOG_ERROR("playerbots", "Social control: unhandled operation {}.", static_cast<uint32>(request.operation));
    return PlayerbotSocialControlOutcome::InternalError;
}

void PlayerbotSocialMgr::UpdateDatabaseWork(uint32 diff)
{
    SocialQueryProcessor().ProcessReadyCallbacks();
    SocialTransactionProcessor().ProcessReadyCallbacks();

    // Every tick, like the event queue and for the same reason: a write that has already been
    // decided should not wait on an interval before it is durable.
    FlushProfileWrites();

    /*
     * Every tick, ahead of the interval driven work below. The queue is what absorbs a burst, so
     * draining it promptly is what keeps the bound from being reached in the first place.
     */
    FlushEvents();

    /*
     * Assistance encounters are completed by elapsed idle time rather than by a combat exit hook.
     * A fight can stop without any event announcing it: the participants log out, the mob despawns,
     * the zone unloads. Sweeping means an encounter always completes eventually, which is both what
     * applies the credit and what stops the open encounter map from growing without bound.
     *
     * On its own interval rather than on every update. The idle threshold is a whole minute, so the
     * answer can only change on two ticks out of the thousands between them, and the sweep walks
     * every open encounter to find out. The interval is far below the threshold, so the delay it adds
     * to a completion is bounded and small.
     */
    _millisecondsSinceEncounterSweep += diff;
    if (_millisecondsSinceEncounterSweep >= PLAYERBOT_SOCIAL_ENCOUNTER_SWEEP_INTERVAL_MS)
    {
        _millisecondsSinceEncounterSweep = 0;
        CompleteStaleEncounters(static_cast<uint64>(GameTime::GetGameTime().count()));
    }

    /*
     * Raw event retention. Every event carries the expiry it was written with, computed by
     * PlayerbotSocialEventExpiry from the configured window and never shorter than
     * PLAYERBOT_SOCIAL_MIN_RETENTION_HOURS, so the purge only has to honour what is already stored
     * rather than recompute the policy here.
     *
     * Nothing else is touched. Moderation cases in particular are never removed by retention: they
     * are the sanitized record that outlives the raw text they were derived from.
     */
    _millisecondsSincePurge += diff;
    if (_millisecondsSincePurge < PLAYERBOT_SOCIAL_PURGE_INTERVAL_MS)
        return;

    _millisecondsSincePurge = 0;

    PlayerbotSocialPreparedStatement* purge = NewPlayerbotSocialStatement(PLAYERBOT_SOCIAL_STMT_DEL_EVENT_EXPIRED);
    purge->SetData(0, static_cast<uint64>(time(nullptr)));
    purge->SetData(1, PLAYERBOT_SOCIAL_PURGE_BATCH);
    PlayerbotSocialExecute(purge);
}

// Assistance encounters ----------------------------------------------------------------------------

namespace
{
/*
 * Makes room for one new pair when a combat map is already at its ceiling.
 *
 * Evicts the entry whose timestamp is oldest, which is the one closest to expiring on its own,
 * so the cap costs the least information it can. Deterministic on a tie: `std::map` iterates in
 * key order and the comparison is strict, so the lowest key among equal timestamps is the one
 * that goes.
 *
 * Called before an insert rather than after, so the map never momentarily exceeds the ceiling.
 */
template <class Map, class Stamp>
void EvictOldestTrackedPair(Map& entries, Stamp stampOf)
{
    if (entries.size() < PLAYERBOT_SOCIAL_MAX_TRACKED_PAIRS)
        return;

    auto oldest = entries.begin();
    for (auto entry = entries.begin(); entry != entries.end(); ++entry)
    {
        if (stampOf(entry->second) < stampOf(oldest->second))
            oldest = entry;
    }

    entries.erase(oldest);
}

// A pair is only an encounter if both ends are named and they are not the same character.
bool AssistancePairIsUsable(uint64 beneficiaryGuidCounter, uint64 helperGuidCounter)
{
    return beneficiaryGuidCounter != 0 && helperGuidCounter != 0 && beneficiaryGuidCounter != helperGuidCounter;
}
}  // namespace

void PlayerbotSocialMgr::RecordAssistanceHealing(uint64 beneficiaryGuidCounter, uint64 helperGuidCounter,
                                                 uint32 effectiveHealing, uint32 beneficiaryMaxHealth,
                                                 uint32 beneficiaryHealthBeforeHeal, uint64 nowUnixSeconds)
{
    if (!AssistancePairIsUsable(beneficiaryGuidCounter, helperGuidCounter))
        return;

    // Overheal arrives here as zero because OnHeal reports health actually restored. Nothing was
    // done, so no encounter opens and no empty tally is left behind to be completed later.
    if (effectiveHealing == 0)
        return;

    PlayerbotSocialRelationshipKey const key{beneficiaryGuidCounter, helperGuidCounter};

    // Capped before the insert, so the map never momentarily exceeds its ceiling.
    if (_openEncounters.find(key) == _openEncounters.end())
        EvictOldestTrackedPair(_openEncounters,
                               [](PlayerbotSocialAssistanceTally const& open) { return open.lastEventAtUnixSeconds; });

    PlayerbotSocialAssistanceTally& tally = _openEncounters[key];
    tally.effectiveHealing = PlayerbotSocialSaturatingAdd(tally.effectiveHealing, effectiveHealing);
    tally.contributionEvents = PlayerbotSocialSaturatingIncrement(tally.contributionEvents);
    tally.beneficiaryMaxHealth = beneficiaryMaxHealth;
    tally.lastEventAtUnixSeconds = nowUnixSeconds;

    if (PlayerbotSocialHealIsRescue(beneficiaryHealthBeforeHeal, beneficiaryMaxHealth))
        tally.rescueCount = PlayerbotSocialSaturatingIncrement(tally.rescueCount);
}

void PlayerbotSocialMgr::RecordAssistanceDamage(uint64 beneficiaryGuidCounter, uint64 helperGuidCounter, uint32 damage,
                                                bool victimEngagedWithBeneficiary, uint64 nowUnixSeconds)
{
    if (!AssistancePairIsUsable(beneficiaryGuidCounter, helperGuidCounter))
        return;

    // Splash onto something nobody was fighting is incidental, however large it was.
    if (!PlayerbotSocialDamageIsMeaningful(victimEngagedWithBeneficiary, damage))
        return;

    PlayerbotSocialRelationshipKey const key{beneficiaryGuidCounter, helperGuidCounter};

    if (_openEncounters.find(key) == _openEncounters.end())
        EvictOldestTrackedPair(_openEncounters,
                               [](PlayerbotSocialAssistanceTally const& open) { return open.lastEventAtUnixSeconds; });

    PlayerbotSocialAssistanceTally& tally = _openEncounters[key];
    tally.meaningfulDamage = PlayerbotSocialSaturatingAdd(tally.meaningfulDamage, damage);
    tally.contributionEvents = PlayerbotSocialSaturatingIncrement(tally.contributionEvents);
    tally.lastEventAtUnixSeconds = nowUnixSeconds;
}

bool PlayerbotSocialMgr::EncounterIsOpen(uint64 beneficiaryGuidCounter, uint64 helperGuidCounter) const
{
    PlayerbotSocialRelationshipKey const key{beneficiaryGuidCounter, helperGuidCounter};
    return _openEncounters.find(key) != _openEncounters.end();
}

PlayerbotSocialRelationshipValues PlayerbotSocialMgr::CompleteEncounter(uint64 beneficiaryGuidCounter,
                                                                        uint64 helperGuidCounter)
{
    PlayerbotSocialRelationshipKey const key{beneficiaryGuidCounter, helperGuidCounter};

    auto const open = _openEncounters.find(key);
    if (open == _openEncounters.end())
        return PlayerbotSocialRelationshipValues();

    PlayerbotSocialRelationshipValues const delta =
        PlayerbotSocialAssistanceDelta(open->second, open->second.beneficiaryMaxHealth);

    // Dropped whether or not it earned anything. An encounter is finished once, and leaving the
    // tally would let the next fight inherit this one's credit.
    _openEncounters.erase(open);

    return delta;
}

PlayerbotSocialEncounterSweepResult PlayerbotSocialMgr::CompleteStaleEncounters(uint64 nowUnixSeconds)
{
    std::vector<PlayerbotSocialRelationshipKey> finished;

    for (auto const& [key, tally] : _openEncounters)
    {
        // ElapsedSeconds reads a backwards clock as zero rather than as an enormous interval, so a
        // clock correction cannot complete every live encounter at once.
        if (ElapsedSeconds(nowUnixSeconds, tally.lastEventAtUnixSeconds) >= PLAYERBOT_SOCIAL_ENCOUNTER_IDLE_SECONDS)
        {
            finished.push_back(key);
        }
    }

    PlayerbotSocialEncounterSweepResult result;
    result.completed = finished.size();

    for (PlayerbotSocialRelationshipKey const& key : finished)
    {
        PlayerbotSocialRelationshipValues const delta = CompleteEncounter(key.botGuidCounter, key.subjectGuidCounter);

        PlayerbotSocialRelationshipValues const applied =
            ApplyRelationshipDelta(key.botGuidCounter, key.subjectGuidCounter, delta, nowUnixSeconds);

        result.applied.familiarity += applied.familiarity;
        result.applied.affinity += applied.affinity;
        result.applied.trust += applied.trust;

        /*
         * Recorded on what the encounter EARNED, not on what was admitted.
         *
         * A window ceiling or an unresolved actor can refuse the write while the help still
         * happened, and the feed is about the help. Gating on `applied` instead would make a capped
         * encounter vanish entirely, which is the reading that hides the ceiling rather than showing
         * it. An encounter that earned nothing is not assistance and is deliberately not recorded:
         * it would put a row in the feed saying a bot did not help anyone.
         */
        if (delta.familiarity == 0.0f && delta.affinity == 0.0f && delta.trust == 0.0f)
            continue;

        PlayerbotSocialAssistanceCompletion completion;
        completion.beneficiaryGuidCounter = key.botGuidCounter;
        completion.helperGuidCounter = key.subjectGuidCounter;
        completion.earned = delta;
        completion.applied = applied;
        completion.occurredAtUnixSeconds = nowUnixSeconds;
        RecordEvent(PlayerbotSocialMakeAssistanceEvent(completion));
    }

    /*
     * Prune spent ledgers. An entry whose window has fully elapsed answers exactly the same as an
     * absent one, so keeping it would mean holding a row per pair that ever helped, for the rest of
     * the uptime. Done here rather than on a timer of its own because this is already the pass that
     * walks encounter state.
     */
    for (auto entry = _assistanceCredit.begin(); entry != _assistanceCredit.end();)
    {
        if (PlayerbotSocialAssistanceCreditIsExpired(entry->second, nowUnixSeconds))
            entry = _assistanceCredit.erase(entry);
        else
            ++entry;
    }

    /*
     * Opposition markers age out on the encounter idle threshold, which is the same thing that
     * decides the fight they belong to has ended. An attacker left with no victims goes too, so the
     * outer map does not accumulate empty entries that hold slots against the attacker ceiling.
     */
    for (auto attacker = _appliedOpposition.begin(); attacker != _appliedOpposition.end();)
    {
        std::map<uint64, uint64>& victims = attacker->second;
        for (auto marker = victims.begin(); marker != victims.end();)
        {
            if (ElapsedSeconds(nowUnixSeconds, marker->second) >= PLAYERBOT_SOCIAL_ENCOUNTER_IDLE_SECONDS)
                marker = victims.erase(marker);
            else
                ++marker;
        }

        attacker = victims.empty() ? _appliedOpposition.erase(attacker) : std::next(attacker);
    }

    return result;
}

bool PlayerbotSocialMgr::AdmitProviderCall(uint64 nowUnixSeconds, bool continuation)
{
    switch (PlayerbotSocialGovernProviderCall(_providerBudget, nowUnixSeconds,
                                              sPlayerbotSocialConfig.socialChatProviderHourlyBudget, continuation,
                                              _runtimeControl.budgetCircuitOpen))
    {
        case PlayerbotSocialBudgetDecision::Admitted:
            return true;
        case PlayerbotSocialBudgetDecision::Refused:
            return false;
        case PlayerbotSocialBudgetDecision::RefusedCircuitTrip:
            OpenBudgetCircuit("sustained_overrun", nowUnixSeconds);
            return false;
    }

    return false;
}

void PlayerbotSocialMgr::OpenBudgetCircuit(std::string_view reason, uint64 nowUnixSeconds)
{
    if (_runtimeControl.budgetCircuitOpen)
        return;

    _runtimeControl.budgetCircuitOpen = true;

    LOG_WARN("playerbots",
             "Social budget circuit OPENED (reason={}): provider demand overran the configured hourly budget of "
             "{}. The feature is silent until an operator clears budget_circuit_open in "
             "playerbot_social_runtime_control.",
             reason, sPlayerbotSocialConfig.socialChatProviderHourlyBudget);

    PlayerbotSocialPreparedStatement* statement = NewPlayerbotSocialStatement(PLAYERBOT_SOCIAL_STMT_UPD_BUDGET_CIRCUIT);
    uint8 index = 0;
    statement->SetData(index++, _runtimeControl.paused);
    statement->SetData(index++, std::string(PlayerbotSocialDensityProfileName(_runtimeControl.density)));
    for (PlayerbotSocialChannel const channel : RUNTIME_CONTROL_CHANNEL_ORDER)
        statement->SetData(index++, _runtimeControl.channelEnabled[static_cast<std::size_t>(channel)]);
    statement->SetData(index++, true);
    statement->SetData(index++, std::string(reason));
    statement->SetData(index++, nowUnixSeconds);
    PlayerbotSocialExecute(statement);

    PlayerbotSocialEventDraft draft;
    draft.eventType = "social.budget";
    draft.origin = PlayerbotSocialEventOrigin::System;
    draft.outcome = PlayerbotSocialEventOutcome::Failed;
    draft.reason = std::string(reason);
    draft.occurredAtUnixSeconds = nowUnixSeconds;
    draft.priority = PlayerbotSocialEventPriority::Critical;
    RecordEvent(std::move(draft));
}

void PlayerbotSocialMgr::NoteHostileLine(uint64 subjectGuidCounter, uint64 speakerGuidCounter,
                                         PlayerbotSocialModerationCategory category, std::string const& text,
                                         uint64 nowUnixSeconds)
{
    if (subjectGuidCounter == 0)
        return;

    auto const key = std::pair{subjectGuidCounter, category};

    // Evicting, like the whisper stamps: a lost tally costs a campaign a fresh count, never a
    // durable record that already exists.
    if (_moderationTallies.find(key) == _moderationTallies.end() &&
        _moderationTallies.size() >= PLAYERBOT_SOCIAL_MAX_TRACKED_PAIRS)
    {
        auto oldest = _moderationTallies.begin();
        for (auto it = _moderationTallies.begin(); it != _moderationTallies.end(); ++it)
            if (it->second.lastAtUnixSeconds < oldest->second.lastAtUnixSeconds)
                oldest = it;
        _moderationTallies.erase(oldest);
    }

    PlayerbotSocialModerationTally& tally = _moderationTallies[key];
    if (!PlayerbotSocialNoteHostileOccurrence(tally, category, nowUnixSeconds))
        return;

    // Durable only once the subject resolves to an actor row; a tally for a not-yet-registered
    // character keeps counting in memory and lands on the first occurrence after registration.
    auto const subject = _actorIds.find(subjectGuidCounter);
    if (subject == _actorIds.end())
        return;

    std::optional<uint32> speakerActorId;
    if (auto const speaker = _actorIds.find(speakerGuidCounter); speaker != _actorIds.end())
        speakerActorId = speaker->second;

    PlayerbotSocialModerationCaseBinding const binding =
        PlayerbotSocialBuildModerationCaseBinding(subject->second, category, tally, speakerActorId, text);

    PlayerbotSocialPreparedStatement* statement =
        NewPlayerbotSocialStatement(PLAYERBOT_SOCIAL_STMT_INS_MODERATION_CASE);
    statement->SetData(0, MakeModerationCasePublicId(subjectGuidCounter, category));
    statement->SetData(1, binding.subjectActorId);
    statement->SetData(2, binding.category);
    statement->SetData(3, binding.occurrenceContribution);
    statement->SetData(4, binding.firstOccurredAtUnixSeconds);
    statement->SetData(5, binding.lastOccurredAtUnixSeconds);
    statement->SetData(6, binding.evidenceJson);
    PlayerbotSocialExecute(statement);

    LOG_WARN("playerbots", "Social moderation case opened or bumped: subject bot {} category {} occurrences {}.",
             subjectGuidCounter, PlayerbotSocialModerationCategoryName(category), tally.occurrences);

    PlayerbotSocialEventDraft draft;
    draft.eventType = "social.moderation";
    draft.origin = PlayerbotSocialEventOrigin::System;
    draft.outcome = PlayerbotSocialEventOutcome::Recorded;
    draft.reason = PlayerbotSocialModerationCategoryName(category);
    draft.botGuidCounter = subjectGuidCounter;
    draft.actorGuidCounter = speakerGuidCounter;
    draft.occurredAtUnixSeconds = nowUnixSeconds;
    draft.priority = PlayerbotSocialEventPriority::Critical;
    RecordEvent(std::move(draft));
}

bool PlayerbotSocialMgr::NoteWhisperStarterAttempt(PlayerbotSocialRelationshipKey const& key, uint64 nowUnixSeconds,
                                                   uint64 cooldownSeconds)
{
    if (key.botGuidCounter == 0 || key.subjectGuidCounter == 0)
        return false;

    auto const stamped = _whisperStarterAttempts.find(key);
    if (stamped != _whisperStarterAttempts.end() && ElapsedSeconds(nowUnixSeconds, stamped->second) < cooldownSeconds)
        return false;

    /*
     * Evicts, unlike the assistance ledger, because a stamp here is not a bound anyone could farm:
     * losing one permits at most a single early whisper for that pair. The oldest stamp is the one
     * most likely to have expired anyway.
     */
    if (stamped == _whisperStarterAttempts.end() &&
        _whisperStarterAttempts.size() >= PLAYERBOT_SOCIAL_MAX_TRACKED_PAIRS)
    {
        auto oldest = _whisperStarterAttempts.begin();
        for (auto it = _whisperStarterAttempts.begin(); it != _whisperStarterAttempts.end(); ++it)
            if (it->second < oldest->second)
                oldest = it;
        _whisperStarterAttempts.erase(oldest);
    }

    _whisperStarterAttempts[key] = nowUnixSeconds;

    return true;
}

void PlayerbotSocialMgr::ClearWhisperStarterAttempt(PlayerbotSocialRelationshipKey const& key)
{
    _whisperStarterAttempts.erase(key);
}

PlayerbotSocialRelationshipValues PlayerbotSocialMgr::ApplyRelationshipDelta(
    uint64 botGuidCounter, uint64 subjectGuidCounter, PlayerbotSocialRelationshipValues const& delta,
    uint64 nowUnixSeconds)
{
    PlayerbotSocialRelationshipValues const nothing;

    if (!AssistancePairIsUsable(botGuidCounter, subjectGuidCounter))
        return nothing;

    // An encounter that earned nothing still completed. Returning before the ledger matters: a zero
    // delta must not open a window, because opening one would start the clock on a ceiling nobody
    // has spent anything against.
    if (delta.familiarity == 0.0f && delta.affinity == 0.0f && delta.trust == 0.0f)
        return nothing;

    PlayerbotSocialRelationshipKey const key{botGuidCounter, subjectGuidCounter};

    if (!PairMayBeStored(botGuidCounter, subjectGuidCounter))
        return nothing;

    /*
     * The window ceiling is consulted before the write, and the write is attempted even when the
     * actor ids are not resolved yet, in which case the credit is spent without being stored.
     *
     * That forfeits a little credit rather than deferring it, and it is the safe direction of the
     * two: the alternative is to admit only on a successful write, which would let a pair retry
     * until one lands and collect the ceiling more than once. Losing a fraction of one window's
     * credit makes a bot slightly less friendly; the other error inflates a relationship, which is
     * the thing Definition of Done 2 exists to prevent.
     */
    /*
     * The ledger is refused when full, never evicted.
     *
     * A ledger entry is what BOUNDS a pair, not what credits it. Evicting one to make room hands
     * that pair a fresh ceiling, so anyone able to churn PLAYERBOT_SOCIAL_MAX_TRACKED_PAIRS other
     * pairs could evict their own entry on demand and collect a new ceiling as often as they liked,
     * which is the exact farm Definition of Done 2 forbids. The transient encounter and opposition
     * maps do evict, because losing an entry there costs at most one unpaid encounter rather than
     * removing a bound.
     *
     * Saturation is self healing: the sweep drops every elapsed window, so slots come back within
     * the hour. Until then a new pair can still be penalised but cannot gain.
     */
    auto ledger = _assistanceCredit.find(key);
    if (ledger == _assistanceCredit.end() && _assistanceCredit.size() >= PLAYERBOT_SOCIAL_MAX_TRACKED_PAIRS)
    {
        PlayerbotSocialRelationshipValues const unledgered = PlayerbotSocialAdmitWithoutLedger(delta);
        if (unledgered.familiarity == 0.0f && unledgered.affinity == 0.0f && unledgered.trust == 0.0f)
            return nothing;

        PersistRelationshipDelta(botGuidCounter, subjectGuidCounter, unledgered, nowUnixSeconds);
        return unledgered;
    }

    if (ledger == _assistanceCredit.end())
        ledger = _assistanceCredit.emplace(key, PlayerbotSocialAssistanceCredit()).first;

    PlayerbotSocialRelationshipValues const admitted =
        PlayerbotSocialAdmitAssistanceCredit(ledger->second, delta, nowUnixSeconds);

    if (admitted.familiarity == 0.0f && admitted.affinity == 0.0f && admitted.trust == 0.0f)
        return nothing;

    PersistRelationshipDelta(botGuidCounter, subjectGuidCounter, admitted, nowUnixSeconds);

    return admitted;
}

bool PlayerbotSocialMgr::PersistRelationshipDelta(uint64 botGuidCounter, uint64 subjectGuidCounter,
                                                  PlayerbotSocialRelationshipValues const& delta, uint64 nowUnixSeconds)
{
    PlayerbotSocialRelationshipKey const key{botGuidCounter, subjectGuidCounter};
    if (key.botGuidCounter == 0 || key.subjectGuidCounter == 0)
        return false;

    if (!PairMayBeStored(key.botGuidCounter, key.subjectGuidCounter))
        return false;

    auto const bot = _actorIds.find(key.botGuidCounter);
    auto const subject = _actorIds.find(key.subjectGuidCounter);
    if (bot == _actorIds.end() || subject == _actorIds.end())
        return false;

    /*
     * The in-memory store is updated best effort and the database is authoritative.
     *
     * This process never loads a stored relationship, so its snapshot starts neutral and stays
     * whatever this uptime happened to write. Adding the delta to it keeps a running conversation
     * consistent with what just happened, while the row itself accumulates correctly regardless.
     * RememberRelationship is also the store's own consent check, which is why its refusal stops the
     * write rather than only skipping the cache update.
     */
    /*
     * The delta itself has to satisfy the column constraints, not only the total it produces.
     *
     * The statement adds on a duplicate key, but on a pair's first write there is no row to add to
     * and the delta becomes the inserted value. MySQL evaluates the CHECK constraints against that
     * inserted row BEFORE it detects the duplicate, so a delta outside a column's range fails the
     * whole statement even when the addition would have landed comfortably in range. The write is
     * asynchronous and its result is never read, so that failure would be silent.
     *
     * Refused and logged rather than clamped. Every delta this module produces is a few hundredths,
     * so reaching here means a caller computed something it did not intend, and quietly reshaping it
     * into a legal value would store that mistake instead of reporting it. Familiarity in particular
     * cannot go below zero at all, so a negative familiarity delta is inexpressible here rather than
     * merely large.
     *
     * Checked BEFORE the cache below is touched. Validating afterwards would leave the snapshot
     * holding a change the database was then refused, which is the one way these two can disagree
     * about something neither of them should have accepted.
     */
    if (!PlayerbotSocialRelationshipIsInRange(delta))
    {
        LOG_ERROR("playerbots",
                  "Social relationship delta for bot {} toward {} is outside the storable range "
                  "(familiarity {}, affinity {}, trust {}) and was not written.",
                  botGuidCounter, subjectGuidCounter, delta.familiarity, delta.affinity, delta.trust);
        return false;
    }

    PlayerbotSocialRelationshipValues cached = _state.RecallRelationship(key);
    cached.familiarity += delta.familiarity;
    cached.affinity += delta.affinity;
    cached.trust += delta.trust;

    if (!_state.RememberRelationship(key, cached))
        return false;

    /*
     * The clamp of the TOTAL lives in the statement. Only the database knows the stored value, so
     * only it can hold the sum inside the range without this process reading the row first.
     */
    PlayerbotSocialPreparedStatement* statement =
        NewPlayerbotSocialStatement(PLAYERBOT_SOCIAL_STMT_INS_RELATIONSHIP_DELTA);
    statement->SetData(0, MakeRelationshipPublicId(key));
    statement->SetData(1, bot->second);
    statement->SetData(2, subject->second);
    statement->SetData(3, delta.familiarity);
    statement->SetData(4, delta.affinity);
    statement->SetData(5, delta.trust);
    statement->SetData(6, static_cast<uint32>(1));
    statement->SetData(7, nowUnixSeconds);
    PlayerbotSocialExecute(statement);

    /*
     * A read for this pair issued before this delta would land afterwards holding the pre delta
     * row, overwrite the cache this function just advanced, and stamp that older view fresh. The
     * database row is correct either way; what would be wrong for a snapshot lifetime is what the
     * bot believes.
     */
    InvalidateRelationshipRead(key);

    return true;
}

PlayerbotSocialRelationshipValues PlayerbotSocialMgr::RecordPvpOpposition(uint64 victimGuidCounter,
                                                                          uint64 opponentGuidCounter,
                                                                          PlayerbotSocialCombatContext context,
                                                                          uint64 nowUnixSeconds)
{
    if (!AssistancePairIsUsable(victimGuidCounter, opponentGuidCounter))
        return PlayerbotSocialRelationshipValues();

    // Consented opposition and an unrecognized context both land here. Neither says anything about
    // the opponent, so neither produces a delta to write.
    //
    // SHORTCUT: opposition is one flat answer per encounter, with no notion of who struck first,
    // whether the victim was flagged, or how lopsided the levels were. Upgrade trigger: when the
    // Social telemetry page shows open world aggression deltas landing on characters who started
    // the fight themselves, carry the initiator through to here and gate on it.
    if (!PlayerbotSocialCombatContextAppliesHostility(context))
        return PlayerbotSocialRelationshipValues();

    /*
     * One answer per fight, not per swing.
     *
     * The damage hook fires on every hit, and a fight is dozens of them. Returning the delta each
     * time would apply the full aggression penalty forty times over and drive affinity to its floor
     * on the first gank, which is not what "they attacked me" is worth. The floor clamp in the
     * database would hide it rather than prevent it, because the value would already be there.
     *
     * Idle time separates one fight from the next, using the same threshold and the same backwards
     * clock handling as an assistance encounter, so someone who returns later to do it again is
     * opposed again.
     */
    /*
     * Eligibility is checked BEFORE any capacity is spent.
     *
     * This runs from the damage hook, which sees every player versus player hit on the realm, and
     * most of those pairs can never be written: consent unread, a reset pending, one side offline.
     * Allocating a marker for them would let ordinary traffic fill the bound, and would let an
     * attacker deliberately pre-saturate it with unrelated pairs and then attack with no marker
     * available to record it. A pair that cannot be stored gets no marker and no delta.
     */
    if (!PairMayBeStored(victimGuidCounter, opponentGuidCounter))
        return PlayerbotSocialRelationshipValues();

    auto attacker = _appliedOpposition.find(opponentGuidCounter);

    /*
     * Refused when full, never evicted, because this marker is a BOUND rather than work. Evicting one
     * lets the same attacker be opposed again during the same fight, which is exactly the once per
     * swing behaviour this map exists to prevent. So if the fight cannot be remembered, the delta
     * that memory would bound is not applied either: an unrecordable fight costs nothing rather than
     * costing every swing.
     *
     * Both caps are per attacker rather than shared, so no single attacker can exhaust the structure
     * and leave everyone else unattributed. Self healing, because the sweep drops markers once their
     * fight goes idle.
     */
    if (attacker == _appliedOpposition.end())
    {
        if (_appliedOpposition.size() >= PLAYERBOT_SOCIAL_MAX_OPPOSITION_ATTACKERS)
            return PlayerbotSocialRelationshipValues();

        attacker = _appliedOpposition.emplace(opponentGuidCounter, std::map<uint64, uint64>()).first;
    }

    std::map<uint64, uint64>& victims = attacker->second;

    auto const opposed = victims.find(victimGuidCounter);
    bool const sameFight =
        opposed != victims.end() && PlayerbotSocialOppositionIsSameFight(opposed->second, nowUnixSeconds);

    if (opposed != victims.end())
    {
        // Refreshed whether or not it pays, so a continuing fight keeps counting as one fight, and
        // never moved backwards.
        opposed->second = PlayerbotSocialAdvanceOppositionMarker(opposed->second, nowUnixSeconds);
    }
    else
    {
        if (victims.size() >= PLAYERBOT_SOCIAL_MAX_OPPOSITION_PER_ATTACKER)
        {
            // An attacker at their own ceiling is refused here. The empty map they may have just
            // opened is dropped rather than left behind holding a slot for nothing.
            if (victims.empty())
                _appliedOpposition.erase(attacker);

            return PlayerbotSocialRelationshipValues();
        }

        victims.emplace(victimGuidCounter, nowUnixSeconds);
    }

    if (sameFight)
        return PlayerbotSocialRelationshipValues();

    PlayerbotSocialRelationshipValues const delta = PlayerbotSocialOpenWorldAggressionDelta();

    /*
     * Recorded here rather than at the caller, which is what gives the feed the once per fight bound
     * for free. Every path above that declines returns before this point, so the event and the delta
     * cannot disagree about whether an opposition happened: a row exists exactly when a delta was
     * produced. Wiring it at the damage hook instead would put the bound in two places and let the
     * feed drift from the relationship it describes.
     */
    PlayerbotSocialPvpOpposition opposition;
    opposition.victimGuidCounter = victimGuidCounter;
    opposition.attackerGuidCounter = opponentGuidCounter;
    opposition.context = context;
    opposition.earned = delta;
    opposition.occurredAtUnixSeconds = nowUnixSeconds;
    RecordEvent(PlayerbotSocialMakePvpEvent(opposition));

    return delta;
}

// Activation ---------------------------------------------------------------------------------------

PlayerbotSocialActivationResult PlayerbotSocialMgr::Activate(PlayerbotSocialActivation const& activation,
                                                             PlayerbotSocialDensityProfile densityProfile,
                                                             PlayerbotSocialRoleplayDirective const& roleplay)
{
    PlayerbotSocialActivationResult result;
    bool const statelessDirectReply = activation.speakerOptedOut && activation.speakerIsHuman && !activation.starter &&
                                      activation.channel == PlayerbotSocialChannel::Whisper;

    /*
     * A handle that names no thread is refused before anything else runs. Observe returns exactly
     * this for an unsupported channel, so an invalid handle is the normal way a refused observation
     * arrives here rather than an exceptional one.
     */
    if (!activation.thread.valid || !PlayerbotSocialChannelIsValid(activation.channel))
    {
        result.rejection = PlayerbotSocialOpportunityRejection::UnsupportedChannel;

        /*
         * Deliberately NOT recorded. An unsupported channel is not a conversation the feature
         * declined to join, it is a line that never belonged to the Social surface at all, and Key
         * Decision 6 says unsupported channels do not enter the Social feed. Recording it would file
         * every guild and raid message as a suppressed social opportunity.
         */
        return result;
    }

    /*
     * Per candidate eligibility first, before pressure and before scoring.
     *
     * The order is load bearing in one direction: a bot that is on cooldown or has opted out must
     * never reach selection, because selection is where the decision to spend a request is made and a
     * refused bot has no business competing for it. The reverse order would let an ineligible bot win
     * and then be dropped, which reads in telemetry as a bot that chose not to speak.
     */
    PlayerbotSocialSelectionInput selection;

    /*
     * The autonomous society stage paces bot-only threads through configuration; every earlier
     * stage keeps the built-in cap and decay, so enabling the stage is the only way to change how
     * long bots talk among themselves.
     */
    bool const autonomousStage = PlayerbotSocialEffectiveGate().stage == PlayerbotSocialRolloutStage::AutonomousSociety;

    for (PlayerbotSocialActivationCandidate const& candidate : activation.candidates)
    {
        if (activation.starter && (activation.starterSourceBotGuidCounter == 0 ||
                                   candidate.botGuidCounter != activation.starterSourceBotGuidCounter))
        {
            result.refusedCandidates.emplace_back(candidate.botGuidCounter,
                                                  PlayerbotSocialOpportunityRejection::StarterSourceMismatch);
            continue;
        }

        if (!activation.starter && activation.speakerGuidCounter != 0 &&
            candidate.botGuidCounter == activation.speakerGuidCounter)
        {
            result.refusedCandidates.emplace_back(candidate.botGuidCounter,
                                                  PlayerbotSocialOpportunityRejection::SelfReply);
            continue;
        }

        PlayerbotSocialOpportunity opportunity;
        opportunity.channel = activation.channel;
        opportunity.starter = activation.starter;
        opportunity.speakerIsHuman = activation.speakerIsHuman;
        opportunity.speakerOptedOut = activation.speakerOptedOut;
        opportunity.botOptedOutOfInitiation = candidate.optedOutOfInitiation;
        opportunity.factionMatches = candidate.factionMatches;
        opportunity.languageMatches = candidate.languageMatches;
        opportunity.threadLastActivityUnixSeconds = activation.threadLastActivityUnixSeconds;
        opportunity.botLastSpokeUnixSeconds = candidate.lastSpokeUnixSeconds;
        opportunity.nowUnixSeconds = activation.nowUnixSeconds;
        opportunity.duplicateOfRecentMessage = activation.duplicateOfRecentMessage;
        opportunity.consecutiveBotOnlyTurns = activation.consecutiveBotOnlyTurns;
        if (autonomousStage)
        {
            opportunity.maxConsecutiveBotOnlyTurns = sPlayerbotSocialConfig.socialChatAutonomousMaxConsecutiveBotTurns;

            // Bot-only threads only: a thread a human has spoken in, or a human line itself,
            // keeps the built-in reply cooldown, so a bot can never pepper a player.
            if (!activation.speakerIsHuman && activation.relevantHumanMessages == 0)
                opportunity.replyCooldownSeconds = sPlayerbotSocialConfig.socialChatAutonomousBotReplyCooldownSeconds;
        }
        opportunity.relationshipDriven = activation.relationshipDriven;
        opportunity.profileLoadState = candidate.profileLoadState;

        PlayerbotSocialOpportunityRejection const refusal = PlayerbotSocialEvaluateOpportunity(opportunity);
        if (refusal != PlayerbotSocialOpportunityRejection::None)
        {
            result.refusedCandidates.emplace_back(candidate.botGuidCounter, refusal);
            continue;
        }

        PlayerbotSocialCandidate scored;
        scored.botGuidCounter = candidate.botGuidCounter;
        scored.effectiveDisposition = candidate.effectiveDisposition;
        scored.stance = candidate.stance;
        scored.addressedByName = candidate.addressedByName;
        scored.askedQuestion = candidate.askedQuestion;
        scored.participatedInThread = candidate.participatedInThread;
        scored.contentRelevance = candidate.contentRelevance;
        selection.candidates.push_back(scored);
    }

    /*
     * Nothing eligible is reported as the reason the FIRST candidate was refused rather than as a
     * generic silence. With one candidate that is exact; with several it names one real cause instead
     * of inventing a summary, and every individual reason is still in `refusedCandidates`.
     */
    if (selection.candidates.empty())
    {
        if (!result.refusedCandidates.empty())
            result.rejection = result.refusedCandidates.front().second;

        if (!statelessDirectReply)
            RecordEvent(PlayerbotSocialMakeOpportunityEvent(activation, result));
        return result;
    }

    PlayerbotSocialThreadPressure pressure;
    pressure.consecutiveBotOnlyTurns = activation.consecutiveBotOnlyTurns;
    pressure.relevantHumanMessages = activation.relevantHumanMessages;
    pressure.lastActivityUnixSeconds = activation.threadLastActivityUnixSeconds;
    pressure.nowUnixSeconds = activation.nowUnixSeconds;
    pressure.channelDensity = activation.channelDensity;
    if (autonomousStage)
    {
        pressure.botOnlyTurnDecay = sPlayerbotSocialConfig.socialChatAutonomousBotTurnDecay;
        pressure.botOnlyContinuationBase = sPlayerbotSocialConfig.socialChatAutonomousContinuationPressureBase;
        pressure.botOnlyDensityThrottleExempt = true;
    }

    PlayerbotSocialDensityMultipliers multipliers;
    multipliers.quiet = sPlayerbotSocialConfig.socialChatDensityMultiplierQuiet;
    multipliers.normal = sPlayerbotSocialConfig.socialChatDensityMultiplierNormal;
    multipliers.lively = sPlayerbotSocialConfig.socialChatDensityMultiplierLively;
    float const densityMultiplier = PlayerbotSocialDensityMultiplier(densityProfile, multipliers);

    /*
     * The pressure roll is NOT taken here. `PlayerbotSocialSelectResponders` already rolls the reply
     * pressure against its own seed, and rolling again before calling it would apply the same
     * probability twice: a thread at 0.6 pressure would answer about a third of the time rather than
     * three fifths, and every calibration figure behind these constants would silently be wrong.
     * Activation supplies the probability and the seed; selection owns the draw.
     */
    result.pressure = activation.starter ? PlayerbotSocialStarterPressureForChannel(
                                               activation.channel, pressure, densityMultiplier,
                                               sPlayerbotSocialConfig.socialChatGeneralStarterPressureMultiplier,
                                               sPlayerbotSocialConfig.socialChatAmbientCadenceSeconds)
                                         : PlayerbotSocialReplyPressure(pressure, densityMultiplier);

    selection.replyPressure = result.pressure;
    selection.secondResponderAllowed = !activation.starter;
    selection.selectionSeed = activation.selectionSeed;

    result.selection = PlayerbotSocialSelectResponders(selection);

    /*
     * Scored candidates but no responders is the pressure roll declining, and nothing else. Every
     * other way to reach an empty responder list removes the candidate before scoring, so it leaves
     * the alternates list empty too and is already named in `refusedCandidates` or in the selection's
     * own suppressions.
     */
    result.pressureDeclined = result.selection.responders.empty() && !result.selection.alternates.empty();
    if (result.pressureDeclined)
    {
        if (!statelessDirectReply)
            RecordEvent(PlayerbotSocialMakeOpportunityEvent(activation, result));
        return result;
    }

    /*
     * A whisper is answered to the character who sent it; every other surface is addressed to a room.
     * Carrying the speaker on a General line would make a room remark look like a private reply in
     * every downstream consumer. A whisper STARTER has no speaker: its addressee is the audience the
     * relationship check-in was opened for.
     *
     * A say or party starter carries its audience too, and this is load bearing rather than
     * decorative: those channels ground against a perceivable audience, so their grounding can name
     * that audience as a Participant, and the provider refuses Participant evidence whose subject
     * did not travel on the request. A target of zero here made every say starter die as
     * provider_failed before a prompt was ever built. Delivery is unaffected: only whisper and
     * emote sends consult the target.
     */
    uint64 target = 0;
    if (activation.channel == PlayerbotSocialChannel::Whisper)
        target = activation.starter ? activation.starterAudienceGuidCounter : activation.speakerGuidCounter;
    else if (activation.starter && PlayerbotSocialStarterParticipantIsPerceivable(activation.channel))
        target = activation.starterAudienceGuidCounter;

    for (uint64 const responder : result.selection.responders)
    {
        /*
         * Derived per responder rather than once for the thread, because being addressed by name is a
         * property of one bot. A thread where one bot was named and another merely overheard must not
         * put both in the lane a player is waiting on.
         *
         * A whisper is direct address by construction: it was sent to this character specifically.
         */
        auto const named = std::find_if(activation.candidates.begin(), activation.candidates.end(),
                                        [responder](PlayerbotSocialActivationCandidate const& candidate)
                                        { return candidate.botGuidCounter == responder; });
        if (named == activation.candidates.end())
            continue;

        bool const addressedDirectly = activation.channel == PlayerbotSocialChannel::Whisper || named->addressedByName;

        PlayerbotSocialRequestPriority const priority = PlayerbotSocialPriorityForLane(
            PlayerbotSocialAdmissionLane(pressure, activation.starter, addressedDirectly));

        /*
         * Recorded before the request is attempted, so a bot that was chosen has a record of it even
         * when nothing downstream succeeds. Emitting it after the attempt would make selection
         * invisible on exactly the paths where knowing who was chosen matters most.
         */
        if (!statelessDirectReply)
            RecordEvent(PlayerbotSocialMakeSelectionEvent(activation, result, responder));

        std::vector<PlayerbotSocialNearbySnapshotEntry> const& nearby = named->nearby;

        /*
         * The worldserver's prompt authority for this responder, decided BEFORE the request opens
         * so the composed provider context and the pending delivery both carry it. A fighting bot
         * never receives an authorized generation: combat suppresses the performance at request
         * time and the request proceeds in ordinary mode instead.
         */
        PlayerbotRoleplayPromptMode mode =
            RoleplayPromptModeFor(activation, roleplay, responder, named->personality.roleplayAffinity);
        if (mode == PlayerbotRoleplayPromptMode::AuthorizedRoleplay && named->inCombat)
            mode = PlayerbotRoleplayPromptMode::Ordinary;

        PlayerbotSocialDeliveryRejection rejection = PlayerbotSocialDeliveryRejection::None;
        uint64 const token = BeginSocialRequest(
            responder, named->personality, target, activation.channel, activation.thread.publicId, priority,
            activation.nowUnixSeconds, activation.zoneId,
            activation.starter ? activation.starterSubject : std::string(), rejection, nearby,
            activation.starter ? activation.starterAudienceGuidCounter : activation.speakerGuidCounter,
            addressedDirectly, activation.currentLine, statelessDirectReply, mode, named->grounding,
            named->askedQuestion);

        /*
         * A refusal here stops this responder and not the opportunity. The common cause is a full
         * queue, and one bot being full says nothing about the next: refusing the whole opportunity
         * would let a single saturated bot silence everyone else in the thread.
         */
        if (token == 0)
        {
            result.refusedRequests.emplace_back(responder, rejection);

            /*
             * The attempt concludes here rather than later. Nothing will ever answer a request that
             * was never opened, so a producer that waited for a provider verdict would simply never
             * emit one. The zero token is what says it never reached the provider.
             */
            PlayerbotSocialProviderAttempt attempt;
            attempt.botGuidCounter = responder;
            attempt.targetGuidCounter = target;
            attempt.channel = activation.channel;
            attempt.threadPublicId = activation.thread.publicId;
            attempt.zoneId = activation.zoneId;
            attempt.occurredAtUnixSeconds = activation.nowUnixSeconds;
            attempt.outcome = PlayerbotSocialProviderAttemptOutcome::Refused;
            attempt.rejection = rejection;
            if (!statelessDirectReply)
                RecordEvent(PlayerbotSocialMakeProviderAttemptEvent(attempt));
            continue;
        }

        if (activation.starter)
        {
            auto const pendingBot = _pendingDeliveries.find(responder);
            if (pendingBot != _pendingDeliveries.end())
            {
                auto const pendingRequest = pendingBot->second.find(token);
                if (pendingRequest != pendingBot->second.end())
                    pendingRequest->second.sourceEventPublicId = activation.starterSourceEventPublicId;
            }
        }

        result.openedTokens.push_back(token);
        result.promptModes.emplace_back(responder, mode);

        // Only an eligible invitation may add a participant; a continuation can only re-authorize
        // a bot an invitation already admitted.
        if (mode == PlayerbotRoleplayPromptMode::AuthorizedRoleplay &&
            roleplay.kind == PlayerbotRoleplayAssessmentKind::RoleplayInvitation)
            NoteRoleplayParticipant(activation.thread.publicId, responder);

        if (mode != PlayerbotRoleplayPromptMode::Ordinary)
            LOG_DEBUG("playerbots", "Social roleplay mode {} for bot {} in thread {}",
                      PlayerbotRoleplayPromptModeName(mode), responder, activation.thread.publicId);
    }

    /*
     * One event for the whole opportunity, emitted on every path that got past the channel check.
     * Placed here rather than at each return so a path added later cannot silently stop reporting:
     * the only way out of this function without an event is the unsupported channel above, which is
     * the one case that must not produce one.
     */
    if (!statelessDirectReply)
        RecordEvent(PlayerbotSocialMakeOpportunityEvent(activation, result));

    return result;
}

// Delivery -----------------------------------------------------------------------------------------

char const* PlayerbotSocialBiographyStateColumn(PlayerbotBiographyState state)
{
    switch (state)
    {
        case PlayerbotBiographyState::Absent:
            return "absent";
        case PlayerbotBiographyState::Pending:
            return "pending";
        case PlayerbotBiographyState::Ready:
            return "ready";
        case PlayerbotBiographyState::RetryableFailure:
            return "retryable_failure";
    }

    // Never written. The ENUM column has no value for an unrecognized state, so a caller must
    // refuse the write rather than invent a spelling the schema would reject anyway.
    return nullptr;
}

namespace
{
// Returned for a bot with no entry. Static so ProfileFor can hand out a reference without
// inserting, which would turn a read into a write and grow the map on every lookup.
PlayerbotSocialProfile const& DefaultSocialProfile()
{
    static PlayerbotSocialProfile const empty;
    return empty;
}

PlayerbotBiographyIdentity IdentityOf(PlayerbotSocialBiographyCandidate const& candidate)
{
    PlayerbotBiographyIdentity identity;
    identity.characterName = candidate.characterName;
    identity.raceId = candidate.raceId;
    identity.classId = candidate.classId;
    identity.genderId = candidate.genderId;
    return identity;
}
}  // namespace

PlayerbotSocialProfile const& PlayerbotSocialMgr::ProfileFor(uint64 botGuidCounter) const
{
    auto const stored = _profiles.find(botGuidCounter);
    return stored == _profiles.end() ? DefaultSocialProfile() : stored->second;
}

PlayerbotSocialProfileLoad const& PlayerbotSocialMgr::ProfileLoadFor(uint64 botGuidCounter) const
{
    static PlayerbotSocialProfileLoad const pending;
    auto const stored = _profileLoads.find(botGuidCounter);
    return stored == _profileLoads.end() ? pending : stored->second;
}

void PlayerbotSocialMgr::QueueProfileWrite(uint64 botGuidCounter, PlayerbotSocialProfile const& profile)
{
    char const* const state = PlayerbotSocialBiographyStateColumn(profile.biographyState);
    if (state == nullptr)
    {
        // Fail closed rather than write a row this build cannot spell. A state that reached here
        // unrecognized came from memory this process owns, so it is a bug worth seeing.
        LOG_ERROR("playerbots",
                  "Playerbot social: refusing to persist profile for bot {} in an unknown "
                  "biography state ({})",
                  botGuidCounter, static_cast<uint32>(profile.biographyState));
        return;
    }

    PlayerbotSocialProfileBinding binding;
    binding.botGuidCounter = botGuidCounter;
    binding.schemaVersion = profile.version;
    binding.traitsVersion = profile.traits.version;
    binding.biographyState = state;
    binding.biographyRequestToken = profile.biographyRequestToken;
    binding.biographyAttemptedAtUnixSeconds = profile.biographyAttemptedAtUnixSeconds;
    binding.biography = profile.biography;

    /*
     * Replaced rather than appended. The row is the whole state, so an earlier pending write is not
     * a change that has to be applied first: it is a value that was already superseded, and writing
     * both would spend two statements to reach the state the second one already describes.
     */
    auto const existing = std::find_if(_pendingProfileWrites.begin(), _pendingProfileWrites.end(),
                                       [botGuidCounter](PlayerbotSocialProfileBinding const& queued)
                                       { return queued.botGuidCounter == botGuidCounter; });
    if (existing != _pendingProfileWrites.end())
    {
        *existing = std::move(binding);
        return;
    }

    _pendingProfileWrites.push_back(std::move(binding));
}

void PlayerbotSocialMgr::QueueTraitsWrite(uint64 botGuidCounter, PlayerbotSocialProfile const& profile)
{
    PlayerbotSocialTraitsBinding binding;
    binding.botGuidCounter = botGuidCounter;
    binding.schemaVersion = profile.version;
    binding.traits = profile.traits;

    auto const existing = std::find_if(_pendingTraitsWrites.begin(), _pendingTraitsWrites.end(),
                                       [botGuidCounter](PlayerbotSocialTraitsBinding const& queued)
                                       { return queued.botGuidCounter == botGuidCounter; });
    if (existing != _pendingTraitsWrites.end())
    {
        *existing = std::move(binding);
        return;
    }

    _pendingTraitsWrites.push_back(std::move(binding));
}

void PlayerbotSocialMgr::LoadProfile(uint64 botGuidCounter)
{
    if (botGuidCounter == 0 || _profileLoads.contains(botGuidCounter))
        return;

    std::optional<PlayerbotPersonalityProfile> const personality = sPlayerbotPersonalityMgr.GetOrCreate(botGuidCounter);
    if (!personality.has_value())
        return;

    PlayerbotSocialProfileLoad pending =
        PlayerbotPersonality::LoadSocialProfile(botGuidCounter, std::nullopt, *personality);
    pending.state = PlayerbotSocialProfileLoadState::Pending;
    _profiles[botGuidCounter] = pending.profile;
    _profileLoads[botGuidCounter] = pending;

    uint64 const epoch = _stateEpoch;

    PlayerbotSocialPreparedStatement* statement = NewPlayerbotSocialStatement(PLAYERBOT_SOCIAL_STMT_SEL_PROFILE);
    statement->SetData(0, static_cast<uint32>(botGuidCounter));

    SocialQueryProcessor().AddCallback(PlayerbotSocialAsyncQuery(statement).WithCallback(
        [this, botGuidCounter, epoch, personality = *personality](QueryResult result)
        {
            // An erasure decided after this read was issued must win over it, or the read restores
            // exactly what was just deleted.
            if (_stateEpoch != epoch)
                return;

            auto const current = _profileLoads.find(botGuidCounter);
            if (current == _profileLoads.end() || current->second.state != PlayerbotSocialProfileLoadState::Pending)
                return;

            auto storeLoad = [this, botGuidCounter](PlayerbotSocialProfileLoad load)
            {
                _profiles[botGuidCounter] = load.profile;
                _profileLoads[botGuidCounter] = std::move(load);
            };

            auto baseLoad = [botGuidCounter, &personality]()
            { return PlayerbotPersonality::LoadSocialProfile(botGuidCounter, std::nullopt, personality); };

            auto storeMalformed = [&storeLoad, &baseLoad]()
            {
                PlayerbotSocialProfileLoad load = baseLoad();
                load.state = PlayerbotSocialProfileLoadState::RejectedUsingBase;
                load.rejection = PlayerbotSocialProfileRejection::MalformedStoredData;
                load.storedRowPresent = true;
                storeLoad(std::move(load));
            };

            // A query failure is not a successful absence. Both use the stable base profile, but
            // only the latter may enter Social.
            if (!result)
            {
                PlayerbotSocialProfileLoad load = baseLoad();
                load.state = PlayerbotSocialProfileLoadState::UnavailableUsingBase;
                storeLoad(std::move(load));
                return;
            }

            Field* fields = result->Fetch();
            if (fields == nullptr)
            {
                PlayerbotSocialProfileLoad load = baseLoad();
                load.state = PlayerbotSocialProfileLoadState::UnavailableUsingBase;
                storeLoad(std::move(load));
                return;
            }

            if (!fields[0].Get<bool>())
            {
                storeLoad(baseLoad());
                return;
            }

            constexpr std::size_t firstProfileField = 1;

            PlayerbotSocialProfile stored;
            stored.version = fields[firstProfileField + 0].Get<uint32>();
            stored.traits.version = fields[firstProfileField + 1].Get<uint32>();
            auto const readTrait = [fields](std::size_t index, uint8& value)
            {
                std::optional<uint64> const storedValue =
                    PlayerbotSocialParseStoredUnsigned(fields[firstProfileField + index].Get<std::string>());
                if (!storedValue || *storedValue > PLAYERBOT_SOCIAL_TRAIT_MAX)
                    return false;

                value = static_cast<uint8>(*storedValue);
                return true;
            };

            if (!readTrait(2, stored.traits.warmth) || !readTrait(3, stored.traits.talkativeness) ||
                !readTrait(4, stored.traits.curiosity) || !readTrait(5, stored.traits.humor) ||
                !readTrait(6, stored.traits.formality))
            {
                LOG_WARN("playerbots",
                         "Playerbot social: bot {} has a stored trait outside 0..{}, "
                         "discarding its profile",
                         botGuidCounter, PLAYERBOT_SOCIAL_TRAIT_MAX);
                storeMalformed();
                return;
            }
            std::optional<uint64> const lastEvolvedAt =
                PlayerbotSocialParseStoredUnsigned(fields[firstProfileField + 7].Get<std::string>());
            if (!lastEvolvedAt)
            {
                storeMalformed();
                return;
            }
            stored.traits.lastEvolvedAtUnixSeconds = *lastEvolvedAt;

            std::size_t const interestCount = std::min<std::size_t>(fields[firstProfileField + 8].Get<uint64>(),
                                                                    PLAYERBOT_SOCIAL_MAX_EVOLVING_TOPICS);
            for (std::size_t index = 0; index < interestCount; ++index)
                stored.traits.interests.push_back(fields[firstProfileField + 9 + index].Get<std::string>());

            std::size_t const aversionCount = std::min<std::size_t>(fields[firstProfileField + 17].Get<uint64>(),
                                                                    PLAYERBOT_SOCIAL_MAX_EVOLVING_TOPICS);
            for (std::size_t index = 0; index < aversionCount; ++index)
                stored.traits.aversions.push_back(fields[firstProfileField + 18 + index].Get<std::string>());

            std::string const state = fields[firstProfileField + 26].Get<std::string>();
            if (state == "pending")
                stored.biographyState = PlayerbotBiographyState::Pending;
            else if (state == "ready")
                stored.biographyState = PlayerbotBiographyState::Ready;
            else if (state == "retryable_failure")
                stored.biographyState = PlayerbotBiographyState::RetryableFailure;
            else if (state == "absent")
                stored.biographyState = PlayerbotBiographyState::Absent;
            else
            {
                storeMalformed();
                return;
            }

            stored.biographyRequestToken = fields[firstProfileField + 27].Get<uint64>();
            stored.biographyAttemptedAtUnixSeconds = fields[firstProfileField + 28].Get<uint64>();

            /*
             * The document, taken apart by MySQL rather than by a JSON parser this module would
             * otherwise have to grow. Restoring the STATE without the text would be worse than not
             * loading at all: Ready suppresses regeneration permanently, so the bot would keep a
             * biography that exists in the database and nowhere else, and the composed persona would
             * carry an empty one while reporting that it had a player profile.
             *
             * Copied ONLY when the query says a document exists and carries all thirteen keys.
             * Anything less is left as the default empty biography, which already holds the current
             * version, so an ordinary row with no biography loads cleanly while a Ready row with a
             * partial one arrives empty and is refused by LoadSocialProfile. Both flags come from
             * the query because neither survives extraction: a missing key and a stored zero read
             * back identically, and for `gender_id` a missing key and GENDER_MALE do too.
             */
            bool const documentPresent = fields[firstProfileField + 29].Get<bool>();
            bool const documentKeysComplete = fields[firstProfileField + 30].Get<bool>();

            if (documentPresent && documentKeysComplete)
            {
                std::optional<uint64> const biographyVersion =
                    PlayerbotSocialParseStoredUnsigned(fields[firstProfileField + 31].Get<std::string>());
                std::optional<uint64> const raceId =
                    PlayerbotSocialParseStoredUnsigned(fields[firstProfileField + 33].Get<std::string>());
                std::optional<uint64> const classId =
                    PlayerbotSocialParseStoredUnsigned(fields[firstProfileField + 34].Get<std::string>());
                std::optional<uint64> const genderId =
                    PlayerbotSocialParseStoredUnsigned(fields[firstProfileField + 35].Get<std::string>());
                if (!biographyVersion || !raceId || !classId || !genderId)
                {
                    storeMalformed();
                    return;
                }

                stored.biography.version = static_cast<uint32>(*biographyVersion);
                stored.biography.identity.characterName = fields[firstProfileField + 32].Get<std::string>();
                stored.biography.identity.raceId = static_cast<uint8>(*raceId);
                stored.biography.identity.classId = static_cast<uint8>(*classId);
                stored.biography.identity.genderId = static_cast<uint8>(*genderId);
                stored.biography.origin = fields[firstProfileField + 36].Get<std::string>();
                stored.biography.motivation = fields[firstProfileField + 37].Get<std::string>();
                stored.biography.formativeExperience = fields[firstProfileField + 38].Get<std::string>();
                stored.biography.interests = fields[firstProfileField + 39].Get<std::string>();
                stored.biography.aversions = fields[firstProfileField + 40].Get<std::string>();
                stored.biography.preferredTopics = fields[firstProfileField + 41].Get<std::string>();
                stored.biography.mannerisms = fields[firstProfileField + 42].Get<std::string>();
                stored.biography.values = fields[firstProfileField + 43].Get<std::string>();
            }
            else if (documentPresent)
            {
                // Present but missing keys. Worth a line, because it means a document was written by
                // something that does not agree with this build about what a biography contains.
                LOG_WARN("playerbots",
                         "Playerbot social: bot {} has a stored biography missing required keys, "
                         "discarding it",
                         botGuidCounter);
            }

            /*
             * A request that was in flight when the process stopped is nobody's now: the bridge, the
             * outstanding map and the sidecar's own work all died with it, so no answer can ever
             * arrive. Restoring it as Pending would make the bot wait out the abandonment window for
             * a call that no longer exists, so it is restored as a failure, which retries sooner.
             */
            if (stored.biographyState == PlayerbotBiographyState::Pending)
            {
                stored.biographyState = PlayerbotBiographyState::RetryableFailure;
                stored.biographyRequestToken = 0;
            }

            PlayerbotSocialProfileLoad const load =
                PlayerbotPersonality::LoadSocialProfile(botGuidCounter, stored, personality);

            storeLoad(load);
        }));
}

uint64 PlayerbotSocialMgr::RequestBiographyFor(PlayerbotSocialBiographyCandidate const& candidate,
                                               uint64 nowUnixSeconds)
{
    if (_provider == nullptr || candidate.botGuidCounter == 0 || candidate.characterName.empty())
        return 0;

    PlayerbotSocialProfile const& profile = ProfileFor(candidate.botGuidCounter);
    if (!PlayerbotPersonality::ShouldRequestBiography(profile, nowUnixSeconds))
        return 0;

    uint64 const token = _nextBiographyRequestToken++;

    /*
     * Submitted BEFORE the profile is marked, and the order is the whole reason a refusal is
     * harmless. Marking first would leave the profile Pending against a request nobody holds
     * whenever the provider refuses, and that bot would then be ineligible for the entire
     * abandonment window without a single generation having been spent on it.
     */
    if (!_provider->SubmitBiography(token, candidate.botGuidCounter, candidate.characterName, candidate.raceId,
                                    candidate.classId, candidate.genderId))
        return 0;

    PlayerbotSocialProfile const requested =
        PlayerbotPersonality::MarkBiographyRequested(profile, nowUnixSeconds, token);

    /*
     * The request this one supersedes is retired here rather than left for the expiry sweep.
     *
     * A profile awaits exactly one request, so the coordinator tracks exactly one. The sweep does
     * normally clear the old entry within its own timeout, but a reissue that happens first would
     * otherwise leak an entry per lost request for the rest of the uptime, and leave a superseded
     * answer still matching a request the coordinator believes it holds.
     */
    std::erase_if(_biographyRequests,
                  [&candidate](auto const& entry) { return entry.second.botGuidCounter == candidate.botGuidCounter; });

    _profiles[candidate.botGuidCounter] = requested;
    _biographyRequests[token] = OutstandingBiography{candidate.botGuidCounter, nowUnixSeconds};
    QueueProfileWrite(candidate.botGuidCounter, requested);
    return token;
}

PlayerbotBiographyCompletionRejection PlayerbotSocialMgr::AcceptBiographyResult(
    uint64 biographyRequestToken, uint64 botGuidCounter, std::vector<PlayerbotBiographyFieldValue> const& fields,
    PlayerbotSocialBiographyCandidate const& authoritative, uint64 nowUnixSeconds)
{
    /*
     * The token has to name a request THIS coordinator issued, for THIS bot. The profile's own
     * token check would catch a superseded reply on its own, but not a well formed answer naming a
     * different bot: a token that leaked would otherwise write one bot's player profile onto another.
     */
    auto const outstanding = _biographyRequests.find(biographyRequestToken);
    if (outstanding == _biographyRequests.end() || outstanding->second.botGuidCounter != botGuidCounter)
        return PlayerbotBiographyCompletionRejection::NotAwaited;

    PlayerbotSocialProfile const& profile = ProfileFor(botGuidCounter);
    PlayerbotBiographyIdentity const identity = IdentityOf(authoritative);

    /*
     * Assembled while the field NAMES still exist. Once a payload has been copied into a typed
     * biography the unknown names are already gone, so this is the only place the whitelist can be
     * enforced, and identity is filled from `identity` rather than accepted from the payload.
     */
    PlayerbotBiographyAssembly const assembled =
        PlayerbotPersonality::AssembleBiography(fields, PLAYERBOT_SOCIAL_PERSONA_VERSION, identity);

    PlayerbotBiographyCompletion const outcome =
        assembled.accepted ? PlayerbotPersonality::ApplyBiographyCompletion(profile, biographyRequestToken,
                                                                            assembled.biography, identity)
                           : PlayerbotPersonality::ApplyBiographyFailure(profile, biographyRequestToken);

    /*
     * A refusal by the fence leaves the profile untouched and the request outstanding: a completion
     * nobody is waiting on must not be able to retire a request that IS still live. Every other
     * outcome answered the request, so it is retired whether or not the answer was usable.
     */
    if (outcome.rejection == PlayerbotBiographyCompletionRejection::NotAwaited ||
        outcome.rejection == PlayerbotBiographyCompletionRejection::TokenMismatch)
        return outcome.rejection;

    (void)nowUnixSeconds;
    _biographyRequests.erase(outstanding);
    _profiles[botGuidCounter] = outcome.profile;
    QueueProfileWrite(botGuidCounter, outcome.profile);
    return outcome.rejection;
}

std::vector<uint64> PlayerbotSocialMgr::ExpireTimedOutBiographyRequests(uint64 nowUnixSeconds)
{
    std::vector<uint64> expired;

    for (auto request = _biographyRequests.begin(); request != _biographyRequests.end();)
    {
        // A clock that moved backwards reads as a request from the future. Waiting is the fail
        // closed answer: expiring early would abandon a call that is still in flight.
        if (nowUnixSeconds < request->second.requestedAtUnixSeconds ||
            nowUnixSeconds - request->second.requestedAtUnixSeconds < PLAYERBOT_SOCIAL_PROVIDER_TIMEOUT_SECONDS)
        {
            ++request;
            continue;
        }

        uint64 const botGuidCounter = request->second.botGuidCounter;
        PlayerbotBiographyCompletion const outcome =
            PlayerbotPersonality::ApplyBiographyFailure(ProfileFor(botGuidCounter), request->first);

        // Recorded even when the fence refused. The request is being abandoned either way, and
        // leaving the entry behind would leak one per lost request for the rest of the uptime.
        if (outcome.rejection == PlayerbotBiographyCompletionRejection::Invalid)
        {
            _profiles[botGuidCounter] = outcome.profile;
            QueueProfileWrite(botGuidCounter, outcome.profile);
        }

        expired.push_back(request->first);
        request = _biographyRequests.erase(request);
    }

    return expired;
}

void PlayerbotSocialMgr::SetSocialProvider(PlayerbotSocialProvider* provider)
{
    /*
     * Outstanding requests are deliberately NOT cancelled here.
     *
     * They are already keyed by token and already time out on their own, and a result that arrives
     * from a provider being replaced is refused by token rather than by identity. Cancelling instead
     * would mean a configuration reload silently swallowed conversations that were about to land.
     *
     * Pending roleplay assessments follow the same rule for a stronger reason: each one HOLDS an
     * activation, and the world pump's ExpireTimedOutAssessments sweep resumes a still-current one
     * in ordinary mode when no answer comes. Cancelling here would drop those held conversations
     * outright; CancelPendingAssessments exists for callers that explicitly want that.
     */
    _provider = provider;
}

PlayerbotSocialRequestContext PlayerbotSocialMgr::ComposeRequestContext(
    uint64 botGuidCounter, PlayerbotPersonalityProfile const& personality, uint64 targetGuidCounter,
    PlayerbotSocialChannel channel, std::string const& starterSubject, uint64 nowUnixSeconds,
    std::string const& threadPublicId, std::vector<PlayerbotSocialNearbySnapshotEntry> const& nearby) const
{
    return ComposeRequestContextForSubject(botGuidCounter, personality, targetGuidCounter, channel, starterSubject,
                                           nowUnixSeconds, threadPublicId, nearby, targetGuidCounter != 0,
                                           PlayerbotSocialPromptLine(), false);
}

PlayerbotSocialRequestContext PlayerbotSocialMgr::ComposeRequestContextForSubject(
    uint64 botGuidCounter, PlayerbotPersonalityProfile const& personality, uint64 subjectGuidCounter,
    PlayerbotSocialChannel channel, std::string const& starterSubject, uint64 nowUnixSeconds,
    std::string const& threadPublicId, std::vector<PlayerbotSocialNearbySnapshotEntry> const& nearby,
    bool addressedDirectly, PlayerbotSocialPromptLine const& currentLine, bool statelessDirectReply,
    PlayerbotRoleplayPromptMode promptMode) const
{
    PlayerbotSocialRequestContext context;
    context.starter = starterSubject;

    /*
     * The trusted authority fields. An invalid mode fails closed to ordinary, and the active
     * expansion always comes from the worldserver's own progression policy: the sidecar can
     * neither supply nor override either.
     */
    context.promptMode =
        PlayerbotRoleplayPromptModeIsValid(promptMode) ? promptMode : PlayerbotRoleplayPromptMode::Ordinary;
    context.activeContentExpansion = PlayerbotSocialActiveContentExpansion();

    PlayerbotSocialPromptContextConsent const consents = [this](uint64 guid) { return !IsOptedOut(guid); };
    if (!statelessDirectReply)
        context.nearby = PlayerbotSocialRenderNearby(PlayerbotSocialBuildNearbyPromptSnapshot(nearby, consents));

    /*
     * The relationship is directional and is read even when it is empty, because "we have never
     * met" is itself the answer the persona composes against: a default relationship is what makes
     * a bot sound like a stranger rather than like nobody in particular.
     */
    PlayerbotSocialRelationshipKey key;
    key.botGuidCounter = botGuidCounter;
    key.subjectGuidCounter = subjectGuidCounter;

    PlayerbotSocialRelationshipValues const relationship =
        statelessDirectReply ? PlayerbotSocialRelationshipValues() : _state.RecallRelationship(key);

    PlayerbotSocialPersonaContext personaContext;
    personaContext.channel = channel;
    personaContext.addressedDirectly = addressedDirectly;

    PlayerbotEffectiveSocialPersona const persona = PlayerbotPersonality::ComposeEffectiveSocialPersona(
        personality, ProfileFor(botGuidCounter), relationship, personaContext);

    context.persona = PlayerbotSocialRenderPersona(persona);

    /*
     * The ordinary fictional player identity is omitted from an authorized performance entirely.
     * A fictional age or home country is a fact about the PLAYER persona, not the character being
     * performed, and offering it alongside an in character premise invites the model to graft one
     * onto the other. The three ordinary modes keep the existing behavior and validator unchanged.
     */
    if (context.promptMode != PlayerbotRoleplayPromptMode::AuthorizedRoleplay)
    {
        PlayerbotFictionalIdentityValue const identity = {PLAYERBOT_FICTIONAL_IDENTITY_VERSION,
                                                          personality.fictionalAge, personality.fictionalHomeCountry};
        context.fictionalIdentity =
            PlayerbotFictionalIdentity::ResolveRequest(identity, currentLine.text, addressedDirectly, persona);
    }

    /*
     * Only when the line is addressed to somebody. A memory is always ABOUT a character, so a
     * broadcast with no addressee has no key to recall against, and recalling against a zero
     * subject would offer the bot facts about nobody.
     */
    if (!statelessDirectReply && subjectGuidCounter != 0)
    {
        context.relationship = PlayerbotSocialRenderRelationship(relationship);
        context.memories = PlayerbotSocialSelectContextMemories(_state, key, channel);
    }

    if (statelessDirectReply)
    {
        PlayerbotSocialPromptContextBuffer immediate;
        immediate.Offer(channel, currentLine, true, nowUnixSeconds);
        PlayerbotSocialPromptContextSnapshot const snapshot =
            PlayerbotSocialBuildPromptContextSnapshot(immediate, [](uint64) { return true; }, nowUnixSeconds);
        context.thread = PlayerbotSocialRenderPromptThread(snapshot);
        return context;
    }

    PlayerbotSocialThreadKey threadScope;
    Thread const* const thread = ThreadScopeFor(threadPublicId, threadScope) && threadScope.channel == channel
                                     ? FindThread(threadPublicId)
                                     : nullptr;
    if (thread != nullptr)
    {
        PlayerbotSocialPromptContextSnapshot const snapshot =
            PlayerbotSocialBuildPromptContextSnapshot(thread->promptContext, consents, nowUnixSeconds);
        context.thread = PlayerbotSocialRenderPromptThread(snapshot);
    }

    return context;
}

uint64 PlayerbotSocialMgr::BeginSocialRequest(uint64 botGuidCounter, PlayerbotPersonalityProfile const& personality,
                                              uint64 targetGuidCounter, PlayerbotSocialChannel channel,
                                              std::string const& threadPublicId,
                                              PlayerbotSocialRequestPriority priority, uint64 nowUnixSeconds,
                                              uint32 zoneId, std::string const& starterSubject,
                                              PlayerbotSocialDeliveryRejection& rejection,
                                              std::vector<PlayerbotSocialNearbySnapshotEntry> const& nearby)
{
    return BeginSocialRequest(botGuidCounter, personality, targetGuidCounter, channel, threadPublicId, priority,
                              nowUnixSeconds, zoneId, starterSubject, rejection, nearby, targetGuidCounter,
                              targetGuidCounter != 0, PlayerbotSocialPromptLine(), false);
}

uint64 PlayerbotSocialMgr::BeginSocialRequest(
    uint64 botGuidCounter, PlayerbotPersonalityProfile const& personality, uint64 targetGuidCounter,
    PlayerbotSocialChannel channel, std::string const& threadPublicId, PlayerbotSocialRequestPriority priority,
    uint64 nowUnixSeconds, uint32 zoneId, std::string const& starterSubject,
    PlayerbotSocialDeliveryRejection& rejection, std::vector<PlayerbotSocialNearbySnapshotEntry> const& nearby,
    uint64 subjectGuidCounter, bool addressedDirectly, PlayerbotSocialPromptLine const& currentLine,
    bool statelessDirectReply, PlayerbotRoleplayPromptMode promptMode,
    PlayerbotSocialGroundingEnvelope const& grounding, bool expectsAnswer)
{
    rejection = PlayerbotSocialDeliveryRejection::None;

    if (botGuidCounter == 0 || !PlayerbotSocialChannelIsValid(channel))
    {
        rejection = PlayerbotSocialDeliveryRejection::UnsupportedChannel;
        return 0;
    }

    /*
     * The thread identity is stored, so its shape is checked before it is: an arbitrary length
     * string copied into a bounded number of slots is not a bounded amount of memory. The typed
     * validator also refuses an identity of the wrong KIND, so an actor or event id cannot be
     * carried here as though it named a conversation.
     */
    if (!PlayerbotSocialPublicIdIsValid(PlayerbotSocialIdKind::Thread, threadPublicId))
    {
        rejection = PlayerbotSocialDeliveryRejection::MalformedThreadIdentity;
        return 0;
    }

    /*
     * The server-wide budget rules before anything is reserved or recorded, so a refused request
     * costs nothing downstream. Every generation the sidecar would run passes through here, which
     * is what makes the ceiling an actual ceiling on sidecar load. A reply is a continuation and
     * may draw the reserved bottom of the bucket; a starter (its subject is what marks it) stops
     * above the reserve, so the starter flood can never silence the conversations it opens.
     */
    if (!AdmitProviderCall(nowUnixSeconds, PlayerbotSocialProviderCallDrawsReserve(channel, starterSubject.empty())))
    {
        rejection = PlayerbotSocialDeliveryRejection::BudgetExhausted;
        return 0;
    }

    if (starterSubject.empty() && !statelessDirectReply)
    {
        if (!PlayerbotSocialPublicIdIsValid(PlayerbotSocialIdKind::Event, currentLine.eventPublicId))
        {
            rejection = PlayerbotSocialDeliveryRejection::MissingReplyParent;
            return 0;
        }

        PlayerbotSocialThreadKey threadScope;
        Thread const* const thread = ThreadScopeFor(threadPublicId, threadScope) ? FindThread(threadPublicId) : nullptr;
        if (thread == nullptr || ElapsedSeconds(nowUnixSeconds, thread->lastActivityUnixSeconds) >
                                     PLAYERBOT_SOCIAL_PROMPT_CONTEXT_RETENTION_SECONDS)
        {
            rejection = PlayerbotSocialDeliveryRejection::SupersededThread;
            return 0;
        }

        if (threadScope.channel != channel)
        {
            rejection = PlayerbotSocialDeliveryRejection::ReplyParentMismatch;
            return 0;
        }

        if (currentLine.speakerIsHuman && IsOptedOut(currentLine.speakerGuidCounter))
        {
            rejection = PlayerbotSocialDeliveryRejection::ConsentWithdrawn;
            return 0;
        }

        bool const parentTimeIsCurrent =
            currentLine.atUnixSeconds <= nowUnixSeconds &&
            nowUnixSeconds - currentLine.atUnixSeconds <= PLAYERBOT_SOCIAL_PROMPT_CONTEXT_RETENTION_SECONDS;
        bool const exactParent =
            parentTimeIsCurrent &&
            std::any_of(thread->promptContext.Lines().begin(), thread->promptContext.Lines().end(),
                        [&currentLine](PlayerbotSocialPromptLine const& retained)
                        {
                            return retained.eventPublicId == currentLine.eventPublicId &&
                                   retained.role == currentLine.role &&
                                   retained.replyToEventPublicId == currentLine.replyToEventPublicId &&
                                   retained.sourceEventPublicId == currentLine.sourceEventPublicId &&
                                   retained.speakerGuidCounter == currentLine.speakerGuidCounter &&
                                   retained.speakerIsHuman == currentLine.speakerIsHuman &&
                                   retained.atUnixSeconds == currentLine.atUnixSeconds &&
                                   retained.text == currentLine.text;
                        });
        if (!exactParent)
        {
            rejection = PlayerbotSocialDeliveryRejection::ReplyParentMismatch;
            return 0;
        }
    }

    if (!PlayerbotSocialGroundingEnvelopeIsValid(grounding))
    {
        rejection = PlayerbotSocialDeliveryRejection::GroundingUnavailable;
        return 0;
    }

    // Silence rather than canned fallback, and cheap: nothing is allocated for a request that has
    // nowhere to go.
    if (_provider == nullptr)
    {
        rejection = PlayerbotSocialDeliveryRejection::NoProvider;
        return 0;
    }

    auto bot = _pendingDeliveries.find(botGuidCounter);

    /*
     * Refused when full rather than displacing an existing request, because a pending delivery is
     * work that a conversation is already waiting on. Dropping one to make room for another loses a
     * reply that was about to land, and lets whichever bot is loudest evict everyone else.
     */
    if (bot == _pendingDeliveries.end())
    {
        if (_pendingDeliveries.size() >= PLAYERBOT_SOCIAL_MAX_PENDING_BOTS)
        {
            rejection = PlayerbotSocialDeliveryRejection::QueueFull;
            return 0;
        }

        bot = _pendingDeliveries.emplace(botGuidCounter, std::map<uint64, PlayerbotSocialPendingDelivery>()).first;
    }

    for (auto const& pending : bot->second)
    {
        if (pending.second.threadPublicId == threadPublicId)
        {
            rejection = PlayerbotSocialDeliveryRejection::QueueFull;
            return 0;
        }
    }

    if (bot->second.size() >= PLAYERBOT_SOCIAL_MAX_PENDING_PER_BOT)
    {
        // A bot on its own ceiling leaves no empty entry behind holding a slot for nothing.
        if (bot->second.empty())
            _pendingDeliveries.erase(bot);

        rejection = PlayerbotSocialDeliveryRejection::QueueFull;
        return 0;
    }

    /*
     * The last slot is reserved for the lanes a player is actually waiting on. Without this a pair of
     * starters occupies everything a bot has and blocks the direct engagement that arrives a moment
     * later, which is the same starvation a shared ceiling causes, one level down.
     */
    if (bot->second.size() == PLAYERBOT_SOCIAL_MAX_PENDING_PER_BOT - 1 &&
        !PlayerbotSocialPriorityMayTakeLastSlot(priority))
    {
        if (bot->second.empty())
            _pendingDeliveries.erase(bot);

        rejection = PlayerbotSocialDeliveryRejection::QueueReservedForPlayers;
        return 0;
    }

    /*
     * A say reply anchors its delivery revalidation on the speaker it answers. A say thread's
     * scope id is a cohort-registry counter rather than a zone, so the targetless revalidation
     * branch (zone == scope) can never hold and every targetless say reply died as different_map
     * on live. On /say the speaker IS the conversation's location, so the spatial revalidation
     * runs against them, exactly as a say starter revalidates against its perceivable audience.
     * General replies stay targetless: their scope is the zone, and the room check is correct.
     */
    if (targetGuidCounter == 0 && channel == PlayerbotSocialChannel::Say && starterSubject.empty() &&
        currentLine.speakerGuidCounter != 0)
        targetGuidCounter = currentLine.speakerGuidCounter;

    PlayerbotSocialPendingDelivery pending;
    pending.requestToken = _nextRequestToken++;
    pending.botGuidCounter = botGuidCounter;
    pending.targetGuidCounter = targetGuidCounter;
    pending.subjectGuidCounter = subjectGuidCounter;
    pending.channel = channel;
    pending.threadPublicId = threadPublicId;
    if (!statelessDirectReply &&
        PlayerbotSocialPublicIdIsValid(PlayerbotSocialIdKind::Event, currentLine.eventPublicId))
        pending.replyToEventPublicId = currentLine.eventPublicId;
    pending.grounding = grounding;
    PlayerbotSocialOperatorEvidence operatorEvidence;
    operatorEvidence.grounding = grounding;
    operatorEvidence.profileLoad = ProfileLoadFor(botGuidCounter);
    operatorEvidence.rolloutStage = PlayerbotSocialEffectiveGate().stage;
    pending.operatorEvidence = std::move(operatorEvidence);
    pending.requestedAtUnixSeconds = nowUnixSeconds;
    pending.priority = priority;
    pending.zoneId = zoneId;
    pending.statelessDirectReply = statelessDirectReply;
    pending.expectsAnswer = expectsAnswer;

    // A trusted worldserver value: an invalid mode fails closed to ordinary rather than carrying
    // an unrecognized authority into the pending state.
    pending.authorizedRoleplay =
        PlayerbotRoleplayPromptModeIsValid(promptMode) && promptMode == PlayerbotRoleplayPromptMode::AuthorizedRoleplay;

    /*
     * Submitted before the request is recorded, so a provider that refuses outright leaves no state
     * behind. It cannot answer before this returns: the seam is asynchronous by contract, and the
     * world thread cannot wait on a network round trip.
     */
    /*
     * Warm the durable state this pair's context is composed from.
     *
     * Both are asynchronous, so what they load lands after this request has already been submitted:
     * the FIRST line between two characters is composed against whatever is already in memory, and
     * every later one against the loaded state. That is the right trade rather than a defect. The
     * alternative is a synchronous read on the world thread for every opportunity, and a bot
     * answering a fraction of a second later with a slightly thinner context is invisible next to a
     * worldserver that stalls on the database whenever anybody speaks.
     *
     * Both no-op when their snapshot is still fresh, when the pair may not be stored, and when
     * either character has no durable actor row yet, so calling them per request is bounded.
     */
    if (!statelessDirectReply)
    {
        LoadRelationship(botGuidCounter, subjectGuidCounter, nowUnixSeconds);
        LoadMemories(botGuidCounter, channel, nowUnixSeconds);
    }

    PlayerbotSocialRequestContext context = ComposeRequestContextForSubject(
        botGuidCounter, personality, subjectGuidCounter, channel, starterSubject, nowUnixSeconds, threadPublicId,
        nearby, addressedDirectly, currentLine, statelessDirectReply, promptMode);
    context.grounding = grounding;

    /*
     * The wire subject the provider validates Participant evidence against. It is the delivery
     * target when one travels (a whisper, a perceivable starter's audience), and otherwise the
     * participant the grounding envelope itself names: a reply grounds against the speaker it
     * answers, but its delivery target must stay zero because a room reply is revalidated against
     * the thread's scope rather than against one character. Reading the envelope rather than
     * reusing subjectGuidCounter is deliberate: a General starter carries an audience it cannot
     * perceive, and that audience must not travel as a participant the prompt would describe as
     * present. Without this every say and general reply carried Participant evidence with no
     * subject and died as provider_failed before a prompt was ever built.
     */
    uint64 wireSubjectGuidCounter = targetGuidCounter;
    if (wireSubjectGuidCounter == 0)
        for (PlayerbotSocialEvidenceEntry const& entry : grounding.entries)
            if (entry.subjectRole == PlayerbotSocialEvidenceSubjectRole::Participant)
            {
                wireSubjectGuidCounter = entry.subjectGuidCounter;
                break;
            }

    if (!_provider->Submit(pending.requestToken, botGuidCounter, wireSubjectGuidCounter, channel, threadPublicId,
                           priority, context))
    {
        if (bot->second.empty())
            _pendingDeliveries.erase(bot);

        rejection = PlayerbotSocialDeliveryRejection::ProviderFailed;
        return 0;
    }

    uint64 const token = pending.requestToken;
    bot->second.emplace(token, std::move(pending));

    return token;
}

namespace
{
// Finds one pending request by token. The map is nested by bot, so the token alone does not say
// where it lives.
template <class Map>
auto FindPendingByToken(Map& pending, uint64 requestToken)
{
    for (auto bot = pending.begin(); bot != pending.end(); ++bot)
    {
        auto found = bot->second.find(requestToken);
        if (found != bot->second.end())
            return std::make_pair(bot, found);
    }

    return std::make_pair(pending.end(), decltype(pending.begin()->second.begin()){});
}
}  // namespace

PlayerbotSocialDeliveryRejection PlayerbotSocialMgr::AcceptSocialResult(PlayerbotSocialProviderResult const& result,
                                                                        uint64 nowUnixMilliseconds, uint32 roll)
{
    // Zero is not a token, so a result carrying one names no request at all.
    if (result.requestToken == 0)
        return PlayerbotSocialDeliveryRejection::UnknownRequest;

    auto [bot, pending] = FindPendingByToken(_pendingDeliveries, result.requestToken);
    if (bot == _pendingDeliveries.end())
        return PlayerbotSocialDeliveryRejection::UnknownRequest;

    // A second result for the same token answers a request that is already answered.
    if (pending->second.resultArrived)
        return PlayerbotSocialDeliveryRejection::UnknownRequest;

    /*
     * Assembled from the pending request BEFORE any branch below erases it. Every exit from here is
     * a conclusion for this attempt, and reading the entry afterwards would find nothing.
     *
     * The zone comes from the request rather than from the bot as it is now. A conclusion arrives
     * after a network round trip, so re-reading the bot would report where it wandered to, not where
     * the conversation happened.
     *
     * The event timestamp is stored in seconds while `nowUnixMilliseconds` carries the same wall
     * clock in milliseconds for delivery scheduling. Reading seconds here keeps the conversion at
     * the event boundary rather than duplicating it at each caller.
     */
    PlayerbotSocialProviderAttempt attempt;
    attempt.requestToken = pending->second.requestToken;
    attempt.botGuidCounter = pending->second.botGuidCounter;
    attempt.targetGuidCounter = pending->second.targetGuidCounter;
    attempt.channel = pending->second.channel;
    attempt.threadPublicId = pending->second.threadPublicId;
    attempt.zoneId = pending->second.zoneId;
    attempt.occurredAtUnixSeconds = static_cast<uint64>(time(nullptr));
    attempt.operatorEvidence = pending->second.operatorEvidence;
    bool const retainTelemetry = !pending->second.statelessDirectReply;

    /*
     * The shape is checked against the channel the bot was ASKED to speak on, not the one the result
     * names, which is what makes a channel switch a refusal rather than a redirect.
     */
    PlayerbotSocialDeliveryRejection const shape = PlayerbotSocialValidateOutput(result, pending->second.channel);
    if (shape != PlayerbotSocialDeliveryRejection::None)
    {
        bot->second.erase(pending);
        if (bot->second.empty())
            _pendingDeliveries.erase(bot);

        attempt.outcome = PlayerbotSocialProviderAttemptOutcome::Refused;
        attempt.rejection = shape;
        if (retainTelemetry)
            RecordEvent(PlayerbotSocialMakeProviderAttemptEvent(attempt));

        return shape;
    }

    if (attempt.operatorEvidence)
    {
        attempt.operatorEvidence->contribution = result.contribution;
        attempt.operatorEvidence->citedEvidenceIds = result.citedEvidenceIds;
    }

    // Silence is a legitimate answer with nothing to deliver, so the request closes here.
    if (result.kind == PlayerbotSocialOutputKind::Silence)
    {
        bot->second.erase(pending);
        if (bot->second.empty())
            _pendingDeliveries.erase(bot);

        attempt.outcome = PlayerbotSocialProviderAttemptOutcome::Silent;
        if (retainTelemetry)
            RecordEvent(PlayerbotSocialMakeProviderAttemptEvent(attempt));

        return PlayerbotSocialDeliveryRejection::None;
    }

    /*
     * Recorded here rather than at delivery. This is where the PROVIDER's part ends: what the world
     * does with the answer afterwards is the delivery event's subject, and folding the two together
     * would leave a revalidated line that the world refused looking like a provider failure.
     */
    attempt.outcome = PlayerbotSocialProviderAttemptOutcome::Answered;
    attempt.callMetadata = result.callMetadata;
    if (retainTelemetry)
        RecordEvent(PlayerbotSocialMakeProviderAttemptEvent(attempt));

    pending->second.resultArrived = true;
    pending->second.result = result;
    pending->second.operatorEvidence = attempt.operatorEvidence;

    // An emote is a gesture and carries no line. Any text attached to one is dropped rather than
    // stored, so nothing unused is held for the life of the request.
    if (result.kind == PlayerbotSocialOutputKind::Emote)
        pending->second.result.text.clear();

    pending->second.deliverAtUnixMilliseconds =
        nowUnixMilliseconds + PlayerbotSocialDeliveryDelayMs(result.text.size(), roll);

    return PlayerbotSocialDeliveryRejection::None;
}

std::vector<uint64> PlayerbotSocialMgr::DueDeliveries(uint64 nowUnixMilliseconds) const
{
    std::vector<uint64> due;

    for (auto const& [botGuidCounter, requests] : _pendingDeliveries)
    {
        for (auto const& [token, pending] : requests)
        {
            if (pending.resultArrived && nowUnixMilliseconds >= pending.deliverAtUnixMilliseconds)
                due.push_back(token);
        }
    }

    return due;
}

bool PlayerbotSocialMgr::PendingDeliveryFor(uint64 requestToken, PlayerbotSocialPendingDelivery& out) const
{
    auto [bot, pending] = FindPendingByToken(_pendingDeliveries, requestToken);
    if (bot == _pendingDeliveries.end())
        return false;

    out = pending->second;
    return true;
}

PlayerbotSocialDeliveryRejection PlayerbotSocialMgr::CompleteDelivery(
    uint64 requestToken, PlayerbotSocialDeliveryConditions const& conditions)
{
    auto [bot, pending] = FindPendingByToken(_pendingDeliveries, requestToken);
    if (bot == _pendingDeliveries.end())
        return PlayerbotSocialDeliveryRejection::UnknownRequest;

    // Nothing to revalidate until the provider has answered.
    if (!pending->second.resultArrived)
        return PlayerbotSocialDeliveryRejection::UnknownRequest;

    PlayerbotSocialDeliveryRejection verdict =
        PlayerbotSocialRevalidateDelivery(pending->second.channel, pending->second.result.kind, conditions);

    Thread* const thread = FindThreadMutable(pending->second.threadPublicId);
    if (verdict == PlayerbotSocialDeliveryRejection::None && !pending->second.statelessDirectReply)
    {
        if (thread == nullptr)
            verdict = PlayerbotSocialDeliveryRejection::SupersededThread;
        else if (!pending->second.replyToEventPublicId.empty() &&
                 std::find(thread->recentEventIds.begin(), thread->recentEventIds.end(),
                           pending->second.replyToEventPublicId) == thread->recentEventIds.end())
            verdict = PlayerbotSocialDeliveryRejection::ReplyParentMismatch;
        else if (pending->second.replyToEventPublicId.empty() &&
                 std::none_of(pending->second.grounding.entries.begin(), pending->second.grounding.entries.end(),
                              [](PlayerbotSocialEvidenceEntry const& entry)
                              { return entry.provenance == PlayerbotSocialEvidenceProvenance::AuthoritativeSource; }))
            verdict = PlayerbotSocialDeliveryRejection::MissingReplyParent;
    }

    if (verdict == PlayerbotSocialDeliveryRejection::None)
        verdict = PlayerbotSocialValidateGroundedProposal(pending->second.result, pending->second.grounding,
                                                          conditions.currentGrounding, pending->second.channel,
                                                          pending->second.expectsAnswer);

    // Combat is revalidated at delivery time because the fight may have started after the request
    // opened. Keep its existing precedence over generated-content validation.
    if (verdict == PlayerbotSocialDeliveryRejection::None && pending->second.authorizedRoleplay &&
        conditions.speakerInCombat)
    {
        verdict = PlayerbotSocialDeliveryRejection::AuthorizedRoleplayInCombat;
    }

    // Every generated line is checked against the worldserver's active progression policy. The
    // prompt reduces bad generations, but the worldserver remains the final delivery authority.
    if (verdict == PlayerbotSocialDeliveryRejection::None)
    {
        for (PlayerbotSocialContentCapability const capability :
             PlayerbotSocialDetectContentCapabilities(pending->second.result.text))
        {
            if (!PlayerbotSocialContentIsAllowed(capability))
            {
                verdict = pending->second.authorizedRoleplay
                              ? PlayerbotSocialDeliveryRejection::LockedRoleplayContent
                              : PlayerbotSocialDeliveryRejection::LockedProgressionContent;
                break;
            }
        }
    }

    if (verdict == PlayerbotSocialDeliveryRejection::None && thread != nullptr && !pending->second.result.text.empty())
    {
        uint64 const lineHash = HashRecentLine(pending->second.result.text);
        if (std::find(thread->recentGeneratedLineHashes.begin(), thread->recentGeneratedLineHashes.end(), lineHash) !=
            thread->recentGeneratedLineHashes.end())
            verdict = PlayerbotSocialDeliveryRejection::DuplicateWording;
        else if (!thread->recentContributionFunctions.empty() &&
                 thread->recentContributionFunctions.back().function == pending->second.result.contribution &&
                 thread->recentContributionFunctions.back().speakerGuidCounter == pending->second.botGuidCounter)
            verdict = PlayerbotSocialDeliveryRejection::DuplicateFunction;

        if (verdict == PlayerbotSocialDeliveryRejection::None)
        {
            thread->recentGeneratedLineHashes.push_back(lineHash);
            if (thread->recentGeneratedLineHashes.size() > PLAYERBOT_SOCIAL_MAX_THREAD_RECENT_LINES)
                thread->recentGeneratedLineHashes.pop_front();

            thread->recentContributionFunctions.push_back(
                {pending->second.botGuidCounter, pending->second.result.contribution});
            if (thread->recentContributionFunctions.size() > PLAYERBOT_SOCIAL_MAX_THREAD_RECENT_LINES)
                thread->recentContributionFunctions.pop_front();
        }
    }

    if (verdict != PlayerbotSocialDeliveryRejection::None && !pending->second.statelessDirectReply)
        RecordEvent(
            PlayerbotSocialMakeDeliverySuppressionEvent(pending->second, verdict, static_cast<uint64>(time(nullptr))));

    /*
     * Consumed either way. A result is delivered once or not at all: leaving a refused one in place
     * would let it be retried into a conversation that has already moved on, which is the stale
     * delivery this whole path exists to prevent.
     */
    bot->second.erase(pending);
    if (bot->second.empty())
        _pendingDeliveries.erase(bot);

    return verdict;
}

std::vector<PlayerbotSocialAbandonedRequest> PlayerbotSocialMgr::ExpireTimedOutRequests(uint64 nowUnixSeconds)
{
    std::vector<PlayerbotSocialAbandonedRequest> expired;

    for (auto bot = _pendingDeliveries.begin(); bot != _pendingDeliveries.end();)
    {
        for (auto pending = bot->second.begin(); pending != bot->second.end();)
        {
            /*
             * Only a request still waiting on its provider can time out. One that already has a
             * result is waiting on its natural delay, which is measured in seconds and is not a
             * failure. ElapsedSeconds floors a backwards clock at zero, so a correction cannot
             * expire every outstanding request at once.
             */
            bool const stillWaiting = !pending->second.resultArrived;
            if (stillWaiting && ElapsedSeconds(nowUnixSeconds, pending->second.requestedAtUnixSeconds) >=
                                    PLAYERBOT_SOCIAL_PROVIDER_TIMEOUT_SECONDS)
            {
                /*
                 * The one failure with no other trace. Nothing was spoken, no result arrived, and the
                 * entry is about to be erased, so without this the feed shows an opportunity and a
                 * selection leading nowhere and nothing says the provider never answered.
                 */
                PlayerbotSocialProviderAttempt attempt;
                attempt.requestToken = pending->second.requestToken;
                attempt.botGuidCounter = pending->second.botGuidCounter;
                attempt.targetGuidCounter = pending->second.targetGuidCounter;
                attempt.channel = pending->second.channel;
                attempt.threadPublicId = pending->second.threadPublicId;
                attempt.zoneId = pending->second.zoneId;
                attempt.occurredAtUnixSeconds = nowUnixSeconds;
                attempt.operatorEvidence = pending->second.operatorEvidence;
                attempt.outcome = PlayerbotSocialProviderAttemptOutcome::Refused;
                attempt.rejection = PlayerbotSocialDeliveryRejection::ProviderTimedOut;
                if (!pending->second.statelessDirectReply)
                    RecordEvent(PlayerbotSocialMakeProviderAttemptEvent(attempt));

                expired.push_back({pending->second.requestToken, pending->second.botGuidCounter,
                                   PlayerbotSocialDeliveryRejection::ProviderTimedOut});
                pending = bot->second.erase(pending);
                continue;
            }

            ++pending;
        }

        bot = bot->second.empty() ? _pendingDeliveries.erase(bot) : std::next(bot);
    }

    return expired;
}

std::vector<PlayerbotSocialAbandonedRequest> PlayerbotSocialMgr::CancelPendingDeliveries()
{
    std::vector<PlayerbotSocialAbandonedRequest> cancelled;

    uint64 const nowUnixSeconds = static_cast<uint64>(time(nullptr));

    for (auto const& [botGuidCounter, requests] : _pendingDeliveries)
    {
        for (auto const& [token, pending] : requests)
        {
            cancelled.push_back({token, botGuidCounter, PlayerbotSocialDeliveryRejection::ShuttingDown});

            /*
             * ONLY a request still waiting on its provider. An entry whose result already arrived is
             * waiting out its natural delay, and its attempt was recorded as `Answered` the moment
             * the provider answered. Reporting it again here would put two attempts under one token
             * and break the once per request contract, which is precisely what this event exists to
             * make countable. What is lost at shutdown for those is the DELIVERY, and there is no
             * delivery event to suppress because nothing was spoken.
             */
            if (pending.resultArrived)
                continue;

            /*
             * A cancellation is a conclusion like any other, so the attempt is reported here too and
             * `PlayerbotSocialProviderAttemptOutcome::Refused` covers shutdown exactly as its comment
             * says. Without this, one of the four ways a request can end was the only one with no row.
             *
             * Whether these rows survive depends on the caller draining afterwards, and no drain is
             * added here for it: this function has NO production caller at the time of writing, only
             * tests. Adding a shutdown flush would be building machinery for a path that never runs.
             * Wiring cancellation, and flushing after it, belongs with whoever gives it a caller.
             */
            PlayerbotSocialProviderAttempt attempt;
            attempt.requestToken = token;
            attempt.botGuidCounter = botGuidCounter;
            attempt.targetGuidCounter = pending.targetGuidCounter;
            attempt.channel = pending.channel;
            attempt.threadPublicId = pending.threadPublicId;
            attempt.zoneId = pending.zoneId;
            attempt.occurredAtUnixSeconds = nowUnixSeconds;
            attempt.operatorEvidence = pending.operatorEvidence;
            attempt.outcome = PlayerbotSocialProviderAttemptOutcome::Refused;
            attempt.rejection = PlayerbotSocialDeliveryRejection::ShuttingDown;
            if (!pending.statelessDirectReply)
                RecordEvent(PlayerbotSocialMakeProviderAttemptEvent(attempt));
        }
    }

    // Values only, so there is nothing to release and nothing to dereference. That is the whole
    // reason a pending delivery never holds a game object.
    _pendingDeliveries.clear();

    return cancelled;
}

std::size_t PlayerbotSocialMgr::PendingDeliveryCount() const
{
    std::size_t total = 0;
    for (auto const& [botGuidCounter, requests] : _pendingDeliveries)
        total += requests.size();

    return total;
}

// Roleplay assessment lifecycle --------------------------------------------------------------------

char const* PlayerbotSocialRoleplayAssessmentDiscardName(PlayerbotSocialRoleplayAssessmentDiscard discard)
{
    switch (discard)
    {
        case PlayerbotSocialRoleplayAssessmentDiscard::None:
            return "none";
        case PlayerbotSocialRoleplayAssessmentDiscard::UnknownToken:
            return "unknown_token";
        case PlayerbotSocialRoleplayAssessmentDiscard::MalformedResult:
            return "malformed_result";
        case PlayerbotSocialRoleplayAssessmentDiscard::StaleThread:
            return "stale_thread";
        case PlayerbotSocialRoleplayAssessmentDiscard::StaleLine:
            return "stale_line";
    }

    return "unknown";
}

PlayerbotSocialMgr::Thread* PlayerbotSocialMgr::FindThreadMutable(std::string const& threadPublicId)
{
    if (!PlayerbotSocialPublicIdIsValid(PlayerbotSocialIdKind::Thread, threadPublicId))
        return nullptr;

    for (auto& scope : _scopes)
        for (Thread& thread : scope.second.threads)
            if (thread.publicId == threadPublicId)
                return &thread;

    return nullptr;
}

PlayerbotSocialRoleplayAssessmentDiscard PlayerbotSocialMgr::AssessmentStaleness(
    PendingRoleplayAssessment const& pending) const
{
    Thread const* const thread = FindThread(pending.threadPublicId);
    if (thread == nullptr)
        return PlayerbotSocialRoleplayAssessmentDiscard::StaleThread;

    /*
     * The assessed line must still be the thread's newest one. A conversation that moved on while
     * the classifier thought gets its assessment dropped, because applying it would answer a line
     * nobody is looking at any more. An assessment issued with no readable line has no anchor to
     * compare, so it stays current as long as its thread does.
     */
    if (pending.currentLineHash != 0 &&
        (thread->recentLineHashes.empty() || thread->recentLineHashes.back() != pending.currentLineHash))
        return PlayerbotSocialRoleplayAssessmentDiscard::StaleLine;

    return PlayerbotSocialRoleplayAssessmentDiscard::None;
}

std::vector<std::string> PlayerbotSocialMgr::AssessmentThreadLines(std::string const& threadPublicId,
                                                                   uint64 nowUnixSeconds) const
{
    Thread const* const thread = FindThread(threadPublicId);
    if (thread == nullptr)
        return {};

    PlayerbotSocialPromptContextConsent const consents = [this](uint64 guid) { return !IsOptedOut(guid); };
    PlayerbotSocialPromptContextSnapshot const snapshot =
        PlayerbotSocialBuildPromptContextSnapshot(thread->promptContext, consents, nowUnixSeconds);
    return PlayerbotSocialRenderPromptThread(snapshot);
}

PlayerbotRoleplayPromptMode PlayerbotSocialMgr::RoleplayPromptModeFor(PlayerbotSocialActivation const& activation,
                                                                      PlayerbotSocialRoleplayDirective const& roleplay,
                                                                      uint64 botGuidCounter,
                                                                      uint8 roleplayAffinity) const
{
    if (!roleplay.roleplayEligible)
        return PlayerbotRoleplayPromptMode::Ordinary;

    bool const invitation = roleplay.kind == PlayerbotRoleplayAssessmentKind::RoleplayInvitation;
    bool const continuation = roleplay.kind == PlayerbotRoleplayAssessmentKind::RoleplayContinuation;
    if (!invitation && !continuation)
        return PlayerbotRoleplayPromptMode::Ordinary;

    switch (PlayerbotRoleplayAffinityBandFor(roleplayAffinity))
    {
        case PlayerbotRoleplayAffinityBand::Averse:
            return PlayerbotRoleplayPromptMode::DeclineRoleplay;
        case PlayerbotRoleplayAffinityBand::Neutral:
            return PlayerbotRoleplayPromptMode::AcknowledgeRoleplay;
        case PlayerbotRoleplayAffinityBand::Receptive:
        case PlayerbotRoleplayAffinityBand::Enthusiast:
            break;
    }

    // A continuation may authorize only a bot an invitation already admitted to this thread.
    if (continuation)
    {
        Thread const* const thread = FindThread(activation.thread.publicId);
        bool const active =
            thread != nullptr && std::find(thread->roleplayParticipants.begin(), thread->roleplayParticipants.end(),
                                           botGuidCounter) != thread->roleplayParticipants.end();
        if (!active)
            return PlayerbotRoleplayPromptMode::AcknowledgeRoleplay;
    }

    uint8 const roll = PlayerbotRoleplayWillingnessRoll(activation.selectionSeed, botGuidCounter);
    return PlayerbotRoleplayWillingnessPasses(roleplayAffinity, roll)
               ? PlayerbotRoleplayPromptMode::AuthorizedRoleplay
               : PlayerbotRoleplayPromptMode::AcknowledgeRoleplay;
}

PlayerbotSocialAssessmentDisposition PlayerbotSocialMgr::AssessAndActivate(PlayerbotSocialActivation const& activation,
                                                                           PlayerbotSocialDensityProfile densityProfile)
{
    PlayerbotSocialAssessmentDisposition disposition;

    /*
     * Every refusal here is an immediate ordinary activation, never a lost opportunity. Capacity
     * refuses new assessments rather than evicting a held one, because an evicted assessment would
     * either answer nothing or answer after its activation was already spent.
     */
    if (_provider == nullptr || !activation.thread.valid ||
        _pendingAssessments.size() >= PLAYERBOT_SOCIAL_MAX_PENDING_BOTS)
    {
        disposition.immediate = Activate(activation, densityProfile);
        return disposition;
    }

    // The classifier sees only what the privacy rules already admitted for prompting: the bounded
    // rendered thread, with unconsented speakers filtered the same way generation context is.
    std::vector<std::string> const threadLines =
        AssessmentThreadLines(activation.thread.publicId, activation.nowUnixSeconds);

    uint64 const token = _nextAssessmentToken++;
    if (!_provider->SubmitRoleplayAssessment(token, activation.thread.publicId, activation.channel,
                                             activation.currentLine.text, threadLines))
    {
        disposition.immediate = Activate(activation, densityProfile);
        return disposition;
    }

    PendingRoleplayAssessment pending;
    pending.activation = activation;
    pending.densityProfile = densityProfile;
    pending.threadPublicId = activation.thread.publicId;
    pending.currentLineHash = activation.currentLine.text.empty() ? 0 : HashRecentLine(activation.currentLine.text);
    pending.issuedAtUnixSeconds = activation.nowUnixSeconds;
    _pendingAssessments.emplace(token, std::move(pending));

    disposition.assessmentPending = true;
    disposition.assessmentToken = token;
    return disposition;
}

PlayerbotSocialAssessmentApplication PlayerbotSocialMgr::ApplyRoleplayAssessment(
    PlayerbotSocialRoleplayAssessmentResult const& result)
{
    PlayerbotSocialAssessmentApplication application;

    auto const found = _pendingAssessments.find(result.assessmentToken);
    if (found == _pendingAssessments.end())
    {
        application.discard = PlayerbotSocialRoleplayAssessmentDiscard::UnknownToken;
        return application;
    }

    // Consumed exactly once, whatever happens next: a result is applied once or not at all.
    PendingRoleplayAssessment const pending = std::move(found->second);
    _pendingAssessments.erase(found);

    PlayerbotSocialRoleplayAssessmentDiscard const staleness = AssessmentStaleness(pending);
    if (staleness != PlayerbotSocialRoleplayAssessmentDiscard::None)
    {
        application.discard = staleness;
        LOG_DEBUG("playerbots", "Social roleplay assessment {} discarded: {}", result.assessmentToken,
                  PlayerbotSocialRoleplayAssessmentDiscardName(staleness));
        return application;
    }

    if (!PlayerbotRoleplayAssessmentKindIsValid(result.kind) ||
        !PlayerbotSocialRoleplayAssessmentShapeIsValid(result.kind, result.capabilities))
    {
        /*
         * Malformed output fails to ordinary social behavior rather than to silence: the
         * opportunity itself is still current and is owed its ordinary decision. Nothing malformed
         * may ever upgrade to roleplay.
         */
        application.discard = PlayerbotSocialRoleplayAssessmentDiscard::MalformedResult;
        application.activated = true;
        application.activation = Activate(pending.activation, pending.densityProfile);
        return application;
    }

    /*
     * The worldserver decision. The assessment is evidence; what follows from it is decided here
     * against the thread's transient roleplay state and the active progression policy, and every
     * refused or unsupported state falls to the ordinary directive rather than to authorization.
     */
    PlayerbotSocialRoleplayDirective directive;
    directive.kind = result.kind;

    uint64 const speaker = pending.activation.speakerGuidCounter;

    switch (result.kind)
    {
        case PlayerbotRoleplayAssessmentKind::Practical:
            // Practical chat exits roleplay immediately. It does not block a later invitation.
            ClearRoleplayParticipants(pending.threadPublicId);
            break;
        case PlayerbotRoleplayAssessmentKind::OptOut:
            // An explicit stop clears the roleplay AND suppresses it for this human until the
            // thread is pruned. No durable preference is stored anywhere.
            ClearRoleplayParticipants(pending.threadPublicId);
            NoteRoleplayOptOut(pending.threadPublicId, speaker);
            break;
        case PlayerbotRoleplayAssessmentKind::RoleplayInvitation:
        case PlayerbotRoleplayAssessmentKind::RoleplayContinuation:
        {
            if (IsRoleplayOptedOut(pending.threadPublicId, speaker))
                break;  // While opted out, every result for this human resolves to ordinary.

            /*
             * The sidecar's evidence is unioned with the worldserver's own indicator scan of the
             * admitted text, so a classifier that omits a protected subject cannot authorize it.
             * The whole premise is all or nothing: one locked, unknown, or contradictory item in
             * the union refuses roleplay entirely.
             */
            std::vector<PlayerbotSocialContentCapability> premise = result.capabilities;

            auto const unionDetected = [&premise](std::string const& text)
            {
                for (PlayerbotSocialContentCapability const capability : PlayerbotSocialDetectContentCapabilities(text))
                    if (std::find(premise.begin(), premise.end(), capability) == premise.end())
                        premise.push_back(capability);
            };

            unionDetected(pending.activation.currentLine.text);
            for (std::string const& line :
                 AssessmentThreadLines(pending.threadPublicId, pending.activation.nowUnixSeconds))
                unionDetected(line);

            directive.roleplayEligible = PlayerbotSocialContentIsAllowed(premise);
            if (!directive.roleplayEligible)
                LOG_DEBUG("playerbots",
                          "Social roleplay premise refused by the active progression policy for "
                          "thread {} ({} capabilities in the union)",
                          pending.threadPublicId, premise.size());
            break;
        }
        case PlayerbotRoleplayAssessmentKind::Ordinary:
        case PlayerbotRoleplayAssessmentKind::Uncertain:
            break;
    }

    application.activated = true;
    application.activation = Activate(pending.activation, pending.densityProfile, directive);
    return application;
}

std::vector<uint64> PlayerbotSocialMgr::ExpireTimedOutAssessments(uint64 nowUnixSeconds)
{
    std::vector<uint64> expired;

    for (auto it = _pendingAssessments.begin(); it != _pendingAssessments.end();)
    {
        uint64 const issuedAt = it->second.issuedAtUnixSeconds;

        // A rewound clock reads as nothing elapsed rather than as an enormous interval.
        bool const timedOut =
            nowUnixSeconds > issuedAt && nowUnixSeconds - issuedAt > PLAYERBOT_SOCIAL_PROVIDER_TIMEOUT_SECONDS;
        if (!timedOut)
        {
            ++it;
            continue;
        }

        PendingRoleplayAssessment const pending = std::move(it->second);
        expired.push_back(it->first);
        it = _pendingAssessments.erase(it);

        /*
         * A silent sidecar costs latency, never a conversation: the held activation resumes in
         * ordinary mode as long as its thread and line are still current. A superseded opportunity
         * is dropped, because answering it now would be a non sequitur.
         */
        if (AssessmentStaleness(pending) == PlayerbotSocialRoleplayAssessmentDiscard::None)
            Activate(pending.activation, pending.densityProfile);
    }

    return expired;
}

std::vector<uint64> PlayerbotSocialMgr::CancelPendingAssessments()
{
    std::vector<uint64> cancelled;
    cancelled.reserve(_pendingAssessments.size());
    for (auto const& [token, pending] : _pendingAssessments)
        cancelled.push_back(token);

    _pendingAssessments.clear();
    return cancelled;
}

bool PlayerbotSocialMgr::NoteRoleplayParticipant(std::string const& threadPublicId, uint64 botGuidCounter)
{
    Thread* const thread = FindThreadMutable(threadPublicId);
    if (thread == nullptr || botGuidCounter == 0)
        return false;

    if (std::find(thread->roleplayParticipants.begin(), thread->roleplayParticipants.end(), botGuidCounter) !=
        thread->roleplayParticipants.end())
        return true;

    thread->roleplayParticipants.push_back(botGuidCounter);
    if (thread->roleplayParticipants.size() > PLAYERBOT_SOCIAL_MAX_ROLEPLAY_PARTICIPANTS)
        thread->roleplayParticipants.pop_front();

    return true;
}

std::vector<uint64> PlayerbotSocialMgr::RoleplayParticipants(std::string const& threadPublicId) const
{
    Thread const* const thread = FindThread(threadPublicId);
    if (thread == nullptr)
        return {};

    return std::vector<uint64>(thread->roleplayParticipants.begin(), thread->roleplayParticipants.end());
}

void PlayerbotSocialMgr::ClearRoleplayParticipants(std::string const& threadPublicId)
{
    if (Thread* const thread = FindThreadMutable(threadPublicId))
        thread->roleplayParticipants.clear();
}

bool PlayerbotSocialMgr::NoteRoleplayOptOut(std::string const& threadPublicId, uint64 humanGuidCounter)
{
    Thread* const thread = FindThreadMutable(threadPublicId);
    if (thread == nullptr || humanGuidCounter == 0)
        return false;

    if (std::find(thread->roleplayOptOuts.begin(), thread->roleplayOptOuts.end(), humanGuidCounter) !=
        thread->roleplayOptOuts.end())
        return true;

    thread->roleplayOptOuts.push_back(humanGuidCounter);
    if (thread->roleplayOptOuts.size() > PLAYERBOT_SOCIAL_MAX_ROLEPLAY_OPTOUTS)
        thread->roleplayOptOuts.pop_front();

    return true;
}

bool PlayerbotSocialMgr::IsRoleplayOptedOut(std::string const& threadPublicId, uint64 humanGuidCounter) const
{
    Thread const* const thread = FindThread(threadPublicId);
    return thread != nullptr && std::find(thread->roleplayOptOuts.begin(), thread->roleplayOptOuts.end(),
                                          humanGuidCounter) != thread->roleplayOptOuts.end();
}
