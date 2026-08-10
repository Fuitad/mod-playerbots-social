/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#include "PlayerbotSocialControl.h"

namespace
{
/*
 * One fixed refusal for every reason. Deliberately says nothing beyond the fact of refusal: no
 * echo of what was presented, no length, and nothing that distinguishes an unconfigured
 * deployment from a wrong token.
 */
constexpr char const* CONTROL_UNAUTHORIZED = "error,unauthorized";

/*
 * Compares without an early return, so the time taken does not depend on how many leading
 * characters matched.
 *
 * The length difference is folded into the same accumulator rather than being checked first,
 * because a length check that returns early is itself the oracle: it separates "wrong length"
 * from "right length, wrong content" by timing alone. Reading the presented token modulo its
 * own length keeps the loop over the configured length in every case, and a length mismatch is
 * already guaranteed to fail through the accumulator seed.
 */
/*
 * Compares over a FIXED width, so the work depends on neither input.
 *
 * A loop over the configured token's length leaks that length: an observer timing refusals
 * learns how long the deployment's secret is, which is exactly the kind of configuration detail
 * the single fixed refusal string exists to withhold. Running the same number of iterations
 * whatever the inputs closes it, and a mismatch in length is folded into the accumulator so a
 * short token can never match a long one by running out of bytes.
 */
bool FixedWidthEquals(std::string_view presented, std::string_view configured)
{
    unsigned char difference = static_cast<unsigned char>(presented.size() != configured.size());

    for (std::size_t i = 0; i < PLAYERBOT_SOCIAL_CONTROL_COMPARE_WIDTH; ++i)
    {
        unsigned char const presentedByte = i < presented.size() ? static_cast<unsigned char>(presented[i]) : 0u;
        unsigned char const configuredByte = i < configured.size() ? static_cast<unsigned char>(configured[i]) : 0u;

        difference |= static_cast<unsigned char>(presentedByte ^ configuredByte);
    }

    return difference == 0;
}

/*
 * What an unusable configured token is compared against instead of returning early.
 *
 * Returning before comparing would make "no token configured" and "token too short" measurably
 * cheaper than a mismatch, so a caller could tell an unconfigured deployment from one that
 * simply refused it. It is a run of high bytes rather than anything guessable, and the caller
 * cannot reach Granted through it in any case: the reason is decided before the comparison's
 * result is consulted.
 */
constexpr char UNUSABLE_TOKEN_PLACEHOLDER[] = "\xff\xff\xff\xff\xff\xff\xff\xff\xff\xff\xff\xff\xff\xff\xff\xff";
}  // namespace

PlayerbotSocialControlAuth PlayerbotSocialControlAuthenticate(std::string_view presented, std::string_view configured)
{
    bool const unconfigured = configured.empty();
    bool const tooShort = !unconfigured && configured.size() < PLAYERBOT_SOCIAL_CONTROL_MIN_TOKEN_LENGTH;
    bool const tooLong = configured.size() > PLAYERBOT_SOCIAL_CONTROL_MAX_TOKEN_LENGTH;
    bool const unusable = unconfigured || tooShort || tooLong;

    /*
     * Compared on EVERY path, including the two where the configured token is unusable and the
     * answer is already known. An early return there would make those refusals measurably cheaper
     * than a mismatch, so a caller could learn from timing alone whether the deployment has a token
     * at all: a configuration detail the fixed refusal string is meant to withhold.
     */
    std::string_view const against =
        unusable ? std::string_view(UNUSABLE_TOKEN_PLACEHOLDER, PLAYERBOT_SOCIAL_CONTROL_MIN_TOKEN_LENGTH) : configured;
    bool const matched = FixedWidthEquals(presented, against);

    /*
     * The reason is decided from the CONFIGURATION, never from the comparison, so guessing the
     * placeholder above cannot authenticate against a deployment that configured no token. Order
     * matters for the log: an operator is told controls are off rather than that someone guessed
     * wrong. The wire response is identical either way.
     */
    if (unconfigured)
        return PlayerbotSocialControlAuth::NoTokenConfigured;

    if (tooShort)
        return PlayerbotSocialControlAuth::TokenTooShort;

    if (tooLong)
        return PlayerbotSocialControlAuth::TokenTooLong;

    return matched ? PlayerbotSocialControlAuth::Granted : PlayerbotSocialControlAuth::TokenMismatch;
}

std::string PlayerbotSocialControlAuthResponse(PlayerbotSocialControlAuth outcome)
{
    // Granted has no refusal text. It returns empty rather than something cheerful, so a caller that
    // writes this straight to the socket cannot accidentally announce a successful authentication
    // before the operation it authorized has actually run.
    if (outcome == PlayerbotSocialControlAuth::Granted)
        return std::string();

    return CONTROL_UNAUTHORIZED;
}

