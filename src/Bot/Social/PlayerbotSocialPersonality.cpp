/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#include "PlayerbotSocialPersonality.h"

#include <algorithm>
#include <cctype>
#include <iterator>

#include "Log.h"
#include "VanillaOnlyRules.h"

namespace
{
std::string FoldAscii(std::string_view text)
{
    std::string folded;
    folded.reserve(text.size());
    for (unsigned char const byte : text)
        folded.push_back(static_cast<char>(byte < 0x80 ? std::tolower(byte) : byte));

    return folded;
}

bool IsAsciiWordByte(char value)
{
    unsigned char const byte = static_cast<unsigned char>(value);
    return byte < 0x80 && std::isalnum(byte) != 0;
}

bool ContainsTopic(std::string const& foldedContent, std::string_view topic)
{
    std::string foldedTopic = FoldAscii(topic);
    std::size_t first = foldedTopic.find_first_not_of(" \t\r\n");
    if (first == std::string::npos)
        return false;

    std::size_t const last = foldedTopic.find_last_not_of(" \t\r\n");
    foldedTopic = foldedTopic.substr(first, last - first + 1);

    for (std::size_t at = foldedContent.find(foldedTopic); at != std::string::npos;
         at = foldedContent.find(foldedTopic, at + 1))
    {
        bool const startsAtBoundary = at == 0 || !IsAsciiWordByte(foldedContent[at - 1]);
        std::size_t const after = at + foldedTopic.size();
        bool const endsAtBoundary = after == foldedContent.size() || !IsAsciiWordByte(foldedContent[after]);
        if (startsAtBoundary && endsAtBoundary)
            return true;
    }

    return false;
}

bool AnyTopicMatches(std::string const& foldedContent, std::vector<std::string> const& topics)
{
    return std::any_of(topics.begin(), topics.end(),
                       [&foldedContent](std::string const& topic) { return ContainsTopic(foldedContent, topic); });
}

bool AnyListedTopicMatches(std::string const& foldedContent, std::string_view topics)
{
    std::size_t start = 0;
    while (start <= topics.size())
    {
        std::size_t const separator = topics.find_first_of(",;", start);
        std::size_t const length = separator == std::string_view::npos ? topics.size() - start : separator - start;
        if (ContainsTopic(foldedContent, topics.substr(start, length)))
            return true;

        if (separator == std::string_view::npos)
            break;

        start = separator + 1;
    }

    return false;
}
}  // namespace

namespace
{
uint8 ClampSocialTrait(uint8 value) { return value > PLAYERBOT_SOCIAL_TRAIT_MAX ? PLAYERBOT_SOCIAL_TRAIT_MAX : value; }

// The fallback is seeded from the persistent base profile rather than being a flat neutral
// row, so a bot with no stored social state still sounds like the bot it has always been.
PlayerbotSocialProfile SeedProfileFromBase(PlayerbotPersonalityProfile const& base)
{
    PlayerbotSocialProfile profile;
    profile.version = PLAYERBOT_SOCIAL_PERSONA_VERSION;
    profile.traits.version = PLAYERBOT_SOCIAL_PERSONA_VERSION;
    profile.traits.warmth = ClampSocialTrait(base.sociability);
    profile.traits.talkativeness = ClampSocialTrait(base.sociability);
    profile.traits.curiosity = ClampSocialTrait(base.explorationAffinity);
    profile.biographyState = PlayerbotBiographyState::Absent;
    return profile;
}
}  // namespace

