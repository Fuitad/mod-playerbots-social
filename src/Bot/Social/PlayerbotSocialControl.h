/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef PLAYERBOTS_PLAYERBOTSOCIALCONTROL_H
#define PLAYERBOTS_PLAYERBOTSOCIALCONTROL_H

#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

#include "Bot/Social/PlayerbotSocialTypes.h"
#include "Define.h"

/*
 * Authenticated administrative control of the social feature.
 *
 * Lives apart from PlayerbotSocialMgr because a mutation arriving from a socket is a different
 * problem from the conversation state it mutates: it crosses a thread boundary, it is the one
 * untrusted input the feature accepts, and it has to answer a caller that may already have given
 * up. Keeping it here means the decisions can be tested as values, and it keeps a manager that is
 * already past three thousand lines from absorbing a second responsibility.
 */

// Why a control request was refused before it was ever looked at.
enum class PlayerbotSocialControlAuth : uint8
{
    Granted = 0,
    NoTokenConfigured,  // The deployment never opted in. Not a mismatch: there is nothing to match.
    TokenTooShort,      // Configured, but too short to be a secret against a caller that can retry.
    TokenTooLong,       // Configured beyond the fixed comparison bound, so it cannot be authenticated.
    TokenMismatch
};

/*
 * The shortest configured token this accepts.
 *
 * Sixteen characters rather than a token of any length, because the loopback bind is a second line
 * of defence and not a reason to accept a weak first one. Anything shorter is refused as a
 * configuration problem, so an operator is told the token is unusable rather than left to discover
 * that the correct token does not work.
 */
inline constexpr std::size_t PLAYERBOT_SOCIAL_CONTROL_MIN_TOKEN_LENGTH = 16;

/*
 * How many bytes every token comparison walks, whatever it is comparing.
 *
 * Fixed rather than following either input, because a loop over the configured token's length leaks
 * that length to anyone who can time a refusal. Above any sane token and cheap enough that the extra
 * iterations cost nothing on a path a human triggers.
 */
inline constexpr std::size_t PLAYERBOT_SOCIAL_CONTROL_COMPARE_WIDTH = 256;

// A configured token must fit entirely inside the fixed comparison window.
inline constexpr std::size_t PLAYERBOT_SOCIAL_CONTROL_MAX_TOKEN_LENGTH = PLAYERBOT_SOCIAL_CONTROL_COMPARE_WIDTH;

/*
 * Decides whether a presented token may mutate anything.
 *
 * Fails closed on an absent or unusable configured token, so the default posture of a deployment
 * that has not thought about this is "no remote mutation" rather than "remote mutation by anyone
 * who can reach the port".
 *
 * The comparison is constant time in the configured token's length. Not because a timing attack
 * over loopback is likely, but because the alternative is an early return whose cost is exactly the
 * length of the matching prefix, and that is a real oracle that costs nothing to close.
 */
[[nodiscard]] PlayerbotSocialControlAuth PlayerbotSocialControlAuthenticate(std::string_view presented,
                                                                            std::string_view configured);

/*
 * What a refused caller is told, which is as little as possible.
 *
 * One fixed string for every refusal. It cannot echo the presented token, say how much of it
 * matched, or reveal the configured token's length, and the three refusal reasons are
 * indistinguishable from each other: a caller learning "the token is too short" would be learning
 * the deployment's configuration from outside the deployment. The reason is for the log, which is
 * already inside the trust boundary.
 */
[[nodiscard]] std::string PlayerbotSocialControlAuthResponse(PlayerbotSocialControlAuth outcome);

// Stable label for the log and for operator facing surfaces. Never reword an existing one.
[[nodiscard]] char const* PlayerbotSocialControlAuthName(PlayerbotSocialControlAuth outcome);

// The request vocabulary ------------------------------------------------------------------------

/*
 * Every mutation an authenticated operator may perform, and there are no others.
 *
 * A closed set rather than a passthrough to some generic setter: the operations here are exactly
 * the ones Definition of Done 3 requires to survive a restart, and anything not named is refused at
 * the parse boundary rather than reaching the world thread to be sorted out there.
 */
