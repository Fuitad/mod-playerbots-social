/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#include "Bot/Social/PlayerbotSocialRepository.h"

#include <algorithm>
#include <cctype>
#include <limits>
#include <string_view>

#include "Bot/Social/PlayerbotSocialPersonality.h"
#include "CryptoRandom.h"
#include "StringConvert.h"

namespace
{
/*
 * Written as a positive range test rather than as its negation, because a NaN compares false
 * against every bound. Asking "is it out of range" would answer no for a NaN and let it through.
 */
[[nodiscard]] bool IsWithinRange(float value, float minimum, float maximum)
{
    return value >= minimum && value <= maximum;
}

[[nodiscard]] std::string ToLowercase(std::string const& text)
{
    std::string lowered;
    lowered.reserve(text.size());
    for (char const symbol : text)
        lowered.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(symbol))));

    return lowered;
}

[[nodiscard]] bool ContainsAny(std::string const& lowered, std::initializer_list<std::string_view> markers)
{
    for (std::string_view const marker : markers)
    {
        if (lowered.find(marker) != std::string::npos)
            return true;
    }

    return false;
}

}  // namespace

std::optional<uint64> PlayerbotSocialParseStoredUnsigned(std::string_view text)
{
    return Acore::StringTo<uint64>(text);
}

/*
 * Real world secrets a character might type into chat. Durable memory is paraphrased game
 * knowledge, so none of these belong in it regardless of who volunteered them.
 */
bool PlayerbotSocialTextLooksSensitive(std::string const& text)
{
    return ContainsAny(ToLowercase(text), {"password", "passwd", "credit card", "social security", "bank account",
                                           "phone number", "email address", "real name is"});
}

bool PlayerbotSocialTextLooksLikeAnInstruction(std::string const& text)
{
    return ContainsAny(ToLowercase(text),
                       {"ignore previous", "ignore all previous", "disregard previous", "you are now",
                        "new instructions", "override your", "system:", "assistant:"});
}

void PlayerbotSocialRelationshipStore::Remember(PlayerbotSocialRelationshipKey const& key,
                                                PlayerbotSocialRelationshipValues const& values)
{
    // Clamped here rather than at the call sites, so no write path can forget it.
    _relationships[key] = PlayerbotSocialClampRelationship(values);
}

PlayerbotSocialRelationshipValues PlayerbotSocialRelationshipStore::Recall(
    PlayerbotSocialRelationshipKey const& key) const
{
    auto const found = _relationships.find(key);
    if (found == _relationships.end())
        return PlayerbotSocialRelationshipValues{};

    return found->second;
}

std::vector<PlayerbotSocialWarmRelationship> PlayerbotSocialRelationshipStore::WarmRelationships(
    float minFamiliarity, std::size_t limit) const
{
    std::vector<PlayerbotSocialWarmRelationship> warm;

    for (auto const& [key, values] : _relationships)
    {
        if (warm.size() >= limit)
            break;

        if (values.familiarity >= minFamiliarity)
            warm.push_back({key, values});
    }

    return warm;
}

std::size_t PlayerbotSocialRelationshipStore::TrackedRelationshipCount() const { return _relationships.size(); }

std::size_t PlayerbotSocialRelationshipStore::Forget(uint64 characterGuidCounter)
{
    std::size_t removed = 0;
    for (auto entry = _relationships.begin(); entry != _relationships.end();)
    {
        if (entry->first.botGuidCounter == characterGuidCounter ||
            entry->first.subjectGuidCounter == characterGuidCounter)
        {
            entry = _relationships.erase(entry);
            ++removed;
            continue;
        }

        ++entry;
    }

    return removed;
}

bool PlayerbotSocialRelationshipStore::ForgetPairMatching(PlayerbotSocialRelationshipMatch const& matches)
{
    if (!matches)
        return false;

    for (auto entry = _relationships.begin(); entry != _relationships.end(); ++entry)
    {
        if (matches(entry->first))
        {
            _relationships.erase(entry);
            return true;
        }
    }

    return false;
}

std::size_t PlayerbotSocialRelationshipStore::ForgetOwnedByAnyOf(std::set<uint64> const& ownerGuidCounters)
{
    std::size_t removed = 0;
    for (auto entry = _relationships.begin(); entry != _relationships.end();)
    {
        if (ownerGuidCounters.find(entry->first.botGuidCounter) != ownerGuidCounters.end())
        {
            entry = _relationships.erase(entry);
            ++removed;
            continue;
        }

        ++entry;
    }

    return removed;
}

