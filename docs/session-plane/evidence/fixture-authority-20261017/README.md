# Fixture authority ruling evidence — 2026-10-17

Tested source `909e80ada525b70931d76b2eb5d95f94a15cc79c`; executable `00bfbb10305f3ac2a29a4f09a1308320ac58e70f44f92daf2258ebf741b24e0c`; Linux, v6 dependency. Single-box detectors only.

Complete suite 46/55; fixed 41/233; all 108 saved schedules; random 2/200 with all 200 executed in 28.973 runner seconds. Default headless policy probes 2/2. Reviewer mapping covers all 121.

- [Final report and every RED row](FINDINGS.txt)
- [Suite](suite.json), [fixed catalogue](catalogue.json), [reviewer shapes](reviewer-shapes.json)
- [Every random group and failed seed](random-schedules.json), [nested suite REDS](nested-suite-reds.json)
- [Fixed stdout](fixed-stdout.log.gz), [random stdout](random-stdout.log.gz), [runner output](runner.log.gz)
- [Invocation](invocation.json), [process samples](load.json), [all launch receipts](runner-launches.json)
- [Source, merges and dependency](source-and-merge.json), [combined build](compilation-combined.log.gz), [v6 configure](configure-v6.log.gz)
- [Headless routing checks](headless-mode-routing.json), [runner self-check](runner-self-test-final.txt), [20 runner unit tests](runner-unit-tests-final.txt)
- [Earlier completed network milestone and focused results](completed-network-milestone.json)

There are 54 headed receipts and exactly two headless policy receipts. Every receipt names the tested executable. No --headless-policy-rows option appears in the suite invocation.
The N=200 interval has 6 process samples and zero foreign work samples. The internal driver reports 28 seconds; the official runner records 28.973 seconds. All failed seeds remain RED.
The three explicit split rows stop at the immediate AI boundary; this run does not prove their later losing-half image return. No engine source was manually modified by the fixture lane.

- [Signed directory capacity check](signed-directory-capacity.json): fresh GREEN; the directory suite also ran all 142 tests.
- [Lossless log archive manifest](log-archives.json), including each compressed and uncompressed SHA-256. Raw originals remain in the local scratch and evidence directories.

The later session-engineer build began at 1:44 PM MST; N=200 ended at 1:41 PM. Other suite phases overlapped that build, so the whole-suite invocation correctly records quiet_throughout=false.

Published paths are relative. [Normalization manifest](sanitization.json) records original and published hashes; the earlier archive manifest describes the retained original bytes. Original files are retained privately. The fixture findings above concern its recorded executable, not the later engine changes.