enum class PlayerbotSocialControlOperation : uint8
{
    Pause = 0,
    Density,
    ChannelGeneral,
    ChannelSay,
    ChannelParty,
    ChannelWhisper,
    ResetMemory,
    ResetRelationship,
    AcknowledgeCase
};

[[nodiscard]] char const* PlayerbotSocialControlOperationName(PlayerbotSocialControlOperation operation);

/*
 * The channel a per-channel operation acts on, or false for the operations that act on none.
 *
 * A function rather than four sites writing out the correspondence, because "ChannelParty toggles
 * Party" is exactly the kind of pairing that survives a transposition unnoticed: each toggle would
 * still appear to work, on the wrong surface.
 */
[[nodiscard]] bool PlayerbotSocialControlChannelFor(PlayerbotSocialControlOperation operation,
                                                    PlayerbotSocialChannel& channel);

// Why a request was refused, or that it was not. Every refusal is a no-state-change error by
// construction: parsing happens before anything is queued, so nothing can have been applied yet.
enum class PlayerbotSocialControlParse : uint8
{
    Accepted = 0,
    MalformedRequest,  // Not "operation,value" at all.
    UnknownOperation,  // Well formed, but names something outside the vocabulary above.
    InvalidValue,      // Known operation, value it cannot mean.
    RequestTooLong
};

[[nodiscard]] bool PlayerbotSocialControlParseIsRefusal(PlayerbotSocialControlParse outcome);
[[nodiscard]] char const* PlayerbotSocialControlParseName(PlayerbotSocialControlParse outcome);

/*
 * The longest request accepted, checked before anything is parsed.
 *
 * The socket reads a line, and a line is whatever the caller chooses to send. Without a bound, a
 * caller that never sends a separator makes the server buffer and scan an arbitrary amount of
 * memory. The longest legitimate request is an operation name plus a 36 character opaque id, so
 * this is generous by roughly a factor of two and still nowhere near expensive to refuse.
 */
inline constexpr std::size_t PLAYERBOT_SOCIAL_CONTROL_MAX_REQUEST_BYTES = 128;

// The length of an opaque public identifier, which is the only identity a control may name. Integer
// row ids and character GUIDs stay inside the worldserver and are never accepted from a caller.
inline constexpr std::size_t PLAYERBOT_SOCIAL_CONTROL_PUBLIC_ID_LENGTH = 36;

/*
 * One parsed mutation, immutable once built.
 *
 * All three value carriers are present rather than a union, because the struct is small, it crosses
 * a thread boundary by copy, and a variant here would buy nothing but a way to read the wrong arm.
 * Only the field the operation uses is meaningful; the others hold their defaults.
 */
struct PlayerbotSocialControlRequest
{
    PlayerbotSocialControlOperation operation = PlayerbotSocialControlOperation::Pause;
    bool flag = false;
    PlayerbotSocialDensityProfile density = PlayerbotSocialDensityProfile::Normal;
    std::string subject;
};

struct PlayerbotSocialControlParseResult
{
    PlayerbotSocialControlParse outcome = PlayerbotSocialControlParse::MalformedRequest;
    PlayerbotSocialControlRequest request;
};

/*
 * Turns an authenticated request body into a mutation, or refuses it.
 *
 * Strict everywhere: exact lowercase operation names, exact values, and an opaque id that must have
 * the shape of one. Nothing is clamped or defaulted, because a control that quietly does something
 * adjacent to what was asked is worse than one that refuses.
 */
[[nodiscard]] PlayerbotSocialControlParseResult PlayerbotSocialControlParseRequest(std::string_view body);

// Outcomes ----------------------------------------------------------------------------------------

/*
 * What became of a request. Only Applied represents a state change; every other value is safe for a
 * caller to retry, which is what makes them usable as the socket's answer.
 */
enum class PlayerbotSocialControlOutcome : uint8
{
    Pending = 0,      // No answer yet. Never sent to a caller; the initial state of the shared slot.
    Applied,          // The one outcome that changed something.
    SubjectNotFound,  // Well formed opaque id that names nothing. Decided on the world thread.
    QueueFull,
    TimedOut,  // The caller abandoned the request first, so the mutation never ran.
    ShuttingDown,
    InternalError  // The mutation threw. Reported rather than swallowed into a false success.
};

