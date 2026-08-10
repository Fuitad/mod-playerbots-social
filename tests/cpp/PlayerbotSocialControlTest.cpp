/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#include <atomic>
#include <chrono>
#include <memory>
#include <set>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "Bot/Social/PlayerbotSocialControl.h"
#include "gtest/gtest.h"

TEST(PlayerbotSocialControlAuthTest, WithNoTokenConfiguredEveryRequestIsRefused)
{
    /*
     * Definition of Done 1, in its most important case. An operator who has not set a token has not
     * opted into remote mutation, and the deployment default is therefore "no controls" rather than
     * "controls open to anyone who can reach the port". Accepting an empty presented token against
     * an empty configured one would turn the absence of configuration into a wide open door, which
     * is the single worst reading of this input.
     */
    EXPECT_EQ(PlayerbotSocialControlAuthenticate("", ""), PlayerbotSocialControlAuth::NoTokenConfigured);
    EXPECT_EQ(PlayerbotSocialControlAuthenticate("anything", ""), PlayerbotSocialControlAuth::NoTokenConfigured);
}

TEST(PlayerbotSocialControlAuthTest, AConfiguredTokenTooShortToBeASecretIsRefused)
{
    /*
     * A four character token is not a secret against a caller that can retry, and the loopback bind
     * is a second line of defence rather than a reason to accept a weak first one. Refused as a
     * configuration problem rather than as a mismatch, so the operator is told the token is unusable
     * instead of quietly discovering that the correct token does not work.
     */
    std::string const shortToken = "abcd";

    EXPECT_EQ(PlayerbotSocialControlAuthenticate(shortToken, shortToken), PlayerbotSocialControlAuth::TokenTooShort);
}

TEST(PlayerbotSocialControlAuthTest, OnlyTheExactTokenIsAccepted)
{
    std::string const token = "0123456789abcdef0123456789abcdef";

    EXPECT_EQ(PlayerbotSocialControlAuthenticate(token, token), PlayerbotSocialControlAuth::Granted);

    // A prefix, an extension, and a single flipped character are all mismatches. The prefix case is
    // the one a length-only or prefix-only comparison would wrongly admit.
    EXPECT_EQ(PlayerbotSocialControlAuthenticate("0123456789abcdef", token), PlayerbotSocialControlAuth::TokenMismatch);
    EXPECT_EQ(PlayerbotSocialControlAuthenticate(token + "0", token), PlayerbotSocialControlAuth::TokenMismatch);
    EXPECT_EQ(PlayerbotSocialControlAuthenticate("0123456789abcdef0123456789abcdeF", token),
              PlayerbotSocialControlAuth::TokenMismatch);
    EXPECT_EQ(PlayerbotSocialControlAuthenticate("", token), PlayerbotSocialControlAuth::TokenMismatch);
}

TEST(PlayerbotSocialControlAuthTest, AMatchingFixedWidthPrefixCannotHideAnOverlongSuffix)
{
    std::string const configured(PLAYERBOT_SOCIAL_CONTROL_COMPARE_WIDTH, 'a');
    std::string const presented = configured + std::string(PLAYERBOT_SOCIAL_CONTROL_COMPARE_WIDTH, 'b');

    EXPECT_EQ(PlayerbotSocialControlAuthenticate(presented, configured), PlayerbotSocialControlAuth::TokenMismatch);
}

TEST(PlayerbotSocialControlAuthTest, TheFixedComparisonWidthIsTheMaximumConfiguredTokenLength)
{
    std::string const atBound(PLAYERBOT_SOCIAL_CONTROL_MAX_TOKEN_LENGTH, 'a');
    std::string const overBound(PLAYERBOT_SOCIAL_CONTROL_MAX_TOKEN_LENGTH + 1, 'a');

    EXPECT_EQ(PlayerbotSocialControlAuthenticate(atBound, atBound), PlayerbotSocialControlAuth::Granted);
    EXPECT_EQ(PlayerbotSocialControlAuthenticate(overBound, overBound), PlayerbotSocialControlAuth::TokenTooLong);
}