char const* PlayerbotSocialMemoryRejectionName(PlayerbotSocialMemoryRejection rejection)
{
    switch (rejection)
    {
        case PlayerbotSocialMemoryRejection::None:
            return "none";
        case PlayerbotSocialMemoryRejection::MissingOwner:
            return "missing_owner";
        case PlayerbotSocialMemoryRejection::UnknownCategory:
            return "unknown_category";
        case PlayerbotSocialMemoryRejection::UnknownProvenance:
            return "unknown_provenance";
        case PlayerbotSocialMemoryRejection::UnknownPrivacyScope:
            return "unknown_privacy_scope";
        case PlayerbotSocialMemoryRejection::EmptyContent:
            return "empty_content";
        case PlayerbotSocialMemoryRejection::ContentTooLong:
            return "content_too_long";
        case PlayerbotSocialMemoryRejection::ConfidenceOutOfRange:
            return "confidence_out_of_range";
        case PlayerbotSocialMemoryRejection::SignificanceOutOfRange:
            return "significance_out_of_range";
        case PlayerbotSocialMemoryRejection::SensitiveContent:
            return "sensitive_content";
        case PlayerbotSocialMemoryRejection::InstructionLikeContent:
            return "instruction_like_content";
        case PlayerbotSocialMemoryRejection::MissingSourceEvent:
            return "missing_source_event";
        case PlayerbotSocialMemoryRejection::MalformedSourceEvent:
            return "malformed_source_event";
        case PlayerbotSocialMemoryRejection::MissingSourceThread:
            return "missing_source_thread";
        case PlayerbotSocialMemoryRejection::MalformedSourceThread:
            return "malformed_source_thread";
        case PlayerbotSocialMemoryRejection::UnknownSourceKind:
            return "unknown_source_kind";
        case PlayerbotSocialMemoryRejection::GeneratedSource:
            return "generated_source";
        case PlayerbotSocialMemoryRejection::CharacterOptedOut:
            return "character_opted_out";
        case PlayerbotSocialMemoryRejection::UnresolvedActor:
            return "unresolved_actor";
    }

    return "unknown";
}

PlayerbotSocialMemoryRejection PlayerbotSocialValidateMemoryCandidate(PlayerbotSocialMemoryRecord const& record)
{
    // A memory with no owner belongs to no bot and could never be retrieved, but would still occupy
    // a row and still carry whatever text it was built with.
    if (record.botGuidCounter == 0)
        return PlayerbotSocialMemoryRejection::MissingOwner;

    if (!PlayerbotSocialMemoryCategoryIsValid(record.category))
        return PlayerbotSocialMemoryRejection::UnknownCategory;

    if (!PlayerbotSocialMemoryProvenanceIsValid(record.provenance))
        return PlayerbotSocialMemoryRejection::UnknownProvenance;

    if (!PlayerbotSocialPrivacyScopeIsValid(record.scope))
        return PlayerbotSocialMemoryRejection::UnknownPrivacyScope;

    if (record.sourceEventPublicId.empty())
        return PlayerbotSocialMemoryRejection::MissingSourceEvent;

    if (!PlayerbotSocialPublicIdIsValid(PlayerbotSocialIdKind::Event, record.sourceEventPublicId))
        return PlayerbotSocialMemoryRejection::MalformedSourceEvent;

    if (record.sourceThreadPublicId.empty())
        return PlayerbotSocialMemoryRejection::MissingSourceThread;

    if (!PlayerbotSocialPublicIdIsValid(PlayerbotSocialIdKind::Thread, record.sourceThreadPublicId))
        return PlayerbotSocialMemoryRejection::MalformedSourceThread;

    if (!record.sourceKind.has_value() || !PlayerbotSocialMemorySourceKindIsValid(*record.sourceKind))
        return PlayerbotSocialMemoryRejection::UnknownSourceKind;

    if (*record.sourceKind == PlayerbotSocialMemorySourceKind::GeneratedDelivery)
        return PlayerbotSocialMemoryRejection::GeneratedSource;

    if (record.paraphrase.empty())
        return PlayerbotSocialMemoryRejection::EmptyContent;

    if (record.paraphrase.size() > PLAYERBOT_SOCIAL_MAX_MEMORY_CONTENT_LENGTH)
        return PlayerbotSocialMemoryRejection::ContentTooLong;

    if (!IsWithinRange(record.confidence, PLAYERBOT_SOCIAL_CONFIDENCE_MIN, PLAYERBOT_SOCIAL_CONFIDENCE_MAX))
        return PlayerbotSocialMemoryRejection::ConfidenceOutOfRange;

    if (!IsWithinRange(record.significance, PLAYERBOT_SOCIAL_SIGNIFICANCE_MIN, PLAYERBOT_SOCIAL_SIGNIFICANCE_MAX))
        return PlayerbotSocialMemoryRejection::SignificanceOutOfRange;

    if (PlayerbotSocialTextLooksSensitive(record.paraphrase))
        return PlayerbotSocialMemoryRejection::SensitiveContent;

    if (PlayerbotSocialTextLooksLikeAnInstruction(record.paraphrase))
        return PlayerbotSocialMemoryRejection::InstructionLikeContent;

    return PlayerbotSocialMemoryRejection::None;
}

PlayerbotSocialMemoryRejection PlayerbotSocialMemoryStore::Remember(PlayerbotSocialMemoryRecord const& record)
{
    PlayerbotSocialMemoryRejection const rejection = PlayerbotSocialValidateMemoryCandidate(record);
    if (rejection != PlayerbotSocialMemoryRejection::None)
        return rejection;

    _memories.push_back(record);
    return PlayerbotSocialMemoryRejection::None;
}

std::vector<PlayerbotSocialMemoryRecord> PlayerbotSocialMemoryStore::Recall(PlayerbotSocialRelationshipKey const& key,
                                                                            PlayerbotSocialChannel channel) const
{
    std::vector<PlayerbotSocialMemoryRecord> recalled;

    // An unrecognized channel is rejected outright rather than resolved to a scope. Resolution
    // already fails closed to Public, but that would still let public facts through for a value
    // that named no real surface to say them on.
    if (!PlayerbotSocialChannelIsValid(channel))
        return recalled;

    for (auto const& record : _memories)
    {
        if (record.botGuidCounter != key.botGuidCounter || record.subjectGuidCounter != key.subjectGuidCounter)
            continue;

        if (!PlayerbotSocialMemoryIsRetrievableInChannel(record.scope, channel))
            continue;

        recalled.push_back(record);
    }

    return recalled;
}

