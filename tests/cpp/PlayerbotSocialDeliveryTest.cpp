/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#include <chrono>
#include <cstdint>
#include <initializer_list>
#include <set>
#include <string>
#include <tuple>
#include <vector>

#include "Bot/Social/PlayerbotSocialContent.h"
#include "Bot/Social/PlayerbotSocialMgr.h"
#include "Bot/Social/PlayerbotSocialPromptContext.h"
#include "Bot/Social/PlayerbotSocialProvider.h"
#include "Bot/Social/PlayerbotSocialRoute.h"
#include "gtest/gtest.h"

namespace
{
PlayerbotPersonalityProfile StoredPersonality(uint8 roleplayAffinity = 100)
{
    PlayerbotPersonalityProfile profile;
    profile.craftingAffinity = 50;
    profile.gatheringAffinity = 50;
    profile.explorationAffinity = 50;
    profile.sociability = 80;
    profile.voice = PlayerbotVoice::Wry;
    profile.fictionalAge = 36;
    profile.fictionalHomeCountry = "Ireland";
    profile.roleplayAffinity = roleplayAffinity;
    return profile;
}

PlayerbotSocialProviderResult Message(std::string text, PlayerbotSocialChannel channel)
{
    PlayerbotSocialProviderResult result;
    result.requestToken = 1;
    result.kind = PlayerbotSocialOutputKind::Message;
    result.text = std::move(text);
    result.channel = channel;
    result.contribution = PlayerbotSocialContributionFunction::FactFreeBanter;
    return result;
}

PlayerbotSocialProviderResult EmoteResult(uint32 emoteId, PlayerbotSocialChannel channel)
{
    PlayerbotSocialProviderResult result;
    result.requestToken = 1;
    result.kind = PlayerbotSocialOutputKind::Emote;
    result.emoteId = emoteId;
    result.channel = channel;
    result.contribution = PlayerbotSocialContributionFunction::Gesture;
    return result;
}

PlayerbotSocialGroundingEnvelope Grounding(uint32 level = 32, std::string zone = "Elwynn Forest",
                                           std::string groupRelation = "same_party",
                                           std::string target = "Kobold Miner",
                                           PlayerbotSocialPrivacyScope scope = PlayerbotSocialPrivacyScope::Public)
{
    PlayerbotSocialGroundingInput input;
    input.nowUnixSeconds = 1000;
    input.profileLoadState = PlayerbotSocialProfileLoadState::Loaded;
    input.memoryInputState = PlayerbotSocialMemoryInputState::Loaded;
    input.evidenceScope = scope;
    input.bot.guidCounter = 500;
    input.bot.name = "Barnek";
    input.bot.level = level;
    input.bot.zone = std::move(zone);
    input.bot.groupRelation = std::move(groupRelation);
    input.bot.visibleTarget = std::move(target);
    input.participant.guidCounter = 900;
    input.participant.name = "Elyse";
    input.participant.visible = true;
    input.participant.inRange = true;
    return PlayerbotSocialBuildGroundingEnvelope(input);
}

/*
 * A room-addressed envelope, matching what the route builds for a General starter: the audience is
 * not perceivable there, so no Participant evidence travels and the request rightly carries no
 * wire subject.
 */
PlayerbotSocialGroundingEnvelope RoomGrounding()
{
    PlayerbotSocialGroundingInput input;
    input.nowUnixSeconds = 1000;
    input.profileLoadState = PlayerbotSocialProfileLoadState::Loaded;
    input.memoryInputState = PlayerbotSocialMemoryInputState::Loaded;
    input.evidenceScope = PlayerbotSocialPrivacyScope::Public;
    input.bot.guidCounter = 500;
    input.bot.name = "Barnek";
    input.bot.level = 32;
    input.bot.zone = "Elwynn Forest";
    return PlayerbotSocialBuildGroundingEnvelope(input);
}

std::string EvidenceId(PlayerbotSocialGroundingEnvelope const& grounding, PlayerbotSocialEvidenceFactKind fact)
{
    auto const found = std::find_if(
        grounding.entries.begin(), grounding.entries.end(), [fact](PlayerbotSocialEvidenceEntry const& entry)
        { return entry.subjectRole == PlayerbotSocialEvidenceSubjectRole::CandidateBot && entry.factKind == fact; });
    return found == grounding.entries.end() ? std::string() : found->id;
}

PlayerbotSocialCallMetadata CallMetadata()
{
    return PlayerbotSocialCallMetadata{
        "fixture-social-model", 42, 100, 50, 20, 30, "0.002900",
    };
}

PlayerbotSocialOperatorEvidence OperatorEvidence()
{
    PlayerbotSocialOperatorEvidence evidence;
    evidence.grounding = Grounding();
    evidence.grounding.transcriptEventPublicIds.push_back("evt_00000000000000000000000000000002");
    evidence.profileLoad.state = PlayerbotSocialProfileLoadState::Loaded;
    evidence.profileLoad.storedRowPresent = true;
    evidence.profileLoad.storedSchemaVersion = 3;
    evidence.profileLoad.storedTraitsVersion = 3;
    evidence.profileLoad.storedBiographyState = PlayerbotBiographyState::Ready;
    evidence.profileLoad.storedBiographyVersion = 3;
    evidence.rolloutStage = PlayerbotSocialRolloutStage::BoundedContinuation;
    evidence.contribution = PlayerbotSocialContributionFunction::SpecificReaction;
    evidence.citedEvidenceIds.push_back(EvidenceId(evidence.grounding, PlayerbotSocialEvidenceFactKind::Level));
    return evidence;
}

// Everything true. Each test turns off exactly the one condition it is about, so a rule that
// depended on something unrelated would show up as the wrong rejection rather than passing.
PlayerbotSocialDeliveryConditions AllHold()
{
    PlayerbotSocialDeliveryConditions conditions;
    conditions.speakerOnline = true;
    conditions.factionAllows = true;
    conditions.languageUnderstood = true;
    conditions.speakerAlive = true;
    conditions.targetOnline = true;
    conditions.sameMap = true;
    conditions.samePhase = true;
    conditions.targetVisible = true;
    conditions.withinRange = true;
    conditions.inSameGroup = true;
    conditions.inChannel = true;
    conditions.consentHolds = true;
    conditions.threadStillCurrent = true;
    conditions.currentGrounding = Grounding();
    return conditions;
}
}  // namespace

TEST(PlayerbotSocialDeliveryTest, AcceptedMessageAndEmoteRecordsKeepCompleteCallMetadata)
{
    PlayerbotSocialDeliveryRequest request;
    request.channel = PlayerbotSocialChannel::Say;
    request.origin = PlayerbotSocialEventOrigin::Social;
    request.text = "Aye.";
    request.callMetadata = CallMetadata();

    PlayerbotSocialSpeaker speaker;
    speaker.botGuidCounter = 500;
    speaker.zoneId = 12;

    PlayerbotSocialDelivery delivery;
    ASSERT_TRUE(PlayerbotSocialDeliveryRecordFor(request, speaker, true, 8000, delivery));
    ASSERT_TRUE(delivery.callMetadata.has_value());

    PlayerbotSocialEventDraft const message = PlayerbotSocialMakeDeliveryEvent(delivery);
    EXPECT_EQ(message.diagnosticsJson,
              "{\"model\":\"fixture-social-model\",\"provider_latency_ms\":42,\"input_tokens\":100,"
              "\"output_tokens\":50,\"cache_creation_input_tokens\":20,\"cache_read_input_tokens\":30,"
              "\"cost_usd\":\"0.002900\"}");

    request.isEmote = true;
    request.emoteId = 4;
    ASSERT_TRUE(PlayerbotSocialDeliveryRecordFor(request, speaker, true, 8000, delivery));

    PlayerbotSocialEventDraft const emote = PlayerbotSocialMakeDeliveryEvent(delivery);
    EXPECT_EQ(emote.diagnosticsJson,
              "{\"emote\":4,\"model\":\"fixture-social-model\",\"provider_latency_ms\":42,"
              "\"input_tokens\":100,\"output_tokens\":50,\"cache_creation_input_tokens\":20,"
              "\"cache_read_input_tokens\":30,\"cost_usd\":\"0.002900\"}");
}

TEST(PlayerbotSocialDeliveryTest, DeliveryEvidenceIsBoundedPublicAndContainsNoInternalIdentity)
{
    PlayerbotSocialDelivery delivery;
    delivery.botGuidCounter = 987654321;
    delivery.channel = PlayerbotSocialChannel::Say;
    delivery.origin = PlayerbotSocialEventOrigin::Social;
    delivery.threadPublicId = "thr_00000000000000000000000000000001";
    delivery.replyToEventPublicId = "evt_00000000000000000000000000000001";
    delivery.text = "Aye.";
    delivery.operatorEvidence = OperatorEvidence();

    PlayerbotSocialEventDraft const event = PlayerbotSocialMakeDeliveryEvent(delivery);
    EXPECT_NE(event.diagnosticsJson.find("\"evidence\":{"), std::string::npos);
    EXPECT_NE(event.diagnosticsJson.find("\"stage\":\"bounded_continuation\""), std::string::npos);
    EXPECT_NE(event.diagnosticsJson.find("\"profile\":{\"state\":\"loaded\""), std::string::npos);
    EXPECT_NE(event.diagnosticsJson.find("\"schema_version\":3"), std::string::npos);
    EXPECT_NE(event.diagnosticsJson.find("\"memory\":{\"state\":\"loaded\"}"), std::string::npos);
    EXPECT_NE(event.diagnosticsJson.find("\"function\":\"specific_reaction\""), std::string::npos);
    EXPECT_NE(event.diagnosticsJson.find("\"cited_evidence_ids\":[\"g"), std::string::npos);
    EXPECT_NE(event.diagnosticsJson.find("\"transcript_event_ids\":[\"evt_00000000000000000000000000000002\"]"),
              std::string::npos);
    EXPECT_EQ(event.diagnosticsJson.find("987654321"), std::string::npos)
        << "the bot GUID is repository-only identity, never evidence JSON";
    EXPECT_EQ(event.diagnosticsJson.find("subjectGuidCounter"), std::string::npos);
    EXPECT_EQ(event.diagnosticsJson.find("request_token"), std::string::npos);
    EXPECT_LE(event.diagnosticsJson.size(), PLAYERBOT_SOCIAL_OPERATOR_EVIDENCE_MAX_BYTES);
}

TEST(PlayerbotSocialDeliveryTest, OperatorEvidenceRefusesCitationsOutsideItsGroundingEnvelope)
{
    PlayerbotSocialOperatorEvidence evidence = OperatorEvidence();
    evidence.citedEvidenceIds = {"g999"};

    EXPECT_FALSE(PlayerbotSocialSerializeOperatorEvidence(evidence).has_value());
}

TEST(PlayerbotSocialDeliveryTest, RefusedAndNonproviderDeliveriesDoNotInventCallMetadata)
{
    PlayerbotSocialDeliveryRequest refused;
    refused.origin = PlayerbotSocialEventOrigin::Social;
    refused.callMetadata = CallMetadata();

    PlayerbotSocialSpeaker speaker;
    speaker.botGuidCounter = 500;

    PlayerbotSocialDelivery untouched;
    EXPECT_FALSE(PlayerbotSocialDeliveryRecordFor(refused, speaker, false, 8000, untouched));

    for (PlayerbotSocialEventOrigin const origin :
         {PlayerbotSocialEventOrigin::Legacy, PlayerbotSocialEventOrigin::CombatStatus,
          PlayerbotSocialEventOrigin::PartyStatus, PlayerbotSocialEventOrigin::Assistance,
          PlayerbotSocialEventOrigin::Pvp})
    {
        PlayerbotSocialDeliveryRequest request;
        request.origin = origin;
        request.channel = PlayerbotSocialChannel::Say;
        request.text = "status";
        request.callMetadata = CallMetadata();

        PlayerbotSocialDelivery delivery;
        ASSERT_TRUE(PlayerbotSocialDeliveryRecordFor(request, speaker, true, 8000, delivery));
        EXPECT_FALSE(delivery.callMetadata.has_value());
        EXPECT_TRUE(PlayerbotSocialMakeDeliveryEvent(delivery).diagnosticsJson.empty());
    }
}

// Output shape ---------------------------------------------------------------------------------

TEST(PlayerbotSocialDeliveryTest, AConciseLineOnTheRequestedChannelIsDeliverable)
{
    EXPECT_EQ(PlayerbotSocialValidateOutput(Message("Aye, that pack hits hard.", PlayerbotSocialChannel::Party),
                                            PlayerbotSocialChannel::Party),
              PlayerbotSocialDeliveryRejection::None);
}

TEST(PlayerbotSocialDeliveryTest, ContributionFunctionAndCitationsMustBeCoherent)
{
    PlayerbotSocialProviderResult answer = Message("Aye.", PlayerbotSocialChannel::Party);
    answer.contribution = PlayerbotSocialContributionFunction::Answer;
    EXPECT_EQ(PlayerbotSocialValidateOutput(answer, PlayerbotSocialChannel::Party),
              PlayerbotSocialDeliveryRejection::UnsupportedClaim);

    PlayerbotSocialProviderResult banter = Message("Aye.", PlayerbotSocialChannel::Party);
    banter.claimSubject = PlayerbotSocialClaimSubject::CandidateBot;
    banter.citedEvidenceIds = {"g1"};
    EXPECT_EQ(PlayerbotSocialValidateOutput(banter, PlayerbotSocialChannel::Party),
              PlayerbotSocialDeliveryRejection::UnsupportedClaim);
}

TEST(PlayerbotSocialDeliveryTest, SilenceIsAnAnswerRatherThanARejection)
{
    // A bot declining to speak is explicitly allowed. Reporting it as a rejection would make an
    // ordinary decision look like a failure in the telemetry.
    PlayerbotSocialProviderResult quiet;
    quiet.kind = PlayerbotSocialOutputKind::Silence;

    EXPECT_EQ(PlayerbotSocialValidateOutput(quiet, PlayerbotSocialChannel::General),
              PlayerbotSocialDeliveryRejection::None);
}

TEST(PlayerbotSocialGroundedProposalTest, CitationsAreRecheckedAgainstCurrentAuthoritativeState)
{
    PlayerbotSocialGroundingEnvelope const original = Grounding();
    PlayerbotSocialProviderResult proposal = Message("I'm level 32.", PlayerbotSocialChannel::Say);
    proposal.contribution = PlayerbotSocialContributionFunction::Answer;
    proposal.claimSubject = PlayerbotSocialClaimSubject::CandidateBot;
    proposal.citedEvidenceIds = {EvidenceId(original, PlayerbotSocialEvidenceFactKind::Level)};

    EXPECT_EQ(
        PlayerbotSocialValidateGroundedProposal(proposal, original, Grounding(), PlayerbotSocialChannel::Say, true),
        PlayerbotSocialDeliveryRejection::None);
    EXPECT_EQ(
        PlayerbotSocialValidateGroundedProposal(proposal, original, Grounding(33), PlayerbotSocialChannel::Say, true),
        PlayerbotSocialDeliveryRejection::EvidenceChanged);

    proposal.citedEvidenceIds = {"g99"};
    EXPECT_EQ(
        PlayerbotSocialValidateGroundedProposal(proposal, original, Grounding(), PlayerbotSocialChannel::Say, true),
        PlayerbotSocialDeliveryRejection::UnknownEvidence);
}

TEST(PlayerbotSocialGroundedProposalTest, LocationGroupTargetAndPrivacyChangesFailClosed)
{
    PlayerbotSocialGroundingEnvelope original = Grounding();
    PlayerbotSocialProviderResult proposal = Message("Still here.", PlayerbotSocialChannel::Say);
    proposal.contribution = PlayerbotSocialContributionFunction::SpecificReaction;
    proposal.claimSubject = PlayerbotSocialClaimSubject::CandidateBot;

    for (auto const& [fact, current] :
         std::initializer_list<std::pair<PlayerbotSocialEvidenceFactKind, PlayerbotSocialGroundingEnvelope>>{
             {PlayerbotSocialEvidenceFactKind::Zone, Grounding(32, "Westfall")},
             {PlayerbotSocialEvidenceFactKind::GroupRelation, Grounding(32, "Elwynn Forest", "not_same_party")},
             {PlayerbotSocialEvidenceFactKind::Target, Grounding(32, "Elwynn Forest", "same_party", "Forest Spider")}})
    {
        proposal.citedEvidenceIds = {EvidenceId(original, fact)};
        EXPECT_EQ(
            PlayerbotSocialValidateGroundedProposal(proposal, original, current, PlayerbotSocialChannel::Say, false),
            PlayerbotSocialDeliveryRejection::EvidenceChanged);
    }

    std::string const levelId = EvidenceId(original, PlayerbotSocialEvidenceFactKind::Level);
    auto const level =
        std::find_if(original.entries.begin(), original.entries.end(),
                     [&levelId](PlayerbotSocialEvidenceEntry const& entry) { return entry.id == levelId; });
    ASSERT_NE(level, original.entries.end());
    level->scope = PlayerbotSocialPrivacyScope::Whisper;
    proposal.citedEvidenceIds = {levelId};
    EXPECT_EQ(
        PlayerbotSocialValidateGroundedProposal(proposal, original, Grounding(), PlayerbotSocialChannel::Say, false),
        PlayerbotSocialDeliveryRejection::EvidenceScopeMismatch);
}

TEST(PlayerbotSocialGroundedProposalTest, ContributionMustFitTheCurrentConversation)
{
    PlayerbotSocialGroundingEnvelope const grounding = Grounding();
    PlayerbotSocialProviderResult proposal = Message("Aye.", PlayerbotSocialChannel::Say);

    proposal.contribution = PlayerbotSocialContributionFunction::Answer;
    EXPECT_EQ(
        PlayerbotSocialValidateGroundedProposal(proposal, grounding, Grounding(), PlayerbotSocialChannel::Say, false),
        PlayerbotSocialDeliveryRejection::IrrelevantContribution);

    proposal.contribution = PlayerbotSocialContributionFunction::FactFreeBanter;
    EXPECT_EQ(
        PlayerbotSocialValidateGroundedProposal(proposal, grounding, Grounding(), PlayerbotSocialChannel::Say, false),
        PlayerbotSocialDeliveryRejection::None);

    proposal.claimSubject = PlayerbotSocialClaimSubject::CandidateBot;
    EXPECT_EQ(
        PlayerbotSocialValidateGroundedProposal(proposal, grounding, Grounding(), PlayerbotSocialChannel::Say, false),
        PlayerbotSocialDeliveryRejection::UnsupportedClaim);
}

TEST(PlayerbotSocialGroundingTest, BaselineFactsAreTypedBoundedAndOwnedByTheirSubject)
{
    PlayerbotSocialGroundingInput input;
    input.nowUnixSeconds = 1000;
    input.activeContentExpansion = 0;
    input.profileLoadState = PlayerbotSocialProfileLoadState::Loaded;
    input.memoryInputState = PlayerbotSocialMemoryInputState::Loaded;
    input.bot.guidCounter = 500;
    input.bot.name = "Barnek";
    input.bot.race = "Dwarf";
    input.bot.characterClass = "Warrior";
    input.bot.level = 32;
    input.bot.faction = "Alliance";
    input.bot.zone = "Elwynn Forest";
    input.bot.area = "Goldshire";
    input.bot.inCombat = false;
    input.bot.visibleTarget = "Kobold Miner";
    input.participant.guidCounter = 900;
    input.participant.name = "Elyse";
    input.participant.visible = true;
    input.participant.inRange = true;
    input.participant.level = 31;
    input.participant.groupRelation = "same_party";
    input.transcriptEventPublicIds.push_back(PlayerbotSocialMakeEventPublicId(80, 900));

    PlayerbotSocialGroundingEnvelope const envelope = PlayerbotSocialBuildGroundingEnvelope(input);

    ASSERT_EQ(envelope.refusal, PlayerbotSocialGroundingRefusal::None);
    EXPECT_TRUE(PlayerbotSocialGroundingEnvelopeIsValid(envelope));
    EXPECT_EQ(envelope.profileLoadState, PlayerbotSocialProfileLoadState::Loaded);
    EXPECT_EQ(envelope.memoryInputState, PlayerbotSocialMemoryInputState::Loaded);
    EXPECT_EQ(envelope.transcriptEventPublicIds, input.transcriptEventPublicIds);
    EXPECT_TRUE(std::any_of(envelope.entries.begin(), envelope.entries.end(),
                            [](PlayerbotSocialEvidenceEntry const& entry)
                            {
                                return entry.subjectRole == PlayerbotSocialEvidenceSubjectRole::CandidateBot &&
                                       entry.factKind == PlayerbotSocialEvidenceFactKind::Level && entry.value == "32";
                            }));
    EXPECT_TRUE(std::any_of(envelope.entries.begin(), envelope.entries.end(),
                            [](PlayerbotSocialEvidenceEntry const& entry)
                            {
                                return entry.subjectRole == PlayerbotSocialEvidenceSubjectRole::Participant &&
                                       entry.factKind == PlayerbotSocialEvidenceFactKind::GroupRelation &&
                                       entry.value == "same_party";
                            }));
}

TEST(PlayerbotSocialGroundingTest, InvisibleParticipantFactsAndUnrelatedStateAreOmitted)
{
    PlayerbotSocialGroundingInput input;
    input.nowUnixSeconds = 1000;
    input.bot.guidCounter = 500;
    input.bot.name = "Barnek";
    input.participant.guidCounter = 900;
    input.participant.name = "Elyse";
    input.participant.visible = false;
    input.participant.level = 31;
    input.participant.visibleTarget = "Forest Spider";

    PlayerbotSocialGroundingEnvelope const envelope = PlayerbotSocialBuildGroundingEnvelope(input);
    ASSERT_EQ(envelope.refusal, PlayerbotSocialGroundingRefusal::None);
    EXPECT_FALSE(std::any_of(envelope.entries.begin(), envelope.entries.end(),
                             [](PlayerbotSocialEvidenceEntry const& entry)
                             {
                                 return entry.subjectRole == PlayerbotSocialEvidenceSubjectRole::Participant &&
                                        (entry.factKind == PlayerbotSocialEvidenceFactKind::Level ||
                                         entry.factKind == PlayerbotSocialEvidenceFactKind::Target);
                             }));
}

TEST(PlayerbotSocialGroundingTest, DuplicateOversizedAndConflictingEvidenceFailClosed)
{
    PlayerbotSocialGroundingEnvelope duplicate;
    PlayerbotSocialEvidenceEntry entry;
    entry.id = "g1";
    entry.subjectRole = PlayerbotSocialEvidenceSubjectRole::CandidateBot;
    entry.subjectGuidCounter = 500;
    entry.factKind = PlayerbotSocialEvidenceFactKind::Name;
    entry.value = "Barnek";
    entry.provenance = PlayerbotSocialEvidenceProvenance::CurrentWorld;
    entry.atUnixSeconds = 1000;
    duplicate.entries = {entry, entry};
    EXPECT_FALSE(PlayerbotSocialGroundingEnvelopeIsValid(duplicate));

    PlayerbotSocialGroundingInput oversized;
    oversized.bot.guidCounter = 500;
    oversized.bot.name.assign(PLAYERBOT_SOCIAL_EVIDENCE_VALUE_MAX_BYTES + 1, 'x');
    EXPECT_EQ(PlayerbotSocialBuildGroundingEnvelope(oversized).refusal, PlayerbotSocialGroundingRefusal::EntryTooLong);

    PlayerbotSocialGroundingInput conflict;
    conflict.bot.guidCounter = 500;
    conflict.bot.name = "Barnek";
    conflict.bot.level = 32;
    PlayerbotSocialEvidenceEntry source = entry;
    source.id.clear();
    source.factKind = PlayerbotSocialEvidenceFactKind::Level;
    source.value = "31";
    source.provenance = PlayerbotSocialEvidenceProvenance::AuthoritativeSource;
    conflict.sourceFacts.push_back(source);
    EXPECT_EQ(PlayerbotSocialBuildGroundingEnvelope(conflict).refusal,
              PlayerbotSocialGroundingRefusal::ConflictingFact);
}