TEST(PlayerbotSocialControlAuthTest, ARefusalNeverRepeatsAnythingItWasGiven)
{
    /*
     * Definition of Done 1's second half: no state change AND no secret leaked. The wire response
     * for every refusal is a fixed string, so it cannot echo the presented token, report how much of
     * it matched, or reveal the configured token's length. Asserted on the text itself because that
     * is the thing that actually reaches the socket.
     */
    std::string const token = "0123456789abcdef0123456789abcdef";

    for (PlayerbotSocialControlAuth outcome :
         {PlayerbotSocialControlAuth::NoTokenConfigured, PlayerbotSocialControlAuth::TokenTooShort,
          PlayerbotSocialControlAuth::TokenTooLong, PlayerbotSocialControlAuth::TokenMismatch})
    {
        std::string const response = PlayerbotSocialControlAuthResponse(outcome);

        EXPECT_EQ(response.find(token), std::string::npos) << "leaked the token";
        EXPECT_EQ(response.find("32"), std::string::npos) << "leaked a length";
    }

    // The three refusals are also indistinguishable from each other on the wire. A caller learning
    // "the token is too short" would learn the deployment's configuration from outside it.
    EXPECT_EQ(PlayerbotSocialControlAuthResponse(PlayerbotSocialControlAuth::NoTokenConfigured),
              PlayerbotSocialControlAuthResponse(PlayerbotSocialControlAuth::TokenMismatch));
    EXPECT_EQ(PlayerbotSocialControlAuthResponse(PlayerbotSocialControlAuth::TokenTooShort),
              PlayerbotSocialControlAuthResponse(PlayerbotSocialControlAuth::TokenMismatch));
    EXPECT_EQ(PlayerbotSocialControlAuthResponse(PlayerbotSocialControlAuth::TokenTooLong),
              PlayerbotSocialControlAuthResponse(PlayerbotSocialControlAuth::TokenMismatch));
}

// Request parsing ------------------------------------------------------------------------------

namespace
{
// The wire shape the command server hands in, minus the token, which authentication already
// consumed. Kept in one helper so a change to the separator shows up in one place.
PlayerbotSocialControlParseResult ParseBody(std::string const& body)
{
    return PlayerbotSocialControlParseRequest(body);
}
}  // namespace

TEST(PlayerbotSocialControlParseTest, TheSixControlsAndTheirValuesAreAccepted)
{
    // Definition of Done 3's vocabulary, asserted as the parse boundary sees it. Every one of these
    // must survive a restart, so every one of them has to be expressible here first.
    EXPECT_EQ(ParseBody("pause,1").outcome, PlayerbotSocialControlParse::Accepted);
    EXPECT_TRUE(ParseBody("pause,1").request.flag);
    EXPECT_FALSE(ParseBody("pause,0").request.flag);

    EXPECT_EQ(ParseBody("density,quiet").request.density, PlayerbotSocialDensityProfile::Quiet);
    EXPECT_EQ(ParseBody("density,lively").request.density, PlayerbotSocialDensityProfile::Lively);

    EXPECT_EQ(ParseBody("channel_general,0").outcome, PlayerbotSocialControlParse::Accepted);
    EXPECT_EQ(ParseBody("channel_say,1").outcome, PlayerbotSocialControlParse::Accepted);
    EXPECT_EQ(ParseBody("channel_party,0").outcome, PlayerbotSocialControlParse::Accepted);
    EXPECT_EQ(ParseBody("channel_whisper,1").outcome, PlayerbotSocialControlParse::Accepted);

    /*
     * The identity shape every social table actually stores: a kind prefix, an underscore, and
     * PLAYERBOT_SOCIAL_PUBLIC_ID_BODY_LENGTH lowercase hex characters. Written out literally rather
     * than assembled from the prefix helper, so a change to the frozen shape has to be answered here
     * rather than silently followed by a test that agrees with whatever the code now does.
     */
    std::string const memoryId = "mem_0123456789abcdef0123456789abcdef";
    std::string const relationshipId = "rel_fedcba98765432100123456789abcdef";
    std::string const caseId = "cas_00112233445566778899aabbccddeeff";

    EXPECT_EQ(ParseBody("reset_memory," + memoryId).outcome, PlayerbotSocialControlParse::Accepted);
    EXPECT_EQ(ParseBody("reset_memory," + memoryId).request.subject, memoryId);
    EXPECT_EQ(ParseBody("reset_relationship," + relationshipId).outcome, PlayerbotSocialControlParse::Accepted);
    EXPECT_EQ(ParseBody("acknowledge_case," + caseId).outcome, PlayerbotSocialControlParse::Accepted);
}

TEST(PlayerbotSocialControlParseTest, AnIdentityIsAcceptedOnlyForTheKindTheOperationNames)
{
    /*
     * The kind prefix exists to keep one identity from being accepted where another is required, and
     * the parse boundary is the only place that can enforce it: the world thread looks a subject up
     * in exactly one table, so a relationship id sent to reset_memory would come back as
     * SubjectNotFound, which reads as "already gone" rather than "you named the wrong thing".
     */
    EXPECT_EQ(ParseBody("reset_memory,rel_0123456789abcdef0123456789abcdef").outcome,
              PlayerbotSocialControlParse::InvalidValue);
    EXPECT_EQ(ParseBody("reset_relationship,mem_0123456789abcdef0123456789abcdef").outcome,
              PlayerbotSocialControlParse::InvalidValue);
    EXPECT_EQ(ParseBody("acknowledge_case,act_0123456789abcdef0123456789abcdef").outcome,
              PlayerbotSocialControlParse::InvalidValue);
}

