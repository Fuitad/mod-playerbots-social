/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef MOD_PLAYERBOTS_PLAYERBOT_SOCIAL_REPOSITORY_H
#define MOD_PLAYERBOTS_PLAYERBOT_SOCIAL_REPOSITORY_H

#include <cstddef>
#include <functional>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "Bot/Social/PlayerbotSocialTypes.h"

/*
 * Durable social state, split so the rules are testable without a database.
 *
 * Everything in this header is pure value logic over GUID counters. The MySQL binding layer
 * consults it and then persists what it decides; it never re-implements a rule here. That split
 * is what lets the privacy, clamping, and directionality guarantees be proven by unit tests
 * rather than only by an integration run against a live schema.
 */

/*
 * MySQL reports JSON_UNQUOTE numeric extractions as text even when the JSON value is a number.
 * Parse that storage representation explicitly instead of asking Field::Get<uint64>() to interpret
 * bytes whose prepared-result metadata says string.
 */
[[nodiscard]] std::optional<uint64> PlayerbotSocialParseStoredUnsigned(std::string_view text);

/*
 * One bot's opinion of one character.
 *
 * The pair is ORDERED and the order is meaningful: `botGuidCounter` is whose opinion this is and
 * `subjectGuidCounter` is who it is about. Swapping them names a different relationship, not the
 * same one from the other side. Nothing in this module ever makes the reverse follow from the
 * forward, because a bot trusting someone is not evidence that they trust the bot.
 */
struct PlayerbotSocialRelationshipKey
{
    uint64 botGuidCounter = 0;
    uint64 subjectGuidCounter = 0;

    [[nodiscard]] bool operator==(PlayerbotSocialRelationshipKey const& other) const = default;
    [[nodiscard]] bool operator<(PlayerbotSocialRelationshipKey const& other) const
    {
        return std::make_pair(botGuidCounter, subjectGuidCounter) <
               std::make_pair(other.botGuidCounter, other.subjectGuidCounter);
    }
};

// Selects a stored pair by something other than its key. See ForgetPairMatching for why the one
// caller cannot supply a key.
using PlayerbotSocialRelationshipMatch = std::function<bool(PlayerbotSocialRelationshipKey const&)>;

/*
 * The in-memory core of relationship storage.
 *
 * A pair that was never written reads as the neutral stranger baseline rather than as an error,
 * so a first meeting needs no special case at any call site. Every write clamps before binding,
 * which is the point at which `PlayerbotSocialClampRelationship` stops being merely available and
 * starts being enforced: a caller cannot reach stored state with an out of range or NaN value.
 */
// One warm pair, as WarmRelationships answers it: the key plus the values that qualified it.
struct PlayerbotSocialWarmRelationship
{
    PlayerbotSocialRelationshipKey key;
    PlayerbotSocialRelationshipValues values;
};

class PlayerbotSocialRelationshipStore
{
public:
    void Remember(PlayerbotSocialRelationshipKey const& key, PlayerbotSocialRelationshipValues const& values);

    [[nodiscard]] PlayerbotSocialRelationshipValues Recall(PlayerbotSocialRelationshipKey const& key) const;

    // Pairs whose familiarity is at or above the floor, up to the limit. The whisper starter pump
    // reads this to find who a bot knows well enough to check in on.
    [[nodiscard]] std::vector<PlayerbotSocialWarmRelationship> WarmRelationships(float minFamiliarity,
                                                                                 std::size_t limit) const;

    [[nodiscard]] std::size_t TrackedRelationshipCount() const;

    // Erases every pair this character appears in, on either side. Returns how many were removed.
    std::size_t Forget(uint64 characterGuidCounter);

    /*
     * Erases the first pair the predicate accepts, and reports whether one was found.
     *
     * A predicate rather than a key, because the caller that needs this identifies a pair by its
     * opaque public id, which is a hash of the pair and cannot be inverted. Resolving the stored
     * actor ids back to guid counters is not an alternative: a cohort purge drops the actor mapping
     * for a deleted bot while deliberately keeping what surviving bots knew ABOUT it, so exactly the
     * pairs most in need of a targeted erase are the ones whose subject no longer resolves.
     *
     * First match only. A public id names one row, and stopping there bounds the scan.
     */
    bool ForgetPairMatching(PlayerbotSocialRelationshipMatch const& matches);

    // Erases only the pairs these characters OWN, leaving pairs where they are merely the subject.
    // One pass over the pairs, so a cohort of thousands does not mean thousands of passes.
    std::size_t ForgetOwnedByAnyOf(std::set<uint64> const& ownerGuidCounters);

private:
    std::map<PlayerbotSocialRelationshipKey, PlayerbotSocialRelationshipValues> _relationships;
};

// Memory candidates ---------------------------------------------------------------------------

/*
 * Which stored privacy scopes a channel is allowed to read.
 *
 * The database has one prepared statement per value here, each with its scope list written as a
 * literal rather than as a parameter. Choosing between statements rather than binding a scope means
 * a caller mistake selects the wrong statement, which is still a legal narrower or equal read, and
 * cannot construct a statement that reads a scope no channel is entitled to.
 */