TEST(PlayerbotSocialGroundingTest, EntryAndEnvelopeBoundsHaveDistinctRefusals)
{
    PlayerbotSocialGroundingInput tooMany;
    tooMany.bot.guidCounter = 500;
    tooMany.bot.name = "Barnek";
    tooMany.nowUnixSeconds = 1000;
    for (std::size_t index = 0; index < PLAYERBOT_SOCIAL_EVIDENCE_MAX_ENTRIES; ++index)
    {
        PlayerbotSocialEvidenceEntry source;
        source.subjectRole = PlayerbotSocialEvidenceSubjectRole::Source;
        source.subjectGuidCounter = 1000 + index;
        source.factKind = PlayerbotSocialEvidenceFactKind::Objective;
        source.value = "objective";
        source.provenance = PlayerbotSocialEvidenceProvenance::AuthoritativeSource;
        source.atUnixSeconds = 1000;
        tooMany.sourceFacts.push_back(std::move(source));
    }
    EXPECT_EQ(PlayerbotSocialBuildGroundingEnvelope(tooMany).refusal, PlayerbotSocialGroundingRefusal::EntryCount);

    PlayerbotSocialGroundingInput tooLarge;
    tooLarge.bot.guidCounter = 500;
    tooLarge.bot.name = "Barnek";
    tooLarge.nowUnixSeconds = 1000;
    for (uint64 index = 0; index < 20; ++index)
    {
        PlayerbotSocialEvidenceEntry source;
        source.subjectRole = PlayerbotSocialEvidenceSubjectRole::Source;
        source.subjectGuidCounter = 2000 + index;
        source.factKind = PlayerbotSocialEvidenceFactKind::Objective;
        source.value = std::string(PLAYERBOT_SOCIAL_EVIDENCE_VALUE_MAX_BYTES, static_cast<char>('a' + index));
        source.provenance = PlayerbotSocialEvidenceProvenance::AuthoritativeSource;
        source.atUnixSeconds = 1000;
        tooLarge.sourceFacts.push_back(std::move(source));
    }
    EXPECT_EQ(PlayerbotSocialBuildGroundingEnvelope(tooLarge).refusal,
              PlayerbotSocialGroundingRefusal::EnvelopeTooLarge);
}

TEST(PlayerbotSocialGroundingTest, TranscriptAndMemoryHealthNeverMintFactualEvidence)
{
    PlayerbotSocialGroundingInput input;
    input.bot.guidCounter = 500;
    input.bot.name = "Barnek";
    input.nowUnixSeconds = 1000;

    PlayerbotSocialGroundingEnvelope const baseline = PlayerbotSocialBuildGroundingEnvelope(input);
    ASSERT_EQ(baseline.refusal, PlayerbotSocialGroundingRefusal::None);

    input.transcriptEventPublicIds = {PlayerbotSocialMakeEventPublicId(90, 700)};
    input.memoryInputState = PlayerbotSocialMemoryInputState::Loaded;
    PlayerbotSocialGroundingEnvelope const withGeneratedInputs = PlayerbotSocialBuildGroundingEnvelope(input);

    ASSERT_EQ(withGeneratedInputs.refusal, PlayerbotSocialGroundingRefusal::None);
    EXPECT_EQ(withGeneratedInputs.entries.size(), baseline.entries.size());
    for (std::size_t index = 0; index < baseline.entries.size(); ++index)
    {
        EXPECT_EQ(withGeneratedInputs.entries[index].factKind, baseline.entries[index].factKind);
        EXPECT_EQ(withGeneratedInputs.entries[index].value, baseline.entries[index].value);
        EXPECT_EQ(withGeneratedInputs.entries[index].provenance, baseline.entries[index].provenance);
    }
    EXPECT_EQ(withGeneratedInputs.memoryInputState, PlayerbotSocialMemoryInputState::Loaded);
    EXPECT_EQ(withGeneratedInputs.transcriptEventPublicIds, input.transcriptEventPublicIds);
}

TEST(PlayerbotSocialDeliveryTest, OneGenerationNeverBecomesSeveralMessages)
{
    // Key Decision 4. Each of these would turn into a burst downstream, or is the model writing a
    // whole exchange rather than one bot's remark.
    for (std::string const& scripted :
         {std::string("First line\nSecond line"), std::string("First\rSecond"), std::string("First\\nSecond"),
          std::string("one || two"), std::string("one<br>two"), std::string("one&#10;two")})
    {
        EXPECT_EQ(
            PlayerbotSocialValidateOutput(Message(scripted, PlayerbotSocialChannel::Say), PlayerbotSocialChannel::Say),
            PlayerbotSocialDeliveryRejection::BurstDelimiter)
            << "accepted: " << scripted;
    }
}

TEST(PlayerbotSocialDeliveryTest, AnOverlongLineIsRefusedAtItsBoundary)
{
    std::string const atTheLimit(PLAYERBOT_SOCIAL_MAX_OUTPUT_LENGTH, 'a');
    std::string const oneOver(PLAYERBOT_SOCIAL_MAX_OUTPUT_LENGTH + 1, 'a');

    EXPECT_EQ(
        PlayerbotSocialValidateOutput(Message(atTheLimit, PlayerbotSocialChannel::Say), PlayerbotSocialChannel::Say),
        PlayerbotSocialDeliveryRejection::None);
    EXPECT_EQ(PlayerbotSocialValidateOutput(Message(oneOver, PlayerbotSocialChannel::Say), PlayerbotSocialChannel::Say),
              PlayerbotSocialDeliveryRejection::TooLong);

    // 255 written out rather than read from the constant, so moving the bound fails this test.
    EXPECT_EQ(PLAYERBOT_SOCIAL_MAX_OUTPUT_LENGTH, std::size_t{255});
}

TEST(PlayerbotSocialDeliveryTest, AnEmptyLineIsNotAMessage)
{
    EXPECT_EQ(PlayerbotSocialValidateOutput(Message("", PlayerbotSocialChannel::Say), PlayerbotSocialChannel::Say),
              PlayerbotSocialDeliveryRejection::EmptyOutput);
}

TEST(PlayerbotSocialDeliveryTest, SpeakingOnADifferentChannelIsRefusedNotRedirected)
{
    /*
     * Honouring the result's own channel is how a party remark reaches a zone channel. The privacy
     * scope of the context that produced the line was decided by the channel the bot was ASKED to
     * speak on, so a result naming another one is not a delivery to fix up.
     */
    EXPECT_EQ(
        PlayerbotSocialValidateOutput(Message("hello", PlayerbotSocialChannel::General), PlayerbotSocialChannel::Party),
        PlayerbotSocialDeliveryRejection::ChannelSwitch);
}

TEST(PlayerbotSocialDeliveryTest, AnUnknownChannelOrKindFailsClosed)
{
    // Neither -Wswitch nor -Werror is on in this build, so a value cast in from a payload reaches
    // here unchallenged and must be refused rather than treated as some default channel.
    EXPECT_EQ(PlayerbotSocialValidateOutput(Message("hello", static_cast<PlayerbotSocialChannel>(200)),
                                            PlayerbotSocialChannel::Say),
              PlayerbotSocialDeliveryRejection::UnsupportedChannel);

    PlayerbotSocialProviderResult odd = Message("hello", PlayerbotSocialChannel::Say);
    odd.kind = static_cast<PlayerbotSocialOutputKind>(77);
    EXPECT_EQ(PlayerbotSocialValidateOutput(odd, PlayerbotSocialChannel::Say),
              PlayerbotSocialDeliveryRejection::EmptyOutput);
}

TEST(PlayerbotSocialDeliveryTest, EmotesAreLegalOnlyWhereSomeoneCanSeeThem)
{
    // Definition of Done 3. General is zone wide and whisper has no physical presence, so neither
    // can carry a gesture.
    EXPECT_EQ(PlayerbotSocialValidateOutput(EmoteResult(4, PlayerbotSocialChannel::Say), PlayerbotSocialChannel::Say),
              PlayerbotSocialDeliveryRejection::None);
    EXPECT_EQ(
        PlayerbotSocialValidateOutput(EmoteResult(4, PlayerbotSocialChannel::Party), PlayerbotSocialChannel::Party),
        PlayerbotSocialDeliveryRejection::None);
    EXPECT_EQ(
        PlayerbotSocialValidateOutput(EmoteResult(4, PlayerbotSocialChannel::General), PlayerbotSocialChannel::General),
        PlayerbotSocialDeliveryRejection::EmoteChannelIllegal);
    EXPECT_EQ(
        PlayerbotSocialValidateOutput(EmoteResult(4, PlayerbotSocialChannel::Whisper), PlayerbotSocialChannel::Whisper),
        PlayerbotSocialDeliveryRejection::EmoteChannelIllegal);
}

TEST(PlayerbotSocialDeliveryTest, AnEmoteWithNoGestureIsNotAnEmote)
{
    EXPECT_EQ(PlayerbotSocialValidateOutput(EmoteResult(0, PlayerbotSocialChannel::Say), PlayerbotSocialChannel::Say),
              PlayerbotSocialDeliveryRejection::EmptyOutput);
}

// Revalidation immediately before delivery -------------------------------------------------------

TEST(PlayerbotSocialDeliveryTest, EverythingStillTrueDelivers)
{
    for (PlayerbotSocialChannel const channel : {PlayerbotSocialChannel::General, PlayerbotSocialChannel::Say,
                                                 PlayerbotSocialChannel::Party, PlayerbotSocialChannel::Whisper})
    {
        EXPECT_EQ(PlayerbotSocialRevalidateDelivery(channel, PlayerbotSocialOutputKind::Message, AllHold()),
                  PlayerbotSocialDeliveryRejection::None);
    }
}

TEST(PlayerbotSocialDeliveryTest, AStaleThreadDropsTheResultOnEveryChannel)
{
    // Definition of Done 1. The answer was fine when it was asked for and is a non sequitur now.
    PlayerbotSocialDeliveryConditions moved = AllHold();
    moved.threadStillCurrent = false;

    for (PlayerbotSocialChannel const channel : {PlayerbotSocialChannel::General, PlayerbotSocialChannel::Say,
                                                 PlayerbotSocialChannel::Party, PlayerbotSocialChannel::Whisper})
    {
        EXPECT_EQ(PlayerbotSocialRevalidateDelivery(channel, PlayerbotSocialOutputKind::Message, moved),
                  PlayerbotSocialDeliveryRejection::SupersededThread);
    }
}

TEST(PlayerbotSocialDeliveryTest, ASpeakerWhoLoggedOutOrDiedSaysNothing)
{
    PlayerbotSocialDeliveryConditions goneAway = AllHold();
    goneAway.speakerOnline = false;
    EXPECT_EQ(
        PlayerbotSocialRevalidateDelivery(PlayerbotSocialChannel::Say, PlayerbotSocialOutputKind::Message, goneAway),
        PlayerbotSocialDeliveryRejection::SpeakerGone);

    PlayerbotSocialDeliveryConditions dead = AllHold();
    dead.speakerAlive = false;
    EXPECT_EQ(PlayerbotSocialRevalidateDelivery(PlayerbotSocialChannel::Say, PlayerbotSocialOutputKind::Message, dead),
              PlayerbotSocialDeliveryRejection::SpeakerDead);
}

TEST(PlayerbotSocialDeliveryTest, ConsentWithdrawnBetweenRequestAndDeliveryStopsIt)
{
    PlayerbotSocialDeliveryConditions withdrawn = AllHold();
    withdrawn.consentHolds = false;

    EXPECT_EQ(
        PlayerbotSocialRevalidateDelivery(PlayerbotSocialChannel::Party, PlayerbotSocialOutputKind::Message, withdrawn),
        PlayerbotSocialDeliveryRejection::ConsentWithdrawn);
}

TEST(PlayerbotSocialDeliveryTest, EachChannelChecksOnlyWhatIsAuthoritativeForIt)
{
    /*
     * Definition of Done 2. Party membership says nothing about a zone channel and range says
     * nothing about a party roster, so a delivery must not fail for a condition its channel does not
     * use. That would read as a bug rather than a rule, and it would report the wrong reason.
     */
    PlayerbotSocialDeliveryConditions spatiallyLost = AllHold();
    spatiallyLost.withinRange = false;
    spatiallyLost.targetVisible = false;
    spatiallyLost.sameMap = false;

    // General is zone wide: two characters in the same channel may be a continent apart within it.
    EXPECT_EQ(PlayerbotSocialRevalidateDelivery(PlayerbotSocialChannel::General, PlayerbotSocialOutputKind::Message,
                                                spatiallyLost),
              PlayerbotSocialDeliveryRejection::None);

    // Whisper is directed and has no spatial requirement either.
    EXPECT_EQ(PlayerbotSocialRevalidateDelivery(PlayerbotSocialChannel::Whisper, PlayerbotSocialOutputKind::Message,
                                                spatiallyLost),
              PlayerbotSocialDeliveryRejection::None);

    PlayerbotSocialDeliveryConditions ungrouped = AllHold();
    ungrouped.inSameGroup = false;

    // But a zone channel does not care about the group, and say does not either.
    EXPECT_EQ(PlayerbotSocialRevalidateDelivery(PlayerbotSocialChannel::General, PlayerbotSocialOutputKind::Message,
                                                ungrouped),
              PlayerbotSocialDeliveryRejection::None);
    EXPECT_EQ(
        PlayerbotSocialRevalidateDelivery(PlayerbotSocialChannel::Say, PlayerbotSocialOutputKind::Message, ungrouped),
        PlayerbotSocialDeliveryRejection::None);
}

TEST(PlayerbotSocialDeliveryTest, GeneralNeedsTheChannelAndNothingSpatial)
{
    PlayerbotSocialDeliveryConditions left = AllHold();
    left.inChannel = false;

    EXPECT_EQ(
        PlayerbotSocialRevalidateDelivery(PlayerbotSocialChannel::General, PlayerbotSocialOutputKind::Message, left),
        PlayerbotSocialDeliveryRejection::NotInChannel);
}

TEST(PlayerbotSocialDeliveryTest, SayNeedsLocalVisibility)
{
    for (auto const& [name, mutate, expected] :
         std::initializer_list<
             std::tuple<char const*, void (*)(PlayerbotSocialDeliveryConditions&), PlayerbotSocialDeliveryRejection>>{
             {"map", [](PlayerbotSocialDeliveryConditions& c) { c.sameMap = false; },
              PlayerbotSocialDeliveryRejection::DifferentMap},
             {"phase", [](PlayerbotSocialDeliveryConditions& c) { c.samePhase = false; },
              PlayerbotSocialDeliveryRejection::DifferentPhase},
             {"range", [](PlayerbotSocialDeliveryConditions& c) { c.withinRange = false; },
              PlayerbotSocialDeliveryRejection::OutOfRange},
             {"visibility", [](PlayerbotSocialDeliveryConditions& c) { c.targetVisible = false; },
              PlayerbotSocialDeliveryRejection::NotVisible}})
    {
        PlayerbotSocialDeliveryConditions conditions = AllHold();
        mutate(conditions);

        EXPECT_EQ(PlayerbotSocialRevalidateDelivery(PlayerbotSocialChannel::Say, PlayerbotSocialOutputKind::Message,
                                                    conditions),
                  expected)
            << "condition: " << name;
    }
}

TEST(PlayerbotSocialDeliveryTest, PartyNeedsCurrentGroupMembership)
{
    PlayerbotSocialDeliveryConditions leftGroup = AllHold();
    leftGroup.inSameGroup = false;

    EXPECT_EQ(
        PlayerbotSocialRevalidateDelivery(PlayerbotSocialChannel::Party, PlayerbotSocialOutputKind::Message, leftGroup),
        PlayerbotSocialDeliveryRejection::NotInGroup);
}

TEST(PlayerbotSocialDeliveryTest, WhisperNeedsTheTargetStillThere)
{
    PlayerbotSocialDeliveryConditions targetLeft = AllHold();
    targetLeft.targetOnline = false;

    EXPECT_EQ(PlayerbotSocialRevalidateDelivery(PlayerbotSocialChannel::Whisper, PlayerbotSocialOutputKind::Message,
                                                targetLeft),
              PlayerbotSocialDeliveryRejection::TargetGone);
}

TEST(PlayerbotSocialDeliveryTest, APartyEmoteStillHasToBeSeenEvenThoughAPartyMessageDoesNot)
{
    /*
     * A party is a roster, not a place: members can be spread across a zone. A remark reaches all of
     * them, but a gesture only means anything to somebody who can see it, so the same channel holds
     * a message and refuses the emote.
     */
    PlayerbotSocialDeliveryConditions apart = AllHold();
    apart.withinRange = false;

    EXPECT_EQ(
        PlayerbotSocialRevalidateDelivery(PlayerbotSocialChannel::Party, PlayerbotSocialOutputKind::Message, apart),
        PlayerbotSocialDeliveryRejection::None);
    EXPECT_EQ(PlayerbotSocialRevalidateDelivery(PlayerbotSocialChannel::Party, PlayerbotSocialOutputKind::Emote, apart),
              PlayerbotSocialDeliveryRejection::EmoteTargetDistant);
}

TEST(PlayerbotSocialDeliveryTest, AnUnknownChannelIsNotDelivered)
{
    EXPECT_EQ(PlayerbotSocialRevalidateDelivery(static_cast<PlayerbotSocialChannel>(200),
                                                PlayerbotSocialOutputKind::Message, AllHold()),
              PlayerbotSocialDeliveryRejection::UnsupportedChannel);
}

// Natural delay --------------------------------------------------------------------------------

TEST(PlayerbotSocialDeliveryTest, EveryDelayLandsInsideTheWindow)
{
    // No combination of length and roll may produce an instant reply or one so late the
    // conversation has moved on.
    for (std::size_t length : {std::size_t{0}, std::size_t{1}, std::size_t{120}, PLAYERBOT_SOCIAL_MAX_OUTPUT_LENGTH,
                               PLAYERBOT_SOCIAL_MAX_OUTPUT_LENGTH * 10})
    {
        for (uint32 roll : {0u, 1u, 12345u, 999983u, UINT32_MAX})
        {
            uint32 const delay = PlayerbotSocialDeliveryDelayMs(length, roll);

            EXPECT_GE(delay, PLAYERBOT_SOCIAL_DELIVERY_DELAY_MIN_MS) << length << "/" << roll;
            EXPECT_LE(delay, PLAYERBOT_SOCIAL_DELIVERY_DELAY_MAX_MS) << length << "/" << roll;
        }
    }
}

TEST(PlayerbotSocialDeliveryTest, DeliveryClockUsesUnixEpochMillisecondsRatherThanServerUptime)
{
    constexpr int64 UNIX_MILLISECONDS = 1725000123456;
    std::chrono::system_clock::time_point const now{std::chrono::milliseconds{UNIX_MILLISECONDS}};

    EXPECT_EQ(PlayerbotSocialUnixMilliseconds(now), static_cast<uint64>(UNIX_MILLISECONDS));
}

TEST(PlayerbotSocialDeliveryTest, ALongerRemarkNeverArrivesSoonerThanAShortOne)
{
    // Same roll, so the only difference is how much there was to type.
    uint32 const roll = 7;

    EXPECT_LE(PlayerbotSocialDeliveryDelayMs(0, roll), PlayerbotSocialDeliveryDelayMs(120, roll));
    EXPECT_LE(PlayerbotSocialDeliveryDelayMs(120, roll),
              PlayerbotSocialDeliveryDelayMs(PLAYERBOT_SOCIAL_MAX_OUTPUT_LENGTH, roll));
}

TEST(PlayerbotSocialDeliveryTest, TwoBotsAnsweringTheSameInstantDoNotSpeakInUnison)
{
    // The whole point of the roll. Bots that all pause for exactly the same time read worse than
    // bots that answer immediately.
    EXPECT_NE(PlayerbotSocialDeliveryDelayMs(40, 3), PlayerbotSocialDeliveryDelayMs(40, 900));
}

TEST(PlayerbotSocialDeliveryTest, TheDelayWindowIsAConversationalPause)
{
    // Literals rather than the constants, so retuning the window fails this deliberately.
    EXPECT_EQ(PLAYERBOT_SOCIAL_DELIVERY_DELAY_MIN_MS, 1200u);
    EXPECT_EQ(PLAYERBOT_SOCIAL_DELIVERY_DELAY_MAX_MS, 6500u);
}

// Refusal reporting ----------------------------------------------------------------------------

TEST(PlayerbotSocialDeliveryTest, EveryRejectionHasItsOwnName)
{
    // Definition of Done 1 wants an explicit reason. Two reasons sharing a name, or a name reported
    // for a value outside the enum, would make the telemetry lie about which rule fired.
    std::set<std::string> names;

    for (uint8 raw = 0; raw <= static_cast<uint8>(PlayerbotSocialDeliveryRejection::LockedProgressionContent); ++raw)
    {
        auto const rejection = static_cast<PlayerbotSocialDeliveryRejection>(raw);
        ASSERT_TRUE(PlayerbotSocialDeliveryRejectionIsValid(rejection)) << "gap at " << static_cast<int>(raw);
        EXPECT_TRUE(names.insert(PlayerbotSocialDeliveryRejectionName(rejection)).second)
            << "duplicate name at " << static_cast<int>(raw);
    }

    auto const outside = static_cast<PlayerbotSocialDeliveryRejection>(200);
    EXPECT_FALSE(PlayerbotSocialDeliveryRejectionIsValid(outside));
    EXPECT_STREQ(PlayerbotSocialDeliveryRejectionName(outside), "unknown");
}

// The request lifecycle in the coordinator --------------------------------------------------------

namespace
{
// Records what it was asked for and answers whatever the test told it to.
class RecordingProvider : public PlayerbotSocialProvider
{
public:
    bool accept = true;
    std::vector<uint64> submitted;
    std::vector<uint64> submittedBots;
    std::vector<uint64> submittedTargets;
    std::vector<std::string> submittedThreads;
    std::vector<std::string> submittedSubjects;
    std::vector<PlayerbotSocialRequestPriority> submittedPriorities;
    std::vector<PlayerbotSocialRequestContext> submittedContexts;

    bool Submit(uint64 requestToken, uint64 botGuidCounter, uint64 targetGuidCounter,
                PlayerbotSocialChannel /*channel*/, std::string const& threadPublicId,
                PlayerbotSocialRequestPriority priority, PlayerbotSocialRequestContext const& context) override
    {
        if (!accept)
            return false;

        submitted.push_back(requestToken);
        submittedBots.push_back(botGuidCounter);
        submittedTargets.push_back(targetGuidCounter);
        submittedThreads.push_back(threadPublicId);
        submittedSubjects.push_back(context.starter);
        submittedPriorities.push_back(priority);
        submittedContexts.push_back(context);
        return true;
    }

    // Refused rather than recorded. This file is about chat delivery, and a double that
    // silently accepted a biography would let a stray request pass unnoticed here instead of
    // failing in the file that actually asserts the biography lifecycle.
    bool SubmitBiography(uint64 /*biographyRequestToken*/, uint64 /*botGuidCounter*/,
                         std::string const& /*characterName*/, uint8 /*raceId*/, uint8 /*classId*/,
                         uint8 /*genderId*/) override
    {
        return false;
    }