TEST(PlayerbotSocialControlParseTest, AnythingOutsideTheVocabularyIsRefusedRatherThanGuessed)
{
    /*
     * Definition of Done 4 at the parse boundary: an invalid control value must never reach the
     * world thread, so it is refused HERE rather than clamped, defaulted, or passed along to be
     * sorted out later. PlayerbotSocialParseDensityProfile deliberately resolves an unknown name to
     * Normal because a configuration typo must not mute or unmute the server; that is the right
     * answer for a config file and the wrong one for an untrusted socket, so this does not reuse it.
     */
    EXPECT_EQ(ParseBody("").outcome, PlayerbotSocialControlParse::MalformedRequest);
    EXPECT_EQ(ParseBody("pause").outcome, PlayerbotSocialControlParse::MalformedRequest);
    EXPECT_EQ(ParseBody(",1").outcome, PlayerbotSocialControlParse::MalformedRequest);

    EXPECT_EQ(ParseBody("shutdown,1").outcome, PlayerbotSocialControlParse::UnknownOperation);
    EXPECT_EQ(ParseBody("PAUSE,1").outcome, PlayerbotSocialControlParse::UnknownOperation);

    EXPECT_EQ(ParseBody("pause,2").outcome, PlayerbotSocialControlParse::InvalidValue);
    EXPECT_EQ(ParseBody("pause,true").outcome, PlayerbotSocialControlParse::InvalidValue);
    EXPECT_EQ(ParseBody("pause,").outcome, PlayerbotSocialControlParse::InvalidValue);
    EXPECT_EQ(ParseBody("density,loud").outcome, PlayerbotSocialControlParse::InvalidValue);
    EXPECT_EQ(ParseBody("density,NORMAL").outcome, PlayerbotSocialControlParse::InvalidValue);

    // An opaque id is a fixed-shape public identifier, not free text. Anything else is a caller
    // sending an internal id, a name, or an injection attempt.
    EXPECT_EQ(ParseBody("reset_memory,661").outcome, PlayerbotSocialControlParse::InvalidValue);
    EXPECT_EQ(ParseBody("reset_memory,").outcome, PlayerbotSocialControlParse::InvalidValue);
    EXPECT_EQ(ParseBody("reset_memory,'; DROP TABLE x; --").outcome, PlayerbotSocialControlParse::InvalidValue);
    EXPECT_EQ(ParseBody("reset_memory,11111111-1111-1111-1111-11111111111g").outcome,
              PlayerbotSocialControlParse::InvalidValue);

    /*
     * A dashed UUID is the right LENGTH and nothing else. It is called out because it is the shape
     * this parser accepted before, while no social table has ever stored one, so every real
     * identifier was refused and every accepted one named nothing.
     */
    EXPECT_EQ(ParseBody("reset_memory,11111111-1111-1111-1111-111111111111").outcome,
              PlayerbotSocialControlParse::InvalidValue);

    // Right prefix, right length, wrong alphabet. The stored body is lowercase hex only.
    EXPECT_EQ(ParseBody("reset_memory,mem_0123456789ABCDEF0123456789abcdef").outcome,
              PlayerbotSocialControlParse::InvalidValue);

    // Right prefix and alphabet, one character short and one character long.
    EXPECT_EQ(ParseBody("reset_memory,mem_0123456789abcdef0123456789abcde").outcome,
              PlayerbotSocialControlParse::InvalidValue);
    EXPECT_EQ(ParseBody("reset_memory,mem_0123456789abcdef0123456789abcdeff").outcome,
              PlayerbotSocialControlParse::InvalidValue);
}

TEST(PlayerbotSocialControlParseTest, AnOversizedRequestIsRefusedBeforeItIsParsed)
{
    /*
     * The socket reads a line, and a line is whatever the caller chooses to send. Bounding it here
     * means a caller cannot make the server allocate or scan an arbitrary amount of memory just by
     * never sending a separator, and the bound is checked before any parsing so the cost of a
     * refusal does not scale with the size of the abuse.
     */
    std::string const huge(PLAYERBOT_SOCIAL_CONTROL_MAX_REQUEST_BYTES + 1, 'a');

    EXPECT_EQ(ParseBody(huge).outcome, PlayerbotSocialControlParse::RequestTooLong);
    EXPECT_EQ(ParseBody("reset_memory," + huge).outcome, PlayerbotSocialControlParse::RequestTooLong);

    // Exactly at the bound is still refused for content, not for length, so the boundary itself is
    // not silently off by one in the permissive direction.
    std::string const atBound(PLAYERBOT_SOCIAL_CONTROL_MAX_REQUEST_BYTES, 'a');
    EXPECT_EQ(ParseBody(atBound).outcome, PlayerbotSocialControlParse::MalformedRequest);
}