enum class PlayerbotSocialMemoryScopeQuery : uint8
{
    PublicOnly = 0,
    PublicAndParty,
    Any
};

/*
 * Mirrors `playerbot_social_event.origin`. Values and order follow the schema's ENUM exactly.
 *
 * The origin is what the Social feed groups by, so it is the difference between a line the feature
 * generated and one the legacy functional path produced. Getting it wrong does not fail a write, it
 * files a message under the wrong heading forever.
 */
enum class PlayerbotSocialEventOrigin : uint8
{
    Social = 0,
    CombatStatus,
    PartyStatus,
    Legacy,
    Assistance,
    Pvp,
    Control,
    System
};

inline constexpr std::size_t PLAYERBOT_SOCIAL_EVENT_ORIGIN_COUNT = 8;

// Mirrors `playerbot_social_event.outcome`. What happened, not why: the reason is its own column.
enum class PlayerbotSocialEventOutcome : uint8
{
    Delivered = 0,
    Suppressed,
    Failed,
    Recorded
};

inline constexpr std::size_t PLAYERBOT_SOCIAL_EVENT_OUTCOME_COUNT = 4;

/*
 * Mirrors `playerbot_social_memory.category`. Values and order follow the schema's ENUM exactly, so
 * the numeric value can be mapped to the stored string without a second source of truth.
 */
enum class PlayerbotSocialMemoryCategory : uint8
{
    Fact = 0,
    Impression,
    Interaction,
    Event
};

// Mirrors `playerbot_social_memory.provenance`. How the bot came to know this, not what it knows.
enum class PlayerbotSocialMemoryProvenance : uint8
{
    Participated = 0,
    Addressed,
    Hearsay,
    Assistance,
    Pvp
};

[[nodiscard]] inline constexpr bool PlayerbotSocialMemoryCategoryIsValid(PlayerbotSocialMemoryCategory category)
{
    switch (category)
    {
        case PlayerbotSocialMemoryCategory::Fact:
        case PlayerbotSocialMemoryCategory::Impression:
        case PlayerbotSocialMemoryCategory::Interaction:
        case PlayerbotSocialMemoryCategory::Event:
            return true;
    }

    return false;
}

[[nodiscard]] inline constexpr bool PlayerbotSocialMemoryProvenanceIsValid(PlayerbotSocialMemoryProvenance provenance)
{
    switch (provenance)
    {
        case PlayerbotSocialMemoryProvenance::Participated:
        case PlayerbotSocialMemoryProvenance::Addressed:
        case PlayerbotSocialMemoryProvenance::Hearsay:
        case PlayerbotSocialMemoryProvenance::Assistance:
        case PlayerbotSocialMemoryProvenance::Pvp:
            return true;
    }

    return false;
}

// Matches `playerbot_social_memory.content VARCHAR(512)`. A longer paraphrase would be silently
// truncated by MySQL, which is how a sentence can change meaning between validation and storage.
inline constexpr std::size_t PLAYERBOT_SOCIAL_MAX_MEMORY_CONTENT_LENGTH = 512;

/*
 * The schema declares these as CHECK constraints, which MySQL enforces only from 8.0.16 while
 * AzerothCore's floor is 8.0.0. On an older server the constraint parses and does nothing, so the
 * bound has to hold here to hold at all. Same reasoning as the relationship clamp.
 */
inline constexpr float PLAYERBOT_SOCIAL_CONFIDENCE_MIN = 0.0f;
inline constexpr float PLAYERBOT_SOCIAL_CONFIDENCE_MAX = 1.0f;
inline constexpr float PLAYERBOT_SOCIAL_SIGNIFICANCE_MIN = 0.0f;
inline constexpr float PLAYERBOT_SOCIAL_SIGNIFICANCE_MAX = 1.0f;

/*
 * One paraphrased fact a bot knows about a character, tagged with how privately it was learned.
 *
 * `scope` defaults to the most private value rather than the least. A record that was default
 * constructed, or built by a future call site that forgets to set it, is then unreachable from
 * General and say instead of being broadcast there.
 *
 * `subjectGuidCounter` is optional and zero means the memory is about no one in particular, which
 * is why the schema allows a NULL subject.
 */
struct PlayerbotSocialMemoryRecord
{
    // The durable row's own identity, carried so a recalled memory can be named in telemetry. Empty
    // only for a candidate that has not been through the write path yet, which is why nothing keys
    // behaviour on it: it identifies a memory, it never decides whether one may be used.
    std::string publicId;
    uint64 botGuidCounter = 0;
    uint64 subjectGuidCounter = 0;
    // Manager-lifetime identity for one pending write. Zero means the record came from durable storage.
    uint64 writeToken = 0;
    PlayerbotSocialMemoryCategory category = PlayerbotSocialMemoryCategory::Fact;
    PlayerbotSocialMemoryProvenance provenance = PlayerbotSocialMemoryProvenance::Participated;
    PlayerbotSocialPrivacyScope scope = PlayerbotSocialPrivacyScope::Whisper;
    float confidence = PLAYERBOT_SOCIAL_CONFIDENCE_MIN;
    float significance = PLAYERBOT_SOCIAL_SIGNIFICANCE_MIN;
    std::string paraphrase;
    std::string sourceEventPublicId;
    std::string sourceThreadPublicId;
    std::optional<PlayerbotSocialMemorySourceKind> sourceKind;
};