PlayerbotSocialProfileLoad PlayerbotPersonality::LoadSocialProfile(uint64 guidCounter,
                                                                   std::optional<PlayerbotSocialProfile> const& stored,
                                                                   PlayerbotPersonalityProfile const& base)
{
    PlayerbotSocialProfileLoad load;

    if (!stored.has_value())
    {
        load.state = PlayerbotSocialProfileLoadState::AbsentUsingBase;
        load.profile = SeedProfileFromBase(base);
        return load;
    }

    load.storedRowPresent = true;
    load.storedSchemaVersion = stored->version;
    load.storedTraitsVersion = stored->traits.version;
    load.storedBiographyState = stored->biographyState;
    load.storedBiographyVersion = stored->biography.version;

    /*
     * Reject rather than coerce, and check every nested version, not only the outer one.
     * Reinterpreting a row written by a different social version would silently change what a bot
     * believes about itself, and a biography carries its own version for exactly that reason. An
     * unrecognized biography state is rejected on the same grounds: it must never reach the request
     * gating or the persona, where it would be neither generated nor treated as finished.
     *
     * The biography version is checked whatever the state says. Gating it on Ready would have let
     * an unsupported biography through on any other state, and composition copies the biography
     * into the persona unconditionally and only reports the state alongside it, so the text would
     * still have travelled. A row with no biography carries the current version by default, so an
     * ordinary absent row is unaffected.
     */
    PlayerbotSocialProfileRejection rejection = PlayerbotSocialProfileRejection::None;

    if (!PlayerbotSocialPersonaVersionIsSupported(stored->version) ||
        !PlayerbotSocialPersonaVersionIsSupported(stored->traits.version))
    {
        rejection = PlayerbotSocialProfileRejection::UnsupportedVersion;
    }
    else if (!PlayerbotBiographyStateIsValid(stored->biographyState))
    {
        rejection = PlayerbotSocialProfileRejection::UnknownBiographyState;
    }
    else if (!PlayerbotSocialPersonaVersionIsSupported(stored->biography.version))
    {
        rejection = PlayerbotSocialProfileRejection::UnsupportedBiographyVersion;
    }
    /*
     * Ready claims a whole biography was generated and stored, so a row that says Ready while part
     * of the document is missing is refused rather than believed. This is the expensive corruption:
     * ShouldRequestBiography reads only the state, so a partial Ready row suppresses regeneration
     * permanently and the bot keeps a persona with holes in it that nothing will ever fill.
     *
     * Only under Ready. Every other state legitimately has an empty document, so requiring
     * completeness of all of them would refuse the ordinary rows this load exists to accept.
     */
    else if (stored->biographyState == PlayerbotBiographyState::Ready &&
             !PlayerbotBiographyDocumentIsComplete(stored->biography))
    {
        rejection = PlayerbotSocialProfileRejection::IncompleteBiography;
    }

    if (rejection != PlayerbotSocialProfileRejection::None)
    {
        load.state = PlayerbotSocialProfileLoadState::RejectedUsingBase;
        load.rejection = rejection;
        load.profile = SeedProfileFromBase(base);

        // The reason is named rather than left to be inferred from the raw values, and the values
        // are logged alongside it so the version that is not understood is visible.
        LOG_WARN("playerbots",
                 "Social profile for bot {} rejected, using base personality: reason={} "
                 "(profile_version={} traits_version={} biography_state={} biography_version={})",
                 guidCounter, PlayerbotSocialProfileRejectionName(rejection), stored->version, stored->traits.version,
                 static_cast<uint32>(stored->biographyState), stored->biography.version);
        return load;
    }

    load.state = PlayerbotSocialProfileLoadState::Loaded;
    load.profile = *stored;

    // A hand edited or corrupt row must not put an out of range trait into circulation. Scalars are
    // clamped and lists are normalized to the same bound the write path applies, so a row that
    // never went through the add path cannot carry an unbounded or unusable topic list.
    load.profile.traits.warmth = ClampSocialTrait(load.profile.traits.warmth);
    load.profile.traits.talkativeness = ClampSocialTrait(load.profile.traits.talkativeness);
    load.profile.traits.curiosity = ClampSocialTrait(load.profile.traits.curiosity);
    load.profile.traits.humor = ClampSocialTrait(load.profile.traits.humor);
    load.profile.traits.formality = ClampSocialTrait(load.profile.traits.formality);
    load.profile.traits.interests = PlayerbotSocialBoundEvolvingTopics(load.profile.traits.interests);
    load.profile.traits.aversions = PlayerbotSocialBoundEvolvingTopics(load.profile.traits.aversions);
    return load;
}