TEST(PlayerbotSocialControlParseTest, AParseRefusalNamesNoStateChange)
{
    // Every refusal is a typed no-state-change error, which is what lets the socket answer without
    // the world thread being involved at all. Asserted so a later operation cannot be added that
    // reports a refusal while having already done something.
    for (PlayerbotSocialControlParse outcome :
         {PlayerbotSocialControlParse::MalformedRequest, PlayerbotSocialControlParse::UnknownOperation,
          PlayerbotSocialControlParse::InvalidValue, PlayerbotSocialControlParse::RequestTooLong})
    {
        EXPECT_TRUE(PlayerbotSocialControlParseIsRefusal(outcome));
        EXPECT_NE(std::string(PlayerbotSocialControlParseName(outcome)), "unknown");
    }

    EXPECT_FALSE(PlayerbotSocialControlParseIsRefusal(PlayerbotSocialControlParse::Accepted));
}

// Cross-thread dispatch --------------------------------------------------------------------------

TEST(PlayerbotSocialControlResultTest, ARequestChangesNothingUntilTheWorldThreadClaimsIt)
{
    /*
     * Definition of Done 4, first half. The socket thread creates the shared state and hands the
     * work over; it never touches social state itself. Nothing is claimed, nothing is completed, and
     * a caller asking for the answer right away is told there is not one rather than a default that
     * reads like success.
     */
    PlayerbotSocialControlResult result;

    EXPECT_FALSE(result.IsCompleted());
    EXPECT_FALSE(result.Wait(std::chrono::milliseconds(0)).has_value());
}

TEST(PlayerbotSocialControlResultTest, TheWorldThreadClaimsOnceAndItsOutcomeReachesTheCaller)
{
    // The other half of Definition of Done 4: exactly one execution, and its answer is the one the
    // waiting caller receives.
    PlayerbotSocialControlResult result;

    EXPECT_TRUE(result.TryClaim());
    // A second executor, or the same operation queued twice, must not run the mutation again.
    EXPECT_FALSE(result.TryClaim());

    result.Complete(PlayerbotSocialControlOutcome::Applied);

    ASSERT_TRUE(result.IsCompleted());
    auto const answer = result.Wait(std::chrono::milliseconds(0));
    ASSERT_TRUE(answer.has_value());
    EXPECT_EQ(*answer, PlayerbotSocialControlOutcome::Applied);
}

TEST(PlayerbotSocialControlResultTest, ATimedOutCallerAbandonsItsRequestSoItIsNeverApplied)
{
    /*
     * The requirement in Key Decision 4, and the one most easily got backwards: a timeout is a
     * no-state-change error, so the mutation behind it must not run afterwards. Abandoning first and
     * only then answering the caller is what makes TimedOut a promise rather than a guess -- an
     * operator who retries after a slow tick applies the control once, not twice.
     */
    PlayerbotSocialControlResult result;

    EXPECT_TRUE(result.Abandon());
    EXPECT_FALSE(result.TryClaim());
    EXPECT_FALSE(result.IsCompleted());
}

TEST(PlayerbotSocialControlResultTest, AClaimedRequestCannotBeAbandonedSoTheRealOutcomeSurvives)
{
    /*
     * The race in the other direction. Once the world thread is inside the mutation, a caller giving
     * up cannot unwind it: the state change is already happening. Abandon refuses, which is the
     * signal the caller uses to wait a bounded grace and report what actually happened instead of
     * claiming a timeout for work that succeeded.
     */
    PlayerbotSocialControlResult result;

    EXPECT_TRUE(result.TryClaim());
    EXPECT_FALSE(result.Abandon());

    result.Complete(PlayerbotSocialControlOutcome::Applied);
    auto const answer = result.Wait(std::chrono::milliseconds(0));
    ASSERT_TRUE(answer.has_value());
    EXPECT_EQ(*answer, PlayerbotSocialControlOutcome::Applied);
}

TEST(PlayerbotSocialControlResultTest, ClaimAndAbandonContendForTheSameStateSoExactlyOneWins)
{
    /*
     * The two tests above prove each ordering in isolation, which a lock-free reading of the same
     * two flags would also pass. This runs the two sides concurrently many times over: whatever the
     * interleaving, the mutation runs at most once and never after its caller was told it timed out.
     */
    constexpr int ROUNDS = 2000;
    int applied = 0;
    int timedOut = 0;

    for (int round = 0; round < ROUNDS; ++round)
    {
        PlayerbotSocialControlResult result;
        std::atomic<bool> start{false};
        bool claimed = false;
        bool abandoned = false;

        std::thread worldThread(
            [&]
            {
                while (!start.load(std::memory_order_acquire))
                {
                }
                claimed = result.TryClaim();
            });
        std::thread socketThread(
            [&]
            {
                while (!start.load(std::memory_order_acquire))
                {
                }
                abandoned = result.Abandon();
            });

        start.store(true, std::memory_order_release);
        worldThread.join();
        socketThread.join();

        // Never both: applying a mutation whose caller was told "no state change" is the bug.
        EXPECT_FALSE(claimed && abandoned) << "round " << round;
        // And never neither, or the request would be stranded with no answer and no execution.
        EXPECT_TRUE(claimed || abandoned) << "round " << round;

        applied += claimed ? 1 : 0;
        timedOut += abandoned ? 1 : 0;
    }

    EXPECT_EQ(applied + timedOut, ROUNDS);
}

