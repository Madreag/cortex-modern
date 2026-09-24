# The rejoin state x event matrix

Every (seat state, event) pair of the rejoin machine with the outcome the policy expects and the line that says so. The subject is one
client seat (peer 2) of a bounded-wait round; the host is peer 1. The self-test `-net-rejoin-matrix-selftest` (row `net-rejoin-matrix` of
`tools/run_selftests.py`) prints this table from its own rows and walks every pair marked `walked` in process: it drives a fresh
host/client coordinator pair and a host/client session pair into the state, delivers the event, runs 500 ms and compares what the
machine did with the expected tokens. This file is generated from that run's `table` lines; edit the rows in
`Source/Network/NetRejoinMatrixSelfTest.cpp`, never here.

Tier 1 is the match path (hold, park, capture, relaunch, the rejoin phases, goodbye, link blip and restore, kick, ban, cap). Tier 2 is
world images, migration, late join and resume from disk. A pair the rig cannot reach is marked `not walked` with the reason; the
arms and scenarios of the inventory cover it.

## Expected-outcome tokens

| token | reads |
|---|---|
| `seat=X` | the subject seat as the host's coordinator reads it: Held (an AI hold with no agreed reclaim), Reclaiming (a reclaim agreed, before E), Left (a leave frame, no hold), Active. |
| `round=run\|over\|relaunch\|ended` | the host coordinator: Running; Stopped on Complete; Failed on ResyncRequested; anything but Running. |
| `peer=run\|held\|left\|over\|relaunch\|noresync` | the subject's own coordinator; noresync = anything but a resync. |
| `sess=ready\|alive\|ended\|ended:Reason\|phase:X` | the subject's session: Ready; not ended; ended; ended with that reject reason; still in rejoin phase X. |
| `holds=0 / holds>0` | holds the host counted on the seat after the state was entered. |
| `api=ok\|refused` | the return of the call that delivers the event. |

A walked expectation is a list of tokens that must all hold. A not-walked expectation is written out in words.

## Sources

| id | kind | line |
|---|---|---|
| RB2 | RULING/PLAN | ROLLBACK.md section 3.1 item 2: after the slow-player bound (default 3 ticks = 50 ms) the host holds the seat at an agreed frame H (HoldAtFrame); the survivors keep committing; a two-peer round does not end when the human is held. |
| RB3 | RULING/PLAN | ROLLBACK.md section 3.1 item 3: from H the held client's input authority is revoked and its backlog fenced; it returns only by a reclaim at a future frame beyond every input it sent. |
| R1F7 | RULING | opus-post-merge-netcode-rulings-20260923.txt FINDING 7 (1): a client's Complete is that client's leave, never the world's end; the seat drains and goes to the AI as a leave does. |
| R1-392ii | RULING | opus-post-merge-netcode-rulings-20260923.txt 392 (ii): a held client whose host is gone takes host migration when survivors exist; with no survivor it leaves to the landing with 'The host left the match' - never a resync attempt against a gone host, never a hang. |
| R2D3 | RULING | opus-post-merge-netcode-rulings-2-20260923.txt D3: a held seat's returner always reclaims its held seat at the agreed frame E; new-member admission is for new identities only. |
| R2WAY2 | RULING | opus-post-merge-netcode-rulings-2-20260923.txt WAY 2: a restarted world's opening resume offer is retired once the round runs past its anchor; a rejoin then takes the image path with the newest image. |
| R2D2 | RULING | opus-post-merge-netcode-rulings-2-20260923.txt D2: a held world seat takes the newest image; a peer's coverage starts at its first tick or the image it loaded; a held match client replays its own committed tail. |
| R2-392 | RULING | opus-post-merge-netcode-rulings-2-20260923.txt 392: a client with no committed frame goes to the landing on a host drop; one with committed frames takes the bounded H4 reconnect, then the landing. |
| R2-206 | RULING | opus-post-merge-netcode-rulings-2-20260923.txt 206: the held client's base image refreshes by the steady capture cost (rolling median of three captures). |
| H4-0 | PLAN | STAGE2_H4_RECONNECT_PLAN.md section 0: admission traffic (an unbound transport, a reconnect handshake) can never stop or mutate the running match. |
| H4-4 | PLAN | STAGE2_H4_RECONNECT_PLAN.md section 4: a reclaim addresses a seat's held incarnation by its stable id. |
| H4-6 | PLAN | STAGE2_H4_RECONNECT_PLAN.md section 6: a newly proven connection replaces the old transport for the seat; the superseded one is fenced. |
| H4-7 | PLAN | STAGE2_H4_RECONNECT_PLAN.md section 7: live-match ticketless joins are denied; a clean leave revokes the holder's ticket; SessionEnded is the one confirmed end. |
| READY-4 | PLAN | CLAUDE.md PHASE 3b READY BAR v2 item 4: resume from disk reloads the match from its newest checkpoint on every peer. |
| NS-PHASE | DESIGN | Source/Network/NetSession.h RejoinPhase comment: in every rejoin phase a transport close still ends the link and the host's goodbye completes the seat. |
| NP-KICK | DESIGN | Source/Network/NetProtocol.h NetRejectReason: ParticipantRemoved = the holder is gone for good; ParticipantBanned = refused for the named scope. |
| LS-STOP | DESIGN | Source/Network/NetLockstep.h NetLockstepStopReason: PeerLeft keeps the survivors going; ResyncRequested ends the round for a reload; Reclaimed/Expired resolve a held seat only. |
| LS-PARK | DESIGN | NetLockstep.cpp DeclareOverdueInputs: an agreed capture park is not evidence that a seat stopped; netcode report RESUME 1 F1: a seat is due a delay after the park's last frame. |
| LS-DRAIN | DESIGN | Source/Network/NetLockstep.h SetGoodbyeDrain: the round has run its last tick and judges no seat. |
| C5102 | DESIGN | commit 5102bb2c54: no seat activation is agreed where a capture park can cover it. |
| DESIGN-MIGRATION | DESIGN | NetLockstep.cpp RestartHostMigrationAfterSuccessorLoss / BeginHostMigration. |
| GAP | NONE | no ruling, plan or design line gives the outcome; the table carries the conservative outcome (refuse or ignore) and the pair is in the gap list. |

## Active