char const* PlayerbotSocialProfileLoadStateName(PlayerbotSocialProfileLoadState state)
{
    switch (state)
    {
        case PlayerbotSocialProfileLoadState::Pending:
            return "pending";
        case PlayerbotSocialProfileLoadState::Loaded:
            return "loaded";
        case PlayerbotSocialProfileLoadState::AbsentUsingBase:
            return "absent_using_base";
        case PlayerbotSocialProfileLoadState::RejectedUsingBase:
            return "rejected_using_base";
        case PlayerbotSocialProfileLoadState::UnavailableUsingBase:
            return "unavailable_using_base";
    }

    return "unknown";
}

char const* PlayerbotSocialProfileRejectionName(PlayerbotSocialProfileRejection rejection)
{
    switch (rejection)
    {
        case PlayerbotSocialProfileRejection::None:
            return "none";
        case PlayerbotSocialProfileRejection::UnsupportedVersion:
            return "unsupported_version";
        case PlayerbotSocialProfileRejection::UnknownBiographyState:
            return "unknown_biography_state";
        case PlayerbotSocialProfileRejection::UnsupportedBiographyVersion:
            return "unsupported_biography_version";
        case PlayerbotSocialProfileRejection::IncompleteBiography:
            return "incomplete_biography";
        case PlayerbotSocialProfileRejection::MalformedStoredData:
            return "malformed_stored_data";
    }

    return "unknown";
}

bool PlayerbotPersonality::ShouldRequestBiography(PlayerbotSocialProfile const& profile, uint64 nowUnixSeconds)
{
    // Both waiting states measure from the last attempt, but for different reasons: a failure waits
    // out a short retry backoff, while a request in flight waits out the longer window after which
    // it is presumed abandoned.
    uint64 wait = 0;

    switch (profile.biographyState)
    {
        case PlayerbotBiographyState::Absent:
            return true;
        case PlayerbotBiographyState::Ready:
            return false;
        case PlayerbotBiographyState::RetryableFailure:
            wait = PLAYERBOT_SOCIAL_BIOGRAPHY_RETRY_SECONDS;
            break;
        case PlayerbotBiographyState::Pending:
            wait = PLAYERBOT_SOCIAL_BIOGRAPHY_PENDING_TIMEOUT_SECONDS;
            break;
        default:
            // Fail closed on an unrecognized state. It must never become a reason to spend a
            // generation, and the compiler is no backstop in a build without -Wswitch.
            return false;
    }

    // A clock that moved backwards would otherwise read as a very old attempt and let a request
    // straight through.
    if (nowUnixSeconds < profile.biographyAttemptedAtUnixSeconds)
        return false;

    return nowUnixSeconds - profile.biographyAttemptedAtUnixSeconds >= wait;
}

PlayerbotSocialProfile PlayerbotPersonality::MarkBiographyRequested(PlayerbotSocialProfile const& profile,
                                                                    uint64 nowUnixSeconds, uint64 requestToken)
{
    /*
     * A request with no identity is refused rather than recorded. Marking Pending with a zero token
     * would leave the profile in the one state the fencing rule cannot evaluate: every completion
     * would fail the token check, so that bot would never receive a biography while still reading
     * as a request in flight for a full timeout window. Refusing leaves it immediately requestable.
     */
    if (requestToken == 0)
        return profile;

    /*
     * The transition that makes generation one time. The predicate alone cannot enforce it: it
     * stays true for as long as the state says no biography exists, so a caller that only consults
     * it would request one on every opportunity. Gating the transition on the same predicate keeps
     * the two from disagreeing and makes a second call a no op, so a pending request is never
     * silently extended and a finished biography is never downgraded.
     */
    if (!ShouldRequestBiography(profile, nowUnixSeconds))
        return profile;

    PlayerbotSocialProfile requested = profile;
    requested.biographyState = PlayerbotBiographyState::Pending;
    requested.biographyAttemptedAtUnixSeconds = nowUnixSeconds;
    /*
     * Replaced, never accumulated. The profile waits on exactly one request, so a reissue after the
     * timeout must retire the previous token: keeping it would leave the superseded call still
     * acceptable, which is the precise hole the token exists to close.
     */
    requested.biographyRequestToken = requestToken;
    return requested;
}