char const* PlayerbotSocialControlAuthName(PlayerbotSocialControlAuth outcome)
{
    switch (outcome)
    {
        case PlayerbotSocialControlAuth::Granted:
            return "granted";
        case PlayerbotSocialControlAuth::NoTokenConfigured:
            return "no_token_configured";
        case PlayerbotSocialControlAuth::TokenTooShort:
            return "token_too_short";
        case PlayerbotSocialControlAuth::TokenTooLong:
            return "token_too_long";
        case PlayerbotSocialControlAuth::TokenMismatch:
            return "token_mismatch";
    }

    return "unknown";
}

// The request vocabulary ------------------------------------------------------------------------

namespace
{
struct ControlOperationName
{
    char const* name;
    PlayerbotSocialControlOperation operation;
};

/*
 * The single table of what an operator may ask for. The parse, the name lookup, and anything
 * later that needs to enumerate the vocabulary all read this, so a new control cannot be added
 * to one of them and forgotten in the others.
 */
constexpr ControlOperationName CONTROL_OPERATIONS[] = {
    {"pause", PlayerbotSocialControlOperation::Pause},
    {"density", PlayerbotSocialControlOperation::Density},
    {"channel_general", PlayerbotSocialControlOperation::ChannelGeneral},
    {"channel_say", PlayerbotSocialControlOperation::ChannelSay},
    {"channel_party", PlayerbotSocialControlOperation::ChannelParty},
    {"channel_whisper", PlayerbotSocialControlOperation::ChannelWhisper},
    {"reset_memory", PlayerbotSocialControlOperation::ResetMemory},
    {"reset_relationship", PlayerbotSocialControlOperation::ResetRelationship},
    {"acknowledge_case", PlayerbotSocialControlOperation::AcknowledgeCase}};

bool FindOperation(std::string_view name, PlayerbotSocialControlOperation& operation)
{
    // Compared exactly, including case. A caller sending PAUSE is not making a typo this should
    // helpfully correct; it is a caller this boundary has no reason to accommodate.
    for (ControlOperationName const& candidate : CONTROL_OPERATIONS)
    {
        if (name == candidate.name)
        {
            operation = candidate.operation;
            return true;
        }
    }

    return false;
}

// Exactly "0" or "1". Not "true", not "yes", not a leading-zero variant: one spelling per value
// means a control cannot be sent in a form the operator believes means something else.
bool ParseFlag(std::string_view value, bool& flag)
{
    if (value == "0")
    {
        flag = false;
        return true;
    }

    if (value == "1")
    {
        flag = true;
        return true;
    }

    return false;
}

// Strict, unlike the configuration parser. An unrecognized density from a socket is refused
// rather than resolved to Normal, because silently applying a different control than the one
// requested is the failure this whole boundary exists to prevent.
bool ParseDensityStrict(std::string_view value, PlayerbotSocialDensityProfile& density)
{
    if (value == "quiet")
        density = PlayerbotSocialDensityProfile::Quiet;
    else if (value == "normal")
        density = PlayerbotSocialDensityProfile::Normal;
    else if (value == "lively")
        density = PlayerbotSocialDensityProfile::Lively;
    else
        return false;

    return true;
}

/*
 * The shape of an opaque public identifier, answered by the frozen contract rather than by a
 * second description of it here.
 *
 * This used to spell out an 8-4-4-4-12 dashed UUID, which no social table has ever stored: every
 * identity is a kind prefix, an underscore, and PLAYERBOT_SOCIAL_PUBLIC_ID_BODY_LENGTH lowercase
 * hex characters (PlayerbotSocialTypes.h). The two shapes are the same LENGTH, so the bound
 * above still held while every real identifier was refused as an invalid value and every
 * accepted one named a row that could not exist. Delegating removes the copy that drifted.
 *
 * The kind is required, not incidental. The world thread looks a subject up in exactly one
 * table, so a relationship id sent to reset_memory would come back SubjectNotFound, which reads
 * as "already gone" rather than "you named the wrong kind of thing".
 *
 * Validated by shape rather than by asking the database, because this runs on the socket thread
 * where there is no database access, and because a malformed id should never become a query in
 * the first place. Whether the id EXISTS is the world thread's question; whether it could
 * possibly be an id is this one's.
 */
bool IsPublicId(PlayerbotSocialIdKind kind, std::string_view value)
{
    return PlayerbotSocialPublicIdIsValid(kind, value);
}

/*
 * Which identity namespace each subject bearing operation names.
 *
 * Written as its own total function rather than inline beside the parse, so a control added to
 * the vocabulary table above cannot quietly inherit whichever kind happened to be in scope. The
 * operations that carry no subject answer false rather than a plausible kind, for the same
 * reason PlayerbotSocialControlChannelFor leaves its out parameter untouched.
 */
bool SubjectKindFor(PlayerbotSocialControlOperation operation, PlayerbotSocialIdKind& kind)
{
    switch (operation)
    {
        case PlayerbotSocialControlOperation::ResetMemory:
            kind = PlayerbotSocialIdKind::Memory;
            return true;
        case PlayerbotSocialControlOperation::ResetRelationship:
            kind = PlayerbotSocialIdKind::Relationship;
            return true;
        case PlayerbotSocialControlOperation::AcknowledgeCase:
            kind = PlayerbotSocialIdKind::ModerationCase;
            return true;
        case PlayerbotSocialControlOperation::Pause:
        case PlayerbotSocialControlOperation::Density:
        case PlayerbotSocialControlOperation::ChannelGeneral:
        case PlayerbotSocialControlOperation::ChannelSay:
        case PlayerbotSocialControlOperation::ChannelParty:
        case PlayerbotSocialControlOperation::ChannelWhisper:
            return false;
    }

    return false;
}
}  // namespace