| event | tier | expected outcome | source | gap | walk |
|---|---|---|---|---|---|
| hold-proposed | 1 | seat=Held round=run peer=held holds>0 sess=ready | RB2 |  | walked |
| hold-resolved | 1 | seat=Active round=run holds=0 sess=ready | LS-STOP |  | walked |
| park-begin | 1 | seat=Active round=run peer=run holds=0 sess=ready | LS-PARK |  | walked |
| park-end | 1 | seat=Active round=run peer=run holds=0 sess=ready | GAP | GAP: a park end with no park open | walked |
| private-capture-complete | 1 | ignore: no seat waits on that image (conservative) | GAP | GAP: a private image completing for a seat that is not waiting on one | not walked: a NetMatchService step (private capture writer, world join, checkpoint resume) that needs an activity load; the in-process rig has none |
| world-image-offered | 2 | ignore: no seat waits on an image (conservative) | GAP | GAP: a world image offered to a seat that is not waiting on one | not walked: a NetMatchService step (private capture writer, world join, checkpoint resume) that needs an activity load; the in-process rig has none |
| opening-resume-offer | 2 | refuse: the opening resume offer is retired once the round runs past its anchor; a rejoin takes the image path with the newest image | R2WAY2 |  | not walked: a NetMatchService step (private capture writer, world join, checkpoint resume) that needs an activity load; the in-process rig has none |
| tail-replay-complete | 1 | n/a: the completion is raised only by the seat's own tail replay | NS-PHASE |  | not walked: a NetMatchService step (private capture writer, world join, checkpoint resume) that needs an activity load; the in-process rig has none |
| resync-relaunch | 1 | round=relaunch peer=relaunch sess=ready | LS-STOP |  | walked |
| host-goodbye | 1 | round=ended peer=noresync sess=ended | R1-392ii R2-392 H4-7 |  | walked |
| host-lost | 1 | peer=noresync sess=ended | R1-392ii R2-392 |  | walked |
| migration-begin | 2 | api=refused round=run | R1-392ii |  | walked |
| migration-complete | 2 | n/a: raised only while a host migration runs | DESIGN-MIGRATION |  | not walked: needs a migration in progress (three peers); not composed in this row |
| migration-fail | 2 | n/a: raised only while a host migration runs | DESIGN-MIGRATION |  | not walked: needs a migration in progress (three peers); not composed in this row |
| migration-successor-lost | 2 | n/a: raised only while a host migration runs | DESIGN-MIGRATION |  | not walked: needs a migration in progress (three peers); not composed in this row |
| late-join | 2 | api=refused round=run sess=ready | H4-7 R2D3 |  | walked |
| ticket-rejoin | 1 | seat=Active round=run peer=run holds=0 sess=ready | H4-0 H4-6 |  | walked |
| held-rejoin | 1 | api=refused seat=Active round=run sess=ready | H4-4 R2D3 |  | walked |
| kick | 1 | seat=Left round=run sess=ended:ParticipantRemoved | NP-KICK LS-STOP | GAP: a round whose last remote human is removed or released: no ruling says whether it ends; the conservative expectation keeps it running | walked |
| ban | 1 | seat=Left round=run sess=ended:ParticipantBanned | NP-KICK LS-STOP | GAP: a round whose last remote human is removed or released: no ruling says whether it ends; the conservative expectation keeps it running | walked |
| seat-release | 1 | seat=Active round=run holds=0 sess=ready | LS-STOP |  | walked |
| own-cap | 1 | seat=Held round=run sess=ended | R1F7 | GAP: R1 F7 rules a client's own end for a persistent world; for a match the conservative reading keeps the round running and hands the seat to the AI as a leave does | walked |
| resume-from-disk | 2 | refuse: a running round is not replaced by a disk resume (conservative) | GAP | GAP: a resume from disk requested while a round runs | not walked: a NetMatchService step (private capture writer, world join, checkpoint resume) that needs an activity load; the in-process rig has none |
| match-over | 1 | round=over peer=over sess=ended | LS-STOP NS-PHASE |  | walked |
| link-blip | 1 | seat=Active round=run peer=run holds=0 sess=ready | RB2 |  | walked |
| link-restore | 1 | seat=Active round=run peer=run holds=0 sess=ready | RB3 |  | walked |

## Held

| event | tier | expected outcome | source | gap | walk |
|---|---|---|---|---|---|
| hold-proposed | 1 | seat=Held peer=held round=run holds=0 sess=ready | RB2 |  | walked |
| hold-resolved | 1 | seat=Held peer=held round=run sess=ready | RB3 R2D3 |  | walked |
| park-begin | 1 | seat=Held peer=held round=run holds=0 sess=ready | LS-PARK |  | walked |
| park-end | 1 | seat=Held peer=held round=run holds=0 sess=ready | GAP | GAP: a park end with no park open | walked |
| private-capture-complete | 1 | ignore: the base image is kept for the seat's next rejoin (the steady-cost refresh rule) | R2-206 |  | not walked: a NetMatchService step (private capture writer, world join, checkpoint resume) that needs an activity load; the in-process rig has none |
| world-image-offered | 2 | ignore: no seat waits on an image (conservative) | GAP | GAP: a world image offered to a seat that is not waiting on one | not walked: a NetMatchService step (private capture writer, world join, checkpoint resume) that needs an activity load; the in-process rig has none |
| opening-resume-offer | 2 | refuse: the opening resume offer is retired once the round runs past its anchor; a rejoin takes the image path with the newest image | R2WAY2 |  | not walked: a NetMatchService step (private capture writer, world join, checkpoint resume) that needs an activity load; the in-process rig has none |
| tail-replay-complete | 1 | n/a: the completion is raised only by the seat's own tail replay | NS-PHASE |  | not walked: a NetMatchService step (private capture writer, world join, checkpoint resume) that needs an activity load; the in-process rig has none |
| resync-relaunch | 1 | round=relaunch seat=Held sess=ready | LS-STOP |  | walked |
| host-goodbye | 1 | round=ended peer=held sess=ended | R1-392ii H4-7 |  | walked |
| host-lost | 1 | peer=held sess=ended | R1-392ii |  | walked |
| migration-begin | 2 | api=refused round=run | R1-392ii |  | walked |
| migration-complete | 2 | n/a: raised only while a host migration runs | DESIGN-MIGRATION |  | not walked: needs a migration in progress (three peers); not composed in this row |
| migration-fail | 2 | n/a: raised only while a host migration runs | DESIGN-MIGRATION |  | not walked: needs a migration in progress (three peers); not composed in this row |
| migration-successor-lost | 2 | n/a: raised only while a host migration runs | DESIGN-MIGRATION |  | not walked: needs a migration in progress (three peers); not composed in this row |
| late-join | 2 | api=refused round=run sess=ready | H4-7 R2D3 |  | walked |
| ticket-rejoin | 1 | seat=Held peer=held round=run sess=ready | H4-0 |  | walked |
| held-rejoin | 1 | api=ok seat=Reclaiming round=run sess=ready | RB3 R2D3 |  | walked |
| kick | 1 | seat=Left round=run sess=ended:ParticipantRemoved | NP-KICK LS-STOP | GAP: a round whose last remote human is removed or released: no ruling says whether it ends; the conservative expectation keeps it running | walked |
| ban | 1 | seat=Left round=run sess=ended:ParticipantBanned | NP-KICK LS-STOP | GAP: a round whose last remote human is removed or released: no ruling says whether it ends; the conservative expectation keeps it running | walked |
| seat-release | 1 | seat=Left round=run sess=ready | LS-STOP | GAP: a round whose last remote human is removed or released: no ruling says whether it ends; the conservative expectation keeps it running | walked |
| own-cap | 1 | seat=Held round=run sess=ended | R1F7 |  | walked |
| resume-from-disk | 2 | refuse: a running round is not replaced by a disk resume (conservative) | GAP | GAP: a resume from disk requested while a round runs | not walked: a NetMatchService step (private capture writer, world join, checkpoint resume) that needs an activity load; the in-process rig has none |
| match-over | 1 | round=over sess=ended | NS-PHASE |  | walked |
| link-blip | 1 | seat=Held peer=held round=run sess=ready | RB3 |  | walked |
| link-restore | 1 | seat=Held peer=held round=run sess=ready | RB3 |  | walked |

## Reclaiming