char const* PlayerbotBiographyCompletionRejectionName(PlayerbotBiographyCompletionRejection rejection)
{
    switch (rejection)
    {
        case PlayerbotBiographyCompletionRejection::None:
            return "none";
        case PlayerbotBiographyCompletionRejection::NotAwaited:
            return "not_awaited";
        case PlayerbotBiographyCompletionRejection::TokenMismatch:
            return "token_mismatch";
        case PlayerbotBiographyCompletionRejection::Invalid:
            return "invalid";
    }

    return "unknown";
}

namespace
{
/*
 * The shared fence. Both completion paths ask the same two questions in the same order, and a
 * caller reaching either one must not be able to answer them differently.
 *
 * Returns None when this profile is genuinely waiting on this request.
 */
PlayerbotBiographyCompletionRejection BiographyCompletionFence(PlayerbotSocialProfile const& profile,
                                                               uint64 requestToken)
{
    /*
     * Not Pending means nothing is outstanding, whatever token the caller carries. A zero token
     * is folded in here rather than named separately: a profile awaiting nothing stores zero, so
     * a zero completion would otherwise "match" every idle profile on the realm.
     */
    if (profile.biographyState != PlayerbotBiographyState::Pending || profile.biographyRequestToken == 0 ||
        requestToken == 0)
        return PlayerbotBiographyCompletionRejection::NotAwaited;

    if (profile.biographyRequestToken != requestToken)
        return PlayerbotBiographyCompletionRejection::TokenMismatch;

    return PlayerbotBiographyCompletionRejection::None;
}

// The state an answered but unusable request leaves behind. The request is retired either way:
// it WAS answered, so leaving the token live would let the same bad answer be replayed.
PlayerbotBiographyCompletion BiographyRetryAfterAnsweredFailure(PlayerbotSocialProfile const& profile,
                                                                PlayerbotBiographyRejection validation)
{
    PlayerbotBiographyCompletion outcome;
    outcome.accepted = false;
    outcome.rejection = PlayerbotBiographyCompletionRejection::Invalid;
    outcome.validation = validation;
    outcome.profile = profile;
    outcome.profile.biographyState = PlayerbotBiographyState::RetryableFailure;
    outcome.profile.biographyRequestToken = 0;
    return outcome;
}
}  // namespace

PlayerbotBiographyCompletion PlayerbotPersonality::ApplyBiographyCompletion(
    PlayerbotSocialProfile const& profile, uint64 requestToken, PlayerbotBiography const& candidate,
    PlayerbotBiographyIdentity const& authoritative)
{
    PlayerbotBiographyCompletionRejection const fence = BiographyCompletionFence(profile, requestToken);
    if (fence != PlayerbotBiographyCompletionRejection::None)
    {
        PlayerbotBiographyCompletion refused;
        refused.rejection = fence;
        refused.profile = profile;
        return refused;
    }

    PlayerbotBiographyValidation const validation = ValidateBiography(candidate, authoritative);
    if (!validation.accepted)
        return BiographyRetryAfterAnsweredFailure(profile, validation.rejection);

    PlayerbotBiographyCompletion accepted;
    accepted.accepted = true;
    accepted.profile = profile;
    accepted.profile.biographyState = PlayerbotBiographyState::Ready;
    accepted.profile.biography = candidate;
    /*
     * Retired on acceptance, so a duplicate of the message just applied is refused by the fence
     * rather than harmlessly reapplied. Reapplying the same bytes happens to be a no op today, but
     * that is idempotence by luck: the moment anything else keys off the transition, a replayable
     * completion becomes a way to fire it twice.
     */
    accepted.profile.biographyRequestToken = 0;
    return accepted;
}