TEST(PlayerbotSocialControlResultTest, ALateCompletionIsDiscardedRatherThanOverwritingTheAnswer)
{
    // "A late completion is discarded safely." The first answer is the one the caller was given, so
    // a second must not change what a later reader sees.
    PlayerbotSocialControlResult result;

    result.Complete(PlayerbotSocialControlOutcome::Applied);
    result.Complete(PlayerbotSocialControlOutcome::InternalError);

    auto const answer = result.Wait(std::chrono::milliseconds(0));
    ASSERT_TRUE(answer.has_value());
    EXPECT_EQ(*answer, PlayerbotSocialControlOutcome::Applied);
}

TEST(PlayerbotSocialControlResultTest, AWaiterIsWokenByTheOutcomeRatherThanByItsDeadline)
{
    /*
     * The socket response is synchronous, so this wait is what a connection thread actually blocks
     * on. It has to return as soon as the world thread answers: a wait that only ever returned at
     * the deadline would make every control take the full timeout and still pass a completion test.
     */
    PlayerbotSocialControlResult result;

    std::thread worldThread(
        [&]
        {
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
            result.Complete(PlayerbotSocialControlOutcome::Applied);
        });

    auto const started = std::chrono::steady_clock::now();
    auto const answer = result.Wait(std::chrono::seconds(10));
    auto const waited = std::chrono::steady_clock::now() - started;
    worldThread.join();

    ASSERT_TRUE(answer.has_value());
    EXPECT_EQ(*answer, PlayerbotSocialControlOutcome::Applied);
    EXPECT_LT(waited, std::chrono::seconds(5));
}

TEST(PlayerbotSocialControlResultTest, AWaitThatExpiresReportsNothingRatherThanAFalseOutcome)
{
    // An expired wait must be distinguishable from an answer. Returning a default outcome here is
    // how a timeout would come to look like a successful application.
    PlayerbotSocialControlResult result;

    EXPECT_FALSE(result.Wait(std::chrono::milliseconds(5)).has_value());
}

TEST(PlayerbotSocialControlAdmissionTest, RefusesBeyondCapacityAndRecoversAsRequestsFinish)
{
    /*
     * Controls share the world thread operation queue with group, LFG, and guild work. Without a
     * bound of their own, a caller that opens sockets faster than the world ticks would fill that
     * shared queue and starve the bots. The refusal is a typed no-state-change error, not a silent
     * drop the caller waits out.
     */
    PlayerbotSocialControlAdmission admission;

    for (std::size_t i = 0; i < PLAYERBOT_SOCIAL_CONTROL_QUEUE_CAPACITY; ++i)
        EXPECT_TRUE(admission.TryAdmit()) << "at " << i;

    EXPECT_FALSE(admission.TryAdmit());
    EXPECT_EQ(admission.InFlight(), PLAYERBOT_SOCIAL_CONTROL_QUEUE_CAPACITY);

    // A finished request frees exactly one slot, so the bound is a live measure and not a lifetime
    // quota that would refuse every control after the first thirty two.
    admission.Release();
    EXPECT_EQ(admission.InFlight(), PLAYERBOT_SOCIAL_CONTROL_QUEUE_CAPACITY - 1);
    EXPECT_TRUE(admission.TryAdmit());
    EXPECT_FALSE(admission.TryAdmit());
}

TEST(PlayerbotSocialControlAdmissionTest, ShutdownRefusesEverythingFromThenOn)
{
    // A mutation must not be applied halfway through teardown, and the refusal has to say so rather
    // than looking like congestion the caller should retry through.
    PlayerbotSocialControlAdmission admission;

    EXPECT_TRUE(admission.TryAdmit());
    admission.Shutdown();

    EXPECT_TRUE(admission.IsShuttingDown());
    EXPECT_FALSE(admission.TryAdmit());

    // Even once the in-flight request drains, nothing new is accepted.
    admission.Release();
    EXPECT_EQ(admission.InFlight(), 0u);
    EXPECT_FALSE(admission.TryAdmit());
}