| event | tier | expected outcome | source | gap | walk |
|---|---|---|---|---|---|
| hold-proposed | 1 | seat=Reclaiming round=run sess=ready | GAP | GAP: a hold proposed for a seat whose reclaim is agreed but not active yet (nothing is owed by it before E) | walked |
| hold-resolved | 1 | seat=Reclaiming round=run sess=ready | RB3 R2D3 |  | walked |
| park-begin | 1 | seat=Reclaiming round=run holds=0 sess=ready | C5102 |  | walked |
| park-end | 1 | seat=Reclaiming round=run sess=ready | GAP | GAP: a park end with no park open | walked |
| private-capture-complete | 1 | ignore: no seat waits on that image (conservative) | GAP | GAP: a private image completing for a seat that is not waiting on one | not walked: a NetMatchService step (private capture writer, world join, checkpoint resume) that needs an activity load; the in-process rig has none |
| world-image-offered | 2 | ignore: no seat waits on an image (conservative) | GAP | GAP: a world image offered to a seat that is not waiting on one | not walked: a NetMatchService step (private capture writer, world join, checkpoint resume) that needs an activity load; the in-process rig has none |
| opening-resume-offer | 2 | refuse: the opening resume offer is retired once the round runs past its anchor; a rejoin takes the image path with the newest image | R2WAY2 |  | not walked: a NetMatchService step (private capture writer, world join, checkpoint resume) that needs an activity load; the in-process rig has none |
| tail-replay-complete | 1 | n/a: the completion is raised only by the seat's own tail replay | NS-PHASE |  | not walked: a NetMatchService step (private capture writer, world join, checkpoint resume) that needs an activity load; the in-process rig has none |
| resync-relaunch | 1 | round=relaunch sess=ready | LS-STOP | GAP: what an agreed reclaim becomes across a relaunch is not ruled | walked |
| host-goodbye | 1 | round=ended peer=held sess=ended | R1-392ii H4-7 |  | walked |
| host-lost | 1 | peer=held sess=ended | R1-392ii |  | walked |
| migration-begin | 2 | api=refused round=run | R1-392ii |  | walked |
| migration-complete | 2 | n/a: raised only while a host migration runs | DESIGN-MIGRATION |  | not walked: needs a migration in progress (three peers); not composed in this row |
| migration-fail | 2 | n/a: raised only while a host migration runs | DESIGN-MIGRATION |  | not walked: needs a migration in progress (three peers); not composed in this row |
| migration-successor-lost | 2 | n/a: raised only while a host migration runs | DESIGN-MIGRATION |  | not walked: needs a migration in progress (three peers); not composed in this row |
| late-join | 2 | api=refused round=run sess=ready | H4-7 R2D3 |  | walked |
| ticket-rejoin | 1 | seat=Reclaiming round=run sess=ready | H4-0 |  | walked |
| held-rejoin | 1 | api=ok seat=Reclaiming round=run sess=ready | H4-6 |  | walked |
| kick | 1 | seat=Left round=run sess=ended:ParticipantRemoved | NP-KICK LS-STOP | GAP: a round whose last remote human is removed or released: no ruling says whether it ends; the conservative expectation keeps it running | walked |
| ban | 1 | seat=Left round=run sess=ended:ParticipantBanned | NP-KICK LS-STOP | GAP: a round whose last remote human is removed or released: no ruling says whether it ends; the conservative expectation keeps it running | walked |
| seat-release | 1 | seat=Reclaiming round=run sess=ready | GAP | GAP: a release of a seat whose reclaim is agreed | walked |
| own-cap | 1 | seat=Reclaiming round=run sess=ended | GAP | GAP: a returner that reaches its own cap before its activation frame | walked |
| resume-from-disk | 2 | refuse: a running round is not replaced by a disk resume (conservative) | GAP | GAP: a resume from disk requested while a round runs | not walked: a NetMatchService step (private capture writer, world join, checkpoint resume) that needs an activity load; the in-process rig has none |
| match-over | 1 | round=over sess=ended | NS-PHASE |  | walked |
| link-blip | 1 | seat=Reclaiming round=run sess=ready | RB3 |  | walked |
| link-restore | 1 | seat=Reclaiming round=run sess=ready | RB3 |  | walked |

## Rejoin:Connecting

| event | tier | expected outcome | source | gap | walk |
|---|---|---|---|---|---|
| hold-proposed | 1 | seat=Held round=run holds=0 sess=alive | RB2 |  | walked |
| hold-resolved | 1 | seat=Held round=run sess=alive | RB3 R2D3 |  | walked |
| park-begin | 1 | seat=Held round=run holds=0 sess=alive | LS-PARK |  | walked |
| park-end | 1 | seat=Held round=run holds=0 sess=alive | GAP | GAP: a park end with no park open | walked |
| private-capture-complete | 1 | ignore: no seat waits on that image (conservative) | GAP | GAP: a private image completing for a seat that is not waiting on one | not walked: a NetMatchService step (private capture writer, world join, checkpoint resume) that needs an activity load; the in-process rig has none |
| world-image-offered | 2 | ignore: no seat waits on an image (conservative) | GAP | GAP: a world image offered to a seat that is not waiting on one | not walked: a NetMatchService step (private capture writer, world join, checkpoint resume) that needs an activity load; the in-process rig has none |
| opening-resume-offer | 2 | refuse: the opening resume offer is retired once the round runs past its anchor; a rejoin takes the image path with the newest image | R2WAY2 |  | not walked: a NetMatchService step (private capture writer, world join, checkpoint resume) that needs an activity load; the in-process rig has none |
| tail-replay-complete | 1 | n/a: the completion is raised only by the seat's own tail replay | NS-PHASE |  | not walked: a NetMatchService step (private capture writer, world join, checkpoint resume) that needs an activity load; the in-process rig has none |
| resync-relaunch | 1 | round=relaunch sess=alive | LS-STOP | GAP: a relaunch while the seat's rejoin is in flight: the conservative expectation keeps the rejoin's session | walked |
| host-goodbye | 1 | round=ended sess=ended | NS-PHASE R1-392ii |  | walked |
| host-lost | 1 | sess=ended | NS-PHASE |  | walked |
| migration-begin | 2 | api=refused round=run | R1-392ii |  | walked |
| migration-complete | 2 | n/a: raised only while a host migration runs | DESIGN-MIGRATION |  | not walked: needs a migration in progress (three peers); not composed in this row |
| migration-fail | 2 | n/a: raised only while a host migration runs | DESIGN-MIGRATION |  | not walked: needs a migration in progress (three peers); not composed in this row |
| migration-successor-lost | 2 | n/a: raised only while a host migration runs | DESIGN-MIGRATION |  | not walked: needs a migration in progress (three peers); not composed in this row |
| late-join | 2 | api=refused round=run sess=alive | H4-7 R2D3 |  | walked |
| ticket-rejoin | 1 | seat=Held round=run sess=alive | H4-0 |  | walked |
| held-rejoin | 1 | api=ok seat=Reclaiming round=run sess=alive | RB3 R2D3 |  | walked |
| kick | 1 | seat=Left round=run | NP-KICK LS-STOP | GAP: a round whose last remote human is removed or released: no ruling says whether it ends; the conservative expectation keeps it running | walked; coordinator half only: the handshaking returner is refused by the admission plane (NetReconnectHost), which the rig does not compose |
| ban | 1 | seat=Left round=run | NP-KICK LS-STOP | GAP: a round whose last remote human is removed or released: no ruling says whether it ends; the conservative expectation keeps it running | walked; coordinator half only: the handshaking returner is refused by the admission plane (NetReconnectHost), which the rig does not compose |
| seat-release | 1 | seat=Left round=run | LS-STOP | GAP: a round whose last remote human is removed or released: no ruling says whether it ends; the conservative expectation keeps it running; and nothing rules what a rejoining client whose seat was released is told | walked |
| own-cap | 1 | seat=Held round=run sess=ended | R1F7 |  | walked |
| resume-from-disk | 2 | refuse: a running round is not replaced by a disk resume (conservative) | GAP | GAP: a resume from disk requested while a round runs | not walked: a NetMatchService step (private capture writer, world join, checkpoint resume) that needs an activity load; the in-process rig has none |
| match-over | 1 | round=over sess=ended | NS-PHASE |  | walked |
| link-blip | 1 | seat=Held round=run sess=alive | RB3 |  | walked |
| link-restore | 1 | seat=Held round=run sess=alive | RB3 |  | walked |

## Rejoin:ImagePending

