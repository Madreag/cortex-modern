# Link-delay transition work

Complete gate pending. All receipts are headed native Mac runs through the
official runner: topology single-box, proof=false. Original and portable hashes
and full compressed stdout/stderr are retained in [receipts](receipts.json).

The original complete lockstep run on `ba91ca1e` reproduces the four authorized
fixture disagreements and the 102 ms decrease wait. The corrected rows pass on
`c6621861`; the decrease retains its 50 ms bound and remains RED at 102 ms.
The exact neutral history now ends the host hold at 4420, and the frame-5000
query contains only seat 2 as a voter. A pending directory answer never fences
an installed local hold. Return trail remains entirely in its neutral interval;
machine cadence and spikes never pad the measured link delay.

The producer-phase detector is RED on `db25847e`: a committed decrease changes
input targets without advancing their producer's phase. The flagged decrease
fixture correction consumes the existing 3x/six-tick match budget and retains
owed clock credit; without the engine correction it remains RED at 119 ms on
`f85a5459`. The original no-hold, progress and 50 ms properties remain, and the
link delay must now be exactly two ticks in all three cases.

The unchanged steady-250-ms case on that executable is RED at frame 34. Active
replay consumes startup inputs faster than the receipt pipeline can replace
them, producing a premature local hold of healthy peers and different histories.
Its earlier 51 ms wait remains in the prior Mac-gate evidence.

The phase-only fix on `280a0028` passes the four-mode native producer detector,
but the decrease still waits 78 ms. Old consumer clock debt spends the new
producer phase immediately. The exactly-once consumer-rebase detector is RED on
`a883008a`. The final correction on `5ff9e654` rebases both ordinary consumer
debt and its existing bounded extra credit. Every simulation tick still runs in
order. A producer already ahead ends the transition when fresh targets resume.

On that executable all three decreases are GREEN: exactly two ticks, zero holds,
waits **22 / 0 / 0 ms**, and no retiming left active. The phase/rebase detector
and the missing/zero RTT, all-mode sizing, immediate-rise and five-second-fall
selections pass **6/6**. The complete succession row and all five direct
split/heal controls also pass.

The complete lockstep row retains six REDS: unsigned-invitation construction;
the obsolete two-two migration, isolated-host and late-voter expectations;
held-incarnation human input at frame 362; and placement-return history at frame
164. These are still open. Steady250ms is still RED; no complete-gate GREEN is
claimed.