/*
 * Whether the reader that produced a snapshot was able to evaluate a record at all.
 *
 * A reader skips a stored row whose subject it cannot resolve to a character, or whose consent
 * answer it has not been told. That skip is a fail closed decision about one record, not a statement
 * that the record is gone, so it has to bound the replacement as well as the acceptance. Expressed as
 * a predicate rather than as a list of subjects because a skipped row can be one whose subject the
 * reader could not name in the first place.
 */
using PlayerbotSocialMemoryVisibility = std::function<bool(PlayerbotSocialMemoryRecord const&)>;

/*
 * Names ONE stored memory. Same shape as the visibility predicate above and deliberately a separate
 * alias, because the two answer different questions: visibility asks what a reader is allowed to
 * see, this asks which single record a caller means.
 */
using PlayerbotSocialMemoryMatch = std::function<bool(PlayerbotSocialMemoryRecord const&)>;

/*
 * Why a proposed memory was refused. Exactly one reason is reported, so a rejection can be counted
 * and logged by name without the refused text ever being copied anywhere.
 */
enum class PlayerbotSocialMemoryRejection : uint8
{
    None = 0,
    MissingOwner,
    UnknownCategory,
    UnknownProvenance,
    UnknownPrivacyScope,
    EmptyContent,
    ContentTooLong,
    ConfidenceOutOfRange,
    SignificanceOutOfRange,
    SensitiveContent,
    InstructionLikeContent,
    MissingSourceEvent,
    MalformedSourceEvent,
    MissingSourceThread,
    MalformedSourceThread,
    UnknownSourceKind,
    GeneratedSource,

    // Not a property of the candidate itself: the bot or the character it is about has opted out, so
    // no durable write about them is permitted regardless of how well formed the candidate is.
    CharacterOptedOut,

    // Also not a property of the candidate: a character in it has no durable actor row yet, so there
    // is no key to write against. Transient, and the next attempt after the row resolves succeeds.
    UnresolvedActor
};

inline constexpr std::size_t PLAYERBOT_SOCIAL_MEMORY_REJECTION_COUNT = 19;

[[nodiscard]] char const* PlayerbotSocialMemoryRejectionName(PlayerbotSocialMemoryRejection rejection);

/*
 * Whether a piece of chat carries something no provider should see and no memory should hold.
 *
 * Exposed rather than kept private to the validator below because the same judgement is needed in
 * BOTH directions: on the way out, so a credential a player typed is never submitted, and on the way
 * back, so one that arrives anyway is never stored. Two copies of the marker list would drift, and
 * the copy that drifted would be the one that stopped catching things.
 *
 * SHORTCUT: shared fixed lowercase marker lists, the same ones the memory validator has always used.
 * Replace both callers with the shared safety validator once Task 10 exposes one this layer can call.
 */
[[nodiscard]] bool PlayerbotSocialTextLooksSensitive(std::string const& text);

// Text shaped like an instruction to the model rather than something a character said. Stored
// memories are replayed into later prompts, so one that gets through is repeated indefinitely.
[[nodiscard]] bool PlayerbotSocialTextLooksLikeAnInstruction(std::string const& text);

/*
 * Validates a proposed memory before anything is persisted. Returns the first failing reason so a
 * rejection always names one cause, matching how capture and opportunity report themselves.
 *
 * The content checks are a last-resort backstop, not the primary safety layer: extraction output is
 * validated on the generation side first (Task 10), and this gate exists so a compromised or skipped
 * provider still cannot write a credential or an injected instruction into durable state.
 *
 * SHORTCUT: fixed lowercase marker lists for sensitive and instruction-like content. Replace with
 * the shared safety validator once Task 10 exposes one this layer can call.
 */
[[nodiscard]] PlayerbotSocialMemoryRejection PlayerbotSocialValidateMemoryCandidate(
    PlayerbotSocialMemoryRecord const& record);

/*
 * Privacy filtered memory retrieval.
 *
 * The filter lives on the READ path, not only on the write path, because the same stored fact is
 * legal in one channel and illegal in another. A caller asks for what this bot may say about this
 * character in this channel, and cannot get a broader answer by asking a different way.
 */
class PlayerbotSocialMemoryStore
{
public:
    /*
     * Stores the candidate only if it validates. Returns the reason it was refused so the caller can
     * count and log it; a refused candidate leaves no trace in the store, which is what keeps a
     * rejected secret from being retained anywhere.
     */
    PlayerbotSocialMemoryRejection Remember(PlayerbotSocialMemoryRecord const& record);

    [[nodiscard]] std::vector<PlayerbotSocialMemoryRecord> Recall(PlayerbotSocialRelationshipKey const& key,
                                                                  PlayerbotSocialChannel channel) const;

    [[nodiscard]] std::size_t StoredMemoryCount() const;