| event | tier | expected outcome | source | gap | walk |
|---|---|---|---|---|---|
| hold-proposed | 1 | seat=Held round=run holds=0 sess=phase:ImagePending | RB2 |  | walked |
| hold-resolved | 1 | seat=Held round=run sess=phase:ImagePending | RB3 R2D3 |  | walked |
| park-begin | 1 | seat=Held round=run holds=0 sess=phase:ImagePending | LS-PARK |  | walked |
| park-end | 1 | seat=Held round=run holds=0 sess=phase:ImagePending | GAP | GAP: a park end with no park open | walked |
| private-capture-complete | 1 | legal: ImagePending -> Loading on the newest base image | R2-206 R2D2 |  | not walked: a NetMatchService step (private capture writer, world join, checkpoint resume) that needs an activity load; the in-process rig has none |
| world-image-offered | 2 | legal: ImagePending -> Loading on the newest image; coverage starts at the loaded image | R2D2 |  | not walked: a NetMatchService step (private capture writer, world join, checkpoint resume) that needs an activity load; the in-process rig has none |
| opening-resume-offer | 2 | refuse: the opening resume offer is retired once the round runs past its anchor; a rejoin takes the image path with the newest image | R2WAY2 |  | not walked: a NetMatchService step (private capture writer, world join, checkpoint resume) that needs an activity load; the in-process rig has none |
| tail-replay-complete | 1 | n/a: the completion is raised only by the seat's own tail replay | NS-PHASE |  | not walked: a NetMatchService step (private capture writer, world join, checkpoint resume) that needs an activity load; the in-process rig has none |
| resync-relaunch | 1 | round=relaunch sess=phase:ImagePending | LS-STOP | GAP: a relaunch while the seat's rejoin is in flight: the conservative expectation keeps the rejoin's session | walked |
| host-goodbye | 1 | round=ended sess=ended | NS-PHASE R1-392ii |  | walked |
| host-lost | 1 | sess=ended | NS-PHASE |  | walked |
| migration-begin | 2 | api=refused round=run | R1-392ii |  | walked |
| migration-complete | 2 | n/a: raised only while a host migration runs | DESIGN-MIGRATION |  | not walked: needs a migration in progress (three peers); not composed in this row |
| migration-fail | 2 | n/a: raised only while a host migration runs | DESIGN-MIGRATION |  | not walked: needs a migration in progress (three peers); not composed in this row |
| migration-successor-lost | 2 | n/a: raised only while a host migration runs | DESIGN-MIGRATION |  | not walked: needs a migration in progress (three peers); not composed in this row |
| late-join | 2 | api=refused round=run sess=phase:ImagePending | H4-7 R2D3 |  | walked |
| ticket-rejoin | 1 | seat=Held round=run sess=phase:ImagePending | H4-0 |  | walked |
| held-rejoin | 1 | api=ok seat=Reclaiming round=run sess=phase:ImagePending | RB3 R2D3 |  | walked |
| kick | 1 | seat=Left round=run sess=ended:ParticipantRemoved | NP-KICK LS-STOP | GAP: a round whose last remote human is removed or released: no ruling says whether it ends; the conservative expectation keeps it running | walked |
| ban | 1 | seat=Left round=run sess=ended:ParticipantBanned | NP-KICK LS-STOP | GAP: a round whose last remote human is removed or released: no ruling says whether it ends; the conservative expectation keeps it running | walked |
| seat-release | 1 | seat=Left round=run | LS-STOP | GAP: a round whose last remote human is removed or released: no ruling says whether it ends; the conservative expectation keeps it running; and nothing rules what a rejoining client whose seat was released is told | walked |
| own-cap | 1 | seat=Held round=run sess=ended | R1F7 |  | walked |
| resume-from-disk | 2 | refuse: a running round is not replaced by a disk resume (conservative) | GAP | GAP: a resume from disk requested while a round runs | not walked: a NetMatchService step (private capture writer, world join, checkpoint resume) that needs an activity load; the in-process rig has none |
| match-over | 1 | round=over sess=ended | NS-PHASE |  | walked |
| link-blip | 1 | seat=Held round=run sess=phase:ImagePending | RB3 |  | walked |
| link-restore | 1 | seat=Held round=run sess=phase:ImagePending | RB3 |  | walked |

## Rejoin:Loading

| event | tier | expected outcome | source | gap | walk |
|---|---|---|---|---|---|
| hold-proposed | 1 | seat=Held round=run holds=0 sess=phase:Loading | RB2 |  | walked |
| hold-resolved | 1 | seat=Held round=run sess=phase:Loading | RB3 R2D3 |  | walked |
| park-begin | 1 | seat=Held round=run holds=0 sess=phase:Loading | LS-PARK |  | walked |
| park-end | 1 | seat=Held round=run holds=0 sess=phase:Loading | GAP | GAP: a park end with no park open | walked |
| private-capture-complete | 1 | ignore: no seat waits on that image (conservative) | GAP | GAP: a private image completing for a seat that is not waiting on one | not walked: a NetMatchService step (private capture writer, world join, checkpoint resume) that needs an activity load; the in-process rig has none |
| world-image-offered | 2 | ignore: the load in progress finishes first (conservative) | GAP | GAP: a newer world image offered while an older one loads or replays | not walked: a NetMatchService step (private capture writer, world join, checkpoint resume) that needs an activity load; the in-process rig has none |
| opening-resume-offer | 2 | refuse: the opening resume offer is retired once the round runs past its anchor; a rejoin takes the image path with the newest image | R2WAY2 |  | not walked: a NetMatchService step (private capture writer, world join, checkpoint resume) that needs an activity load; the in-process rig has none |
| tail-replay-complete | 1 | n/a: the completion is raised only by the seat's own tail replay | NS-PHASE |  | not walked: a NetMatchService step (private capture writer, world join, checkpoint resume) that needs an activity load; the in-process rig has none |
| resync-relaunch | 1 | round=relaunch sess=phase:Loading | LS-STOP | GAP: a relaunch while the seat's rejoin is in flight: the conservative expectation keeps the rejoin's session | walked |
| host-goodbye | 1 | round=ended sess=ended | NS-PHASE R1-392ii |  | walked |
| host-lost | 1 | sess=ended | NS-PHASE |  | walked |
| migration-begin | 2 | api=refused round=run | R1-392ii |  | walked |
| migration-complete | 2 | n/a: raised only while a host migration runs | DESIGN-MIGRATION |  | not walked: needs a migration in progress (three peers); not composed in this row |
| migration-fail | 2 | n/a: raised only while a host migration runs | DESIGN-MIGRATION |  | not walked: needs a migration in progress (three peers); not composed in this row |
| migration-successor-lost | 2 | n/a: raised only while a host migration runs | DESIGN-MIGRATION |  | not walked: needs a migration in progress (three peers); not composed in this row |
| late-join | 2 | api=refused round=run sess=phase:Loading | H4-7 R2D3 |  | walked |
| ticket-rejoin | 1 | seat=Held round=run sess=phase:Loading | H4-0 |  | walked |
| held-rejoin | 1 | api=ok seat=Reclaiming round=run sess=phase:Loading | RB3 R2D3 |  | walked |
| kick | 1 | seat=Left round=run sess=ended:ParticipantRemoved | NP-KICK LS-STOP | GAP: a round whose last remote human is removed or released: no ruling says whether it ends; the conservative expectation keeps it running | walked |
| ban | 1 | seat=Left round=run sess=ended:ParticipantBanned | NP-KICK LS-STOP | GAP: a round whose last remote human is removed or released: no ruling says whether it ends; the conservative expectation keeps it running | walked |
| seat-release | 1 | seat=Left round=run | LS-STOP | GAP: a round whose last remote human is removed or released: no ruling says whether it ends; the conservative expectation keeps it running; and nothing rules what a rejoining client whose seat was released is told | walked |
| own-cap | 1 | seat=Held round=run sess=ended | R1F7 |  | walked |
| resume-from-disk | 2 | refuse: a running round is not replaced by a disk resume (conservative) | GAP | GAP: a resume from disk requested while a round runs | not walked: a NetMatchService step (private capture writer, world join, checkpoint resume) that needs an activity load; the in-process rig has none |
| match-over | 1 | round=over sess=ended | NS-PHASE |  | walked |
| link-blip | 1 | seat=Held round=run sess=phase:Loading | RB3 |  | walked |
| link-restore | 1 | seat=Held round=run sess=phase:Loading | RB3 |  | walked |

## Rejoin:TailReplay

