# Input repair includes the links between readers

An input's receipts and repair travel between its readers as well as from its
sender. The retained frame-349 trace has a sender with 100 ms adjacent links
and a 200 ms reader-to-reader link. Its seven-frame runway omits the longer
repair path: one healthy reader commits while the other takes its bounded hold.

The sender now includes the authenticated transport-derived runway already
published by active readers. Local RTT/margin publication remains local, so
this cannot feed a group maximum back into successive increases. Held and
unmeasured reports contribute nothing. No receipt age, startup duration or
machine cost enters the permanent delay; no new probe or wire field is added.
The existing immediate-rise and five-second-decrease logic remains.

The three-arm detector is RED on `57ea4c7f`, GREEN on `1b5c07e6`.
Both original receipt/deadline controls and all four D1-D3 link controls pass.
The original 102-percent different-donor row is GREEN on `1b5c07e6`, with
its wait, identical-history, complete image-return and no-rehold properties.
The earlier connected-history RED is retained under internet-authority.
Receipt and authority quorums are unchanged.

Nineteen complete headed official-runner receipts are in the [index](receipts.json),
single-box, proof=false. Complete [inventory](inventory.json): 7/9 network rows
plus world-join GREEN, 164/233 fixed cases with all 108 saved schedules, and
72/200 random schedules with all 200 executed. No harness timeout.
Every remaining RED is retained in the logs and [groups](fixed-failure-groups.txt).
The relay setup remains 33 ms against its unchanged 60 ms requirement.
Image admission, legacy bounds and wider loss/jitter/history failures remain.

The separate [receipt experiment](../partial-publication-experiment/README.md)
is fully reverted. No single-player change. B3, #45, #46 and the final gate
remain open; REPORT-final.txt is unchanged.
