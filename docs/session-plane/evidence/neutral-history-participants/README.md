# Neutral participants and committed history

Seed 197 returns seat 2 into a certified neutral interval, then hangs the
host. At the next missing host input, seat 2 owes no human sample but is
actively playing. The old input-voter predicate excluded it from local
hold eligibility, leaving no survivor eligible to hold at the 50 ms bound.
After correcting the local hold, the same predicate still rejected its
committed tail when the held host resumed, so the host never returned.

Input voting and history participation now use explicit scopes of the
same retained-seat/quorum predicates. Neutral participants can install
local holds and witness committed recovery history. Held seats, declared
departures and invalid members remain excluded. Input acceptance,
ordinary decisions and directory queries retain input-voter scope.

The expanded consumer-deadline detector is RED on `c5155a80` for the local
hold, then RED on `e61b4e14` for the authenticated committed tail. Final
`3f7f5ff8` is GREEN for both, with early-hold, private-reader, departed-seat,
invalid-member, unchanged-prefix and unchanged-input-authority negatives.
An intermediate `120cf01b` overreach made neutral directory queries vote;
its RED is retained and final code restores that query's original scope.

Final checks: 12/12 focused native controls, complete native lockstep and
world-join 2/2, original seed 197 through the host return, the 102-percent
four-seat return, 250 ms three-seat image return, repeated slowdown and
steady 250 ms/120 s controls are GREEN. Seed 80 remains RED at 51 ms
against 50 ms; this change does not claim that separate cause or a new
wider inventory score. No bound, quorum, wire, fixture property or SP
path changed.

Fifty complete headed [receipts](receipts.json) retain the transitions,
intermediate failures and final checks, single-box, proof=false. One
launch started before its pending build exited and used the previous
executable; it is explicitly invalid for verdict and was rerun idle.
Collection names are workflow labels, not verdicts. Wider history,
return and deadline groups remain open, then B3 and #45/#46. Final report
untouched.
