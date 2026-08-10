/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#include "PlayerbotSocialControlWork.h"

#include <atomic>
#include <exception>
#include <utility>

#include "Bot/Social/PlayerbotSocialConfig.h"
#include "Bot/Social/PlayerbotSocialMgr.h"
#include "Log.h"
#include "Script/WorldThr/PlayerbotWorldThreadProcessor.h"

namespace
{
std::atomic<bool> acceptingControls{false};

/*
 * The in flight bound, shared by every connection thread.
 *
 * A namespace scope singleton rather than a member of something, because the thing it bounds is
 * the process: two connections must contend for the same thirty two slots, and a per connection
 * counter would bound nothing at all.
 */
PlayerbotSocialControlAdmission& ControlAdmission()
{
    static PlayerbotSocialControlAdmission admission;
    return admission;
}

/*
 * Runs the whole authenticated pipeline for a control body and returns the wire answer.
 *
 * Separated from the line handling above it so the order is visible in one place: bound, then
 * authenticate, then parse, then queue. Nothing reaches the world thread until all four have
 * passed, which is what makes every refusal a no state change error by construction.
 */
std::string DispatchControl(PlayerbotSocialControlRequest const& request)
{
    if (!IsPlayerbotSocialControlAcceptingRequests())
        return PlayerbotSocialControlOutcomeResponse(PlayerbotSocialControlOutcome::ShuttingDown);

    PlayerbotSocialControlAdmission& admission = ControlAdmission();
    if (!admission.TryAdmit())
    {
        return PlayerbotSocialControlOutcomeResponse(admission.IsShuttingDown()
                                                         ? PlayerbotSocialControlOutcome::ShuttingDown
                                                         : PlayerbotSocialControlOutcome::QueueFull);
    }

    /*
     * The slot is handed to the shared answer rather than released by this function.
     *
     * The bound counts work outstanding in the system, not callers currently waiting. A caller
     * that times out is finished, but its request is still queued for the world thread, and
     * giving the slot back here would let someone sending faster than the world drains hold far
     * more than the bound at once. Tying it to the answer, which both this caller and the queued
     * work hold, returns it when the last of them is done.
     *
     * It also makes every path below correct by construction, including the one where queueing
     * fails: the answer is destroyed on the way out and takes the slot with it.
     */
    auto result = std::make_shared<PlayerbotSocialControlResult>();
    result->HoldSlot(PlayerbotSocialControlSlot(admission));

    if (!PlayerbotWorldThreadProcessor::instance().QueueOperation(
            std::make_unique<PlayerbotSocialControlWork>(request, result)))
    {
        return PlayerbotSocialControlOutcomeResponse(PlayerbotSocialControlOutcome::QueueFull);
    }

    std::optional<PlayerbotSocialControlOutcome> outcome =
        result->Wait(std::chrono::seconds(PLAYERBOT_SOCIAL_CONTROL_TIMEOUT_SECONDS));

    if (!outcome)
    {
        /*
         * Abandon BEFORE answering, never after. Abandonment and execution claim the same state
         * under one lock, so a successful abandon proves the mutation has not started and never
         * will: that is what lets this report a timeout as a no state change error rather than as
         * a guess. An operator retrying then applies the control once, not twice.
         */
        if (result->Abandon())
            return PlayerbotSocialControlOutcomeResponse(PlayerbotSocialControlOutcome::TimedOut);

        /*
         * The world thread is already inside the mutation, which cannot be unwound. Waiting a
         * bounded grace and reporting what it actually did beats reporting a timeout for a state
         * change that happened.
         */
        outcome = result->Wait(std::chrono::milliseconds(PLAYERBOT_SOCIAL_CONTROL_COMPLETION_GRACE_MS));
        if (!outcome)
        {
            /*
             * The one case where TimedOut is not a promise, stated exactly rather than
             * flattered. The condition is NOT "execution began in the last quarter second": it is
             * that execution was claimed and has not finished within the deadline plus the grace,
             * however early it began. A slow database or a stalled world tick reaches it just as
             * readily. The mutation is running and will complete; this caller simply will not
             * learn the result.
             *
             * Every operation behind this surface is idempotent, so an operator who retries into
             * this window sets the same value twice rather than compounding anything. Closing it
             * properly would mean waiting indefinitely on a world thread that may be wedged,
             * which is a worse failure than an imprecise answer.
             */
            LOG_WARN("playerbots",
                     "Social control {} was still executing after {}s; its caller was answered "
                     "timed_out and will not learn the result.",
                     PlayerbotSocialControlOperationName(request.operation), PLAYERBOT_SOCIAL_CONTROL_TIMEOUT_SECONDS);
            return PlayerbotSocialControlOutcomeResponse(PlayerbotSocialControlOutcome::TimedOut);
        }
    }

    return PlayerbotSocialControlOutcomeResponse(*outcome);
}
}  // namespace