char const* PlayerbotSocialControlOperationName(PlayerbotSocialControlOperation operation)
{
    for (ControlOperationName const& candidate : CONTROL_OPERATIONS)
    {
        if (candidate.operation == operation)
            return candidate.name;
    }

    return "unknown";
}

bool PlayerbotSocialControlParseIsRefusal(PlayerbotSocialControlParse outcome)
{
    return outcome != PlayerbotSocialControlParse::Accepted;
}

char const* PlayerbotSocialControlParseName(PlayerbotSocialControlParse outcome)
{
    switch (outcome)
    {
        case PlayerbotSocialControlParse::Accepted:
            return "accepted";
        case PlayerbotSocialControlParse::MalformedRequest:
            return "malformed_request";
        case PlayerbotSocialControlParse::UnknownOperation:
            return "unknown_operation";
        case PlayerbotSocialControlParse::InvalidValue:
            return "invalid_value";
        case PlayerbotSocialControlParse::RequestTooLong:
            return "request_too_long";
    }

    return "unknown";
}

PlayerbotSocialControlParseResult PlayerbotSocialControlParseRequest(std::string_view body)
{
    PlayerbotSocialControlParseResult result;

    // Length first, before anything scans or copies, so the cost of refusing abuse does not scale
    // with the size of the abuse.
    if (body.size() > PLAYERBOT_SOCIAL_CONTROL_MAX_REQUEST_BYTES)
    {
        result.outcome = PlayerbotSocialControlParse::RequestTooLong;
        return result;
    }

    std::size_t const separator = body.find(',');
    if (separator == std::string_view::npos || separator == 0)
    {
        result.outcome = PlayerbotSocialControlParse::MalformedRequest;
        return result;
    }

    std::string_view const name = body.substr(0, separator);
    std::string_view const value = body.substr(separator + 1);

    PlayerbotSocialControlOperation operation = PlayerbotSocialControlOperation::Pause;
    if (!FindOperation(name, operation))
    {
        result.outcome = PlayerbotSocialControlParse::UnknownOperation;
        return result;
    }

    result.request.operation = operation;

    switch (operation)
    {
        case PlayerbotSocialControlOperation::Pause:
        case PlayerbotSocialControlOperation::ChannelGeneral:
        case PlayerbotSocialControlOperation::ChannelSay:
        case PlayerbotSocialControlOperation::ChannelParty:
        case PlayerbotSocialControlOperation::ChannelWhisper:
            if (!ParseFlag(value, result.request.flag))
            {
                result.outcome = PlayerbotSocialControlParse::InvalidValue;
                return result;
            }
            break;

        case PlayerbotSocialControlOperation::Density:
            if (!ParseDensityStrict(value, result.request.density))
            {
                result.outcome = PlayerbotSocialControlParse::InvalidValue;
                return result;
            }
            break;

        case PlayerbotSocialControlOperation::ResetMemory:
        case PlayerbotSocialControlOperation::ResetRelationship:
        case PlayerbotSocialControlOperation::AcknowledgeCase:
        {
            PlayerbotSocialIdKind kind = PlayerbotSocialIdKind::Memory;

            // A subject bearing operation with no kind is a vocabulary that grew without this switch
            // growing with it. Refused rather than validated against the initializer above.
            if (!SubjectKindFor(operation, kind) || !IsPublicId(kind, value))
            {
                result.outcome = PlayerbotSocialControlParse::InvalidValue;
                return result;
            }

            result.request.subject = std::string(value);
            break;
        }
    }

    result.outcome = PlayerbotSocialControlParse::Accepted;
    return result;
}