    // Erases every memory this character owns or is the subject of. Returns how many were removed.
    std::size_t Forget(uint64 characterGuidCounter);

    // Erases only the memories these characters OWN, leaving memories that are merely about them.
    std::size_t ForgetOwnedByAnyOf(std::set<uint64> const& ownerGuidCounters);

    /*
     * Erases the FIRST record the predicate accepts and reports whether one was found. Deliberately
     * singular: its caller is a durable write that failed, one statement is one row, and a predicate
     * that happens to match several must not turn that into a purge of memories whose own writes
     * succeeded.
     */
    bool ForgetMatching(PlayerbotSocialMemoryMatch const& matches);

    /*
     * The same, narrowed twice: to the scopes a query covers, and to the records `visibleToReader`
     * accepts. The scope bound is what lets a scope restricted read be applied as a replacement
     * without disturbing the scopes it did not ask about. The visibility bound is the same idea for
     * the subject: a reader that could not evaluate a record has no opinion about it, so removing it
     * would forget a fact on the strength of a read that never saw it.
     */
    std::size_t ForgetOwnedByAnyOfWithinScope(std::set<uint64> const& ownerGuidCounters,
                                              PlayerbotSocialMemoryScopeQuery query,
                                              PlayerbotSocialMemoryVisibility const& visibleToReader);

private:
    std::vector<PlayerbotSocialMemoryRecord> _memories;
};

// Consent and reset ---------------------------------------------------------------------------

/*
 * The consent aware face of durable social state, and the only one production wires.
 *
 * The two stores above are the value layer: they enforce directionality, clamping, privacy, and
 * candidate validation, but they know nothing about who agreed to take part. Composing them here
 * means a single object owns the opt out registry, so the relationship side and the memory side
 * cannot disagree about whether a character participates, and a caller cannot reach one of them
 * through a path that skips the check.
 *
 * Opting out suppresses reads and writes; it does not delete. Deleting is what reset does, which is
 * why the two are separate commands. A character who opts out and back in finds what they left.
 */
class PlayerbotSocialStateStore
{
public:
    void SetOptedOut(uint64 characterGuidCounter, bool optedOut);

    [[nodiscard]] bool IsOptedOut(uint64 characterGuidCounter) const;

    /*
     * Writes are refused, not clamped to neutral, when either end of the pair has opted out. The
     * bot's own consent counts as well as the subject's, so a bot excluded from the feature stops
     * forming opinions rather than forming them invisibly.
     */
    bool RememberRelationship(PlayerbotSocialRelationshipKey const& key,
                              PlayerbotSocialRelationshipValues const& values);

    [[nodiscard]] PlayerbotSocialRelationshipValues RecallRelationship(PlayerbotSocialRelationshipKey const& key) const;

    // Warm pairs both of whose ends still participate; an opted-out end drops the pair from the
    // answer exactly as it blocks a recall. The manager's own fail-closed consent check still runs
    // before anything durable happens with an answer from here.
    [[nodiscard]] std::vector<PlayerbotSocialWarmRelationship> WarmRelationships(float minFamiliarity,
                                                                                 std::size_t limit) const;

    PlayerbotSocialMemoryRejection RememberMemory(PlayerbotSocialMemoryRecord const& record);

    /*
     * Replaces what one bot remembers AT THE SCOPES `query` covers with a freshly read snapshot, and
     * returns how many of the supplied records were accepted.
     *
     * Replacement rather than insertion, because a snapshot read is the whole answer for the scopes
     * it covers rather than an addition to them. Remembering each record instead would append the
     * same facts again on every refresh and grow the bot's memory without bound.
     *
     * Scoped to the query rather than to the whole bot, because the read itself was scoped: a
     * General conversation reads only public memory, and letting that replace everything would
     * delete the party and whisper memories it never asked about. An empty snapshot is still a real
     * answer and clears that scope, which is what a reset looks like from here.
     *
     * Only the named bot's own memories are touched. What other bots remember about it is theirs,
     * matching the ownership asymmetry ForgetBotCohort follows.
     *
     * `visibleToReader` reports whether the reader was able to evaluate a record at all, and bounds
     * BOTH halves of the replacement. A reader skips a row whose subject it cannot resolve or whose
     * consent it has not been told, and a record it skipped is neither removed nor accepted here.
     * Passing a predicate that always answers true is correct only for a reader that genuinely saw
     * everything the bot owns in scope.
     */
    std::size_t ReplaceMemoriesOwnedBy(uint64 botGuidCounter, PlayerbotSocialMemoryScopeQuery query,
                                       std::vector<PlayerbotSocialMemoryRecord> const& records,
                                       PlayerbotSocialMemoryVisibility const& visibleToReader);

    [[nodiscard]] std::vector<PlayerbotSocialMemoryRecord> RecallMemories(PlayerbotSocialRelationshipKey const& key,
                                                                          PlayerbotSocialChannel channel) const;

    /*
     * Erases every relationship and memory in which this character appears, as the owner or as the
     * subject, across all bots. Consent itself is deliberately left alone: a reset is not a
     * statement about whether the character wants to keep taking part.
     */
    std::size_t ResetCharacter(uint64 characterGuidCounter);

