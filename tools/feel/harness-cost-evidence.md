# Instrument cost validity

The required cost pin names sim dump, tick-end hashes/controller records, full
state, memory census, preview fidelity, stall sampling, screen watches and video
recording. Existing `[harness-cost]`, `[fullstate-cost]`, `census_us` and preview
`harness_ms_total` observations are retained with source lines and original
measurement meaning. Cumulative snapshots are not added together; missing costs
are never zero. Partial summaries cannot prove complete per-frame coverage.

To certify a run, native instrumentation must emit one `[harness-cost-scope]`
JSON object per process/incarnation/round, with version=1, numeric process ID,
incarnation, round, inclusive first_frame/last_frame and `instruments`, an object
mapping all eight names in `feel.harness_cost.INSTRUMENTS` to their effective
runtime enabled state. A false value is an explicit runtime disabled receipt,
not absence of a cost log; contradictory native observations fail.

For every frame in that declared interval, `[harness-cost-frame]` carries the
same process/incarnation/round, frame, partition_valid=true and `costs_ms` for
every enabled instrument, including measured zero. Each value is measured
exclusive elapsed work charged to that frame. Nested instrumentation must be
partitioned at its actual boundaries to prevent double counting. Work on the
fullstate writer, census worker, sampler or encoder must be attributed by native
measurement; a guessed per-frame allocation is invalid. If that accounting is
unavailable, keep partition_valid=false and the run is instrument-invalid.

The tools sum only this measured partition, retaining per-instrument totals and
maxima, with the existing 50 ms frame budget. Individual native observations
above that budget also fail. The original product wall TPS, wait, horizon and
input values are never adjusted or subtracted. `product_pass` is reported apart
from `instrument_valid`; acceptance requires both. Video review emits a required
harness-cost item, and cross reports require every incarnation's cost validity.

Current source lacks complete cost receipts for the sampler, watches and
recorder and lacks the complete partition above. Adding those native receipts,
then collecting quiet timing/capture evidence, is an engine-side request.
