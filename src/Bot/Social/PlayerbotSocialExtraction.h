/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef PLAYERBOTS_PLAYERBOTSOCIALEXTRACTION_H
#define PLAYERBOTS_PLAYERBOTSOCIALEXTRACTION_H

#include <cstddef>
#include <deque>
#include <functional>
#include <string>
#include <vector>

// For PLAYERBOT_SOCIAL_THREAD_STALE_SECONDS: the extraction window is expressed against the window
// that prunes a thread, because a bound sitting near that number by coincidence would silently stop
// collecting anything the day either one moved.
#include "Bot/Social/PlayerbotSocialPolicy.h"
#include "Bot/Social/PlayerbotSocialTypes.h"
#include "Define.h"

/*
 * The transient chat buffer idle memory extraction reads from.
 *
 * This is the one place in the feature that holds raw player chat, and it exists only because
 * extraction happens at an IDLE boundary: a thread is idle precisely because nobody has spoken for
 * the staleness window, so the words have to have been kept from the moment they arrived. Everything
 * here is shaped by making that as small a privacy surface as it can be while still being useful.
 *
 * Four rules, each enforced here rather than at the call sites:
 *
 * Whispers are buffered only while the operator's whisper memory switch is on, and then only under
 * the same per speaker consent as every other surface. The switch arrives as an argument on each
 * offer rather than being read here, and it defaults to OFF: a caller that never learned about it
 * keeps the old refusal, so the most private surface fails closed at every un-updated call site.
 *
 * Consent is checked when a line is OFFERED, not when it is submitted. A player who has not
 * consented never has their words in the worldserver's memory at all, and an opt out purges what was
 * already there. "We never kept it" is a stronger promise than "we did not send it".
 *
 * Nothing here is ever written to MySQL or any other durable store. The buffer dies with its thread,
 * on Clear after extraction, or on the retention window below, whichever comes first.
 *
 * It is bounded in count, in bytes, and in time, independently. A quiet thread that is never
 * extracted from must not leave chat in memory until the process restarts.
 */

// One buffered line. Values only: a guid rather than a Player, so a logout cannot leave a dangling
// pointer, and no display name, which is resolved at submission time and never held here.
struct PlayerbotSocialBufferedLine
{
    uint64 speakerGuidCounter = 0;
    bool speakerIsHuman = false;
    PlayerbotSocialMemorySourceKind sourceKind = PlayerbotSocialMemorySourceKind::GeneratedDelivery;
    std::string sourceEventPublicId;
    uint64 atUnixSeconds = 0;
    std::string text;
};

// Why a line was not buffered. Counted and logged; never shown to a player.
enum class PlayerbotSocialBufferRejection : uint8
{
    Accepted = 0,
    WhisperMemoryDisabled,  // The operator's switch is off. Refused before consent is consulted.
    EmptyText,
    TextTooLong,
    SpeakerNotConsented,
    UnrecognizedChannel  // A surface this build does not know cannot have its privacy honoured.
};

[[nodiscard]] char const* PlayerbotSocialBufferRejectionName(PlayerbotSocialBufferRejection rejection);

/*
 * How many lines one thread may hold, and how many bytes across all of them.
 *
 * Two bounds because one does not cover the other: many short lines and a few enormous ones are
 * different ways to spend the same memory. Per thread, and a busy realm has a thread per zone, per
 * party, so these are multiplied by everything talking at once.
 */
inline constexpr std::size_t PLAYERBOT_SOCIAL_EXTRACTION_MAX_LINES = 12;
inline constexpr std::size_t PLAYERBOT_SOCIAL_EXTRACTION_MAX_BYTES = 4096;

// The longest single line accepted. A longer one is refused WHOLE rather than truncated: a cut
// sentence changes what was said, and the model would attribute the remainder to its speaker.
inline constexpr std::size_t PLAYERBOT_SOCIAL_EXTRACTION_MAX_LINE_BYTES = 512;

/*
 * How long a line may sit here before it is dropped unread.
 *
 * Enforced on its own rather than as a side effect of the idle sweep, so a thread that goes quiet
 * and is never extracted from still stops holding chat. Comfortably longer than the staleness window
 * that triggers extraction, so an ordinary conversation is not expired out from under it.
 */
inline constexpr uint64 PLAYERBOT_SOCIAL_EXTRACTION_RETENTION_SECONDS = 900;

/*
 * How long a thread must have been silent before it is worth extracting from.
 *
 * The trigger is silence rather than volume, because a memory is drawn from a conversation that
 * FINISHED: extracting from one still in progress produces a memory of its first half, and then
 * another of the rest. Long enough that a pause to fight something is not mistaken for an ending,
 * and short enough to leave room before the staleness window prunes the thread and its buffer with
 * it. That gap is the window the sweep runs inside, and it is asserted rather than assumed.
 */
