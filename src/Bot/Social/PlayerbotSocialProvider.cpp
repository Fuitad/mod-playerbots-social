/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#include "Bot/Social/PlayerbotSocialProvider.h"

#include <algorithm>
#include <cstdio>
#include <string>

#include "Bot/Social/PlayerbotSocialPromptContext.h"

namespace
{
/*
 * The largest prefix of `text` that is at most `limit` bytes and is still valid UTF-8.
 *
 * Cutting at the byte limit alone would leave a half written multi byte character behind. The
 * far side decodes the WHOLE frame as UTF-8 before it parses any field, so one truncated accent
 * does not shorten a persona, it takes the entire request dark.
 */
std::string BoundedUtf8(std::string text, std::size_t limit)
{
    if (text.size() <= limit)
        return text;

    std::size_t cut = limit;
    while (cut > 0 && (static_cast<unsigned char>(text[cut]) & 0xC0) == 0x80)
        --cut;

    text.resize(cut);
    return text;
}

// Appends `item` to a comma separated run, or starts one after `lead`.
void AppendListed(std::string& out, bool& started, char const* lead, std::string const& item)
{
    if (item.empty())
        return;

    out += started ? ", " : lead;
    out += item;
    started = true;
}
}  // namespace

static_assert(PLAYERBOT_SOCIAL_PROMPT_CONTEXT_MAX_LINES == PLAYERBOT_SOCIAL_CONTEXT_ENTRIES);
static_assert(PLAYERBOT_SOCIAL_PROMPT_CONTEXT_MAX_BYTES == PLAYERBOT_SOCIAL_CONTEXT_BYTES);
static_assert(PLAYERBOT_SOCIAL_PROMPT_CONTEXT_MAX_LINE_BYTES == PLAYERBOT_SOCIAL_CONTEXT_ENTRY_BYTES);

namespace
{
bool EvidenceSubjectRoleIsValid(PlayerbotSocialEvidenceSubjectRole role)
{
    switch (role)
    {
        case PlayerbotSocialEvidenceSubjectRole::CandidateBot:
        case PlayerbotSocialEvidenceSubjectRole::Participant:
        case PlayerbotSocialEvidenceSubjectRole::Source:
            return true;
    }

    return false;
}

bool EvidenceFactKindIsValid(PlayerbotSocialEvidenceFactKind kind)
{
    return kind >= PlayerbotSocialEvidenceFactKind::Name && kind <= PlayerbotSocialEvidenceFactKind::Achievement;
}

bool EvidenceProvenanceIsValid(PlayerbotSocialEvidenceProvenance provenance)
{
    switch (provenance)
    {
        case PlayerbotSocialEvidenceProvenance::CurrentWorld:
        case PlayerbotSocialEvidenceProvenance::HumanObservation:
        case PlayerbotSocialEvidenceProvenance::AuthoritativeSource:
            return true;
    }

    return false;
}

bool GroundingIdIsValid(std::string const& id)
{
    if (id.size() < 2 || id.front() != 'g')
        return false;

    return std::all_of(id.begin() + 1, id.end(), [](char character) { return character >= '0' && character <= '9'; });
}

void OfferEvidence(PlayerbotSocialGroundingEnvelope& envelope, PlayerbotSocialEvidenceSubjectRole subjectRole,
                   uint64 subjectGuidCounter, PlayerbotSocialEvidenceFactKind factKind, std::string value,
                   PlayerbotSocialEvidenceProvenance provenance, PlayerbotSocialPrivacyScope scope,
                   uint64 atUnixSeconds)
{
    if (envelope.refusal != PlayerbotSocialGroundingRefusal::None || value.empty())
        return;

    if (value.size() > PLAYERBOT_SOCIAL_EVIDENCE_VALUE_MAX_BYTES)
    {
        envelope.refusal = PlayerbotSocialGroundingRefusal::EntryTooLong;
        return;
    }

    if (envelope.entries.size() >= PLAYERBOT_SOCIAL_EVIDENCE_MAX_ENTRIES)
    {
        envelope.refusal = PlayerbotSocialGroundingRefusal::EntryCount;
        return;
    }

    std::string const id = "g" + std::to_string(envelope.entries.size() + 1);
    std::size_t bytes = id.size() + value.size();
    for (PlayerbotSocialEvidenceEntry const& existing : envelope.entries)
        bytes += existing.id.size() + existing.value.size();
    if (bytes > PLAYERBOT_SOCIAL_EVIDENCE_ENVELOPE_MAX_BYTES)
    {
        envelope.refusal = PlayerbotSocialGroundingRefusal::EnvelopeTooLarge;
        return;
    }

    auto const conflict = std::find_if(
        envelope.entries.begin(), envelope.entries.end(),
        [subjectRole, subjectGuidCounter, factKind, &value](PlayerbotSocialEvidenceEntry const& existing)
        {
            return existing.subjectRole == subjectRole && existing.subjectGuidCounter == subjectGuidCounter &&
                   existing.factKind == factKind && existing.value != value;
        });
    if (conflict != envelope.entries.end())
    {
        envelope.refusal = PlayerbotSocialGroundingRefusal::ConflictingFact;
        return;
    }

    PlayerbotSocialEvidenceEntry entry;
    entry.id = id;
    entry.subjectRole = subjectRole;
    entry.subjectGuidCounter = subjectGuidCounter;
    entry.factKind = factKind;
    entry.value = std::move(value);
    entry.provenance = provenance;
    entry.scope = scope;
    entry.atUnixSeconds = atUnixSeconds;
    envelope.entries.push_back(std::move(entry));
}

void OfferCharacterEvidence(PlayerbotSocialGroundingEnvelope& envelope, PlayerbotSocialCharacterFacts const& character,
                            PlayerbotSocialEvidenceSubjectRole role, bool perceivable,
                            PlayerbotSocialPrivacyScope scope, uint64 nowUnixSeconds)
{
    /*
     * Identity first, and never gated on perception.
     *
     * These are the facts /who reports about any character on the realm: nobody has to see someone
     * to know they are a level 31 gnome rogue in Elwynn Forest. Gating them behind line of sight is
     * what made a General reply ground on a bare name, and a bot answering a name alone guesses the
     * class it is talking to. Visibility travels as its own fact below, so the model is still told
     * it cannot see them.
     */
    OfferEvidence(envelope, role, character.guidCounter, PlayerbotSocialEvidenceFactKind::Name, character.name,
                  PlayerbotSocialEvidenceProvenance::CurrentWorld, scope, nowUnixSeconds);
    OfferEvidence(envelope, role, character.guidCounter, PlayerbotSocialEvidenceFactKind::Race, character.race,
                  PlayerbotSocialEvidenceProvenance::CurrentWorld, scope, nowUnixSeconds);
    OfferEvidence(envelope, role, character.guidCounter, PlayerbotSocialEvidenceFactKind::CharacterClass,
                  character.characterClass, PlayerbotSocialEvidenceProvenance::CurrentWorld, scope, nowUnixSeconds);
    if (character.level != 0)
        OfferEvidence(envelope, role, character.guidCounter, PlayerbotSocialEvidenceFactKind::Level,
                      std::to_string(character.level), PlayerbotSocialEvidenceProvenance::CurrentWorld, scope,
                      nowUnixSeconds);
    OfferEvidence(envelope, role, character.guidCounter, PlayerbotSocialEvidenceFactKind::Faction, character.faction,
                  PlayerbotSocialEvidenceProvenance::CurrentWorld, scope, nowUnixSeconds);
    OfferEvidence(envelope, role, character.guidCounter, PlayerbotSocialEvidenceFactKind::Zone, character.zone,
                  PlayerbotSocialEvidenceProvenance::CurrentWorld, scope, nowUnixSeconds);

    /*
     * Everything past here is what the character is doing right now, which the bot could only know
     * by looking at them. An unseen character's exact spot, party, guild, fight, and target are not
     * things to state as fact.
     */
    if (!perceivable)
        return;

    OfferEvidence(envelope, role, character.guidCounter, PlayerbotSocialEvidenceFactKind::Area, character.area,
                  PlayerbotSocialEvidenceProvenance::CurrentWorld, scope, nowUnixSeconds);
    OfferEvidence(envelope, role, character.guidCounter, PlayerbotSocialEvidenceFactKind::GroupRelation,
                  character.groupRelation, PlayerbotSocialEvidenceProvenance::CurrentWorld, scope, nowUnixSeconds);
    OfferEvidence(envelope, role, character.guidCounter, PlayerbotSocialEvidenceFactKind::GuildRelation,
                  character.guildRelation, PlayerbotSocialEvidenceProvenance::CurrentWorld, scope, nowUnixSeconds);
    OfferEvidence(envelope, role, character.guidCounter, PlayerbotSocialEvidenceFactKind::CombatState,
                  character.inCombat ? "in_combat" : "not_in_combat", PlayerbotSocialEvidenceProvenance::CurrentWorld,
                  scope, nowUnixSeconds);
    OfferEvidence(envelope, role, character.guidCounter, PlayerbotSocialEvidenceFactKind::Target,
                  character.visibleTarget, PlayerbotSocialEvidenceProvenance::CurrentWorld, scope, nowUnixSeconds);
}
}  // namespace