PlayerbotBiographyCompletion PlayerbotPersonality::ApplyBiographyFailure(PlayerbotSocialProfile const& profile,
                                                                         uint64 requestToken)
{
    PlayerbotBiographyCompletionRejection const fence = BiographyCompletionFence(profile, requestToken);
    if (fence != PlayerbotBiographyCompletionRejection::None)
    {
        PlayerbotBiographyCompletion refused;
        refused.rejection = fence;
        refused.profile = profile;
        return refused;
    }

    // No payload arrived, so there is no payload level reason to report.
    return BiographyRetryAfterAnsweredFailure(profile, PlayerbotBiographyRejection::None);
}

uint8 PlayerbotPersonality::SocialContentRelevance(PlayerbotSocialProfile const& profile, std::string_view content)
{
    if (content.empty())
        return PLAYERBOT_SOCIAL_MOOD_NEUTRAL;

    std::string const foldedContent = FoldAscii(content);

    bool averse = AnyTopicMatches(foldedContent, profile.traits.aversions);
    bool interested = AnyTopicMatches(foldedContent, profile.traits.interests);

    if (profile.biographyState == PlayerbotBiographyState::Ready)
    {
        averse = averse || AnyListedTopicMatches(foldedContent, profile.biography.aversions);
        interested = interested || AnyListedTopicMatches(foldedContent, profile.biography.interests) ||
                     AnyListedTopicMatches(foldedContent, profile.biography.preferredTopics);
    }

    if (averse)
        return PLAYERBOT_SOCIAL_TRAIT_MIN;

    return interested ? PLAYERBOT_SOCIAL_TRAIT_MAX : PLAYERBOT_SOCIAL_MOOD_NEUTRAL;
}

PlayerbotSocialProfile PlayerbotPersonality::EvolveSocialProfileAfterIndependentInteraction(
    PlayerbotPersonalityProfile const& base, PlayerbotSocialProfile const& profile, uint64 nowUnixSeconds)
{
    if (profile.traits.talkativeness == base.sociability)
        return profile;

    int32 const direction = profile.traits.talkativeness < base.sociability ? PLAYERBOT_SOCIAL_TRAIT_MAX_STEP
                                                                            : -PLAYERBOT_SOCIAL_TRAIT_MAX_STEP;
    PlayerbotSocialTraitEvolution const evolution = PlayerbotSocialEvolveTraitIfDue(
        profile.traits.talkativeness, direction, profile.traits.lastEvolvedAtUnixSeconds, nowUnixSeconds);

    if (!evolution.applied)
        return profile;

    PlayerbotSocialProfile evolved = profile;
    evolved.traits.talkativeness = evolution.value;
    evolved.traits.lastEvolvedAtUnixSeconds = evolution.lastAcceptedAtUnixSeconds;
    return evolved;
}

PlayerbotEffectiveSocialPersona PlayerbotPersonality::ComposeEffectiveSocialPersona(
    PlayerbotPersonalityProfile const& base, PlayerbotSocialProfile const& profile,
    PlayerbotSocialRelationshipValues const& relationship, PlayerbotSocialPersonaContext const& context)
{
    PlayerbotEffectiveSocialPersona persona;
    persona.personaVersion = PLAYERBOT_SOCIAL_PERSONA_VERSION;

    // Copied through unchanged. Composition reads these layers, it never rewrites them.
    persona.base = base;
    persona.traits = profile.traits;
    persona.biographyState = profile.biographyState;
    persona.biography = profile.biography;

    persona.relationship = PlayerbotSocialClampRelationship(relationship);
    persona.channel = context.channel;
    persona.retrievalScope = PlayerbotSocialChannelPrivacyScope(context.channel);

    // Base sociability decides how much this bot talks at all, the stored warmth decides how it
    // treats people, and the transient context colours the moment without overriding either.
    uint8 const disposition =
        PlayerbotSocialEngagementDisposition(base.sociability, profile.traits.warmth, persona.relationship);
    persona.engagementDisposition =
        PlayerbotSocialApplyContextToDisposition(disposition, context.addressedDirectly, context.mood);
    persona.stance = PlayerbotSocialStanceFor(persona.engagementDisposition, persona.relationship);
    return persona;
}