TEST(PlayerbotSocialControlAdmissionTest, ConcurrentAdmissionNeverExceedsTheBound)
{
    // The bound is read and written from every connection thread at once. A check-then-increment
    // that is not atomic passes both tests above and still overshoots here.
    PlayerbotSocialControlAdmission admission;
    constexpr int THREADS = 16;
    constexpr int ATTEMPTS = 64;
    std::atomic<int> admitted{0};
    std::atomic<bool> start{false};
    std::vector<std::thread> threads;

    threads.reserve(THREADS);
    for (int i = 0; i < THREADS; ++i)
    {
        threads.emplace_back(
            [&]
            {
                while (!start.load(std::memory_order_acquire))
                {
                }
                for (int attempt = 0; attempt < ATTEMPTS; ++attempt)
                    if (admission.TryAdmit())
                        admitted.fetch_add(1, std::memory_order_relaxed);
            });
    }

    start.store(true, std::memory_order_release);
    for (std::thread& thread : threads)
        thread.join();

    EXPECT_EQ(static_cast<std::size_t>(admitted.load()), PLAYERBOT_SOCIAL_CONTROL_QUEUE_CAPACITY);
    EXPECT_EQ(admission.InFlight(), PLAYERBOT_SOCIAL_CONTROL_QUEUE_CAPACITY);
}

TEST(PlayerbotSocialControlOutcomeTest, EveryOutcomeHasAStableNameAndKnowsIfItChangedState)
{
    // The wire and the log both read these, and Medivh will later display them. Only Applied
    // represents a state change; every other value must be safe for a caller to retry.
    EXPECT_TRUE(PlayerbotSocialControlOutcomeChangedState(PlayerbotSocialControlOutcome::Applied));

    for (PlayerbotSocialControlOutcome outcome :
         {PlayerbotSocialControlOutcome::Pending, PlayerbotSocialControlOutcome::QueueFull,
          PlayerbotSocialControlOutcome::TimedOut, PlayerbotSocialControlOutcome::ShuttingDown,
          PlayerbotSocialControlOutcome::SubjectNotFound, PlayerbotSocialControlOutcome::InternalError})
    {
        EXPECT_FALSE(PlayerbotSocialControlOutcomeChangedState(outcome)) << PlayerbotSocialControlOutcomeName(outcome);
        EXPECT_NE(std::string(PlayerbotSocialControlOutcomeName(outcome)), "unknown_outcome");
    }
}

TEST(PlayerbotSocialControlParseTest, EachChannelOperationActsOnItsOwnChannelAndNoOther)
{
    /*
     * The pairing a transposition would break silently: swap two arms and every toggle still appears
     * to work, on the wrong surface. Asserted as a bijection rather than four equalities, so a
     * duplicated arm is caught as well as a swapped one.
     */
    struct Pairing
    {
        PlayerbotSocialControlOperation operation;
        PlayerbotSocialChannel channel;
    };

    constexpr Pairing PAIRINGS[] = {{PlayerbotSocialControlOperation::ChannelGeneral, PlayerbotSocialChannel::General},
                                    {PlayerbotSocialControlOperation::ChannelSay, PlayerbotSocialChannel::Say},
                                    {PlayerbotSocialControlOperation::ChannelParty, PlayerbotSocialChannel::Party},
                                    {PlayerbotSocialControlOperation::ChannelWhisper, PlayerbotSocialChannel::Whisper}};

    std::set<PlayerbotSocialChannel> reached;
    for (Pairing const& pairing : PAIRINGS)
    {
        PlayerbotSocialChannel channel = PlayerbotSocialChannel::Whisper;
        ASSERT_TRUE(PlayerbotSocialControlChannelFor(pairing.operation, channel))
            << PlayerbotSocialControlOperationName(pairing.operation);
        EXPECT_EQ(channel, pairing.channel) << PlayerbotSocialControlOperationName(pairing.operation);
        reached.insert(channel);
    }

    EXPECT_EQ(reached.size(), PLAYERBOT_SOCIAL_CHANNEL_COUNT);

    // And the operations that act on no channel say so rather than leaving a stale value the caller
    // would then toggle.
    for (PlayerbotSocialControlOperation operation :
         {PlayerbotSocialControlOperation::Pause, PlayerbotSocialControlOperation::Density,
          PlayerbotSocialControlOperation::ResetMemory, PlayerbotSocialControlOperation::ResetRelationship,
          PlayerbotSocialControlOperation::AcknowledgeCase})
    {
        PlayerbotSocialChannel channel = PlayerbotSocialChannel::Party;
        EXPECT_FALSE(PlayerbotSocialControlChannelFor(operation, channel))
            << PlayerbotSocialControlOperationName(operation);
        EXPECT_EQ(channel, PlayerbotSocialChannel::Party) << "left untouched";
    }
}

// The command server line -------------------------------------------------------------------------

TEST(PlayerbotSocialControlLineTest, OnlyTheControlPrefixIsClaimedAndNothingElseIsTouched)
{
    /*
     * Definition of Done 2. The command port already answers telemetry and inspect, and those must
     * keep working exactly as they do: this splitter is the first thing the port runs, so anything it
     * claims by mistake would stop reaching the handler that owns it.
     */
    for (std::string_view line :
         {"telemetry,0", "inspect,42", "state,42", "position,42", "social,42", "socialconsent,42", "", ","})
    {
        EXPECT_FALSE(PlayerbotSocialControlSplitLine(line).isControl) << "claimed " << line;
    }
}