    // Refused for the same reason, and it matters more here: an extraction request carries raw
    // chat, so a double that quietly accepted one would let this file be the place a leak into
    // a provider went unnoticed.
    bool SubmitMemory(uint64 /*memoryRequestToken*/, uint64 /*botGuidCounter*/, std::string const& /*threadPublicId*/,
                      PlayerbotSocialPrivacyScope /*scope*/, std::vector<uint64> const& /*subjectGuidCounters*/,
                      std::vector<PlayerbotSocialMemoryLine> const& /*thread*/) override
    {
        return false;
    }
};

// A well formed thread identity. It is validated at admission now, so a placeholder will not do.
std::string ThreadId() { return "thr_00000000000000000000000000000001"; }
std::string OtherThreadId() { return "thr_00000000000000000000000000000002"; }
std::string ThirdThreadId() { return "thr_00000000000000000000000000000003"; }

/*
 * The one zone every request in this file opens in.
 *
 * Named rather than repeated, because the zone is now carried from the request through to each
 * conclusion event: a literal per call site would let the producer and the expectation drift
 * apart while both still read plausibly.
 */
constexpr uint32 REQUEST_ZONE_ID = 12;

uint64 OpenRequest(PlayerbotSocialMgr& coordinator, uint64 bot, PlayerbotSocialChannel channel, uint64 now,
                   PlayerbotSocialDeliveryRejection* observedRejection = nullptr)
{
    PlayerbotSocialObservation observation;
    observation.key.channel = channel;
    observation.key.scopeId = 42;
    observation.eventPublicId = PlayerbotSocialMakeEventPublicId(now + bot, 900);
    observation.speakerGuidCounter = 900;
    observation.speakerName = "Elyse";
    observation.zoneId = REQUEST_ZONE_ID;
    observation.atUnixSeconds = now;
    observation.text = "A current fixture line.";
    PlayerbotSocialThreadHandle const thread = coordinator.Observe(observation);

    PlayerbotSocialPromptLine currentLine;
    currentLine.eventPublicId = observation.eventPublicId;
    currentLine.role = observation.role;
    currentLine.speakerGuidCounter = observation.speakerGuidCounter;
    currentLine.speakerName = observation.speakerName;
    currentLine.atUnixSeconds = observation.atUnixSeconds;
    currentLine.text = observation.text;

    PlayerbotSocialGroundingEnvelope grounding = Grounding();
    grounding.transcriptEventPublicIds = {observation.eventPublicId};
    PlayerbotSocialDeliveryRejection rejection = PlayerbotSocialDeliveryRejection::None;
    uint64 const token = coordinator.BeginSocialRequest(
        bot, StoredPersonality(), 900, channel, thread.publicId, PlayerbotSocialRequestPriority::DirectHumanEngagement,
        now, REQUEST_ZONE_ID, std::string(), rejection, {}, 900, true, currentLine, false,
        PlayerbotRoleplayPromptMode::Ordinary, grounding);
    if (observedRejection != nullptr)
        *observedRejection = rejection;
    return token;
}

uint64 OpenStarterRequest(PlayerbotSocialMgr& coordinator, uint64 bot, uint64 target, PlayerbotSocialChannel channel,
                          std::string const& threadPublicId, PlayerbotSocialRequestPriority priority, uint64 now,
                          PlayerbotSocialDeliveryRejection& rejection, std::string starterSubject = "fixture subject")
{
    // A starter with no travelling target grounds room-addressed, exactly as the route builds it:
    // Participant evidence may only appear when its subject travels on the request.
    return coordinator.BeginSocialRequest(bot, StoredPersonality(), target, channel, threadPublicId, priority, now,
                                          REQUEST_ZONE_ID, std::move(starterSubject), rejection, {}, target,
                                          target != 0, {}, false, PlayerbotRoleplayPromptMode::Ordinary,
                                          target != 0 ? Grounding() : RoomGrounding());
}

struct GroundedThreadFixture
{
    PlayerbotSocialThreadHandle thread;
    PlayerbotSocialPromptLine currentLine;
    PlayerbotSocialGroundingEnvelope grounding;
    PlayerbotSocialChannel channel = PlayerbotSocialChannel::Say;
};

GroundedThreadFixture OpenGroundedThread(PlayerbotSocialMgr& coordinator,
                                         PlayerbotSocialChannel channel = PlayerbotSocialChannel::Say,
                                         uint64 scopeId = 42)
{
    PlayerbotSocialObservation observation;
    observation.key.channel = channel;
    observation.key.scopeId = scopeId;
    observation.eventPublicId = PlayerbotSocialMakeEventPublicId(scopeId + 100, 900);
    observation.speakerGuidCounter = 900;
    observation.speakerName = "Elyse";
    observation.speakerIsHuman = false;
    observation.zoneId = REQUEST_ZONE_ID;
    observation.atUnixSeconds = 1000;
    observation.text = "That pull got messy.";

    GroundedThreadFixture fixture;
    fixture.thread = coordinator.Observe(observation);
    fixture.channel = channel;
    fixture.grounding = Grounding();
    fixture.grounding.transcriptEventPublicIds = {observation.eventPublicId};
    fixture.currentLine.eventPublicId = observation.eventPublicId;
    fixture.currentLine.role = observation.role;
    fixture.currentLine.speakerGuidCounter = observation.speakerGuidCounter;
    fixture.currentLine.speakerName = observation.speakerName;
    fixture.currentLine.speakerIsHuman = observation.speakerIsHuman;
    fixture.currentLine.atUnixSeconds = observation.atUnixSeconds;
    fixture.currentLine.text = observation.text;
    return fixture;
}

uint64 OpenGroundedRequest(PlayerbotSocialMgr& coordinator, GroundedThreadFixture const& fixture,
                           bool expectsAnswer = false)
{
    PlayerbotSocialDeliveryRejection rejection = PlayerbotSocialDeliveryRejection::None;
    uint64 const target = fixture.channel == PlayerbotSocialChannel::Whisper ? 900 : 0;
    return coordinator.BeginSocialRequest(500, StoredPersonality(), target, fixture.channel, fixture.thread.publicId,
                                          PlayerbotSocialRequestPriority::DirectHumanEngagement, 1000, REQUEST_ZONE_ID,
                                          std::string(), rejection, {}, 900, true, fixture.currentLine, false,
                                          PlayerbotRoleplayPromptMode::Ordinary, fixture.grounding, expectsAnswer);
}

uint64 OpenObservedLineRequest(PlayerbotSocialMgr& coordinator, uint64 bot,
                               PlayerbotPersonalityProfile const& personality, uint64 target,
                               PlayerbotSocialChannel channel, PlayerbotSocialRequestPriority priority, uint64 now,
                               PlayerbotSocialDeliveryRejection& rejection, uint64 subject, bool addressedDirectly,
                               PlayerbotSocialPromptLine currentLine, bool statelessDirectReply = false,
                               PlayerbotRoleplayPromptMode promptMode = PlayerbotRoleplayPromptMode::Ordinary,
                               std::vector<PlayerbotSocialNearbySnapshotEntry> const& nearby = {})
{
    if (currentLine.eventPublicId.empty())
        currentLine.eventPublicId = PlayerbotSocialMakeEventPublicId(now + bot, currentLine.speakerGuidCounter);

    if (currentLine.speakerIsHuman)
        coordinator.ApplyConsentSnapshot(currentLine.speakerGuidCounter, false);

    PlayerbotSocialObservation observation;
    observation.key.channel = channel;
    observation.key.scopeId = 42;
    observation.eventPublicId = currentLine.eventPublicId;
    observation.role = currentLine.role;
    observation.replyToEventPublicId = currentLine.replyToEventPublicId;
    observation.sourceEventPublicId = currentLine.sourceEventPublicId;
    observation.speakerGuidCounter = currentLine.speakerGuidCounter;
    observation.speakerName = currentLine.speakerName;
    observation.speakerIsHuman = currentLine.speakerIsHuman;
    observation.zoneId = REQUEST_ZONE_ID;
    observation.atUnixSeconds = currentLine.atUnixSeconds;
    observation.text = currentLine.text;
    PlayerbotSocialThreadHandle const thread = coordinator.Observe(observation);

    PlayerbotSocialGroundingEnvelope grounding = Grounding();
    grounding.transcriptEventPublicIds = {currentLine.eventPublicId};
    return coordinator.BeginSocialRequest(bot, personality, target, channel, thread.publicId, priority, now,
                                          REQUEST_ZONE_ID, std::string(), rejection, nearby, subject, addressedDirectly,
                                          currentLine, statelessDirectReply, promptMode, grounding);
}

std::vector<PlayerbotSocialEventBinding> EventsOfType(PlayerbotSocialMgr const& coordinator,
                                                      std::string_view eventType);
}  // namespace

TEST(PlayerbotSocialDeliveryTest, RepeatedWordingAndFunctionAreSuppressedWithinOneThread)
{
    PlayerbotSocialDeliveryConditions conditions = AllHold();
    conditions.currentGrounding = Grounding();

    {
        PlayerbotSocialMgr coordinator;
        RecordingProvider provider;
        coordinator.SetSocialProvider(&provider);
        GroundedThreadFixture const fixture = OpenGroundedThread(coordinator);

        uint64 const firstToken = OpenGroundedRequest(coordinator, fixture);
        PlayerbotSocialProviderResult first = Message("Still standing.", fixture.channel);
        first.requestToken = firstToken;
        ASSERT_EQ(coordinator.AcceptSocialResult(first, 100000, 3), PlayerbotSocialDeliveryRejection::None);
        ASSERT_EQ(coordinator.CompleteDelivery(firstToken, conditions), PlayerbotSocialDeliveryRejection::None);

        uint64 const repeatedToken = OpenGroundedRequest(coordinator, fixture);
        PlayerbotSocialProviderResult repeated = first;
        repeated.requestToken = repeatedToken;
        ASSERT_EQ(coordinator.AcceptSocialResult(repeated, 200000, 3), PlayerbotSocialDeliveryRejection::None);
        EXPECT_EQ(coordinator.CompleteDelivery(repeatedToken, conditions),
                  PlayerbotSocialDeliveryRejection::DuplicateWording);

        std::vector<PlayerbotSocialEventBinding> const suppressions =
            EventsOfType(coordinator, PLAYERBOT_SOCIAL_EVENT_TYPE_DELIVERY);
        ASSERT_EQ(suppressions.size(), 1u);
        EXPECT_EQ(suppressions.front().outcome, "suppressed");
        EXPECT_EQ(suppressions.front().reason, "duplicate_wording");
        EXPECT_TRUE(suppressions.front().messageText.empty());
    }

    {
        PlayerbotSocialMgr coordinator;
        RecordingProvider provider;
        coordinator.SetSocialProvider(&provider);
        GroundedThreadFixture const fixture = OpenGroundedThread(coordinator);

        uint64 const firstToken = OpenGroundedRequest(coordinator, fixture);
        PlayerbotSocialProviderResult first = Message("Still standing.", fixture.channel);
        first.requestToken = firstToken;
        ASSERT_EQ(coordinator.AcceptSocialResult(first, 100000, 3), PlayerbotSocialDeliveryRejection::None);
        ASSERT_EQ(coordinator.CompleteDelivery(firstToken, conditions), PlayerbotSocialDeliveryRejection::None);

        uint64 const repeatedToken = OpenGroundedRequest(coordinator, fixture);
        PlayerbotSocialProviderResult repeated = Message("Could have gone worse.", fixture.channel);
        repeated.requestToken = repeatedToken;
        ASSERT_EQ(coordinator.AcceptSocialResult(repeated, 200000, 3), PlayerbotSocialDeliveryRejection::None);
        EXPECT_EQ(coordinator.CompleteDelivery(repeatedToken, conditions),
                  PlayerbotSocialDeliveryRejection::DuplicateFunction);

        std::vector<PlayerbotSocialEventBinding> const suppressions =
            EventsOfType(coordinator, PLAYERBOT_SOCIAL_EVENT_TYPE_DELIVERY);
        ASSERT_EQ(suppressions.size(), 1u);
        EXPECT_EQ(suppressions.front().outcome, "suppressed");
        EXPECT_EQ(suppressions.front().reason, "duplicate_function");
        EXPECT_TRUE(suppressions.front().messageText.empty());
    }
}

TEST(PlayerbotSocialDeliveryTest, TwoSpeakersMayExchangeTheSameContributionFunction)
{
    /*
     * Two bots trading the same contribution function IS a conversation: banter answered with
     * banter. The duplicate-function rail exists to stop ONE bot repeating itself, so it compares
     * the speaker alongside the function; window 8 showed the speaker-blind version cutting a
     * third of the delivered replies out of exactly the exchanges the oracle asks for.
     */
    PlayerbotSocialDeliveryConditions conditions = AllHold();
    conditions.currentGrounding = Grounding();

    PlayerbotSocialMgr coordinator;
    RecordingProvider provider;
    coordinator.SetSocialProvider(&provider);
    GroundedThreadFixture const fixture = OpenGroundedThread(coordinator);

    uint64 const firstToken = OpenGroundedRequest(coordinator, fixture);
    PlayerbotSocialProviderResult first = Message("Still standing.", fixture.channel);
    first.requestToken = firstToken;
    ASSERT_EQ(coordinator.AcceptSocialResult(first, 100000, 3), PlayerbotSocialDeliveryRejection::None);
    ASSERT_EQ(coordinator.CompleteDelivery(firstToken, conditions), PlayerbotSocialDeliveryRejection::None);

    // A different bot answers in the same thread with the same function and different words.
    PlayerbotSocialDeliveryRejection rejection = PlayerbotSocialDeliveryRejection::None;
    uint64 const answerToken = coordinator.BeginSocialRequest(
        501, StoredPersonality(), 0, fixture.channel, fixture.thread.publicId,
        PlayerbotSocialRequestPriority::DirectHumanEngagement, 1000, REQUEST_ZONE_ID, std::string(), rejection, {},
        900, true, fixture.currentLine, false, PlayerbotRoleplayPromptMode::Ordinary, fixture.grounding, false);
    ASSERT_NE(answerToken, 0u);
    PlayerbotSocialProviderResult answer = Message("Could have gone worse.", fixture.channel);
    answer.requestToken = answerToken;
    ASSERT_EQ(coordinator.AcceptSocialResult(answer, 200000, 3), PlayerbotSocialDeliveryRejection::None);
    EXPECT_EQ(coordinator.CompleteDelivery(answerToken, conditions), PlayerbotSocialDeliveryRejection::None);

    // The same bot repeating its own function on the next generated line is still refused.
    uint64 const repeatToken = coordinator.BeginSocialRequest(
        501, StoredPersonality(), 0, fixture.channel, fixture.thread.publicId,
        PlayerbotSocialRequestPriority::DirectHumanEngagement, 1000, REQUEST_ZONE_ID, std::string(), rejection, {},
        900, true, fixture.currentLine, false, PlayerbotRoleplayPromptMode::Ordinary, fixture.grounding, false);
    ASSERT_NE(repeatToken, 0u);
    PlayerbotSocialProviderResult repeat = Message("Anyone need water?", fixture.channel);
    repeat.requestToken = repeatToken;
    ASSERT_EQ(coordinator.AcceptSocialResult(repeat, 300000, 3), PlayerbotSocialDeliveryRejection::None);
    EXPECT_EQ(coordinator.CompleteDelivery(repeatToken, conditions),
              PlayerbotSocialDeliveryRejection::DuplicateFunction);
}

TEST(PlayerbotSocialDeliveryTest, AReplyWhoseExactParentAgedOutIsSuppressedByName)
{
    PlayerbotSocialMgr coordinator;
    RecordingProvider provider;
    coordinator.SetSocialProvider(&provider);
    GroundedThreadFixture const fixture = OpenGroundedThread(coordinator);

    uint64 const token = OpenGroundedRequest(coordinator, fixture);
    ASSERT_NE(token, 0u);
    PlayerbotSocialProviderResult answer = Message("Still standing.", fixture.channel);
    answer.requestToken = token;
    ASSERT_EQ(coordinator.AcceptSocialResult(answer, 100000, 3), PlayerbotSocialDeliveryRejection::None);

    for (uint64 sequence = 0; sequence < PLAYERBOT_SOCIAL_MAX_THREAD_EVENTS; ++sequence)
    {
        PlayerbotSocialObservation observation;
        observation.key.channel = fixture.channel;
        observation.key.scopeId = 42;
        observation.eventPublicId = PlayerbotSocialMakeEventPublicId(1000 + sequence, 901);
        observation.speakerGuidCounter = 901;
        observation.speakerName = "LaterSpeaker";
        observation.zoneId = REQUEST_ZONE_ID;
        observation.atUnixSeconds = 1001 + sequence;
        observation.text = "A later line." + std::to_string(sequence);
        ASSERT_EQ(coordinator.Observe(observation).publicId, fixture.thread.publicId);
    }

    PlayerbotSocialDeliveryConditions conditions = AllHold();
    conditions.currentGrounding = Grounding();
    EXPECT_EQ(coordinator.CompleteDelivery(token, conditions), PlayerbotSocialDeliveryRejection::ReplyParentMismatch);

    std::vector<PlayerbotSocialEventBinding> const suppressions =
        EventsOfType(coordinator, PLAYERBOT_SOCIAL_EVENT_TYPE_DELIVERY);
    ASSERT_EQ(suppressions.size(), 1u);
    EXPECT_EQ(suppressions.front().outcome, "suppressed");
    EXPECT_EQ(suppressions.front().reason, "reply_parent_mismatch");
    EXPECT_TRUE(suppressions.front().messageText.empty());
}

TEST(PlayerbotSocialDeliveryTest, FunctionHistoryIsIsolatedByConversationScope)
{
    PlayerbotSocialMgr coordinator;
    RecordingProvider provider;
    coordinator.SetSocialProvider(&provider);
    PlayerbotSocialDeliveryConditions conditions = AllHold();
    conditions.currentGrounding = Grounding();

    for (auto const& [channel, scope] :
         {std::pair(PlayerbotSocialChannel::Say, uint64(10)), std::pair(PlayerbotSocialChannel::General, uint64(20)),
          std::pair(PlayerbotSocialChannel::Party, uint64(30)), std::pair(PlayerbotSocialChannel::Whisper, uint64(40))})
    {
        GroundedThreadFixture const fixture = OpenGroundedThread(coordinator, channel, scope);
        uint64 const token = OpenGroundedRequest(coordinator, fixture);
        ASSERT_NE(token, 0u);
        PlayerbotSocialProviderResult answer = Message("Could have gone worse.", channel);
        answer.requestToken = token;
        ASSERT_EQ(coordinator.AcceptSocialResult(answer, 100000 + scope, 3), PlayerbotSocialDeliveryRejection::None);
        EXPECT_EQ(coordinator.CompleteDelivery(token, conditions), PlayerbotSocialDeliveryRejection::None)
            << "channel " << static_cast<uint32>(channel);
    }
}

TEST(PlayerbotSocialDeliveryTest, WithNoProviderTheRequestIsRefusedAndNothingIsAllocated)
{
    // Definition of Done 5. Absence is a supported state: silence and a reason, never canned text,
    // and no state left behind for a request that had nowhere to go.
    PlayerbotSocialMgr coordinator;
    PlayerbotSocialDeliveryRejection rejection = PlayerbotSocialDeliveryRejection::None;

    EXPECT_EQ(OpenRequest(coordinator, 500, PlayerbotSocialChannel::Say, 1000, &rejection), 0u);
    EXPECT_EQ(rejection, PlayerbotSocialDeliveryRejection::NoProvider);
    EXPECT_EQ(coordinator.PendingDeliveryCount(), 0u);
    EXPECT_FALSE(coordinator.HasSocialProvider());
}

TEST(PlayerbotSocialDeliveryTest, AProviderThatRefusesOutrightLeavesNoStateBehind)
{
    PlayerbotSocialMgr coordinator;
    RecordingProvider provider;
    provider.accept = false;
    coordinator.SetSocialProvider(&provider);

    PlayerbotSocialDeliveryRejection rejection = PlayerbotSocialDeliveryRejection::None;

    EXPECT_EQ(OpenRequest(coordinator, 500, PlayerbotSocialChannel::Say, 1000, &rejection), 0u);
    EXPECT_EQ(rejection, PlayerbotSocialDeliveryRejection::ProviderFailed);
    EXPECT_EQ(coordinator.PendingDeliveryCount(), 0u);
}

TEST(PlayerbotSocialDeliveryTest, AResultIsScheduledRatherThanSpokenImmediately)
{
    // Key Decision 3. A bot that answers on the same tick the message landed reads as scripted.
    PlayerbotSocialMgr coordinator;
    RecordingProvider provider;
    coordinator.SetSocialProvider(&provider);

    uint64 const token = OpenRequest(coordinator, 500, PlayerbotSocialChannel::Say, 1000);
    ASSERT_NE(token, 0u);

    PlayerbotSocialProviderResult result = Message("aye", PlayerbotSocialChannel::Say);
    result.requestToken = token;

    ASSERT_EQ(coordinator.AcceptSocialResult(result, 100000, 3), PlayerbotSocialDeliveryRejection::None);

    // Nothing is due before the delay elapses, and it is due once it has.
    EXPECT_TRUE(coordinator.DueDeliveries(100000).empty());
    EXPECT_TRUE(coordinator.DueDeliveries(100000 + PLAYERBOT_SOCIAL_DELIVERY_DELAY_MIN_MS - 1).empty());
    EXPECT_EQ(coordinator.DueDeliveries(100000 + PLAYERBOT_SOCIAL_DELIVERY_DELAY_MAX_MS).size(), 1u);
}

TEST(PlayerbotSocialDeliveryTest, AResultForAnUnknownOrAlreadyAnsweredRequestIsRefused)
{
    PlayerbotSocialMgr coordinator;
    RecordingProvider provider;
    coordinator.SetSocialProvider(&provider);

    uint64 const token = OpenRequest(coordinator, 500, PlayerbotSocialChannel::Say, 1000);
    ASSERT_NE(token, 0u);

    PlayerbotSocialProviderResult stranger = Message("aye", PlayerbotSocialChannel::Say);
    stranger.requestToken = token + 5000;
    EXPECT_EQ(coordinator.AcceptSocialResult(stranger, 100000, 3), PlayerbotSocialDeliveryRejection::UnknownRequest);

    // Zero is not a token at all.
    PlayerbotSocialProviderResult untokened = Message("aye", PlayerbotSocialChannel::Say);
    untokened.requestToken = 0;
    EXPECT_EQ(coordinator.AcceptSocialResult(untokened, 100000, 3), PlayerbotSocialDeliveryRejection::UnknownRequest);

    PlayerbotSocialProviderResult answer = Message("aye", PlayerbotSocialChannel::Say);
    answer.requestToken = token;
    ASSERT_EQ(coordinator.AcceptSocialResult(answer, 100000, 3), PlayerbotSocialDeliveryRejection::None);

    // A second result answers a request that is already answered.
    EXPECT_EQ(coordinator.AcceptSocialResult(answer, 100000, 3), PlayerbotSocialDeliveryRejection::UnknownRequest);
}

TEST(PlayerbotSocialDeliveryTest, ATokenIsNeverReusedSoAStaleResultCannotHitALiveRequest)
{
    PlayerbotSocialMgr coordinator;
    RecordingProvider provider;
    coordinator.SetSocialProvider(&provider);

    uint64 const first = OpenRequest(coordinator, 500, PlayerbotSocialChannel::Say, 1000);
    ASSERT_EQ(coordinator.CancelPendingDeliveries().size(), 1u);

    uint64 const second = OpenRequest(coordinator, 500, PlayerbotSocialChannel::Say, 1000);

    EXPECT_NE(first, second);
    EXPECT_NE(first, 0u);
    EXPECT_NE(second, 0u);
}

TEST(PlayerbotSocialDeliveryTest, AMalformedResultDropsTheRequestWithItsOwnReason)
{
    PlayerbotSocialMgr coordinator;
    RecordingProvider provider;
    coordinator.SetSocialProvider(&provider);

    uint64 const token = OpenRequest(coordinator, 500, PlayerbotSocialChannel::Party, 1000);
    ASSERT_NE(token, 0u);

    PlayerbotSocialProviderResult scripted = Message("one\ntwo", PlayerbotSocialChannel::Party);
    scripted.requestToken = token;

    EXPECT_EQ(coordinator.AcceptSocialResult(scripted, 100000, 3), PlayerbotSocialDeliveryRejection::BurstDelimiter);
    EXPECT_EQ(coordinator.PendingDeliveryCount(), 0u);
}