namespace
{
// The eight fields of the version 1 biography, in the wire spelling the sidecar uses. Each name
// sits next to the member it writes to, in one table rather than two parallel ones: two arrays
// sharing an index can be reordered apart and a length assertion would not notice, while here a
// name and its destination cannot be separated without editing the same line. This is the only
// list of biography fields; the whitelist, the assembler, and the validation all read it.
struct BiographyField
{
    std::string_view name;
    std::string PlayerbotBiography::* member;
};

constexpr BiographyField BIOGRAPHY_FIELDS[] = {{"origin", &PlayerbotBiography::origin},
                                               {"motivation", &PlayerbotBiography::motivation},
                                               {"formative_experience", &PlayerbotBiography::formativeExperience},
                                               {"interests", &PlayerbotBiography::interests},
                                               {"aversions", &PlayerbotBiography::aversions},
                                               {"preferred_topics", &PlayerbotBiography::preferredTopics},
                                               {"mannerisms", &PlayerbotBiography::mannerisms},
                                               {"values", &PlayerbotBiography::values}};

constexpr std::size_t BIOGRAPHY_FIELD_COUNT = std::size(BIOGRAPHY_FIELDS);

// The exported count and the table it describes, checked against each other. A field added to
// the table without updating the constant would let a bridge producer assert agreement with a
// number that no longer describes anything.
static_assert(BIOGRAPHY_FIELD_COUNT == PLAYERBOT_SOCIAL_BIOGRAPHY_FIELD_COUNT,
              "the exported biography field count must match the table");

// One definition of what a known field is, shared by the predicate and the assembler, so the
// whitelist cannot answer one thing to a caller asking about a name and another to the parse
// boundary acting on it. BIOGRAPHY_FIELD_COUNT means no match.
std::size_t FindBiographyFieldIndex(std::string_view name)
{
    for (std::size_t i = 0; i < BIOGRAPHY_FIELD_COUNT; ++i)
    {
        if (BIOGRAPHY_FIELDS[i].name == name)
            return i;
    }

    return BIOGRAPHY_FIELD_COUNT;
}
}  // namespace

bool PlayerbotBiographyDocumentIsComplete(PlayerbotBiography const& biography)
{
    /*
     * Identity is part of the document, not decoration around it. The loader restores all four
     * values from the same stored JSON and composition copies them into the persona
     * unconditionally, so a document whose name did not come back would have the bot introduce
     * itself as nobody. Race and class are 1 based, which makes zero absence rather than a value.
     *
     * Gender is deliberately not checked. GENDER_MALE is 0, so a missing gender and a real one are
     * the same value once loaded, and only the key presence test the query performs can tell them
     * apart. This function answers what can be answered from the values alone.
     */
    if (biography.identity.characterName.empty() || biography.identity.raceId == 0 || biography.identity.classId == 0)
    {
        return false;
    }

    // The table, not a list of names repeated here. A field added to the structure becomes required
    // by this check without anyone remembering to come back for it, which is the whole reason the
    // table exists.
    for (BiographyField const& field : BIOGRAPHY_FIELDS)
    {
        if ((biography.*field.member).empty())
            return false;
    }

    return true;
}