PlayerbotSocialGroundingEnvelope PlayerbotSocialBuildGroundingEnvelope(PlayerbotSocialGroundingInput const& input)
{
    PlayerbotSocialGroundingEnvelope envelope;
    envelope.profileLoadState = input.profileLoadState;
    envelope.memoryInputState = input.memoryInputState;
    envelope.activeContentExpansion = input.activeContentExpansion;
    envelope.transcriptEventPublicIds = input.transcriptEventPublicIds;

    if (input.bot.guidCounter == 0)
    {
        envelope.refusal = PlayerbotSocialGroundingRefusal::MissingBot;
        return envelope;
    }

    OfferCharacterEvidence(envelope, input.bot, PlayerbotSocialEvidenceSubjectRole::CandidateBot, true,
                           input.evidenceScope, input.nowUnixSeconds);
    OfferCharacterEvidence(envelope, input.participant, PlayerbotSocialEvidenceSubjectRole::Participant,
                           input.participant.visible, input.evidenceScope, input.nowUnixSeconds);
    if (input.participant.guidCounter != 0)
    {
        OfferEvidence(envelope, PlayerbotSocialEvidenceSubjectRole::Participant, input.participant.guidCounter,
                      PlayerbotSocialEvidenceFactKind::Visibility,
                      input.participant.visible ? "visible" : "not_visible",
                      PlayerbotSocialEvidenceProvenance::CurrentWorld, input.evidenceScope, input.nowUnixSeconds);
        OfferEvidence(envelope, PlayerbotSocialEvidenceSubjectRole::Participant, input.participant.guidCounter,
                      PlayerbotSocialEvidenceFactKind::Proximity,
                      input.participant.inRange ? "in_range" : "out_of_range",
                      PlayerbotSocialEvidenceProvenance::CurrentWorld, input.evidenceScope, input.nowUnixSeconds);
    }
    OfferEvidence(envelope, PlayerbotSocialEvidenceSubjectRole::CandidateBot, input.bot.guidCounter,
                  PlayerbotSocialEvidenceFactKind::Progression, std::to_string(input.activeContentExpansion),
                  PlayerbotSocialEvidenceProvenance::CurrentWorld, input.evidenceScope, input.nowUnixSeconds);

    for (PlayerbotSocialEvidenceEntry const& source : input.sourceFacts)
        OfferEvidence(envelope, source.subjectRole, source.subjectGuidCounter, source.factKind, source.value,
                      source.provenance, source.scope, source.atUnixSeconds);

    if (envelope.refusal == PlayerbotSocialGroundingRefusal::None && !PlayerbotSocialGroundingEnvelopeIsValid(envelope))
        envelope.refusal = PlayerbotSocialGroundingRefusal::InvalidEntry;

    return envelope;
}

bool PlayerbotSocialGroundingEnvelopeIsValid(PlayerbotSocialGroundingEnvelope const& envelope)
{
    if (envelope.refusal != PlayerbotSocialGroundingRefusal::None || envelope.entries.empty() ||
        envelope.entries.size() > PLAYERBOT_SOCIAL_EVIDENCE_MAX_ENTRIES ||
        envelope.transcriptEventPublicIds.size() > PLAYERBOT_SOCIAL_EVIDENCE_MAX_TRANSCRIPT_EVENTS)
        return false;

    std::size_t bytes = 0;
    std::vector<std::string> ids;
    ids.reserve(envelope.entries.size());
    for (PlayerbotSocialEvidenceEntry const& entry : envelope.entries)
    {
        if (!GroundingIdIsValid(entry.id) || !EvidenceSubjectRoleIsValid(entry.subjectRole) ||
            !EvidenceFactKindIsValid(entry.factKind) || !EvidenceProvenanceIsValid(entry.provenance) ||
            !PlayerbotSocialPrivacyScopeIsValid(entry.scope) || entry.subjectGuidCounter == 0 || entry.value.empty() ||
            entry.value.size() > PLAYERBOT_SOCIAL_EVIDENCE_VALUE_MAX_BYTES || entry.atUnixSeconds == 0 ||
            std::find(ids.begin(), ids.end(), entry.id) != ids.end())
            return false;

        ids.push_back(entry.id);
        bytes += entry.id.size() + entry.value.size();
        if (bytes > PLAYERBOT_SOCIAL_EVIDENCE_ENVELOPE_MAX_BYTES)
            return false;
    }

    for (std::string const& eventPublicId : envelope.transcriptEventPublicIds)
        if (!PlayerbotSocialPublicIdIsValid(PlayerbotSocialIdKind::Event, eventPublicId))
            return false;

    return true;
}