| event | tier | expected outcome | source | gap | walk |
|---|---|---|---|---|---|
| hold-proposed | 1 | seat=Held round=run holds=0 sess=phase:TailReplay | RB2 |  | walked |
| hold-resolved | 1 | seat=Held round=run sess=phase:TailReplay | RB3 R2D3 |  | walked |
| park-begin | 1 | seat=Held round=run holds=0 sess=phase:TailReplay | LS-PARK |  | walked |
| park-end | 1 | seat=Held round=run holds=0 sess=phase:TailReplay | GAP | GAP: a park end with no park open | walked |
| private-capture-complete | 1 | ignore: no seat waits on that image (conservative) | GAP | GAP: a private image completing for a seat that is not waiting on one | not walked: a NetMatchService step (private capture writer, world join, checkpoint resume) that needs an activity load; the in-process rig has none |
| world-image-offered | 2 | ignore: the load in progress finishes first (conservative) | GAP | GAP: a newer world image offered while an older one loads or replays | not walked: a NetMatchService step (private capture writer, world join, checkpoint resume) that needs an activity load; the in-process rig has none |
| opening-resume-offer | 2 | refuse: the opening resume offer is retired once the round runs past its anchor; a rejoin takes the image path with the newest image | R2WAY2 |  | not walked: a NetMatchService step (private capture writer, world join, checkpoint resume) that needs an activity load; the in-process rig has none |
| tail-replay-complete | 1 | legal: TailReplay -> Active; the seat plays live from its activation | R2D2 RB3 |  | not walked: a NetMatchService step (private capture writer, world join, checkpoint resume) that needs an activity load; the in-process rig has none |
| resync-relaunch | 1 | round=relaunch sess=phase:TailReplay | LS-STOP | GAP: a relaunch while the seat's rejoin is in flight: the conservative expectation keeps the rejoin's session | walked |
| host-goodbye | 1 | round=ended sess=ended | NS-PHASE R1-392ii |  | walked |
| host-lost | 1 | sess=ended | NS-PHASE |  | walked |
| migration-begin | 2 | api=refused round=run | R1-392ii |  | walked |
| migration-complete | 2 | n/a: raised only while a host migration runs | DESIGN-MIGRATION |  | not walked: needs a migration in progress (three peers); not composed in this row |
| migration-fail | 2 | n/a: raised only while a host migration runs | DESIGN-MIGRATION |  | not walked: needs a migration in progress (three peers); not composed in this row |
| migration-successor-lost | 2 | n/a: raised only while a host migration runs | DESIGN-MIGRATION |  | not walked: needs a migration in progress (three peers); not composed in this row |
| late-join | 2 | api=refused round=run sess=phase:TailReplay | H4-7 R2D3 |  | walked |
| ticket-rejoin | 1 | seat=Held round=run sess=phase:TailReplay | H4-0 |  | walked |
| held-rejoin | 1 | api=ok seat=Reclaiming round=run sess=phase:TailReplay | RB3 R2D3 |  | walked |
| kick | 1 | seat=Left round=run sess=ended:ParticipantRemoved | NP-KICK LS-STOP | GAP: a round whose last remote human is removed or released: no ruling says whether it ends; the conservative expectation keeps it running | walked |
| ban | 1 | seat=Left round=run sess=ended:ParticipantBanned | NP-KICK LS-STOP | GAP: a round whose last remote human is removed or released: no ruling says whether it ends; the conservative expectation keeps it running | walked |
| seat-release | 1 | seat=Left round=run | LS-STOP | GAP: a round whose last remote human is removed or released: no ruling says whether it ends; the conservative expectation keeps it running; and nothing rules what a rejoining client whose seat was released is told | walked |
| own-cap | 1 | seat=Held round=run sess=ended | R1F7 |  | walked |
| resume-from-disk | 2 | refuse: a running round is not replaced by a disk resume (conservative) | GAP | GAP: a resume from disk requested while a round runs | not walked: a NetMatchService step (private capture writer, world join, checkpoint resume) that needs an activity load; the in-process rig has none |
| match-over | 1 | round=over sess=ended | NS-PHASE |  | walked |
| link-blip | 1 | seat=Held round=run sess=phase:TailReplay | RB3 |  | walked |
| link-restore | 1 | seat=Held round=run sess=phase:TailReplay | RB3 |  | walked |

## Parked

| event | tier | expected outcome | source | gap | walk |
|---|---|---|---|---|---|
| hold-proposed | 1 | seat=Held round=run peer=held sess=ready | RB2 LS-PARK |  | walked |
| hold-resolved | 1 | seat=Active round=run holds=0 sess=ready | LS-STOP |  | walked |
| park-begin | 1 | seat=Active round=run peer=run holds=0 sess=ready | LS-PARK |  | walked |
| park-end | 1 | seat=Active round=run peer=run holds=0 sess=ready | LS-PARK |  | walked |
| private-capture-complete | 1 | ignore: no seat waits on that image (conservative) | GAP | GAP: a private image completing for a seat that is not waiting on one | not walked: a NetMatchService step (private capture writer, world join, checkpoint resume) that needs an activity load; the in-process rig has none |
| world-image-offered | 2 | ignore: no seat waits on an image (conservative) | GAP | GAP: a world image offered to a seat that is not waiting on one | not walked: a NetMatchService step (private capture writer, world join, checkpoint resume) that needs an activity load; the in-process rig has none |
| opening-resume-offer | 2 | refuse: the opening resume offer is retired once the round runs past its anchor; a rejoin takes the image path with the newest image | R2WAY2 |  | not walked: a NetMatchService step (private capture writer, world join, checkpoint resume) that needs an activity load; the in-process rig has none |
| tail-replay-complete | 1 | n/a: the completion is raised only by the seat's own tail replay | NS-PHASE |  | not walked: a NetMatchService step (private capture writer, world join, checkpoint resume) that needs an activity load; the in-process rig has none |
| resync-relaunch | 1 | round=relaunch peer=relaunch sess=ready | LS-STOP |  | walked |
| host-goodbye | 1 | round=ended peer=noresync sess=ended | R1-392ii R2-392 H4-7 |  | walked |
| host-lost | 1 | peer=noresync sess=ended | R1-392ii R2-392 |  | walked |
| migration-begin | 2 | api=refused round=run | R1-392ii |  | walked |
| migration-complete | 2 | n/a: raised only while a host migration runs | DESIGN-MIGRATION |  | not walked: needs a migration in progress (three peers); not composed in this row |
| migration-fail | 2 | n/a: raised only while a host migration runs | DESIGN-MIGRATION |  | not walked: needs a migration in progress (three peers); not composed in this row |
| migration-successor-lost | 2 | n/a: raised only while a host migration runs | DESIGN-MIGRATION |  | not walked: needs a migration in progress (three peers); not composed in this row |
| late-join | 2 | api=refused round=run sess=ready | H4-7 R2D3 |  | walked |
| ticket-rejoin | 1 | seat=Active round=run peer=run holds=0 sess=ready | H4-0 H4-6 |  | walked |
| held-rejoin | 1 | api=refused seat=Active round=run sess=ready | H4-4 R2D3 |  | walked |
| kick | 1 | seat=Left round=run sess=ended:ParticipantRemoved | NP-KICK LS-STOP | GAP: a round whose last remote human is removed or released: no ruling says whether it ends; the conservative expectation keeps it running | walked |
| ban | 1 | seat=Left round=run sess=ended:ParticipantBanned | NP-KICK LS-STOP | GAP: a round whose last remote human is removed or released: no ruling says whether it ends; the conservative expectation keeps it running | walked |
| seat-release | 1 | seat=Active round=run holds=0 sess=ready | LS-STOP |  | walked |
| own-cap | 1 | seat=Held round=run sess=ended | R1F7 | GAP: R1 F7 rules a client's own end for a persistent world; for a match the conservative reading keeps the round running and hands the seat to the AI as a leave does | walked |
| resume-from-disk | 2 | refuse: a running round is not replaced by a disk resume (conservative) | GAP | GAP: a resume from disk requested while a round runs | not walked: a NetMatchService step (private capture writer, world join, checkpoint resume) that needs an activity load; the in-process rig has none |
| match-over | 1 | round=over peer=over sess=ended | LS-STOP NS-PHASE |  | walked |
| link-blip | 1 | seat=Active round=run peer=run holds=0 sess=ready | RB2 |  | walked |
| link-restore | 1 | seat=Active round=run peer=run holds=0 sess=ready | RB3 |  | walked |

## Relaunching