TEST(PlayerbotSocialDeliveryTest, SilenceClosesTheRequestWithNothingToDeliver)
{
    PlayerbotSocialMgr coordinator;
    RecordingProvider provider;
    coordinator.SetSocialProvider(&provider);

    uint64 const token = OpenRequest(coordinator, 500, PlayerbotSocialChannel::Say, 1000);

    PlayerbotSocialProviderResult quiet;
    quiet.requestToken = token;
    quiet.kind = PlayerbotSocialOutputKind::Silence;

    EXPECT_EQ(coordinator.AcceptSocialResult(quiet, 100000, 3), PlayerbotSocialDeliveryRejection::None);
    EXPECT_EQ(coordinator.PendingDeliveryCount(), 0u);
    EXPECT_TRUE(coordinator.DueDeliveries(UINT64_MAX).empty());

    std::vector<PlayerbotSocialEventBinding> attempts;
    for (PlayerbotSocialEventBinding const& event : coordinator.PendingEvents())
        if (event.eventType == PLAYERBOT_SOCIAL_EVENT_TYPE_PROVIDER_ATTEMPT)
            attempts.push_back(event);

    ASSERT_EQ(attempts.size(), 1u);
    EXPECT_EQ(attempts.front().outcome, "suppressed");
    EXPECT_EQ(attempts.front().reason, "provider_silence");
    EXPECT_TRUE(attempts.front().messageText.empty());
    EXPECT_NE(attempts.front().diagnosticsJson.find("\"evidence\":{"), std::string::npos);
    EXPECT_NE(attempts.front().diagnosticsJson.find("\"stage\":\"human_replies\""), std::string::npos);
    EXPECT_NE(attempts.front().diagnosticsJson.find("\"profile\":{\"state\":\"pending\""), std::string::npos);
    EXPECT_NE(attempts.front().diagnosticsJson.find("\"memory\":{\"state\":\"loaded\"}"), std::string::npos);
}

TEST(PlayerbotSocialDeliveryTest, ADroppedDeliveryIsConsumedRatherThanRetried)
{
    // Definition of Done 1. Leaving a refused result in place would let it be retried into a
    // conversation that has already moved on, which is the stale delivery this path prevents.
    PlayerbotSocialMgr coordinator;
    RecordingProvider provider;
    coordinator.SetSocialProvider(&provider);

    uint64 const token = OpenRequest(coordinator, 500, PlayerbotSocialChannel::Say, 1000);
    PlayerbotSocialProviderResult answer = Message("aye", PlayerbotSocialChannel::Say);
    answer.requestToken = token;
    ASSERT_EQ(coordinator.AcceptSocialResult(answer, 100000, 3), PlayerbotSocialDeliveryRejection::None);

    PlayerbotSocialDeliveryConditions moved = AllHold();
    moved.threadStillCurrent = false;

    EXPECT_EQ(coordinator.CompleteDelivery(token, moved), PlayerbotSocialDeliveryRejection::SupersededThread);
    EXPECT_EQ(coordinator.PendingDeliveryCount(), 0u);
    EXPECT_EQ(coordinator.CompleteDelivery(token, AllHold()), PlayerbotSocialDeliveryRejection::UnknownRequest);
}

TEST(PlayerbotSocialDeliveryTest, ADeliveredResultIsAlsoConsumed)
{
    PlayerbotSocialMgr coordinator;
    RecordingProvider provider;
    coordinator.SetSocialProvider(&provider);

    uint64 const token = OpenRequest(coordinator, 500, PlayerbotSocialChannel::Say, 1000);
    PlayerbotSocialProviderResult answer = Message("aye", PlayerbotSocialChannel::Say);
    answer.requestToken = token;
    ASSERT_EQ(coordinator.AcceptSocialResult(answer, 100000, 3), PlayerbotSocialDeliveryRejection::None);

    EXPECT_EQ(coordinator.CompleteDelivery(token, AllHold()), PlayerbotSocialDeliveryRejection::None);
    EXPECT_EQ(coordinator.PendingDeliveryCount(), 0u);
}

TEST(PlayerbotSocialDeliveryTest, AProviderThatNeverAnswersIsAbandoned)
{
    PlayerbotSocialMgr coordinator;
    RecordingProvider provider;
    coordinator.SetSocialProvider(&provider);

    ASSERT_NE(OpenRequest(coordinator, 500, PlayerbotSocialChannel::Say, 1000), 0u);

    EXPECT_EQ(coordinator.ExpireTimedOutRequests(1000 + PLAYERBOT_SOCIAL_PROVIDER_TIMEOUT_SECONDS - 1).size(), 0u);
    EXPECT_EQ(coordinator.ExpireTimedOutRequests(1000 + PLAYERBOT_SOCIAL_PROVIDER_TIMEOUT_SECONDS).size(), 1u);
    EXPECT_EQ(coordinator.PendingDeliveryCount(), 0u);
}

TEST(PlayerbotSocialDeliveryTest, ARequestWaitingOnItsDelayIsNotATimeout)
{
    // A request that already has a result is waiting on a conversational pause, not on a provider.
    PlayerbotSocialMgr coordinator;
    RecordingProvider provider;
    coordinator.SetSocialProvider(&provider);

    uint64 const token = OpenRequest(coordinator, 500, PlayerbotSocialChannel::Say, 1000);
    PlayerbotSocialProviderResult answer = Message("aye", PlayerbotSocialChannel::Say);
    answer.requestToken = token;
    ASSERT_EQ(coordinator.AcceptSocialResult(answer, 100000, 3), PlayerbotSocialDeliveryRejection::None);

    EXPECT_EQ(coordinator.ExpireTimedOutRequests(1000 + PLAYERBOT_SOCIAL_PROVIDER_TIMEOUT_SECONDS * 10).size(), 0u);
    EXPECT_EQ(coordinator.PendingDeliveryCount(), 1u);
}

TEST(PlayerbotSocialDeliveryTest, ARewoundClockDoesNotExpireEveryOutstandingRequest)
{
    PlayerbotSocialMgr coordinator;
    RecordingProvider provider;
    coordinator.SetSocialProvider(&provider);

    ASSERT_NE(OpenRequest(coordinator, 500, PlayerbotSocialChannel::Say, 5000), 0u);

    EXPECT_EQ(coordinator.ExpireTimedOutRequests(1000).size(), 0u);
    EXPECT_EQ(coordinator.PendingDeliveryCount(), 1u);
}

TEST(PlayerbotSocialDeliveryTest, ShutdownDropsEverythingWithoutTouchingAGameObject)
{
    // Definition of Done 4. At shutdown the characters these name are being removed, so the only
    // safe thing to do with a pending delivery is forget it. Nothing here holds a pointer to hold.
    PlayerbotSocialMgr coordinator;
    RecordingProvider provider;
    coordinator.SetSocialProvider(&provider);

    ASSERT_NE(OpenRequest(coordinator, 500, PlayerbotSocialChannel::Say, 1000), 0u);
    ASSERT_NE(OpenRequest(coordinator, 501, PlayerbotSocialChannel::Party, 1000), 0u);

    EXPECT_EQ(coordinator.CancelPendingDeliveries().size(), 2u);
    EXPECT_EQ(coordinator.PendingDeliveryCount(), 0u);
    EXPECT_EQ(coordinator.CancelPendingDeliveries().size(), 0u);
}

TEST(PlayerbotSocialDeliveryTest, OneBotCannotFillTheQueueForEveryoneElse)
{
    // A single shared ceiling is a starvation surface: one bot in a very busy conversation would
    // otherwise silence every other bot on the realm.
    PlayerbotSocialMgr coordinator;
    RecordingProvider provider;
    coordinator.SetSocialProvider(&provider);

    PlayerbotSocialDeliveryRejection rejection = PlayerbotSocialDeliveryRejection::None;

    ASSERT_NE(OpenStarterRequest(coordinator, 500, 900, PlayerbotSocialChannel::Say, ThreadId(),
                                 PlayerbotSocialRequestPriority::DirectHumanEngagement, 1000, rejection),
              0u);
    ASSERT_NE(OpenStarterRequest(coordinator, 500, 900, PlayerbotSocialChannel::Say, OtherThreadId(),
                                 PlayerbotSocialRequestPriority::DirectHumanEngagement, 1000, rejection),
              0u);

    EXPECT_EQ(OpenStarterRequest(coordinator, 500, 900, PlayerbotSocialChannel::Say, ThirdThreadId(),
                                 PlayerbotSocialRequestPriority::DirectHumanEngagement, 1000, rejection),
              0u);
    EXPECT_EQ(rejection, PlayerbotSocialDeliveryRejection::QueueFull);

    // A different bot is unaffected, which is the whole point of the per bot bound.
    EXPECT_NE(OpenStarterRequest(coordinator, 501, 900, PlayerbotSocialChannel::Say, ThreadId(),
                                 PlayerbotSocialRequestPriority::DirectHumanEngagement, 1000, rejection),
              0u);
}

TEST(PlayerbotSocialDeliveryTest, OneBotCannotOweTwoRepliesToTheSameThread)
{
    PlayerbotSocialMgr coordinator;
    RecordingProvider provider;
    coordinator.SetSocialProvider(&provider);

    PlayerbotSocialDeliveryRejection rejection = PlayerbotSocialDeliveryRejection::None;

    ASSERT_NE(OpenStarterRequest(coordinator, 500, 0, PlayerbotSocialChannel::General, ThreadId(),
                                 PlayerbotSocialRequestPriority::DirectHumanEngagement, 1000, rejection),
              0u);

    EXPECT_EQ(OpenStarterRequest(coordinator, 500, 0, PlayerbotSocialChannel::General, ThreadId(),
                                 PlayerbotSocialRequestPriority::MixedThread, 1001, rejection),
              0u);
    EXPECT_EQ(rejection, PlayerbotSocialDeliveryRejection::QueueFull);

    PlayerbotSocialMgr separateThreads;
    separateThreads.SetSocialProvider(&provider);
    ASSERT_NE(OpenStarterRequest(separateThreads, 500, 0, PlayerbotSocialChannel::General, ThreadId(),
                                 PlayerbotSocialRequestPriority::DirectHumanEngagement, 1000, rejection),
              0u);
    EXPECT_NE(OpenStarterRequest(separateThreads, 500, 0, PlayerbotSocialChannel::General, OtherThreadId(),
                                 PlayerbotSocialRequestPriority::MixedThread, 1001, rejection),
              0u);
}

TEST(PlayerbotSocialDeliveryTest, ClearingTheProviderDoesNotSwallowRepliesAboutToLand)
{
    /*
     * A configuration reload replaces the provider. Cancelling outstanding requests here would
     * silently drop conversations that were about to land, and it is not needed: they are keyed by
     * token and time out on their own.
     */
    PlayerbotSocialMgr coordinator;
    RecordingProvider provider;
    coordinator.SetSocialProvider(&provider);

    uint64 const token = OpenRequest(coordinator, 500, PlayerbotSocialChannel::Say, 1000);
    ASSERT_NE(token, 0u);

    coordinator.SetSocialProvider(nullptr);

    EXPECT_FALSE(coordinator.HasSocialProvider());
    EXPECT_EQ(coordinator.PendingDeliveryCount(), 1u);

    PlayerbotSocialProviderResult answer = Message("aye", PlayerbotSocialChannel::Say);
    answer.requestToken = token;
    EXPECT_EQ(coordinator.AcceptSocialResult(answer, 100000, 3), PlayerbotSocialDeliveryRejection::None);
}

TEST(PlayerbotSocialDeliveryTest, FactionAndLanguageAreAuthoritativeOnEveryChannel)
{
    /*
     * The channel contract names faction and language alongside phase, visibility, membership, map,
     * and proximity. A party is necessarily one faction so the test is vacuous there, but an absent
     * check is how cross faction chat reaches a player as readable text.
     */
    for (PlayerbotSocialChannel const channel : {PlayerbotSocialChannel::General, PlayerbotSocialChannel::Say,
                                                 PlayerbotSocialChannel::Party, PlayerbotSocialChannel::Whisper})
    {
        PlayerbotSocialDeliveryConditions hostile = AllHold();
        hostile.factionAllows = false;
        EXPECT_EQ(PlayerbotSocialRevalidateDelivery(channel, PlayerbotSocialOutputKind::Message, hostile),
                  PlayerbotSocialDeliveryRejection::FactionForbids);

        PlayerbotSocialDeliveryConditions foreign = AllHold();
        foreign.languageUnderstood = false;
        EXPECT_EQ(PlayerbotSocialRevalidateDelivery(channel, PlayerbotSocialOutputKind::Message, foreign),
                  PlayerbotSocialDeliveryRejection::LanguageNotUnderstood);
    }
}

TEST(PlayerbotSocialDeliveryTest, AStarterCannotTakeTheSlotAPlayerIsWaitingOn)
{
    /*
     * Refusing when a bot is full stops one bot starving another, but a pair of starters would
     * otherwise occupy everything a bot has and block the direct engagement that arrives a moment
     * later. The last slot is reserved for the two lanes a player is actually waiting on.
     */
    PlayerbotSocialMgr coordinator;
    RecordingProvider provider;
    coordinator.SetSocialProvider(&provider);

    PlayerbotSocialDeliveryRejection rejection = PlayerbotSocialDeliveryRejection::None;

    ASSERT_NE(OpenStarterRequest(coordinator, 500, 900, PlayerbotSocialChannel::General, ThreadId(),
                                 PlayerbotSocialRequestPriority::Starter, 1000, rejection),
              0u);

    // One slot left, and a starter may not have it.
    EXPECT_EQ(OpenStarterRequest(coordinator, 500, 900, PlayerbotSocialChannel::General, OtherThreadId(),
                                 PlayerbotSocialRequestPriority::Starter, 1000, rejection),
              0u);
    EXPECT_EQ(rejection, PlayerbotSocialDeliveryRejection::QueueReservedForPlayers);

    EXPECT_EQ(OpenStarterRequest(coordinator, 500, 900, PlayerbotSocialChannel::General, OtherThreadId(),
                                 PlayerbotSocialRequestPriority::BotContinuation, 1000, rejection),
              0u);
    EXPECT_EQ(rejection, PlayerbotSocialDeliveryRejection::QueueReservedForPlayers);

    // Direct human engagement may.
    EXPECT_NE(OpenStarterRequest(coordinator, 500, 900, PlayerbotSocialChannel::General, OtherThreadId(),
                                 PlayerbotSocialRequestPriority::DirectHumanEngagement, 1000, rejection),
              0u);
}

TEST(PlayerbotSocialDeliveryTest, AnUnrecognizedPriorityLosesTheReservedSlotRatherThanClaimingIt)
{
    EXPECT_TRUE(PlayerbotSocialPriorityMayTakeLastSlot(PlayerbotSocialRequestPriority::DirectHumanEngagement));
    EXPECT_TRUE(PlayerbotSocialPriorityMayTakeLastSlot(PlayerbotSocialRequestPriority::MixedThread));
    EXPECT_FALSE(PlayerbotSocialPriorityMayTakeLastSlot(PlayerbotSocialRequestPriority::BotContinuation));
    EXPECT_FALSE(PlayerbotSocialPriorityMayTakeLastSlot(PlayerbotSocialRequestPriority::Starter));
    EXPECT_FALSE(PlayerbotSocialPriorityMayTakeLastSlot(static_cast<PlayerbotSocialRequestPriority>(99)));
}

TEST(PlayerbotSocialDeliveryTest, AThreadIdentityIsValidatedBeforeItIsStored)
{
    // An arbitrary length string copied into a bounded number of slots is not a bounded amount of
    // memory, and a typed identity of the wrong kind does not name a conversation at all.
    PlayerbotSocialMgr coordinator;
    RecordingProvider provider;
    coordinator.SetSocialProvider(&provider);

    PlayerbotSocialDeliveryRejection rejection = PlayerbotSocialDeliveryRejection::None;

    for (std::string const& bad :
         {std::string(""), std::string(4096, 'x'), std::string("act_00000000000000000000000000000001")})
    {
        EXPECT_EQ(coordinator.BeginSocialRequest(500, StoredPersonality(), 900, PlayerbotSocialChannel::Say, bad,
                                                 PlayerbotSocialRequestPriority::DirectHumanEngagement, 1000,
                                                 REQUEST_ZONE_ID, std::string(), rejection),
                  0u);
        EXPECT_EQ(rejection, PlayerbotSocialDeliveryRejection::MalformedThreadIdentity);
    }

    EXPECT_EQ(coordinator.PendingDeliveryCount(), 0u);
}

TEST(PlayerbotSocialDeliveryTest, AbandonedRequestsCarryTheirOwnReason)
{
    // A bare count cannot say which conversation went unanswered or under which rule.
    PlayerbotSocialMgr coordinator;
    RecordingProvider provider;
    coordinator.SetSocialProvider(&provider);

    uint64 const timedOut = OpenRequest(coordinator, 500, PlayerbotSocialChannel::Say, 1000);
    std::vector<PlayerbotSocialAbandonedRequest> const expired =
        coordinator.ExpireTimedOutRequests(1000 + PLAYERBOT_SOCIAL_PROVIDER_TIMEOUT_SECONDS);

    ASSERT_EQ(expired.size(), 1u);
    EXPECT_EQ(expired.front().requestToken, timedOut);
    EXPECT_EQ(expired.front().botGuidCounter, 500u);
    EXPECT_EQ(expired.front().rejection, PlayerbotSocialDeliveryRejection::ProviderTimedOut);

    uint64 const cancelled = OpenRequest(coordinator, 501, PlayerbotSocialChannel::Party, 1000);
    std::vector<PlayerbotSocialAbandonedRequest> const dropped = coordinator.CancelPendingDeliveries();

    ASSERT_EQ(dropped.size(), 1u);
    EXPECT_EQ(dropped.front().requestToken, cancelled);
    EXPECT_EQ(dropped.front().botGuidCounter, 501u);
    EXPECT_EQ(dropped.front().rejection, PlayerbotSocialDeliveryRejection::ShuttingDown);
}

TEST(PlayerbotSocialDeliveryTest, AnEmoteCannotSmuggleAnUnboundedLineThroughTheTextField)
{
    /*
     * The length bound used to run only on the message path, so an emote result could carry an
     * arbitrarily large text blob that was then copied into the pending request and held for the
     * life of it. An emote carries no line, but nothing stops a provider attaching one.
     */
    PlayerbotSocialProviderResult oversized = EmoteResult(4, PlayerbotSocialChannel::Say);
    oversized.text = std::string(PLAYERBOT_SOCIAL_MAX_OUTPUT_LENGTH + 1, 'a');

    EXPECT_EQ(PlayerbotSocialValidateOutput(oversized, PlayerbotSocialChannel::Say),
              PlayerbotSocialDeliveryRejection::TooLong);
}

TEST(PlayerbotSocialDeliveryTest, TextAttachedToAnEmoteIsNotStored)
{
    // Accepted within the bound, but there is no reason to hold a line an emote will never speak.
    PlayerbotSocialMgr coordinator;
    RecordingProvider provider;
    coordinator.SetSocialProvider(&provider);

    uint64 const token = OpenRequest(coordinator, 500, PlayerbotSocialChannel::Say, 1000);
    ASSERT_NE(token, 0u);

    PlayerbotSocialProviderResult gesture = EmoteResult(4, PlayerbotSocialChannel::Say);
    gesture.requestToken = token;
    gesture.text = "waves at you";

    ASSERT_EQ(coordinator.AcceptSocialResult(gesture, 100000, 3), PlayerbotSocialDeliveryRejection::None);

    PlayerbotSocialPendingDelivery stored;
    ASSERT_TRUE(coordinator.PendingDeliveryFor(token, stored));
    EXPECT_TRUE(stored.result.text.empty());
    EXPECT_EQ(stored.result.emoteId, 4u);
}

TEST(PlayerbotSocialDeliveryTest, TheProviderIsToldWhoTheLineIsForWithoutReadingTheCoordinator)
{
    /*
     * The coordinator submits BEFORE it records the request, deliberately, so a provider that
     * refuses outright leaves no state behind. A provider that tried to learn its target by calling
     * PendingDeliveryFor from inside Submit would therefore find nothing and refuse every single
     * request, which is exactly the shape of failure this pins: the seam has to be self sufficient.
     */
    PlayerbotSocialMgr coordinator;
    RecordingProvider provider;
    coordinator.SetSocialProvider(&provider);

    uint64 const token = OpenRequest(coordinator, 500, PlayerbotSocialChannel::Whisper, 1000);

    ASSERT_NE(token, 0u);
    ASSERT_EQ(provider.submittedTargets.size(), 1u);
    EXPECT_EQ(provider.submittedTargets[0], 900u);

    coordinator.SetSocialProvider(nullptr);
}

TEST(PlayerbotSocialDeliveryTest, ARoomAddressedRequestCarriesNoTarget)
{
    // Zero is a legitimate answer rather than a missing one: a General line is addressed to a room.
    PlayerbotSocialMgr coordinator;
    RecordingProvider provider;
    coordinator.SetSocialProvider(&provider);

    PlayerbotSocialDeliveryRejection rejection = PlayerbotSocialDeliveryRejection::None;
    ASSERT_NE(OpenStarterRequest(coordinator, 500, 0, PlayerbotSocialChannel::General, ThreadId(),
                                 PlayerbotSocialRequestPriority::Starter, 1000, rejection),
              0u);

    ASSERT_EQ(provider.submittedTargets.size(), 1u);
    EXPECT_EQ(provider.submittedTargets[0], 0u);

    coordinator.SetSocialProvider(nullptr);
}

TEST(PlayerbotSocialDeliveryTest, APublicReplyKeepsTheSpeakerAsContextWithoutMakingItADeliveryTarget)
{
    PlayerbotSocialMgr coordinator;
    RecordingProvider provider;
    coordinator.SetSocialProvider(&provider);

    GroundedThreadFixture const fixture = OpenGroundedThread(coordinator, PlayerbotSocialChannel::General);
    PlayerbotSocialDeliveryRejection rejection = PlayerbotSocialDeliveryRejection::None;
    uint64 const token = coordinator.BeginSocialRequest(
        500, StoredPersonality(), 0, PlayerbotSocialChannel::General, fixture.thread.publicId,
        PlayerbotSocialRequestPriority::DirectHumanEngagement, 1000, REQUEST_ZONE_ID, std::string(), rejection, {},
        900, true, fixture.currentLine, false, PlayerbotRoleplayPromptMode::Ordinary, fixture.grounding);
    ASSERT_NE(token, 0u);

    /*
     * The two halves of "who this line is for" travel separately. The grounded speaker rides as the
     * wire subject, because the provider refuses Participant evidence whose subject did not travel.
     * The DELIVERY target stays zero: a General line is revalidated against its room's scope, and a
     * stored target would make it read as a private reply.
     */
    ASSERT_EQ(provider.submittedTargets.size(), 1u);
    EXPECT_EQ(provider.submittedTargets[0], 900u) << "the grounded speaker must travel as the wire subject";

    PlayerbotSocialPendingDelivery stored;
    ASSERT_TRUE(coordinator.PendingDeliveryFor(token, stored));
    EXPECT_EQ(stored.targetGuidCounter, 0u) << "General still delivers to the room";

    ASSERT_EQ(provider.submittedContexts.size(), 1u);
    EXPECT_FALSE(provider.submittedContexts[0].relationship.empty())
        << "the context still carries the responder's directional relationship with the speaker";
}

TEST(PlayerbotSocialDeliveryTest, ASayReplyRevalidatesAgainstTheSpeakerItAnswers)
{
    /*
     * A say thread's scope id is a cohort-registry counter, not a zone, so the targetless
     * revalidation branch (zone == scopeId) can never hold and every targetless say reply died as
     * different_map on live. The speaker the reply answers IS the conversation's location on /say,
     * so it travels as the delivery target and the spatial revalidation runs against them, exactly
     * as a say starter revalidates against its perceivable audience.
     */
    PlayerbotSocialMgr coordinator;
    RecordingProvider provider;
    coordinator.SetSocialProvider(&provider);

    GroundedThreadFixture const fixture = OpenGroundedThread(coordinator, PlayerbotSocialChannel::Say);
    PlayerbotSocialDeliveryRejection rejection = PlayerbotSocialDeliveryRejection::None;
    uint64 const token = coordinator.BeginSocialRequest(
        500, StoredPersonality(), 0, PlayerbotSocialChannel::Say, fixture.thread.publicId,
        PlayerbotSocialRequestPriority::DirectHumanEngagement, 1000, REQUEST_ZONE_ID, std::string(), rejection, {},
        900, true, fixture.currentLine, false, PlayerbotRoleplayPromptMode::Ordinary, fixture.grounding);
    ASSERT_NE(token, 0u);

    ASSERT_EQ(provider.submittedTargets.size(), 1u);
    EXPECT_EQ(provider.submittedTargets[0], 900u);

    PlayerbotSocialPendingDelivery stored;
    ASSERT_TRUE(coordinator.PendingDeliveryFor(token, stored));
    EXPECT_EQ(stored.targetGuidCounter, 900u) << "the speaker is the say reply's revalidation anchor";
}