std::size_t PlayerbotSocialMemoryStore::StoredMemoryCount() const { return _memories.size(); }

std::size_t PlayerbotSocialMemoryStore::Forget(uint64 characterGuidCounter)
{
    std::size_t const before = _memories.size();
    _memories.erase(std::remove_if(_memories.begin(), _memories.end(),
                                   [characterGuidCounter](PlayerbotSocialMemoryRecord const& record)
                                   {
                                       return record.botGuidCounter == characterGuidCounter ||
                                              record.subjectGuidCounter == characterGuidCounter;
                                   }),
                    _memories.end());

    return before - _memories.size();
}

std::size_t PlayerbotSocialMemoryStore::ForgetOwnedByAnyOf(std::set<uint64> const& ownerGuidCounters)
{
    std::size_t const before = _memories.size();
    _memories.erase(
        std::remove_if(_memories.begin(), _memories.end(),
                       [&ownerGuidCounters](PlayerbotSocialMemoryRecord const& record)
                       { return ownerGuidCounters.find(record.botGuidCounter) != ownerGuidCounters.end(); }),
        _memories.end());

    return before - _memories.size();
}

bool PlayerbotSocialMemoryStore::ForgetMatching(PlayerbotSocialMemoryMatch const& matches)
{
    if (!matches)
        return false;

    for (auto record = _memories.begin(); record != _memories.end(); ++record)
    {
        if (matches(*record))
        {
            _memories.erase(record);
            return true;
        }
    }

    return false;
}

std::size_t PlayerbotSocialMemoryStore::ForgetOwnedByAnyOfWithinScope(
    std::set<uint64> const& ownerGuidCounters, PlayerbotSocialMemoryScopeQuery query,
    PlayerbotSocialMemoryVisibility const& visibleToReader)
{
    // An absent predicate would mean "every record", which is exactly the assumption this parameter
    // exists to stop being made silently. Removing nothing is the fail closed answer.
    if (!visibleToReader)
        return 0;

    std::size_t const before = _memories.size();

    _memories.erase(
        std::remove_if(_memories.begin(), _memories.end(),
                       [&ownerGuidCounters, query, &visibleToReader](PlayerbotSocialMemoryRecord const& record)
                       {
                           return ownerGuidCounters.find(record.botGuidCounter) != ownerGuidCounters.end() &&
                                  PlayerbotSocialMemoryScopeIsWithinQuery(record.scope, query) &&
                                  visibleToReader(record);
                       }),
        _memories.end());

    return before - _memories.size();
}

void PlayerbotSocialStateStore::SetOptedOut(uint64 characterGuidCounter, bool optedOut)
{
    if (optedOut)
        _optedOut.insert(characterGuidCounter);
    else
        _optedOut.erase(characterGuidCounter);
}

bool PlayerbotSocialStateStore::IsOptedOut(uint64 characterGuidCounter) const
{
    return _optedOut.find(characterGuidCounter) != _optedOut.end();
}

/*
 * Both ends must take part. The subject's consent is the obvious one, but the bot's own matters too:
 * a bot excluded from the feature must stop forming opinions rather than form them where nothing can
 * read them back.
 */
bool PlayerbotSocialStateStore::PairParticipates(PlayerbotSocialRelationshipKey const& key) const
{
    return !IsOptedOut(key.botGuidCounter) && !IsOptedOut(key.subjectGuidCounter);
}

bool PlayerbotSocialStateStore::RememberRelationship(PlayerbotSocialRelationshipKey const& key,
                                                     PlayerbotSocialRelationshipValues const& values)
{
    if (!PairParticipates(key))
        return false;

    _relationships.Remember(key, values);
    return true;
}

PlayerbotSocialRelationshipValues PlayerbotSocialStateStore::RecallRelationship(
    PlayerbotSocialRelationshipKey const& key) const
{
    // The stranger baseline, not the stored value. A caller that skipped the consent check would
    // otherwise be handed the opinion it is not allowed to have.
    if (!PairParticipates(key))
        return PlayerbotSocialRelationshipValues{};

    return _relationships.Recall(key);
}

std::vector<PlayerbotSocialWarmRelationship> PlayerbotSocialStateStore::WarmRelationships(float minFamiliarity,
                                                                                          std::size_t limit) const
{
    std::vector<PlayerbotSocialWarmRelationship> warm;

    for (PlayerbotSocialWarmRelationship const& candidate : _relationships.WarmRelationships(minFamiliarity, limit))
        if (PairParticipates(candidate.key))
            warm.push_back(candidate);

    return warm;
}

PlayerbotSocialMemoryRejection PlayerbotSocialStateStore::RememberMemory(PlayerbotSocialMemoryRecord const& record)
{
    if (!PairParticipates({record.botGuidCounter, record.subjectGuidCounter}))
        return PlayerbotSocialMemoryRejection::CharacterOptedOut;

    return _memories.Remember(record);
}