| event | tier | expected outcome | source | gap | walk |
|---|---|---|---|---|---|
| hold-proposed | 1 | api=refused round=relaunch sess=ready | LS-STOP |  | walked |
| hold-resolved | 1 | round=relaunch sess=ready | LS-STOP |  | walked |
| park-begin | 1 | round=relaunch sess=ready | LS-STOP |  | walked |
| park-end | 1 | round=relaunch sess=ready | LS-STOP |  | walked |
| private-capture-complete | 1 | ignore: no seat waits on that image (conservative) | GAP | GAP: a private image completing for a seat that is not waiting on one | not walked: a NetMatchService step (private capture writer, world join, checkpoint resume) that needs an activity load; the in-process rig has none |
| world-image-offered | 2 | ignore: no seat waits on an image (conservative) | GAP | GAP: a world image offered to a seat that is not waiting on one | not walked: a NetMatchService step (private capture writer, world join, checkpoint resume) that needs an activity load; the in-process rig has none |
| opening-resume-offer | 2 | refuse: the opening resume offer is retired once the round runs past its anchor; a rejoin takes the image path with the newest image | R2WAY2 |  | not walked: a NetMatchService step (private capture writer, world join, checkpoint resume) that needs an activity load; the in-process rig has none |
| tail-replay-complete | 1 | n/a: the completion is raised only by the seat's own tail replay | NS-PHASE |  | not walked: a NetMatchService step (private capture writer, world join, checkpoint resume) that needs an activity load; the in-process rig has none |
| resync-relaunch | 1 | round=relaunch sess=ready | LS-STOP |  | walked |
| host-goodbye | 1 | sess=ended | H4-7 R1-392ii |  | walked |
| host-lost | 1 | sess=ended | R1-392ii |  | walked |
| migration-begin | 2 | api=refused round=relaunch | R1-392ii |  | walked |
| migration-complete | 2 | n/a: raised only while a host migration runs | DESIGN-MIGRATION |  | not walked: needs a migration in progress (three peers); not composed in this row |
| migration-fail | 2 | n/a: raised only while a host migration runs | DESIGN-MIGRATION |  | not walked: needs a migration in progress (three peers); not composed in this row |
| migration-successor-lost | 2 | n/a: raised only while a host migration runs | DESIGN-MIGRATION |  | not walked: needs a migration in progress (three peers); not composed in this row |
| late-join | 2 | api=refused round=relaunch | H4-7 R2D3 |  | walked |
| ticket-rejoin | 1 | round=relaunch sess=ready | H4-0 |  | walked |
| held-rejoin | 1 | api=refused round=relaunch sess=ready | GAP | GAP: a returner arriving during a relaunch (conservative: the running round refuses; the relaunch's own admission carries it) | walked |
| kick | 1 | round=relaunch sess=ended:ParticipantRemoved | NP-KICK | GAP: a kick or ban while the round relaunches: no line says what the ended round becomes; the conservative expectation leaves it relaunching and drops the seat from the next roster | walked |
| ban | 1 | round=relaunch sess=ended:ParticipantBanned | NP-KICK | GAP: a kick or ban while the round relaunches: no line says what the ended round becomes; the conservative expectation leaves it relaunching and drops the seat from the next roster | walked |
| seat-release | 1 | round=relaunch sess=ready | LS-STOP |  | walked |
| own-cap | 1 | round=relaunch sess=ended | R1F7 |  | walked |
| resume-from-disk | 2 | legal: the relaunch loads the match's newest checkpoint from disk on every peer | READY-4 |  | not walked: a NetMatchService step (private capture writer, world join, checkpoint resume) that needs an activity load; the in-process rig has none |
| match-over | 1 | sess=ended | NS-PHASE |  | walked |
| link-blip | 1 | round=relaunch sess=ready | NS-PHASE |  | walked |
| link-restore | 1 | round=relaunch sess=ready | NS-PHASE |  | walked |

## Migrating

| event | tier | expected outcome | source | gap | walk |
|---|---|---|---|---|---|
| hold-proposed | 2 | api=refused sub=run subhost=2 | GAP | GAP: an event other than the migration's own steps arriving while the host is being replaced | walked |
| hold-resolved | 2 | sub=run subhost=2 | GAP | GAP: an event other than the migration's own steps arriving while the host is being replaced | walked |
| park-begin | 2 | sub=run subhost=2 | GAP | GAP: an event other than the migration's own steps arriving while the host is being replaced | walked |
| park-end | 2 | sub=run subhost=2 | GAP | GAP: an event other than the migration's own steps arriving while the host is being replaced | walked |
| private-capture-complete | 2 | ignore until the migration completes (conservative) | GAP | GAP: an event other than the migration's own steps arriving while the host is being replaced | not walked: a NetMatchService step (private capture writer, world join, checkpoint resume) that needs an activity load; the in-process rig has none |
| world-image-offered | 2 | ignore until the migration completes (conservative) | GAP | GAP: an event other than the migration's own steps arriving while the host is being replaced | not walked: a NetMatchService step (private capture writer, world join, checkpoint resume) that needs an activity load; the in-process rig has none |
| opening-resume-offer | 2 | ignore until the migration completes (conservative) | GAP | GAP: an event other than the migration's own steps arriving while the host is being replaced | not walked: a NetMatchService step (private capture writer, world join, checkpoint resume) that needs an activity load; the in-process rig has none |
| tail-replay-complete | 2 | ignore until the migration completes (conservative) | GAP | GAP: an event other than the migration's own steps arriving while the host is being replaced | not walked: a NetMatchService step (private capture writer, world join, checkpoint resume) that needs an activity load; the in-process rig has none |
| resync-relaunch | 2 | sub=run subhost=2 | GAP | GAP: an event other than the migration's own steps arriving while the host is being replaced | walked |
| host-goodbye | 2 | n/a: the host is already gone | R1-392ii |  | not walked: the lost host sends nothing |
| host-lost | 2 | sub=run subhost=2 | R1-392ii |  | walked |
| migration-begin | 2 | sub=run subhost=2 | DESIGN-MIGRATION |  | walked |
| migration-complete | 2 | sub=run subhost=2 | R1-392ii |  | walked |
| migration-fail | 2 | legal: the survivors leave to the landing with 'The host left the match' | R1-392ii |  | not walked: no in-process lever fails a migration short of losing every successor |
| migration-successor-lost | 2 | subhost=3 | DESIGN-MIGRATION |  | walked |
| late-join | 2 | api=refused sub=run subhost=2 | H4-7 |  | walked |
| ticket-rejoin | 2 | sub=run subhost=2 | H4-0 |  | walked |
| held-rejoin | 2 | api=refused sub=run subhost=2 | GAP | GAP: an event other than the migration's own steps arriving while the host is being replaced | walked |
| kick | 2 | sub=run subhost=2 | GAP | GAP: an event other than the migration's own steps arriving while the host is being replaced | walked |
| ban | 2 | sub=run subhost=2 | GAP | GAP: an event other than the migration's own steps arriving while the host is being replaced | walked |
| seat-release | 2 | sub=run subhost=2 | GAP | GAP: an event other than the migration's own steps arriving while the host is being replaced | walked |
| own-cap | 2 | sub=over | R1F7 | GAP: the subject reaching its own cap while the host is being replaced (R1 F7 read for a match) | walked |
| resume-from-disk | 2 | ignore until the migration completes (conservative) | GAP | GAP: an event other than the migration's own steps arriving while the host is being replaced | not walked: a NetMatchService step (private capture writer, world join, checkpoint resume) that needs an activity load; the in-process rig has none |
| match-over | 2 | refuse: no host remains to end the match until the successor hosts | GAP | GAP: an end of match requested while the host is being replaced | not walked: no peer is the host while the migration runs, so nothing authors a match end |
| link-blip | 2 | sub=run subhost=2 | RB2 |  | walked |
| link-restore | 2 | sub=run subhost=2 | RB2 |  | walked |

## Draining

| event | tier | expected outcome | source | gap | walk |
|---|---|---|---|---|---|
| hold-proposed | 1 | seat=Active round=run holds=0 sess=ready | LS-DRAIN |  | walked |
| hold-resolved | 1 | seat=Active round=run holds=0 sess=ready | LS-STOP |  | walked |
| park-begin | 1 | round=run holds=0 sess=ready | GAP | GAP: a capture park opened after the round's last tick | walked |
| park-end | 1 | round=run holds=0 sess=ready | GAP | GAP: a park end with no park open | walked |
| private-capture-complete | 1 | ignore: no seat waits on that image (conservative) | GAP | GAP: a private image completing for a seat that is not waiting on one | not walked: a NetMatchService step (private capture writer, world join, checkpoint resume) that needs an activity load; the in-process rig has none |
| world-image-offered | 2 | ignore: no seat waits on an image (conservative) | GAP | GAP: a world image offered to a seat that is not waiting on one | not walked: a NetMatchService step (private capture writer, world join, checkpoint resume) that needs an activity load; the in-process rig has none |
| opening-resume-offer | 2 | refuse: the opening resume offer is retired once the round runs past its anchor; a rejoin takes the image path with the newest image | R2WAY2 |  | not walked: a NetMatchService step (private capture writer, world join, checkpoint resume) that needs an activity load; the in-process rig has none |
| tail-replay-complete | 1 | n/a: the completion is raised only by the seat's own tail replay | NS-PHASE |  | not walked: a NetMatchService step (private capture writer, world join, checkpoint resume) that needs an activity load; the in-process rig has none |
| resync-relaunch | 1 | round=run sess=ready | GAP | GAP: a relaunch requested after the round's last tick | walked |
| host-goodbye | 1 | round=ended peer=noresync sess=ended | R1-392ii R2-392 H4-7 |  | walked |
| host-lost | 1 | peer=noresync sess=ended | R1-392ii R2-392 |  | walked |
| migration-begin | 2 | api=refused round=run | R1-392ii |  | walked |
| migration-complete | 2 | n/a: raised only while a host migration runs | DESIGN-MIGRATION |  | not walked: needs a migration in progress (three peers); not composed in this row |
| migration-fail | 2 | n/a: raised only while a host migration runs | DESIGN-MIGRATION |  | not walked: needs a migration in progress (three peers); not composed in this row |
| migration-successor-lost | 2 | n/a: raised only while a host migration runs | DESIGN-MIGRATION |  | not walked: needs a migration in progress (three peers); not composed in this row |
| late-join | 2 | api=refused round=run sess=ready | H4-7 R2D3 |  | walked |
| ticket-rejoin | 1 | round=run holds=0 sess=ready | H4-0 |  | walked |
| held-rejoin | 1 | api=refused seat=Active round=run sess=ready | H4-4 R2D3 |  | walked |
| kick | 1 | seat=Left round=run sess=ended:ParticipantRemoved | NP-KICK LS-STOP | GAP: a round whose last remote human is removed or released: no ruling says whether it ends; the conservative expectation keeps it running | walked |
| ban | 1 | seat=Left round=run sess=ended:ParticipantBanned | NP-KICK LS-STOP | GAP: a round whose last remote human is removed or released: no ruling says whether it ends; the conservative expectation keeps it running | walked |
| seat-release | 1 | seat=Active round=run holds=0 sess=ready | LS-STOP |  | walked |
| own-cap | 1 | peer=over sess=ended | LS-DRAIN |  | walked |
| resume-from-disk | 2 | refuse: a running round is not replaced by a disk resume (conservative) | GAP | GAP: a resume from disk requested while a round runs | not walked: a NetMatchService step (private capture writer, world join, checkpoint resume) that needs an activity load; the in-process rig has none |
| match-over | 1 | round=over peer=over sess=ended | LS-STOP NS-PHASE |  | walked |
| link-blip | 1 | round=run holds=0 sess=ready | LS-DRAIN |  | walked |
| link-restore | 1 | round=run holds=0 sess=ready | LS-DRAIN |  | walked |

## Left

| event | tier | expected outcome | source | gap | walk |
|---|---|---|---|---|---|
| hold-proposed | 1 | seat=Held peer=left round=run holds=0 sess=ready | RB2 |  | walked |
| hold-resolved | 1 | seat=Held peer=left round=run sess=ready | RB3 R2D3 |  | walked |
| park-begin | 1 | seat=Held peer=left round=run holds=0 sess=ready | LS-PARK |  | walked |
| park-end | 1 | seat=Held peer=left round=run holds=0 sess=ready | GAP | GAP: a park end with no park open | walked |
| private-capture-complete | 1 | ignore: no seat waits on that image (conservative) | GAP | GAP: a private image completing for a seat that is not waiting on one | not walked: a NetMatchService step (private capture writer, world join, checkpoint resume) that needs an activity load; the in-process rig has none |
| world-image-offered | 2 | ignore: no seat waits on an image (conservative) | GAP | GAP: a world image offered to a seat that is not waiting on one | not walked: a NetMatchService step (private capture writer, world join, checkpoint resume) that needs an activity load; the in-process rig has none |
| opening-resume-offer | 2 | refuse: the opening resume offer is retired once the round runs past its anchor; a rejoin takes the image path with the newest image | R2WAY2 |  | not walked: a NetMatchService step (private capture writer, world join, checkpoint resume) that needs an activity load; the in-process rig has none |
| tail-replay-complete | 1 | n/a: the completion is raised only by the seat's own tail replay | NS-PHASE |  | not walked: a NetMatchService step (private capture writer, world join, checkpoint resume) that needs an activity load; the in-process rig has none |
| resync-relaunch | 1 | round=relaunch seat=Held sess=ready | LS-STOP |  | walked |
| host-goodbye | 1 | round=ended peer=left sess=ended | R1-392ii H4-7 |  | walked |
| host-lost | 1 | peer=left sess=ended | R1-392ii |  | walked |
| migration-begin | 2 | api=refused round=run | R1-392ii |  | walked |
| migration-complete | 2 | n/a: raised only while a host migration runs | DESIGN-MIGRATION |  | not walked: needs a migration in progress (three peers); not composed in this row |
| migration-fail | 2 | n/a: raised only while a host migration runs | DESIGN-MIGRATION |  | not walked: needs a migration in progress (three peers); not composed in this row |
| migration-successor-lost | 2 | n/a: raised only while a host migration runs | DESIGN-MIGRATION |  | not walked: needs a migration in progress (three peers); not composed in this row |
| late-join | 2 | api=refused round=run sess=ready | H4-7 R2D3 |  | walked |
| ticket-rejoin | 1 | seat=Held peer=left round=run sess=ready | H4-0 |  | walked |
| held-rejoin | 1 | api=refused seat=Held round=run sess=ready | GAP | GAP: a clean leaver's return: H4 section 7 revokes its ticket, while R1 F7 and R2 D3 hand a left seat to the AI and let a held seat reclaim; walked at the coordinator, where the admission plane's ticket check is not composed | walked |
| kick | 1 | seat=Left round=run sess=ended:ParticipantRemoved | NP-KICK LS-STOP | GAP: a round whose last remote human is removed or released: no ruling says whether it ends; the conservative expectation keeps it running | walked |
| ban | 1 | seat=Left round=run sess=ended:ParticipantBanned | NP-KICK LS-STOP | GAP: a round whose last remote human is removed or released: no ruling says whether it ends; the conservative expectation keeps it running | walked |
| seat-release | 1 | seat=Left round=run sess=ready | LS-STOP | GAP: a round whose last remote human is removed or released: no ruling says whether it ends; the conservative expectation keeps it running | walked |
| own-cap | 1 | seat=Held round=run sess=ended | R1F7 |  | walked |
| resume-from-disk | 2 | refuse: a running round is not replaced by a disk resume (conservative) | GAP | GAP: a resume from disk requested while a round runs | not walked: a NetMatchService step (private capture writer, world join, checkpoint resume) that needs an activity load; the in-process rig has none |
| match-over | 1 | round=over sess=ended | NS-PHASE |  | walked |
| link-blip | 1 | seat=Held peer=left round=run sess=ready | RB3 |  | walked |
| link-restore | 1 | seat=Held peer=left round=run sess=ready | RB3 |  | walked |

## Gap list (96 pairs)

| state | event | the question no line answers |
|---|---|---|
| Active | park-end | a park end with no park open |
| Active | private-capture-complete | a private image completing for a seat that is not waiting on one |
| Active | world-image-offered | a world image offered to a seat that is not waiting on one |
| Active | kick | a round whose last remote human is removed or released: no ruling says whether it ends; the conservative expectation keeps it running |
| Active | ban | a round whose last remote human is removed or released: no ruling says whether it ends; the conservative expectation keeps it running |
| Active | own-cap | R1 F7 rules a client's own end for a persistent world; for a match the conservative reading keeps the round running and hands the seat to the AI as a leave does |
| Active | resume-from-disk | a resume from disk requested while a round runs |
| Held | park-end | a park end with no park open |
| Held | world-image-offered | a world image offered to a seat that is not waiting on one |
| Held | kick | a round whose last remote human is removed or released: no ruling says whether it ends; the conservative expectation keeps it running |
| Held | ban | a round whose last remote human is removed or released: no ruling says whether it ends; the conservative expectation keeps it running |
| Held | seat-release | a round whose last remote human is removed or released: no ruling says whether it ends; the conservative expectation keeps it running |
| Held | resume-from-disk | a resume from disk requested while a round runs |
| Reclaiming | hold-proposed | a hold proposed for a seat whose reclaim is agreed but not active yet (nothing is owed by it before E) |
| Reclaiming | park-end | a park end with no park open |
| Reclaiming | private-capture-complete | a private image completing for a seat that is not waiting on one |
| Reclaiming | world-image-offered | a world image offered to a seat that is not waiting on one |
| Reclaiming | resync-relaunch | what an agreed reclaim becomes across a relaunch is not ruled |
| Reclaiming | kick | a round whose last remote human is removed or released: no ruling says whether it ends; the conservative expectation keeps it running |
| Reclaiming | ban | a round whose last remote human is removed or released: no ruling says whether it ends; the conservative expectation keeps it running |
| Reclaiming | seat-release | a release of a seat whose reclaim is agreed |
| Reclaiming | own-cap | a returner that reaches its own cap before its activation frame |
| Reclaiming | resume-from-disk | a resume from disk requested while a round runs |
| Rejoin:Connecting | park-end | a park end with no park open |
| Rejoin:Connecting | private-capture-complete | a private image completing for a seat that is not waiting on one |
| Rejoin:Connecting | world-image-offered | a world image offered to a seat that is not waiting on one |
| Rejoin:Connecting | resync-relaunch | a relaunch while the seat's rejoin is in flight: the conservative expectation keeps the rejoin's session |
| Rejoin:Connecting | kick | a round whose last remote human is removed or released: no ruling says whether it ends; the conservative expectation keeps it running |
| Rejoin:Connecting | ban | a round whose last remote human is removed or released: no ruling says whether it ends; the conservative expectation keeps it running |
| Rejoin:Connecting | seat-release | a round whose last remote human is removed or released: no ruling says whether it ends; the conservative expectation keeps it running; and nothing rules what a rejoining client whose seat was released is told |
| Rejoin:Connecting | resume-from-disk | a resume from disk requested while a round runs |
| Rejoin:ImagePending | park-end | a park end with no park open |
| Rejoin:ImagePending | resync-relaunch | a relaunch while the seat's rejoin is in flight: the conservative expectation keeps the rejoin's session |
| Rejoin:ImagePending | kick | a round whose last remote human is removed or released: no ruling says whether it ends; the conservative expectation keeps it running |
| Rejoin:ImagePending | ban | a round whose last remote human is removed or released: no ruling says whether it ends; the conservative expectation keeps it running |
| Rejoin:ImagePending | seat-release | a round whose last remote human is removed or released: no ruling says whether it ends; the conservative expectation keeps it running; and nothing rules what a rejoining client whose seat was released is told |
| Rejoin:ImagePending | resume-from-disk | a resume from disk requested while a round runs |
| Rejoin:Loading | park-end | a park end with no park open |
| Rejoin:Loading | private-capture-complete | a private image completing for a seat that is not waiting on one |
| Rejoin:Loading | world-image-offered | a newer world image offered while an older one loads or replays |
| Rejoin:Loading | resync-relaunch | a relaunch while the seat's rejoin is in flight: the conservative expectation keeps the rejoin's session |
| Rejoin:Loading | kick | a round whose last remote human is removed or released: no ruling says whether it ends; the conservative expectation keeps it running |
| Rejoin:Loading | ban | a round whose last remote human is removed or released: no ruling says whether it ends; the conservative expectation keeps it running |
| Rejoin:Loading | seat-release | a round whose last remote human is removed or released: no ruling says whether it ends; the conservative expectation keeps it running; and nothing rules what a rejoining client whose seat was released is told |
| Rejoin:Loading | resume-from-disk | a resume from disk requested while a round runs |
| Rejoin:TailReplay | park-end | a park end with no park open |
| Rejoin:TailReplay | private-capture-complete | a private image completing for a seat that is not waiting on one |
| Rejoin:TailReplay | world-image-offered | a newer world image offered while an older one loads or replays |
| Rejoin:TailReplay | resync-relaunch | a relaunch while the seat's rejoin is in flight: the conservative expectation keeps the rejoin's session |
| Rejoin:TailReplay | kick | a round whose last remote human is removed or released: no ruling says whether it ends; the conservative expectation keeps it running |
| Rejoin:TailReplay | ban | a round whose last remote human is removed or released: no ruling says whether it ends; the conservative expectation keeps it running |
| Rejoin:TailReplay | seat-release | a round whose last remote human is removed or released: no ruling says whether it ends; the conservative expectation keeps it running; and nothing rules what a rejoining client whose seat was released is told |
| Rejoin:TailReplay | resume-from-disk | a resume from disk requested while a round runs |
| Parked | private-capture-complete | a private image completing for a seat that is not waiting on one |
| Parked | world-image-offered | a world image offered to a seat that is not waiting on one |
| Parked | kick | a round whose last remote human is removed or released: no ruling says whether it ends; the conservative expectation keeps it running |
| Parked | ban | a round whose last remote human is removed or released: no ruling says whether it ends; the conservative expectation keeps it running |
| Parked | own-cap | R1 F7 rules a client's own end for a persistent world; for a match the conservative reading keeps the round running and hands the seat to the AI as a leave does |
| Parked | resume-from-disk | a resume from disk requested while a round runs |
| Relaunching | private-capture-complete | a private image completing for a seat that is not waiting on one |
| Relaunching | world-image-offered | a world image offered to a seat that is not waiting on one |
| Relaunching | held-rejoin | a returner arriving during a relaunch (conservative: the running round refuses; the relaunch's own admission carries it) |
| Relaunching | kick | a kick or ban while the round relaunches: no line says what the ended round becomes; the conservative expectation leaves it relaunching and drops the seat from the next roster |
| Relaunching | ban | a kick or ban while the round relaunches: no line says what the ended round becomes; the conservative expectation leaves it relaunching and drops the seat from the next roster |
| Migrating | hold-proposed | an event other than the migration's own steps arriving while the host is being replaced |
| Migrating | hold-resolved | an event other than the migration's own steps arriving while the host is being replaced |
| Migrating | park-begin | an event other than the migration's own steps arriving while the host is being replaced |
| Migrating | park-end | an event other than the migration's own steps arriving while the host is being replaced |
| Migrating | private-capture-complete | an event other than the migration's own steps arriving while the host is being replaced |
| Migrating | world-image-offered | an event other than the migration's own steps arriving while the host is being replaced |
| Migrating | opening-resume-offer | an event other than the migration's own steps arriving while the host is being replaced |
| Migrating | tail-replay-complete | an event other than the migration's own steps arriving while the host is being replaced |
| Migrating | resync-relaunch | an event other than the migration's own steps arriving while the host is being replaced |
| Migrating | held-rejoin | an event other than the migration's own steps arriving while the host is being replaced |
| Migrating | kick | an event other than the migration's own steps arriving while the host is being replaced |
| Migrating | ban | an event other than the migration's own steps arriving while the host is being replaced |
| Migrating | seat-release | an event other than the migration's own steps arriving while the host is being replaced |
| Migrating | own-cap | the subject reaching its own cap while the host is being replaced (R1 F7 read for a match) |
| Migrating | resume-from-disk | an event other than the migration's own steps arriving while the host is being replaced |
| Migrating | match-over | an end of match requested while the host is being replaced |
| Draining | park-begin | a capture park opened after the round's last tick |
| Draining | park-end | a park end with no park open |
| Draining | private-capture-complete | a private image completing for a seat that is not waiting on one |
| Draining | world-image-offered | a world image offered to a seat that is not waiting on one |
| Draining | resync-relaunch | a relaunch requested after the round's last tick |
| Draining | kick | a round whose last remote human is removed or released: no ruling says whether it ends; the conservative expectation keeps it running |
| Draining | ban | a round whose last remote human is removed or released: no ruling says whether it ends; the conservative expectation keeps it running |
| Draining | resume-from-disk | a resume from disk requested while a round runs |
| Left | park-end | a park end with no park open |
| Left | private-capture-complete | a private image completing for a seat that is not waiting on one |
| Left | world-image-offered | a world image offered to a seat that is not waiting on one |
| Left | held-rejoin | a clean leaver's return: H4 section 7 revokes its ticket, while R1 F7 and R2 D3 hand a left seat to the AI and let a held seat reclaim; walked at the coordinator, where the admission plane's ticket check is not composed |
| Left | kick | a round whose last remote human is removed or released: no ruling says whether it ends; the conservative expectation keeps it running |
| Left | ban | a round whose last remote human is removed or released: no ruling says whether it ends; the conservative expectation keeps it running |
| Left | seat-release | a round whose last remote human is removed or released: no ruling says whether it ends; the conservative expectation keeps it running |
| Left | resume-from-disk | a resume from disk requested while a round runs |
