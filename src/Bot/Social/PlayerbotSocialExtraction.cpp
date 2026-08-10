/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#include "PlayerbotSocialExtraction.h"

#include <algorithm>
#include <utility>

#include "Bot/Social/PlayerbotSocialRepository.h"

char const* PlayerbotSocialBufferRejectionName(PlayerbotSocialBufferRejection rejection)
{
    switch (rejection)
    {
        case PlayerbotSocialBufferRejection::Accepted:
            return "accepted";
        case PlayerbotSocialBufferRejection::WhisperNeverBuffered:
            return "whisper_never_buffered";
        case PlayerbotSocialBufferRejection::EmptyText:
            return "empty_text";
        case PlayerbotSocialBufferRejection::TextTooLong:
            return "text_too_long";
        case PlayerbotSocialBufferRejection::SpeakerNotConsented:
            return "speaker_not_consented";
    }

    return "unknown";
}

PlayerbotSocialBufferRejection PlayerbotSocialExtractionBuffer::Offer(PlayerbotSocialChannel channel,
                                                                      PlayerbotSocialBufferedLine line,
                                                                      bool speakerConsented, uint64 nowUnixSeconds)
{
    /*
     * The surface decides first, before anything about the speaker is consulted. A consenting player
     * has agreed to bots remembering conversations, which is not the same as agreeing to their
     * whispers being held in memory for a model to read, so consent cannot unlock this.
     */
    if (channel == PlayerbotSocialChannel::Whisper)
        return PlayerbotSocialBufferRejection::WhisperNeverBuffered;

    // An unrecognised channel is refused for the same reason routing fails closed: a surface this
    // build does not know about is not one whose privacy expectations it can honour.
    if (!PlayerbotSocialChannelIsValid(channel))
        return PlayerbotSocialBufferRejection::WhisperNeverBuffered;

    if (line.text.empty())
        return PlayerbotSocialBufferRejection::EmptyText;

    if (line.text.size() > PLAYERBOT_SOCIAL_EXTRACTION_MAX_LINE_BYTES)
        return PlayerbotSocialBufferRejection::TextTooLong;

    // Only a human has consent to give. A bot's line carries no privacy interest of its own, and
    // refusing it would leave a thread of players answering nobody.
    if (line.speakerIsHuman && !speakerConsented)
        return PlayerbotSocialBufferRejection::SpeakerNotConsented;

    /*
     * Expired before the new line is added rather than after. Doing it after would let a line that
     * is about to be dropped anyway push a live one out through the count bound, so a busy thread
     * would lose recent turns to stale ones.
     */
    Expire(nowUnixSeconds);

    _bytes += line.text.size();
    _lines.push_back(std::move(line));
    EnforceBounds();

    return PlayerbotSocialBufferRejection::Accepted;
}

void PlayerbotSocialExtractionBuffer::ForgetSpeaker(uint64 characterGuidCounter)
{
    for (auto line = _lines.begin(); line != _lines.end();)
    {
        if (line->speakerGuidCounter != characterGuidCounter)
        {
            ++line;
            continue;
        }

        // The accounting follows every removal. A stale byte total would make the buffer refuse
        // lines it is no longer holding, and it would do so silently for the rest of the thread.
        _bytes -= std::min(_bytes, line->text.size());
        line = _lines.erase(line);
    }
}

void PlayerbotSocialExtractionBuffer::Expire(uint64 nowUnixSeconds)
{
    while (!_lines.empty())
    {
        PlayerbotSocialBufferedLine const& oldest = _lines.front();

        /*
         * A stamp in the future is dropped as well as one that is too old. A clock that stepped
         * backwards leaves an entry that can never age out, and here that would mean chat held in
         * memory for the life of the process, which is exactly what this window exists to prevent.
         */
        bool const inFuture = oldest.atUnixSeconds > nowUnixSeconds;
        bool const expired =
            !inFuture && nowUnixSeconds - oldest.atUnixSeconds > PLAYERBOT_SOCIAL_EXTRACTION_RETENTION_SECONDS;

        if (!inFuture && !expired)
            return;

        DropFront();
    }
}

void PlayerbotSocialExtractionBuffer::Clear()
{
    _lines.clear();
    _bytes = 0;
}