    /*
     * Erases the cached relationship the predicate accepts, and reports whether one was held.
     *
     * Narrower than ResetCharacter on purpose. An operator deleting a relationship names one stored
     * ROW, and a row is one direction of one pair: erasing the character's other pairs would delete
     * state the operator did not name. Consent and memories are untouched for the same reason.
     */
    bool ForgetRelationshipPairMatching(PlayerbotSocialRelationshipMatch const& matches);

    /*
     * Drops one cached memory, for the case where the record was accepted here and then refused by
     * the database. Singular for the same reason as the pair erase above: the caller is undoing one
     * statement, not reconciling the cache.
     */
    bool ForgetMemoryMatching(PlayerbotSocialMemoryMatch const& matches);

    /*
     * Erases the state OWNED by a cohort of deleted bots, and nothing else.
     *
     * The asymmetry with ResetCharacter is deliberate and is what Definition of Done 6 asks for. A
     * character reset erases the character from both sides, because they asked to be forgotten. A
     * cohort purge follows bot deletion, so it removes what those bots knew and leaves what other
     * bots knew about them: those rows belong to bots outside the cohort, and the criterion is that
     * only the supplied cohort loses bot owned state.
     *
     * That leaves a surviving memory pointing at an actor row that is gone. It resolves to nothing
     * rather than to the wrong character, because the actor key is AUTO_INCREMENT and MySQL 8.0
     * persists the counter across restarts, so a deleted id is never handed out again.
     */
    std::size_t ForgetBotCohort(std::vector<uint64> const& botGuidCounters);

    [[nodiscard]] std::size_t TrackedRelationshipCount() const;

    [[nodiscard]] std::size_t StoredMemoryCount() const;

private:
    [[nodiscard]] bool PairParticipates(PlayerbotSocialRelationshipKey const& key) const;

    std::set<uint64> _optedOut;
    PlayerbotSocialRelationshipStore _relationships;
    PlayerbotSocialMemoryStore _memories;
};

// Persistence bindings ------------------------------------------------------------------------

/*
 * Exactly the values a relationship row is written with.
 *
 * This type exists so the clamp has one enforcement point instead of one per call site. The MySQL
 * layer binds these fields and nothing else, so it has no way to bind a familiarity, affinity, or
 * trust that did not come through `PlayerbotSocialBuildRelationshipBinding` and therefore through
 * `PlayerbotSocialClampRelationship`. Definition of Done 7 is that property, and it is provable in
 * a unit test because the binding is a value rather than a side effect on a statement handle.
 */
struct PlayerbotSocialRelationshipBinding
{
    uint64 botGuidCounter = 0;
    uint64 subjectGuidCounter = 0;
    float familiarity = PLAYERBOT_SOCIAL_NEUTRAL_FAMILIARITY;
    float affinity = PLAYERBOT_SOCIAL_NEUTRAL_AFFINITY;
    float trust = PLAYERBOT_SOCIAL_NEUTRAL_TRUST;
    uint32 interactionCount = 0;
    uint64 lastInteractionAtUnixSeconds = 0;
};

/*
 * The exact spellings of the schema's ENUM members.
 *
 * Each returns an empty view for a value outside its enum, and the write path refuses to bind an
 * empty one. The module compiles without -Wswitch, so an out of range value is reachable; MySQL
 * would coerce an unrecognized ENUM string to the empty member under a non strict mode, which would
 * store a memory whose category and privacy no longer mean anything.
 */
// Column bounds from `playerbot_social_event`. Stated here so the binding enforces the schema rather
// than discovering it as a truncation warning at write time.
inline constexpr std::size_t PLAYERBOT_SOCIAL_EVENT_TYPE_MAX_LENGTH = 48;
inline constexpr std::size_t PLAYERBOT_SOCIAL_EVENT_REASON_MAX_LENGTH = 64;
inline constexpr std::size_t PLAYERBOT_SOCIAL_EVENT_TEXT_MAX_LENGTH = 512;

/*
 * What an event is worth when there is not room for all of it.
 *
 * Three tiers rather than a number, because the only decision this drives is which record gives way,
 * and a numeric score invites producers to compete rather than to classify.
 *
 * `Diagnostic` is the correlation detail: opportunities, selection factors, provider attempts. Losing
 * it under pressure costs the ability to reconstruct WHY a line was said. `Standard` is what the feed
 * is actually about: deliveries, suppressions, assistance, PVP. `Critical` is the audit trail, where
 * a missing row is not a degraded view but an incomplete record: control changes, moderation, and
 * storage failures.
 */
enum class PlayerbotSocialEventPriority : uint8
{
    Diagnostic = 0,
    Standard,
    Critical
};

/*
 * What became of a draft handed to the queue.
 *
 * `Refused` and the two loss outcomes are deliberately distinct. A refusal means the draft was never
 * writable and nothing was lost; a drop or an eviction means a real record is gone. Collapsing them
 * into one signal would make a producer emitting malformed drafts read as queue pressure forever.
 */
enum class PlayerbotSocialEventQueueResult : uint8
{
    Queued = 0,
    QueuedAfterEviction,
    Dropped,
    Refused
};

