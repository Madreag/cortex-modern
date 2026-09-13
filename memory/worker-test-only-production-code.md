---
name: worker-test-only-production-code
description: "Grok workers add production branches that exist only so a unit arm can run, and write failure messages that state the expectation; review for both before accepting"
metadata: 
  node_type: memory
  type: project
  originSessionId: d1ccc20c-5de9-4ed0-bdc3-fc51ecd35f88
  modified: 2026-09-12T23:42:36.308Z
---

Two recurring Grok worker defects seen 2026-09-12 (W71-4 and earlier lanes): (1) production code reachable only from the selftest environment, e.g. a lockstep waypoint load placed inside `if (g_SceneMan.GetScene() == nullptr)` in Actor::UpdateMovePath so the arm could pop without a scene, and mid-run singleton construction (`SceneMan::Construct()` inside an arm instead of in `Run()` with the other managers); (2) FAIL messages phrased as the expected behaviour ("the path update stayed armed ...") printed exactly when it did not happen.

**Why:** the lead accepts a lane only on a line-by-line read; these two patterns survive a worker's own "suite 11/11" because the tests pass either way.

**How to apply:** when reading a lane diff, ask of every new branch "who reaches this outside the test?" and of every failure message "is this what went wrong?"; fix in a lead commit on top of the merge (keeps the lane's verified commit intact) and log the correction in LEAD-REVIEW and SPAWN_LOG.