void SetPlayerbotSocialControlAcceptingRequests(bool accepting)
{
    acceptingControls.store(accepting, std::memory_order_release);

    // Shutdown is one way. Reopening admission after teardown has begun would let a control start
    // against a world that is already being dismantled.
    if (!accepting)
        ControlAdmission().Shutdown();
}

bool IsPlayerbotSocialControlAcceptingRequests() { return acceptingControls.load(std::memory_order_acquire); }

PlayerbotSocialControlWork::PlayerbotSocialControlWork(PlayerbotSocialControlRequest request,
                                                       std::shared_ptr<PlayerbotSocialControlResult> result)
    : request(std::move(request)), result(std::move(result))
{
}

bool PlayerbotSocialControlWork::Execute()
{
    // The caller reached its deadline and abandoned this. Applying it now would change state that
    // nothing is waiting on, after its operator was told nothing changed.
    if (!result || !result->TryClaim())
        return false;

    PlayerbotSocialControlOutcome outcome = PlayerbotSocialControlOutcome::InternalError;
    try
    {
        outcome = sPlayerbotSocialMgr.ApplyRuntimeControl(request);
    }
    catch (std::exception const& error)
    {
        LOG_ERROR("playerbots", "Social control {} failed: {}", PlayerbotSocialControlOperationName(request.operation),
                  error.what());
    }
    catch (...)
    {
        LOG_ERROR("playerbots", "Social control {} failed.", PlayerbotSocialControlOperationName(request.operation));
    }

    // Completed on every path, including both catches. A control that threw and never completed
    // would leave its caller waiting out the full timeout for an answer that already exists.
    result->Complete(outcome);
    return outcome == PlayerbotSocialControlOutcome::Applied;
}

bool PlayerbotSocialControlHandleLine(std::string_view line, std::string& response)
{
    PlayerbotSocialControlLine const split = PlayerbotSocialControlSplitLine(line);
    if (!split.isControl)
        return false;

    if (split.oversized)
    {
        /*
         * Answered with the unauthorized refusal rather than a size error, and deliberately so. This
         * line was never authenticated, so telling an unauthenticated caller anything specific about
         * why it failed hands them a probe. The log below carries the real reason.
         */
        LOG_WARN("playerbots", "Social control refused: request exceeded {} bytes.",
                 PLAYERBOT_SOCIAL_CONTROL_MAX_LINE_BYTES);

        /*
         * Authenticated anyway, with nothing to present, purely so this refusal costs what every
         * other refusal costs. Skipping it would make an oversized line measurably cheaper to refuse
         * than a wrong token, which is a timing channel that reports the server's limits to a caller
         * that has proved nothing. The result is discarded: an oversized line is refused whatever it
         * carried.
         */
        (void)PlayerbotSocialControlAuthenticate({}, sPlayerbotSocialConfig.socialChatControlToken);
        response = PlayerbotSocialControlAuthResponse(PlayerbotSocialControlAuth::TokenMismatch);
        return true;
    }

    PlayerbotSocialControlAuth const auth =
        PlayerbotSocialControlAuthenticate(split.token, sPlayerbotSocialConfig.socialChatControlToken);

    if (auth != PlayerbotSocialControlAuth::Granted)
    {
        // The reason goes to the log, which is already inside the trust boundary, and never onto the
        // wire, where it would tell a caller about a deployment they have not authenticated to.
        LOG_WARN("playerbots", "Social control refused: {}.", PlayerbotSocialControlAuthName(auth));
        response = PlayerbotSocialControlAuthResponse(auth);
        return true;
    }

    PlayerbotSocialControlParseResult const parsed = PlayerbotSocialControlParseRequest(split.body);
    if (PlayerbotSocialControlParseIsRefusal(parsed.outcome))
    {
        /*
         * Named on the wire, unlike an authentication failure. This caller has already proved it
         * holds the token, so it is inside the trust boundary and telling it that its operation name
         * was wrong reveals nothing it could not learn by trying every name.
         */
        response = std::string("error,") + PlayerbotSocialControlParseName(parsed.outcome);
        return true;
    }

    response = DispatchControl(parsed.request);

    LOG_INFO("playerbots", "Social control {} answered {}.",
             PlayerbotSocialControlOperationName(parsed.request.operation), response);
    return true;
}