inline constexpr uint64 PLAYERBOT_SOCIAL_EXTRACTION_IDLE_SECONDS = 120;

/*
 * How many threads one sweep may collect from.
 *
 * Each collected thread becomes a provider request, and a realm has a thread per zone and per party,
 * so an unbounded sweep would turn one quiet moment into hundreds of simultaneous requests. Threads
 * beyond the bound are left alone rather than emptied, so they are collected by a later sweep;
 * because collecting CLEARS a buffer, the ones taken this time drop out of eligibility and a
 * different set is first in line next time, which is what keeps a low ordered scope from starving.
 */
inline constexpr std::size_t PLAYERBOT_SOCIAL_EXTRACTION_MAX_PER_SWEEP = 4;

/*
 * What an extracted memory is worth, on the store's 0..1 scales.
 *
 * Middling on purpose, and fixed rather than asked for. A model rating its own output is vouching
 * for itself, and these values decide which memories survive retention and which are drawn on
 * first, so letting a generation set them would let it promote its own guesses above a fact the
 * server observed directly. An extraction is a plausible reading of a conversation, which is worth
 * less than something the worldserver watched happen and more than nothing.
 */
inline constexpr float PLAYERBOT_SOCIAL_EXTRACTION_CONFIDENCE = 0.5f;
inline constexpr float PLAYERBOT_SOCIAL_EXTRACTION_SIGNIFICANCE = 0.4f;

/*
 * Whether a thread last active at `lastActivityUnixSeconds` is in that window right now.
 *
 * A stamp in the future reads as NOT idle. A clock that stepped backwards would otherwise make every
 * live conversation on the realm eligible at once, which is both a spending event and a model being
 * handed conversations mid sentence.
 */
[[nodiscard]] bool PlayerbotSocialThreadIsIdleForExtraction(uint64 lastActivityUnixSeconds, uint64 nowUnixSeconds);

class PlayerbotSocialExtractionBuffer
{
public:
    /*
     * Offers one observed line. Returns why it was refused, or that it was not.
     *
     * `speakerConsented` is the caller's answer for a HUMAN speaker and is ignored for a bot, which
     * has no consent to give. The caller passes it rather than this class asking, because the
     * authoritative answer lives in the manager's fail closed consent state and this unit is
     * deliberately free of it.
     *
     * `whisperMemoryEnabled` is the operator's switch for the whisper surface, injected the same
     * way and for the same reason. It defaults to off so an un-updated caller keeps refusing the
     * most private surface.
     */
    PlayerbotSocialBufferRejection Offer(PlayerbotSocialChannel channel, PlayerbotSocialBufferedLine line,
                                         bool speakerConsented, uint64 nowUnixSeconds,
                                         bool whisperMemoryEnabled = false);

    // Drops every line this character spoke. The other participants' lines stay: they consented, and
    // dropping theirs too would be a second privacy decision taken on their behalf.
    void ForgetSpeaker(uint64 characterGuidCounter);

    // Drops everything older than the retention window.
    void Expire(uint64 nowUnixSeconds);

    // Releases everything, called once extraction concludes.
    void Clear();

    /*
     * Whether this is worth spending a provider request on.
     *
     * False for a buffer of only bot lines: bots talking to each other is not a conversation anyone
     * needs remembered, and extracting from one costs real money to learn nothing.
     */
    [[nodiscard]] bool EligibleForExtraction() const;

    [[nodiscard]] std::deque<PlayerbotSocialBufferedLine> const& Lines() const { return _lines; }
    [[nodiscard]] std::size_t LineCount() const { return _lines.size(); }
    [[nodiscard]] std::size_t ByteCount() const { return _bytes; }

private:
    // Drops the oldest until both bounds hold. Oldest first because a conversation's recent turns are
    // the ones worth remembering; evicting those would leave a record of how the thread started.
    void EnforceBounds();

    void DropFront();

    std::deque<PlayerbotSocialBufferedLine> _lines;

    // Tracked rather than recomputed, because every Offer would otherwise walk the whole buffer. Kept
    // in step by every path that removes a line, which is what the tests pin.
    std::size_t _bytes = 0;
};

/*
 * The second gate: what may LEAVE the buffer.
 *
 * Buffering and submitting are separated because a thread reaches extraction by going QUIET, so
 * minutes pass between the two, and everything the first gate decided may have changed since.
 * Buffer time consent is necessary; it is not sufficient.
 */

// Why nothing may be submitted. Counted and logged; the refused text is never copied anywhere.
enum class PlayerbotSocialSnapshotRefusal : uint8
{
    Accepted = 0,
    NothingBuffered,
    NoConsentedSpeaker,    // Every human has withdrawn, or none was ever eligible.
    UnsafeContent,         // A surviving line carries a secret or an instruction. Refuses the thread.
    NoBotPresent,          // Nobody was there to remember it. Overhearing is not participating.
    WhisperMemoryDisabled  // A whisper thread whose lines outlived the operator's permission.
};