TEST(PlayerbotSocialControlLineTest, AControlLineIsSplitIntoItsTokenAndItsBody)
{
    // The token is separated here rather than inside the parser, so the parser never sees the secret
    // and cannot echo it into a refusal message by accident.
    PlayerbotSocialControlLine const line = PlayerbotSocialControlSplitLine("socialcontrol,s3cret,pause,1");

    EXPECT_TRUE(line.isControl);
    EXPECT_EQ(line.token, "s3cret");
    EXPECT_EQ(line.body, "pause,1");
}

TEST(PlayerbotSocialControlLineTest, AControlLineWithNoBodyIsStillClaimedSoItIsRefusedNotIgnored)
{
    /*
     * Claimed with an empty body rather than left unclaimed. An unclaimed line falls through to the
     * bot command handler, which would answer "invalid guid" and tell an operator with a real token
     * that their token was the problem.
     */
    PlayerbotSocialControlLine const line = PlayerbotSocialControlSplitLine("socialcontrol,s3cret");

    EXPECT_TRUE(line.isControl);
    EXPECT_EQ(line.token, "s3cret");
    EXPECT_TRUE(line.body.empty());
}

TEST(PlayerbotSocialControlLineTest, ABodyKeepsEveryCommaAfterTheTokenSoAValueMayContainOne)
{
    // Split twice, not on every comma. A value is the parser's business, and a splitter that ate the
    // rest would silently truncate any future value that contains one.
    PlayerbotSocialControlLine const line =
        PlayerbotSocialControlSplitLine("socialcontrol,s3cret,acknowledge_case,a,b");

    EXPECT_TRUE(line.isControl);
    EXPECT_EQ(line.token, "s3cret");
    EXPECT_EQ(line.body, "acknowledge_case,a,b");
}

TEST(PlayerbotSocialControlLineTest, AnOversizedLineIsClaimedAndRefusedRatherThanSplit)
{
    // The bound is checked before anything is copied out of the line, so a caller cannot make the
    // server allocate a token and a body from an arbitrarily long line just by prefixing it.
    std::string oversized = "socialcontrol,";
    oversized.append(PLAYERBOT_SOCIAL_CONTROL_MAX_LINE_BYTES * 2, 'x');

    PlayerbotSocialControlLine const line = PlayerbotSocialControlSplitLine(oversized);

    EXPECT_TRUE(line.isControl);
    EXPECT_TRUE(line.oversized);
    EXPECT_TRUE(line.token.empty());
    EXPECT_TRUE(line.body.empty());
}

TEST(PlayerbotSocialControlLineTest, EveryOutcomeAnswersWithItsOwnStableWireLine)
{
    /*
     * Medivh reads these, so they are a contract rather than log text. Applied is the only one that
     * reports a change; the rest have to be distinguishable so an operator can tell a rejected
     * request from one that never ran.
     */
    std::set<std::string> answers;
    for (PlayerbotSocialControlOutcome outcome :
         {PlayerbotSocialControlOutcome::Applied, PlayerbotSocialControlOutcome::SubjectNotFound,
          PlayerbotSocialControlOutcome::QueueFull, PlayerbotSocialControlOutcome::TimedOut,
          PlayerbotSocialControlOutcome::ShuttingDown, PlayerbotSocialControlOutcome::InternalError,
          PlayerbotSocialControlOutcome::Pending})
    {
        std::string const answer = PlayerbotSocialControlOutcomeResponse(outcome);
        EXPECT_FALSE(answer.empty()) << PlayerbotSocialControlOutcomeName(outcome);
        EXPECT_TRUE(answers.insert(answer).second) << "duplicate answer " << answer;
    }

    EXPECT_EQ(PlayerbotSocialControlOutcomeResponse(PlayerbotSocialControlOutcome::Applied), "ok,applied");

    // A refusal never carries anything a caller could learn the configuration from.
    for (PlayerbotSocialControlOutcome outcome :
         {PlayerbotSocialControlOutcome::QueueFull, PlayerbotSocialControlOutcome::TimedOut,
          PlayerbotSocialControlOutcome::ShuttingDown})
    {
        EXPECT_EQ(PlayerbotSocialControlOutcomeResponse(outcome).rfind("error,", 0), 0u);
    }
}

TEST(PlayerbotSocialControlLineTest, ALongTokenIsNotMistakenForAnOversizedLine)
{
    /*
     * The line bound has to clear the prefix, a long token, and the longest real body together. A
     * bound that did not would refuse every control from a deployment that chose a strong token and
     * answer "unauthorized", sending an operator off to replace a secret that was fine.
     */
    std::string const token(128, 'k');
    std::string const body = "acknowledge_case," + std::string(PLAYERBOT_SOCIAL_CONTROL_PUBLIC_ID_LENGTH, 'a');
    std::string const line = std::string(PLAYERBOT_SOCIAL_CONTROL_PREFIX) + token + "," + body;

    PlayerbotSocialControlLine const split = PlayerbotSocialControlSplitLine(line);

    EXPECT_TRUE(split.isControl);
    EXPECT_FALSE(split.oversized);
    EXPECT_EQ(split.token, token);
    EXPECT_EQ(split.body, body);
}