char const* PlayerbotSocialGroundingRefusalName(PlayerbotSocialGroundingRefusal refusal)
{
    switch (refusal)
    {
        case PlayerbotSocialGroundingRefusal::None:
            return "none";
        case PlayerbotSocialGroundingRefusal::MissingBot:
            return "missing_bot";
        case PlayerbotSocialGroundingRefusal::EntryCount:
            return "entry_count";
        case PlayerbotSocialGroundingRefusal::EntryTooLong:
            return "entry_too_long";
        case PlayerbotSocialGroundingRefusal::EnvelopeTooLarge:
            return "envelope_too_large";
        case PlayerbotSocialGroundingRefusal::DuplicateId:
            return "duplicate_id";
        case PlayerbotSocialGroundingRefusal::InvalidEntry:
            return "invalid_entry";
        case PlayerbotSocialGroundingRefusal::ConflictingFact:
            return "conflicting_fact";
    }

    return "unknown";
}

char const* PlayerbotSocialDeliveryRejectionName(PlayerbotSocialDeliveryRejection rejection)
{
    switch (rejection)
    {
        case PlayerbotSocialDeliveryRejection::None:
            return "none";
        case PlayerbotSocialDeliveryRejection::NoProvider:
            return "no_provider";
        case PlayerbotSocialDeliveryRejection::ProviderFailed:
            return "provider_failed";
        case PlayerbotSocialDeliveryRejection::ProviderTimedOut:
            return "provider_timed_out";
        case PlayerbotSocialDeliveryRejection::ShuttingDown:
            return "shutting_down";
        case PlayerbotSocialDeliveryRejection::UnknownRequest:
            return "unknown_request";
        case PlayerbotSocialDeliveryRejection::SupersededThread:
            return "superseded_thread";
        case PlayerbotSocialDeliveryRejection::EmptyOutput:
            return "empty_output";
        case PlayerbotSocialDeliveryRejection::MultilineOutput:
            return "multiline_output";
        case PlayerbotSocialDeliveryRejection::BurstDelimiter:
            return "burst_delimiter";
        case PlayerbotSocialDeliveryRejection::TooLong:
            return "too_long";
        case PlayerbotSocialDeliveryRejection::ChannelSwitch:
            return "channel_switch";
        case PlayerbotSocialDeliveryRejection::UnsupportedChannel:
            return "unsupported_channel";
        case PlayerbotSocialDeliveryRejection::EmoteChannelIllegal:
            return "emote_channel_illegal";
        case PlayerbotSocialDeliveryRejection::EmoteTargetDistant:
            return "emote_target_distant";
        case PlayerbotSocialDeliveryRejection::SpeakerGone:
            return "speaker_gone";
        case PlayerbotSocialDeliveryRejection::TargetGone:
            return "target_gone";
        case PlayerbotSocialDeliveryRejection::NotInGroup:
            return "not_in_group";
        case PlayerbotSocialDeliveryRejection::NotInChannel:
            return "not_in_channel";
        case PlayerbotSocialDeliveryRejection::OutOfRange:
            return "out_of_range";
        case PlayerbotSocialDeliveryRejection::NotVisible:
            return "not_visible";
        case PlayerbotSocialDeliveryRejection::DifferentPhase:
            return "different_phase";
        case PlayerbotSocialDeliveryRejection::DifferentMap:
            return "different_map";
        case PlayerbotSocialDeliveryRejection::SpeakerDead:
            return "speaker_dead";
        case PlayerbotSocialDeliveryRejection::ConsentWithdrawn:
            return "consent_withdrawn";
        case PlayerbotSocialDeliveryRejection::QueueFull:
            return "queue_full";
        case PlayerbotSocialDeliveryRejection::QueueReservedForPlayers:
            return "queue_reserved_for_players";
        case PlayerbotSocialDeliveryRejection::FactionForbids:
            return "faction_forbids";
        case PlayerbotSocialDeliveryRejection::LanguageNotUnderstood:
            return "language_not_understood";
        case PlayerbotSocialDeliveryRejection::MalformedThreadIdentity:
            return "malformed_thread_identity";
        case PlayerbotSocialDeliveryRejection::MissingReplyParent:
            return "missing_reply_parent";
        case PlayerbotSocialDeliveryRejection::ReplyParentMismatch:
            return "reply_parent_mismatch";
        case PlayerbotSocialDeliveryRejection::GroundingUnavailable:
            return "grounding_unavailable";
        case PlayerbotSocialDeliveryRejection::UnknownEvidence:
            return "unknown_evidence";
        case PlayerbotSocialDeliveryRejection::EvidenceSubjectMismatch:
            return "evidence_subject_mismatch";
        case PlayerbotSocialDeliveryRejection::EvidenceChanged:
            return "evidence_changed";
        case PlayerbotSocialDeliveryRejection::EvidenceScopeMismatch:
            return "evidence_scope_mismatch";
        case PlayerbotSocialDeliveryRejection::UnsupportedClaim:
            return "unsupported_claim";
        case PlayerbotSocialDeliveryRejection::IrrelevantContribution:
            return "irrelevant_contribution";
        case PlayerbotSocialDeliveryRejection::DuplicateWording:
            return "duplicate_wording";
        case PlayerbotSocialDeliveryRejection::DuplicateFunction:
            return "duplicate_function";
        case PlayerbotSocialDeliveryRejection::LockedRoleplayContent:
            return "locked_roleplay_content";
        case PlayerbotSocialDeliveryRejection::AuthorizedRoleplayInCombat:
            return "authorized_roleplay_in_combat";
        case PlayerbotSocialDeliveryRejection::LockedProgressionContent:
            return "locked_progression_content";
        case PlayerbotSocialDeliveryRejection::BudgetExhausted:
            return "budget_exhausted";
        default:
            break;
    }

    // A value from outside the enum is reported as unknown rather than as one of the named reasons,
    // so a corrupt value can never be read as a specific decision that was never made.
    return "unknown";
}

