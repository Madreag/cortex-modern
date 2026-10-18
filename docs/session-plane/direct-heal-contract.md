# Direct play after a host fault

The lead's resumed ruling replaces the old direct-host-cut stop. At the slow
player bound, every peer missing the host's input installs the ordinary local
AI hold with empty input and keeps playing. A client with its own link cut does
the same. Transport silence never classifies either fault.

When links heal, peers exchange their roster revision and committed progress
after their installed hold boundary. A host with no continuation follows the
survivors through private return. Two independently committed continuations are
a split: more connected human seats wins; a tie belongs to the original host's
side; without that host, the lowest original seat index wins. Administration
changes during the outage do not replace the original-host tie rule. The loser
finishes any granted tick, stops at its completed decision boundary, and rejoins
the winning history through the image return. Winning peers never replace their
displayed history.

At the hold window's end, every reachable group without the host migrates the
roster within that group. A lone survivor also migrates and keeps its game. It
still yields if the healed histories later choose the other continuation.
Reachability inside the group and committed history are the whole mechanism;
there is no new liveness probe or transport classifier.

The engine uses its existing hold histories, split-decision ledger, roster
reader and image-return path. Migration-wire version 8 and admission identity 52 guard the added roster
revision and direct heartbeat history semantics. The heartbeat's frame names
the split's common epoch; its boundary names the installed hold; its applied
frame and digest describe completed simulation, not speculative preparation.
Its explicit electorate marks a continuation; zero is a valid hold frame and
split epoch. The ledger records an installed hold separately from its number.
The heartbeat's voter mask acknowledges whose committed history this peer has
received in that epoch. Its offset mask separately acknowledges pre-hold reader
reports; receiving one never acknowledges a committed continuation. A competing branch is selected only after both
directions of that exchange complete; incoming one-way traffic cannot decide
the fault. A foreign group's local hold is a history offer to an active excluded
owner. If the healed exchange confirms that owner has no continuation, it can
adopt the standing hold as a reader. Otherwise its own local hold keeps play
moving until the split decision. Direct administration follows the installed
playing group through the hold window, including one-way cuts.

The native `direct_heal_uses_committed_histories_and_human_seats` detector checks
human counts, both tie rules, no decision from silence, a non-continuing host,
frame zero and a granted tick's completed stop boundary. Full hold, migration and image
return detectors remain required; a comparison helper alone proves none of them.