/*
 * How many events may wait for the next drain.
 *
 * Sized for a burst rather than for steady state: a busy zone produces a few events per delivered
 * line, and the drain runs on the world update. The bound is what turns an unbounded backlog into a
 * counted, visible loss.
 */
inline constexpr std::size_t PLAYERBOT_SOCIAL_EVENT_QUEUE_CAPACITY = 512;

// The marker written when the queue lost events. One per drain, carrying the count.
inline constexpr std::string_view PLAYERBOT_SOCIAL_EVENT_TYPE_GAP = "social.telemetry.gap";
inline constexpr std::string_view PLAYERBOT_SOCIAL_EVENT_REASON_QUEUE_OVERFLOW = "event_queue_overflow";
inline constexpr std::string_view PLAYERBOT_SOCIAL_EVENT_REASON_PERSISTENCE_FAILURE = "event_persistence_failure";

/*
 * One opaque event identity.
 *
 * Lives here rather than beside the coordinator's thread identities because both the durable event
 * row and the coordinator's in memory thread history need the same identities to mean the same
 * thing. Two implementations of this would drift, and the copy that drifted would be the one nobody
 * re-reads, which is the failure shape this plan has met repeatedly. Fresh entropy makes the durable
 * identity unique when a worldserver restart reuses its process local sequence.
 */
[[nodiscard]] std::string PlayerbotSocialMakeEventPublicId(uint64 sequence, uint64 salt);

/*
 * What a caller wants recorded, before any of it has been checked.
 *
 * Separate from the binding on purpose. A producer assembles this from whatever it knows, and the
 * binding is what survived validation: there is no way to reach the statement's fields except
 * through `PlayerbotSocialBuildEventBinding`, so a new producer cannot be added that skips a rule.
 */
struct PlayerbotSocialEventDraft
{
    // The manager may reserve the durable identity before delivery so the observed line and row
    // share one exact value. Other producers leave it empty and receive a generated identity.
    std::string eventPublicId;
    uint64 eventSequence = 0;

    std::string eventType;
    PlayerbotSocialEventOrigin origin = PlayerbotSocialEventOrigin::System;
    PlayerbotSocialEventOutcome outcome = PlayerbotSocialEventOutcome::Recorded;

    // Assistance, PVP and control events belong to no conversation surface, and the column is
    // nullable for them. The flag is explicit rather than inferred, because General is a legitimate
    // value and a defaulted enum would silently mean it.
    PlayerbotSocialChannel channel = PlayerbotSocialChannel::General;
    bool hasChannel = false;

    std::string threadPublicId;
    std::string replyToEventPublicId;
    std::string sourceEventPublicId;
    uint64 botGuidCounter = 0;
    uint64 actorGuidCounter = 0;
    uint64 targetGuidCounter = 0;
    uint32 zoneId = 0;

    std::string reason;
    std::string messageText;
    std::string diagnosticsJson;

    uint64 occurredAtUnixSeconds = 0;

    /*
     * What this event is worth when the queue is full.
     *
     * `Standard` is the default on purpose. An event whose producer never considered its priority is
     * unclassified, and treating unclassified as droppable would quietly discard records nobody
     * decided were expendable. Both the droppable tier and the protected tier have to be asked for.
     *
     * Not carried onto the binding: it decides whether the row is written, never what the row says.
     */
    PlayerbotSocialEventPriority priority = PlayerbotSocialEventPriority::Standard;
};

/*
 * Exactly the values an event row is written with.
 *
 * The enum members are already resolved to their schema spellings here, so the MySQL layer binds
 * strings it cannot have invented. An empty origin or outcome is unreachable, because the builder
 * refuses rather than returning one.
 */
struct PlayerbotSocialEventBinding
{
    std::string publicId;
    std::string threadPublicId;
    std::string replyToEventPublicId;
    std::string sourceEventPublicId;
    std::string eventType;
    std::string origin;
    std::string outcome;

    std::string channel;
    bool hasChannel = false;

    uint64 botGuidCounter = 0;
    uint64 actorGuidCounter = 0;
    uint64 targetGuidCounter = 0;
    uint32 zoneId = 0;

    std::string reason;
    std::string messageText;
    std::string diagnosticsJson;

    uint64 occurredAtUnixSeconds = 0;
};

/*
 * Validates a draft and produces the row, or refuses.
 *
 * Refuses an unknown origin, outcome or channel, an empty or over long event type, and a malformed
 * thread identity. Truncates retained text rather than refusing it, because losing the telemetry for
 * a message that was actually delivered is worse than losing its tail.
 *
 * Drops the message text entirely for anything that was not delivered. This is the one table that
 * retains raw chat, and a line nobody heard has no business being in it.
 */
[[nodiscard]] bool PlayerbotSocialBuildEventBinding(PlayerbotSocialEventDraft const& draft,
                                                    PlayerbotSocialEventBinding& binding);