[[nodiscard]] char const* PlayerbotSocialSnapshotRefusalName(PlayerbotSocialSnapshotRefusal refusal);

// What may be submitted for one idle thread, or why nothing may be.
struct PlayerbotSocialExtractionSnapshot
{
    PlayerbotSocialSnapshotRefusal refusal = PlayerbotSocialSnapshotRefusal::NothingBuffered;

    // The conversation as the provider will read it, oldest first, bot turns included.
    std::vector<PlayerbotSocialBufferedLine> lines;

    /*
     * Who a returned memory may be about: the consenting humans still present, in the order they
     * spoke. Bots are absent deliberately. A memory is a bot's record ABOUT a character, so offering
     * a bot as a subject would let one bot accumulate opinions about another with no consent behind
     * either of them.
     */
    std::vector<uint64> subjects;

    /*
     * The bot whose memory this becomes: the first one that spoke, and only that one.
     *
     * A memory is one character's recollection, so it needs a character who was actually in the
     * conversation. Storing the same extracted fact against every bot in the zone would be several
     * times the cost and several bots holding an identical memory none of them individually formed.
     */
    uint64 holderGuidCounter = 0;

    [[nodiscard]] bool Accepted() const { return refusal == PlayerbotSocialSnapshotRefusal::Accepted; }
};

// Answers whether this character consents right now. Injected rather than asked directly, because the
// authoritative answer is the coordinator's fail closed one and this unit stays free of it.
using PlayerbotSocialExtractionConsent = std::function<bool(uint64 characterGuidCounter)>;

/*
 * Decides what one idle thread may submit.
 *
 * Rechecks consent, reapplies the retention window rather than trusting a sweep ran, and refuses the
 * WHOLE thread when any surviving line carries a real world secret or an injected instruction. The
 * refusal is deliberately not a redaction: a credential is given meaning by the lines around it, and
 * an injected instruction reaching durable memory is replayed into every later prompt.
 */
[[nodiscard]] PlayerbotSocialExtractionSnapshot PlayerbotSocialBuildExtractionSnapshot(
    PlayerbotSocialExtractionBuffer const& buffer, PlayerbotSocialExtractionConsent const& consents,
    uint64 nowUnixSeconds, PlayerbotSocialChannel channel = PlayerbotSocialChannel::General,
    bool whisperMemoryEnabled = false);

/*
 * The third gate: what may be WRITTEN, out of what the provider sent back.
 *
 * The far side already applies both of these rules. They are applied again here because the far
 * side is exactly what this gate exists to be wrong about: the sidecar is a separate process
 * speaking over a socket, and a compromised, downgraded, or simply buggy one would otherwise be
 * trusted to police the two rules that separate a memory from a leak.
 *
 * A subject not in the request never consented to be remembered. The request's subject list is the
 * set of consenting humans who actually spoke, so a returned memory naming anyone else is about
 * somebody who was not there, or was there and said no.
 *
 * A scope wider than the surface is a party confidence a bot may then repeat in a zone. Equality
 * rather than a rank comparison is deliberate: narrowing is not a favour either, because a memory
 * recorded narrower than the conversation it came from misrepresents what those players said in
 * front of each other, and the store's visibility rules would then answer questions about it wrongly.
 *
 * Pure and free of the coordinator on purpose. This is the one decision on the extraction return
 * path that cannot be reached from a test through the coordinator, because getting there needs a
 * consenting speaker and granting consent issues a prepared statement.
 */
[[nodiscard]] bool PlayerbotSocialExtractedMemoryIsAdmissible(std::vector<uint64> const& requestSubjects,
                                                              PlayerbotSocialPrivacyScope requestScope,
                                                              uint64 aboutGuidCounter,
                                                              PlayerbotSocialPrivacyScope memoryScope);

[[nodiscard]] bool PlayerbotSocialMemorySourceIsAdmissible(
    std::vector<PlayerbotSocialBufferedLine> const& requestSources, uint64 aboutGuidCounter,
    std::string const& sourceEventPublicId);

/*
 * Whether an answer is the answer to the request it names.
 *
 * A matching token is not enough. The token says which request is being answered; this says the
 * answer describes the same conversation and the same bot the request was about, and an answer that
 * does not is somebody else's however well formed it is.
 *
 * The caller must run this BEFORE releasing the outstanding request. A mismatched answer that
 * consumed the request would leave the real one arriving later to a token nobody is holding, so a
 * wrong answer would silently cost a right one. Releasing belongs between this check and the write.
 */
[[nodiscard]] bool PlayerbotSocialExtractionAnswerIsForRequest(uint64 requestBotGuidCounter,
                                                               std::string const& requestThreadPublicId,
                                                               uint64 answerBotGuidCounter,
                                                               std::string const& answerThreadPublicId);

#endif