bool PlayerbotSocialDeliveryRejectionIsValid(PlayerbotSocialDeliveryRejection rejection)
{
    switch (rejection)
    {
        case PlayerbotSocialDeliveryRejection::None:
        case PlayerbotSocialDeliveryRejection::NoProvider:
        case PlayerbotSocialDeliveryRejection::ProviderFailed:
        case PlayerbotSocialDeliveryRejection::ProviderTimedOut:
        case PlayerbotSocialDeliveryRejection::ShuttingDown:
        case PlayerbotSocialDeliveryRejection::UnknownRequest:
        case PlayerbotSocialDeliveryRejection::SupersededThread:
        case PlayerbotSocialDeliveryRejection::EmptyOutput:
        case PlayerbotSocialDeliveryRejection::MultilineOutput:
        case PlayerbotSocialDeliveryRejection::BurstDelimiter:
        case PlayerbotSocialDeliveryRejection::TooLong:
        case PlayerbotSocialDeliveryRejection::ChannelSwitch:
        case PlayerbotSocialDeliveryRejection::UnsupportedChannel:
        case PlayerbotSocialDeliveryRejection::EmoteChannelIllegal:
        case PlayerbotSocialDeliveryRejection::EmoteTargetDistant:
        case PlayerbotSocialDeliveryRejection::SpeakerGone:
        case PlayerbotSocialDeliveryRejection::TargetGone:
        case PlayerbotSocialDeliveryRejection::NotInGroup:
        case PlayerbotSocialDeliveryRejection::NotInChannel:
        case PlayerbotSocialDeliveryRejection::OutOfRange:
        case PlayerbotSocialDeliveryRejection::NotVisible:
        case PlayerbotSocialDeliveryRejection::DifferentPhase:
        case PlayerbotSocialDeliveryRejection::DifferentMap:
        case PlayerbotSocialDeliveryRejection::SpeakerDead:
        case PlayerbotSocialDeliveryRejection::ConsentWithdrawn:
        case PlayerbotSocialDeliveryRejection::QueueFull:
        case PlayerbotSocialDeliveryRejection::QueueReservedForPlayers:
        case PlayerbotSocialDeliveryRejection::FactionForbids:
        case PlayerbotSocialDeliveryRejection::LanguageNotUnderstood:
        case PlayerbotSocialDeliveryRejection::MalformedThreadIdentity:
        case PlayerbotSocialDeliveryRejection::MissingReplyParent:
        case PlayerbotSocialDeliveryRejection::ReplyParentMismatch:
        case PlayerbotSocialDeliveryRejection::GroundingUnavailable:
        case PlayerbotSocialDeliveryRejection::UnknownEvidence:
        case PlayerbotSocialDeliveryRejection::EvidenceSubjectMismatch:
        case PlayerbotSocialDeliveryRejection::EvidenceChanged:
        case PlayerbotSocialDeliveryRejection::EvidenceScopeMismatch:
        case PlayerbotSocialDeliveryRejection::UnsupportedClaim:
        case PlayerbotSocialDeliveryRejection::IrrelevantContribution:
        case PlayerbotSocialDeliveryRejection::DuplicateWording:
        case PlayerbotSocialDeliveryRejection::DuplicateFunction:
        case PlayerbotSocialDeliveryRejection::LockedRoleplayContent:
        case PlayerbotSocialDeliveryRejection::AuthorizedRoleplayInCombat:
        case PlayerbotSocialDeliveryRejection::LockedProgressionContent:
        case PlayerbotSocialDeliveryRejection::BudgetExhausted:
            return true;
        default:
            break;
    }

    return false;
}

bool PlayerbotSocialRequestPriorityIsValid(PlayerbotSocialRequestPriority priority)
{
    switch (priority)
    {
        case PlayerbotSocialRequestPriority::DirectHumanEngagement:
        case PlayerbotSocialRequestPriority::MixedThread:
        case PlayerbotSocialRequestPriority::BotContinuation:
        case PlayerbotSocialRequestPriority::Starter:
            return true;
        default:
            break;
    }

    return false;
}

PlayerbotSocialRequestPriority PlayerbotSocialPriorityForLane(PlayerbotSocialPriorityLane lane)
{
    switch (lane)
    {
        case PlayerbotSocialPriorityLane::DirectHuman:
            return PlayerbotSocialRequestPriority::DirectHumanEngagement;
        case PlayerbotSocialPriorityLane::MixedHumanBot:
            return PlayerbotSocialRequestPriority::MixedThread;
        case PlayerbotSocialPriorityLane::BotOnlyContinuation:
            return PlayerbotSocialRequestPriority::BotContinuation;
        case PlayerbotSocialPriorityLane::NewStarter:
        case PlayerbotSocialPriorityLane::CareerGeneration:
        case PlayerbotSocialPriorityLane::BackgroundExtraction:
            break;
    }

    /*
     * The starter lane and the two non chat lanes share the lowest priority, which is also the fall
     * through for a value outside the enumeration. Failing to the lane that may NOT take a bot's last
     * slot is the safe direction: the cost is a refused starter, and the alternative is an unknown
     * lane quietly outranking a player.
     */
    return PlayerbotSocialRequestPriority::Starter;
}

bool PlayerbotSocialPriorityMayTakeLastSlot(PlayerbotSocialRequestPriority priority)
{
    /*
     * Only the two lanes a player is actually waiting on. A pair of starters would otherwise occupy
     * every slot a bot has and block the direct engagement that arrives a moment later, which is the
     * same starvation a shared ceiling causes, one level down.
     *
     * An unrecognized priority is treated as the lowest, so a value cast in from a payload can only
     * lose the reserved slot rather than claim it.
     */
    if (!PlayerbotSocialRequestPriorityIsValid(priority))
        return false;

    return priority == PlayerbotSocialRequestPriority::DirectHumanEngagement ||
           priority == PlayerbotSocialRequestPriority::MixedThread;
}

bool PlayerbotSocialOutputKindIsValid(PlayerbotSocialOutputKind kind)
{
    switch (kind)
    {
        case PlayerbotSocialOutputKind::Silence:
        case PlayerbotSocialOutputKind::Message:
        case PlayerbotSocialOutputKind::Emote:
            return true;
        default:
            break;
    }

    return false;
}

bool PlayerbotSocialContributionFunctionIsValid(PlayerbotSocialContributionFunction contribution)
{
    return contribution >= PlayerbotSocialContributionFunction::Answer &&
           contribution <= PlayerbotSocialContributionFunction::None;
}

char const* PlayerbotSocialContributionFunctionName(PlayerbotSocialContributionFunction contribution)
{
    switch (contribution)
    {
        case PlayerbotSocialContributionFunction::Answer:
            return "answer";
        case PlayerbotSocialContributionFunction::SpecificReaction:
            return "specific_reaction";
        case PlayerbotSocialContributionFunction::FactFreeBanter:
            return "fact_free_banter";
        case PlayerbotSocialContributionFunction::Gesture:
            return "gesture";
        case PlayerbotSocialContributionFunction::None:
            return "none";
    }

    return "unknown";
}

std::optional<PlayerbotSocialContributionFunction> PlayerbotSocialContributionFunctionFromName(std::string_view name)
{
    for (PlayerbotSocialContributionFunction const candidate :
         {PlayerbotSocialContributionFunction::Answer, PlayerbotSocialContributionFunction::SpecificReaction,
          PlayerbotSocialContributionFunction::FactFreeBanter, PlayerbotSocialContributionFunction::Gesture,
          PlayerbotSocialContributionFunction::None})
        if (name == PlayerbotSocialContributionFunctionName(candidate))
            return candidate;

    return std::nullopt;
}