[[nodiscard]] char const* PlayerbotSocialControlOutcomeName(PlayerbotSocialControlOutcome outcome);
[[nodiscard]] bool PlayerbotSocialControlOutcomeChangedState(PlayerbotSocialControlOutcome outcome);

// The command server line -------------------------------------------------------------------------

/*
 * The wire prefix that marks a line as an administrative control.
 *
 * Distinct from the existing "social" chat command on purpose. The command port's other requests are
 * "<command>,<guid>", so a prefix that could also read as one would make an operator's mistyped
 * control reach the bot command handler and come back as "invalid guid": a refusal about the wrong
 * thing, which is the hardest kind to debug from outside the server.
 */
inline constexpr std::string_view PLAYERBOT_SOCIAL_CONTROL_PREFIX = "socialcontrol,";

/*
 * One command port line, split far enough to authenticate it and no further.
 *
 * The token is separated here rather than inside the parser, so nothing downstream ever holds the
 * secret and no refusal can echo it by accident.
 */
struct PlayerbotSocialControlLine
{
    // True when this line is a control, which is also what says the command port must not pass it on
    // to the bot command handler. A malformed control is still a control.
    bool isControl = false;

    // True when the line exceeded the request bound, checked before anything was copied out of it.
    bool oversized = false;

    std::string token;

    // Everything after the token, commas included, for the parser to make sense of.
    std::string body;
};

/*
 * The longest whole line accepted, which is a DIFFERENT bound from the body's.
 *
 * The body bound above sizes a request: an operation name and a 36 character opaque id, and nothing
 * legitimate comes close to it. This one also has to carry the prefix and the operator's token, and
 * a token has no natural length. Reusing the body's 128 here would mean a deployment that chose a
 * long, strong token had every control refused as oversized and answered "unauthorized", which is
 * the most misleading refusal this surface could give: the token would be the problem, the message
 * would say so, and the operator would go and replace a perfectly good secret.
 *
 * Still small enough that refusing one costs nothing.
 */
inline constexpr std::size_t PLAYERBOT_SOCIAL_CONTROL_MAX_LINE_BYTES = 512;

[[nodiscard]] PlayerbotSocialControlLine PlayerbotSocialControlSplitLine(std::string_view line);

// What the command port answers for an outcome. Stable: Medivh reads these.
[[nodiscard]] std::string PlayerbotSocialControlOutcomeResponse(PlayerbotSocialControlOutcome outcome);

// Cross-thread dispatch --------------------------------------------------------------------------

/*
 * How many control requests may be in flight at once.
 *
 * Small on purpose. These are operator actions, not traffic: a human toggling a channel produces
 * one, and anything approaching this number means either a script in a loop or an attempt to make
 * the server buffer work.
 *
 * This is a bound OF ITS OWN, on top of the world thread queue's much larger one, because that
 * queue is shared with group, LFG, guild, and battleground work coming from the map threads. A
 * socket that could fill it would starve the bots, so controls get a small reservation instead of
 * competing for the whole thing.
 */
inline constexpr std::size_t PLAYERBOT_SOCIAL_CONTROL_QUEUE_CAPACITY = 32;

// How long a caller waits for the world thread before abandoning its request.
inline constexpr uint64 PLAYERBOT_SOCIAL_CONTROL_TIMEOUT_SECONDS = 5;

/*
 * Extra wait granted when the deadline expires while the world thread is already inside the
 * mutation. That work cannot be unwound, so reporting what it actually did beats reporting a
 * timeout for a state change that happened.
 */
inline constexpr uint64 PLAYERBOT_SOCIAL_CONTROL_COMPLETION_GRACE_MS = 250;

/*
 * The answer slot shared by the socket thread and the world thread.
 *
 * The rule that shapes it: TimedOut is a no-state-change error, so it has to be a promise rather
 * than a guess. A caller reaching its deadline abandons the request, and an abandoned request is
 * never executed, so an operator who retries after a slow tick applies the control once instead of
 * twice. Abandonment and execution claim the same state under one lock, so exactly one of them
 * wins: either the mutation runs and the caller hears the real outcome, or it never runs and the
 * caller is told so.
 *
 * Deliberately the same shape as PlayerbotVerificationResult, which solved this for the MCP
 * verification path. Two different answers to one concurrency question in one module would be a
 * standing invitation to reason about the wrong one.
 */
