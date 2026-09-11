import io

P = r"D:\Projects\RESUME.md"
L = io.open(P, encoding="utf-8").read().split("\n")

# settled decisions list
d = next(k for k, l in enumerate(L) if l.startswith("## Decisions SETTLED by the user"))
end = d + 1
while end < len(L) and not L[end].startswith("# ") and not L[end].startswith("## "):
    end += 1
last_bullet = max(k for k in range(d, end) if L[k].startswith("- ") or L[k].startswith("* ")) if any(L[k].startswith(("- ", "* ")) for k in range(d, end)) else end - 1
L.insert(last_bullet + 1,
         "- **Reclaim hold = PAUSE THE MATCH (user decision 2026-09-11 05:55 UTC).** While a dropped player's seat is held for reclaim, "
         "the simulation does not advance for anyone: lockstep produces no frames for the held seat and every peer waits, up to the hold "
         "limit. Reclaim or substitution resumes from the held frame (resync snapshot = the held state); hold expiry drops the seat and "
         "the match resumes with the ledgered consequences. Supersedes the H4 §4 'hold the outcome, keep simulating' semantics; announced "
         "(clean) leaves still close the seat immediately with no pause. Rejected alternatives: protect/freeze only the dropped player's "
         "units; keep 'you can lose while disconnected' and move the gates to a non-hostile activity.")

# §B dated entry
j = next(k for k, l in enumerate(L) if l.startswith("**2026-09-11 05:30 UTC - PHASE 1 PROGRESS"))
entry = (
    "**2026-09-11 05:58 UTC - ITEM 6 ROOT CAUSE AND USER DECISION.** W10/W11 traces (`grok-workers/w10-h4-gates/REPORT.md`, "
    "`w11-b1-substitution/REPORT.md`): three of the four long-red H4 gates (`clean_leave` hosts_survived, `reclaim_socket`, "
    "`rejoin_after_resync`) and both red B1 gates fail for ONE reason: `P4AlphaDuel.lua:128-134` ends the match when a team that had a "
    "brain has none; the dropped player's undefended brain is killed by the opponent during the hold window (actors_peak 4 -> 2, "
    "winner_team 0, reseat ledger names no living actors); the engine defers the end while the seat is held (`ActivityMan.cpp:1006-1024`, "
    "`ScenarioRunner.cpp:796-804`) and completes it on the first evaluation after the returner/substitute is seated; the e2e rule "
    "`Over && running_ticks<100` after the relaunch then scores it as a setup failure (`Main.cpp:2935-2938`), `ReportRuntimeError` tears "
    "down the report tree, and the gates read `None`. `fencing_two_transports`: the same brain death with no hold (that client never "
    "dropped), so the host refuses the resync save on an Over activity (`ActivityMan.cpp:140-142`) and kills the session. Secondary "
    "defects found: `UsedStoredTicket` stays true after a refused reclaim falls back to a new join (`NetReconnectSession.cpp:1340-1373`), "
    "the report writer drops the admission counters on the failure path (`NetMatchService.cpp:503-518`, `928-930`), a resync requested "
    "against an Over activity fails the whole session instead of answering 'match over', and the host's own local AI sends a first-frame "
    "`NetGameAIOrder` for a team it does not control (`Actor.cpp:1478`), which the receiver rightly rejects (`MovableMan.cpp:415`). "
    "Reviewer findings A and B of 2026-09-09 are not present at `c8f8188ae0` (A: `NetAdmissionClock` elapsed; B: `TickAdmissionPlane:172` "
    "expires silent handshakes). USER DECISION: the reclaim hold pauses the whole match (see section J settled decisions). The item 6 "
    "fix group is designed on that basis together with the item 2 residuals; the H4/B1 gate oracles are rewritten to the new "
    "semantics (frames must not advance during a hold; resume from the held frame) and are not widened.**"
)
L[j:j] = [entry, ""]
io.open(P, "w", encoding="utf-8", newline="\n").write("\n".join(L))
print("ok", d, last_bullet, j)
