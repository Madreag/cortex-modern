# The rejoin state x event matrix

Every (seat state, event) pair of the rejoin machine with the outcome the policy expects and the policy it follows. The subject is one
client seat (peer 2) of a bounded-wait round; the host is peer 1. The self-test `-net-rejoin-matrix-selftest` (row `net-rejoin-matrix` of
`tools/run_selftests.py`) prints this table from its own rows and walks every pair marked `walked` in process: it drives a fresh
host/client coordinator pair and a host/client session pair into the state, delivers the event, runs 500 ms and compares what the
machine did with the expected tokens. This file is generated from that run's `table` lines; edit the rows in
`Source/Network/NetRejoinMatrixSelfTest.cpp`, never here.

The Migrating row runs on a three-peer star instead: host 1, successor 2 and the subject 3 (the survivor that is not next in line).
The host stops without a close or a last packet, the survivors begin a migration, the event is delivered (host-side calls go to the
successor, which is not the host yet), and the rig reads the subject once the migration settles.

Tier 1 is the match path (hold, park, capture, relaunch, the rejoin phases, goodbye, link blip and restore, kick, ban, cap). Tier 2 is
world images, migration, late join and resume from disk. A pair the rig cannot reach is marked `not walked` with the reason; the
two-process scenarios cover it.

## Expected-outcome tokens

| token | reads |
|---|---|
| `seat=X` | the subject seat as the host's coordinator reads it: Held (an AI hold for a returner, no agreed reclaim), Reclaiming (a reclaim agreed, before E), Left (a leave frame and no hold waiting for a returner: a released seat stays under the AI), Active. |
| `round=run\|over\|relaunch\|ended` | the host coordinator: Running; Stopped on Complete; Failed on ResyncRequested; anything but Running. |
| `peer=run\|held\|left\|over\|relaunch\|noresync` | the subject's own coordinator; noresync = anything but a resync. |
| `sess=ready\|alive\|ended\|ended:Reason\|phase:X` | the subject's session: Ready; not ended; ended; ended with that reject reason; still in rejoin phase X. |
| `holds=0 / holds>0` | holds the host counted on the seat after the state was entered. |
| `api=ok\|refused` | the return of the call that delivers the event. |
| `sub=run\|over\|ended` | Migrating row only: the subject survivor's coordinator once the migration settles (running; stopped on its own Complete; anything but running). |
| `subhost=N` | Migrating row only: the peer the subject survivor takes for its host once the migration settles. |

A walked expectation is a list of tokens that must all hold. A not-walked expectation is written out in words.

## Sources

| id | kind | line |
|---|---|---|
| HOLD | POLICY | after the slow-player bound (default 3 ticks = 50 ms) the host holds the seat at an agreed frame H (HoldAtFrame); the survivors keep committing; a two-peer round does not end when its human is held. |
| RETURN | POLICY | from H the held client's input authority is revoked and its backlog fenced; it returns only by a reclaim at a future frame beyond every input it sent. |
| LEAVE | POLICY | a client's Complete is that client's leave, never the world's end; the seat drains and goes to the AI as a leave does. |
| LEAVE-HELD | POLICY | a clean leaver's seat is held for it like a dropped one's (the AI plays it, the ticket is kept); its return is a reclaim; only the host's kick, ban or release takes the seat away. |
| HOSTLOSS-HELD | POLICY | a held client whose host is gone takes host migration when survivors exist; with no survivor it leaves to the landing with 'The host left the match' - never a resync attempt against a gone host, never a hang. |
| QUORUM | POLICY | a migration starts only when a strict majority of the seats connected in the last roster revision every survivor holds agree the host's link is lost and reach the candidate successor; one member's lost link never replaces a live host. |
| RECLAIM | POLICY | a held seat's returner always reclaims its held seat at the agreed frame E; new-member admission is for new identities only. |
| WORLD-RESUME | POLICY | a restarted world's opening resume offer is retired once the round runs past its anchor; a rejoin then takes the image path with the newest image. |
| WORLD-IMAGE | POLICY | a held world seat takes the newest image; a peer's coverage starts at its first tick or the image it loaded; a held match client replays its own committed tail. |
| HOSTDROP | POLICY | a client with no committed frame goes to the landing on a host drop; one with committed frames takes the bounded reconnect, then the landing. |
| BASE-REFRESH | POLICY | the held client's base image refreshes by the steady capture cost (rolling median of three captures). |
| ADMISSION | DESIGN | admission traffic (an unbound transport, a reconnect handshake) can never stop or mutate the running match. |
| SEAT-ID | DESIGN | a reclaim addresses a seat's held incarnation by its stable id. |
| TRANSPORT | DESIGN | a newly proven connection replaces the old transport for the seat; the superseded one is fenced. |
| TICKET | DESIGN | live-match ticketless joins are denied; SessionEnded is the one confirmed end. |
| DISK-RESUME | POLICY | resume from disk reloads the match from its newest checkpoint on every peer. |
| MATCH-END | POLICY | a match ends only by its own rules or the host's decision, never because the last remote human left (the AI holds the seats; the host plays on); a client's cap or Complete is that client's leave; a kick during a relaunch lets the relaunch complete; a survivor's own end survives a completing migration; a kick or ban of a held seat ends its AI hold and cancels an agreed reclaim; a seat release on an AI-held seat releases it; no hold in the goodbye drain. Bounded-wait rounds. |
| NS-PHASE | DESIGN | Source/Network/NetSession.h RejoinPhase: in every rejoin phase a transport close still ends the link and the host's goodbye completes the seat. |
| NP-KICK | DESIGN | Source/Network/NetProtocol.h NetRejectReason: ParticipantRemoved = the holder is gone for good; ParticipantBanned = refused for the named scope. |
| LS-STOP | DESIGN | Source/Network/NetLockstep.h NetLockstepStopReason: PeerLeft keeps the survivors going; ResyncRequested ends the round for a reload; Reclaimed/Expired resolve a held seat only. |
| LS-PARK | DESIGN | Source/Network/NetLockstep.cpp DeclareOverdueInputs: an agreed capture park is not evidence that a seat stopped; a seat is due a delay after the park's last frame. |
| LS-DRAIN | DESIGN | Source/Network/NetLockstep.h SetGoodbyeDrain: the round has run its last tick and judges no seat. |
| PARK-START | DESIGN | Source/Network/NetLockstep.cpp: no seat activation is agreed where a capture park can cover it. |
| DESIGN-MIGRATION | DESIGN | Source/Network/NetLockstep.cpp RestartHostMigrationAfterSuccessorLoss / BeginHostMigration. |
| GAP | NONE | no policy or design line gives the outcome; the table carries the conservative outcome (refuse or ignore) and the pair is in the gap list. |

## Active