bool PlayerbotSocialControlChannelFor(PlayerbotSocialControlOperation operation, PlayerbotSocialChannel& channel)
{
    switch (operation)
    {
        case PlayerbotSocialControlOperation::ChannelGeneral:
            channel = PlayerbotSocialChannel::General;
            return true;
        case PlayerbotSocialControlOperation::ChannelSay:
            channel = PlayerbotSocialChannel::Say;
            return true;
        case PlayerbotSocialControlOperation::ChannelParty:
            channel = PlayerbotSocialChannel::Party;
            return true;
        case PlayerbotSocialControlOperation::ChannelWhisper:
            channel = PlayerbotSocialChannel::Whisper;
            return true;
        case PlayerbotSocialControlOperation::Pause:
        case PlayerbotSocialControlOperation::Density:
        case PlayerbotSocialControlOperation::ResetMemory:
        case PlayerbotSocialControlOperation::ResetRelationship:
        case PlayerbotSocialControlOperation::AcknowledgeCase:
            // Left untouched rather than set to a channel the caller might then act on.
            return false;
    }

    return false;
}

// The command server line -------------------------------------------------------------------------

PlayerbotSocialControlLine PlayerbotSocialControlSplitLine(std::string_view line)
{
    PlayerbotSocialControlLine split;

    if (line.size() < PLAYERBOT_SOCIAL_CONTROL_PREFIX.size() ||
        line.substr(0, PLAYERBOT_SOCIAL_CONTROL_PREFIX.size()) != PLAYERBOT_SOCIAL_CONTROL_PREFIX)
    {
        // Not ours. Every other request the command port already answers reaches its handler
        // untouched, which is what keeps telemetry and inspect working exactly as they did.
        return split;
    }

    split.isControl = true;

    /*
     * Bounded before anything is copied. A caller that prefixes an arbitrarily long line would
     * otherwise make the server allocate a token and a body out of it just by getting the first
     * fourteen bytes right.
     */
    if (line.size() > PLAYERBOT_SOCIAL_CONTROL_MAX_LINE_BYTES)
    {
        split.oversized = true;
        return split;
    }

    std::string_view remainder = line.substr(PLAYERBOT_SOCIAL_CONTROL_PREFIX.size());

    /*
     * Split once more and no further. Everything after the token is the parser's business, so a
     * value containing a comma stays intact rather than being silently truncated here.
     */
    std::size_t const separator = remainder.find(',');
    if (separator == std::string_view::npos)
    {
        split.token = std::string(remainder);
        return split;
    }

    split.token = std::string(remainder.substr(0, separator));
    split.body = std::string(remainder.substr(separator + 1));
    return split;
}

std::string PlayerbotSocialControlOutcomeResponse(PlayerbotSocialControlOutcome outcome)
{
    /*
     * One line per outcome, all distinct, and every non-applied answer prefixed "error," so a caller
     * can tell a change from a refusal without knowing the vocabulary. None of them reveals anything
     * about the deployment: an operator who is authenticated already knows what they asked for, and
     * one who is not never reaches this function.
     */
    switch (outcome)
    {
        case PlayerbotSocialControlOutcome::Applied:
            return "ok,applied";
        case PlayerbotSocialControlOutcome::SubjectNotFound:
            return "error,subject_not_found";
        case PlayerbotSocialControlOutcome::QueueFull:
            return "error,busy";
        case PlayerbotSocialControlOutcome::TimedOut:
            return "error,timed_out";
        case PlayerbotSocialControlOutcome::ShuttingDown:
            return "error,shutting_down";
        case PlayerbotSocialControlOutcome::InternalError:
            return "error,internal";
        case PlayerbotSocialControlOutcome::Pending:
            /*
             * Never sent while the dispatch is behaving: a caller either gets an answer or is told it
             * abandoned the request. Distinct anyway, so if one ever does reach the wire it reads as
             * the bug it is rather than as a plausible refusal.
             */
            return "error,no_answer";
    }

    return "error,internal";
}

// Cross-thread dispatch --------------------------------------------------------------------------

