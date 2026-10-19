# The direct fallback test's host answers a mesh start in kind (FLAGGED test correction)

Row `net-directory`, case T2 (`queue_full` signaling falls back to the direct address) and U5 Direct-only. Linux, headed,
single-box, proof=false.

RED on this branch at 22876760 on a quiet box: `T2: queue_full stopped a join that the direct address could complete:
timed out waiting for lockstep start` (`round start timeout ... round_received=0`); cm-dev passes on the same box.
Cause: since f580842ec a peer waiting in a mesh round publishes its start as an authenticated `PeerControl` message
(signed with the frame key the lobby capsule supplies), and every real peer reads it; the test's fake host decoded only
the plain start codec, so it never answered. A temporary trace showed only 246/197/205-byte controls after the lobby
(`runs.txt`).

Correction (8e87f2205): the fake host keeps the frame key it hands out in its capsule and answers a control with a
control, a plain start with a plain start. No assertion changed: the join must still connect by IP, ready the session,
run the coordinator and record the `ip` route. GREEN after.