bool PlayerbotSocialExtractionBuffer::EligibleForExtraction() const
{
    return std::any_of(_lines.begin(), _lines.end(),
                       [](PlayerbotSocialBufferedLine const& line) { return line.speakerIsHuman; });
}

void PlayerbotSocialExtractionBuffer::EnforceBounds()
{
    while (!_lines.empty() &&
           (_lines.size() > PLAYERBOT_SOCIAL_EXTRACTION_MAX_LINES || _bytes > PLAYERBOT_SOCIAL_EXTRACTION_MAX_BYTES))
    {
        DropFront();
    }
}

void PlayerbotSocialExtractionBuffer::DropFront()
{
    _bytes -= std::min(_bytes, _lines.front().text.size());
    _lines.pop_front();
}

bool PlayerbotSocialThreadIsIdleForExtraction(uint64 lastActivityUnixSeconds, uint64 nowUnixSeconds)
{
    if (lastActivityUnixSeconds > nowUnixSeconds)
        return false;

    uint64 const quietFor = nowUnixSeconds - lastActivityUnixSeconds;

    return quietFor >= PLAYERBOT_SOCIAL_EXTRACTION_IDLE_SECONDS && quietFor <= PLAYERBOT_SOCIAL_THREAD_STALE_SECONDS;
}

char const* PlayerbotSocialSnapshotRefusalName(PlayerbotSocialSnapshotRefusal refusal)
{
    switch (refusal)
    {
        case PlayerbotSocialSnapshotRefusal::Accepted:
            return "accepted";
        case PlayerbotSocialSnapshotRefusal::NothingBuffered:
            return "nothing_buffered";
        case PlayerbotSocialSnapshotRefusal::NoConsentedSpeaker:
            return "no_consented_speaker";
        case PlayerbotSocialSnapshotRefusal::UnsafeContent:
            return "unsafe_content";
        case PlayerbotSocialSnapshotRefusal::NoBotPresent:
            return "no_bot_present";
    }

    return "unknown";
}

PlayerbotSocialExtractionSnapshot PlayerbotSocialBuildExtractionSnapshot(
    PlayerbotSocialExtractionBuffer const& buffer, PlayerbotSocialExtractionConsent const& consents,
    uint64 nowUnixSeconds)
{
    PlayerbotSocialExtractionSnapshot snapshot;
    bool hadConsentedHuman = false;

    // No callable is a refusal rather than a crash or an assumption of consent. This runs on the
    // world thread beside everything else, and neither of those is a reasonable thing to do there.
    if (buffer.LineCount() == 0 || !consents)
        return snapshot;

    for (PlayerbotSocialBufferedLine const& line : buffer.Lines())
    {
        /*
         * The retention window is reapplied here rather than assumed to have been swept. The two
         * checks fail differently: a missed sweep leaves old text in memory, while a missed sweep
         * that also submits sends a player's words to a provider long after the window they were
         * told about. This is what makes the second impossible on its own.
         */
        bool const inFuture = line.atUnixSeconds > nowUnixSeconds;
        if (!inFuture && nowUnixSeconds - line.atUnixSeconds > PLAYERBOT_SOCIAL_EXTRACTION_RETENTION_SECONDS)
            continue;

        // Rechecked, not trusted from buffer time. Consent can be withdrawn during the very silence
        // that makes a thread eligible.
        if (line.speakerIsHuman && !consents(line.speakerGuidCounter))
            continue;
        if (line.speakerIsHuman)
            hadConsentedHuman = true;

        if (!line.speakerIsHuman && snapshot.holderGuidCounter == 0)
            snapshot.holderGuidCounter = line.speakerGuidCounter;

        // Generated dialogue remains short term conversational context only. Even when it repeats
        // a supplied fact, it is not evidence and cannot become durable factual memory.
        if (line.sourceKind == PlayerbotSocialMemorySourceKind::GeneratedDelivery)
            continue;

        if (!PlayerbotSocialPublicIdIsValid(PlayerbotSocialIdKind::Event, line.sourceEventPublicId))
            continue;

        /*
         * One unsafe line refuses the whole thread, and a bot's line is checked alongside a
         * player's: an injection arrives at a bot by being said to it, so trusting bot turns would
         * leave the shortest path in open.
         */
        if (PlayerbotSocialTextLooksSensitive(line.text) || PlayerbotSocialTextLooksLikeAnInstruction(line.text))
        {
            snapshot.lines.clear();
            snapshot.subjects.clear();
            snapshot.refusal = PlayerbotSocialSnapshotRefusal::UnsafeContent;
            return snapshot;
        }

        if (line.speakerIsHuman && std::find(snapshot.subjects.begin(), snapshot.subjects.end(),
                                             line.speakerGuidCounter) == snapshot.subjects.end())
            snapshot.subjects.push_back(line.speakerGuidCounter);

        snapshot.lines.push_back(line);
    }

    /*
     * A thread with no surviving human is not submitted at all, even when bot turns remain. There is
     * nobody it could be a memory about, so the request would cost money to learn nothing.
     */
    if (snapshot.subjects.empty())
    {
        // The two reasons are separated because they tell an operator different things: nothing left
        // means the window emptied, while no consented speaker means people opted out.
        snapshot.lines.clear();
        snapshot.refusal = hadConsentedHuman ? PlayerbotSocialSnapshotRefusal::NothingBuffered
                                             : PlayerbotSocialSnapshotRefusal::NoConsentedSpeaker;
        return snapshot;
    }

    /*
     * A conversation no bot took part in belongs to nobody. Attributing it to some bot that
     * happened to be standing nearby would be that bot remembering something it was not part of,
     * which is eavesdropping rather than a memory.
     */
    if (snapshot.holderGuidCounter == 0)
    {
        snapshot.lines.clear();
        snapshot.subjects.clear();
        snapshot.refusal = PlayerbotSocialSnapshotRefusal::NoBotPresent;
        return snapshot;
    }

    snapshot.refusal = PlayerbotSocialSnapshotRefusal::Accepted;
    return snapshot;
}