namespace
{

/*
 * Claims a player profile may not make. The Product Contract forbids invented relatives, titles,
 * achievements, relationships, and shared history, so all five categories are covered here and
 * not just the famous relative case. Matched as whole words against the lowercased field.
 *
 * The list is deliberately strict. A rejection only schedules a regeneration, so a false
 * positive costs one retry, while a false negative puts an invented claim into chat. The
 * accepting fixtures in the tests are what keep the strictness from rejecting every ordinary
 * compact player profile.
 */
// SHORTCUT: curated denylist, upgrade to a classifier when live telemetry shows a forbidden
// claim reaching chat that this list does not name. It is one deterministic layer. The
// constrained generation prompt and the bounded structure are the other two.
constexpr std::string_view FORBIDDEN_CLAIM_TERMS[] = {
    // Kinship, by relation and by degree.
    "son of", "daughter of", "brother of", "sister of", "child of", "heir of", "heir to", "descendant of",
    "descended from", "cousin of", "nephew of", "niece of", "grandson of", "granddaughter of", "married to", "widow of",
    "widower of", "betrothed to",
    // Invented relationships with other characters.
    "friend of", "friends with", "apprentice of", "apprenticed to", "student of", "mentor of", "mentored by",
    "trained by", "rival of", "sworn to", "served under", "squire to", "companion of", "ally of",
    // Shared history: having been somewhere or done something alongside someone.
    "fought alongside", "fought beside", "fought with", "rode with", "marched with", "personally met", "once met",
    "grew up with", "survived together", "witnessed the fall",
    // Titles and ranks.
    "highlord", "high lord", "warchief", "archmage", "lich king", "lord commander", "grand marshal", "grand admiral",
    "high priestess", "chieftain", "prince", "princess", "king of", "queen of", "lord of", "lady of", "captain of",
    "master of",
    // Personal achievements and renown.
    "hero of", "champion of", "slayer of", "single-handedly", "singlehandedly", "legendary", "vanquished",
    "saved azeroth", "renowned", "renown", "famed", "famous",
    // Named figures. Places are deliberately absent: being from Lordaeron is an ordinary origin.
    "arthas", "thrall", "jaina", "sylvanas", "illidan", "muradin"};

std::string ToLowerAscii(std::string const& text)
{
    std::string lowered = text;
    std::transform(lowered.begin(), lowered.end(), lowered.begin(),
                   [](unsigned char character) { return static_cast<char>(std::tolower(character)); });
    return lowered;
}

bool IsWordCharacter(char character)
{
    unsigned char const value = static_cast<unsigned char>(character);
    return std::isalnum(value) != 0 || character == '_';
}

/*
 * Whole word search. Without the boundary checks a term like "king of" would still be safe, but
 * "prince" would reject an ordinary sentence containing "princely", and "legendary" style terms
 * would fire inside longer words. A term may itself contain a space, which is why the boundary
 * is tested on the characters around the whole match rather than on each word.
 */
bool ContainsWholeWord(std::string const& haystack, std::string_view needle)
{
    // An empty needle is found at every position, so guard it rather than let a stray entry in
    // the list above reject every biography.
    if (needle.empty())
        return false;

    std::string::size_type position = haystack.find(needle);
    while (position != std::string::npos)
    {
        bool const startIsBoundary = position == 0 || !IsWordCharacter(haystack[position - 1]);
        std::string::size_type const end = position + needle.size();
        bool const endIsBoundary = end >= haystack.size() || !IsWordCharacter(haystack[end]);

        if (startIsBoundary && endIsBoundary)
            return true;

        position = haystack.find(needle, position + 1);
    }

    return false;
}

bool ContainsForbiddenClaim(std::string const& field)
{
    std::string const lowered = ToLowerAscii(field);
    for (std::string_view term : FORBIDDEN_CLAIM_TERMS)
    {
        if (ContainsWholeWord(lowered, term))
            return true;
    }

    return false;
}

PlayerbotBiographyValidation Reject(PlayerbotBiographyRejection rejection)
{
    PlayerbotBiographyValidation validation;
    validation.accepted = false;
    validation.rejection = rejection;
    return validation;
}
}  // namespace

bool PlayerbotBiographyFieldIsKnown(std::string_view name)
{
    return FindBiographyFieldIndex(name) != BIOGRAPHY_FIELD_COUNT;
}

char const* PlayerbotBiographyRejectionName(PlayerbotBiographyRejection rejection)
{
    switch (rejection)
    {
        case PlayerbotBiographyRejection::None:
            return "none";
        case PlayerbotBiographyRejection::UnsupportedVersion:
            return "unsupported_version";
        case PlayerbotBiographyRejection::IdentityMismatch:
            return "identity_mismatch";
        case PlayerbotBiographyRejection::MissingRequiredField:
            return "missing_required_field";
        case PlayerbotBiographyRejection::FieldTooLong:
            return "field_too_long";
        case PlayerbotBiographyRejection::ForbiddenClaim:
            return "forbidden_claim";
        case PlayerbotBiographyRejection::UnknownField:
            return "unknown_field";
        case PlayerbotBiographyRejection::DuplicateField:
            return "duplicate_field";
        case PlayerbotBiographyRejection::LockedProgressionContent:
            return "locked_progression_content";
    }

    return "unknown";
}