std::size_t PlayerbotSocialStateStore::ReplaceMemoriesOwnedBy(uint64 botGuidCounter,
                                                              PlayerbotSocialMemoryScopeQuery query,
                                                              std::vector<PlayerbotSocialMemoryRecord> const& records,
                                                              PlayerbotSocialMemoryVisibility const& visibleToReader)
{
    if (botGuidCounter == 0 || !visibleToReader)
        return 0;

    _memories.ForgetOwnedByAnyOfWithinScope({botGuidCounter}, query, visibleToReader);

    std::size_t accepted = 0;
    for (PlayerbotSocialMemoryRecord const& record : records)
    {
        // The same bound as the removal above, applied to the other half. A record the reader could
        // not see had nothing removed for it, so accepting it would turn the replacement into an
        // append. In practice a reader never supplies one, because it skipped that row on the way in.
        if (!visibleToReader(record))
            continue;

        // Ownership is not taken on trust: a record naming a different bot would otherwise survive
        // a replacement that was supposed to be scoped to this one.
        if (record.botGuidCounter != botGuidCounter)
            continue;

        // Nor is scope. A record outside what the read covered would be added without the matching
        // removal having been made for it, which is how a replacement turns back into an append.
        if (!PlayerbotSocialMemoryScopeIsWithinQuery(record.scope, query))
            continue;

        // Still routed through the consent aware path, so a snapshot cannot reinstate memories about
        // a character who opted out between the read being issued and it landing.
        if (RememberMemory(record) == PlayerbotSocialMemoryRejection::None)
            ++accepted;
    }

    return accepted;
}

std::vector<PlayerbotSocialMemoryRecord> PlayerbotSocialStateStore::RecallMemories(
    PlayerbotSocialRelationshipKey const& key, PlayerbotSocialChannel channel) const
{
    if (!PairParticipates(key))
        return {};

    return _memories.Recall(key, channel);
}

std::size_t PlayerbotSocialStateStore::ResetCharacter(uint64 characterGuidCounter)
{
    return _relationships.Forget(characterGuidCounter) + _memories.Forget(characterGuidCounter);
}

bool PlayerbotSocialStateStore::ForgetRelationshipPairMatching(PlayerbotSocialRelationshipMatch const& matches)
{
    return _relationships.ForgetPairMatching(matches);
}

bool PlayerbotSocialStateStore::ForgetMemoryMatching(PlayerbotSocialMemoryMatch const& matches)
{
    return _memories.ForgetMatching(matches);
}

std::size_t PlayerbotSocialStateStore::ForgetBotCohort(std::vector<uint64> const& botGuidCounters)
{
    // An empty cohort selected nobody. Deleting everything would be the opposite of that, and it is
    // the shape a bulk delete fails into when a selection step silently returns nothing.
    if (botGuidCounters.empty())
        return 0;

    std::set<uint64> const owners(botGuidCounters.begin(), botGuidCounters.end());
    return _relationships.ForgetOwnedByAnyOf(owners) + _memories.ForgetOwnedByAnyOf(owners);
}

std::size_t PlayerbotSocialStateStore::TrackedRelationshipCount() const
{
    return _relationships.TrackedRelationshipCount();
}

std::size_t PlayerbotSocialStateStore::StoredMemoryCount() const { return _memories.StoredMemoryCount(); }

namespace
{
// Distinct from the thread and actor namespaces, so one identity is never derivable from another.
constexpr uint64 SOCIAL_EVENT_ID_NAMESPACE = 0x4556454E544944ULL;
}  // namespace

std::string PlayerbotSocialMakeEventPublicId(uint64 sequence, uint64 salt)
{
    static constexpr char HEX[] = "0123456789abcdef";

    std::array<uint8, 16> const entropy = Acore::Crypto::GetRandomBytes<16>();
    uint64 randomHigh = 0;
    uint64 randomLow = 0;
    for (std::size_t index = 0; index < 8; ++index)
    {
        randomHigh = (randomHigh << 8) | entropy[index];
        randomLow = (randomLow << 8) | entropy[index + 8];
    }

    uint64 const high = PlayerbotPersonality::SplitMix64(sequence ^ SOCIAL_EVENT_ID_NAMESPACE ^ randomHigh);
    uint64 const low = PlayerbotPersonality::SplitMix64(high ^ salt ^ randomLow);

    std::string_view const prefix = PlayerbotSocialPublicIdPrefix(PlayerbotSocialIdKind::Event);
    std::string text;
    text.reserve(prefix.size() + 1 + PLAYERBOT_SOCIAL_PUBLIC_ID_BODY_LENGTH);
    text.append(prefix);
    text.push_back('_');

    for (uint64 word : {high, low})
        for (int shift = 60; shift >= 0; shift -= 4)
            text.push_back(HEX[(word >> shift) & 0xF]);

    return text;
}