char const* PlayerbotSocialControlOutcomeName(PlayerbotSocialControlOutcome outcome)
{
    switch (outcome)
    {
        case PlayerbotSocialControlOutcome::Pending:
            return "pending";
        case PlayerbotSocialControlOutcome::Applied:
            return "applied";
        case PlayerbotSocialControlOutcome::SubjectNotFound:
            return "subject_not_found";
        case PlayerbotSocialControlOutcome::QueueFull:
            return "queue_full";
        case PlayerbotSocialControlOutcome::TimedOut:
            return "timed_out";
        case PlayerbotSocialControlOutcome::ShuttingDown:
            return "shutting_down";
        case PlayerbotSocialControlOutcome::InternalError:
            return "internal_error";
    }

    return "unknown_outcome";
}

bool PlayerbotSocialControlOutcomeChangedState(PlayerbotSocialControlOutcome outcome)
{
    // Enumerated rather than defaulted, so a new outcome has to state its own answer instead of
    // inheriting "changed nothing" from a fallthrough and being retried by a caller that should not.
    switch (outcome)
    {
        case PlayerbotSocialControlOutcome::Applied:
            return true;
        case PlayerbotSocialControlOutcome::Pending:
        case PlayerbotSocialControlOutcome::SubjectNotFound:
        case PlayerbotSocialControlOutcome::QueueFull:
        case PlayerbotSocialControlOutcome::TimedOut:
        case PlayerbotSocialControlOutcome::ShuttingDown:
        case PlayerbotSocialControlOutcome::InternalError:
            return false;
    }

    return false;
}

bool PlayerbotSocialControlResult::TryClaim()
{
    std::lock_guard<std::mutex> lock(_mutex);
    if (_abandoned || _claimed)
        return false;

    _claimed = true;
    return true;
}

bool PlayerbotSocialControlResult::Abandon()
{
    std::lock_guard<std::mutex> lock(_mutex);
    // Completed is checked as well as claimed: a request that finished between the caller's last
    // look and this call has a real answer waiting, and discarding it for a timeout would report a
    // no-state-change error for a mutation that changed state.
    if (_claimed || _completed)
        return false;

    _abandoned = true;
    return true;
}

void PlayerbotSocialControlResult::Complete(PlayerbotSocialControlOutcome outcome)
{
    {
        std::lock_guard<std::mutex> lock(_mutex);
        if (_completed)
            return;

        _completed = true;
        _outcome = outcome;
    }

    _ready.notify_all();
}

bool PlayerbotSocialControlResult::IsCompleted() const
{
    std::lock_guard<std::mutex> lock(_mutex);
    return _completed;
}

std::optional<PlayerbotSocialControlOutcome> PlayerbotSocialControlResult::Wait(std::chrono::milliseconds timeout)
{
    std::unique_lock<std::mutex> lock(_mutex);
    if (!_ready.wait_for(lock, timeout, [this] { return _completed; }))
        return std::nullopt;

    return _outcome;
}

PlayerbotSocialControlSlot::~PlayerbotSocialControlSlot() { Release(); }

PlayerbotSocialControlSlot& PlayerbotSocialControlSlot::operator=(PlayerbotSocialControlSlot&& other) noexcept
{
    if (this == &other)
        return *this;

    // Whatever this already held is given back before taking the other's, so an assignment cannot
    // quietly drop a slot and leak it out of the bound for the life of the process.
    Release();
    _admission = other._admission;
    other._admission = nullptr;
    return *this;
}

void PlayerbotSocialControlSlot::Release()
{
    if (!_admission)
        return;

    // Cleared before releasing, so a slot can never be returned twice however this is reached.
    PlayerbotSocialControlAdmission* const admission = _admission;
    _admission = nullptr;
    admission->Release();
}

bool PlayerbotSocialControlAdmission::TryAdmit()
{
    std::lock_guard<std::mutex> lock(_mutex);
    if (_shuttingDown || _inFlight >= PLAYERBOT_SOCIAL_CONTROL_QUEUE_CAPACITY)
        return false;

    ++_inFlight;
    return true;
}

void PlayerbotSocialControlAdmission::Release()
{
    std::lock_guard<std::mutex> lock(_mutex);
    if (_inFlight)
        --_inFlight;
}

void PlayerbotSocialControlAdmission::Shutdown()
{
    std::lock_guard<std::mutex> lock(_mutex);
    _shuttingDown = true;
}

bool PlayerbotSocialControlAdmission::IsShuttingDown() const
{
    std::lock_guard<std::mutex> lock(_mutex);
    return _shuttingDown;
}

std::size_t PlayerbotSocialControlAdmission::InFlight() const
{
    std::lock_guard<std::mutex> lock(_mutex);
    return _inFlight;
}
