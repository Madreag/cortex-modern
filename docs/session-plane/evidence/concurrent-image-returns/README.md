# Concurrent private image returns

FLAGGED fixture departure: the old blocking helper completed one private
return and its entire healthy observation before starting the next reader.
Seed 80's second reader first reported at 9329 ms and returned 819 ms past
the unchanged deadline. Independent coroutine jobs now advance on the same
shared fixture tick, through the same transported tail, private replay,
certificate, activation, history and healthy-observation checks. Their first
reports arrive at 7024/7033 ms. No deadline or observation property changes.

Concurrent readers exposed three production handoff defects:

* A fresh live handshake lacked its completed private replay prefixes, so
  it rejected a valid concurrent return certificate and then its Start.
  Native detector `e28aa4e9` RED / `adf2e8d5` GREEN. Import is limited to
  completed frames before activation, from the same session, round,
  configuration and local seat; it never advances the live cursor or ends
  private seat-state replay.
* Proposal voting still required local live history after that prefix was
  available. The direct two-two trace shows peer 3 rejecting boundary 1784
  with live start 1820, despite its completed replay proof, leaving the
  frame-1844 promise unresolved for 51 ms. Expanded detector `2a1cbbb9`
  RED / `7d2840cf` GREEN. Voting and certificates now use the same completed
  historical proof, with the original voters and bound.
* Replayed seat transitions restored controllers but omitted mesh bridge,
  hold-history and membership state. Seed 80 differed at frame 490, and
  direct two-two then differed at 1844. Native state detector `7d2840cf`
  RED / final `fb6e1bd9` GREEN. Hold and return handoff now restore the mesh
  state too, retaining actual member start terms, newer holds/returns,
  preceding history and the unchanged simulation cursor.

Final `fb6e1bd9`: the two expanded detectors, member-start control and all
five direct split/image controls pass 8/8. Complete native lockstep and
world-join suites pass 2/2. Causal-history, authority, three-seat 250 ms and
four-seat 100 ms image controls pass. One misspelled authority selector is
retained as a harness selection error and was rerun with the exact name.

Seed 80 completes both image returns and their 1001 ms healthy observations;
its remaining RED is a held-seat-3 record at 7981 ms that never closes by
9842 ms. Its losing-image source selection still treats only physical cuts
as discarded continuations; the copied hold state is the next investigation.
Seed 106 retains same-group history 248. Different-donor retains history
1694. No new wider inventory score or final gate is claimed.

Forty-five complete headed Mac [receipts](receipts.json), including every
intermediate RED, single-box/proof=false. No bound, quorum, wire-format,
permanent-delay or SP-path change.