bool PlayerbotSocialBuildEventBinding(PlayerbotSocialEventDraft const& draft, PlayerbotSocialEventBinding& binding)
{
    /*
     * Every refusal below is a value MySQL would otherwise accept and store as something meaningless
     * under a non strict mode. The check belongs here rather than at each producer because this is
     * the only way to obtain the fields the statement binds.
     */
    std::string_view const origin = PlayerbotSocialEventOriginName(draft.origin);
    if (origin.empty())
        return false;

    std::string_view const outcome = PlayerbotSocialEventOutcomeName(draft.outcome);
    if (outcome.empty())
        return false;

    if (draft.eventType.empty() || draft.eventType.size() > PLAYERBOT_SOCIAL_EVENT_TYPE_MAX_LENGTH)
        return false;

    // Absent is legitimate: assistance, PVP and control events belong to no conversation. Malformed
    // is not, because it would join to nothing while looking correlated.
    if (!draft.threadPublicId.empty() &&
        !PlayerbotSocialPublicIdIsValid(PlayerbotSocialIdKind::Thread, draft.threadPublicId))
        return false;

    if (!draft.eventPublicId.empty() &&
        !PlayerbotSocialPublicIdIsValid(PlayerbotSocialIdKind::Event, draft.eventPublicId))
        return false;

    if (!draft.replyToEventPublicId.empty() &&
        !PlayerbotSocialPublicIdIsValid(PlayerbotSocialIdKind::Event, draft.replyToEventPublicId))
        return false;

    if (!draft.sourceEventPublicId.empty() &&
        !PlayerbotSocialPublicIdIsValid(PlayerbotSocialIdKind::Event, draft.sourceEventPublicId))
        return false;

    if (draft.eventType == "social.delivery" && draft.origin == PlayerbotSocialEventOrigin::Social &&
        draft.replyToEventPublicId.empty() == draft.sourceEventPublicId.empty())
        return false;

    std::string channel;
    if (draft.hasChannel)
    {
        if (!PlayerbotSocialChannelIsValid(draft.channel))
            return false;

        channel = PlayerbotSocialChannelName(draft.channel);
        if (channel.empty())
            return false;
    }

    binding = PlayerbotSocialEventBinding();
    binding.publicId = draft.eventPublicId.empty()
                           ? PlayerbotSocialMakeEventPublicId(draft.eventSequence, draft.botGuidCounter)
                           : draft.eventPublicId;
    binding.threadPublicId = draft.threadPublicId;
    binding.replyToEventPublicId = draft.replyToEventPublicId;
    binding.sourceEventPublicId = draft.sourceEventPublicId;
    binding.eventType = draft.eventType;
    binding.origin = std::string(origin);
    binding.outcome = std::string(outcome);
    binding.channel = std::move(channel);
    binding.hasChannel = draft.hasChannel;
    binding.botGuidCounter = draft.botGuidCounter;
    binding.actorGuidCounter = draft.actorGuidCounter;
    binding.targetGuidCounter = draft.targetGuidCounter;
    binding.zoneId = draft.zoneId;
    binding.occurredAtUnixSeconds = draft.occurredAtUnixSeconds;

    binding.reason = draft.reason;
    if (binding.reason.size() > PLAYERBOT_SOCIAL_EVENT_REASON_MAX_LENGTH)
        binding.reason.resize(PLAYERBOT_SOCIAL_EVENT_REASON_MAX_LENGTH);

    binding.diagnosticsJson = draft.diagnosticsJson;

    /*
     * Only a delivered event keeps its text. This is the one table that retains raw chat, and a line
     * that was suppressed, failed, or merely recorded was never heard by anyone: storing it would put
     * words into the feed and into retention that no player ever saw.
     */
    if (draft.outcome == PlayerbotSocialEventOutcome::Delivered)
    {
        binding.messageText = draft.messageText;

        // Truncated rather than refused. Losing the tail of a line that WAS delivered beats losing
        // the whole event and leaving a gap where a real conversation happened.
        if (binding.messageText.size() > PLAYERBOT_SOCIAL_EVENT_TEXT_MAX_LENGTH)
            binding.messageText.resize(PLAYERBOT_SOCIAL_EVENT_TEXT_MAX_LENGTH);
    }

    return true;
}

namespace
{
// Absent and unresolvable are the same answer to the statement: a null column. A guid counter of
// zero names nobody, and one the manager has never resolved cannot be named yet.
void ResolveActor(uint64 guidCounter, std::map<uint64, uint32> const& actorIds, uint32& actorId, bool& resolved)
{
    actorId = 0;
    resolved = false;

    if (guidCounter == 0)
        return;

    auto const found = actorIds.find(guidCounter);
    if (found == actorIds.end())
        return;

    actorId = found->second;
    resolved = true;
}
}  // namespace

PlayerbotSocialEventRow PlayerbotSocialBuildEventRow(PlayerbotSocialEventBinding const& binding,
                                                     std::map<uint64, uint32> const& actorIds,
                                                     uint32 configuredRetentionHours)
{
    PlayerbotSocialEventRow row;

    ResolveActor(binding.actorGuidCounter, actorIds, row.actorId, row.hasActor);
    ResolveActor(binding.targetGuidCounter, actorIds, row.targetActorId, row.hasTargetActor);
    ResolveActor(binding.botGuidCounter, actorIds, row.botActorId, row.hasBotActor);

    row.zoneId = binding.zoneId;
    row.hasZone = binding.zoneId != 0;

    /*
     * Computed once, here, and stored on the row. The purge honours what each row was written with
     * rather than recomputing the policy, so a row keeps the window it was created under even if the
     * configuration changes later.
     */
    row.expiresAtUnixSeconds = PlayerbotSocialEventExpiry(binding.occurredAtUnixSeconds, configuredRetentionHours);

    return row;
}