/*
 * A binding resolved against durable identities, ready for the statement.
 *
 * Separate from the binding because resolution needs state the binding layer has no business
 * holding: the actor id map and the configured retention window. Every identity here is optional,
 * matching the nullable columns, and the flag is what the statement binds on rather than a
 * sentinel: a literal zero would join to nothing while looking like a real actor.
 */
struct PlayerbotSocialEventRow
{
    uint32 actorId = 0;
    bool hasActor = false;

    uint32 targetActorId = 0;
    bool hasTargetActor = false;

    uint32 botActorId = 0;
    bool hasBotActor = false;

    uint32 zoneId = 0;
    bool hasZone = false;

    uint64 expiresAtUnixSeconds = 0;
};

/*
 * Resolves an event's actors and computes the expiry it will be purged by.
 *
 * An unresolved actor leaves its column null rather than dropping the event. What happened, on which
 * surface, in which thread, is the part the feed is about, and refusing to record it because one
 * identity was not cached would lose the event entirely to fix nothing.
 */
[[nodiscard]] PlayerbotSocialEventRow PlayerbotSocialBuildEventRow(PlayerbotSocialEventBinding const& binding,
                                                                   std::map<uint64, uint32> const& actorIds,
                                                                   uint32 configuredRetentionHours);

/*
 * Tracks one asynchronous event transaction and the rows lost when it fails.
 *
 * Prepare appends one critical persistence gap when earlier rows were lost. A failed recovery keeps
 * that earlier count and adds every row in the failed batch. A successful recovery clears the count.
 */
class PlayerbotSocialEventPersistenceTracker
{
public:
    bool Prepare(std::vector<PlayerbotSocialEventBinding>& rows, uint64 gapEventSequence, uint64 nowUnixSeconds);
    void Complete(bool success);

    [[nodiscard]] bool InFlight() const { return _inFlight; }
    [[nodiscard]] uint64 LostRows() const { return _lostRows; }

private:
    void AddLostRows(uint64 count);

    bool _inFlight = false;
    bool _inFlightReportsLoss = false;
    uint64 _inFlightRows = 0;
    uint64 _lostRows = 0;
};

/*
 * A bounded backlog of events waiting to be written.
 *
 * This is what keeps telemetry off the world update loop's critical path: a producer validates and
 * hands over a row and returns immediately, and the drain hands the batch to the database layer,
 * whose own worker performs the write.
 *
 * Deliberately not thread safe, and deliberately without a lock. Every social producer and the drain
 * run on the world thread, matching the rest of this module, and a mutex here would advertise a
 * concurrency guarantee nothing in the design actually relies on.
 *
 * Validation happens on the way in rather than at drain time, so capacity is only ever spent on rows
 * that can actually be written, and a producer bug surfaces at the producer.
 */
class PlayerbotSocialEventQueue
{
public:
    explicit PlayerbotSocialEventQueue(std::size_t capacity = PLAYERBOT_SOCIAL_EVENT_QUEUE_CAPACITY);

    /*
     * Validates the draft and takes it, evicting a strictly lower priority entry if that is the only
     * way to make room. When nothing queued ranks below the incoming event, the incoming one is what
     * gives way: an event already accepted is never displaced by one no more important than itself.
     */
    PlayerbotSocialEventQueueResult Push(PlayerbotSocialEventDraft const& draft);

    /*
     * Hands over everything waiting and clears the backlog.
     *
     * When events were lost since the last drain, exactly one gap marker is appended, carrying the
     * count. The caller supplies its sequence, because the producer owns the sequence and a marker
     * must not invent an identity under a different rule than every other event.
     */
    std::vector<PlayerbotSocialEventBinding> Drain(uint64 gapEventSequence, uint64 nowUnixSeconds);

    [[nodiscard]] std::size_t PendingCount() const;

    /*
     * A copy of what is waiting, in the order it arrived, without consuming any of it.
     *
     * Copies rather than a reference, so a caller inspecting the backlog cannot hold anything that
     * the next `Push` could reallocate out from under it. The bound is the queue's own capacity, so
     * this cannot become an unbounded allocation.
     */
    [[nodiscard]] std::vector<PlayerbotSocialEventBinding> Pending() const;

    // Events discarded since the last drain, whether dropped on arrival or evicted to make room.
    [[nodiscard]] uint64 LostSinceLastDrain() const;

private:
    struct Entry
    {
        PlayerbotSocialEventBinding binding;
        PlayerbotSocialEventPriority priority = PlayerbotSocialEventPriority::Standard;
    };

    std::size_t _capacity;
    std::vector<Entry> _pending;
    uint64 _lost = 0;
};

// Mirrors `playerbot_social_event.channel`. Empty for a value outside the enum, like every other
// schema spelling here, so an unrecognized channel is refused rather than coerced to the first member.
[[nodiscard]] std::string_view PlayerbotSocialChannelName(PlayerbotSocialChannel channel);

[[nodiscard]] std::string_view PlayerbotSocialEventOriginName(PlayerbotSocialEventOrigin origin);

[[nodiscard]] std::string_view PlayerbotSocialEventOutcomeName(PlayerbotSocialEventOutcome outcome);