// Activation: an observed message becomes a request ------------------------------------------------

namespace
{
// A candidate that passes every eligibility rule. Each test turns off exactly the one thing it
// is about, so a rule that depended on something unrelated shows up as the wrong outcome rather
// than as a pass.
PlayerbotSocialActivationCandidate WillingCandidate(uint64 botGuidCounter)
{
    PlayerbotSocialActivationCandidate candidate;
    candidate.botGuidCounter = botGuidCounter;
    candidate.personality = StoredPersonality();
    candidate.profileLoadState = PlayerbotSocialProfileLoadState::Loaded;
    candidate.grounding = Grounding();
    candidate.effectiveDisposition = 90;
    candidate.stance = PlayerbotSocialStance::Engaged;
    candidate.addressedByName = true;
    candidate.participatedInThread = true;
    candidate.contentRelevance = 90;
    candidate.optedOutOfInitiation = false;
    candidate.factionMatches = true;
    candidate.languageMatches = true;
    candidate.lastSpokeUnixSeconds = 0;
    return candidate;
}

// A live human thread with one candidate who wants to answer.
void ObserveActivationThread(PlayerbotSocialMgr& coordinator, PlayerbotSocialActivation& activation)
{
    if (activation.currentLine.speakerIsHuman)
        coordinator.ApplyConsentSnapshot(activation.currentLine.speakerGuidCounter, false);

    PlayerbotSocialObservation observation;
    observation.key.channel = activation.channel;
    observation.key.scopeId = 42;
    observation.eventPublicId = activation.currentLine.eventPublicId;
    observation.role = activation.currentLine.role;
    observation.replyToEventPublicId = activation.currentLine.replyToEventPublicId;
    observation.sourceEventPublicId = activation.currentLine.sourceEventPublicId;
    observation.speakerGuidCounter = activation.currentLine.speakerGuidCounter;
    observation.speakerName = activation.currentLine.speakerName;
    observation.speakerIsHuman = activation.currentLine.speakerIsHuman;
    observation.zoneId = activation.zoneId;
    observation.atUnixSeconds = activation.currentLine.atUnixSeconds;
    observation.text = activation.currentLine.text;
    activation.thread = coordinator.Observe(observation);
}

PlayerbotSocialActivation LiveOpportunity(PlayerbotSocialMgr& coordinator, uint64 nowUnixSeconds)
{
    PlayerbotSocialActivation activation;
    activation.channel = PlayerbotSocialChannel::Say;
    activation.speakerGuidCounter = 900;
    activation.speakerIsHuman = true;
    activation.speakerOptedOut = false;
    activation.starter = false;
    activation.duplicateOfRecentMessage = false;
    activation.channelDensity = 0;
    activation.threadLastActivityUnixSeconds = nowUnixSeconds;
    activation.relevantHumanMessages = 3;
    activation.consecutiveBotOnlyTurns = 0;
    activation.nowUnixSeconds = nowUnixSeconds;
    activation.selectionSeed = 0;
    activation.zoneId = REQUEST_ZONE_ID;
    activation.currentLine.eventPublicId = PlayerbotSocialMakeEventPublicId(nowUnixSeconds, 900);
    activation.currentLine.speakerGuidCounter = 900;
    activation.currentLine.speakerName = "Elyse";
    activation.currentLine.speakerIsHuman = true;
    activation.currentLine.atUnixSeconds = nowUnixSeconds;
    activation.currentLine.text = "A current fixture line.";
    activation.candidates.push_back(WillingCandidate(500));
    activation.candidates.front().grounding = Grounding();
    activation.candidates.front().grounding.transcriptEventPublicIds = {activation.currentLine.eventPublicId};
    ObserveActivationThread(coordinator, activation);
    return activation;
}

/*
 * The lowest seed whose reply roll clears the pressure this opportunity produces.
 *
 * The seed is searched rather than hardcoded because the roll lives inside responder selection
 * and is derived from the seed by a hash. Hardcoding a magic number here would make the test
 * silently stop testing activation the moment that derivation was retuned: it would start failing
 * on the roll instead of on the thing under test, and read as a regression in the wrong place.
 */
uint64 SeedThatAnswers(PlayerbotSocialActivation activation)
{
    for (uint64 seed = 1; seed < 512; ++seed)
    {
        PlayerbotSocialMgr probe;
        RecordingProvider accepting;
        probe.SetSocialProvider(&accepting);

        PlayerbotSocialActivation probeActivation = activation;
        ObserveActivationThread(probe, probeActivation);
        probeActivation.selectionSeed = seed;
        if (!probe.Activate(probeActivation, PlayerbotSocialDensityProfile::Normal).openedTokens.empty())
        {
            probe.SetSocialProvider(nullptr);
            return seed;
        }

        probe.SetSocialProvider(nullptr);
    }

    return 0;
}
}  // namespace

TEST(PlayerbotSocialDeliveryTest, AnEligibleObservationOpensARequestThroughTheProvider)
{
    /*
     * Definition of Done 1. This is the seam the whole feature was missing: every piece below it was
     * built and tested, and nothing called any of it, so conversation pressure accumulated and no
     * request was ever opened.
     */
    PlayerbotSocialMgr coordinator;
    RecordingProvider provider;
    coordinator.SetSocialProvider(&provider);

    PlayerbotSocialActivation activation = LiveOpportunity(coordinator, 1000);
    activation.selectionSeed = SeedThatAnswers(activation);
    ASSERT_NE(activation.selectionSeed, 0u) << "no seed in the search window produced an answer";

    PlayerbotSocialActivationResult const result =
        coordinator.Activate(activation, PlayerbotSocialDensityProfile::Normal);

    EXPECT_EQ(result.rejection, PlayerbotSocialOpportunityRejection::None);
    EXPECT_FALSE(result.pressureDeclined);
    EXPECT_TRUE(result.refusedCandidates.empty());
    ASSERT_EQ(result.openedTokens.size(), 1u);
    EXPECT_NE(result.openedTokens[0], 0u);

    // The request reached the provider, rather than merely being recorded as opened.
    ASSERT_EQ(provider.submitted.size(), 1u);
    EXPECT_EQ(provider.submitted[0], result.openedTokens[0]);

    coordinator.SetSocialProvider(nullptr);
}

TEST(PlayerbotSocialDeliveryTest, ActivationAddsIdentityOnlyToTheSelectedMachineResponderRequest)
{
    PlayerbotSocialMgr coordinator;
    RecordingProvider provider;
    coordinator.SetSocialProvider(&provider);

    PlayerbotSocialActivation activation = LiveOpportunity(coordinator, 1000);
    activation.candidates.front() = WillingCandidate(42);
    activation.currentLine.speakerGuidCounter = 900;
    activation.currentLine.speakerName = "Human";
    activation.currentLine.speakerIsHuman = true;
    activation.currentLine.atUnixSeconds = 1000;
    activation.currentLine.text = "Botname, how old are you and where are you from?";
    ObserveActivationThread(coordinator, activation);
    activation.selectionSeed = SeedThatAnswers(activation);
    ASSERT_NE(activation.selectionSeed, 0u);

    PlayerbotSocialActivationResult const result =
        coordinator.Activate(activation, PlayerbotSocialDensityProfile::Normal);

    ASSERT_EQ(result.openedTokens.size(), 1u);
    ASSERT_EQ(provider.submittedContexts.size(), 1u)
        << "the human speaker and public room must not receive their own identity requests";
    PlayerbotFictionalIdentityPromptContext const& identity = provider.submittedContexts.front().fictionalIdentity;
    EXPECT_EQ(identity.request, PlayerbotFictionalIdentityRequest::AgeAndHomeCountry);
    EXPECT_EQ(identity.age, 36u);
    EXPECT_FALSE(identity.homeCountry.has_value());
}

TEST(PlayerbotSocialDeliveryTest, ActivationUsesTheSelectedRespondersOwnNearbySnapshot)
{
    PlayerbotSocialMgr coordinator;
    RecordingProvider provider;
    coordinator.SetSocialProvider(&provider);

    PlayerbotSocialActivation activation = LiveOpportunity(coordinator, 1000);
    activation.candidates.front().nearby = {{0, "Deszy", false}, {0, "Barnek", false}};
    activation.selectionSeed = SeedThatAnswers(activation);
    ASSERT_NE(activation.selectionSeed, 0u);

    PlayerbotSocialActivationResult const result =
        coordinator.Activate(activation, PlayerbotSocialDensityProfile::Normal);

    ASSERT_EQ(result.openedTokens.size(), 1u);
    ASSERT_EQ(provider.submittedContexts.size(), 1u);
    EXPECT_EQ(provider.submittedContexts.front().nearby, (std::vector<std::string>{"Deszy", "Barnek"}));
}

TEST(PlayerbotSocialDeliveryTest, ReplyPressureIsAppliedExactlyOncePerOpportunity)
{
    /*
     * A regression guard for a bug this task introduced and caught before it shipped.
     *
     * Responder selection already rolls the reply pressure against its own seed. Activation rolled
     * it a second time before calling selection, which applied the same probability twice: a thread
     * at 0.6 pressure answered about a third of the time rather than three fifths. Nothing failed, no
     * test broke, and every calibration constant behind these numbers would quietly have been wrong.
     *
     * Measured over seeds rather than asserted structurally, because "rolled once" is a property of
     * the observable answer rate and not of any particular call sequence. A future refactor that
     * moves the roll somewhere else is fine; one that reintroduces a second gate is not.
     */
    constexpr uint64 SAMPLES = 400;

    float expected = 0.0f;
    uint64 answered = 0;

    for (uint64 seed = 1; seed <= SAMPLES; ++seed)
    {
        PlayerbotSocialMgr coordinator;
        RecordingProvider provider;
        coordinator.SetSocialProvider(&provider);

        PlayerbotSocialActivation activation = LiveOpportunity(coordinator, 1000);
        activation.selectionSeed = seed;

        PlayerbotSocialActivationResult const result =
            coordinator.Activate(activation, PlayerbotSocialDensityProfile::Normal);
        expected = result.pressure;

        if (!result.openedTokens.empty())
            ++answered;

        coordinator.SetSocialProvider(nullptr);
    }

    ASSERT_GT(expected, 0.0f);

    float const observed = static_cast<float>(answered) / static_cast<float>(SAMPLES);

    /*
     * The tolerance has to admit sampling noise while still excluding the bug. At this pressure a
     * doubled roll lands near `expected * expected`, which is roughly 0.23 below `expected`, so a
     * window of 0.1 separates them with room to spare.
     */
    EXPECT_NEAR(observed, expected, 0.1f)
        << "answer rate " << observed << " against pressure " << expected << "; a rate near " << (expected * expected)
        << " means pressure is being rolled twice";
}

TEST(PlayerbotSocialDeliveryTest, DensityProfilesReachActivationPressure)
{
    auto pressureFor = [&](PlayerbotSocialDensityProfile profile)
    {
        PlayerbotSocialMgr coordinator;
        RecordingProvider provider;
        coordinator.SetSocialProvider(&provider);

        PlayerbotSocialActivation const activation = LiveOpportunity(coordinator, 1000);
        return coordinator.Activate(activation, profile).pressure;
    };

    float const quiet = pressureFor(PlayerbotSocialDensityProfile::Quiet);
    float const normal = pressureFor(PlayerbotSocialDensityProfile::Normal);
    float const lively = pressureFor(PlayerbotSocialDensityProfile::Lively);

    EXPECT_LT(quiet, normal);
    EXPECT_LT(normal, lively);

    PlayerbotSocialThreadPressure thread;
    thread.relevantHumanMessages = 3;
    thread.lastActivityUnixSeconds = 1000;
    thread.nowUnixSeconds = 1000;
    thread.channelDensity = 0;
    EXPECT_FLOAT_EQ(normal, PlayerbotSocialReplyPressure(thread));
}

TEST(PlayerbotSocialDeliveryTest, ACandidateOnCooldownIsRefusedByNameAndOpensNothing)
{
    // Definition of Done 1's negative half. An ineligible bot must never reach selection, because
    // selection is where the decision to spend a request is made.
    PlayerbotSocialMgr coordinator;
    RecordingProvider provider;
    coordinator.SetSocialProvider(&provider);

    PlayerbotSocialActivation activation = LiveOpportunity(coordinator, 1000);
    activation.selectionSeed = SeedThatAnswers(activation);
    ASSERT_NE(activation.selectionSeed, 0u);

    // Spoke one second ago, well inside the reply cooldown.
    activation.candidates[0].lastSpokeUnixSeconds = 999;

    PlayerbotSocialActivationResult const result =
        coordinator.Activate(activation, PlayerbotSocialDensityProfile::Normal);

    EXPECT_TRUE(result.openedTokens.empty());
    EXPECT_TRUE(provider.submitted.empty());
    EXPECT_FALSE(result.pressureDeclined);
    ASSERT_EQ(result.refusedCandidates.size(), 1u);
    EXPECT_EQ(result.refusedCandidates[0].first, 500u);
    EXPECT_EQ(result.refusedCandidates[0].second, PlayerbotSocialOpportunityRejection::CooldownActive);

    // The thread level reason names the one real cause rather than a generic silence.
    EXPECT_EQ(result.rejection, PlayerbotSocialOpportunityRejection::CooldownActive);

    coordinator.SetSocialProvider(nullptr);
}

TEST(PlayerbotSocialDeliveryTest, AnInvalidThreadHandleOpensNothing)
{
    // Observe returns exactly this handle for an unsupported channel, so a refused observation
    // arriving here is the normal path rather than an exceptional one.
    PlayerbotSocialMgr coordinator;
    RecordingProvider provider;
    coordinator.SetSocialProvider(&provider);

    PlayerbotSocialActivation activation = LiveOpportunity(coordinator, 1000);
    activation.thread.valid = false;

    PlayerbotSocialActivationResult const result =
        coordinator.Activate(activation, PlayerbotSocialDensityProfile::Normal);

    EXPECT_TRUE(result.openedTokens.empty());
    EXPECT_TRUE(provider.submitted.empty());
    EXPECT_EQ(result.rejection, PlayerbotSocialOpportunityRejection::UnsupportedChannel);

    coordinator.SetSocialProvider(nullptr);
}

TEST(PlayerbotSocialDeliveryTest, WithNoProviderAnEligibleOpportunityOpensNothingAndSaysWhy)
{
    /*
     * Definition of Done 5's shape at the activation seam. With the feature off there is no provider,
     * and an opportunity that would otherwise have spoken produces silence plus a reason rather than
     * a canned line or a dangling request.
     */
    PlayerbotSocialMgr coordinator;

    PlayerbotSocialActivation activation = LiveOpportunity(coordinator, 1000);
    activation.selectionSeed = SeedThatAnswers(activation);
    ASSERT_NE(activation.selectionSeed, 0u);

    PlayerbotSocialActivationResult const result =
        coordinator.Activate(activation, PlayerbotSocialDensityProfile::Normal);

    EXPECT_TRUE(result.openedTokens.empty());
    ASSERT_EQ(result.refusedRequests.size(), 1u);
    EXPECT_EQ(result.refusedRequests[0].first, 500u);
    EXPECT_EQ(result.refusedRequests[0].second, PlayerbotSocialDeliveryRejection::NoProvider);
}

TEST(PlayerbotSocialDeliveryTest, AThreadIsRecognisedByItsPublicIdentityAndForgottenWithIt)
{
    /*
     * Delivery revalidation has to refuse a superseded thread, and a pending request carries the
     * thread's PUBLIC identity rather than its internal id: the public id is what crosses the seam
     * into telemetry and Medivh. So currency has to be answerable from that identity alone.
     *
     * Failing closed on an unknown identity is the load bearing half. A thread pruned while an answer
     * was in flight must read as gone, because the alternative is delivering a reply into a
     * conversation that has already ended.
     */
    PlayerbotSocialMgr coordinator;

    PlayerbotSocialObservation observation;
    observation.key.channel = PlayerbotSocialChannel::Say;
    observation.key.scopeId = 42;
    observation.speakerGuidCounter = 900;
    observation.speakerIsHuman = true;
    observation.atUnixSeconds = 1000;

    PlayerbotSocialThreadHandle const thread = coordinator.Observe(observation);
    ASSERT_TRUE(thread.valid);
    ASSERT_FALSE(thread.publicId.empty());

    EXPECT_TRUE(coordinator.ThreadIsCurrent(thread.publicId));

    // A well formed identity nobody issued, and a malformed one, are both refused.
    EXPECT_FALSE(coordinator.ThreadIsCurrent("thr_0000000000000000000000000000ffff"));
    EXPECT_FALSE(coordinator.ThreadIsCurrent(""));
    EXPECT_FALSE(coordinator.ThreadIsCurrent("act_00000000000000000000000000000001"));
}

TEST(PlayerbotSocialDeliveryTest, AThreadReportsTheConversationSpaceItBelongsTo)
{
    /*
     * Delivery revalidation needs this and cannot derive it. A pending request carries the thread's
     * public identity but not its scope, so without a lookup a party answer would be checked against
     * whatever group the bot happens to be in at delivery time rather than the one the conversation
     * happened in, and a say answer against nothing at all. Both are the same defect: the answer
     * checked against the bot's present circumstances instead of the conversation's.
     */
    PlayerbotSocialMgr coordinator;

    PlayerbotSocialObservation observation;
    observation.key.channel = PlayerbotSocialChannel::Party;
    observation.key.scopeId = 7788;
    observation.speakerGuidCounter = 900;
    observation.speakerIsHuman = true;
    observation.atUnixSeconds = 1000;

    PlayerbotSocialThreadHandle const thread = coordinator.Observe(observation);
    ASSERT_TRUE(thread.valid);

    PlayerbotSocialThreadKey scope;
    ASSERT_TRUE(coordinator.ThreadScopeFor(thread.publicId, scope));
    EXPECT_EQ(scope.scopeId, 7788u);
    EXPECT_EQ(scope.channel, PlayerbotSocialChannel::Party);

    /*
     * An identity nobody issued leaves the caller's key untouched and says so, rather than reporting
     * scope zero. Scope zero is a real scope, so returning it for an unknown thread would revalidate
     * an answer against a conversation space it never belonged to.
     */
    PlayerbotSocialThreadKey untouched;
    untouched.scopeId = 4242;
    untouched.channel = PlayerbotSocialChannel::Whisper;

    EXPECT_FALSE(coordinator.ThreadScopeFor("thr_0000000000000000000000000000ffff", untouched));
    EXPECT_FALSE(coordinator.ThreadScopeFor("act_00000000000000000000000000000001", untouched));
    EXPECT_EQ(untouched.scopeId, 4242u);
    EXPECT_EQ(untouched.channel, PlayerbotSocialChannel::Whisper);
}

// Provider attempt telemetry, wired ------------------------------------------------------------------

namespace
{
/*
 * A well formed activation with one eligible candidate, so the whole activation path runs.
 *
 * The event ASSERTIONS below are on counts, not on content. The pure builders in
 * PlayerbotSocialCoordinatorTest own what an event says; these own whether a production path
 * emits one, once. Splitting it that way is what keeps a wiring test from silently re-asserting
 * the builder's own logic.
 */
PlayerbotSocialActivation WiredActivation(PlayerbotSocialMgr& coordinator, uint64 candidateGuidCounter)
{
    PlayerbotSocialActivation activation;
    activation.channel = PlayerbotSocialChannel::Say;
    activation.speakerGuidCounter = 900;
    activation.speakerIsHuman = true;
    activation.zoneId = REQUEST_ZONE_ID;
    activation.nowUnixSeconds = 5000;
    activation.threadLastActivityUnixSeconds = 5000;
    activation.relevantHumanMessages = 4;
    activation.currentLine.eventPublicId = PlayerbotSocialMakeEventPublicId(5000, 900);
    activation.currentLine.speakerGuidCounter = 900;
    activation.currentLine.speakerName = "Elyse";
    activation.currentLine.speakerIsHuman = true;
    activation.currentLine.atUnixSeconds = 5000;
    activation.currentLine.text = "A current fixture line.";

    activation.consecutiveBotOnlyTurns = 0;

    PlayerbotSocialActivationCandidate candidate;
    candidate.botGuidCounter = candidateGuidCounter;
    candidate.personality = StoredPersonality();
    candidate.profileLoadState = PlayerbotSocialProfileLoadState::Loaded;
    candidate.effectiveDisposition = 100;
    candidate.contentRelevance = 100;
    candidate.addressedByName = true;
    candidate.participatedInThread = true;
    candidate.grounding = Grounding();
    candidate.grounding.transcriptEventPublicIds = {activation.currentLine.eventPublicId};
    activation.candidates.push_back(candidate);
    ObserveActivationThread(coordinator, activation);

    return activation;
}

/*
 * The first seed whose pressure roll selects a responder.
 *
 * Found rather than hardcoded, on a throwaway coordinator so the search leaves no events behind
 * in the one being measured. A literal seed would silently stop selecting the day a pressure
 * coefficient moves, and the wiring test would then pass by asserting the wrong path.
 */
uint64 SeedThatSelects(uint64 candidateGuidCounter)
{
    for (uint64 seed = 1; seed < 500; ++seed)
    {
        PlayerbotSocialMgr probe;
        RecordingProvider provider;
        probe.SetSocialProvider(&provider);

        PlayerbotSocialActivation activation = WiredActivation(probe, candidateGuidCounter);
        activation.selectionSeed = seed;

        if (!probe.Activate(activation, PlayerbotSocialDensityProfile::Normal).openedTokens.empty())
            return seed;
    }

    return 0;
}

/*
 * The queued events of one type, as the rows they will be written as.
 *
 * Wiring tests assert on these rather than on a count. A count passes just as happily when a
 * producer queues the wrong event, which is exactly the hole the review found: swapping one
 * valid builder for another would have left every count assertion green.
 */
std::vector<PlayerbotSocialEventBinding> EventsOfType(PlayerbotSocialMgr const& coordinator, std::string_view eventType)
{
    std::vector<PlayerbotSocialEventBinding> matching;
    for (PlayerbotSocialEventBinding const& binding : coordinator.PendingEvents())
        if (binding.eventType == eventType)
            matching.push_back(binding);

    return matching;
}

/*
 * The COMPLETE binding contract for one provider attempt.
 *
 * A helper rather than a field list repeated per test, for the same reason the production field
 * tables are single tables: an assertion set maintained by hand at six call sites drifts, and the
 * copy that drifts is the one nobody re-reads. Three review rounds found progressively smaller
 * holes in hand-maintained assertions here; this closes the shape rather than the instance.
 *
 * Every field is asserted, including the ones expected to be EMPTY or ZERO. A wrong producer
 * value that no test names is indistinguishable from a correct one.
 */
struct ExpectedAttempt
{
    std::string outcome;
    std::string reason;
    uint64 botGuidCounter = 0;
    uint64 targetGuidCounter = 0;
    std::string channel;
    std::string threadPublicId;
    uint64 token = 0;
    uint32 zoneId = 0;
    std::optional<PlayerbotSocialCallMetadata> callMetadata;
    bool operatorEvidenceExpected = true;