bool PlayerbotSocialEventPersistenceTracker::Prepare(std::vector<PlayerbotSocialEventBinding>& rows,
                                                     uint64 gapEventSequence, uint64 nowUnixSeconds)
{
    if (_inFlight || (rows.empty() && _lostRows == 0))
        return false;

    _inFlightReportsLoss = _lostRows > 0;
    if (_inFlightReportsLoss)
    {
        PlayerbotSocialEventDraft gap;
        gap.eventSequence = gapEventSequence;
        gap.eventType = std::string(PLAYERBOT_SOCIAL_EVENT_TYPE_GAP);
        gap.origin = PlayerbotSocialEventOrigin::System;
        gap.outcome = PlayerbotSocialEventOutcome::Failed;
        gap.reason = std::string(PLAYERBOT_SOCIAL_EVENT_REASON_PERSISTENCE_FAILURE);
        gap.diagnosticsJson = "{\"lost_events\":" + std::to_string(_lostRows) + "}";
        gap.occurredAtUnixSeconds = nowUnixSeconds;
        gap.priority = PlayerbotSocialEventPriority::Critical;

        PlayerbotSocialEventBinding binding;
        if (!PlayerbotSocialBuildEventBinding(gap, binding))
        {
            AddLostRows(static_cast<uint64>(rows.size()));
            _inFlightReportsLoss = false;
            return false;
        }

        rows.push_back(std::move(binding));
    }

    _inFlight = true;
    _inFlightRows = static_cast<uint64>(rows.size());
    return true;
}

void PlayerbotSocialEventPersistenceTracker::Complete(bool success)
{
    if (!_inFlight)
        return;

    if (!success)
        AddLostRows(_inFlightRows);
    else if (_inFlightReportsLoss)
        _lostRows = 0;

    _inFlight = false;
    _inFlightReportsLoss = false;
    _inFlightRows = 0;
}

void PlayerbotSocialEventPersistenceTracker::AddLostRows(uint64 count)
{
    uint64 const remaining = std::numeric_limits<uint64>::max() - _lostRows;
    _lostRows += std::min(count, remaining);
}

PlayerbotSocialEventQueue::PlayerbotSocialEventQueue(std::size_t capacity) : _capacity(capacity) {}

PlayerbotSocialEventQueueResult PlayerbotSocialEventQueue::Push(PlayerbotSocialEventDraft const& draft)
{
    Entry entry;
    if (!PlayerbotSocialBuildEventBinding(draft, entry.binding))
        return PlayerbotSocialEventQueueResult::Refused;

    entry.priority = draft.priority;

    bool evicted = false;
    if (_pending.size() >= _capacity)
    {
        /*
         * The oldest entry in the lowest occupied tier, and only if that tier is strictly below the
         * incoming event. `min_element` returns the FIRST minimum, which is the oldest one because
         * the vector is in arrival order: pressure costs the stalest record rather than an arbitrary
         * one.
         */
        auto const weakest =
            std::min_element(_pending.begin(), _pending.end(),
                             [](Entry const& left, Entry const& right) { return left.priority < right.priority; });

        // Nothing already accepted is displaced by something no more important than itself, so a
        // flood of diagnostics cannot evict each other into an empty feed.
        if (weakest == _pending.end() || !(weakest->priority < entry.priority))
        {
            ++_lost;
            return PlayerbotSocialEventQueueResult::Dropped;
        }

        _pending.erase(weakest);
        ++_lost;
        evicted = true;
    }

    _pending.push_back(std::move(entry));
    return evicted ? PlayerbotSocialEventQueueResult::QueuedAfterEviction : PlayerbotSocialEventQueueResult::Queued;
}

std::vector<PlayerbotSocialEventBinding> PlayerbotSocialEventQueue::Drain(uint64 gapEventSequence,
                                                                          uint64 nowUnixSeconds)
{
    std::vector<PlayerbotSocialEventBinding> drained;
    drained.reserve(_pending.size() + 1);

    for (Entry& entry : _pending)
        drained.push_back(std::move(entry.binding));

    _pending.clear();

    if (_lost > 0)
    {
        /*
         * One marker for the window that just closed, appended last and stamped now: by this point in
         * the feed, this many events had been lost. A row per lost event would spend the very
         * capacity that ran out, and repeating the marker on later drains would turn one burst into a
         * permanent alarm, so the counter resets here.
         */
        PlayerbotSocialEventDraft gap;
        gap.eventSequence = gapEventSequence;
        gap.eventType = std::string(PLAYERBOT_SOCIAL_EVENT_TYPE_GAP);
        gap.origin = PlayerbotSocialEventOrigin::System;
        gap.outcome = PlayerbotSocialEventOutcome::Failed;
        gap.hasChannel = false;
        gap.reason = std::string(PLAYERBOT_SOCIAL_EVENT_REASON_QUEUE_OVERFLOW);
        gap.diagnosticsJson = "{\"lost_events\":" + std::to_string(_lost) + "}";
        gap.occurredAtUnixSeconds = nowUnixSeconds;
        gap.priority = PlayerbotSocialEventPriority::Critical;

        PlayerbotSocialEventBinding binding;
        if (PlayerbotSocialBuildEventBinding(gap, binding))
            drained.push_back(std::move(binding));

        _lost = 0;
    }

    return drained;
}

std::size_t PlayerbotSocialEventQueue::PendingCount() const { return _pending.size(); }

std::vector<PlayerbotSocialEventBinding> PlayerbotSocialEventQueue::Pending() const
{
    std::vector<PlayerbotSocialEventBinding> waiting;
    waiting.reserve(_pending.size());

    for (Entry const& entry : _pending)
        waiting.push_back(entry.binding);

    return waiting;
}

uint64 PlayerbotSocialEventQueue::LostSinceLastDrain() const { return _lost; }

