/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#include "Bot/Social/PlayerbotSocialPromptContext.h"

#include <algorithm>
#include <utility>

#include "Bot/Social/PlayerbotSocialRepository.h"

PlayerbotSocialPromptContextRejection PlayerbotSocialPromptContextBuffer::Offer(PlayerbotSocialChannel channel,
                                                                                PlayerbotSocialPromptLine line,
                                                                                bool speakerConsented,
                                                                                uint64 nowUnixSeconds)
{
    if (!PlayerbotSocialChannelIsValid(channel))
        return PlayerbotSocialPromptContextRejection::UnsupportedChannel;

    if (line.text.empty())
        return PlayerbotSocialPromptContextRejection::EmptyText;

    if (line.text.size() > PLAYERBOT_SOCIAL_PROMPT_CONTEXT_MAX_LINE_BYTES)
        return PlayerbotSocialPromptContextRejection::TextTooLong;

    if (line.speakerIsHuman && !speakerConsented)
        return PlayerbotSocialPromptContextRejection::SpeakerNotConsented;

    _bytes += line.text.size();
    _lines.push_back(std::move(line));
    Expire(nowUnixSeconds);
    EnforceBounds();
    return PlayerbotSocialPromptContextRejection::Accepted;
}

void PlayerbotSocialPromptContextBuffer::ForgetSpeaker(uint64 characterGuidCounter)
{
    for (auto line = _lines.begin(); line != _lines.end();)
    {
        if (line->speakerGuidCounter != characterGuidCounter)
        {
            ++line;
            continue;
        }

        _bytes -= std::min(_bytes, line->text.size());
        line = _lines.erase(line);
    }
}

void PlayerbotSocialPromptContextBuffer::Expire(uint64 nowUnixSeconds)
{
    for (auto line = _lines.begin(); line != _lines.end();)
    {
        bool const inFuture = line->atUnixSeconds > nowUnixSeconds;
        bool const expired =
            !inFuture && nowUnixSeconds - line->atUnixSeconds > PLAYERBOT_SOCIAL_PROMPT_CONTEXT_RETENTION_SECONDS;
        if (!inFuture && !expired)
        {
            ++line;
            continue;
        }

        _bytes -= std::min(_bytes, line->text.size());
        line = _lines.erase(line);
    }
}

void PlayerbotSocialPromptContextBuffer::Clear()
{
    _lines.clear();
    _bytes = 0;
}

void PlayerbotSocialPromptContextBuffer::EnforceBounds()
{
    while (!_lines.empty() && (_lines.size() > PLAYERBOT_SOCIAL_PROMPT_CONTEXT_MAX_LINES ||
                               _bytes > PLAYERBOT_SOCIAL_PROMPT_CONTEXT_MAX_BYTES))
    {
        DropFront();
    }
}

void PlayerbotSocialPromptContextBuffer::DropFront()
{
    _bytes -= std::min(_bytes, _lines.front().text.size());
    _lines.pop_front();
}

PlayerbotSocialPromptContextSnapshot PlayerbotSocialBuildPromptContextSnapshot(
    PlayerbotSocialPromptContextBuffer const& buffer, PlayerbotSocialPromptContextConsent const& consents,
    uint64 nowUnixSeconds)
{
    PlayerbotSocialPromptContextSnapshot snapshot;
    if (buffer.LineCount() == 0 || !consents)
        return snapshot;

    for (PlayerbotSocialPromptLine const& line : buffer.Lines())
    {
        if (line.atUnixSeconds > nowUnixSeconds ||
            nowUnixSeconds - line.atUnixSeconds > PLAYERBOT_SOCIAL_PROMPT_CONTEXT_RETENTION_SECONDS)
            continue;

        if (line.speakerIsHuman && !consents(line.speakerGuidCounter))
            continue;

        if (PlayerbotSocialTextLooksSensitive(line.text) || PlayerbotSocialTextLooksLikeAnInstruction(line.text))
        {
            snapshot.lines.clear();
            snapshot.refusal = PlayerbotSocialPromptContextSnapshotRefusal::UnsafeContent;
            return snapshot;
        }

        snapshot.lines.push_back(line);
    }

    if (snapshot.lines.empty())
        return snapshot;

    snapshot.refusal = PlayerbotSocialPromptContextSnapshotRefusal::Accepted;
    return snapshot;
}

std::vector<std::string> PlayerbotSocialBuildNearbyPromptSnapshot(
    std::vector<PlayerbotSocialNearbySnapshotEntry> const& captured,
    PlayerbotSocialPromptContextConsent const& consents)
{
    if (!consents)
        return {};

    std::vector<std::string> names;
    names.reserve(captured.size());
    for (PlayerbotSocialNearbySnapshotEntry const& character : captured)
    {
        if (character.name.empty() || (character.characterIsHuman && !consents(character.characterGuidCounter)))
            continue;

        names.push_back(character.name);
    }

    return names;
}