| event | tier | expected outcome | source | gap | walk |
|---|---|---|---|---|---|
| hold-proposed | 1 | seat=Held round=run peer=held holds>0 sess=ready | HOLD |  | walked |
| hold-resolved | 1 | seat=Active round=run holds=0 sess=ready | LS-STOP |  | walked |
| park-begin | 1 | seat=Active round=run peer=run holds=0 sess=ready | LS-PARK |  | walked |
| park-end | 1 | seat=Active round=run peer=run holds=0 sess=ready | GAP | GAP: a park end with no park open | walked |
| private-capture-complete | 1 | ignore: no seat waits on that image (conservative) | GAP | GAP: a private image completing for a seat that is not waiting on one | not walked: a NetMatchService step (private capture writer, world join, checkpoint resume) that needs an activity load; the in-process rig has none |
| world-image-offered | 2 | ignore: no seat waits on an image (conservative) | GAP | GAP: a world image offered to a seat that is not waiting on one | not walked: a NetMatchService step (private capture writer, world join, checkpoint resume) that needs an activity load; the in-process rig has none |
| opening-resume-offer | 2 | refuse: the opening resume offer is retired once the round runs past its anchor; a rejoin takes the image path with the newest image | WORLD-RESUME |  | not walked: a NetMatchService step (private capture writer, world join, checkpoint resume) that needs an activity load; the in-process rig has none |
| tail-replay-complete | 1 | n/a: the completion is raised only by the seat's own tail replay | NS-PHASE |  | not walked: a NetMatchService step (private capture writer, world join, checkpoint resume) that needs an activity load; the in-process rig has none |
| resync-relaunch | 1 | round=relaunch peer=relaunch sess=ready | LS-STOP |  | walked |
| host-goodbye | 1 | round=ended peer=noresync sess=ended | HOSTLOSS-HELD HOSTDROP TICKET |  | walked |
| host-lost | 1 | peer=noresync sess=ended | HOSTLOSS-HELD HOSTDROP |  | walked |
| migration-begin | 2 | api=refused round=run | HOSTLOSS-HELD |  | walked |
| migration-complete | 2 | n/a: raised only while a host migration runs | DESIGN-MIGRATION |  | not walked: needs a migration in progress (three peers); not composed in this row |
| migration-fail | 2 | n/a: raised only while a host migration runs | DESIGN-MIGRATION |  | not walked: needs a migration in progress (three peers); not composed in this row |
| migration-successor-lost | 2 | n/a: raised only while a host migration runs | DESIGN-MIGRATION |  | not walked: needs a migration in progress (three peers); not composed in this row |
| late-join | 2 | api=refused round=run sess=ready | TICKET RECLAIM |  | walked |
| ticket-rejoin | 1 | seat=Active round=run peer=run holds=0 sess=ready | ADMISSION TRANSPORT |  | walked |
| held-rejoin | 1 | api=refused seat=Active round=run sess=ready | SEAT-ID RECLAIM |  | walked |
| kick | 1 | seat=Left round=run sess=ended:ParticipantRemoved | NP-KICK MATCH-END |  | walked |
| ban | 1 | seat=Left round=run sess=ended:ParticipantBanned | NP-KICK MATCH-END |  | walked |
| seat-release | 1 | seat=Active round=run holds=0 sess=ready | LS-STOP |  | walked |
| own-cap | 1 | seat=Held round=run sess=ended | LEAVE LEAVE-HELD |  | walked |
| resume-from-disk | 2 | refuse: a running round is not replaced by a disk resume (conservative) | GAP | GAP: a resume from disk requested while a round runs | not walked: a NetMatchService step (private capture writer, world join, checkpoint resume) that needs an activity load; the in-process rig has none |
| match-over | 1 | round=over peer=over sess=ended | LS-STOP NS-PHASE |  | walked |
| link-blip | 1 | seat=Active round=run peer=run holds=0 sess=ready | HOLD |  | walked |
| link-restore | 1 | seat=Active round=run peer=run holds=0 sess=ready | RETURN |  | walked |

## Held

| event | tier | expected outcome | source | gap | walk |
|---|---|---|---|---|---|
| hold-proposed | 1 | seat=Held peer=held round=run holds=0 sess=ready | HOLD |  | walked |
| hold-resolved | 1 | seat=Held peer=held round=run sess=ready | RETURN RECLAIM |  | walked |
| park-begin | 1 | seat=Held peer=held round=run holds=0 sess=ready | LS-PARK |  | walked |
| park-end | 1 | seat=Held peer=held round=run holds=0 sess=ready | GAP | GAP: a park end with no park open | walked |
| private-capture-complete | 1 | ignore: the base image is kept for the seat's next rejoin (the steady-cost refresh rule) | BASE-REFRESH |  | not walked: a NetMatchService step (private capture writer, world join, checkpoint resume) that needs an activity load; the in-process rig has none |
| world-image-offered | 2 | ignore: no seat waits on an image (conservative) | GAP | GAP: a world image offered to a seat that is not waiting on one | not walked: a NetMatchService step (private capture writer, world join, checkpoint resume) that needs an activity load; the in-process rig has none |
| opening-resume-offer | 2 | refuse: the opening resume offer is retired once the round runs past its anchor; a rejoin takes the image path with the newest image | WORLD-RESUME |  | not walked: a NetMatchService step (private capture writer, world join, checkpoint resume) that needs an activity load; the in-process rig has none |
| tail-replay-complete | 1 | n/a: the completion is raised only by the seat's own tail replay | NS-PHASE |  | not walked: a NetMatchService step (private capture writer, world join, checkpoint resume) that needs an activity load; the in-process rig has none |
| resync-relaunch | 1 | round=relaunch seat=Held sess=ready | LS-STOP |  | walked |
| host-goodbye | 1 | round=ended peer=held sess=ended | HOSTLOSS-HELD TICKET |  | walked |
| host-lost | 1 | peer=held sess=ended | HOSTLOSS-HELD |  | walked |
| migration-begin | 2 | api=refused round=run | HOSTLOSS-HELD |  | walked |
| migration-complete | 2 | n/a: raised only while a host migration runs | DESIGN-MIGRATION |  | not walked: needs a migration in progress (three peers); not composed in this row |
| migration-fail | 2 | n/a: raised only while a host migration runs | DESIGN-MIGRATION |  | not walked: needs a migration in progress (three peers); not composed in this row |
| migration-successor-lost | 2 | n/a: raised only while a host migration runs | DESIGN-MIGRATION |  | not walked: needs a migration in progress (three peers); not composed in this row |
| late-join | 2 | api=refused round=run sess=ready | TICKET RECLAIM |  | walked |
| ticket-rejoin | 1 | seat=Held peer=held round=run sess=ready | ADMISSION |  | walked |
| held-rejoin | 1 | api=ok seat=Reclaiming round=run sess=ready | RETURN RECLAIM |  | walked |
| kick | 1 | seat=Left round=run sess=ended:ParticipantRemoved | NP-KICK MATCH-END |  | walked |
| ban | 1 | seat=Left round=run sess=ended:ParticipantBanned | NP-KICK MATCH-END |  | walked |
| seat-release | 1 | seat=Left round=run sess=ready | LS-STOP MATCH-END |  | walked |
| own-cap | 1 | seat=Held round=run sess=ended | LEAVE LEAVE-HELD |  | walked |
| resume-from-disk | 2 | refuse: a running round is not replaced by a disk resume (conservative) | GAP | GAP: a resume from disk requested while a round runs | not walked: a NetMatchService step (private capture writer, world join, checkpoint resume) that needs an activity load; the in-process rig has none |
| match-over | 1 | round=over sess=ended | NS-PHASE |  | walked |
| link-blip | 1 | seat=Held peer=held round=run sess=ready | RETURN |  | walked |
| link-restore | 1 | seat=Held peer=held round=run sess=ready | RETURN |  | walked |