TEST(PlayerbotSocialControlAdmissionTest, ASlotIsHeldUntilTheWorkIsDoneNotUntilTheCallerGivesUp)
{
    /*
     * The bound counts work outstanding in the SYSTEM, not callers currently waiting. Releasing when
     * a caller times out would let someone who sends requests faster than the world thread drains
     * them hold far more than the bound: each abandoned request frees its slot while its operation is
     * still sitting in the world queue. Tying the slot to the shared answer, which both the caller
     * and the queued work hold, makes it come back exactly when the last of them is finished with it.
     */
    PlayerbotSocialControlAdmission admission;
    ASSERT_TRUE(admission.TryAdmit());

    auto result = std::make_shared<PlayerbotSocialControlResult>();
    result->HoldSlot(PlayerbotSocialControlSlot(admission));

    // The world thread still holds the work.
    auto heldByWorldThread = result;

    // The caller gives up and lets go.
    result.reset();
    EXPECT_EQ(admission.InFlight(), 1u) << "freed while the work was still outstanding";

    // Only when the work itself is finished does the slot come back.
    heldByWorldThread.reset();
    EXPECT_EQ(admission.InFlight(), 0u);
}

TEST(PlayerbotSocialControlAdmissionTest, ASlotIsGivenBackExactlyOnce)
{
    // A slot returned twice would let the bound drift upward over time until it stopped bounding
    // anything, which is the failure that looks like nothing at all until the server is under load.
    PlayerbotSocialControlAdmission admission;
    ASSERT_TRUE(admission.TryAdmit());
    ASSERT_TRUE(admission.TryAdmit());
    EXPECT_EQ(admission.InFlight(), 2u);

    {
        PlayerbotSocialControlSlot slot(admission);
        PlayerbotSocialControlSlot moved(std::move(slot));
        // The moved-from slot must not also release: one admission, one return.
    }

    EXPECT_EQ(admission.InFlight(), 1u);
}

TEST(PlayerbotSocialControlAuthTest, EveryRefusalCostsTheSameWorkAsEveryOther)
{
    /*
     * The refusals are message-identical, which is not the same as indistinguishable. An
     * unconfigured or too-short token that returned before comparing anything, or a comparison whose
     * length followed the configured token's, would let a caller learn the deployment's
     * configuration by timing alone, which is exactly what the fixed refusal string exists to
     * prevent. The comparison is therefore run on every path, over a fixed width, so its cost
     * depends on neither input.
     *
     * Timing is not asserted here, because a wall-clock assertion on a loaded machine is a flaky
     * test rather than a proof. What is asserted is the property that makes the timing equal: every
     * path returns a refusal, and none of them is reached by a shortcut that skips the comparison.
     */
    std::string const configured(PLAYERBOT_SOCIAL_CONTROL_MIN_TOKEN_LENGTH, 'k');

    EXPECT_EQ(PlayerbotSocialControlAuthenticate("anything", ""), PlayerbotSocialControlAuth::NoTokenConfigured);
    EXPECT_EQ(PlayerbotSocialControlAuthenticate("anything", "short"), PlayerbotSocialControlAuth::TokenTooShort);
    EXPECT_EQ(PlayerbotSocialControlAuthenticate("anything", configured), PlayerbotSocialControlAuth::TokenMismatch);
    EXPECT_EQ(PlayerbotSocialControlAuthenticate(configured, configured), PlayerbotSocialControlAuth::Granted);

    /*
     * The placeholder the unusable-token paths compare against must never grant. A caller that
     * guessed it would otherwise authenticate against a deployment that configured no token at all,
     * turning a timing defence into an authentication bypass.
     */
    for (std::size_t length = 0; length <= PLAYERBOT_SOCIAL_CONTROL_MIN_TOKEN_LENGTH + 1; ++length)
    {
        for (char const filler : {'\0', '\x7f', static_cast<char>(0xff)})
        {
            /*
             * 0xff is the placeholder's own byte, which is the input that matters. A guess of zeros
             * could never match it and so could never catch a reordering that consulted the
             * comparison before the configuration; this one would, and that reordering would turn a
             * timing defence into an authentication bypass against a deployment with no token at all.
             */
            std::string const guess(length, filler);
            EXPECT_NE(PlayerbotSocialControlAuthenticate(guess, ""), PlayerbotSocialControlAuth::Granted)
                << "length " << length;
            EXPECT_NE(PlayerbotSocialControlAuthenticate(guess, "short"), PlayerbotSocialControlAuth::Granted)
                << "length " << length;
        }
    }
}