    /*
     * The timestamp as a closed range rather than a value, because the attempt paths do not share
     * one clock. A path given its `now` is pinned by setting both bounds to it; a path that reads
     * the wall clock at conclusion is bounded by readings taken around the call.
     *
     * One mechanism for both keeps the helper branch free, and a range still has teeth: it fails
     * a producer that leaves the field zero, copies the wrong one, or reaches for the monotonic
     * uptime clock that the delivery scheduler uses.
     */
    uint64 occurredAtAtLeast = 0;
    uint64 occurredAtAtMost = 0;
};

void ExpectAttemptMatches(PlayerbotSocialEventBinding const& binding, ExpectedAttempt const& expected,
                          std::string_view label)
{
    SCOPED_TRACE(std::string(label));

    EXPECT_EQ(binding.eventType, PLAYERBOT_SOCIAL_EVENT_TYPE_PROVIDER_ATTEMPT);
    EXPECT_EQ(binding.origin, "social");
    EXPECT_EQ(binding.outcome, expected.outcome);
    EXPECT_EQ(binding.reason, expected.reason);
    EXPECT_EQ(binding.botGuidCounter, expected.botGuidCounter);
    EXPECT_EQ(binding.targetGuidCounter, expected.targetGuidCounter);

    // An attempt is between the coordinator and the provider. No third character acted, so an
    // actor here would attribute the request to somebody who had nothing to do with it.
    EXPECT_EQ(binding.actorGuidCounter, 0u);

    EXPECT_TRUE(binding.hasChannel);
    EXPECT_EQ(binding.channel, expected.channel) << "the surface the bot was ASKED to speak on";
    EXPECT_EQ(binding.threadPublicId, expected.threadPublicId) << "the correlation key";

    // Only the delivery event retains a line. An attempt describes a round trip, not speech.
    EXPECT_TRUE(binding.messageText.empty());

    EXPECT_EQ(binding.zoneId, expected.zoneId) << "where the exchange happened, carried from the request";

    EXPECT_GE(binding.occurredAtUnixSeconds, expected.occurredAtAtLeast);
    EXPECT_LE(binding.occurredAtUnixSeconds, expected.occurredAtAtMost);

    /*
     * Pinned structurally, not to a literal. The value is derived from a sequence and a salt, so
     * naming one here would assert the hash rather than the contract. What matters is that every
     * attempt gets an identity of the EVENT kind: the validator refuses an empty one, a truncated
     * one, and one minted in the thread or actor namespace.
     */
    EXPECT_TRUE(PlayerbotSocialPublicIdIsValid(PlayerbotSocialIdKind::Event, binding.publicId))
        << "public id was: " << binding.publicId;

    EXPECT_NE(binding.diagnosticsJson.find("\"token\":" + std::to_string(expected.token)), std::string::npos);
    if (expected.operatorEvidenceExpected)
        EXPECT_NE(binding.diagnosticsJson.find("\"evidence\":{"), std::string::npos)
            << "every opened terminal attempt keeps the bounded pending evidence";
    else
        EXPECT_EQ(binding.diagnosticsJson.find("\"evidence\":{"), std::string::npos);

    if (!expected.callMetadata)
        return;

    EXPECT_NE(binding.diagnosticsJson.find("\"model\":\"" + expected.callMetadata->model + "\""), std::string::npos);
    EXPECT_NE(binding.diagnosticsJson.find("\"provider_latency_ms\":" +
                                           std::to_string(expected.callMetadata->providerLatencyMs)),
              std::string::npos);
    EXPECT_NE(binding.diagnosticsJson.find("\"input_tokens\":" + std::to_string(expected.callMetadata->inputTokens)),
              std::string::npos);
    EXPECT_NE(binding.diagnosticsJson.find("\"output_tokens\":" + std::to_string(expected.callMetadata->outputTokens)),
              std::string::npos);
    EXPECT_NE(binding.diagnosticsJson.find("\"cache_creation_input_tokens\":" +
                                           std::to_string(expected.callMetadata->cacheCreationInputTokens)),
              std::string::npos);
    EXPECT_NE(binding.diagnosticsJson.find("\"cache_read_input_tokens\":" +
                                           std::to_string(expected.callMetadata->cacheReadInputTokens)),
              std::string::npos);
    EXPECT_NE(binding.diagnosticsJson.find("\"cost_usd\":\"" + expected.callMetadata->costUsd + "\""),
              std::string::npos);
}
}  // namespace

TEST(PlayerbotSocialDeliveryTest, AStarterIsOwnedByTheBotThatSuppliedTheAuthoritativeSource)
{
    /*
     * The thread comes from the coordinator's own speakerless entry point rather than being
     * hand-built, because that is what production does. The activation contains both the event owner
     * and another eligible character so this proves ownership at the provider boundary, not merely
     * that a starter can obtain a thread.
     */
    PlayerbotSocialMgr coordinator;
    RecordingProvider provider;
    coordinator.SetSocialProvider(&provider);

    PlayerbotSocialThreadKey key;
    key.channel = PlayerbotSocialChannel::General;
    key.scopeId = REQUEST_ZONE_ID;

    PlayerbotSocialStarterContext context;
    context.key = key;
    context.botGuidCounter = 500;
    context.source.kind = PlayerbotSocialStarterSourceKind::Loot;
    context.source.sourceEventPublicId = PlayerbotSocialMakeEventPublicId(5000, 500);
    context.source.subjectId = 1;
    context.source.subject = "a rare drop";
    context.audienceGuidCounter = 900;
    context.zoneId = REQUEST_ZONE_ID;
    context.atUnixSeconds = 5000;
    ASSERT_TRUE(coordinator.NoteStarterContext(context));

    PlayerbotSocialThreadHandle const thread = coordinator.OpenStarterThread(key, 5000);
    ASSERT_TRUE(thread.valid);

    PlayerbotSocialActivation activation;
    activation.thread = thread;
    activation.channel = PlayerbotSocialChannel::General;
    activation.starter = true;
    activation.starterSourceBotGuidCounter = context.botGuidCounter;
    activation.starterAudienceGuidCounter = context.audienceGuidCounter;
    activation.starterSourceEventPublicId = context.source.sourceEventPublicId;

    // Nobody spoke. Zero is what says so, and it must not be read as a speaker who opted out.
    activation.speakerGuidCounter = 0;

    // Taken from the pending context, exactly as the pump takes it from the freshest starter.
    activation.starterSubject = PlayerbotSocialStarterGroundingSubject(context.source);

    activation.zoneId = REQUEST_ZONE_ID;
    activation.nowUnixSeconds = 5000;
    activation.threadLastActivityUnixSeconds = 5000;

    PlayerbotSocialActivationCandidate candidate;
    candidate.botGuidCounter = 500;
    candidate.personality = StoredPersonality();
    candidate.profileLoadState = PlayerbotSocialProfileLoadState::Loaded;
    // Room-addressed like every real General starter: no Participant evidence, no wire subject.
    candidate.grounding = RoomGrounding();
    candidate.effectiveDisposition = 100;
    candidate.contentRelevance = 100;
    activation.candidates.push_back(candidate);

    PlayerbotSocialActivationCandidate other = candidate;
    other.botGuidCounter = 501;
    activation.candidates.push_back(other);

    /*
     * Searched rather than fixed, because a starter rolls against the STARTER pressure curve, which
     * is deliberately lower than the reply curve. A hardcoded seed would assert whichever side of
     * that curve it happened to land on and would quietly become an assertion about the wrong path
     * the first time a coefficient moved.
     */
    uint64 selectingSeed = 0;
    for (uint64 seed = 1; seed <= 4096 && selectingSeed == 0; ++seed)
    {
        PlayerbotSocialMgr probe;
        RecordingProvider probeProvider;
        probe.SetSocialProvider(&probeProvider);
        probe.NoteStarterContext(context);

        PlayerbotSocialActivation attempt = activation;
        attempt.thread = probe.OpenStarterThread(key, 5000);
        attempt.selectionSeed = seed;

        PlayerbotSocialActivationResult const result = probe.Activate(attempt, PlayerbotSocialDensityProfile::Normal);
        if (!result.openedTokens.empty() && probeProvider.submittedBots.front() == context.botGuidCounter)
            selectingSeed = seed;
    }

    ASSERT_NE(selectingSeed, 0u) << "no seed in the range starts a conversation, so starter pressure "
                                    "never selects and the path is still unreachable";

    activation.selectionSeed = selectingSeed;
    PlayerbotSocialActivationResult const result =
        coordinator.Activate(activation, PlayerbotSocialDensityProfile::Normal);

    ASSERT_EQ(result.openedTokens.size(), 1u) << "a starter must be able to reach the provider";
    ASSERT_EQ(provider.submittedBots.size(), 1u);
    EXPECT_EQ(provider.submittedBots.front(), context.botGuidCounter)
        << "only the bot that performed the authoritative event may speak about it";
    ASSERT_EQ(result.refusedCandidates.size(), 1u);
    EXPECT_EQ(result.refusedCandidates.front().first, 501u);
    EXPECT_EQ(result.refusedCandidates.front().second, PlayerbotSocialOpportunityRejection::StarterSourceMismatch);

    // Addressed to the room. A General line carrying a target would read as a private reply.
    ASSERT_EQ(provider.submittedTargets.size(), 1u);
    EXPECT_EQ(provider.submittedTargets.front(), 0u);

    /*
     * The subject is the entire content of a starter. Without it the provider is told only that some
     * bot wishes to speak, and answers about nothing in particular: the path would look wired and
     * would produce a line that has nothing to do with what happened.
     */
    ASSERT_EQ(provider.submittedSubjects.size(), 1u);
    EXPECT_EQ(provider.submittedSubjects.front(), "loot: a rare drop");

    PlayerbotSocialMgr missingOwnerCoordinator;
    RecordingProvider missingOwnerProvider;
    missingOwnerCoordinator.SetSocialProvider(&missingOwnerProvider);
    ASSERT_TRUE(missingOwnerCoordinator.NoteStarterContext(context));

    PlayerbotSocialActivation missingOwner = activation;
    missingOwner.thread = missingOwnerCoordinator.OpenStarterThread(key, 5000);
    missingOwner.candidates = {other};

    PlayerbotSocialActivationResult const missingOwnerResult =
        missingOwnerCoordinator.Activate(missingOwner, PlayerbotSocialDensityProfile::Normal);

    EXPECT_TRUE(missingOwnerResult.openedTokens.empty());
    EXPECT_TRUE(missingOwnerProvider.submittedBots.empty());
    ASSERT_EQ(missingOwnerResult.refusedCandidates.size(), 1u);
    EXPECT_EQ(missingOwnerResult.refusedCandidates.front().second,
              PlayerbotSocialOpportunityRejection::StarterSourceMismatch);
}

TEST(PlayerbotSocialDeliveryTest, AReplyStillRefusesTheCharacterWhoseLineItObserved)
{
    PlayerbotSocialMgr coordinator;
    RecordingProvider provider;
    coordinator.SetSocialProvider(&provider);

    PlayerbotSocialActivation activation = LiveOpportunity(coordinator, 5000);
    activation.candidates = {WillingCandidate(activation.speakerGuidCounter)};

    PlayerbotSocialActivationResult const result =
        coordinator.Activate(activation, PlayerbotSocialDensityProfile::Normal);

    EXPECT_TRUE(result.openedTokens.empty());
    EXPECT_TRUE(provider.submittedBots.empty());
    ASSERT_EQ(result.refusedCandidates.size(), 1u);
    EXPECT_EQ(result.refusedCandidates.front().first, activation.speakerGuidCounter);
    EXPECT_EQ(result.refusedCandidates.front().second, PlayerbotSocialOpportunityRejection::SelfReply);
}

TEST(PlayerbotSocialDeliveryTest, AReplyCarriesNoStarterSubject)
{
    /*
     * A reply's subject is the message it answers, which the thread already carries. Letting a stale
     * `starterSubject` ride along on a reply would hand the provider a second, older topic and
     * quietly change what the bot is answering about.
     */
    PlayerbotSocialMgr coordinator;
    RecordingProvider provider;
    coordinator.SetSocialProvider(&provider);

    uint64 const seed = SeedThatSelects(500);
    ASSERT_NE(seed, 0u);

    PlayerbotSocialActivation activation = WiredActivation(coordinator, 500);
    activation.selectionSeed = seed;

    // Set on a REPLY, where it has no business being honoured.
    activation.starter = false;
    activation.starterSubject = "a rare drop";

    ASSERT_EQ(coordinator.Activate(activation, PlayerbotSocialDensityProfile::Normal).openedTokens.size(), 1u);

    ASSERT_EQ(provider.submittedSubjects.size(), 1u);
    EXPECT_TRUE(provider.submittedSubjects.front().empty());
}

TEST(PlayerbotSocialDeliveryTest, AnOptedOutWhisperUsesOnlyPersonaAndTheCurrentLine)
{
    PlayerbotSocialMgr coordinator;
    RecordingProvider provider;
    coordinator.SetSocialProvider(&provider);

    uint64 const seed = SeedThatSelects(500);
    ASSERT_NE(seed, 0u);

    PlayerbotSocialActivation activation = WiredActivation(coordinator, 500);
    activation.channel = PlayerbotSocialChannel::Whisper;
    activation.speakerOptedOut = true;
    activation.selectionSeed = seed;
    activation.currentLine.speakerGuidCounter = 900;
    activation.currentLine.speakerName = "PrivateHuman";
    activation.currentLine.speakerIsHuman = true;
    activation.currentLine.atUnixSeconds = activation.nowUnixSeconds;
    activation.currentLine.text = "What kind of work would suit a mage outside Stormwind?";

    PlayerbotSocialActivationResult const opened =
        coordinator.Activate(activation, PlayerbotSocialDensityProfile::Normal);
    ASSERT_EQ(opened.openedTokens.size(), 1u);
    ASSERT_EQ(provider.submittedContexts.size(), 1u);

    PlayerbotSocialRequestContext const& context = provider.submittedContexts.front();
    EXPECT_FALSE(context.persona.empty());
    EXPECT_TRUE(context.relationship.empty());
    EXPECT_TRUE(context.memories.empty());
    EXPECT_TRUE(context.nearby.empty());
    EXPECT_TRUE(context.starter.empty());
    EXPECT_EQ(context.thread,
              (std::vector<std::string>{"PrivateHuman: What kind of work would suit a mage outside Stormwind?"}));

    PlayerbotSocialPendingDelivery pending;
    ASSERT_TRUE(coordinator.PendingDeliveryFor(opened.openedTokens.front(), pending));
    EXPECT_TRUE(pending.statelessDirectReply);

    EXPECT_EQ(coordinator.PendingEventCount(), 0u) << "an opted-out exchange must not enter the telemetry queue";

    PlayerbotSocialProviderResult answer =
        Message("The road east needs capable hands.", PlayerbotSocialChannel::Whisper);
    answer.requestToken = opened.openedTokens.front();
    EXPECT_EQ(coordinator.AcceptSocialResult(answer, 100000, 3), PlayerbotSocialDeliveryRejection::None);
    EXPECT_EQ(coordinator.PendingEventCount(), 0u) << "the provider conclusion must remain private too";
}

TEST(PlayerbotSocialDeliveryTest, ASelectedResponderIsRecordedBesideTheOpportunityItAnswered)
{
    /*
     * Definition of Done 5: a delivered line needs an opportunity, a selection, a provider attempt
     * and a delivery event. The opportunity was already wired; without the selection event the
     * chain has a hole in the middle, and a second responder has no record at all.
     */
    uint64 const seed = SeedThatSelects(500);
    ASSERT_NE(seed, 0u) << "no seed in the search range selects, so the fixture is wrong, not the wiring";

    PlayerbotSocialMgr coordinator;
    RecordingProvider provider;
    coordinator.SetSocialProvider(&provider);

    PlayerbotSocialActivation activation = WiredActivation(coordinator, 500);
    activation.selectionSeed = seed;

    PlayerbotSocialActivationResult const result =
        coordinator.Activate(activation, PlayerbotSocialDensityProfile::Normal);
    ASSERT_EQ(result.openedTokens.size(), 1u) << "the activation must reach a request for this to mean anything";

    // Opportunity and selection. The attempt is not here yet: the request is outstanding, and its
    // outcome is not known until the provider answers or the request expires.
    EXPECT_EQ(coordinator.PendingEventCount(), 2u);

    std::vector<PlayerbotSocialEventBinding> const selection =
        EventsOfType(coordinator, PLAYERBOT_SOCIAL_EVENT_TYPE_SELECTION);
    ASSERT_EQ(selection.size(), 1u) << "one selection event, for the one responder chosen";
    EXPECT_EQ(selection.front().origin, "social");
    EXPECT_EQ(selection.front().outcome, "recorded");
    EXPECT_EQ(selection.front().botGuidCounter, 500u) << "the responder, not the speaker";
    EXPECT_EQ(selection.front().actorGuidCounter, 900u);
    EXPECT_EQ(selection.front().threadPublicId, activation.thread.publicId);
    EXPECT_EQ(selection.front().channel, "say");
    EXPECT_TRUE(selection.front().reason.empty()) << "a bot that WAS selected has no suppression to name";
    EXPECT_TRUE(selection.front().messageText.empty()) << "selection decides who speaks, not what";
    EXPECT_TRUE(selection.front().hasChannel);
    EXPECT_EQ(selection.front().targetGuidCounter, 0u)
        << "a say opportunity is addressed to a room; only a whisper names a target";
    EXPECT_EQ(selection.front().zoneId, 12u);

    EXPECT_TRUE(EventsOfType(coordinator, PLAYERBOT_SOCIAL_EVENT_TYPE_PROVIDER_ATTEMPT).empty())
        << "an outstanding request has no conclusion to report yet";
}

TEST(PlayerbotSocialDeliveryTest, ARequestThatCouldNotOpenRecordsItsAttemptImmediately)
{
    // Nothing will ever answer this request, so waiting for a conclusion would mean never recording
    // one. The refusal IS the conclusion.
    uint64 const seed = SeedThatSelects(500);
    ASSERT_NE(seed, 0u);

    // No provider. Selection still runs and still chooses; the request is what cannot open.
    PlayerbotSocialMgr coordinator;

    PlayerbotSocialActivation activation = WiredActivation(coordinator, 500);
    activation.selectionSeed = seed;

    PlayerbotSocialActivationResult const result =
        coordinator.Activate(activation, PlayerbotSocialDensityProfile::Normal);
    ASSERT_TRUE(result.openedTokens.empty());
    ASSERT_EQ(result.refusedRequests.size(), 1u) << "selection chose a bot and the request was refused";

    // Opportunity, selection, and the attempt that never opened.
    EXPECT_EQ(coordinator.PendingEventCount(), 3u);

    std::vector<PlayerbotSocialEventBinding> const attempts =
        EventsOfType(coordinator, PLAYERBOT_SOCIAL_EVENT_TYPE_PROVIDER_ATTEMPT);
    ASSERT_EQ(attempts.size(), 1u);

    // Token zero is what says the request never reached a provider. No sentinel is invented for it.
    // This path concludes inside the activation, so it carries the activation's own clock and zone.
    ExpectAttemptMatches(attempts.front(),
                         {.outcome = "failed",
                          .reason = "no_provider",
                          .botGuidCounter = 500u,
                          .targetGuidCounter = 0u,
                          .channel = "say",
                          .threadPublicId = activation.thread.publicId,
                          .token = 0u,
                          .zoneId = REQUEST_ZONE_ID,
                          .operatorEvidenceExpected = false,
                          .occurredAtAtLeast = 5000u,
                          .occurredAtAtMost = 5000u},
                         "never opened");
}

TEST(PlayerbotSocialDeliveryTest, AnAnsweredRequestRecordsExactlyOneAttempt)
{
    PlayerbotSocialMgr coordinator;
    RecordingProvider provider;
    coordinator.SetSocialProvider(&provider);

    uint64 const token = OpenRequest(coordinator, 500, PlayerbotSocialChannel::Say, 1000);
    ASSERT_NE(token, 0u);
    ASSERT_EQ(coordinator.PendingEventCount(), 0u) << "opening a request outside an activation records nothing yet";

    PlayerbotSocialProviderResult result = Message("aye", PlayerbotSocialChannel::Say);
    result.requestToken = token;
    result.callMetadata = PlayerbotSocialCallMetadata{
        "fixture-social-model", 42, 100, 50, 20, 30, "0.002900",
    };

    /*
     * Bracketed by wall clock readings. A conclusion arriving from the provider has no injected
     * clock, and `AcceptSocialResult`'s millisecond argument is the monotonic uptime counter the
     * delivery scheduler runs on, so a producer reaching for it lands decades away from these bounds.
     */
    uint64 const before = static_cast<uint64>(time(nullptr));
    ASSERT_EQ(coordinator.AcceptSocialResult(result, 100000, 3), PlayerbotSocialDeliveryRejection::None);
    uint64 const after = static_cast<uint64>(time(nullptr));

    std::vector<PlayerbotSocialEventBinding> attempts =
        EventsOfType(coordinator, PLAYERBOT_SOCIAL_EVENT_TYPE_PROVIDER_ATTEMPT);
    ASSERT_EQ(attempts.size(), 1u);
    ExpectAttemptMatches(attempts.front(),
                         {.outcome = "recorded",
                          .reason = "",
                          .botGuidCounter = 500u,
                          .targetGuidCounter = 900u,
                          .channel = "say",
                          .threadPublicId = provider.submittedThreads.back(),
                          .token = token,
                          .zoneId = REQUEST_ZONE_ID,
                          .callMetadata = result.callMetadata,
                          .occurredAtAtLeast = before,
                          .occurredAtAtMost = after},
                         "answered");

    // A second result for the same token answers a request that is already answered, and must not
    // record a second attempt for it.
    EXPECT_EQ(coordinator.AcceptSocialResult(result, 100000, 3), PlayerbotSocialDeliveryRejection::UnknownRequest);
    EXPECT_EQ(EventsOfType(coordinator, PLAYERBOT_SOCIAL_EVENT_TYPE_PROVIDER_ATTEMPT).size(), 1u);
}

TEST(PlayerbotSocialDeliveryTest, ARefusedAnswerRecordsAnAttemptRatherThanVanishing)
{
    PlayerbotSocialMgr coordinator;
    RecordingProvider provider;
    coordinator.SetSocialProvider(&provider);

    uint64 const token = OpenRequest(coordinator, 500, PlayerbotSocialChannel::Say, 1000);
    ASSERT_NE(token, 0u);

    // A channel the bot was not asked to speak on. The coordinator refuses it, and the feed has to
    // say so: a provider answering on the wrong surface is invisible otherwise.
    PlayerbotSocialProviderResult result = Message("aye", PlayerbotSocialChannel::General);
    result.requestToken = token;

    uint64 const before = static_cast<uint64>(time(nullptr));
    ASSERT_NE(coordinator.AcceptSocialResult(result, 100000, 3), PlayerbotSocialDeliveryRejection::None);
    uint64 const after = static_cast<uint64>(time(nullptr));

    std::vector<PlayerbotSocialEventBinding> const attempts =
        EventsOfType(coordinator, PLAYERBOT_SOCIAL_EVENT_TYPE_PROVIDER_ATTEMPT);
    ASSERT_EQ(attempts.size(), 1u);

    // The channel is the one the bot was ASKED to speak on, not the one the result named. That is
    // what makes a channel switch a refusal rather than a redirect.
    ExpectAttemptMatches(attempts.front(),
                         {.outcome = "failed",
                          .reason = "channel_switch",
                          .botGuidCounter = 500u,
                          .targetGuidCounter = 900u,
                          .channel = "say",
                          .threadPublicId = provider.submittedThreads.back(),
                          .token = token,
                          .zoneId = REQUEST_ZONE_ID,
                          .occurredAtAtLeast = before,
                          .occurredAtAtMost = after},
                         "refused answer");
}