## Reclaiming

| event | tier | expected outcome | source | gap | walk |
|---|---|---|---|---|---|
| hold-proposed | 1 | seat=Reclaiming round=run sess=ready | GAP | GAP: a hold proposed for a seat whose reclaim is agreed but not active yet (nothing is owed by it before E) | walked |
| hold-resolved | 1 | seat=Reclaiming round=run sess=ready | RETURN RECLAIM |  | walked |
| park-begin | 1 | seat=Reclaiming round=run holds=0 sess=ready | PARK-START |  | walked |
| park-end | 1 | seat=Reclaiming round=run sess=ready | GAP | GAP: a park end with no park open | walked |
| private-capture-complete | 1 | ignore: no seat waits on that image (conservative) | GAP | GAP: a private image completing for a seat that is not waiting on one | not walked: a NetMatchService step (private capture writer, world join, checkpoint resume) that needs an activity load; the in-process rig has none |
| world-image-offered | 2 | ignore: no seat waits on an image (conservative) | GAP | GAP: a world image offered to a seat that is not waiting on one | not walked: a NetMatchService step (private capture writer, world join, checkpoint resume) that needs an activity load; the in-process rig has none |
| opening-resume-offer | 2 | refuse: the opening resume offer is retired once the round runs past its anchor; a rejoin takes the image path with the newest image | WORLD-RESUME |  | not walked: a NetMatchService step (private capture writer, world join, checkpoint resume) that needs an activity load; the in-process rig has none |
| tail-replay-complete | 1 | n/a: the completion is raised only by the seat's own tail replay | NS-PHASE |  | not walked: a NetMatchService step (private capture writer, world join, checkpoint resume) that needs an activity load; the in-process rig has none |
| resync-relaunch | 1 | round=relaunch sess=ready | LS-STOP | GAP: no policy says what an agreed reclaim becomes across a relaunch | walked |
| host-goodbye | 1 | round=ended peer=held sess=ended | HOSTLOSS-HELD TICKET |  | walked |
| host-lost | 1 | peer=held sess=ended | HOSTLOSS-HELD |  | walked |
| migration-begin | 2 | api=refused round=run | HOSTLOSS-HELD |  | walked |
| migration-complete | 2 | n/a: raised only while a host migration runs | DESIGN-MIGRATION |  | not walked: needs a migration in progress (three peers); not composed in this row |
| migration-fail | 2 | n/a: raised only while a host migration runs | DESIGN-MIGRATION |  | not walked: needs a migration in progress (three peers); not composed in this row |
| migration-successor-lost | 2 | n/a: raised only while a host migration runs | DESIGN-MIGRATION |  | not walked: needs a migration in progress (three peers); not composed in this row |
| late-join | 2 | api=refused round=run sess=ready | TICKET RECLAIM |  | walked |
| ticket-rejoin | 1 | seat=Reclaiming round=run sess=ready | ADMISSION |  | walked |
| held-rejoin | 1 | api=ok seat=Reclaiming round=run sess=ready | TRANSPORT |  | walked |
| kick | 1 | seat=Left round=run sess=ended:ParticipantRemoved | NP-KICK MATCH-END |  | walked |
| ban | 1 | seat=Left round=run sess=ended:ParticipantBanned | NP-KICK MATCH-END |  | walked |
| seat-release | 1 | seat=Reclaiming round=run sess=ready | GAP | GAP: a release of a seat whose reclaim is agreed | walked |
| own-cap | 1 | seat=Reclaiming round=run sess=ended | GAP | GAP: a returner that reaches its own cap before its activation frame | walked |
| resume-from-disk | 2 | refuse: a running round is not replaced by a disk resume (conservative) | GAP | GAP: a resume from disk requested while a round runs | not walked: a NetMatchService step (private capture writer, world join, checkpoint resume) that needs an activity load; the in-process rig has none |
| match-over | 1 | round=over sess=ended | NS-PHASE |  | walked |
| link-blip | 1 | seat=Reclaiming round=run sess=ready | RETURN |  | walked |
| link-restore | 1 | seat=Reclaiming round=run sess=ready | RETURN |  | walked |

## Rejoin:Connecting

| event | tier | expected outcome | source | gap | walk |
|---|---|---|---|---|---|
| hold-proposed | 1 | seat=Held round=run holds=0 sess=alive | HOLD |  | walked |
| hold-resolved | 1 | seat=Held round=run sess=alive | RETURN RECLAIM |  | walked |
| park-begin | 1 | seat=Held round=run holds=0 sess=alive | LS-PARK |  | walked |
| park-end | 1 | seat=Held round=run holds=0 sess=alive | GAP | GAP: a park end with no park open | walked |
| private-capture-complete | 1 | ignore: no seat waits on that image (conservative) | GAP | GAP: a private image completing for a seat that is not waiting on one | not walked: a NetMatchService step (private capture writer, world join, checkpoint resume) that needs an activity load; the in-process rig has none |
| world-image-offered | 2 | ignore: no seat waits on an image (conservative) | GAP | GAP: a world image offered to a seat that is not waiting on one | not walked: a NetMatchService step (private capture writer, world join, checkpoint resume) that needs an activity load; the in-process rig has none |
| opening-resume-offer | 2 | refuse: the opening resume offer is retired once the round runs past its anchor; a rejoin takes the image path with the newest image | WORLD-RESUME |  | not walked: a NetMatchService step (private capture writer, world join, checkpoint resume) that needs an activity load; the in-process rig has none |
| tail-replay-complete | 1 | n/a: the completion is raised only by the seat's own tail replay | NS-PHASE |  | not walked: a NetMatchService step (private capture writer, world join, checkpoint resume) that needs an activity load; the in-process rig has none |
| resync-relaunch | 1 | round=relaunch sess=alive | LS-STOP | GAP: a relaunch while the seat's rejoin is in flight: the conservative expectation keeps the rejoin's session | walked |
| host-goodbye | 1 | round=ended sess=ended | NS-PHASE HOSTLOSS-HELD |  | walked |
| host-lost | 1 | sess=ended | NS-PHASE |  | walked |
| migration-begin | 2 | api=refused round=run | HOSTLOSS-HELD |  | walked |
| migration-complete | 2 | n/a: raised only while a host migration runs | DESIGN-MIGRATION |  | not walked: needs a migration in progress (three peers); not composed in this row |
| migration-fail | 2 | n/a: raised only while a host migration runs | DESIGN-MIGRATION |  | not walked: needs a migration in progress (three peers); not composed in this row |
| migration-successor-lost | 2 | n/a: raised only while a host migration runs | DESIGN-MIGRATION |  | not walked: needs a migration in progress (three peers); not composed in this row |
| late-join | 2 | api=refused round=run sess=alive | TICKET RECLAIM |  | walked |
| ticket-rejoin | 1 | seat=Held round=run sess=alive | ADMISSION |  | walked |
| held-rejoin | 1 | api=ok seat=Reclaiming round=run sess=alive | RETURN RECLAIM |  | walked |
| kick | 1 | seat=Left round=run | NP-KICK MATCH-END |  | walked; coordinator half only: the handshaking returner is refused by the admission plane (NetReconnectHost), which the rig does not compose |
| ban | 1 | seat=Left round=run | NP-KICK MATCH-END |  | walked; coordinator half only: the handshaking returner is refused by the admission plane (NetReconnectHost), which the rig does not compose |
| seat-release | 1 | seat=Left round=run | LS-STOP MATCH-END |  | walked |
| own-cap | 1 | seat=Held round=run sess=ended | LEAVE LEAVE-HELD |  | walked |
| resume-from-disk | 2 | refuse: a running round is not replaced by a disk resume (conservative) | GAP | GAP: a resume from disk requested while a round runs | not walked: a NetMatchService step (private capture writer, world join, checkpoint resume) that needs an activity load; the in-process rig has none |
| match-over | 1 | round=over sess=ended | NS-PHASE |  | walked |
| link-blip | 1 | seat=Held round=run sess=alive | RETURN |  | walked |
| link-restore | 1 | seat=Held round=run sess=alive | RETURN |  | walked |

