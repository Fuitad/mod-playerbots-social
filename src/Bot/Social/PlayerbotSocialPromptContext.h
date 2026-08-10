/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef PLAYERBOTS_PLAYERBOTSOCIALPROMPTCONTEXT_H
#define PLAYERBOTS_PLAYERBOTSOCIALPROMPTCONTEXT_H

#include <cstddef>
#include <deque>
#include <functional>
#include <string>
#include <vector>

#include "Bot/Social/PlayerbotSocialPolicy.h"
#include "Bot/Social/PlayerbotSocialTypes.h"
#include "Define.h"

/*
 * Recent raw chat retained only to help a generation answer its current conversation.
 *
 * This is deliberately separate from PlayerbotSocialExtractionBuffer. Prompt context is consumed
 * while a thread is active, including whispers. Extraction is retained until an idle boundary and
 * never admits whispers. Neither buffer may be read for the other's purpose.
 */
enum class PlayerbotSocialPromptLineRole : uint8
{
    HumanObservation = 0,
    GeneratedReply,
    GeneratedStarter,
    AuthoritativeSource
};

struct PlayerbotSocialPromptLine
{
    std::string eventPublicId;
    PlayerbotSocialPromptLineRole role = PlayerbotSocialPromptLineRole::HumanObservation;
    std::string replyToEventPublicId;
    std::string sourceEventPublicId;
    uint64 speakerGuidCounter = 0;
    std::string speakerName;
    bool speakerIsHuman = false;
    uint64 atUnixSeconds = 0;
    std::string text;
};

enum class PlayerbotSocialPromptContextRejection : uint8
{
    Accepted = 0,
    UnsupportedChannel,
    EmptyText,
    TextTooLong,
    SpeakerNotConsented
};

inline constexpr std::size_t PLAYERBOT_SOCIAL_PROMPT_CONTEXT_MAX_LINES = 12;
inline constexpr std::size_t PLAYERBOT_SOCIAL_PROMPT_CONTEXT_MAX_BYTES = 4096;
inline constexpr std::size_t PLAYERBOT_SOCIAL_PROMPT_CONTEXT_MAX_LINE_BYTES = 512;

// A prompt cannot outlive the thread it describes.
inline constexpr uint64 PLAYERBOT_SOCIAL_PROMPT_CONTEXT_RETENTION_SECONDS = PLAYERBOT_SOCIAL_THREAD_STALE_SECONDS;

class PlayerbotSocialPromptContextBuffer
{
public:
    PlayerbotSocialPromptContextRejection Offer(PlayerbotSocialChannel channel, PlayerbotSocialPromptLine line,
                                                bool speakerConsented, uint64 nowUnixSeconds);

    void ForgetSpeaker(uint64 characterGuidCounter);
    void Expire(uint64 nowUnixSeconds);
    void Clear();

    [[nodiscard]] std::deque<PlayerbotSocialPromptLine> const& Lines() const { return _lines; }
    [[nodiscard]] std::size_t LineCount() const { return _lines.size(); }
    [[nodiscard]] std::size_t ByteCount() const { return _bytes; }

private:
    void EnforceBounds();
    void DropFront();

    std::deque<PlayerbotSocialPromptLine> _lines;
    std::size_t _bytes = 0;
};

enum class PlayerbotSocialPromptContextSnapshotRefusal : uint8
{
    Accepted = 0,
    NothingBuffered,
    UnsafeContent
};

struct PlayerbotSocialPromptContextSnapshot
{
    PlayerbotSocialPromptContextSnapshotRefusal refusal = PlayerbotSocialPromptContextSnapshotRefusal::NothingBuffered;
    std::vector<PlayerbotSocialPromptLine> lines;

    [[nodiscard]] bool Accepted() const { return refusal == PlayerbotSocialPromptContextSnapshotRefusal::Accepted; }
};

using PlayerbotSocialPromptContextConsent = std::function<bool(uint64)>;

// Internal world snapshot. The GUID exists only so human consent can be rechecked at submission;
// PlayerbotSocialBuildNearbyPromptSnapshot removes it before the provider sees the value.
struct PlayerbotSocialNearbySnapshotEntry
{
    uint64 characterGuidCounter = 0;
    std::string name;
    bool characterIsHuman = false;
};

[[nodiscard]] PlayerbotSocialPromptContextSnapshot PlayerbotSocialBuildPromptContextSnapshot(
    PlayerbotSocialPromptContextBuffer const& buffer, PlayerbotSocialPromptContextConsent const& consents,
    uint64 nowUnixSeconds);

[[nodiscard]] std::vector<std::string> PlayerbotSocialBuildNearbyPromptSnapshot(
    std::vector<PlayerbotSocialNearbySnapshotEntry> const& captured,
    PlayerbotSocialPromptContextConsent const& consents);

#endif
