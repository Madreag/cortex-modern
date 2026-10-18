# Direct-ruling milestone

The complete gate is pending. These headed Mac native runs use the official
runner on one box, proof=false for field play. [Receipts and complete compressed
logs](receipts.json) retain the original, portable and compressed hashes.

Both unchanged hung-host blockers move from RED to GREEN: three seconds holds
and recovers; eighteen seconds also migrates the lone survivor. The authenticated
heal-history detector moves from RED (the losing pair remains Running) to GREEN
in ten arms, including frame zero. The retained early milestone executable
`88568bf3` passes its nine original arms, both hung-host controls, and the
complete match/startup and seat-succession suites.

Executable `4638d2b1` passes all five full 23-second direct split detectors:
two-two, lone-peer, human-majority, one-way host and one-way joiner. Each requires
both groups to hold locally and play within 50 ms, the hostless group to migrate,
the healed loser to stop at its completed decision frame and image-return, and
all adopted hashes to agree without changing the winner's history. The native
history, foreign-hold scope, wire/admission and retained member-start detectors
pass too. Both unchanged hung-host controls and the complete identity, session,
match/startup and seat-succession suites pass on that same executable.

The reader-receipt follow-up reproduces two REDs before the fix: a pre-hold
report was acknowledged as a continuation, and a reader-only acknowledgement
could not adopt the sole committed history. Executable `ba91ca1e` passes both
detectors, all five full split recoveries and both unchanged hung-host controls.
Five wrongly named case selections in `direct-reader-receipt-verified` are
retained as failed invocations, with no engine-property verdict; the corrected
full cases are in `direct-reader-full-verified`.

The original obsolete direct assertions and every intervening RED remain in the
receipts. Other complete-suite rows and the fixed/random catalogues still need
their final validation; this is not the complete gate.

The wire's only encoder and decoder are
`NetHostMigrationCodec::Encode/Decode` in `NetLockstep.cpp`. Version 8 adds the
authenticated roster revision. Direct heartbeats name their applied membership,
original split epoch and installed hold boundary. Old version 7 packets are
rejected by the same version check. Admission identity 52 rejects older peers;
the [complete encoder census](encoder-census.json) records the production and
test paths. All senders still use that codec. Heartbeat receipts require both
directions of the history exchange before selecting a competing continuation.

Source behavior changes affect lockstep matches only. Single-player code is
untouched; the final native hash and feel measurements remain required.