[[nodiscard]] std::string_view PlayerbotSocialMemoryCategoryName(PlayerbotSocialMemoryCategory category);
[[nodiscard]] std::string_view PlayerbotSocialMemoryProvenanceName(PlayerbotSocialMemoryProvenance provenance);
[[nodiscard]] std::string_view PlayerbotSocialPrivacyScopeName(PlayerbotSocialPrivacyScope scope);

/*
 * The same mapping read back. Each returns false and leaves its output untouched for a spelling the
 * enum does not have, so a row written by a future schema, an older build, or by hand is skipped
 * rather than coerced into whichever member happens to be zero. For the privacy scope that matters
 * most: member zero is Public, and silently defaulting to it would publish a private memory.
 */
[[nodiscard]] bool PlayerbotSocialParseMemoryCategory(std::string_view text, PlayerbotSocialMemoryCategory& category);
[[nodiscard]] bool PlayerbotSocialParseMemoryProvenance(std::string_view text,
                                                        PlayerbotSocialMemoryProvenance& provenance);
[[nodiscard]] bool PlayerbotSocialParsePrivacyScope(std::string_view text, PlayerbotSocialPrivacyScope& scope);
[[nodiscard]] bool PlayerbotSocialParseMemorySourceKind(std::string_view text, PlayerbotSocialMemorySourceKind& kind);

[[nodiscard]] PlayerbotSocialRelationshipBinding PlayerbotSocialBuildRelationshipBinding(
    PlayerbotSocialRelationshipKey const& key, PlayerbotSocialRelationshipValues const& values, uint32 interactionCount,
    uint64 nowUnixSeconds);

/*
 * Fails closed on an unknown channel: returns false and leaves `query` untouched, so a caller that
 * ignores the result reads with whatever it already had rather than silently widening to Any. The
 * module compiles without -Wswitch, so an out of range channel really is reachable here.
 */
[[nodiscard]] bool PlayerbotSocialMemoryScopeQueryFor(PlayerbotSocialChannel channel,
                                                      PlayerbotSocialMemoryScopeQuery& query);

// The write-side twin: the surface a line was heard on decides the privacy scope its extracted
// memory persists with. False for a channel this build does not recognize (fails closed).
[[nodiscard]] bool PlayerbotSocialPrivacyScopeForChannel(PlayerbotSocialChannel channel,
                                                         PlayerbotSocialPrivacyScope& scope);

// The reverse mapping extraction telemetry files under; Public collapses to General.
[[nodiscard]] PlayerbotSocialChannel PlayerbotSocialChannelForPrivacyScope(PlayerbotSocialPrivacyScope scope);

/*
 * Whether a stored scope is inside what a query returns.
 *
 * This is what makes a scoped read replaceable. A read restricted to public scope is the whole
 * answer for the public scope and says nothing about party or whisper, so applying it may only
 * replace the part of a bot's memory it actually covers. Replacing everything would delete the
 * private memories a General conversation never asked about.
 */
[[nodiscard]] bool PlayerbotSocialMemoryScopeIsWithinQuery(PlayerbotSocialPrivacyScope scope,
                                                           PlayerbotSocialMemoryScopeQuery query);

/*
 * How long a loaded snapshot may be served before it is read again.
 *
 * Key Decision 2 asks for short lived snapshots. Short matters because the durable row is not the
 * only writer: a reset from another session, a cohort purge, or a Medivh side change all land in
 * MySQL without passing through this process's containers, and the TTL is the bound on how long
 * this process can keep answering from a view that has stopped being true.
 */
inline constexpr uint64 PLAYERBOT_SOCIAL_SNAPSHOT_TTL_SECONDS = 300;

/*
 * A snapshot older than the TTL, or one stamped in the future, is stale. The second case is the
 * clock moving backwards: treating it as fresh would pin the snapshot until the clock caught up,
 * which is exactly the unbounded staleness the TTL exists to prevent.
 */
[[nodiscard]] bool PlayerbotSocialSnapshotIsFresh(uint64 loadedAtUnixSeconds, uint64 nowUnixSeconds);

// Consent commands ----------------------------------------------------------------------------

/*
 * What a character typed under `.playerbots social`.
 *
 * There is deliberately no target in this type. Every one of these commands acts on the character
 * who issued it, and the parser refuses any form carrying an extra word, so there is no spelling of
 * a command that names somebody else. That is enforced here rather than by the handler checking a
 * parsed name against the issuer, because a check can be forgotten and a type that cannot express
 * a target cannot be.
 */
enum class PlayerbotSocialConsentCommand : uint8
{
    Unrecognized = 0,
    Status,
    OptOut,
    OptIn,

    // `reset` on its own. Explains what reset would do and changes nothing, so the destructive form
    // is never one keystroke away from the informational one.
    ResetRequested,

    // `reset confirm`. The only form that erases anything.
    ResetConfirmed
};

[[nodiscard]] char const* PlayerbotSocialConsentCommandName(PlayerbotSocialConsentCommand command);

[[nodiscard]] PlayerbotSocialConsentCommand PlayerbotSocialParseConsentCommand(std::string_view arguments);

#endif  // MOD_PLAYERBOTS_PLAYERBOT_SOCIAL_REPOSITORY_H