bool PlayerbotSocialExtractedMemoryIsAdmissible(std::vector<uint64> const& requestSubjects,
                                                PlayerbotSocialPrivacyScope requestScope, uint64 aboutGuidCounter,
                                                PlayerbotSocialPrivacyScope memoryScope)
{
    // An empty list is nobody, not everybody, and the search is what makes that true: over an empty
    // range it returns end(), so a request carrying no subjects admits nothing. Written as one check
    // rather than two because a separate empty() guard cannot change the answer, and a branch that
    // cannot change the answer reads like the thing holding the rule up.
    if (std::find(requestSubjects.begin(), requestSubjects.end(), aboutGuidCounter) == requestSubjects.end())
        return false;

    return memoryScope == requestScope;
}

bool PlayerbotSocialMemorySourceIsAdmissible(std::vector<PlayerbotSocialBufferedLine> const& requestSources,
                                             uint64 aboutGuidCounter, std::string const& sourceEventPublicId)
{
    if (aboutGuidCounter == 0 || !PlayerbotSocialPublicIdIsValid(PlayerbotSocialIdKind::Event, sourceEventPublicId))
        return false;

    PlayerbotSocialBufferedLine const* matched = nullptr;
    for (PlayerbotSocialBufferedLine const& source : requestSources)
    {
        if (source.sourceEventPublicId != sourceEventPublicId)
            continue;

        if (matched)
            return false;

        matched = &source;
    }

    if (!matched || matched->speakerGuidCounter != aboutGuidCounter)
        return false;

    if (matched->sourceKind == PlayerbotSocialMemorySourceKind::HumanObservation)
        return matched->speakerIsHuman;

    if (matched->sourceKind == PlayerbotSocialMemorySourceKind::AuthoritativeSource)
        return !matched->speakerIsHuman;

    return false;
}

bool PlayerbotSocialExtractionAnswerIsForRequest(uint64 requestBotGuidCounter, std::string const& requestThreadPublicId,
                                                 uint64 answerBotGuidCounter, std::string const& answerThreadPublicId)
{
    // Neither side may be blank. Two empties comparing equal is how a reply that identifies nothing
    // passes an identity check, and this one is the last thing between a stray answer and a write.
    if (requestBotGuidCounter == 0 || requestThreadPublicId.empty())
        return false;

    return requestBotGuidCounter == answerBotGuidCounter && requestThreadPublicId == answerThreadPublicId;
}