bool PlayerbotSocialClaimSubjectIsValid(PlayerbotSocialClaimSubject subject)
{
    return subject >= PlayerbotSocialClaimSubject::CandidateBot && subject <= PlayerbotSocialClaimSubject::None;
}

char const* PlayerbotSocialClaimSubjectName(PlayerbotSocialClaimSubject subject)
{
    switch (subject)
    {
        case PlayerbotSocialClaimSubject::CandidateBot:
            return "candidate_bot";
        case PlayerbotSocialClaimSubject::Participant:
            return "participant";
        case PlayerbotSocialClaimSubject::None:
            return "none";
    }

    return "unknown";
}

std::optional<PlayerbotSocialClaimSubject> PlayerbotSocialClaimSubjectFromName(std::string_view name)
{
    for (PlayerbotSocialClaimSubject const candidate :
         {PlayerbotSocialClaimSubject::CandidateBot, PlayerbotSocialClaimSubject::Participant,
          PlayerbotSocialClaimSubject::None})
        if (name == PlayerbotSocialClaimSubjectName(candidate))
            return candidate;

    return std::nullopt;
}

bool PlayerbotSocialOutputHasBurstDelimiter(std::string const& text)
{
    /*
     * Anything that would become several messages downstream, or that shows the model scripted an
     * exchange instead of answering once.
     *
     * Newlines and carriage returns are the literal split. A speaker prefix such as "Bot:" or a
     * chat marker is the model writing somebody else's turn as well as its own, which is the same
     * problem one layer up: the result stops being one bot's remark.
     */
    if (text.find('\n') != std::string::npos || text.find('\r') != std::string::npos)
        return true;

    if (text.find("\\n") != std::string::npos)
        return true;

    for (char const* marker : {"||", "<br", "&#10;"})
    {
        if (text.find(marker) != std::string::npos)
            return true;
    }

    return false;
}

PlayerbotSocialDeliveryRejection PlayerbotSocialValidateOutput(PlayerbotSocialProviderResult const& result,
                                                               PlayerbotSocialChannel requestedChannel)
{
    if (!PlayerbotSocialOutputKindIsValid(result.kind))
        return PlayerbotSocialDeliveryRejection::EmptyOutput;

    if (!PlayerbotSocialContributionFunctionIsValid(result.contribution) ||
        !PlayerbotSocialClaimSubjectIsValid(result.claimSubject) ||
        result.citedEvidenceIds.size() > PLAYERBOT_SOCIAL_EVIDENCE_MAX_ENTRIES)
        return PlayerbotSocialDeliveryRejection::UnsupportedClaim;

    // Silence is a legitimate answer and carries no output to check. It is not a rejection: the bot
    // decided not to speak, which the plan explicitly allows.
    if (result.kind == PlayerbotSocialOutputKind::Silence)
        return result.text.empty() && result.emoteId == 0 &&
                       result.contribution == PlayerbotSocialContributionFunction::None &&
                       result.claimSubject == PlayerbotSocialClaimSubject::None && result.citedEvidenceIds.empty()
                   ? PlayerbotSocialDeliveryRejection::None
                   : PlayerbotSocialDeliveryRejection::UnsupportedClaim;

    if (!PlayerbotSocialChannelIsValid(result.channel) || !PlayerbotSocialChannelIsValid(requestedChannel))
        return PlayerbotSocialDeliveryRejection::UnsupportedChannel;

    /*
     * Refused rather than redirected. A result naming a different channel than the one the bot was
     * asked to speak on is not a delivery to fix up: honouring it is how a party remark reaches a
     * zone channel, and the privacy scope of the context that produced it was decided by the
     * REQUESTED channel.
     */
    if (result.channel != requestedChannel)
        return PlayerbotSocialDeliveryRejection::ChannelSwitch;

    /*
     * The length bound applies to any kind, before the shape is dispatched on.
     *
     * An emote carries no text, but nothing stops a provider attaching some, and the result is
     * stored: a check that only ran for messages would let an unbounded blob be copied into a
     * pending request through the emote path. It is refused here and the text is dropped at storage
     * as well, so neither the size nor the content survives.
     */
    if (result.text.size() > PLAYERBOT_SOCIAL_MAX_OUTPUT_LENGTH)
        return PlayerbotSocialDeliveryRejection::TooLong;

    if (result.kind == PlayerbotSocialOutputKind::Emote)
    {
        if (result.contribution != PlayerbotSocialContributionFunction::Gesture ||
            result.claimSubject != PlayerbotSocialClaimSubject::None || !result.citedEvidenceIds.empty())
            return PlayerbotSocialDeliveryRejection::UnsupportedClaim;

        /*
         * An emote is a gesture someone has to be present to see. General is a zone wide channel
         * whose participants are nowhere near each other, and whisper has no physical presence at
         * all, so neither can carry one.
         */
        if (result.channel != PlayerbotSocialChannel::Say && result.channel != PlayerbotSocialChannel::Party)
            return PlayerbotSocialDeliveryRejection::EmoteChannelIllegal;

        if (result.emoteId == 0)
            return PlayerbotSocialDeliveryRejection::EmptyOutput;

        return PlayerbotSocialDeliveryRejection::None;
    }

    if (result.text.empty())
        return PlayerbotSocialDeliveryRejection::EmptyOutput;

    /*
     * A claim rests on something cited, and either list satisfies it: evidence for what the world is
     * now, a memory for what this person said earlier. Before memories could be cited, a claim with
     * no evidence was refused outright, which left a bot asked about something it had been told with
     * no legal way to answer at all.
     */
    bool const citesSomething = !result.citedEvidenceIds.empty() || !result.citedMemoryIds.empty();

    if ((result.contribution == PlayerbotSocialContributionFunction::Answer && !citesSomething) ||
        (result.contribution == PlayerbotSocialContributionFunction::FactFreeBanter &&
         (result.claimSubject != PlayerbotSocialClaimSubject::None || citesSomething)))
        return PlayerbotSocialDeliveryRejection::UnsupportedClaim;

    if (result.contribution == PlayerbotSocialContributionFunction::Gesture ||
        result.contribution == PlayerbotSocialContributionFunction::None ||
        (result.claimSubject == PlayerbotSocialClaimSubject::None) == citesSomething)
        return PlayerbotSocialDeliveryRejection::UnsupportedClaim;

    // Checked before the length bound, so a model that returned a scripted exchange is reported as
    // that rather than as a long message.
    if (PlayerbotSocialOutputHasBurstDelimiter(result.text))
        return PlayerbotSocialDeliveryRejection::BurstDelimiter;

    return PlayerbotSocialDeliveryRejection::None;
}

