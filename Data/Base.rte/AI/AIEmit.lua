-- AIEmit.lua
--
-- Lua-side wrapper for the C++ AIDecisionChannel.
--
-- Drop dispatch-level AIEmit(...) calls at decision points in the AI Lua. The
-- M0 trust suite + the per-tick "decisions" subsystem checksum + the in-game
-- debug overlay all read from the channel.
--
-- Args (all optional except actor):
--   actor    : the deciding actor (provides UniqueID)
--   layer    : "reflex" | "locomotion" | "squad" | "decision"  (default: "decision")
--   decisionType : short string id, e.g. "target_acquired", "behaviour_selected"
--   chosen   : short string id of what was picked, e.g. "ShootTarget", "GoToWpt"
--   reason   : human-readable why, e.g. "enemy in line-of-sight"
--   target   : optional target actor; its UniqueID + position is recorded
--   targetX  : optional target x (used when target is a Vector, not an actor)
--   targetY  : optional target y

function AIEmit(actor, layer, decisionType, chosen, reason, target, targetX, targetY)
    if not AIDecisionChannel then return; end
    local actorId = -1;
    if actor and actor.UniqueID then actorId = actor.UniqueID; end
    local targetId = -1;
    local tx = targetX or 0;
    local ty = targetY or 0;
    if target then
        if target.UniqueID then
            targetId = target.UniqueID;
        end
        if target.Pos then
            tx = target.Pos.X;
            ty = target.Pos.Y;
        end
    end
    AIDecisionChannel:EmitWithTarget(actorId,
                                     layer or "decision",
                                     decisionType or "",
                                     chosen or "",
                                     reason or "",
                                     targetId,
                                     tx,
                                     ty);
end