TEST(PlayerbotSocialDeliveryTest, ALaterDeliveryRefusalKeepsTheAnsweredAttemptMetadata)
{
    PlayerbotSocialMgr coordinator;
    RecordingProvider provider;
    coordinator.SetSocialProvider(&provider);

    uint64 const token = OpenRequest(coordinator, 500, PlayerbotSocialChannel::Say, 1000);
    ASSERT_NE(token, 0u);

    PlayerbotSocialProviderResult answer = Message("aye", PlayerbotSocialChannel::Say);
    answer.requestToken = token;
    answer.callMetadata = PlayerbotSocialCallMetadata{
        "fixture-social-model", 42, 100, 50, 20, 30, "0.002900",
    };
    ASSERT_EQ(coordinator.AcceptSocialResult(answer, 100000, 3), PlayerbotSocialDeliveryRejection::None);

    PlayerbotSocialPendingDelivery retained;
    ASSERT_TRUE(coordinator.PendingDeliveryFor(token, retained));
    ASSERT_TRUE(retained.operatorEvidence.has_value());
    EXPECT_EQ(retained.operatorEvidence->contribution, PlayerbotSocialContributionFunction::FactFreeBanter);
    EXPECT_EQ(retained.operatorEvidence->grounding.transcriptEventPublicIds,
              provider.submittedContexts.back().grounding.transcriptEventPublicIds);

    PlayerbotSocialDeliveryConditions moved = AllHold();
    moved.threadStillCurrent = false;
    EXPECT_EQ(coordinator.CompleteDelivery(token, moved), PlayerbotSocialDeliveryRejection::SupersededThread);

    std::vector<PlayerbotSocialEventBinding> const attempts =
        EventsOfType(coordinator, PLAYERBOT_SOCIAL_EVENT_TYPE_PROVIDER_ATTEMPT);
    ASSERT_EQ(attempts.size(), 1u);
    EXPECT_NE(attempts.front().diagnosticsJson.find("\"model\":\"fixture-social-model\""), std::string::npos);
    EXPECT_NE(attempts.front().diagnosticsJson.find("\"cost_usd\":\"0.002900\""), std::string::npos);
    std::vector<PlayerbotSocialEventBinding> const suppressions =
        EventsOfType(coordinator, PLAYERBOT_SOCIAL_EVENT_TYPE_DELIVERY);
    ASSERT_EQ(suppressions.size(), 1u);
    EXPECT_EQ(suppressions.front().outcome, "suppressed");
    EXPECT_EQ(suppressions.front().reason, "superseded_thread");
    EXPECT_TRUE(suppressions.front().messageText.empty());
    EXPECT_NE(suppressions.front().diagnosticsJson.find("\"evidence\":{"), std::string::npos);
    EXPECT_NE(suppressions.front().diagnosticsJson.find("\"profile\":{\"state\":\"pending\""), std::string::npos);
    EXPECT_NE(suppressions.front().diagnosticsJson.find("\"function\":\"fact_free_banter\""), std::string::npos);
}

TEST(PlayerbotSocialDeliveryTest, AResultForAnUnknownTokenRecordsNothing)
{
    /*
     * There is no request to attribute it to: no bot, no thread, no channel. Recording it would put
     * a row in a thread scoped feed under an identity invented for the occasion.
     */
    PlayerbotSocialMgr coordinator;
    RecordingProvider provider;
    coordinator.SetSocialProvider(&provider);

    PlayerbotSocialProviderResult stranger = Message("aye", PlayerbotSocialChannel::Say);
    stranger.requestToken = 4242;

    EXPECT_EQ(coordinator.AcceptSocialResult(stranger, 100000, 3), PlayerbotSocialDeliveryRejection::UnknownRequest);
    EXPECT_EQ(coordinator.PendingEventCount(), 0u);
}

TEST(PlayerbotSocialDeliveryTest, AnExpiredRequestRecordsTheAttemptItAbandoned)
{
    // A request that timed out is the one failure mode with no other trace: nothing was spoken, no
    // result arrived, and the pending entry is erased. Without this the feed shows an opportunity
    // and a selection leading nowhere.
    PlayerbotSocialMgr coordinator;
    RecordingProvider provider;
    coordinator.SetSocialProvider(&provider);

    uint64 const token = OpenRequest(coordinator, 500, PlayerbotSocialChannel::Say, 1000);
    ASSERT_NE(token, 0u);

    uint64 const expiredAt = 1000 + PLAYERBOT_SOCIAL_PROVIDER_TIMEOUT_SECONDS;
    ASSERT_EQ(coordinator.ExpireTimedOutRequests(expiredAt).size(), 1u);

    std::vector<PlayerbotSocialEventBinding> const attempts =
        EventsOfType(coordinator, PLAYERBOT_SOCIAL_EVENT_TYPE_PROVIDER_ATTEMPT);
    ASSERT_EQ(attempts.size(), 1u);

    // Given its `now`, so the timestamp is pinned exactly: the sweep reports when the request was
    // declared dead, not when the sweep happened to run.
    ExpectAttemptMatches(attempts.front(),
                         {.outcome = "failed",
                          .reason = "provider_timed_out",
                          .botGuidCounter = 500u,
                          .targetGuidCounter = 900u,
                          .channel = "say",
                          .threadPublicId = provider.submittedThreads.back(),
                          .token = token,
                          .zoneId = REQUEST_ZONE_ID,
                          .occurredAtAtLeast = expiredAt,
                          .occurredAtAtMost = expiredAt},
                         "timed out");
}

TEST(PlayerbotSocialDeliveryTest, ACancelledRequestRecordsTheAttemptItAbandoned)
{
    /*
     * Cancellation is the fourth and last way a request can end, and it was the only one with no
     * row. The outcome enum's own comment says a shutdown is a refusal carrying its rejection name,
     * so leaving this uninstrumented made the comment describe something the code did not do.
     *
     * This function has no production caller yet, only tests, so no shutdown drain is added for it.
     * The event is queued correctly; whether it reaches storage is the caller's to arrange.
     */
    PlayerbotSocialMgr coordinator;
    RecordingProvider provider;
    coordinator.SetSocialProvider(&provider);

    uint64 const token = OpenRequest(coordinator, 500, PlayerbotSocialChannel::Say, 1000);
    ASSERT_NE(token, 0u);

    uint64 const before = static_cast<uint64>(time(nullptr));
    ASSERT_EQ(coordinator.CancelPendingDeliveries().size(), 1u);
    uint64 const after = static_cast<uint64>(time(nullptr));

    std::vector<PlayerbotSocialEventBinding> const attempts =
        EventsOfType(coordinator, PLAYERBOT_SOCIAL_EVENT_TYPE_PROVIDER_ATTEMPT);
    ASSERT_EQ(attempts.size(), 1u);
    ExpectAttemptMatches(attempts.front(),
                         {.outcome = "failed",
                          .reason = "shutting_down",
                          .botGuidCounter = 500u,
                          .targetGuidCounter = 900u,
                          .channel = "say",
                          .threadPublicId = provider.submittedThreads.back(),
                          .token = token,
                          .zoneId = REQUEST_ZONE_ID,
                          .occurredAtAtLeast = before,
                          .occurredAtAtMost = after},
                         "cancelled");
}

TEST(PlayerbotSocialDeliveryTest, AnAnsweredRequestCancelledAtShutdownRecordsNoSecondAttempt)
{
    /*
     * A request whose result arrived stays in the pending map, waiting out its natural delay, so a
     * cancellation walks straight over it. Its attempt was already concluded as `Answered` when the
     * provider replied, and reporting it again as `shutting_down` would put two attempts under one
     * token and break the once per request contract this event exists to make countable.
     *
     * What shutdown actually costs an answered request is its DELIVERY. There is no delivery event
     * to suppress, because nothing was spoken.
     */
    PlayerbotSocialMgr coordinator;
    RecordingProvider provider;
    coordinator.SetSocialProvider(&provider);

    uint64 const token = OpenRequest(coordinator, 500, PlayerbotSocialChannel::Say, 1000);
    ASSERT_NE(token, 0u);

    PlayerbotSocialProviderResult result = Message("aye", PlayerbotSocialChannel::Say);
    result.requestToken = token;
    ASSERT_EQ(coordinator.AcceptSocialResult(result, 100000, 3), PlayerbotSocialDeliveryRejection::None);
    ASSERT_EQ(EventsOfType(coordinator, PLAYERBOT_SOCIAL_EVENT_TYPE_PROVIDER_ATTEMPT).size(), 1u);

    ASSERT_EQ(coordinator.CancelPendingDeliveries().size(), 1u) << "the entry is still cancelled";

    std::vector<PlayerbotSocialEventBinding> const attempts =
        EventsOfType(coordinator, PLAYERBOT_SOCIAL_EVENT_TYPE_PROVIDER_ATTEMPT);
    ASSERT_EQ(attempts.size(), 1u) << "still exactly one attempt for this token";
    EXPECT_EQ(attempts.front().outcome, "recorded") << "and it is still the answer, not the shutdown";
    EXPECT_TRUE(attempts.front().reason.empty());
}

// Task 15A: the request context the worldserver composes ----------------------------------------

TEST(PlayerbotSocialRequestContextTest, ARenderedPersonaNamesTheVoiceTheStanceAndTheMood)
{
    /*
     * The persona is a struct on this side and one fenced block of text on the far side, so
     * something has to render it. What the model can actually use is the voice, how the bot feels
     * about whoever it is answering, and the mood dials; a struct dump would be neither.
     */
    PlayerbotEffectiveSocialPersona persona;
    persona.base = StoredPersonality();
    persona.base.voice = PlayerbotVoice::Wry;
    persona.stance = PlayerbotSocialStance::Reserved;
    persona.traits.warmth = 20;
    persona.traits.talkativeness = 80;
    persona.traits.interests = {"fishing", "old coins"};
    persona.traits.aversions = {"crowds"};

    std::string const rendered = PlayerbotSocialRenderPersona(persona);

    EXPECT_NE(rendered.find("wry"), std::string::npos) << "the voice is the bot's whole manner of speaking";
    EXPECT_NE(rendered.find("reserved"), std::string::npos) << "the stance is how it feels about this listener";
    EXPECT_NE(rendered.find("fishing"), std::string::npos);
    EXPECT_NE(rendered.find("crowds"), std::string::npos);
    EXPECT_LE(rendered.size(), PLAYERBOT_SOCIAL_CONTEXT_ENTRY_BYTES);
}

TEST(PlayerbotSocialRequestContextTest, ARenderedPersonaStaysWithinTheEntryBoundNoMatterHowManyInterests)
{
    /*
     * Interests and aversions are evolving lists loaded from a row. The far side REJECTS an entry
     * over the bound rather than truncating it, and a rejected context is dropped, so an unbounded
     * render here is a bot that silently stops having a persona at all.
     */
    PlayerbotEffectiveSocialPersona persona;
    persona.base = StoredPersonality();
    for (int index = 0; index < 64; ++index)
        persona.traits.interests.push_back(std::string(64, 'a' + (index % 26)));

    std::string const rendered = PlayerbotSocialRenderPersona(persona);

    EXPECT_LE(rendered.size(), PLAYERBOT_SOCIAL_CONTEXT_ENTRY_BYTES);
    EXPECT_FALSE(rendered.empty()) << "bounding must not empty the persona, only shorten it";
}

TEST(PlayerbotSocialRequestContextTest, AReadyBiographyShapesTheRenderedPersona)
{
    PlayerbotEffectiveSocialPersona persona;
    persona.base = StoredPersonality();
    persona.biographyState = PlayerbotBiographyState::Ready;
    persona.biography.origin = "usually explores while leveling";
    persona.biography.motivation = "wants to finish every Northrend dungeon";
    persona.biography.formativeExperience = "learned routes by following patient groups";
    persona.biography.preferredTopics = "dungeon routes and profession choices";
    persona.biography.mannerisms = "keeps replies short and dry";
    persona.biography.values = "prepared groups and fair loot";

    std::string const rendered = PlayerbotSocialRenderPersona(persona);

    EXPECT_NE(rendered.find("; play approach: "), std::string::npos);
    EXPECT_NE(rendered.find("; play motivation: "), std::string::npos);
    EXPECT_NE(rendered.find("; learning history: "), std::string::npos);
    EXPECT_NE(rendered.find("; chat habits: "), std::string::npos);
    EXPECT_NE(rendered.find("; group values: "), std::string::npos);
    EXPECT_EQ(rendered.find("; origin: "), std::string::npos);
    EXPECT_EQ(rendered.find("; formative experience: "), std::string::npos);
    EXPECT_NE(rendered.find("usually explores while leveling"), std::string::npos);
    EXPECT_NE(rendered.find("wants to finish every Northrend dungeon"), std::string::npos);
    EXPECT_NE(rendered.find("dungeon routes and profession choices"), std::string::npos);
    EXPECT_NE(rendered.find("keeps replies short and dry"), std::string::npos);
    EXPECT_NE(rendered.find("prepared groups and fair loot"), std::string::npos);
    EXPECT_LE(rendered.size(), PLAYERBOT_SOCIAL_CONTEXT_ENTRY_BYTES);
}

TEST(PlayerbotSocialRequestContextTest, TheComposedContextCarriesThePersonaAndKeepsTheStarter)
{
    /*
     * The plan's objective is a personality driven conversation, and the persona is the whole of
     * "personality driven" as far as a generation is concerned. A context carrying only a subject
     * describes what to talk about and says nothing about who is talking.
     */
    PlayerbotSocialMgr coordinator;

    PlayerbotSocialRequestContext const context = coordinator.ComposeRequestContext(
        500, StoredPersonality(), 0, PlayerbotSocialChannel::General, "a rare drop", 1000);

    EXPECT_EQ(context.starter, "a rare drop") << "the subject a starter is about must survive";
    EXPECT_FALSE(context.persona.empty()) << "a bot with no stored row still has a derived persona";
    EXPECT_NE(context.persona.find("speaks "), std::string::npos);
    EXPECT_LE(context.persona.size(), PLAYERBOT_SOCIAL_CONTEXT_ENTRY_BYTES);
}

TEST(PlayerbotSocialRequestContextTest, AWhisperMemoryNeverReachesTheContextBuiltForAZoneLine)
{
    /*
     * The single failure this whole privacy model exists to prevent, asserted where it is decided
     * rather than only at the far side. A bot that learned something in a whisper must not be
     * handed it while it is composing a line for General, because what it is handed is what it may
     * repeat.
     */
    /*
     * Against the state store rather than through the coordinator, because the coordinator's own
     * write path issues a prepared statement and this binary has no database. That is the same
     * harness limitation Task 10B recorded. The selector below is the whole of the producer's
     * privacy decision, so asserting it here asserts the decision itself rather than a wrapper.
     */
    PlayerbotSocialStateStore state;
    state.SetOptedOut(500, false);
    state.SetOptedOut(900, false);

    PlayerbotSocialMemoryRecord confided;
    confided.botGuidCounter = 500;
    confided.subjectGuidCounter = 900;
    confided.scope = PlayerbotSocialPrivacyScope::Whisper;
    confided.confidence = 0.9f;
    confided.significance = 0.9f;
    confided.paraphrase = "is quietly training as a blacksmith";
    confided.sourceEventPublicId = PlayerbotSocialMakeEventPublicId(1, 900);
    confided.sourceThreadPublicId = "thr_00000000000000000000000000000001";
    confided.sourceKind = PlayerbotSocialMemorySourceKind::HumanObservation;
    ASSERT_EQ(state.RememberMemory(confided), PlayerbotSocialMemoryRejection::None);

    PlayerbotSocialMemoryRecord shared;
    shared.botGuidCounter = 500;
    shared.subjectGuidCounter = 900;
    shared.scope = PlayerbotSocialPrivacyScope::Public;
    shared.confidence = 0.9f;
    shared.significance = 0.9f;
    shared.paraphrase = "runs the same dungeon every night";
    shared.sourceEventPublicId = PlayerbotSocialMakeEventPublicId(2, 900);
    shared.sourceThreadPublicId = "thr_00000000000000000000000000000002";
    shared.sourceKind = PlayerbotSocialMemorySourceKind::HumanObservation;
    ASSERT_EQ(state.RememberMemory(shared), PlayerbotSocialMemoryRejection::None);

    std::vector<PlayerbotSocialContextMemory> const general =
        PlayerbotSocialSelectContextMemories(state, {500, 900}, PlayerbotSocialChannel::General);

    for (PlayerbotSocialContextMemory const& memory : general)
    {
        EXPECT_NE(memory.scope, PlayerbotSocialPrivacyScope::Whisper)
            << "a whisper memory reached a zone context: " << memory.text;
        EXPECT_EQ(memory.text.find("blacksmith"), std::string::npos);
    }

    ASSERT_EQ(general.size(), 1u) << "the public memory is still offered";
    EXPECT_NE(general.front().text.find("dungeon"), std::string::npos);

    std::vector<PlayerbotSocialContextMemory> const whisper =
        PlayerbotSocialSelectContextMemories(state, {500, 900}, PlayerbotSocialChannel::Whisper);

    EXPECT_EQ(whisper.size(), 2u) << "the same bot may draw on both in a whisper";
}

TEST(PlayerbotSocialRequestContextTest, TheSelectedMemoriesAreBoundedInCountAndInEntrySize)
{
    /*
     * A bot that has been playing for months accumulates far more than a prompt can carry, and the
     * far side refuses a list over its declared bounds rather than trimming it. An unbounded
     * selection is therefore not a long prompt, it is a dropped context.
     */
    PlayerbotSocialStateStore state;
    state.SetOptedOut(500, false);
    state.SetOptedOut(900, false);

    for (int index = 0; index < static_cast<int>(PLAYERBOT_SOCIAL_CONTEXT_ENTRIES) + 8; ++index)
    {
        PlayerbotSocialMemoryRecord record;
        record.botGuidCounter = 500;
        record.subjectGuidCounter = 900;
        record.scope = PlayerbotSocialPrivacyScope::Public;
        record.confidence = 0.9f;
        record.significance = 0.9f;
        record.paraphrase = "remembers detail number " + std::to_string(index);
        record.sourceEventPublicId = PlayerbotSocialMakeEventPublicId(index + 1, 900);
        record.sourceThreadPublicId = "thr_00000000000000000000000000000001";
        record.sourceKind = PlayerbotSocialMemorySourceKind::HumanObservation;
        ASSERT_EQ(state.RememberMemory(record), PlayerbotSocialMemoryRejection::None);
    }

    std::vector<PlayerbotSocialContextMemory> const selected =
        PlayerbotSocialSelectContextMemories(state, {500, 900}, PlayerbotSocialChannel::General);

    ASSERT_LE(selected.size(), PLAYERBOT_SOCIAL_CONTEXT_ENTRIES);
    EXPECT_FALSE(selected.empty()) << "bounding must not empty the list, only shorten it";
    for (PlayerbotSocialContextMemory const& memory : selected)
        EXPECT_LE(memory.text.size(), PLAYERBOT_SOCIAL_CONTEXT_ENTRY_BYTES);
}

TEST(PlayerbotSocialRequestContextTest, TheRelationshipIsRenderedAndIsEmptyForARoomRatherThanAPerson)
{
    /*
     * A directional relationship is what makes the same bot answer a friend and a stranger
     * differently, so it has to reach the prompt as its own labelled block rather than being folded
     * into the persona text. A broadcast is addressed to nobody, and inventing a relationship with
     * nobody would describe a rapport that does not exist.
     */
    PlayerbotSocialRelationshipValues values;
    values.familiarity = 0.80f;
    values.affinity = 0.60f;
    values.trust = 0.40f;

    std::string const rendered = PlayerbotSocialRenderRelationship(values);

    EXPECT_NE(rendered.find("familiarity"), std::string::npos);
    EXPECT_NE(rendered.find("affinity"), std::string::npos);
    EXPECT_NE(rendered.find("trust"), std::string::npos);
    EXPECT_LE(rendered.size(), PLAYERBOT_SOCIAL_CONTEXT_ENTRY_BYTES);

    PlayerbotSocialMgr coordinator;

    PlayerbotSocialRequestContext const addressed =
        coordinator.ComposeRequestContext(500, StoredPersonality(), 900, PlayerbotSocialChannel::Say, "", 1000);
    EXPECT_FALSE(addressed.relationship.empty()) << "a line for a person carries how the bot feels about them";

    PlayerbotSocialRequestContext const broadcast = coordinator.ComposeRequestContext(
        500, StoredPersonality(), 0, PlayerbotSocialChannel::General, "a rare drop", 1000);
    EXPECT_TRUE(broadcast.relationship.empty()) << "a line for a room has no counterpart to have a rapport with";
}

TEST(PlayerbotSocialRequestContextTest, TheProviderReceivesTheComposedContextAndNotAnEmptyOne)
{
    /*
     * The handoff itself, not the composition. Every other test here calls ComposeRequestContext
     * directly, so a BeginSocialRequest that submitted a default constructed context would pass all
     * of them while the model saw nothing. This is the assertion that fails if the two are ever
     * disconnected.
     */
    PlayerbotSocialMgr coordinator;
    RecordingProvider provider;
    coordinator.SetSocialProvider(&provider);

    PlayerbotSocialDeliveryRejection rejection = PlayerbotSocialDeliveryRejection::None;
    uint64 const token =
        OpenStarterRequest(coordinator, 500, 900, PlayerbotSocialChannel::Say, ThreadId(),
                           PlayerbotSocialRequestPriority::DirectHumanEngagement, 1000, rejection, "a rare drop");

    ASSERT_NE(token, 0u);
    ASSERT_EQ(provider.submittedContexts.size(), 1u);
    ASSERT_EQ(provider.submittedPriorities.size(), 1u);
    EXPECT_EQ(provider.submittedPriorities.front(), PlayerbotSocialRequestPriority::DirectHumanEngagement);

    PlayerbotSocialRequestContext const& submitted = provider.submittedContexts.front();
    PlayerbotSocialRequestContext const expected = coordinator.ComposeRequestContext(
        500, StoredPersonality(), 900, PlayerbotSocialChannel::Say, "a rare drop", 1000);

    EXPECT_EQ(submitted.persona, expected.persona);
    EXPECT_EQ(submitted.relationship, expected.relationship);
    EXPECT_EQ(submitted.starter, "a rare drop");
    EXPECT_FALSE(submitted.persona.empty()) << "the provider was handed a context with no persona in it";
    EXPECT_FALSE(submitted.relationship.empty()) << "a line for a person carries the rapport with them";
}

TEST(PlayerbotSocialRequestContextTest, DirectIdentityQuestionCarriesOnlyTheApprovedAssignedFacts)
{
    PlayerbotSocialMgr coordinator;
    RecordingProvider provider;
    coordinator.SetSocialProvider(&provider);

    PlayerbotSocialPromptLine currentLine;
    currentLine.speakerGuidCounter = 900;
    currentLine.speakerName = "Human";
    currentLine.speakerIsHuman = true;
    currentLine.atUnixSeconds = 1000;
    currentLine.text = "How old are you and where are you from?";

    PlayerbotSocialDeliveryRejection rejection = PlayerbotSocialDeliveryRejection::None;
    uint64 const token = OpenObservedLineRequest(
        coordinator, 42, StoredPersonality(), 900, PlayerbotSocialChannel::Whisper,
        PlayerbotSocialRequestPriority::DirectHumanEngagement, 1000, rejection, 900, true, currentLine);

    ASSERT_NE(token, 0u);
    ASSERT_EQ(provider.submittedContexts.size(), 1u);
    PlayerbotFictionalIdentityPromptContext const& identity = provider.submittedContexts.front().fictionalIdentity;
    EXPECT_EQ(identity.request, PlayerbotFictionalIdentityRequest::AgeAndHomeCountry);
    EXPECT_EQ(identity.age, 36u);
    EXPECT_EQ(identity.homeCountry, "Ireland");
}