PlayerbotSocialDeliveryRejection PlayerbotSocialValidateGroundedProposal(
    PlayerbotSocialProviderResult const& result, PlayerbotSocialGroundingEnvelope const& originalGrounding,
    PlayerbotSocialGroundingEnvelope const& currentGrounding, PlayerbotSocialChannel requestedChannel,
    bool expectsAnswer, std::vector<PlayerbotSocialOfferedMemory> const& offeredMemories)
{
    if (!PlayerbotSocialGroundingEnvelopeIsValid(originalGrounding))
        return PlayerbotSocialDeliveryRejection::GroundingUnavailable;

    if (result.kind == PlayerbotSocialOutputKind::Silence ||
        result.contribution == PlayerbotSocialContributionFunction::Gesture)
        return PlayerbotSocialDeliveryRejection::None;

    /*
     * A question is answered by whatever the model wrote, not by what it labeled the reply. Every
     * message function still admitted here (answer, specific_reaction, fact_free_banter) is a
     * conversational response, and the prompt never teaches the model that a question narrows the
     * label set, so refusing a claim-free banter label suppressed casual replies to casual
     * questions ("got a minute?") as irrelevant - a human whispering a bot heard silence. The one
     * label that still cannot fit is an evidence-citing answer to a line that asked nothing:
     * that is the model volunteering facts nobody requested, which is the shape this gate exists
     * to refuse.
     */
    if (!expectsAnswer && result.contribution == PlayerbotSocialContributionFunction::Answer)
        return PlayerbotSocialDeliveryRejection::IrrelevantContribution;

    // Either list satisfies the claim, exactly as in the shape check above, so the two gates cannot
    // disagree about the same reply.
    if ((result.claimSubject == PlayerbotSocialClaimSubject::None) !=
        (result.citedEvidenceIds.empty() && result.citedMemoryIds.empty()))
        return PlayerbotSocialDeliveryRejection::UnsupportedClaim;

    /*
     * Memory citations, resolved against what this request actually offered.
     *
     * A memory is not re-read against current world state the way a `CurrentWorld` fact is, because
     * it never described the world in the first place: it is what somebody said earlier, and that
     * does not stop having been said. What is re-asserted is the scope it was learned under, so a
     * confidence from a whisper cannot be repeated into a zone even if selection ever offered it.
     */
    for (std::string const& memoryId : result.citedMemoryIds)
    {
        auto const offered = std::find_if(offeredMemories.begin(), offeredMemories.end(),
                                          [&memoryId](PlayerbotSocialOfferedMemory const& candidate)
                                          { return candidate.id == memoryId; });
        if (offered == offeredMemories.end())
            return PlayerbotSocialDeliveryRejection::UnknownEvidence;

        // A memory is held by the bot ABOUT the person it is talking to, so it can support a claim
        // about that person and about nobody else, least of all about the bot itself.
        if (result.claimSubject != PlayerbotSocialClaimSubject::Participant)
            return PlayerbotSocialDeliveryRejection::EvidenceSubjectMismatch;

        if (!PlayerbotSocialMemoryIsRetrievableInChannel(offered->scope, requestedChannel))
            return PlayerbotSocialDeliveryRejection::EvidenceScopeMismatch;
    }

    for (std::string const& evidenceId : result.citedEvidenceIds)
    {
        auto const cited =
            std::find_if(originalGrounding.entries.begin(), originalGrounding.entries.end(),
                         [&evidenceId](PlayerbotSocialEvidenceEntry const& entry) { return entry.id == evidenceId; });
        if (cited == originalGrounding.entries.end())
            return PlayerbotSocialDeliveryRejection::UnknownEvidence;

        bool const subjectCompatible = result.claimSubject == PlayerbotSocialClaimSubject::CandidateBot
                                           ? cited->subjectRole == PlayerbotSocialEvidenceSubjectRole::CandidateBot ||
                                                 cited->subjectRole == PlayerbotSocialEvidenceSubjectRole::Source
                                           : cited->subjectRole == PlayerbotSocialEvidenceSubjectRole::Participant;
        if (!subjectCompatible)
            return PlayerbotSocialDeliveryRejection::EvidenceSubjectMismatch;

        if (!PlayerbotSocialMemoryIsRetrievableInChannel(cited->scope, requestedChannel))
            return PlayerbotSocialDeliveryRejection::EvidenceScopeMismatch;

        if (cited->provenance != PlayerbotSocialEvidenceProvenance::CurrentWorld)
            continue;

        if (!PlayerbotSocialGroundingEnvelopeIsValid(currentGrounding))
            return PlayerbotSocialDeliveryRejection::EvidenceChanged;

        auto const current = std::find_if(currentGrounding.entries.begin(), currentGrounding.entries.end(),
                                          [&cited](PlayerbotSocialEvidenceEntry const& entry)
                                          {
                                              return entry.subjectRole == cited->subjectRole &&
                                                     entry.subjectGuidCounter == cited->subjectGuidCounter &&
                                                     entry.factKind == cited->factKind;
                                          });
        if (current == currentGrounding.entries.end() || current->value != cited->value)
            return PlayerbotSocialDeliveryRejection::EvidenceChanged;
    }

    return PlayerbotSocialDeliveryRejection::None;
}