## Rejoin:ImagePending

| event | tier | expected outcome | source | gap | walk |
|---|---|---|---|---|---|
| hold-proposed | 1 | seat=Held round=run holds=0 sess=phase:ImagePending | HOLD |  | walked |
| hold-resolved | 1 | seat=Held round=run sess=phase:ImagePending | RETURN RECLAIM |  | walked |
| park-begin | 1 | seat=Held round=run holds=0 sess=phase:ImagePending | LS-PARK |  | walked |
| park-end | 1 | seat=Held round=run holds=0 sess=phase:ImagePending | GAP | GAP: a park end with no park open | walked |
| private-capture-complete | 1 | legal: ImagePending -> Loading on the newest base image | BASE-REFRESH WORLD-IMAGE |  | not walked: a NetMatchService step (private capture writer, world join, checkpoint resume) that needs an activity load; the in-process rig has none |
| world-image-offered | 2 | legal: ImagePending -> Loading on the newest image; coverage starts at the loaded image | WORLD-IMAGE |  | not walked: a NetMatchService step (private capture writer, world join, checkpoint resume) that needs an activity load; the in-process rig has none |
| opening-resume-offer | 2 | refuse: the opening resume offer is retired once the round runs past its anchor; a rejoin takes the image path with the newest image | WORLD-RESUME |  | not walked: a NetMatchService step (private capture writer, world join, checkpoint resume) that needs an activity load; the in-process rig has none |
| tail-replay-complete | 1 | n/a: the completion is raised only by the seat's own tail replay | NS-PHASE |  | not walked: a NetMatchService step (private capture writer, world join, checkpoint resume) that needs an activity load; the in-process rig has none |
| resync-relaunch | 1 | round=relaunch sess=phase:ImagePending | LS-STOP | GAP: a relaunch while the seat's rejoin is in flight: the conservative expectation keeps the rejoin's session | walked |
| host-goodbye | 1 | round=ended sess=ended | NS-PHASE HOSTLOSS-HELD |  | walked |
| host-lost | 1 | sess=ended | NS-PHASE |  | walked |
| migration-begin | 2 | api=refused round=run | HOSTLOSS-HELD |  | walked |
| migration-complete | 2 | n/a: raised only while a host migration runs | DESIGN-MIGRATION |  | not walked: needs a migration in progress (three peers); not composed in this row |
| migration-fail | 2 | n/a: raised only while a host migration runs | DESIGN-MIGRATION |  | not walked: needs a migration in progress (three peers); not composed in this row |
| migration-successor-lost | 2 | n/a: raised only while a host migration runs | DESIGN-MIGRATION |  | not walked: needs a migration in progress (three peers); not composed in this row |
| late-join | 2 | api=refused round=run sess=phase:ImagePending | TICKET RECLAIM |  | walked |
| ticket-rejoin | 1 | seat=Held round=run sess=phase:ImagePending | ADMISSION |  | walked |
| held-rejoin | 1 | api=ok seat=Reclaiming round=run sess=phase:ImagePending | RETURN RECLAIM |  | walked |
| kick | 1 | seat=Left round=run sess=ended:ParticipantRemoved | NP-KICK MATCH-END |  | walked |
| ban | 1 | seat=Left round=run sess=ended:ParticipantBanned | NP-KICK MATCH-END |  | walked |
| seat-release | 1 | seat=Left round=run | LS-STOP MATCH-END |  | walked |
| own-cap | 1 | seat=Held round=run sess=ended | LEAVE LEAVE-HELD |  | walked |
| resume-from-disk | 2 | refuse: a running round is not replaced by a disk resume (conservative) | GAP | GAP: a resume from disk requested while a round runs | not walked: a NetMatchService step (private capture writer, world join, checkpoint resume) that needs an activity load; the in-process rig has none |
| match-over | 1 | round=over sess=ended | NS-PHASE |  | walked |
| link-blip | 1 | seat=Held round=run sess=phase:ImagePending | RETURN |  | walked |
| link-restore | 1 | seat=Held round=run sess=phase:ImagePending | RETURN |  | walked |

## Rejoin:Loading