PlayerbotBiographyValidation PlayerbotPersonality::ValidateBiography(PlayerbotBiography const& candidate,
                                                                     PlayerbotBiographyIdentity const& authoritative)
{
    if (!PlayerbotSocialPersonaVersionIsSupported(candidate.version))
        return Reject(PlayerbotBiographyRejection::UnsupportedVersion);

    // Identity is not the model's to choose. Every fact here comes from the character record.
    if (candidate.identity.characterName != authoritative.characterName ||
        candidate.identity.raceId != authoritative.raceId || candidate.identity.classId != authoritative.classId ||
        candidate.identity.genderId != authoritative.genderId)
    {
        return Reject(PlayerbotBiographyRejection::IdentityMismatch);
    }

    // The same table the whitelist and the assembler read, not a third hand maintained list. A
    // field added to the structure is required, bounded, and scanned here without anyone
    // remembering to add it in a second place.
    for (BiographyField const& field : BIOGRAPHY_FIELDS)
    {
        std::string const& value = candidate.*field.member;

        if (value.empty())
            return Reject(PlayerbotBiographyRejection::MissingRequiredField);

        if (value.size() > PLAYERBOT_SOCIAL_BIOGRAPHY_MAX_FIELD_LENGTH)
            return Reject(PlayerbotBiographyRejection::FieldTooLong);
    }

    // Bounds first for every field, then content, so an oversized field is reported as oversized
    // even when it also carries a forbidden claim.
    for (BiographyField const& field : BIOGRAPHY_FIELDS)
    {
        std::string const& value = candidate.*field.member;
        if (ContainsForbiddenClaim(value))
            return Reject(PlayerbotBiographyRejection::ForbiddenClaim);

        for (VanillaOnlyRules::RoleplayContentCapability const capability :
             VanillaOnlyRules::DetectRoleplayContentCapabilities(value))
        {
            if (!VanillaOnlyRules::IsRoleplayContentAllowed(capability))
                return Reject(PlayerbotBiographyRejection::LockedProgressionContent);
        }
    }

    PlayerbotBiographyValidation validation;
    validation.accepted = true;
    validation.rejection = PlayerbotBiographyRejection::None;
    return validation;
}

PlayerbotBiographyAssembly PlayerbotPersonality::AssembleBiography(
    std::vector<PlayerbotBiographyFieldValue> const& fields, uint32 version,
    PlayerbotBiographyIdentity const& authoritative)
{
    // The candidate is built locally and only handed over once it is accepted, so every refusal
    // returns the same empty biography and no rejection path can leak a half filled one.
    PlayerbotBiography candidate;
    candidate.version = version;

    // Identity is copied in rather than read out of the payload, which is why naming an identity
    // field is an unknown field below and never a mismatch.
    candidate.identity = authoritative;

    bool assigned[BIOGRAPHY_FIELD_COUNT] = {};

    PlayerbotBiographyAssembly assembly;

    for (PlayerbotBiographyFieldValue const& field : fields)
    {
        std::size_t const index = FindBiographyFieldIndex(field.name);

        if (index == BIOGRAPHY_FIELD_COUNT)
        {
            assembly.rejection = PlayerbotBiographyRejection::UnknownField;
            return assembly;
        }

        if (assigned[index])
        {
            assembly.rejection = PlayerbotBiographyRejection::DuplicateField;
            return assembly;
        }

        assigned[index] = true;
        candidate.*BIOGRAPHY_FIELDS[index].member = field.value;
    }

    // A name that never appeared leaves its member empty, which the content validation already
    // treats as a missing required field. There is no second rule about absence here.
    PlayerbotBiographyValidation const validation = ValidateBiography(candidate, authoritative);
    assembly.accepted = validation.accepted;
    assembly.rejection = validation.rejection;

    if (assembly.accepted)
        assembly.biography = candidate;

    return assembly;
}
