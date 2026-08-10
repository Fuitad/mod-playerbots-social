/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#include "PlayerbotSocialFictionalIdentity.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <vector>

#include "PlayerbotSocialPersonality.h"

namespace
{
std::string NormalizeMessage(std::string_view message)
{
    std::vector<std::string> words;
    std::string word;
    auto flushWord = [&]()
    {
        if (word.empty())
            return;

        if (word == "what's")
        {
            words.emplace_back("what");
            words.emplace_back("is");
        }
        else if (word == "where're")
        {
            words.emplace_back("where");
            words.emplace_back("are");
        }
        else if (word == "you're")
        {
            words.emplace_back("you");
            words.emplace_back("are");
        }
        else
            words.push_back(word);

        word.clear();
    };

    for (unsigned char const byte : message)
    {
        if (byte < 0x80 && (std::isalnum(byte) != 0 || byte == '\''))
            word.push_back(static_cast<char>(std::tolower(byte)));
        else
            flushWord();
    }
    flushWord();

    std::string normalized;
    for (std::string const& normalizedWord : words)
    {
        if (!normalized.empty())
            normalized.push_back(' ');
        normalized += normalizedWord;
    }
    return normalized;
}

struct PhraseMatch
{
    std::size_t begin;
    std::size_t end;
};

template <std::size_t Size>
std::vector<PhraseMatch> FindPhrases(std::string const& normalizedMessage,
                                     std::array<std::string_view, Size> const& phrases)
{
    std::vector<PhraseMatch> matches;
    for (std::string_view const phrase : phrases)
    {
        for (std::size_t at = normalizedMessage.find(phrase); at != std::string::npos;
             at = normalizedMessage.find(phrase, at + 1))
        {
            std::size_t const end = at + phrase.size();
            bool const startsAtBoundary = at == 0 || normalizedMessage[at - 1] == ' ';
            bool const endsAtBoundary = end == normalizedMessage.size() || normalizedMessage[end] == ' ';
            if (startsAtBoundary && endsAtBoundary)
                matches.push_back(PhraseMatch{at, end});
        }
    }
    return matches;
}

std::vector<PhraseMatch> FindAgeRequests(std::string const& normalizedMessage)
{
    constexpr std::array<std::string_view, 3> PHRASES = {"how old are you", "what is your age", "your age"};
    return FindPhrases(normalizedMessage, PHRASES);
}

std::vector<PhraseMatch> FindCountryRequests(std::string const& normalizedMessage)
{
    constexpr std::array<std::string_view, 5> PHRASES = {"where are you from", "what country are you from",
                                                         "which country are you from", "what is your home country",
                                                         "your home country"};
    return FindPhrases(normalizedMessage, PHRASES);
}

bool CombinedSeparatorIsAllowed(std::string const& normalizedMessage, PhraseMatch age, PhraseMatch country)
{
    std::size_t const separatorBegin = age.end <= country.begin ? age.end : country.end;
    std::size_t const separatorEnd = age.end <= country.begin ? country.begin : age.begin;
    if (separatorEnd < separatorBegin)
        return false;

    std::string_view const separator =
        std::string_view(normalizedMessage).substr(separatorBegin, separatorEnd - separatorBegin);
    std::size_t const first = separator.find_first_not_of(' ');
    if (first == std::string_view::npos)
        return true;

    std::size_t const last = separator.find_last_not_of(' ');
    return separator.substr(first, last - first + 1) == "and";
}

bool HasAllowedCombinedPair(std::string const& normalizedMessage, std::vector<PhraseMatch> const& ageRequests,
                            std::vector<PhraseMatch> const& countryRequests)
{
    for (PhraseMatch const age : ageRequests)
        for (PhraseMatch const country : countryRequests)
            if (CombinedSeparatorIsAllowed(normalizedMessage, age, country))
                return true;

    return false;
}

std::uint8_t PrivacyScore(PlayerbotSocialChannel channel)
{
    switch (channel)
    {
        case PlayerbotSocialChannel::Party:
            return 50;
        case PlayerbotSocialChannel::Whisper:
            return 100;
        case PlayerbotSocialChannel::General:
        case PlayerbotSocialChannel::Say:
            return 0;
    }

    return 0;
}

std::uint8_t DisclosureWillingness(PlayerbotEffectiveSocialPersona const& persona)
{
    float const trust =
        std::isfinite(persona.relationship.trust) ? std::clamp(persona.relationship.trust, -1.0f, 1.0f) : -1.0f;
    std::uint8_t const trustScore = static_cast<std::uint8_t>((trust + 1.0f) * 50.0f);
    std::uint8_t const disposition = std::min<std::uint8_t>(persona.engagementDisposition, 100);
    std::uint32_t const weighted = 50u * disposition + 30u * trustScore + 20u * PrivacyScore(persona.channel);
    return static_cast<std::uint8_t>(weighted / 100u);
}
}  // namespace

PlayerbotFictionalIdentityPromptContext PlayerbotFictionalIdentity::ResolveRequest(
    PlayerbotFictionalIdentityValue const& identity, std::string_view message, bool addressedDirectly,
    PlayerbotEffectiveSocialPersona const& persona)
{
    PlayerbotFictionalIdentityPromptContext context;
    bool const direct = persona.channel == PlayerbotSocialChannel::Whisper || addressedDirectly;
    if (!direct)
        return context;

    std::string const normalized = NormalizeMessage(message);
    std::vector<PhraseMatch> const ageRequests = FindAgeRequests(normalized);
    std::vector<PhraseMatch> const countryRequests = FindCountryRequests(normalized);
    bool const requestsAge = !ageRequests.empty();
    bool const requestsCountry = !countryRequests.empty();
    if (!requestsAge && !requestsCountry)
        return context;

    if (requestsAge && requestsCountry && !HasAllowedCombinedPair(normalized, ageRequests, countryRequests))
        return context;

    if (requestsAge && requestsCountry)
        context.request = PlayerbotFictionalIdentityRequest::AgeAndHomeCountry;
    else if (requestsAge)
        context.request = PlayerbotFictionalIdentityRequest::Age;
    else
        context.request = PlayerbotFictionalIdentityRequest::HomeCountry;

    std::uint8_t const willingness = DisclosureWillingness(persona);
    if (requestsAge && willingness >= 55)
        context.age = identity.age;
    if (requestsCountry && willingness >= 70)
        context.homeCountry = std::string(identity.homeCountry);
    return context;
}