| event | tier | expected outcome | source | gap | walk |
|---|---|---|---|---|---|
| hold-proposed | 1 | seat=Held round=run holds=0 sess=phase:Loading | HOLD |  | walked |
| hold-resolved | 1 | seat=Held round=run sess=phase:Loading | RETURN RECLAIM |  | walked |
| park-begin | 1 | seat=Held round=run holds=0 sess=phase:Loading | LS-PARK |  | walked |
| park-end | 1 | seat=Held round=run holds=0 sess=phase:Loading | GAP | GAP: a park end with no park open | walked |
| private-capture-complete | 1 | ignore: no seat waits on that image (conservative) | GAP | GAP: a private image completing for a seat that is not waiting on one | not walked: a NetMatchService step (private capture writer, world join, checkpoint resume) that needs an activity load; the in-process rig has none |
| world-image-offered | 2 | ignore: the load in progress finishes first (conservative) | GAP | GAP: a newer world image offered while an older one loads or replays | not walked: a NetMatchService step (private capture writer, world join, checkpoint resume) that needs an activity load; the in-process rig has none |
| opening-resume-offer | 2 | refuse: the opening resume offer is retired once the round runs past its anchor; a rejoin takes the image path with the newest image | WORLD-RESUME |  | not walked: a NetMatchService step (private capture writer, world join, checkpoint resume) that needs an activity load; the in-process rig has none |
| tail-replay-complete | 1 | n/a: the completion is raised only by the seat's own tail replay | NS-PHASE |  | not walked: a NetMatchService step (private capture writer, world join, checkpoint resume) that needs an activity load; the in-process rig has none |
| resync-relaunch | 1 | round=relaunch sess=phase:Loading | LS-STOP | GAP: a relaunch while the seat's rejoin is in flight: the conservative expectation keeps the rejoin's session | walked |
| host-goodbye | 1 | round=ended sess=ended | NS-PHASE HOSTLOSS-HELD |  | walked |
| host-lost | 1 | sess=ended | NS-PHASE |  | walked |
| migration-begin | 2 | api=refused round=run | HOSTLOSS-HELD |  | walked |
| migration-complete | 2 | n/a: raised only while a host migration runs | DESIGN-MIGRATION |  | not walked: needs a migration in progress (three peers); not composed in this row |
| migration-fail | 2 | n/a: raised only while a host migration runs | DESIGN-MIGRATION |  | not walked: needs a migration in progress (three peers); not composed in this row |
| migration-successor-lost | 2 | n/a: raised only while a host migration runs | DESIGN-MIGRATION |  | not walked: needs a migration in progress (three peers); not composed in this row |
| late-join | 2 | api=refused round=run sess=phase:Loading | TICKET RECLAIM |  | walked |
| ticket-rejoin | 1 | seat=Held round=run sess=phase:Loading | ADMISSION |  | walked |
| held-rejoin | 1 | api=ok seat=Reclaiming round=run sess=phase:Loading | RETURN RECLAIM |  | walked |
| kick | 1 | seat=Left round=run sess=ended:ParticipantRemoved | NP-KICK MATCH-END |  | walked |
| ban | 1 | seat=Left round=run sess=ended:ParticipantBanned | NP-KICK MATCH-END |  | walked |
| seat-release | 1 | seat=Left round=run | LS-STOP MATCH-END |  | walked |
| own-cap | 1 | seat=Held round=run sess=ended | LEAVE LEAVE-HELD |  | walked |
| resume-from-disk | 2 | refuse: a running round is not replaced by a disk resume (conservative) | GAP | GAP: a resume from disk requested while a round runs | not walked: a NetMatchService step (private capture writer, world join, checkpoint resume) that needs an activity load; the in-process rig has none |
| match-over | 1 | round=over sess=ended | NS-PHASE |  | walked |
| link-blip | 1 | seat=Held round=run sess=phase:Loading | RETURN |  | walked |
| link-restore | 1 | seat=Held round=run sess=phase:Loading | RETURN |  | walked |

## Rejoin:TailReplay

| event | tier | expected outcome | source | gap | walk |
|---|---|---|---|---|---|
| hold-proposed | 1 | seat=Held round=run holds=0 sess=phase:TailReplay | HOLD |  | walked |
| hold-resolved | 1 | seat=Held round=run sess=phase:TailReplay | RETURN RECLAIM |  | walked |
| park-begin | 1 | seat=Held round=run holds=0 sess=phase:TailReplay | LS-PARK |  | walked |
| park-end | 1 | seat=Held round=run holds=0 sess=phase:TailReplay | GAP | GAP: a park end with no park open | walked |
| private-capture-complete | 1 | ignore: no seat waits on that image (conservative) | GAP | GAP: a private image completing for a seat that is not waiting on one | not walked: a NetMatchService step (private capture writer, world join, checkpoint resume) that needs an activity load; the in-process rig has none |
| world-image-offered | 2 | ignore: the load in progress finishes first (conservative) | GAP | GAP: a newer world image offered while an older one loads or replays | not walked: a NetMatchService step (private capture writer, world join, checkpoint resume) that needs an activity load; the in-process rig has none |
| opening-resume-offer | 2 | refuse: the opening resume offer is retired once the round runs past its anchor; a rejoin takes the image path with the newest image | WORLD-RESUME |  | not walked: a NetMatchService step (private capture writer, world join, checkpoint resume) that needs an activity load; the in-process rig has none |
| tail-replay-complete | 1 | legal: TailReplay -> Active; the seat plays live from its activation | WORLD-IMAGE RETURN |  | not walked: a NetMatchService step (private capture writer, world join, checkpoint resume) that needs an activity load; the in-process rig has none |
| resync-relaunch | 1 | round=relaunch sess=phase:TailReplay | LS-STOP | GAP: a relaunch while the seat's rejoin is in flight: the conservative expectation keeps the rejoin's session | walked |
| host-goodbye | 1 | round=ended sess=ended | NS-PHASE HOSTLOSS-HELD |  | walked |
| host-lost | 1 | sess=ended | NS-PHASE |  | walked |
| migration-begin | 2 | api=refused round=run | HOSTLOSS-HELD |  | walked |
| migration-complete | 2 | n/a: raised only while a host migration runs | DESIGN-MIGRATION |  | not walked: needs a migration in progress (three peers); not composed in this row |
| migration-fail | 2 | n/a: raised only while a host migration runs | DESIGN-MIGRATION |  | not walked: needs a migration in progress (three peers); not composed in this row |
| migration-successor-lost | 2 | n/a: raised only while a host migration runs | DESIGN-MIGRATION |  | not walked: needs a migration in progress (three peers); not composed in this row |
| late-join | 2 | api=refused round=run sess=phase:TailReplay | TICKET RECLAIM |  | walked |
| ticket-rejoin | 1 | seat=Held round=run sess=phase:TailReplay | ADMISSION |  | walked |
| held-rejoin | 1 | api=ok seat=Reclaiming round=run sess=phase:TailReplay | RETURN RECLAIM |  | walked |
| kick | 1 | seat=Left round=run sess=ended:ParticipantRemoved | NP-KICK MATCH-END |  | walked |
| ban | 1 | seat=Left round=run sess=ended:ParticipantBanned | NP-KICK MATCH-END |  | walked |
| seat-release | 1 | seat=Left round=run | LS-STOP MATCH-END |  | walked |
| own-cap | 1 | seat=Held round=run sess=ended | LEAVE LEAVE-HELD |  | walked |
| resume-from-disk | 2 | refuse: a running round is not replaced by a disk resume (conservative) | GAP | GAP: a resume from disk requested while a round runs | not walked: a NetMatchService step (private capture writer, world join, checkpoint resume) that needs an activity load; the in-process rig has none |
| match-over | 1 | round=over sess=ended | NS-PHASE |  | walked |
| link-blip | 1 | seat=Held round=run sess=phase:TailReplay | RETURN |  | walked |
| link-restore | 1 | seat=Held round=run sess=phase:TailReplay | RETURN |  | walked |

## Parked

