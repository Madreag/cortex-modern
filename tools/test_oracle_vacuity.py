"""Synthetic RED records for the six vacuous oracle sites. Written, not run.

Each tightening fails an empty or skipped input with a message that names the
missing fact, and records that a comparison happened. The completion pass
feeds these shapes; this file is the method, not a launch.
"""
from __future__ import annotations

# net_activity_launch.census_compare / compare_census_lines
# BEFORE: tick_lines missing or [] compared as [] == [] and passed.
# AFTER: empty left fails "offline census tick-1 lines missing"; empty right
# fails "match census tick-1 lines missing". A real compare then runs.
CENSUS_EMPTY_OFFLINE = {"pass": False, "reason": "offline census tick-1 lines missing"}
CENSUS_EMPTY_MATCH = {"pass": False, "reason": "match census tick-1 lines missing"}

# seat_facts.phase_b.pair_arms / gates
# BEFORE: missing p5snap saves skipped check_world; invoke() exits discarded;
# main returned 0 after a failed gate.
# AFTER: missing saves raise "missing pair snapshot for world compare";
# gates and run_pair require_zero=True so a comparison must complete.
PHASE_B_MISSING_SAVES = "missing pair snapshot for world compare"
PHASE_B_INVOKE_FAILED = "exited"

# seat_facts.check_world.compare
# BEFORE: brains is None on a pre-WorldStructure3 tag, so None == None passed.
# AFTER: brain_records_present is False and brain_records_equal cannot pass.
CHECK_WORLD_NO_BRAINS = {"brain_records_present": False, "brain_records_equal": False}

# seat_facts.compare_offline
# BEFORE: missing traces skipped the compare; red_equal_reference was unused
# in the exit; an all-green-missing row still returned 0 when pie lists were empty.
# AFTER: missing traces set reason "missing traces for offline compare";
# exit requires present and red_equal_reference is False and green is True.
COMPARE_OFFLINE_MISSING = {"present": False, "reason": "missing traces for offline compare"}

# test_net_brainless_spectate
# BEFORE: len(kills) >= human_seats (a 3-kill log passed a 2-seat roster);
# sp_arm pass ignored exit_code/timed_out.
# AFTER: len(kills) == human_seats; process_completed requires exit 0 and no timeout.
BRAINLESS_KILL_COUNT = "brain-kill lines must equal the human seat count"
BRAINLESS_PROCESS = "process_completed"

SITES = (
    ("net_activity_launch.py:compare_census_lines", CENSUS_EMPTY_OFFLINE),
    ("seat_facts/phase_b.py:pair_arms", PHASE_B_MISSING_SAVES),
    ("seat_facts/check_world.py:compare", CHECK_WORLD_NO_BRAINS),
    ("seat_facts/compare_offline.py:main", COMPARE_OFFLINE_MISSING),
    ("test_net_brainless_spectate.py:net_arm", BRAINLESS_KILL_COUNT),
    ("test_net_brainless_spectate.py:sp_arm", BRAINLESS_PROCESS),
)