PlayerbotSocialDeliveryRejection PlayerbotSocialRevalidateDelivery(PlayerbotSocialChannel channel,
                                                                   PlayerbotSocialOutputKind kind,
                                                                   PlayerbotSocialDeliveryConditions const& conditions)
{
    if (!PlayerbotSocialChannelIsValid(channel) || !PlayerbotSocialOutputKindIsValid(kind))
        return PlayerbotSocialDeliveryRejection::UnsupportedChannel;

    if (kind == PlayerbotSocialOutputKind::Silence)
        return PlayerbotSocialDeliveryRejection::None;

    /*
     * Conditions every channel shares. The speaker has to exist and be alive to say anything, the
     * character has to still be willing to be spoken about, and the conversation has to still be the
     * one this answers. A superseded thread is the stale case Definition of Done 1 names: the answer
     * was fine when it was asked for and is a non sequitur now.
     */
    if (!conditions.speakerOnline)
        return PlayerbotSocialDeliveryRejection::SpeakerGone;

    if (!conditions.speakerAlive)
        return PlayerbotSocialDeliveryRejection::SpeakerDead;

    if (!conditions.consentHolds)
        return PlayerbotSocialDeliveryRejection::ConsentWithdrawn;

    /*
     * Named authoritative by the channel contract alongside everything spatial. Checked for every
     * channel: a party is necessarily one faction so this is vacuous there, but a vacuous check costs
     * nothing and an absent one is how cross faction chat reaches a player as readable text.
     */
    if (!conditions.factionAllows)
        return PlayerbotSocialDeliveryRejection::FactionForbids;

    if (!conditions.languageUnderstood)
        return PlayerbotSocialDeliveryRejection::LanguageNotUnderstood;

    if (!conditions.threadStillCurrent)
        return PlayerbotSocialDeliveryRejection::SupersededThread;

    switch (channel)
    {
        case PlayerbotSocialChannel::General:
            /*
             * Zone wide. Membership of the localized channel is the authority, and nothing spatial
             * is: two characters in the same zone channel may be a continent apart within it. An
             * emote here was already refused by the shape check, so nothing directed can reach this.
             */
            if (!conditions.inChannel)
                return PlayerbotSocialDeliveryRejection::NotInChannel;

            return PlayerbotSocialDeliveryRejection::None;

        case PlayerbotSocialChannel::Say:
            // Local. Everything spatial is authoritative, because say is only heard nearby.
            if (!conditions.sameMap)
                return PlayerbotSocialDeliveryRejection::DifferentMap;

            if (!conditions.samePhase)
                return PlayerbotSocialDeliveryRejection::DifferentPhase;

            if (!conditions.withinRange)
                return PlayerbotSocialDeliveryRejection::OutOfRange;

            if (!conditions.targetVisible)
                return PlayerbotSocialDeliveryRejection::NotVisible;

            return PlayerbotSocialDeliveryRejection::None;

        case PlayerbotSocialChannel::Party:
            /*
             * Group membership is the authority. A directed emote additionally has to make spatial
             * sense, because a party can be spread across a zone and gesturing at someone who cannot
             * see you is the distant directed emote the contract forbids.
             */
            if (!conditions.inSameGroup)
                return PlayerbotSocialDeliveryRejection::NotInGroup;

            if (kind == PlayerbotSocialOutputKind::Emote)
            {
                if (!conditions.sameMap || !conditions.samePhase)
                    return PlayerbotSocialDeliveryRejection::EmoteTargetDistant;

                if (!conditions.withinRange || !conditions.targetVisible)
                    return PlayerbotSocialDeliveryRejection::EmoteTargetDistant;
            }

            return PlayerbotSocialDeliveryRejection::None;

        case PlayerbotSocialChannel::Whisper:
            // Directed at one character, who has to still be there to receive it.
            if (!conditions.targetOnline)
                return PlayerbotSocialDeliveryRejection::TargetGone;

            return PlayerbotSocialDeliveryRejection::None;

        default:
            break;
    }

    return PlayerbotSocialDeliveryRejection::UnsupportedChannel;
}

uint32 PlayerbotSocialDeliveryDelayMs(std::size_t outputLength, uint32 roll)
{
    /*
     * A spread rather than a constant, and weighted by how much there was to type.
     *
     * The roll supplies the variation and the length supplies the floor, so two bots answering the
     * same message at the same instant do not speak in unison, and a long remark never arrives
     * faster than a short one. Both halves are bounded into the same window, so no combination can
     * produce an instant reply or one so late the conversation has moved on.
     */
    // The static_asserts beside the constants make both of these safe: the span is positive and the
    // output bound is non zero, so neither the subtraction nor the divisions below can go wrong.
    constexpr uint32 span = PLAYERBOT_SOCIAL_DELIVERY_DELAY_MAX_MS - PLAYERBOT_SOCIAL_DELIVERY_DELAY_MIN_MS;

    // Typing time, capped so an oversized result cannot push the delay past the window on its own.
    uint32 const typing = static_cast<uint32>(std::min<std::size_t>(outputLength, PLAYERBOT_SOCIAL_MAX_OUTPUT_LENGTH)) *
                          span / static_cast<uint32>(PLAYERBOT_SOCIAL_MAX_OUTPUT_LENGTH) / 2;

    uint32 const jitter = span == 0 ? 0 : (roll % (span / 2 + 1));

    return PLAYERBOT_SOCIAL_DELIVERY_DELAY_MIN_MS + typing + jitter;
}

uint64 PlayerbotSocialUnixMilliseconds(std::chrono::system_clock::time_point now)
{
    int64 const milliseconds = std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch()).count();
    return milliseconds > 0 ? static_cast<uint64>(milliseconds) : 0;
}

std::string PlayerbotSocialRenderPersona(PlayerbotEffectiveSocialPersona const& persona)
{
    /*
     * The head is what a persona cannot be without: how this bot speaks, and how it currently feels
     * about whoever it is answering. It is short by construction, which is what lets the bound below
     * shorten the tail without ever emptying the whole thing.
     */
    std::string rendered = "speaks ";
    rendered += PlayerbotPersonality::VoiceName(persona.base.voice);
    rendered += ", ";
    rendered += PlayerbotSocialStanceName(persona.stance);
    rendered += " toward this listener";

    /*
     * The dials travel as their numbers rather than as adjectives. Naming bands here would be this
     * layer deciding that 61 is "warm", which is a characterisation the generation is better placed
     * to make than a lookup table is.
     */
    rendered += "; warmth ";
    rendered += std::to_string(persona.traits.warmth);
    rendered += ", talkativeness ";
    rendered += std::to_string(persona.traits.talkativeness);
    rendered += ", curiosity ";
    rendered += std::to_string(persona.traits.curiosity);
    rendered += ", humor ";
    rendered += std::to_string(persona.traits.humor);
    rendered += ", formality ";
    rendered += std::to_string(persona.traits.formality);

    bool interested = false;
    for (std::string const& interest : persona.traits.interests)
        AppendListed(rendered, interested, "; interested in ", interest);

    bool averse = false;
    for (std::string const& aversion : persona.traits.aversions)
        AppendListed(rendered, averse, "; avoids ", aversion);

    if (persona.biographyState == PlayerbotBiographyState::Ready)
    {
        auto const field = [&rendered](char const* label, std::string const& value)
        {
            if (value.empty())
                return;

            rendered += label;
            rendered += value;
        };

        field("; play approach: ", persona.biography.origin);
        field("; play motivation: ", persona.biography.motivation);
        field("; learning history: ", persona.biography.formativeExperience);
        field("; chat topics: ", persona.biography.preferredTopics);
        field("; chat habits: ", persona.biography.mannerisms);
        field("; group values: ", persona.biography.values);
    }

    return BoundedUtf8(std::move(rendered), PLAYERBOT_SOCIAL_CONTEXT_ENTRY_BYTES);
}