class PlayerbotSocialControlAdmission;

/*
 * One admitted slot, given back exactly once when its owner is destroyed.
 *
 * Exists so the bound can count work outstanding in the SYSTEM rather than callers currently
 * waiting. A caller that times out is finished with its request, but the request itself is still
 * queued for the world thread; returning the slot then would let someone sending faster than the
 * world drains hold far more than the bound at once.
 *
 * Movable and not copyable, because a copy would be a second claim on one admission and would give
 * it back twice, drifting the bound upward until it stopped bounding anything.
 */
class PlayerbotSocialControlSlot
{
public:
    PlayerbotSocialControlSlot() = default;
    explicit PlayerbotSocialControlSlot(PlayerbotSocialControlAdmission& admission) : _admission(&admission) {}
    ~PlayerbotSocialControlSlot();

    PlayerbotSocialControlSlot(PlayerbotSocialControlSlot&& other) noexcept : _admission(other._admission)
    {
        other._admission = nullptr;
    }

    PlayerbotSocialControlSlot& operator=(PlayerbotSocialControlSlot&& other) noexcept;

    PlayerbotSocialControlSlot(PlayerbotSocialControlSlot const&) = delete;
    PlayerbotSocialControlSlot& operator=(PlayerbotSocialControlSlot const&) = delete;

private:
    void Release();

    PlayerbotSocialControlAdmission* _admission = nullptr;
};

class PlayerbotSocialControlResult
{
public:
    /*
     * Ties an admitted slot to this answer's lifetime.
     *
     * The answer is shared by the waiting caller and the queued work, so the slot comes back when
     * the LAST of them is done with it. That is the only moment at which the request is genuinely no
     * longer outstanding.
     */
    void HoldSlot(PlayerbotSocialControlSlot slot) { _slot = std::move(slot); }

    // World thread. False when the caller already abandoned, meaning: do not run the mutation.
    [[nodiscard]] bool TryClaim();

    // Socket thread. False when execution already started, so the caller waits the grace out.
    bool Abandon();

    // Records the outcome and wakes the waiter. A second call is discarded: the first answer is the
    // one the caller was given.
    void Complete(PlayerbotSocialControlOutcome outcome);

    [[nodiscard]] bool IsCompleted() const;

    // Blocks until the outcome arrives or the timeout expires. Empty means no answer, which is
    // never the same thing as an outcome value.
    [[nodiscard]] std::optional<PlayerbotSocialControlOutcome> Wait(std::chrono::milliseconds timeout);

private:
    mutable std::mutex _mutex;
    std::condition_variable _ready;
    bool _completed = false;
    bool _abandoned = false;
    bool _claimed = false;
    PlayerbotSocialControlOutcome _outcome = PlayerbotSocialControlOutcome::Pending;

    // Not guarded by the mutex: it is written once at construction and read only by the destructor,
    // which by definition runs when no other owner is left to race with.
    PlayerbotSocialControlSlot _slot;
};

/*
 * The live count of in-flight control requests, and the shutdown gate.
 *
 * A live measure rather than a lifetime quota: a slot is taken when a request is admitted and given
 * back when it finishes, so the thirty third control of the day is fine and the thirty third
 * SIMULTANEOUS one is not.
 */
class PlayerbotSocialControlAdmission
{
public:
    // Takes a slot, or refuses. Thread safe: the check and the increment are one step, because
    // every connection thread runs this at once.
    [[nodiscard]] bool TryAdmit();

    void Release();

    // Refuses everything from here on, so a mutation cannot start during teardown.
    void Shutdown();

    [[nodiscard]] bool IsShuttingDown() const;
    [[nodiscard]] std::size_t InFlight() const;

private:
    mutable std::mutex _mutex;
    std::size_t _inFlight = 0;
    bool _shuttingDown = false;
};

#endif