| event | tier | expected outcome | source | gap | walk |
|---|---|---|---|---|---|
| hold-proposed | 1 | seat=Held round=run peer=held sess=ready | HOLD LS-PARK |  | walked |
| hold-resolved | 1 | seat=Active round=run holds=0 sess=ready | LS-STOP |  | walked |
| park-begin | 1 | seat=Active round=run peer=run holds=0 sess=ready | LS-PARK |  | walked |
| park-end | 1 | seat=Active round=run peer=run holds=0 sess=ready | LS-PARK |  | walked |
| private-capture-complete | 1 | ignore: no seat waits on that image (conservative) | GAP | GAP: a private image completing for a seat that is not waiting on one | not walked: a NetMatchService step (private capture writer, world join, checkpoint resume) that needs an activity load; the in-process rig has none |
| world-image-offered | 2 | ignore: no seat waits on an image (conservative) | GAP | GAP: a world image offered to a seat that is not waiting on one | not walked: a NetMatchService step (private capture writer, world join, checkpoint resume) that needs an activity load; the in-process rig has none |
| opening-resume-offer | 2 | refuse: the opening resume offer is retired once the round runs past its anchor; a rejoin takes the image path with the newest image | WORLD-RESUME |  | not walked: a NetMatchService step (private capture writer, world join, checkpoint resume) that needs an activity load; the in-process rig has none |
| tail-replay-complete | 1 | n/a: the completion is raised only by the seat's own tail replay | NS-PHASE |  | not walked: a NetMatchService step (private capture writer, world join, checkpoint resume) that needs an activity load; the in-process rig has none |
| resync-relaunch | 1 | round=relaunch peer=relaunch sess=ready | LS-STOP |  | walked |
| host-goodbye | 1 | round=ended peer=noresync sess=ended | HOSTLOSS-HELD HOSTDROP TICKET |  | walked |
| host-lost | 1 | peer=noresync sess=ended | HOSTLOSS-HELD HOSTDROP |  | walked |
| migration-begin | 2 | api=refused round=run | HOSTLOSS-HELD |  | walked |
| migration-complete | 2 | n/a: raised only while a host migration runs | DESIGN-MIGRATION |  | not walked: needs a migration in progress (three peers); not composed in this row |
| migration-fail | 2 | n/a: raised only while a host migration runs | DESIGN-MIGRATION |  | not walked: needs a migration in progress (three peers); not composed in this row |
| migration-successor-lost | 2 | n/a: raised only while a host migration runs | DESIGN-MIGRATION |  | not walked: needs a migration in progress (three peers); not composed in this row |
| late-join | 2 | api=refused round=run sess=ready | TICKET RECLAIM |  | walked |
| ticket-rejoin | 1 | seat=Active round=run peer=run holds=0 sess=ready | ADMISSION TRANSPORT |  | walked |
| held-rejoin | 1 | api=refused seat=Active round=run sess=ready | SEAT-ID RECLAIM |  | walked |
| kick | 1 | seat=Left round=run sess=ended:ParticipantRemoved | NP-KICK MATCH-END |  | walked |
| ban | 1 | seat=Left round=run sess=ended:ParticipantBanned | NP-KICK MATCH-END |  | walked |
| seat-release | 1 | seat=Active round=run holds=0 sess=ready | LS-STOP |  | walked |
| own-cap | 1 | seat=Held round=run sess=ended | LEAVE LEAVE-HELD |  | walked |
| resume-from-disk | 2 | refuse: a running round is not replaced by a disk resume (conservative) | GAP | GAP: a resume from disk requested while a round runs | not walked: a NetMatchService step (private capture writer, world join, checkpoint resume) that needs an activity load; the in-process rig has none |
| match-over | 1 | round=over peer=over sess=ended | LS-STOP NS-PHASE |  | walked |
| link-blip | 1 | seat=Active round=run peer=run holds=0 sess=ready | HOLD |  | walked |
| link-restore | 1 | seat=Active round=run peer=run holds=0 sess=ready | RETURN |  | walked |

## Relaunching