std::string_view PlayerbotSocialEventOriginName(PlayerbotSocialEventOrigin origin)
{
    switch (origin)
    {
        case PlayerbotSocialEventOrigin::Social:
            return "social";
        case PlayerbotSocialEventOrigin::CombatStatus:
            return "combat_status";
        case PlayerbotSocialEventOrigin::PartyStatus:
            return "party_status";
        case PlayerbotSocialEventOrigin::Legacy:
            return "legacy";
        case PlayerbotSocialEventOrigin::Assistance:
            return "assistance";
        case PlayerbotSocialEventOrigin::Pvp:
            return "pvp";
        case PlayerbotSocialEventOrigin::Control:
            return "control";
        case PlayerbotSocialEventOrigin::System:
            return "system";
    }

    /*
     * Empty rather than a plausible default, and the write path refuses to bind an empty one. This
     * build has no -Wswitch, so a value cast in from a payload reaches here, and MySQL under a non
     * strict mode would coerce an unrecognized ENUM string to the empty member: the event would be
     * stored with an origin that means nothing rather than rejected.
     */
    return {};
}

std::string_view PlayerbotSocialEventOutcomeName(PlayerbotSocialEventOutcome outcome)
{
    switch (outcome)
    {
        case PlayerbotSocialEventOutcome::Delivered:
            return "delivered";
        case PlayerbotSocialEventOutcome::Suppressed:
            return "suppressed";
        case PlayerbotSocialEventOutcome::Failed:
            return "failed";
        case PlayerbotSocialEventOutcome::Recorded:
            return "recorded";
    }

    return {};
}

std::string_view PlayerbotSocialMemoryCategoryName(PlayerbotSocialMemoryCategory category)
{
    switch (category)
    {
        case PlayerbotSocialMemoryCategory::Fact:
            return "fact";
        case PlayerbotSocialMemoryCategory::Impression:
            return "impression";
        case PlayerbotSocialMemoryCategory::Interaction:
            return "interaction";
        case PlayerbotSocialMemoryCategory::Event:
            return "event";
    }

    return {};
}

std::string_view PlayerbotSocialMemoryProvenanceName(PlayerbotSocialMemoryProvenance provenance)
{
    switch (provenance)
    {
        case PlayerbotSocialMemoryProvenance::Participated:
            return "participated";
        case PlayerbotSocialMemoryProvenance::Addressed:
            return "addressed";
        case PlayerbotSocialMemoryProvenance::Hearsay:
            return "hearsay";
        case PlayerbotSocialMemoryProvenance::Assistance:
            return "assistance";
        case PlayerbotSocialMemoryProvenance::Pvp:
            return "pvp";
    }

    return {};
}

std::string_view PlayerbotSocialChannelName(PlayerbotSocialChannel channel)
{
    switch (channel)
    {
        case PlayerbotSocialChannel::General:
            return "general";
        case PlayerbotSocialChannel::Say:
            return "say";
        case PlayerbotSocialChannel::Party:
            return "party";
        case PlayerbotSocialChannel::Whisper:
            return "whisper";
    }

    return {};
}

std::string_view PlayerbotSocialPrivacyScopeName(PlayerbotSocialPrivacyScope scope)
{
    switch (scope)
    {
        case PlayerbotSocialPrivacyScope::Public:
            return "public";
        case PlayerbotSocialPrivacyScope::Party:
            return "party";
        case PlayerbotSocialPrivacyScope::Whisper:
            return "whisper";
    }

    return {};
}

bool PlayerbotSocialParseMemoryCategory(std::string_view text, PlayerbotSocialMemoryCategory& category)
{
    for (PlayerbotSocialMemoryCategory const candidate :
         {PlayerbotSocialMemoryCategory::Fact, PlayerbotSocialMemoryCategory::Impression,
          PlayerbotSocialMemoryCategory::Interaction, PlayerbotSocialMemoryCategory::Event})
    {
        if (PlayerbotSocialMemoryCategoryName(candidate) == text)
        {
            category = candidate;
            return true;
        }
    }

    return false;
}

bool PlayerbotSocialParseMemoryProvenance(std::string_view text, PlayerbotSocialMemoryProvenance& provenance)
{
    for (PlayerbotSocialMemoryProvenance const candidate :
         {PlayerbotSocialMemoryProvenance::Participated, PlayerbotSocialMemoryProvenance::Addressed,
          PlayerbotSocialMemoryProvenance::Hearsay, PlayerbotSocialMemoryProvenance::Assistance,
          PlayerbotSocialMemoryProvenance::Pvp})
    {
        if (PlayerbotSocialMemoryProvenanceName(candidate) == text)
        {
            provenance = candidate;
            return true;
        }
    }

    return false;
}

bool PlayerbotSocialParsePrivacyScope(std::string_view text, PlayerbotSocialPrivacyScope& scope)
{
    for (PlayerbotSocialPrivacyScope const candidate :
         {PlayerbotSocialPrivacyScope::Public, PlayerbotSocialPrivacyScope::Party,
          PlayerbotSocialPrivacyScope::Whisper})
    {
        if (PlayerbotSocialPrivacyScopeName(candidate) == text)
        {
            scope = candidate;
            return true;
        }
    }

    return false;
}

bool PlayerbotSocialParseMemorySourceKind(std::string_view text, PlayerbotSocialMemorySourceKind& kind)
{
    for (PlayerbotSocialMemorySourceKind const candidate :
         {PlayerbotSocialMemorySourceKind::HumanObservation, PlayerbotSocialMemorySourceKind::AuthoritativeSource})
    {
        if (PlayerbotSocialMemorySourceKindName(candidate) == text)
        {
            kind = candidate;
            return true;
        }
    }

    return false;
}

