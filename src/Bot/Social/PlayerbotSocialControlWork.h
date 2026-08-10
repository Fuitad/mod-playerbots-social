/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef PLAYERBOTS_PLAYERBOTSOCIALCONTROLWORK_H
#define PLAYERBOTS_PLAYERBOTSOCIALCONTROLWORK_H

#include <memory>
#include <string>
#include <string_view>

#include "Bot/Social/PlayerbotSocialControl.h"
#include "Script/WorldThr/PlayerbotOperation.h"

/*
 * Carries an administrative control from the command port's socket thread to the world thread.
 *
 * Split from PlayerbotSocialControl.h, which holds the decisions this file obeys, because those are
 * total functions of their arguments and are unit tested as values, while everything here needs a
 * running world. The same split the MCP verification path uses, for the same reason: a decision that
 * can only be exercised against a live server is a decision that does not get exercised.
 */

/*
 * Runs one control on the world thread.
 *
 * Named Work rather than Operation because PlayerbotSocialControlOperation is already the ENUM of
 * the nine things an operator may ask for. One is the request's vocabulary and the other is the unit
 * of world thread work that carries it, and letting them share a name made the two collide.
 *
 * Stores the parsed request by value and nothing else. No Player, no PlayerbotAI, no live pointer of
 * any kind crosses the thread boundary, so a bot logging out between the submit and the tick cannot
 * leave this holding a dangling reference.
 */
class PlayerbotSocialControlWork : public PlayerbotOperation
{
public:
    PlayerbotSocialControlWork(PlayerbotSocialControlRequest request,
                               std::shared_ptr<PlayerbotSocialControlResult> result);

    bool Execute() override;

    // Always executed, so a queued control completes with a typed answer rather than being silently
    // skipped and leaving its caller to time out for a reason nothing recorded.
    bool IsValid() const override { return true; }

    /*
     * Above ordinary background work and below the crash prevention band.
     *
     * A control is an operator waiting on a socket with a five second deadline, so it should not
     * queue behind a tick of statistics. It is still not urgent enough to precede cleanup.
     */
    uint32 GetPriority() const override { return 50; }

    std::string GetName() const override { return "Playerbot social control"; }

private:
    PlayerbotSocialControlRequest request;
    std::shared_ptr<PlayerbotSocialControlResult> result;
};

// Opened once the world is initialised and closed during shutdown, so a control cannot be applied
// against a half built or half torn down world.
void SetPlayerbotSocialControlAcceptingRequests(bool accepting);
[[nodiscard]] bool IsPlayerbotSocialControlAcceptingRequests();

/*
 * Handles one command port line from the socket thread, start to finish.
 *
 * Returns the exact text to write back, or nothing at all when the line was not a control, which is
 * the signal for the command port to pass it to the handler that owns it. Definition of Done 2 lives
 * on that distinction: telemetry and inspect must reach their handlers untouched.
 *
 * Blocks the calling connection thread for at most the control timeout. That thread exists to serve
 * one connection, so waiting on it costs nothing the world thread notices.
 */
[[nodiscard]] bool PlayerbotSocialControlHandleLine(std::string_view line, std::string& response);

#endif