| event | tier | expected outcome | source | gap | walk |
|---|---|---|---|---|---|
| hold-proposed | 1 | api=refused round=relaunch sess=ready | LS-STOP |  | walked |
| hold-resolved | 1 | round=relaunch sess=ready | LS-STOP |  | walked |
| park-begin | 1 | round=relaunch sess=ready | LS-STOP |  | walked |
| park-end | 1 | round=relaunch sess=ready | LS-STOP |  | walked |
| private-capture-complete | 1 | ignore: no seat waits on that image (conservative) | GAP | GAP: a private image completing for a seat that is not waiting on one | not walked: a NetMatchService step (private capture writer, world join, checkpoint resume) that needs an activity load; the in-process rig has none |
| world-image-offered | 2 | ignore: no seat waits on an image (conservative) | GAP | GAP: a world image offered to a seat that is not waiting on one | not walked: a NetMatchService step (private capture writer, world join, checkpoint resume) that needs an activity load; the in-process rig has none |
| opening-resume-offer | 2 | refuse: the opening resume offer is retired once the round runs past its anchor; a rejoin takes the image path with the newest image | WORLD-RESUME |  | not walked: a NetMatchService step (private capture writer, world join, checkpoint resume) that needs an activity load; the in-process rig has none |
| tail-replay-complete | 1 | n/a: the completion is raised only by the seat's own tail replay | NS-PHASE |  | not walked: a NetMatchService step (private capture writer, world join, checkpoint resume) that needs an activity load; the in-process rig has none |
| resync-relaunch | 1 | round=relaunch sess=ready | LS-STOP |  | walked |
| host-goodbye | 1 | sess=ended | TICKET HOSTLOSS-HELD |  | walked |
| host-lost | 1 | sess=ended | HOSTLOSS-HELD |  | walked |
| migration-begin | 2 | api=refused round=relaunch | HOSTLOSS-HELD |  | walked |
| migration-complete | 2 | n/a: raised only while a host migration runs | DESIGN-MIGRATION |  | not walked: needs a migration in progress (three peers); not composed in this row |
| migration-fail | 2 | n/a: raised only while a host migration runs | DESIGN-MIGRATION |  | not walked: needs a migration in progress (three peers); not composed in this row |
| migration-successor-lost | 2 | n/a: raised only while a host migration runs | DESIGN-MIGRATION |  | not walked: needs a migration in progress (three peers); not composed in this row |
| late-join | 2 | api=refused round=relaunch | TICKET RECLAIM |  | walked |
| ticket-rejoin | 1 | round=relaunch sess=ready | ADMISSION |  | walked |
| held-rejoin | 1 | api=refused round=relaunch sess=ready | GAP | GAP: a returner arriving during a relaunch (conservative: the running round refuses; the relaunch's own admission carries it) | walked |
| kick | 1 | round=relaunch sess=ended:ParticipantRemoved | NP-KICK MATCH-END |  | walked |
| ban | 1 | round=relaunch sess=ended:ParticipantBanned | NP-KICK MATCH-END |  | walked |
| seat-release | 1 | round=relaunch sess=ready | LS-STOP |  | walked |
| own-cap | 1 | round=relaunch sess=ended | LEAVE |  | walked |
| resume-from-disk | 2 | legal: the relaunch loads the match's newest checkpoint from disk on every peer | DISK-RESUME |  | not walked: a NetMatchService step (private capture writer, world join, checkpoint resume) that needs an activity load; the in-process rig has none |
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
| host-goodbye | 2 | n/a: the host is already gone | HOSTLOSS-HELD |  | not walked: the lost host sends nothing |
| host-lost | 2 | sub=run subhost=2 | HOSTLOSS-HELD |  | walked |
| migration-begin | 2 | sub=run subhost=2 | DESIGN-MIGRATION |  | walked |
| migration-complete | 2 | sub=run subhost=2 | HOSTLOSS-HELD |  | walked |
| migration-fail | 2 | legal: the survivors leave to the landing with 'The host left the match' | HOSTLOSS-HELD |  | not walked: no in-process lever fails a migration short of losing every successor |
| migration-successor-lost | 2 | sub=unreachable subhost=1 | QUORUM |  | walked |
| late-join | 2 | api=refused sub=run subhost=2 | TICKET |  | walked |
| ticket-rejoin | 2 | sub=run subhost=2 | ADMISSION |  | walked |
| held-rejoin | 2 | api=refused sub=run subhost=2 | GAP | GAP: an event other than the migration's own steps arriving while the host is being replaced | walked |
| kick | 2 | sub=run subhost=2 | GAP | GAP: an event other than the migration's own steps arriving while the host is being replaced | walked |
| ban | 2 | sub=run subhost=2 | GAP | GAP: an event other than the migration's own steps arriving while the host is being replaced | walked |
| seat-release | 2 | sub=run subhost=2 | GAP | GAP: an event other than the migration's own steps arriving while the host is being replaced | walked |
| own-cap | 2 | sub=over | LEAVE MATCH-END |  | walked |
| resume-from-disk | 2 | ignore until the migration completes (conservative) | GAP | GAP: an event other than the migration's own steps arriving while the host is being replaced | not walked: a NetMatchService step (private capture writer, world join, checkpoint resume) that needs an activity load; the in-process rig has none |
| match-over | 2 | refuse: no host remains to end the match until the successor hosts | GAP | GAP: an end of match requested while the host is being replaced | not walked: no peer is the host while the migration runs, so nothing authors a match end |
| link-blip | 2 | sub=run subhost=2 | HOLD |  | walked |
| link-restore | 2 | sub=run subhost=2 | HOLD |  | walked |

## Draining

| event | tier | expected outcome | source | gap | walk |
|---|---|---|---|---|---|
| hold-proposed | 1 | seat=Active round=run holds=0 sess=ready | LS-DRAIN |  | walked |
| hold-resolved | 1 | seat=Active round=run holds=0 sess=ready | LS-STOP |  | walked |
| park-begin | 1 | round=run holds=0 sess=ready | GAP | GAP: a capture park opened after the round's last tick | walked |
| park-end | 1 | round=run holds=0 sess=ready | GAP | GAP: a park end with no park open | walked |
| private-capture-complete | 1 | ignore: no seat waits on that image (conservative) | GAP | GAP: a private image completing for a seat that is not waiting on one | not walked: a NetMatchService step (private capture writer, world join, checkpoint resume) that needs an activity load; the in-process rig has none |
| world-image-offered | 2 | ignore: no seat waits on an image (conservative) | GAP | GAP: a world image offered to a seat that is not waiting on one | not walked: a NetMatchService step (private capture writer, world join, checkpoint resume) that needs an activity load; the in-process rig has none |
| opening-resume-offer | 2 | refuse: the opening resume offer is retired once the round runs past its anchor; a rejoin takes the image path with the newest image | WORLD-RESUME |  | not walked: a NetMatchService step (private capture writer, world join, checkpoint resume) that needs an activity load; the in-process rig has none |
| tail-replay-complete | 1 | n/a: the completion is raised only by the seat's own tail replay | NS-PHASE |  | not walked: a NetMatchService step (private capture writer, world join, checkpoint resume) that needs an activity load; the in-process rig has none |
| resync-relaunch | 1 | round=run sess=ready | GAP | GAP: a relaunch requested after the round's last tick | walked |
| host-goodbye | 1 | round=ended peer=noresync sess=ended | HOSTLOSS-HELD HOSTDROP TICKET |  | walked |
| host-lost | 1 | peer=noresync sess=ended | HOSTLOSS-HELD HOSTDROP |  | walked |
| migration-begin | 2 | api=refused round=run | HOSTLOSS-HELD |  | walked |
| migration-complete | 2 | n/a: raised only while a host migration runs | DESIGN-MIGRATION |  | not walked: needs a migration in progress (three peers); not composed in this row |
| migration-fail | 2 | n/a: raised only while a host migration runs | DESIGN-MIGRATION |  | not walked: needs a migration in progress (three peers); not composed in this row |
| migration-successor-lost | 2 | n/a: raised only while a host migration runs | DESIGN-MIGRATION |  | not walked: needs a migration in progress (three peers); not composed in this row |
| late-join | 2 | api=refused round=run sess=ready | TICKET RECLAIM |  | walked |
| ticket-rejoin | 1 | round=run holds=0 sess=ready | ADMISSION |  | walked |
| held-rejoin | 1 | api=refused seat=Active round=run sess=ready | SEAT-ID RECLAIM |  | walked |
| kick | 1 | seat=Left round=run sess=ended:ParticipantRemoved | NP-KICK MATCH-END |  | walked |
| ban | 1 | seat=Left round=run sess=ended:ParticipantBanned | NP-KICK MATCH-END |  | walked |
| seat-release | 1 | seat=Active round=run holds=0 sess=ready | LS-STOP |  | walked |
| own-cap | 1 | peer=over sess=ended | LS-DRAIN |  | walked |
| resume-from-disk | 2 | refuse: a running round is not replaced by a disk resume (conservative) | GAP | GAP: a resume from disk requested while a round runs | not walked: a NetMatchService step (private capture writer, world join, checkpoint resume) that needs an activity load; the in-process rig has none |
| match-over | 1 | round=over peer=over sess=ended | LS-STOP NS-PHASE |  | walked |
| link-blip | 1 | round=run holds=0 sess=ready | LS-DRAIN |  | walked |
| link-restore | 1 | round=run holds=0 sess=ready | LS-DRAIN |  | walked |

## Left

| event | tier | expected outcome | source | gap | walk |
|---|---|---|---|---|---|
| hold-proposed | 1 | seat=Held peer=left round=run holds=0 sess=ready | HOLD |  | walked |
| hold-resolved | 1 | seat=Held peer=left round=run sess=ready | RETURN RECLAIM |  | walked |
| park-begin | 1 | seat=Held peer=left round=run holds=0 sess=ready | LS-PARK |  | walked |
| park-end | 1 | seat=Held peer=left round=run holds=0 sess=ready | GAP | GAP: a park end with no park open | walked |
| private-capture-complete | 1 | ignore: no seat waits on that image (conservative) | GAP | GAP: a private image completing for a seat that is not waiting on one | not walked: a NetMatchService step (private capture writer, world join, checkpoint resume) that needs an activity load; the in-process rig has none |
| world-image-offered | 2 | ignore: no seat waits on an image (conservative) | GAP | GAP: a world image offered to a seat that is not waiting on one | not walked: a NetMatchService step (private capture writer, world join, checkpoint resume) that needs an activity load; the in-process rig has none |
| opening-resume-offer | 2 | refuse: the opening resume offer is retired once the round runs past its anchor; a rejoin takes the image path with the newest image | WORLD-RESUME |  | not walked: a NetMatchService step (private capture writer, world join, checkpoint resume) that needs an activity load; the in-process rig has none |
| tail-replay-complete | 1 | n/a: the completion is raised only by the seat's own tail replay | NS-PHASE |  | not walked: a NetMatchService step (private capture writer, world join, checkpoint resume) that needs an activity load; the in-process rig has none |
| resync-relaunch | 1 | round=relaunch seat=Held sess=ready | LS-STOP LEAVE-HELD |  | walked |
| host-goodbye | 1 | round=ended peer=left sess=ended | HOSTLOSS-HELD TICKET |  | walked |
| host-lost | 1 | peer=left sess=ended | HOSTLOSS-HELD |  | walked |
| migration-begin | 2 | api=refused round=run | HOSTLOSS-HELD |  | walked |
| migration-complete | 2 | n/a: raised only while a host migration runs | DESIGN-MIGRATION |  | not walked: needs a migration in progress (three peers); not composed in this row |
| migration-fail | 2 | n/a: raised only while a host migration runs | DESIGN-MIGRATION |  | not walked: needs a migration in progress (three peers); not composed in this row |
| migration-successor-lost | 2 | n/a: raised only while a host migration runs | DESIGN-MIGRATION |  | not walked: needs a migration in progress (three peers); not composed in this row |
| late-join | 2 | api=refused round=run sess=ready | TICKET RECLAIM |  | walked |
| ticket-rejoin | 1 | seat=Held peer=left round=run sess=ready | ADMISSION |  | walked |
| held-rejoin | 1 | api=ok seat=Reclaiming round=run sess=ready | LEAVE-HELD RETURN |  | walked |
| kick | 1 | seat=Left round=run sess=ended:ParticipantRemoved | NP-KICK MATCH-END |  | walked |
| ban | 1 | seat=Left round=run sess=ended:ParticipantBanned | NP-KICK MATCH-END |  | walked |
| seat-release | 1 | seat=Left round=run sess=ready | LS-STOP MATCH-END |  | walked |
| own-cap | 1 | seat=Held round=run sess=ended | LEAVE LEAVE-HELD |  | walked |
| resume-from-disk | 2 | refuse: a running round is not replaced by a disk resume (conservative) | GAP | GAP: a resume from disk requested while a round runs | not walked: a NetMatchService step (private capture writer, world join, checkpoint resume) that needs an activity load; the in-process rig has none |
| match-over | 1 | round=over sess=ended | NS-PHASE |  | walked |
| link-blip | 1 | seat=Held peer=left round=run sess=ready | RETURN |  | walked |
| link-restore | 1 | seat=Held peer=left round=run sess=ready | RETURN |  | walked |

## Gap list (64 pairs)

| state | event | gap |
|---|---|---|
| Active | park-end | a park end with no park open |
| Active | private-capture-complete | a private image completing for a seat that is not waiting on one |
| Active | world-image-offered | a world image offered to a seat that is not waiting on one |
| Active | resume-from-disk | a resume from disk requested while a round runs |
| Held | park-end | a park end with no park open |
| Held | world-image-offered | a world image offered to a seat that is not waiting on one |
| Held | resume-from-disk | a resume from disk requested while a round runs |
| Reclaiming | hold-proposed | a hold proposed for a seat whose reclaim is agreed but not active yet (nothing is owed by it before E) |
| Reclaiming | park-end | a park end with no park open |
| Reclaiming | private-capture-complete | a private image completing for a seat that is not waiting on one |
| Reclaiming | world-image-offered | a world image offered to a seat that is not waiting on one |
| Reclaiming | resync-relaunch | no policy says what an agreed reclaim becomes across a relaunch |
| Reclaiming | seat-release | a release of a seat whose reclaim is agreed |
| Reclaiming | own-cap | a returner that reaches its own cap before its activation frame |
| Reclaiming | resume-from-disk | a resume from disk requested while a round runs |
| Rejoin:Connecting | park-end | a park end with no park open |
| Rejoin:Connecting | private-capture-complete | a private image completing for a seat that is not waiting on one |
| Rejoin:Connecting | world-image-offered | a world image offered to a seat that is not waiting on one |
| Rejoin:Connecting | resync-relaunch | a relaunch while the seat's rejoin is in flight: the conservative expectation keeps the rejoin's session |
| Rejoin:Connecting | resume-from-disk | a resume from disk requested while a round runs |
| Rejoin:ImagePending | park-end | a park end with no park open |
| Rejoin:ImagePending | resync-relaunch | a relaunch while the seat's rejoin is in flight: the conservative expectation keeps the rejoin's session |
| Rejoin:ImagePending | resume-from-disk | a resume from disk requested while a round runs |
| Rejoin:Loading | park-end | a park end with no park open |
| Rejoin:Loading | private-capture-complete | a private image completing for a seat that is not waiting on one |
| Rejoin:Loading | world-image-offered | a newer world image offered while an older one loads or replays |
| Rejoin:Loading | resync-relaunch | a relaunch while the seat's rejoin is in flight: the conservative expectation keeps the rejoin's session |
| Rejoin:Loading | resume-from-disk | a resume from disk requested while a round runs |
| Rejoin:TailReplay | park-end | a park end with no park open |
| Rejoin:TailReplay | private-capture-complete | a private image completing for a seat that is not waiting on one |
| Rejoin:TailReplay | world-image-offered | a newer world image offered while an older one loads or replays |
| Rejoin:TailReplay | resync-relaunch | a relaunch while the seat's rejoin is in flight: the conservative expectation keeps the rejoin's session |
| Rejoin:TailReplay | resume-from-disk | a resume from disk requested while a round runs |
| Parked | private-capture-complete | a private image completing for a seat that is not waiting on one |
| Parked | world-image-offered | a world image offered to a seat that is not waiting on one |
| Parked | resume-from-disk | a resume from disk requested while a round runs |
| Relaunching | private-capture-complete | a private image completing for a seat that is not waiting on one |
| Relaunching | world-image-offered | a world image offered to a seat that is not waiting on one |
| Relaunching | held-rejoin | a returner arriving during a relaunch (conservative: the running round refuses; the relaunch's own admission carries it) |
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
| Migrating | resume-from-disk | an event other than the migration's own steps arriving while the host is being replaced |
| Migrating | match-over | an end of match requested while the host is being replaced |
| Draining | park-begin | a capture park opened after the round's last tick |
| Draining | park-end | a park end with no park open |
| Draining | private-capture-complete | a private image completing for a seat that is not waiting on one |
| Draining | world-image-offered | a world image offered to a seat that is not waiting on one |
| Draining | resync-relaunch | a relaunch requested after the round's last tick |
| Draining | resume-from-disk | a resume from disk requested while a round runs |
| Left | park-end | a park end with no park open |
| Left | private-capture-complete | a private image completing for a seat that is not waiting on one |
| Left | world-image-offered | a world image offered to a seat that is not waiting on one |
| Left | resume-from-disk | a resume from disk requested while a round runs |