PlayerbotSocialRelationshipBinding PlayerbotSocialBuildRelationshipBinding(
    PlayerbotSocialRelationshipKey const& key, PlayerbotSocialRelationshipValues const& values, uint32 interactionCount,
    uint64 nowUnixSeconds)
{
    // The clamp is applied here rather than at the caller, so a new write path cannot be added that
    // forgets it: there is no other way to obtain the fields the statement binds.
    PlayerbotSocialRelationshipValues const clamped = PlayerbotSocialClampRelationship(values);

    PlayerbotSocialRelationshipBinding binding;
    binding.botGuidCounter = key.botGuidCounter;
    binding.subjectGuidCounter = key.subjectGuidCounter;
    binding.familiarity = clamped.familiarity;
    binding.affinity = clamped.affinity;
    binding.trust = clamped.trust;
    binding.interactionCount = interactionCount;
    binding.lastInteractionAtUnixSeconds = nowUnixSeconds;

    return binding;
}

bool PlayerbotSocialMemoryScopeQueryFor(PlayerbotSocialChannel channel, PlayerbotSocialMemoryScopeQuery& query)
{
    switch (channel)
    {
        case PlayerbotSocialChannel::General:
        case PlayerbotSocialChannel::Say:
            query = PlayerbotSocialMemoryScopeQuery::PublicOnly;
            return true;
        case PlayerbotSocialChannel::Party:
            query = PlayerbotSocialMemoryScopeQuery::PublicAndParty;
            return true;
        case PlayerbotSocialChannel::Whisper:
            query = PlayerbotSocialMemoryScopeQuery::Any;
            return true;
    }

    return false;
}

bool PlayerbotSocialMemoryScopeIsWithinQuery(PlayerbotSocialPrivacyScope scope, PlayerbotSocialMemoryScopeQuery query)
{
    // An invalid scope belongs to no query, so it is never removed by a replacement and never
    // accepted into one. It cannot have come from the database, whose column is an ENUM.
    if (!PlayerbotSocialPrivacyScopeIsValid(scope))
        return false;

    switch (query)
    {
        case PlayerbotSocialMemoryScopeQuery::PublicOnly:
            return scope == PlayerbotSocialPrivacyScope::Public;
        case PlayerbotSocialMemoryScopeQuery::PublicAndParty:
            return scope == PlayerbotSocialPrivacyScope::Public || scope == PlayerbotSocialPrivacyScope::Party;
        case PlayerbotSocialMemoryScopeQuery::Any:
            return true;
    }

    // Narrowest answer for an unrecognized query: covering nothing means a replacement removes
    // nothing and accepts nothing, which loses a refresh rather than deleting live state.
    return false;
}

bool PlayerbotSocialSnapshotIsFresh(uint64 loadedAtUnixSeconds, uint64 nowUnixSeconds)
{
    if (nowUnixSeconds < loadedAtUnixSeconds)
        return false;

    return nowUnixSeconds - loadedAtUnixSeconds < PLAYERBOT_SOCIAL_SNAPSHOT_TTL_SECONDS;
}

char const* PlayerbotSocialConsentCommandName(PlayerbotSocialConsentCommand command)
{
    switch (command)
    {
        case PlayerbotSocialConsentCommand::Unrecognized:
            return "unrecognized";
        case PlayerbotSocialConsentCommand::Status:
            return "status";
        case PlayerbotSocialConsentCommand::OptOut:
            return "off";
        case PlayerbotSocialConsentCommand::OptIn:
            return "on";
        case PlayerbotSocialConsentCommand::ResetRequested:
            return "reset_requested";
        case PlayerbotSocialConsentCommand::ResetConfirmed:
            return "reset_confirmed";
    }

    return "unknown";
}

PlayerbotSocialConsentCommand PlayerbotSocialParseConsentCommand(std::string_view arguments)
{
    /*
     * Tokenized and matched whole, never searched for substrings. Substring matching is what would
     * make "reset confirm Deszy" parse as a reset, by finding the words it recognizes and ignoring
     * the name it does not. Here an unrecognized word makes the whole command unrecognized, which is
     * the property Definition of Done 4 needs: there is no spelling that names another character.
     */
    std::vector<std::string> tokens;
    std::string current;
    for (char const symbol : arguments)
    {
        if (symbol == ' ' || symbol == '\t' || symbol == '\r' || symbol == '\n')
        {
            if (!current.empty())
            {
                tokens.push_back(ToLowercase(current));
                current.clear();
            }

            continue;
        }

        current.push_back(symbol);
    }

    if (!current.empty())
        tokens.push_back(ToLowercase(current));

    if (tokens.size() == 1)
    {
        if (tokens[0] == "status")
            return PlayerbotSocialConsentCommand::Status;

        if (tokens[0] == "off")
            return PlayerbotSocialConsentCommand::OptOut;

        if (tokens[0] == "on")
            return PlayerbotSocialConsentCommand::OptIn;

        // Deliberately a different outcome from the confirmed form, so a handler cannot read
        // "they typed reset" as "they confirmed".
        if (tokens[0] == "reset")
            return PlayerbotSocialConsentCommand::ResetRequested;

        return PlayerbotSocialConsentCommand::Unrecognized;
    }

    if (tokens.size() == 2 && tokens[0] == "reset" && tokens[1] == "confirm")
        return PlayerbotSocialConsentCommand::ResetConfirmed;

    return PlayerbotSocialConsentCommand::Unrecognized;
}