TEST(PlayerbotSocialRequestContextTest, IdentityContextIsAbsentOrWithheldOnProtectedPaths)
{
    PlayerbotSocialMgr coordinator;
    RecordingProvider provider;
    coordinator.SetSocialProvider(&provider);

    EXPECT_EQ(
        coordinator
            .ComposeRequestContext(42, StoredPersonality(), 0, PlayerbotSocialChannel::General, "a rare drop", 1000)
            .fictionalIdentity.request,
        PlayerbotFictionalIdentityRequest::None);

    PlayerbotSocialPromptLine currentLine;
    currentLine.speakerGuidCounter = 900;
    currentLine.speakerName = "Human";
    currentLine.speakerIsHuman = true;
    currentLine.atUnixSeconds = 1000;
    currentLine.text = "How old are you and where are you from?";

    PlayerbotSocialDeliveryRejection rejection = PlayerbotSocialDeliveryRejection::None;
    ASSERT_NE(OpenObservedLineRequest(coordinator, 42, StoredPersonality(), 0, PlayerbotSocialChannel::General,
                                      PlayerbotSocialRequestPriority::DirectHumanEngagement, 1000, rejection, 900,
                                      false, currentLine),
              0u);
    ASSERT_EQ(provider.submittedContexts.size(), 1u);
    EXPECT_EQ(provider.submittedContexts.back().fictionalIdentity.request, PlayerbotFictionalIdentityRequest::None);

    PlayerbotPersonalityProfile withheldPersonality = StoredPersonality();
    withheldPersonality.sociability = 0;
    ASSERT_NE(OpenObservedLineRequest(coordinator, 1000, withheldPersonality, 0, PlayerbotSocialChannel::Say,
                                      PlayerbotSocialRequestPriority::DirectHumanEngagement, 1000, rejection, 900, true,
                                      currentLine),
              0u);
    ASSERT_EQ(provider.submittedContexts.size(), 2u);
    PlayerbotFictionalIdentityPromptContext const& withheld = provider.submittedContexts.back().fictionalIdentity;
    EXPECT_EQ(withheld.request, PlayerbotFictionalIdentityRequest::AgeAndHomeCountry);
    EXPECT_FALSE(withheld.age.has_value());
    EXPECT_FALSE(withheld.homeCountry.has_value());
}

TEST(PlayerbotSocialRequestContextTest, StatelessDirectReplyUsesNeutralRelationshipWithoutWideningCountry)
{
    PlayerbotSocialMgr coordinator;
    RecordingProvider provider;
    coordinator.SetSocialProvider(&provider);

    PlayerbotSocialPromptLine currentLine;
    currentLine.speakerGuidCounter = 900;
    currentLine.speakerName = "PrivateHuman";
    currentLine.speakerIsHuman = true;
    currentLine.atUnixSeconds = 1000;
    currentLine.text = "How old are you and where are you from?";

    PlayerbotSocialDeliveryRejection rejection = PlayerbotSocialDeliveryRejection::None;
    PlayerbotPersonalityProfile personality = StoredPersonality();
    personality.sociability = 0;
    personality.fictionalAge = 60;
    ASSERT_NE(OpenObservedLineRequest(coordinator, 1000, personality, 900, PlayerbotSocialChannel::Whisper,
                                      PlayerbotSocialRequestPriority::DirectHumanEngagement, 1000, rejection, 900, true,
                                      currentLine, true),
              0u);

    ASSERT_EQ(provider.submittedContexts.size(), 1u);
    PlayerbotSocialRequestContext const& context = provider.submittedContexts.front();
    EXPECT_TRUE(context.relationship.empty());
    EXPECT_EQ(context.fictionalIdentity.request, PlayerbotFictionalIdentityRequest::AgeAndHomeCountry);
    EXPECT_EQ(context.fictionalIdentity.age, 60u);
    EXPECT_FALSE(context.fictionalIdentity.homeCountry.has_value());
}

TEST(PlayerbotSocialRequestContextTest, PromptLinesAndNearbyNamesAreRenderedWithinTheWireBounds)
{
    PlayerbotSocialPromptContextSnapshot snapshot;
    snapshot.refusal = PlayerbotSocialPromptContextSnapshotRefusal::Accepted;

    for (std::size_t index = 0; index < PLAYERBOT_SOCIAL_CONTEXT_ENTRIES + 4; ++index)
    {
        PlayerbotSocialPromptLine line;
        line.speakerGuidCounter = index + 1;
        line.speakerName = "speaker" + std::to_string(index);
        line.speakerIsHuman = index % 2 == 0;
        line.atUnixSeconds = 1000 + index;
        line.text = std::string(PLAYERBOT_SOCIAL_CONTEXT_ENTRY_BYTES, 'x');
        snapshot.lines.push_back(std::move(line));
    }

    std::vector<std::string> const thread = PlayerbotSocialRenderPromptThread(snapshot);
    ASSERT_LE(thread.size(), PLAYERBOT_SOCIAL_CONTEXT_ENTRIES);
    std::size_t threadBytes = 0;
    for (std::string const& line : thread)
    {
        EXPECT_LE(line.size(), PLAYERBOT_SOCIAL_CONTEXT_ENTRY_BYTES);
        threadBytes += line.size();
    }
    EXPECT_LE(threadBytes, PLAYERBOT_SOCIAL_CONTEXT_BYTES);
    EXPECT_NE(thread.back().find("speaker15:"), std::string::npos) << "the newest conversation turns survive the bound";

    std::vector<std::string> const nearby = PlayerbotSocialRenderNearby(
        {"Deszy", "Barnek", "Deszy", std::string(PLAYERBOT_SOCIAL_CONTEXT_ENTRY_BYTES + 20, 'n')});
    ASSERT_EQ(nearby.size(), 3u) << "nearby identities are bounded and deduplicated";
    EXPECT_EQ(nearby[0], "Deszy");
    EXPECT_EQ(nearby[1], "Barnek");
    EXPECT_LE(nearby[2].size(), PLAYERBOT_SOCIAL_CONTEXT_ENTRY_BYTES);
}

TEST(PlayerbotSocialRequestContextTest, TheProductionRequestCarriesItsThreadAndNearbySnapshot)
{
    PlayerbotSocialMgr coordinator;
    RecordingProvider provider;
    coordinator.SetSocialProvider(&provider);

    PlayerbotSocialObservation observation;
    observation.key.channel = PlayerbotSocialChannel::Say;
    observation.key.scopeId = 7;
    observation.eventPublicId = PlayerbotSocialMakeEventPublicId(1000, 600);
    observation.speakerGuidCounter = 600;
    observation.speakerName = "Barnek";
    observation.speakerIsHuman = false;
    observation.atUnixSeconds = 1000;
    observation.text = "heading to the mine";
    PlayerbotSocialThreadHandle const thread = coordinator.Observe(observation);
    ASSERT_TRUE(thread.valid);

    PlayerbotSocialDeliveryRejection rejection = PlayerbotSocialDeliveryRejection::None;
    PlayerbotSocialPromptLine currentLine;
    currentLine.eventPublicId = observation.eventPublicId;
    currentLine.role = observation.role;
    currentLine.speakerGuidCounter = observation.speakerGuidCounter;
    currentLine.speakerName = observation.speakerName;
    currentLine.atUnixSeconds = observation.atUnixSeconds;
    currentLine.text = observation.text;
    PlayerbotSocialGroundingEnvelope grounding = Grounding();
    grounding.transcriptEventPublicIds = {observation.eventPublicId};
    uint64 const token =
        coordinator.BeginSocialRequest(500, StoredPersonality(), 0, PlayerbotSocialChannel::Say, thread.publicId,
                                       PlayerbotSocialRequestPriority::BotContinuation, 1000, REQUEST_ZONE_ID, "",
                                       rejection, {{0, "Deszy", false}, {0, "Barnek", false}}, 600, false, currentLine,
                                       false, PlayerbotRoleplayPromptMode::Ordinary, grounding);

    ASSERT_NE(token, 0u);
    ASSERT_EQ(provider.submittedContexts.size(), 1u);
    PlayerbotSocialRequestContext const& submitted = provider.submittedContexts.front();
    ASSERT_EQ(submitted.thread.size(), 1u) << "the observed game message must reach the provider exactly once";
    EXPECT_EQ(submitted.thread.front(), "Barnek: heading to the mine");
    EXPECT_EQ(submitted.nearby, (std::vector<std::string>{"Deszy", "Barnek"}));
}

TEST(PlayerbotSocialRequestContextTest, AThreadNeverCrossesItsChannelPrivacyBoundary)
{
    PlayerbotSocialMgr coordinator;

    PlayerbotSocialObservation observation;
    observation.key.channel = PlayerbotSocialChannel::Whisper;
    observation.key.scopeId = 600;
    observation.speakerGuidCounter = 600;
    observation.speakerName = "Barnek";
    observation.speakerIsHuman = false;
    observation.atUnixSeconds = 1000;
    observation.text = "meet me behind the bank";
    PlayerbotSocialThreadHandle const thread = coordinator.Observe(observation);
    ASSERT_TRUE(thread.valid);

    EXPECT_EQ(coordinator
                  .ComposeRequestContext(500, StoredPersonality(), 600, PlayerbotSocialChannel::Whisper, "", 1000,
                                         thread.publicId)
                  .thread,
              (std::vector<std::string>{"Barnek: meet me behind the bank"}));
    EXPECT_TRUE(coordinator
                    .ComposeRequestContext(500, StoredPersonality(), 600, PlayerbotSocialChannel::General, "", 1000,
                                           thread.publicId)
                    .thread.empty())
        << "private thread text must not enter a prompt for a public channel";
}

// Roleplay prompt authority and authorized delivery containment ------------------------------------

namespace
{
PlayerbotSocialPromptLine IdentityProbingLine()
{
    PlayerbotSocialPromptLine line;
    line.speakerGuidCounter = 900;
    line.speakerName = "Deszy";
    line.speakerIsHuman = true;
    line.atUnixSeconds = 1000;
    line.text = "How old are you and where are you from?";
    return line;
}

uint64 OpenModedRequest(PlayerbotSocialMgr& coordinator, uint64 bot, PlayerbotRoleplayPromptMode mode,
                        uint64 now = 1000)
{
    PlayerbotSocialDeliveryRejection rejection = PlayerbotSocialDeliveryRejection::None;
    return OpenObservedLineRequest(coordinator, bot, StoredPersonality(), 900, PlayerbotSocialChannel::Whisper,
                                   PlayerbotSocialRequestPriority::DirectHumanEngagement, now, rejection, 900, true,
                                   IdentityProbingLine(), false, mode);
}
}  // namespace

TEST(PlayerbotSocialRoleplayDeliveryTest, TheComposedContextCarriesTheWorldserverModeAndExpansion)
{
    PlayerbotSocialMgr coordinator;
    RecordingProvider provider;
    coordinator.SetSocialProvider(&provider);

    // The three ordinary modes keep the existing fictional identity behavior for a line that
    // explicitly asks for age and origin.
    ASSERT_NE(OpenModedRequest(coordinator, 500, PlayerbotRoleplayPromptMode::Ordinary), 0u);
    ASSERT_NE(OpenModedRequest(coordinator, 501, PlayerbotRoleplayPromptMode::DeclineRoleplay), 0u);
    ASSERT_NE(OpenModedRequest(coordinator, 502, PlayerbotRoleplayPromptMode::AcknowledgeRoleplay), 0u);

    // The authorized mode omits the ordinary fictional player identity entirely.
    ASSERT_NE(OpenModedRequest(coordinator, 503, PlayerbotRoleplayPromptMode::AuthorizedRoleplay), 0u);

    ASSERT_EQ(provider.submittedContexts.size(), 4u);

    EXPECT_EQ(provider.submittedContexts[0].promptMode, PlayerbotRoleplayPromptMode::Ordinary);
    EXPECT_EQ(provider.submittedContexts[1].promptMode, PlayerbotRoleplayPromptMode::DeclineRoleplay);
    EXPECT_EQ(provider.submittedContexts[2].promptMode, PlayerbotRoleplayPromptMode::AcknowledgeRoleplay);
    EXPECT_EQ(provider.submittedContexts[3].promptMode, PlayerbotRoleplayPromptMode::AuthorizedRoleplay);

    for (PlayerbotSocialRequestContext const& context : provider.submittedContexts)
        EXPECT_EQ(context.activeContentExpansion, PlayerbotSocialActiveContentExpansion());

    for (std::size_t at = 0; at < 3; ++at)
        EXPECT_NE(provider.submittedContexts[at].fictionalIdentity.request, PlayerbotFictionalIdentityRequest::None)
            << "ordinary mode " << at << " must keep the existing identity behavior";

    EXPECT_EQ(provider.submittedContexts[3].fictionalIdentity.request, PlayerbotFictionalIdentityRequest::None);
    EXPECT_FALSE(provider.submittedContexts[3].fictionalIdentity.age.has_value());
    EXPECT_FALSE(provider.submittedContexts[3].fictionalIdentity.homeCountry.has_value());

    coordinator.SetSocialProvider(nullptr);
}

TEST(PlayerbotSocialRoleplayDeliveryTest, AuthorizedDeliveryAllowsRecognizedWrathContent)
{
    std::vector<std::string> const wrathLines = {
        "In Outland we would feast tonight!",
        "I, a proud blood elf, greet thee.",
        "The Sin'dorei remember.",
        "A draenei vision guides me.",
        "Rise, death knight of the tale!",
        "My jewelcrafting shop awaits your visit.",
        "Let me scribe you an inscription of legend.",
        "The Burning Crusade begins anew tonight.",
        "When the Wrath of the Lich King falls upon us...",
        "To Northrend, my companions!",
    };

    uint64 now = 1000;
    for (std::string const& line : wrathLines)
    {
        PlayerbotSocialMgr coordinator;
        RecordingProvider provider;
        coordinator.SetSocialProvider(&provider);

        now += 10;
        uint64 const token =
            OpenModedRequest(coordinator, 500 + now, PlayerbotRoleplayPromptMode::AuthorizedRoleplay, now);
        ASSERT_NE(token, 0u);

        PlayerbotSocialProviderResult result = Message(line, PlayerbotSocialChannel::Whisper);
        result.requestToken = token;
        ASSERT_EQ(coordinator.AcceptSocialResult(result, now * 1000, 3), PlayerbotSocialDeliveryRejection::None);

        PlayerbotSocialDeliveryConditions conditions = AllHold();
        EXPECT_EQ(coordinator.CompleteDelivery(token, conditions), PlayerbotSocialDeliveryRejection::None)
            << "line: " << line;
        coordinator.SetSocialProvider(nullptr);
    }

    // The same expansion content is valid in an ordinary result on a Wrath server.
    PlayerbotSocialMgr coordinator;
    RecordingProvider provider;
    coordinator.SetSocialProvider(&provider);
    uint64 const ordinaryToken = OpenModedRequest(coordinator, 499, PlayerbotRoleplayPromptMode::Ordinary);
    ASSERT_NE(ordinaryToken, 0u);
    PlayerbotSocialProviderResult ordinary = Message("Northrend will open one day", PlayerbotSocialChannel::Whisper);
    ordinary.requestToken = ordinaryToken;
    ASSERT_EQ(coordinator.AcceptSocialResult(ordinary, 5000000, 3), PlayerbotSocialDeliveryRejection::None);
    EXPECT_EQ(coordinator.CompleteDelivery(ordinaryToken, AllHold()), PlayerbotSocialDeliveryRejection::None);

    coordinator.SetSocialProvider(nullptr);
}

TEST(PlayerbotSocialRoleplayDeliveryTest, OrdinaryDeliveryAllowsWrathProgressionLanguage)
{
    std::vector<std::string> const wrathLines = {"yeah for sure, been grinding heroics all week",
                                                 "finally hit 80, time to start grinding tier pieces"};

    uint64 botGuid = 700;
    for (std::string const& line : wrathLines)
    {
        PlayerbotSocialMgr coordinator;
        RecordingProvider provider;
        coordinator.SetSocialProvider(&provider);

        uint64 const token = OpenModedRequest(coordinator, botGuid++, PlayerbotRoleplayPromptMode::Ordinary);
        ASSERT_NE(token, 0u);

        PlayerbotSocialProviderResult result = Message(line, PlayerbotSocialChannel::Whisper);
        result.requestToken = token;
        ASSERT_EQ(coordinator.AcceptSocialResult(result, 1000000, 3), PlayerbotSocialDeliveryRejection::None);

        EXPECT_EQ(coordinator.CompleteDelivery(token, AllHold()), PlayerbotSocialDeliveryRejection::None)
            << "line: " << line;
        coordinator.SetSocialProvider(nullptr);
    }

    PlayerbotSocialMgr coordinator;
    RecordingProvider provider;
    coordinator.SetSocialProvider(&provider);
    uint64 const validToken = OpenModedRequest(coordinator, botGuid, PlayerbotRoleplayPromptMode::Ordinary);
    ASSERT_NE(validToken, 0u);
    PlayerbotSocialProviderResult valid = Message("still questing around Dun Morogh", PlayerbotSocialChannel::Whisper);
    valid.requestToken = validToken;
    ASSERT_EQ(coordinator.AcceptSocialResult(valid, 1000000, 3), PlayerbotSocialDeliveryRejection::None);
    EXPECT_EQ(coordinator.CompleteDelivery(validToken, AllHold()), PlayerbotSocialDeliveryRejection::None);

    coordinator.SetSocialProvider(nullptr);
}

TEST(PlayerbotSocialRoleplayDeliveryTest, CombatSuppressesAuthorizedRoleplayNotOrdinaryDelivery)
{
    PlayerbotSocialMgr coordinator;
    RecordingProvider provider;
    coordinator.SetSocialProvider(&provider);

    uint64 const authorized = OpenModedRequest(coordinator, 500, PlayerbotRoleplayPromptMode::AuthorizedRoleplay);
    ASSERT_NE(authorized, 0u);
    PlayerbotSocialProviderResult tale = Message("gather round for a tale", PlayerbotSocialChannel::Whisper);
    tale.requestToken = authorized;
    ASSERT_EQ(coordinator.AcceptSocialResult(tale, 1000000, 3), PlayerbotSocialDeliveryRejection::None);

    PlayerbotSocialDeliveryConditions inCombat = AllHold();
    inCombat.speakerInCombat = true;
    EXPECT_EQ(coordinator.CompleteDelivery(authorized, inCombat),
              PlayerbotSocialDeliveryRejection::AuthorizedRoleplayInCombat);

    // Ordinary social delivery is untouched by combat.
    uint64 const ordinary = OpenModedRequest(coordinator, 501, PlayerbotRoleplayPromptMode::Ordinary);
    ASSERT_NE(ordinary, 0u);
    PlayerbotSocialProviderResult chat = Message("pulling the next pack", PlayerbotSocialChannel::Whisper);
    chat.requestToken = ordinary;
    ASSERT_EQ(coordinator.AcceptSocialResult(chat, 1000000, 3), PlayerbotSocialDeliveryRejection::None);

    PlayerbotSocialDeliveryConditions ordinaryCombat = AllHold();
    ordinaryCombat.speakerInCombat = true;
    EXPECT_EQ(coordinator.CompleteDelivery(ordinary, ordinaryCombat), PlayerbotSocialDeliveryRejection::None);

    coordinator.SetSocialProvider(nullptr);
}

TEST(PlayerbotSocialRoleplayDeliveryTest, CombatAtRequestTimeNeverOpensAnAuthorizedGeneration)
{
    PlayerbotSocialMgr coordinator;
    RecordingProvider provider;
    coordinator.SetSocialProvider(&provider);

    // A willing enthusiast candidate who is ALREADY fighting cannot receive authorized roleplay:
    // the mode falls back to ordinary at request time, before any generation is asked for.
    PlayerbotSocialActivation activation;
    activation.channel = PlayerbotSocialChannel::Say;
    activation.speakerGuidCounter = 900;
    activation.speakerIsHuman = true;
    activation.threadLastActivityUnixSeconds = 1000;
    activation.relevantHumanMessages = 3;
    activation.nowUnixSeconds = 1000;
    activation.zoneId = REQUEST_ZONE_ID;
    activation.currentLine = IdentityProbingLine();
    activation.currentLine.eventPublicId = PlayerbotSocialMakeEventPublicId(1000, 900);
    ObserveActivationThread(coordinator, activation);

    // Search a GUID and seed whose selection answers with the stored enthusiast affinity when not in combat.
    constexpr uint8 affinity = 100;
    uint64 chosenGuid = 0;
    uint64 chosenSeed = 0;
    for (uint64 guid = 500; guid < 5000 && chosenGuid == 0; ++guid)
    {
        for (uint64 seed = 1; seed < 512; ++seed)
        {
            if (!PlayerbotRoleplayWillingnessPasses(affinity, PlayerbotRoleplayWillingnessRoll(seed, guid)))
                continue;

            PlayerbotSocialMgr probe;
            RecordingProvider accepting;
            probe.SetSocialProvider(&accepting);
            PlayerbotSocialActivation probeActivation = activation;
            ObserveActivationThread(probe, probeActivation);
            PlayerbotSocialActivationCandidate candidate;
            candidate.botGuidCounter = guid;
            candidate.personality = StoredPersonality(affinity);
            candidate.profileLoadState = PlayerbotSocialProfileLoadState::Loaded;
            candidate.grounding = Grounding();
            candidate.grounding.transcriptEventPublicIds = {probeActivation.currentLine.eventPublicId};
            candidate.effectiveDisposition = 90;
            candidate.stance = PlayerbotSocialStance::Engaged;
            candidate.addressedByName = true;
            candidate.participatedInThread = true;
            candidate.contentRelevance = 90;
            probeActivation.candidates.push_back(candidate);
            probeActivation.selectionSeed = seed;
            bool const answers =
                !probe.Activate(probeActivation, PlayerbotSocialDensityProfile::Normal).openedTokens.empty();
            probe.SetSocialProvider(nullptr);
            if (answers)
            {
                chosenGuid = guid;
                chosenSeed = seed;
                break;
            }
        }
    }
    ASSERT_NE(chosenGuid, 0u);

    PlayerbotSocialActivationCandidate fighter;
    fighter.botGuidCounter = chosenGuid;
    fighter.personality = StoredPersonality(affinity);
    fighter.profileLoadState = PlayerbotSocialProfileLoadState::Loaded;
    fighter.grounding = Grounding();
    fighter.grounding.transcriptEventPublicIds = {activation.currentLine.eventPublicId};
    fighter.effectiveDisposition = 90;
    fighter.stance = PlayerbotSocialStance::Engaged;
    fighter.addressedByName = true;
    fighter.participatedInThread = true;
    fighter.contentRelevance = 90;
    fighter.inCombat = true;

    activation.candidates.push_back(fighter);
    activation.selectionSeed = chosenSeed;

    PlayerbotSocialRoleplayDirective directive;
    directive.kind = PlayerbotRoleplayAssessmentKind::RoleplayInvitation;
    directive.roleplayEligible = true;

    PlayerbotSocialActivationResult const result =
        coordinator.Activate(activation, PlayerbotSocialDensityProfile::Normal, directive);

    ASSERT_EQ(result.openedTokens.size(), 1u);
    ASSERT_EQ(result.promptModes.size(), 1u);
    EXPECT_EQ(result.promptModes[0].second, PlayerbotRoleplayPromptMode::Ordinary)
        << "combat at request time suppresses authorization before any generation is asked for";

    ASSERT_EQ(provider.submittedContexts.size(), 1u);
    EXPECT_EQ(provider.submittedContexts[0].promptMode, PlayerbotRoleplayPromptMode::Ordinary);

    coordinator.SetSocialProvider(nullptr);
}

TEST(PlayerbotSocialRoleplayDeliveryTest, TheNewDeliveryRejectionsAreNamedAndValid)
{
    EXPECT_TRUE(PlayerbotSocialDeliveryRejectionIsValid(PlayerbotSocialDeliveryRejection::LockedRoleplayContent));
    EXPECT_TRUE(PlayerbotSocialDeliveryRejectionIsValid(PlayerbotSocialDeliveryRejection::AuthorizedRoleplayInCombat));
    EXPECT_TRUE(PlayerbotSocialDeliveryRejectionIsValid(PlayerbotSocialDeliveryRejection::LockedProgressionContent));
    EXPECT_STREQ(PlayerbotSocialDeliveryRejectionName(PlayerbotSocialDeliveryRejection::LockedRoleplayContent),
                 "locked_roleplay_content");
    EXPECT_STREQ(PlayerbotSocialDeliveryRejectionName(PlayerbotSocialDeliveryRejection::AuthorizedRoleplayInCombat),
                 "authorized_roleplay_in_combat");
    EXPECT_STREQ(PlayerbotSocialDeliveryRejectionName(PlayerbotSocialDeliveryRejection::LockedProgressionContent),
                 "locked_progression_content");
}