std::vector<PlayerbotSocialContextMemory> PlayerbotSocialSelectContextMemories(
    PlayerbotSocialStateStore const& state, PlayerbotSocialRelationshipKey const& key, PlayerbotSocialChannel channel)
{
    std::vector<PlayerbotSocialMemoryRecord> records = state.RecallMemories(key, channel);

    /*
     * Stable, because two memories of equal significance and confidence must not swap places
     * between two requests. A prompt that reshuffles on its own is a bot whose recollection changes
     * for no reason the world can see.
     */
    std::stable_sort(records.begin(), records.end(),
                     [](PlayerbotSocialMemoryRecord const& left, PlayerbotSocialMemoryRecord const& right)
                     {
                         if (left.significance != right.significance)
                             return left.significance > right.significance;

                         return left.confidence > right.confidence;
                     });

    if (records.size() > PLAYERBOT_SOCIAL_CONTEXT_ENTRIES)
        records.resize(PLAYERBOT_SOCIAL_CONTEXT_ENTRIES);

    std::vector<PlayerbotSocialContextMemory> selected;
    selected.reserve(records.size());
    for (PlayerbotSocialMemoryRecord const& record : records)
    {
        PlayerbotSocialContextMemory memory;
        memory.text = BoundedUtf8(record.paraphrase, PLAYERBOT_SOCIAL_CONTEXT_ENTRY_BYTES);
        memory.scope = record.scope;
        memory.publicId = record.publicId;

        // The far side rejects an empty memory entry outright, so a paraphrase bounded away to
        // nothing is dropped here rather than taking the whole context with it.
        if (memory.text.empty())
            continue;

        /*
         * Numbered over what survived, not over what was considered, so the ids a reply may cite are
         * dense and match the order the prompt renders. Assigning before the emptiness check would
         * leave a gap naming an entry the model was never shown.
         */
        memory.id = "m" + std::to_string(selected.size() + 1);
        selected.push_back(std::move(memory));
    }

    return selected;
}

std::string PlayerbotSocialRenderRelationship(PlayerbotSocialRelationshipValues const& values)
{
    /*
     * Two decimals, because the stored range is fractional and the difference between a stranger
     * and an acquaintance lives in the first two places. Rendered from a fixed width buffer rather
     * than through std::to_string, whose float overload emits six decimals of noise.
     */
    auto twoPlaces = [](float value)
    {
        char buffer[16] = {};
        std::snprintf(buffer, sizeof(buffer), "%.2f", static_cast<double>(value));
        return std::string(buffer);
    };

    std::string rendered = "familiarity ";
    rendered += twoPlaces(values.familiarity);
    rendered += ", affinity ";
    rendered += twoPlaces(values.affinity);
    rendered += ", trust ";
    rendered += twoPlaces(values.trust);

    return BoundedUtf8(std::move(rendered), PLAYERBOT_SOCIAL_CONTEXT_ENTRY_BYTES);
}

namespace
{
/*
 * The bracketed facts about who is speaking: `[Troll Rogue 23, Durotar]`.
 *
 * Every component is optional and an unknown one is simply absent, because a bot answering an
 * unresolved character must be short of information rather than misinformed. An entirely unknown
 * speaker produces no bracket at all, which is the bare `Name:` line this thread rendered before.
 */
std::string RenderSpeakerIdentity(PlayerbotSocialSpeakerIdentity const& identity)
{
    std::string head;
    auto append = [&head](std::string const& component)
    {
        if (component.empty())
            return;
        if (!head.empty())
            head += ' ';
        head += component;
    };

    append(identity.race);
    append(identity.characterClass);
    if (identity.level != 0)
        append(std::to_string(identity.level));

    if (head.empty() && identity.zone.empty())
        return {};

    std::string rendered = " [";
    rendered += head;
    if (!identity.zone.empty())
    {
        if (!head.empty())
            rendered += ", ";
        rendered += identity.zone;
    }
    rendered += ']';
    return rendered;
}

/*
 * Who the line was answering, resolved inside the snapshot rather than trusted from anywhere else.
 * A parent that has aged out of the buffer, or one whose speaker is this line's own speaker, names
 * nobody: the marker exists to tell a bystander that a question already has an addressee, and a bot
 * continuing its own turn has not addressed anyone new.
 */
std::string RenderAddressee(PlayerbotSocialPromptContextSnapshot const& snapshot, PlayerbotSocialPromptLine const& line)
{
    if (line.replyToEventPublicId.empty())
        return {};

    for (PlayerbotSocialPromptLine const& parent : snapshot.lines)
    {
        if (parent.eventPublicId != line.replyToEventPublicId)
            continue;
        if (parent.speakerName.empty())
            return {};

        /*
         * Same speaker only when the identifier is a real one. Zero means unresolved everywhere in
         * this feature, so comparing it would make two different unknown speakers one, and drop a
         * marker that names a genuinely different character. Fall back to the name in that case:
         * it is what the marker would have printed anyway.
         */
        bool const sameSpeaker = parent.speakerGuidCounter != 0 && line.speakerGuidCounter != 0
                                     ? parent.speakerGuidCounter == line.speakerGuidCounter
                                     : parent.speakerName == line.speakerName;
        if (sameSpeaker)
            return {};

        return " (to " + parent.speakerName + ")";
    }

    return {};
}
}  // namespace

std::vector<std::string> PlayerbotSocialRenderPromptThread(PlayerbotSocialPromptContextSnapshot const& snapshot)
{
    if (!snapshot.Accepted())
        return {};

    std::vector<std::string> rendered;
    std::size_t bytes = 0;

    // Walk newest first so the byte and count ceilings discard the oldest context rather than the
    // line the bot is answering. Insert at the front to restore chronological order for the model.
    for (auto line = snapshot.lines.rbegin(); line != snapshot.lines.rend(); ++line)
    {
        std::string speaker = line->speakerName;
        if (speaker.empty())
            speaker = line->speakerIsHuman ? "human" : "bot";

        /*
         * Everything up to and including the colon is written here, from values this module read off
         * the world. Everything after it is what the speaker typed, rendered verbatim: mangling
         * speech to defend the notation would corrupt legitimate lines, so the prompt's notation
         * rule instead confines trust to this prefix. Annotation shaped text a player types lands
         * after the colon, where the rule says facts do not live.
         */
        std::string value = speaker + RenderSpeakerIdentity(line->speakerIdentity) + RenderAddressee(snapshot, *line) +
                            ": " + line->text;
        value = BoundedUtf8(std::move(value), PLAYERBOT_SOCIAL_CONTEXT_ENTRY_BYTES);
        if (value.empty() || rendered.size() >= PLAYERBOT_SOCIAL_CONTEXT_ENTRIES ||
            bytes + value.size() > PLAYERBOT_SOCIAL_CONTEXT_BYTES)
            continue;

        bytes += value.size();
        rendered.insert(rendered.begin(), std::move(value));
    }

    return rendered;
}

std::vector<std::string> PlayerbotSocialRenderNearby(std::vector<std::string> const& nearbyNames)
{
    std::vector<std::string> rendered;
    std::size_t bytes = 0;

    for (std::string const& name : nearbyNames)
    {
        std::string bounded = BoundedUtf8(name, PLAYERBOT_SOCIAL_CONTEXT_ENTRY_BYTES);
        if (bounded.empty() || std::find(rendered.begin(), rendered.end(), bounded) != rendered.end())
            continue;

        if (rendered.size() >= PLAYERBOT_SOCIAL_CONTEXT_ENTRIES ||
            bytes + bounded.size() > PLAYERBOT_SOCIAL_CONTEXT_BYTES)
            break;

        bytes += bounded.size();
        rendered.push_back(std::move(bounded));
    }

    return rendered;
}
