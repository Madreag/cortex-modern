"""Run the complete retained breadth, sequentially, on one pinned approved build.

    python run_breadth.py --source 41 --build-manifest <build.json> --out D:/mx/s41b1

The output directory must not exist. Test failures are retained and do not skip later
jobs. Pin drift or a surviving engine stops further launches and leaves missing jobs
failed. This is a breadth verdict, not milestone or cross-platform sign-off.
"""

from __future__ import annotations

import argparse
from collections import Counter
from dataclasses import asdict, dataclass, field
import hashlib
import importlib.util
import json
import math
import ntpath
import os
from pathlib import Path
import re
import stat
import subprocess
import sys
import time
from typing import Callable

sys.dont_write_bytecode = True
REPO = Path("D:/Projects/p4b-interp-validation")
CONTRACT = Path("D:/Projects/reviews/recovery-2026-09-07/contract-audit")
LANES = Path("D:/Projects/reviews/claude-review-2026-09-08/lanes")
DRIVERS = LANES / "source33-lanes/driver"
STAGE = Path("D:/Projects/stage2_p4")
REFERENCE_EXE = CONTRACT / "grouped-build-4244e87e/Cortex Command.exe"
REFERENCE_SHA = "4244e87ea7fcf5f72a12b7f32523a9460278a902029c69044fe2740195268d7e"
SCRIPT = Path(__file__).resolve()
SEMANTIC = "fire_reload weapon_switch jetpack terrain_fire pie_reload ai_orders death_brainkill cargo_deliver buy_order pause".split()
INTERP = dict(zip(
    "baseline funds spawn deliver scuttle brainkill stall mismatch perturb rematch inventory buy brainspawn pause fullloop".split(),
    "- FundsCommandHost SpawnCommandHost DeliverCommandHost ScuttleCommandHost BrainKillCommandHost StallTest MismatchSmoke PerturbHost RematchTest InventoryCommandHost BuyCommandHost BrainSpawnTest PauseTest FullLoopTest".split(),
))
H4 = "gns_provider_smoke old_wire_fixture clean_leave reclaim_socket crash_relaunch_provisional fencing_two_transports rejoin_after_resync peers_3_4_regression".split()
B1 = "substitute_commit substitute_returner_wins substitute_host_cancel substitute_bounds".split()
LOBBY = [
    ("rejoin_4_200", 4, "rejoin", 200, 44160, 0),
    ("drop_4_200", 4, "drop", 200, 44170, 0),
    ("ctl_rejoin_4_100", 4, "rejoin", 100, 44200, 0),
    ("ctl_rejoin_3_200", 3, "rejoin", 200, 44180, 0),
    ("ctl_drop_3_200", 3, "drop", 200, 44190, 0),
    ("drop_4_200_early", 4, "drop", 200, 44210, 40),
]
SOUND = {"auth-match": ("match", 0), "auth-fault": ("diverge", 72),
         "wounds": ("match", 0), "rng_particles": ("match", 0),
         "ui_bus": ("match", 0), "ai_defer": ("match", 0),
         "ai_defer_alias": ("match", 0), "per_machine": ("diverge", 141)}
COMPAT = {"deferral": ("run_deferral_probe.py", None),
          "alias": ("run_alias_probe.py", "compat_review_alias.lua"),
          "alias2": ("run_alias_probe.py", "compat_review_alias2.lua"),
          "extra": ("run_extra_probe.py", None)}
REQUIRED_CASES = tuple([
    *[f"semantic_{name}" for name in SEMANTIC],
    *[f"interp_{name}" for name in INTERP], "interp_rtdesync", "fl100", "fl200",
    "invariance_300", "discovery",
    *[f"h4_{name}_{repeat}" for name in H4 for repeat in range(1, 6 if name == "peers_3_4_regression" else 2)],
    *["b1_" + name for name in B1],
    *[f"lobby_{name}_{repeat}" for name, *_ in LOBBY for repeat in range(1, 4)],
    *["sound_" + name for name in SOUND], "heal",
    *[f"compat_{name}_{rung}" for name in COMPAT for rung in ("source22", "approved")],
])

# Named check coverage and unchanged fixture identities retained from Source40.
BASELINE = json.loads(r'''
{
  "h4": {
    "clean_leave": [
      "executable_matches_manifest",
      "clean_leave_acked",
      "clean_leave_cleared_ticket",
      "host_closed_the_seat",
      "clean_old_ticket_refused",
      "ambiguous_kept_ticket",
      "ambiguous_reclaim_worked",
      "hosts_survived"
    ],
    "crash_relaunch_provisional": [
      "executable_matches_manifest",
      "arm_a_first_client_dropped",
      "arm_a_ticket_on_disk",
      "arm_a_resumed_or_rejoined",
      "arm_a_host_counted_resume",
      "arm_a_no_host_failure",
      "arm_b_first_client_dropped",
      "arm_b_provisional_expired",
      "arm_b_no_stale_commit",
      "arm_b_no_host_failure"
    ],
    "fencing_two_transports": [
      "executable_matches_manifest",
      "second_client_committed",
      "host_bound_second_incarnation",
      "host_fenced_the_old_transport",
      "seat_not_dropped_by_stale_timeout",
      "first_client_superseded",
      "host_survived"
    ],
    "gns_provider_smoke": [
      "executable_matches_manifest",
      "epoch_armed_on_host",
      "admission_attached_both",
      "client_committed_over_socket",
      "ticket_written",
      "canary_allowlist_present",
      "no_secret_in_artifacts",
      "no_crypto_unavailable_line"
    ],
    "old_wire_fixture": [
      "executable_matches_manifest",
      "old_wire_selftest_pass",
      "live_match_undisturbed",
      "host_counted_no_old_wire"
    ],
    "peers_3_4_regression": [
      "executable_matches_manifest",
      "peers3_all_exit_zero",
      "peers3_sim_gated_identical",
      "peers4_all_exit_zero",
      "peers4_sim_gated_identical",
      "drop3_returner_reclaimed",
      "drop3_survivors_identical",
      "drop3_distinct_seats",
      "no_census_refusals"
    ],
    "reclaim_socket": [
      "executable_matches_manifest",
      "host_exit_zero",
      "first_client_dropped_midmatch",
      "returner_reclaimed_stored_ticket",
      "returner_committed",
      "host_saw_seat_drop_and_return",
      "returner_on_same_stable_seat",
      "host_census_clean",
      "host_reseat_issued",
      "resync_round_survived",
      "funds_unchanged",
      "no_authority_error"
    ],
    "rejoin_after_resync": [
      "executable_matches_manifest",
      "resync_returner_reclaimed",
      "resync_round_ran",
      "resync_match_continued",
      "resync_census_clean",
      "rematch_completed",
      "rematch_ticket_survived",
      "rematch_seat_still_protected",
      "rematch_no_host_fatal",
      "no_stale_epoch_denials"
    ]
  },
  "b1": {
    "substitute_bounds": [
      "executable_recorded",
      "host_adjudicated_the_drop",
      "host_exit_zero",
      "stayer_exit_zero",
      "leaver_dropped_midmatch",
      "host_census_clean",
      "no_authority_error",
      "seat_dropped",
      "two_applicants_registered",
      "third_applicant_refused",
      "bound_respected",
      "nothing_committed",
      "applicant1_inert",
      "applicant2_inert",
      "applicant3_inert"
    ],
    "substitute_commit": [
      "executable_recorded",
      "host_adjudicated_the_drop",
      "host_exit_zero",
      "stayer_exit_zero",
      "leaver_dropped_midmatch",
      "host_census_clean",
      "no_authority_error",
      "seat_dropped",
      "applicant_registered",
      "host_approved",
      "substitution_committed",
      "substitute_offered_a_ticket",
      "substitute_joined",
      "substitute_stored_its_ticket",
      "no_reclaim_happened",
      "reseat_issued"
    ],
    "substitute_host_cancel": [
      "executable_recorded",
      "host_adjudicated_the_drop",
      "host_exit_zero",
      "stayer_exit_zero",
      "leaver_dropped_midmatch",
      "host_census_clean",
      "no_authority_error",
      "seat_dropped",
      "host_approved",
      "host_cancelled",
      "substitution_cancelled",
      "substitution_did_not_commit",
      "substitute_not_joined",
      "seat_stayed_reassignable"
    ],
    "substitute_returner_wins": [
      "executable_recorded",
      "host_adjudicated_the_drop",
      "host_exit_zero",
      "stayer_exit_zero",
      "leaver_dropped_midmatch",
      "host_census_clean",
      "no_authority_error",
      "seat_dropped",
      "returner_reclaimed",
      "returner_joined",
      "substitution_did_not_commit",
      "substitute_not_joined",
      "seat_has_one_holder"
    ]
  },
  "interp": {
    "baseline": [
      "process_exit",
      "liveness",
      "config_agreement",
      "content_identity_agreement",
      "controller_boundary",
      "controller_boundary",
      "prediction_executed",
      "simgated",
      "provenance_stable",
      "binary_matches_source",
      "required_checks_present"
    ],
    "funds": [
      "process_exit",
      "liveness",
      "config_agreement",
      "content_identity_agreement",
      "mode_fundscommandhost",
      "controller_boundary",
      "controller_boundary",
      "prediction_executed",
      "simgated",
      "provenance_stable",
      "binary_matches_source",
      "required_checks_present"
    ],
    "spawn": [
      "process_exit",
      "liveness",
      "config_agreement",
      "content_identity_agreement",
      "mode_spawncommandhost",
      "controller_boundary",
      "controller_boundary",
      "prediction_executed",
      "simgated",
      "provenance_stable",
      "binary_matches_source",
      "required_checks_present"
    ],
    "deliver": [
      "process_exit",
      "liveness",
      "config_agreement",
      "content_identity_agreement",
      "mode_delivercommandhost",
      "controller_boundary",
      "controller_boundary",
      "prediction_executed",
      "simgated",
      "provenance_stable",
      "binary_matches_source",
      "required_checks_present"
    ],
    "scuttle": [
      "process_exit",
      "liveness",
      "config_agreement",
      "content_identity_agreement",
      "mode_scuttlecommandhost",
      "controller_boundary",
      "controller_boundary",
      "prediction_executed",
      "simgated",
      "provenance_stable",
      "binary_matches_source",
      "required_checks_present"
    ],
    "brainkill": [
      "process_exit",
      "liveness",
      "config_agreement",
      "content_identity_agreement",
      "mode_brainkillcommandhost",
      "controller_boundary",
      "controller_boundary",
      "prediction_executed",
      "simgated",
      "provenance_stable",
      "binary_matches_source",
      "required_checks_present"
    ],
    "stall": [
      "process_exit",
      "liveness",
      "config_agreement",
      "content_identity_agreement",
      "mode_stalltest",
      "controller_boundary",
      "controller_boundary",
      "prediction_executed",
      "simgated",
      "provenance_stable",
      "binary_matches_source",
      "required_checks_present"
    ],
    "mismatch": [
      "mismatch_smoke",
      "provenance_stable",
      "binary_matches_source",
      "required_checks_present"
    ],
    "perturb": [
      "process_exit",
      "liveness",
      "config_agreement",
      "content_identity_agreement",
      "mode_perturbhost",
      "controller_boundary",
      "controller_boundary",
      "prediction_executed",
      "positive_control",
      "provenance_stable",
      "binary_matches_source",
      "required_checks_present"
    ],
    "rematch": [
      "rematch",
      "provenance_stable",
      "binary_matches_source",
      "required_checks_present"
    ],
    "inventory": [
      "process_exit",
      "liveness",
      "config_agreement",
      "content_identity_agreement",
      "mode_inventorycommandhost",
      "controller_boundary",
      "controller_boundary",
      "prediction_executed",
      "simgated",
      "provenance_stable",
      "binary_matches_source",
      "required_checks_present"
    ],
    "buy": [
      "process_exit",
      "liveness",
      "config_agreement",
      "content_identity_agreement",
      "mode_buycommandhost",
      "controller_boundary",
      "controller_boundary",
      "prediction_executed",
      "simgated",
      "provenance_stable",
      "binary_matches_source",
      "required_checks_present"
    ],
    "brainspawn": [
      "process_exit",
      "liveness",
      "config_agreement",
      "content_identity_agreement",
      "mode_brainspawntest",
      "controller_boundary",
      "controller_boundary",
      "prediction_executed",
      "simgated",
      "provenance_stable",
      "binary_matches_source",
      "required_checks_present"
    ],
    "pause": [
      "process_exit",
      "liveness",
      "config_agreement",
      "content_identity_agreement",
      "mode_pausetest",
      "controller_boundary",
      "controller_boundary",
      "prediction_executed",
      "simgated",
      "provenance_stable",
      "binary_matches_source",
      "required_checks_present"
    ],
    "fullloop": [
      "process_exit",
      "liveness",
      "config_agreement",
      "content_identity_agreement",
      "mode_fulllooptest",
      "controller_boundary",
      "controller_boundary",
      "prediction_executed",
      "simgated",
      "provenance_stable",
      "binary_matches_source",
      "required_checks_present"
    ]
  },
  "semantic": {
    "fire_reload": {
      "checks": [
        "binary_matches_source",
        "fire_reload.e2e",
        "fire_reload.semantic"
      ],
      "lines": [
        "fire: 1 round(s) over the hold, first at tick 204 (pressed 200, lands 204)",
        "reload: started at 304, full magazine (15 rounds) at tick 443",
        "PASS fire_reload: seated actor 1048615 (Green Dummy) from tick 35, peers identical over 27..520"
      ],
      "nested_checks": [
        "process_exit",
        "liveness",
        "config_agreement",
        "content_identity_agreement",
        "prediction_executed",
        "prediction_isolation",
        "prediction_executed",
        "prediction_isolation",
        "controller_boundary",
        "controller_boundary",
        "simgated",
        "replay_recording_intact",
        "replay_playback",
        "replay_compare",
        "provenance_stable",
        "binary_matches_source",
        "required_checks_present"
      ]
    },
    "weapon_switch": {
      "checks": [
        "binary_matches_source",
        "weapon_switch.e2e",
        "weapon_switch.semantic"
      ],
      "lines": [
        "switch: 1048625 -> 1048628 at 104, -> 1048631 at 204 (carrying 2 spare)",
        "PASS weapon_switch: seated actor 1048615 (Green Dummy) from tick 35, peers identical over 27..210"
      ],
      "nested_checks": [
        "process_exit",
        "liveness",
        "config_agreement",
        "content_identity_agreement",
        "prediction_executed",
        "prediction_isolation",
        "prediction_executed",
        "prediction_isolation",
        "controller_boundary",
        "controller_boundary",
        "simgated",
        "replay_recording_intact",
        "replay_playback",
        "replay_compare",
        "provenance_stable",
        "binary_matches_source",
        "required_checks_present"
      ]
    },
    "jetpack": {
      "checks": [
        "binary_matches_source",
        "jetpack.e2e",
        "jetpack.semantic"
      ],
      "lines": [
        "jetpack: emitted 31 ticks from 104, fuel 1360.0->760.0, rose 30.6 px",
        "PASS jetpack: seated actor 1048615 (Green Dummy) from tick 35, peers identical over 27..145"
      ],
      "nested_checks": [
        "process_exit",
        "liveness",
        "config_agreement",
        "content_identity_agreement",
        "prediction_executed",
        "prediction_isolation",
        "prediction_executed",
        "prediction_isolation",
        "controller_boundary",
        "controller_boundary",
        "simgated",
        "replay_recording_intact",
        "replay_playback",
        "replay_compare",
        "provenance_stable",
        "binary_matches_source",
        "required_checks_present"
      ]
    },
    "terrain_fire": {
      "checks": [
        "binary_matches_source",
        "terrain_fire.e2e",
        "terrain_fire.semantic"
      ],
      "lines": [
        "terrain: first carve at tick 155 (shot lands 154, aim -1.57 rad)",
        "PASS terrain_fire: seated actor 1048615 (Green Dummy) from tick 35, peers identical over 27..225"
      ],
      "nested_checks": [
        "process_exit",
        "liveness",
        "config_agreement",
        "content_identity_agreement",
        "prediction_executed",
        "prediction_isolation",
        "prediction_executed",
        "prediction_isolation",
        "controller_boundary",
        "controller_boundary",
        "simgated",
        "replay_recording_intact",
        "replay_playback",
        "replay_compare",
        "provenance_stable",
        "binary_matches_source",
        "required_checks_present"
      ]
    },
    "pie_reload": {
      "checks": [
        "binary_matches_source",
        "pie_reload.e2e",
        "pie_reload.semantic"
      ],
      "lines": [
        "pie: opened 153 (Enabled 157), hovered Pick Up at 163, released 194, reload started at 195 from 14 rounds",
        "PASS pie_reload: seated actor 1048615 (Green Dummy) from tick 35, peers identical over 27..240"
      ],
      "nested_checks": [
        "process_exit",
        "liveness",
        "config_agreement",
        "content_identity_agreement",
        "prediction_executed",
        "prediction_isolation",
        "prediction_executed",
        "prediction_isolation",
        "controller_boundary",
        "controller_boundary",
        "simgated",
        "replay_recording_intact",
        "replay_playback",
        "replay_compare",
        "provenance_stable",
        "binary_matches_source",
        "required_checks_present"
      ]
    },
    "ai_orders": {
      "checks": [
        "binary_matches_source",
        "ai_orders.e2e",
        "ai_orders.semantic"
      ],
      "lines": [
        "ai_orders: go-to 1048615 at 53, follow at 203, squad under brain 1048577 at 403, disbanded at 603",
        "PASS ai_orders: peers identical over 27..640"
      ],
      "nested_checks": [
        "process_exit",
        "liveness",
        "config_agreement",
        "content_identity_agreement",
        "mode_aiordercommandhost",
        "prediction_executed",
        "prediction_isolation",
        "prediction_executed",
        "prediction_isolation",
        "controller_boundary",
        "controller_boundary",
        "simgated",
        "replay_recording_intact",
        "replay_playback",
        "replay_compare",
        "provenance_stable",
        "binary_matches_source",
        "required_checks_present"
      ]
    },
    "death_brainkill": {
      "checks": [
        "binary_matches_source",
        "death_brainkill.e2e"
      ],
      "lines": null,
      "nested_checks": [
        "process_exit",
        "liveness",
        "config_agreement",
        "content_identity_agreement",
        "mode_brainkillcommandhost",
        "prediction_executed",
        "prediction_isolation",
        "prediction_executed",
        "prediction_isolation",
        "controller_boundary",
        "controller_boundary",
        "simgated",
        "replay_recording_intact",
        "replay_playback",
        "replay_compare",
        "provenance_stable",
        "binary_matches_source",
        "required_checks_present"
      ]
    },
    "cargo_deliver": {
      "checks": [
        "binary_matches_source",
        "cargo_deliver.e2e"
      ],
      "lines": null,
      "nested_checks": [
        "process_exit",
        "liveness",
        "config_agreement",
        "content_identity_agreement",
        "mode_delivercommandhost",
        "prediction_executed",
        "prediction_isolation",
        "prediction_executed",
        "prediction_isolation",
        "controller_boundary",
        "controller_boundary",
        "simgated",
        "replay_recording_intact",
        "replay_playback",
        "replay_compare",
        "provenance_stable",
        "binary_matches_source",
        "required_checks_present"
      ]
    },
    "buy_order": {
      "checks": [
        "binary_matches_source",
        "buy_order.e2e"
      ],
      "lines": null,
      "nested_checks": [
        "process_exit",
        "liveness",
        "config_agreement",
        "content_identity_agreement",
        "mode_buycommandhost",
        "prediction_executed",
        "prediction_isolation",
        "prediction_executed",
        "prediction_isolation",
        "controller_boundary",
        "controller_boundary",
        "simgated",
        "replay_recording_intact",
        "replay_playback",
        "replay_compare",
        "provenance_stable",
        "binary_matches_source",
        "required_checks_present"
      ]
    },
    "pause": {
      "checks": [
        "binary_matches_source",
        "pause.e2e"
      ],
      "lines": null,
      "nested_checks": [
        "process_exit",
        "liveness",
        "config_agreement",
        "content_identity_agreement",
        "mode_pausetest",
        "prediction_executed",
        "prediction_isolation",
        "prediction_executed",
        "prediction_isolation",
        "controller_boundary",
        "controller_boundary",
        "simgated",
        "replay_recording_intact",
        "replay_playback",
        "replay_compare",
        "provenance_stable",
        "binary_matches_source",
        "required_checks_present"
      ]
    }
  },
  "fakelag": [
    "host_process_exit",
    "host_report_present",
    "client_process_exit",
    "client_report_present",
    "host_liveness",
    "host_lockstep_completed",
    "host_no_editor",
    "host_pace_recorded",
    "client_liveness",
    "client_lockstep_completed",
    "client_no_editor",
    "client_pace_recorded",
    "config_agreement",
    "census_agreement",
    "scenario_identity_recorded",
    "prediction_counters_recorded",
    "prediction_executed",
    "simgated_strict",
    "replay_recording_closed",
    "replay_verify",
    "replay_playback_completed",
    "replay_playback_matches_host"
  ],
  "heal": [
    "host_process_exit",
    "host_report_present",
    "client_process_exit",
    "client_report_present",
    "host_liveness",
    "host_lockstep_completed",
    "host_no_editor",
    "host_pace_recorded",
    "host_resynced",
    "client_liveness",
    "client_lockstep_completed",
    "client_no_editor",
    "client_pace_recorded",
    "client_resynced",
    "config_agreement",
    "census_agreement",
    "scenario_identity_recorded",
    "prediction_counters_recorded",
    "prediction_executed",
    "host_script_continued",
    "host_no_script_errors",
    "client_script_continued",
    "client_no_script_errors"
  ],
  "invariance": {
    "reference": [
      "process",
      "files",
      "trace",
      "script_continued",
      "no_script_errors"
    ],
    "clean": [
      "process",
      "files",
      "trace",
      "script_continued",
      "no_script_errors",
      "reference_trace",
      "reference_dump",
      "same_binary",
      "invariance"
    ],
    "lua_fault": [
      "process",
      "files",
      "trace",
      "script_continued",
      "no_script_errors",
      "reference_trace",
      "reference_dump",
      "same_binary",
      "lua_fault_detected",
      "full_lua_evidence"
    ]
  },
  "sound": {
    "ai_defer": {
      "fixture_sha256": "ff90f7595309baa6e181290c17d1706fb265c3754ff1f02ae7f188644a5d3835",
      "changed_ticks": 0,
      "effect_counts": {
        "host": 2404,
        "client": 2404
      }
    },
    "ai_defer_alias": {
      "fixture_sha256": "436e5a61a98d9d7647faf1e3e4986480d57b1cfa16c6577edd5efab8ee3674d6",
      "changed_ticks": 0,
      "effect_counts": {
        "host": 4808,
        "client": 4808
      }
    },
    "auth-fault": {
      "fixture_sha256": "ee023f74bc67d9c1e1b9e61cac6f2766b26fb2653860d6a2415aeaa9459aa88b",
      "changed_ticks": 72
    },
    "auth-match": {
      "fixture_sha256": "ee023f74bc67d9c1e1b9e61cac6f2766b26fb2653860d6a2415aeaa9459aa88b",
      "changed_ticks": 0
    },
    "per_machine": {
      "fixture_sha256": "2f698888a5c3661085050b85d4cb3e313638dfbc67adfc27dc1aa373f7d1754b",
      "changed_ticks": 141,
      "effect_counts": {
        "host": 4808,
        "client": 4808
      }
    },
    "rng_particles": {
      "fixture_sha256": "0e4d3d6ec4e36881b4b16f5d609973c6c27e461f7ee6365a29e18c00dad4dda4",
      "changed_ticks": 0,
      "effect_counts": {
        "host": 1566,
        "client": 1566
      }
    },
    "ui_bus": {
      "fixture_sha256": "7b540086cf5330604eef6e7c7903e3aec06672b4f3cd0bf9c9f99a089b4f0351",
      "changed_ticks": 0,
      "effect_counts": {
        "host": 192,
        "client": 192
      }
    },
    "wounds": {
      "fixture_sha256": "152b018f1e5c6e45aa2cd6b77feb9a0340682cc47c746cb8a1f9adbcc5eccd66",
      "changed_ticks": 0,
      "effect_counts": {
        "host": 4,
        "client": 4
      }
    }
  },
  "compat": {
    "deferral": {
      "fixture_sha256": "ec135e312b10348e3bd8ca3e6382909acaeb2909863c076f00dcd505f495bc05",
      "cases": {
        "shared_pos_component": 1,
        "shared_pos_identity": 1,
        "shared_vector_ops": 1,
        "shared_native_argument": 1,
        "shared_native_effect": 1,
        "shared_immobile_guard": 1,
        "shared_lifetime_setup": 1,
        "shared_lifetime_read": 1,
        "shared_play": 1,
        "ai_sees_play": 1,
        "ai_replay_guard": 1,
        "ai_restart_takes": 1,
        "ai_play_lands": 1,
        "ai_props_readback": 1,
        "ai_props_visible": 1,
        "ai_pos_component": 1,
        "ai_pos_alias": 1,
        "ai_read_tracks_shared": 1,
        "shared_pos_retained_alias": 1,
        "ai_alias_only": 1,
        "shared_props": 1,
        "ai_alias_only_retained": 1,
        "ai_soundset_selection": 1,
        "spawn_child": 1,
        "ai_pos_retained_alias": 1,
        "ai_stop_takes": 1,
        "spawn_first_ai": 1,
        "done": 1
      },
      "lines": [
        "PRINT: [deferral] claim actor=1048607 preset=Green Dummy",
        "PRINT: [deferral] case=shared_pos_component ok=1 x=33 y=77 alias_y=77",
        "PRINT: [deferral] case=shared_pos_identity ok=1 b_x=33.5 read_x=33.5",
        "PRINT: [deferral] case=shared_vector_ops ok=1 magnitude=134.72309875488281 sum_x=121.5 sum_y=62.25 copy_ok=0 copy_x=none",
        "PRINT: [deferral] case=shared_native_argument ok=1 grounded_x=120.5 matter=0 played=1 pos_x=120.5 pos_y=60.25",
        "PRINT: [deferral] case=shared_native_effect ok=1 particle_x=140.5 particle_vel_x=1.4049999713897705",
        "PRINT: [deferral] case=shared_immobile_guard ok=1 x=7.5 y=8.5",
        "PRINT: [deferral] case=shared_lifetime_setup ok=1 held=1",
        "PRINT: [deferral] case=shared_lifetime_read ok=1 read_ok=1 x=0",
        "PRINT: [deferral] case=shared_play ok=1 live=1 hold=1 restart=1 live_playing=1",
        "PRINT: [deferral] case=ai_sees_play ok=1 ai_playing=1 upd_playing=1 ai_audible=0 upd_audible=0",
        "PRINT: [deferral] case=ai_replay_guard ok=1 ai_hold_replay=0 upd_hold_replay=0",
        "PRINT: [deferral] case=ai_restart_takes ok=1 ai_restart=1 upd_playing=1",
        "PRINT: [deferral] case=ai_play_lands ok=1 ai_play=1 ai_playing_after=1 upd_playing=1",
        "PRINT: [deferral] case=ai_props_readback ok=1 read=0.25/0.5/0.125/7/42/0/1/1/123.5/0.5/0.75/1/1/13.5,-24.25",
        "PRINT: [deferral] case=ai_props_visible ok=1 read=0.25/0.5/0.125/7/42/0/1/1/123.5/0.5/0.75/1/1/13.5,-24.25",
        "PRINT: [deferral] case=ai_pos_component ok=1 ai_x=66 ai_y=55 upd_x=66 upd_y=55",
        "PRINT: [deferral] case=ai_pos_alias ok=1 ai_reread=111 ai_alias=111 upd_x=111",
        "PRINT: [deferral] case=ai_read_tracks_shared ok=1 ai_x=-400.5 upd_x=-400.5",
        "PRINT: [deferral] case=shared_pos_retained_alias ok=1 read_x=55.25 read_y=66.5",
        "PRINT: [deferral] case=ai_alias_only ok=1 ai_x=210.5 upd_x=210.5 upd_y=201",
        "PRINT: [deferral] case=shared_props ok=1 read=0.25/0.5/0.125/7/42/0/1/1/123.5/0.5/0.75/1/1/13.5,-24.25",
        "PRINT: [deferral] case=ai_alias_only_retained ok=1 upd_x=210.5 upd_y=220.5",
        "PRINT: [deferral] case=ai_soundset_selection ok=1 ai_live=0 ctrl_live=0 ai_played=1 ctrl_played=1",
        "PRINT: [deferral] case=spawn_child ok=1 uid=1049517",
        "PRINT: [deferral] case=ai_pos_retained_alias ok=1 ai_y=-77.25 upd_y=-77.25",
        "PRINT: [deferral] case=ai_stop_takes ok=1 ai_stop=1 ai_playing_after=0 upd_playing=0",
        "PRINT: [deferral] case=spawn_first_ai ok=1 created=1 ai_before_first_update=1 ai_runs=20 updates=40",
        "PRINT: [deferral] case=done ok=1 step=150 ai_runs=75"
      ],
      "exit_code": 1,
      "ticks": 280
    },
    "alias": {
      "fixture_sha256": "901161776d1d2ee1bcc72406a88e778e3175b202fe981af656334d3842928e8a",
      "cases": {
        "setup_shared_alias": "x=10",
        "alias_taken_shared_written_by_ai": "ai_read=44.5 upd_x=44.5 upd_y=45.5",
        "setup_ai_alias_write": "wrote=77.5 read_now=77.5",
        "alias_taken_ai_written_by_shared": "upd_x=77.5 upd_y=40 ai_x=77.5",
        "done": "step=120 ai_runs=60"
      },
      "lines": [
        "PRINT: [alias] case=setup_shared_alias x=10",
        "PRINT: [alias] case=alias_taken_shared_written_by_ai ai_read=44.5 upd_x=44.5 upd_y=45.5",
        "PRINT: [alias] case=setup_ai_alias_write wrote=77.5 read_now=77.5",
        "PRINT: [alias] case=alias_taken_ai_written_by_shared upd_x=77.5 upd_y=40 ai_x=77.5",
        "PRINT: [alias] case=done step=120 ai_runs=60"
      ],
      "exit_code": 1,
      "ticks": 260
    },
    "alias2": {
      "fixture_sha256": "3cf2e97ee2ee177ad95991b3fb0fdbffe222d43cd590ae42302667445aec386f",
      "cases": {
        "orphan_alias_write_now": "read_now=88.5 alias_x=88.5",
        "orphan_alias_write_settled": "upd_x=88.5 upd_y=89.5 alias_x=88.5 ai_touches=1",
        "done": "step=120 ai_runs=60"
      },
      "lines": [
        "PRINT: [alias] case=orphan_alias_write_now read_now=88.5 alias_x=88.5",
        "PRINT: [alias] case=orphan_alias_write_settled upd_x=88.5 upd_y=89.5 alias_x=88.5 ai_touches=1",
        "PRINT: [alias] case=done step=120 ai_runs=60"
      ],
      "exit_code": 1,
      "ticks": 260
    },
    "extra": {
      "fixture_sha256": "e92fa5fb5095b4cec374a6f7ef69c3559edb7028e6ed2fbc00a2fe214eccfaaf",
      "cases": {
        "shared_pos_setxy": "call_ok=0 x=1 y=2",
        "shared_pos_methods": "setxy=0 flipx=0 setmag=0 x=3 y=4 mag=5",
        "shared_pos_absradangle": "call_ok=1 x=10 y=0",
        "shared_soundset_structural": "before=1 removed=0 any=1",
        "shared_soundset_cyclemode": "mode=2",
        "shared_stopall_setup": "played=1 playing=1",
        "ai_pos_setxy": "ai_ok=0 ai_x=5 ai_y=6 upd_x=5 upd_y=6",
        "ai_soundset_addsound": "ai_ok=1 ai_any=1 upd_any=1",
        "ai_soundset_cyclemode": "ai_mode=1 upd_mode=1",
        "ai_set_top_level_sound_set": "ai_ok=1 ai_any=1 upd_any=1",
        "ai_property_next_pass": "ai_same_pass=0.375 ai_next_pass=0.375 upd=0.375",
        "ai_pos_retained_alias_method": "ai_x=7 upd_x=7 upd_y=8",
        "ai_audioman_stopall": "ai_playing_after=0 upd_playing=0",
        "two_actor_shared_container": "volume=0.125 writers=2 primary=40 secondary=40",
        "done": "step=140 ai_runs=70"
      },
      "lines": [
        "PRINT: [extra] claim actor=1048607",
        "PRINT: [extra] case=shared_pos_setxy call_ok=0 x=1 y=2",
        "PRINT: [extra] case=shared_pos_methods setxy=0 flipx=0 setmag=0 x=3 y=4 mag=5",
        "PRINT: [extra] case=shared_pos_absradangle call_ok=1 x=10 y=0",
        "PRINT: [extra] case=shared_soundset_structural before=1 removed=0 any=1",
        "PRINT: [extra] case=shared_soundset_cyclemode mode=2",
        "PRINT: [extra] case=shared_stopall_setup played=1 playing=1",
        "PRINT: [extra] case=ai_pos_setxy ai_ok=0 ai_x=5 ai_y=6 upd_x=5 upd_y=6",
        "PRINT: [extra] case=ai_soundset_addsound ai_ok=1 ai_any=1 upd_any=1",
        "PRINT: [extra] case=ai_soundset_cyclemode ai_mode=1 upd_mode=1",
        "PRINT: [extra] case=ai_set_top_level_sound_set ai_ok=1 ai_any=1 upd_any=1",
        "PRINT: [extra] case=ai_property_next_pass ai_same_pass=0.375 ai_next_pass=0.375 upd=0.375",
        "PRINT: [extra] case=ai_pos_retained_alias_method ai_x=7 upd_x=7 upd_y=8",
        "PRINT: [extra] case=ai_audioman_stopall ai_playing_after=0 upd_playing=0",
        "PRINT: [extra] case=two_actor_shared_container volume=0.125 writers=2 primary=40 secondary=40",
        "PRINT: [extra] case=done step=140 ai_runs=70",
        "PRINT: [extra] claim actor=1050807",
        "PRINT: [extra] claim actor=1050793",
        "PRINT: [extra] claim actor=1050821"
      ],
      "exit_code": 1,
      "ticks": 280
    }
  },
  "reports": {
    "h4": {
      "clean_leave": [
        "ambiguous_client2_report.json",
        "ambiguous_host_report.json",
        "clean_client_report.json",
        "clean_host2_report.json",
        "clean_host_report.json",
        "clean_stale_report.json"
      ],
      "crash_relaunch_provisional": [
        "arm_a_client2_report.json",
        "arm_a_host_report.json",
        "arm_b_client2_report.json",
        "arm_b_host_report.json"
      ],
      "fencing_two_transports": [
        "client1_report.json",
        "client2_report.json",
        "host_report.json"
      ],
      "gns_provider_smoke": [
        "client_report.json",
        "host_report.json"
      ],
      "old_wire_fixture": [
        "client_report.json",
        "host_report.json"
      ],
      "peers_3_4_regression": [
        "drop3_client2_report.json",
        "drop3_host_report.json",
        "drop3_returner_report.json",
        "peers3_client1_report.json",
        "peers3_client2_report.json",
        "peers3_host_report.json",
        "peers4_client1_report.json",
        "peers4_client2_report.json",
        "peers4_client3_report.json",
        "peers4_host_report.json"
      ],
      "reclaim_socket": [
        "client2_report.json",
        "host_report.json"
      ],
      "rejoin_after_resync": [
        "rematch_client_report.json",
        "rematch_host_report.json",
        "resync_client2_report.json",
        "resync_host_report.json"
      ]
    },
    "b1": {
      "substitute_bounds": [
        "applicant1_report.json",
        "applicant2_report.json",
        "applicant3_report.json",
        "host_report.json",
        "stayer_report.json"
      ],
      "substitute_commit": [
        "host_report.json",
        "stayer_report.json",
        "substitute_report.json"
      ],
      "substitute_host_cancel": [
        "host_report.json",
        "stayer_report.json",
        "substitute_report.json"
      ],
      "substitute_returner_wins": [
        "host_report.json",
        "returner_report.json",
        "stayer_report.json",
        "substitute_report.json"
      ]
    }
  },
  "launches": {
    "h4": {
      "clean_leave": [
        "ambiguous_client1",
        "ambiguous_client2",
        "ambiguous_host",
        "clean_client",
        "clean_host",
        "clean_host2",
        "clean_stale"
      ],
      "crash_relaunch_provisional": [
        "arm_a_client1",
        "arm_a_client2",
        "arm_a_host",
        "arm_b_client1",
        "arm_b_client2",
        "arm_b_host"
      ],
      "fencing_two_transports": [
        "client1",
        "client2",
        "host"
      ],
      "gns_provider_smoke": [
        "client",
        "host"
      ],
      "old_wire_fixture": [
        "client",
        "host",
        "old_wire_selftest"
      ],
      "peers_3_4_regression": [
        "drop3_client1",
        "drop3_client2",
        "drop3_host",
        "drop3_returner",
        "peers3_client1",
        "peers3_client2",
        "peers3_host",
        "peers4_client1",
        "peers4_client2",
        "peers4_client3",
        "peers4_host"
      ],
      "reclaim_socket": [
        "client1",
        "host",
        "returner"
      ],
      "rejoin_after_resync": [
        "rematch_client",
        "rematch_host",
        "resync_client1",
        "resync_client2",
        "resync_host"
      ]
    },
    "b1": {
      "substitute_bounds": [
        "applicant1",
        "applicant2",
        "applicant3",
        "host",
        "leaver",
        "stayer"
      ],
      "substitute_commit": [
        "host",
        "leaver",
        "stayer",
        "substitute"
      ],
      "substitute_host_cancel": [
        "host",
        "leaver",
        "stayer",
        "substitute"
      ],
      "substitute_returner_wins": [
        "host",
        "leaver",
        "returner",
        "stayer",
        "substitute"
      ]
    }
  }
}
''')

ERRORS = re.compile(
    r"invalid AudioMan checkpoint|checkpoint[^\r\n]*(?:refus(?:ed|al)|fail(?:ed|ure))"
    r"|cannot be carried|no persistent owner|has no registered owner"
    r"|could not apply MovableMan checkpoint|unresolved native references"
    r"|bad[ _]alloc(?:ation)?|resync snapshot save failed|timed out waiting for the resync round"
    r"|refus(?:ed|al)[^\r\n]*checkpoint|SendMessageToConnection failed|RTE Abort|RTE Assert"
    r"|EXCEPTION_|ERROR:|stack traceback|Runtime Error|scriptgraph[^\r\n]*failed",
    re.IGNORECASE,
)
SPAWN_CHILD_LINE = re.compile(r"^PRINT: \[deferral\] case=spawn_child ok=1 uid=\d+$")
AUTO_DELAY_LINE = re.compile(r"\[net-match\] auto input delay: peer 2 rtt (\d+)ms -> (\d+) frames")
COMPAT_LINES_ORACLE = (
    "compat output differs from Source22 (spawn_child uid value excluded, run-to-run counter)"
)
COUNTERS = ("clock_divergence_max_ms", "timeout_resumptions", "census_refusals",
            "host_reseats_issued", "host_reseats_without_a_ledger", "host_reseats_without_survivors",
            "host_reseat_live_on_team_not_named", "host_ledger_drops_recorded", "host_seats_dropped",
            "round_readoptions", "start_answers_suppressed", "start_packets_sent", "start_retransmits",
            "peer_leave_frames", "stops_from_left_peers", "unresolved_observation_packets",
            "relay_send_failures", "relay_resends", "relay_congestion_holds",
            "relay_longest_congestion_hold_ms", "relay_backlog_overflows", "timeout_reason")


def select_jobs(jobs: list[Job], cases: list[str] | None) -> list[Job]:
    if not cases:
        return list(jobs)
    wanted = set(cases)
    unknown = sorted(wanted - set(REQUIRED_CASES))
    if unknown:
        raise ValueError("unknown --cases id(s): " + ", ".join(unknown))
    selected = [job for job in jobs if job.id in wanted]
    if {job.id for job in selected} != wanted:
        raise ValueError("planned inventory is missing a requested case")
    return selected


def compat_lines_match(actual, expected) -> bool:
    if not isinstance(actual, list) or not isinstance(expected, list) or len(actual) != len(expected):
        return False
    for got, want in zip(actual, expected):
        if SPAWN_CHILD_LINE.match(want):
            if not SPAWN_CHILD_LINE.match(got):
                return False
        elif got != want:
            return False
    return True


def expected_auto_delay_frames(rtt_ms: int) -> int:
    # NetMatchRunner.cpp:80-86 neededDelay = ceil(rttMs / (1000/30)) + 1
    return int(math.ceil(rtt_ms / (1000.0 / 30.0))) + 1


def fake_lag_auto_delay_failures(case: str, host_log: str, client_log: str,
                                 host_delays, client_delays) -> list[str]:
    lag = 100 if case == "fl100" else 200
    lo, hi = 2 * lag, 2 * lag + 15
    host_m = AUTO_DELAY_LINE.search(host_log or "")
    client_m = AUTO_DELAY_LINE.search(client_log or "")
    if host_m is None:
        return ["host: auto input delay line missing"]
    rtt, frames = int(host_m.group(1)), int(host_m.group(2))
    failures = []
    if client_m is not None:
        client_rtt, client_frames = int(client_m.group(1)), int(client_m.group(2))
        if (client_rtt, client_frames) != (rtt, frames):
            failures.append(
                f"auto delay peer disagreement: host rtt {rtt}ms -> {frames} "
                f"client rtt {client_rtt}ms -> {client_frames}"
            )
    wanted = expected_auto_delay_frames(rtt)
    if frames != wanted:
        failures.append(
            f"auto delay formula violated: rtt {rtt}ms -> {frames} frames, expected {wanted}"
        )
    if not (lo <= rtt <= hi):
        failures.append(f"auto delay rtt band violated: rtt {rtt}ms not in [{lo}, {hi}]")
    expected_map = {"1": 1, "2": frames}
    if host_delays != expected_map:
        failures.append(f"host: auto delay peer disagreement: {host_delays} != {expected_map}")
    if client_delays != expected_map:
        failures.append(f"client: auto delay peer disagreement: {client_delays} != {expected_map}")
    return failures


def digest(path: Path) -> str:
    with path.open("rb") as stream:
        return hashlib.file_digest(stream, "sha256").hexdigest()


def read_json(path: Path):
    return json.loads(path.read_text(encoding="utf-8-sig"))


def write_json(path: Path, data) -> None:
    path.write_text(json.dumps(data, indent=2, ensure_ascii=False) + "\n", encoding="utf-8")


def is_reparse(path: Path) -> bool:
    return bool(path.lstat().st_file_attributes & stat.FILE_ATTRIBUTE_REPARSE_POINT) if os.name == "nt" else path.is_symlink()


def files_under(root: Path):
    """Never traverse a runtime Data junction or another reparse point."""
    if not root.exists():
        return
    if is_reparse(root):
        raise ValueError(f"evidence root is a reparse point: {root}")
    for folder, dirs, files in os.walk(root, followlinks=False):
        dirs[:] = sorted(d for d in dirs if not is_reparse(Path(folder) / d))
        for name in sorted(files):
            path = Path(folder) / name
            if not is_reparse(path):
                yield path


def unique_json(root: Path, filename: str) -> tuple[Path, dict]:
    paths = [p for p in files_under(root) if p.name == filename]
    if len(paths) != 1:
        raise ValueError(f"expected one {filename} under {root}; found {len(paths)}")
    return paths[0], read_json(paths[0])


def values_named(value, name: str, prefix=""):
    if isinstance(value, dict):
        for key, item in value.items():
            child = f"{prefix}.{key}" if prefix else key
            if key == name:
                yield child, item
            yield from values_named(item, name, child)
    elif isinstance(value, list):
        for index, item in enumerate(value):
            yield from values_named(item, name, f"{prefix}[{index}]")


class PinError(RuntimeError):
    pass


# These are upstream Dear ImGui sample files, not runtime fonts. The compiled
# default is the byte array in the pinned imgui_draw.cpp. The source exporter
# deliberately excludes TTFs. Still require these six samples to equal HEAD;
# no new or modified binary file under Source is silently exempted.
SOURCE_FONT_SAMPLES = frozenset(
    'Source/GUI/imgui/misc/fonts/' + name for name in (
        'Cousine-Regular.ttf', 'DroidSans.ttf', 'Karla-Regular.ttf',
        'ProggyClean.ttf', 'ProggyTiny.ttf', 'Roboto-Medium.ttf'))


class Pin:
    def __init__(self, source: int, manifest: Path, repo: Path = REPO):
        self.repo, self.path, self.source = repo.resolve(), manifest.resolve(), source
        self.manifest_sha = digest(self.path)
        self.build = read_json(self.path)
        self.driver_inputs = {}
        self.sample_inputs = {}
        self.sha = self.build["exe_sha256"]
        self.head = self.build["head"]
        if not re.fullmatch(r"[0-9a-f]{64}", self.sha) or not re.fullmatch(r"[0-9a-f]{40}", self.head):
            raise PinError("invalid executable or source pin")
        self.artifacts = {Path(p): h for p, h in self.build.get("artifacts", {}).items()}
        self.inputs = self.build.get("inputs", {})
        self.source_manifest = None
        if not self.inputs:
            candidates = [p for p in self.artifacts if p.name == f"combined-source-{source}.manifest.json"]
            if len(candidates) != 1:
                raise PinError("need build.inputs or exactly one pinned combined-source-N.manifest.json")
            self.source_manifest = candidates[0]
            export = read_json(self.source_manifest)
            if export.get("head") != self.head:
                raise PinError("export HEAD differs from build HEAD")
            self.inputs = {p: v["sha256"] for p, v in export["files"].items()}
        if not self.inputs or not any(p.startswith("Source/") for p in self.inputs):
            raise PinError("source coverage is empty or lacks engine source")
        for name in self.inputs:
            target = (self.repo / name).resolve()
            if not target.is_relative_to(self.repo):
                raise PinError(f"input escapes approved tree: {name}")

    def verify(self, artifacts=False):
        if digest(self.path) != self.manifest_sha:
            raise PinError("build manifest changed")
        if digest(self.repo / "Cortex Command.exe") != self.sha:
            raise PinError("approved executable differs from build pin")
        head = subprocess.check_output(["git", "-C", str(self.repo), "rev-parse", "HEAD"], text=True).strip()
        if head != self.head:
            raise PinError(f"source HEAD changed: {head}")
        mismatches = [name for name, sha in self.inputs.items()
                      if not (self.repo / name).is_file() or digest(self.repo / name) != sha]
        if mismatches:
            raise PinError(f"source files differ from compiled export: {mismatches[:20]}")
        # New untracked or tracked engine inputs are also drift, even if old hashes still match.
        names = subprocess.check_output(["git", "-C", str(self.repo), "ls-files", "--cached", "--others", "--exclude-standard", "-z", "Source"], text=True).split("\0")
        missing = [name for name in names if name and name not in self.inputs]
        for name in list(missing):
            if name not in SOURCE_FONT_SAMPLES:
                continue
            if name not in self.sample_inputs:
                try:
                    committed = subprocess.check_output(['git', '-C', str(self.repo), 'show', f'{self.head}:{name}'])
                except subprocess.CalledProcessError as error:
                    raise PinError(f'unpinned font sample is not in the compiled HEAD: {name}') from error
                self.sample_inputs[name] = hashlib.sha256(committed).hexdigest()
            if not (self.repo / name).is_file() or digest(self.repo / name) != self.sample_inputs[name]:
                raise PinError(f'vendored font sample changed: {name}')
            missing.remove(name)
        if missing:
            raise PinError(f"engine inputs absent from build pin: {missing[:20]}")
        if artifacts:
            if not self.artifacts:
                raise PinError("build artifact pin is empty")
            for path, sha in self.artifacts.items():
                if not path.is_file() or digest(path) != sha:
                    raise PinError(f"build artifact differs: {path}")
        elif self.source_manifest and digest(self.source_manifest) != self.artifacts[self.source_manifest]:
            raise PinError("source manifest changed")
        for path, sha in self.driver_inputs.items():
            if not path.is_file() or digest(path) != sha:
                raise PinError(f"breadth driver or fixture changed: {path}")
        return {"head": head, "exe_sha256": self.sha, "source_files": len(self.inputs),
                "unchanged_vendor_font_samples": dict(self.sample_inputs), "verified_utc": now()}


@dataclass
class Job:
    id: str
    family: str
    out: str
    command: list[str]
    env: dict[str, str] = field(default_factory=dict)
    meta: dict = field(default_factory=dict)


def plan(source: int, manifest: Path, out: Path, sha: str) -> list[Job]:
    jobs = []
    py = [sys.executable, "-B"]
    ps = ["pwsh", "-NoProfile", "-NonInteractive", "-File"]

    def add(name, family, command, meta=None, env=None):
        target = out / f"j{len(jobs) + 1:02d}"
        cmd = [str(part).replace("{out}", str(target)) for part in command]
        job_env = {k: str(v).replace("{out}", str(target)) for k, v in (env or {}).items()}
        jobs.append(Job(name, family, str(target), cmd, job_env, meta or {}))

    for index, name in enumerate(SEMANTIC):
        add("semantic_" + name, "semantic", [*ps, LANES / "semantic-battery/driver/test_gameplay_fixtures.ps1", "-Repo", REPO, "-OutDir", "{out}", "-PortBase", 45700 + index * 10, "-Only", name], {"case": name})
    for index, (name, flag) in enumerate(INTERP.items()):
        add("interp_" + name, "interp", [*ps, STAGE / "run_interp_e2e.ps1", "-SkipBuild", "-ExactOutDir", "-OutDir", "{out}", "-Port", 46110 + index * 10, *([] if flag == "-" else ["-" + flag])], {"case": name})
    add("interp_rtdesync", "rtdesync", [*ps, DRIVERS / "run_interp_desync.ps1", "-Port", 46260], env={"CC_LANE_RUN_ROOT": "{out}"})
    for name in ("fl100", "fl200"):
        add(name, "fakelag", [*py, DRIVERS / "run_fakelag.py", name], {"case": name},
            {"CC_LANE_MANIFEST": str(manifest), "CC_LANE_RUN_ROOT": "{out}"})
    add("invariance_300", "invariance", [*py, SCRIPT, "--source", source, "--build-manifest", manifest, "--out", "{out}", "--child", "invariance"])
    add("discovery", "discovery", [*py, REPO / "tools/run_sim_test.py", "--repo", REPO, "--out", "{out}", "--timeout", 300, "--", "-net-discovery-selftest"])
    for name in H4:
        for repeat in range(1, 6 if name == "peers_3_4_regression" else 2):
            add(f"h4_{name}_{repeat}", "h4", [*py, DRIVERS / f"h4gates/{name}.py", *(["--inprocess"] if name == "old_wire_fixture" else [])],
                {"case": name, "repeat": repeat}, {"CC_H4_BUILD_MANIFEST": str(manifest), "CC_H4_RUN_ROOT": "{out}"})
    for name in B1:
        add("b1_" + name, "b1", [*py, REPO / "tools/h4_substitution_gates.py", name, "--out", "{out}", "--expect-sha256", sha, "--repo", REPO], {"case": name})
    for name, players, action, lag, port, frame in LOBBY:
        for repeat in range(1, 4):
            add(f"lobby_{name}_{repeat}", "lobby", [*py, REPO / "tools/test_lobby_lifecycle.py", "--repo", REPO, "--out", "{out}", "--players", players, "--action", action, "--fake-lag-ms", lag, "--port", port + repeat - 1, *(["--leave-at-frame", frame] if frame else [])], {"case": name, "players": players, "action": action, "frame": frame, "repeat": repeat})
    for name, (expected, changed) in SOUND.items():
        authority = name.startswith("auth-")
        driver = CONTRACT / ("run_sound_volume_authority.py" if authority else "run_sound_query_gate.py")
        add("sound_" + name, "sound", [*py, driver, *([] if authority else [name]), expected, "--out", "{out}", "--build-manifest", manifest, *(["--env", "CC_FAULT_INJECT=local_audibility"] if expected == "diverge" else [])], {"case": name, "expected": expected, "changed_ticks": changed,
            "chain": str(CONTRACT / f"chain-source{source}-1.json"), "source": source, "build_manifest": str(manifest)})
    add("heal", "heal", [*py, SCRIPT, "--source", source, "--build-manifest", manifest, "--out", "{out}", "--child", "heal"])
    for name, (driver, fixture) in COMPAT.items():
        for rung in ("source22", "approved"):
            add(f"compat_{name}_{rung}", "compat", [*py, DRIVERS / driver, rung, "--out", "{out}", *(["--fixture", fixture] if fixture else [])], {"case": name, "rung": rung})
    return jobs


class Verdict:
    def __init__(self):
        self.failures, self.evidence, self.counters = [], [], []

    def require(self, ok, message):
        if ok is not True:
            self.failures.append(message)

    def checks(self, data, required, flag="pass", allowed_na=()):
        self.require(data.get(flag) is True, f"{flag} is missing or false")
        checks = data.get("checks")
        if isinstance(checks, dict):
            rows = [{"name": k, "status": "pass" if v is True else "fail"} for k, v in checks.items()]
        elif isinstance(checks, list):
            rows = checks
        else:
            rows = []
        seen = Counter(c.get("name") for c in rows)
        for name, count in Counter(required).items():
            self.require(seen[name] >= count, f"missing required check {name} x{count}")
        self.require(bool(rows), "empty checks")
        for row in rows:
            ok = row.get("status") == "pass" or (row.get("name") in allowed_na and row.get("status") == "n/a")
            self.require(ok, f"check {row.get('name')}: {row.get('status')} {row.get('detail', '')}")

    def json(self, path: Path):
        self.evidence.append(str(path))
        return read_json(path)


def service(report):
    return report.get("service", report)


def lockstep(report):
    return service(report).get("runner", {}).get("lockstep", {})


def numeric(value):
    return isinstance(value, (int, float)) and not isinstance(value, bool)


def counter_is(value, expected):
    return numeric(value) and value == expected


def inspect_counters(v: Verdict, root: Path, require_clock=False):
    reports = []
    for path in files_under(root):
        if path.suffix != ".json" or not ("report" in path.name or path.name in ("lockstep-stats.json", "host.json", "client.json")):
            continue
        data = v.json(path)
        rows = {name: list(values_named(data, name)) for name in COUNTERS}
        rows = {name: found for name, found in rows.items() if found}
        if rows:
            v.counters.append({"path": str(path), "values": rows})
        rec = service(data).get("reconnect") if isinstance(data, dict) else None
        if isinstance(rec, dict):
            reports.append(path)
            if require_clock:
                v.require(counter_is(rec.get("clock_divergence_max_ms"), 0), f"{path.name}: clock divergence missing or nonzero")
        for name in ("clock_divergence_max_ms", "census_refusals"):
            for at, value in rows.get(name, []):
                v.require(counter_is(value, 0), f"{path.name}:{at}={value}")
        for at, value in rows.get("timeout_resumptions", []):
            v.require(numeric(value) and 0 <= value <= 1, f"{path.name}:{at}={value}, expected at most one per reported phase")
        for at, value in rows.get("round_readoptions", []):
            v.require(numeric(value) and value in (0, 1), f"{path.name}:{at}={value}")
        ls = lockstep(data) if isinstance(data, dict) else {}
        if ls.get("round_readoptions") == 1:
            v.require(ls.get("peer_leave_frames") == {}, f"{path.name}: returning peer retained leaves")
            v.require(numeric(ls.get("start_answers_suppressed")) and ls["start_answers_suppressed"] >= 1,
                      f"{path.name}: restarted played peer lacks a suppressed Start answer")
    if require_clock:
        v.require(bool(reports), "no reconnect reports for clock verification")


def scan_logs(v: Verdict, root: Path):
    count = 0
    for path in files_under(root):
        if path.name.lower() == "abortlog.txt":
            v.failures.append(f"abort artifact: {path}")
        if path.suffix.lower() not in (".log", ".txt"):
            continue
        if not (path.suffix.lower() == ".log" or any(x in path.name.lower() for x in ("stdout", "stderr", ".out.", ".err.", "console"))):
            continue
        count += 1
        scan_log(v, path)
    v.require(count > 0, "no runtime logs retained")


def scan_log(v: Verdict, path: Path):
    with path.open(encoding="utf-8-sig", errors="replace") as stream:
        for line_no, line in enumerate(stream, 1):
            if ERRORS.search(line):
                v.failures.append(f"{path}:{line_no}: {line.strip()}")


def inspect_launches(v: Verdict, root: Path, sha: str, reference=False):
    launches = [p for p in files_under(root) if p.name == "launch.json"]
    v.require(bool(launches), "no guarded launch records")
    for path in launches:
        data = v.json(path)
        expected = REFERENCE_SHA if reference else sha
        v.require(data.get("exe_sha256") == expected, f"{path}: wrong executable pin")
        argv = data.get("argv", [])
        executable = REFERENCE_EXE if reference else REPO / "Cortex Command.exe"
        v.require(bool(argv) and Path(argv[0]).resolve() == executable.resolve(), f"{path}: unapproved executable path")
        v.require(data.get("runner") == "win32_test_runner.py" and bool(data.get("private_desktop")), f"{path}: missing private guarded launch")
        v.require(data.get("input_desktop_before") is not None and data.get("input_desktop_before") == data.get("input_desktop_after"), f"{path}: desktop changed or missing")
        v.require(data.get("timed_out") is False, f"{path}: timeout or unfinished launch")
        v.require(data.get("started") is True and data.get("evidence_complete") is True, f"{path}: incomplete launch evidence")


def gate_reports(v: Verdict, root: Path, family: str, name: str):
    expected = BASELINE["reports"][family][name]
    for filename in expected:
        data = v.json(root / filename)
        v.require(counter_is(service(data).get("reconnect", {}).get("clock_divergence_max_ms"), 0),
                  f"{filename}: required clock sample is missing or nonzero")
    actual = {p.parent.name for p in files_under(root) if p.name == "launch.json"}
    v.require(actual == set(BASELINE["launches"][family][name]), f"{name}: guarded peer/arm coverage differs")


def validate_lobby(v: Verdict, data: dict, meta: dict):
    players, early = meta["players"], bool(meta["frame"])
    names = ["host", "departing", *[f"stayer{i}" for i in range(2, players)], *([] if early else ["replacement"])]
    survivors = [n for n in names if n != "departing"]
    required = [f"{peer}_{suffix}" for peer in names for suffix in ("process", "steps", "desktop", "binary")]
    required += [f"{peer}_simulation" for peer in survivors]
    required += ["only_the_leaver_left", "the_leaver_is_the_departing_peer", "no_missing_frame_timeout", "input_barrier_reached", "host_received_last_frame"] if early else ["host_survived_departure", *[f"{peer}_replacement_name" for peer in survivors]]
    v.checks(data, required)
    v.require(data.get("players") == players and data.get("action") == meta["action"], "lobby configuration differs")
    stats = data.get("lockstep", {})
    barrier = data.get("input_barrier", {})
    if not isinstance(barrier, dict):
        barrier = {}
    peer_id = barrier.get("peer_id")
    if early:
        v.require(type(peer_id) is int and 0 < peer_id <= players, "departing input barrier has no valid peer identity")
        v.require(barrier.get("target_frame") == 40 and type(barrier.get("produced_frame")) is int
                  and 0 < barrier["produced_frame"] < 40, "input barrier did not precede target frame 40")
        v.require(data.get("host_received_barrier") == f"[lockstep-test] received peer={peer_id} target=39",
                  "host did not witness the departing peer's last input")
    for peer in survivors:
        comparison = data.get("details", {}).get(peer, {}).get("comparison", {})
        v.require(comparison.get("compared_ticks") == 180 and comparison.get("first_divergence") is None, f"{peer}: missing full 180-tick comparison")
        ls = stats.get(peer, {})
        for name in ("relay_send_failures", "relay_resends", "unresolved_observation_packets", "relay_congestion_holds", "relay_longest_congestion_hold_ms", "relay_backlog_overflows"):
            v.require(counter_is(ls.get(name), 0), f"{peer}: {name} missing or nonzero")
        v.require("MissingFrameTimeout" not in str(ls.get("timeout_reason", "")), f"{peer}: MissingFrameTimeout")
        if early:
            leaves = ls.get("peer_leave_frames")
            v.require(leaves == {str(peer_id): 40}, f"{peer}: observed departing identity/frame differs: {leaves}")
            v.require(numeric(ls.get("stops_from_left_peers")) and ls["stops_from_left_peers"] >= 0, f"{peer}: stops_from_left_peers missing")
    if early:
        lines = data.get("leave_lines", [])
        v.require(len(lines) == 1 and list(lines[0]) == ["Departing", "40"], f"wrong departing identity/frame: {lines}")


def validate_sound(v: Verdict, data: dict, meta: dict, sha: str):
    name, expected = meta["case"], meta["expected"]
    v.require(data.get("pass_expected_outcome") is True, "sound outcome verdict missing or false")
    v.require(data.get("binary_sha256") == sha and data.get("expected") == expected and data.get("ticks") == 600, "sound identity/configuration differs")
    v.require(data.get("changed_ticks") == meta["changed_ticks"], "sound changed-tick count differs from Source40")
    v.require(data.get("trace_errors") == [], "sound trace errors missing or nonempty")
    v.require(data.get("native_or_lua_errors") == {"host": [], "client": []}, "sound native/Lua errors missing or nonempty")
    peers = data.get("peers", {})
    v.require(set(peers) == {"host", "client"}, "sound peer coverage")
    for peer, record in peers.items():
        v.require(record.get("exit_code") == 0 and record.get("timed_out") is False and record.get("evidence_complete") is True, f"{peer}: sound peer did not finish")
    if name.startswith("auth-"):
        common = "process desktop_and_binary nosound_verified complete_ticks same_query_keys enough_unique_queries nonzero_actual_volume ownership_transfers no_native_or_lua_errors".split()
        required = common + ("local_private_values observed_query_divergence observed_shared_divergence divergence_before_ownership_transfer".split() if expected == "diverge" else "shared_trace_matches all_queries_match authority_values_follow_team_owner".split())
        v.checks(data, required, flag="pass_expected_outcome")
    else:
        v.require(data.get("mode") == name and data.get("complete") is True and data.get("trace_counts") == [600, 600] and data.get("common_ticks") == 600, "sound mode or complete trace coverage missing")
        v.require(data.get("effect_counts") == BASELINE["sound"][name]["effect_counts"], "sound effect coverage differs from Source40")
    if name.startswith("auth-") or name in ("ui_bus", "per_machine"):
        v.require(data.get("guard_checks") == {"host": True, "client": True}, "NOSOUND guards missing")
    if expected == "diverge":
        v.require(data.get("environment", {}).get("CC_FAULT_INJECT") == "local_audibility", "negative control was not armed")


def validate_chain_sound(v: Verdict, data: dict, meta: dict, sha: str):
    chain = v.json(Path(meta["chain"]))
    v.require(chain.get("source") == meta["source"], "sound chain source differs")
    v.require(Path(chain.get("manifest", "")).resolve() == Path(meta["build_manifest"]).resolve(), "sound chain names a different build manifest")
    name, expected = meta["case"], meta["expected"]
    prefix = f"gate:authority:{expected}:" if name.startswith("auth-") else f"gate:query:{name}:{expected}:"
    rows = [row for row in chain.get("steps", []) if row.get("step", "").startswith(prefix)]
    v.require(len(rows) == 1, f"missing or duplicate named sound chain step: {prefix}")
    if rows:
        row = rows[0]
        v.require(row.get("exit") == 0, "corresponding chain sound driver failed")
        prior = v.json(Path(row["result"]))
        validate_sound(v, prior, meta, sha)
        for key in ("changed_ticks", "expected", "fixture_sha256", "effect_counts"):
            v.require(prior.get(key) == data.get(key), f"breadth sound {key} differs from the same-build chain")


def validate_job(job: Job, sha: str, code: int, log: Path) -> dict:
    v, root, family = Verdict(), Path(job.out), job.family
    v.require(code == 0, f"driver exit {code}")
    try:
        if family in ("h4", "b1"):
            path, data = unique_json(root, "verdict.json")
            v.evidence.append(str(path))
            name = job.meta["case"]
            v.require(data.get("gate") == name and data.get("executable_sha256") == sha, "gate or binary identity differs")
            v.checks(data, BASELINE[family][name], flag="passed")
            if name == "rejoin_after_resync":
                v.checks(data, ["resync_reseat_decided"], flag="passed")
            if name == "substitute_returner_wins":
                v.checks(data, ["returner_reseat_decided"], flag="passed")
            gate_reports(v, path.parent, family, name)
            if family == "h4" and name == "clean_leave":
                host_log = path.parent / "clean_host/stdout.log"
                text = host_log.read_text(encoding="utf-8-sig", errors="replace")
                v.require(bool(re.search(r"left the match at frame \d+ \(Match left\)", text)), "clean host did not record Match left")
                v.require(not re.search(r"left the match at frame \d+ \(connection lost\)", text), "clean leave was classified as connection loss")
                client = v.json(path.parent / "clean_client_report.json")
                host = v.json(path.parent / "clean_host_report.json")
                v.require(counter_is(service(client).get("reconnect", {}).get("client_leave_acks"), 1), "clean leave was not acked exactly once")
                v.require(counter_is(service(host).get("runner", {}).get("session", {}).get("admission", {}).get("seats_closed_by_leave"), 1),
                          "clean leave did not close exactly one seat")
            if family == "h4" and name == "crash_relaunch_provisional":
                detail = next((c.get("detail", "") for c in data["checks"] if c["name"] == "arm_b_provisional_expired"), "")
                v.require("26s" in detail and "20s" in detail, "provisional-expiry full margin is not evidenced")
            if name in ("reclaim_socket", "rejoin_after_resync", "substitute_commit", "substitute_returner_wins"):
                report = v.json(path.parent / ("resync_host_report.json" if name == "rejoin_after_resync" else "host_report.json"))
                rec = service(report).get("reconnect", {})
                for counter in ("host_seats_dropped", "host_reseats_issued", "host_reseats_without_a_ledger", "host_reseats_without_survivors", "host_reseat_live_on_team_not_named", "host_ledger_drops_recorded"):
                    v.require(type(rec.get(counter)) is int and rec[counter] >= 0, f"reseat world evidence missing: {counter}")
                v.require(counter_is(rec.get("host_reseats_without_a_ledger"), 0), "reseat has no ledger")
                for counter in ("host_seats_dropped", "host_ledger_drops_recorded"):
                    v.require(numeric(rec.get(counter)) and rec[counter] >= 1, f"reseat lacks a recorded drop: {counter}")
                v.require((numeric(rec.get("host_reseats_issued")) and rec["host_reseats_issued"] >= 1)
                          or (numeric(rec.get("host_reseats_without_survivors")) and rec["host_reseats_without_survivors"] >= 1),
                          "reseat did not record either an issued handoff or the no-survivors branch")
                if numeric(rec.get("host_reseats_issued")) and rec["host_reseats_issued"] >= 1:
                    log_path = path.parent / ("resync_host/stdout.log" if name == "rejoin_after_resync" else "host/stdout.log")
                    v.require("[net-reconnect] reseating team" in log_path.read_text(encoding="utf-8-sig", errors="replace"),
                              "issued reseat was not observed in the host log")
                if family == "b1":
                    v.require(report.get("running_ticks", 0) > 0, "B1 running activity is not evidenced")
                    v.require(numeric(report.get("resyncs")) and report["resyncs"] >= 1, "B1 did not complete a resync")
                    admission = service(report).get("runner", {}).get("session", {}).get("admission", {})
                    v.require(counter_is(admission.get("seats_dropped"), 1), "B1 did not adjudicate exactly one dropped seat")
                    counter = "substitutions_committed" if name == "substitute_commit" else "reclaims_accepted"
                    v.require(counter_is(admission.get(counter), 1), f"B1 {counter} is not exactly one")
        elif family == "semantic":
            name = job.meta["case"]
            data = v.json(root / "result.json")
            v.checks(data, BASELINE["semantic"][name]["checks"])
            nested = v.json(root / name / "result.json")
            v.checks(nested, BASELINE["semantic"][name]["nested_checks"])
            expected = BASELINE["semantic"][name]["lines"]
            if expected:
                lines = (root / f"{name}.check.txt").read_text(encoding="utf-8-sig").splitlines()
                v.require([line.rstrip() for line in lines if line.strip()] == expected, "semantic detail differs from Source40")
        elif family == "interp":
            name = job.meta["case"]
            data = v.json(root / "result.json")
            v.checks(data, BASELINE["interp"][name], allowed_na=("prediction_executed",))
            if name == "perturb":
                v.require("POSITIVE CONTROL PASS" in log.read_text(encoding="utf-8-sig", errors="replace"), "missing perturb positive-control sentinel")
        elif family == "rtdesync":
            text = log.read_text(encoding="utf-8-sig", errors="replace")
            for sentinel in ("clean_no_false_positive: True (ran_to_cap: True)", "perturb_desync_detected: True", "RUNTIME DESYNC DETECTION: PASS"):
                v.require(sentinel in text, "missing runtime-desync verdict: " + sentinel)
            for arm in ("clean", "perturb"):
                for peer in ("host", "client"):
                    data = v.json(root / f"mx/rtdesync_{arm}/{peer}.json")
                    reason = str(data.get("runtime_error", ""))
                    v.require(("Desync" in reason) == (arm == "perturb"), f"{arm}/{peer}: wrong runtime stop reason")
                    if arm == "clean":
                        v.require(data.get("running_ticks", 0) >= 580, f"{peer}: runtime clean control ended early")
        elif family == "fakelag":
            folder = root / "fl" / job.meta["case"]
            data = v.json(folder / "result.json")
            v.checks(data, BASELINE["fakelag"])
            v.require(data.get("exe_sha256") == sha and data.get("ticks") == 600, "fake-lag identity/coverage")
            for peer in ("host", "client"):
                report = v.json(folder / f"{peer}_report.json")
                ls = lockstep(report)
                v.require(counter_is(report.get("resyncs"), 0), f"{peer}: unexpected fake-lag resync")
                for counter in ("start_packets_sent", "start_retransmits"):
                    value = ls.get(counter)
                    v.require(numeric(value) and 0 <= value <= 9, f"{peer}: {counter} not single digits")
            host_log = (folder / "host" / "stdout.log").read_text(encoding="utf-8-sig", errors="replace")
            client_log = ""
            client_stdout = folder / "client" / "stdout.log"
            if client_stdout.is_file():
                client_log = client_stdout.read_text(encoding="utf-8-sig", errors="replace")
            host_ls = lockstep(v.json(folder / "host_report.json"))
            client_ls = lockstep(v.json(folder / "client_report.json"))
            for message in fake_lag_auto_delay_failures(
                    job.meta["case"], host_log, client_log,
                    host_ls.get("peer_input_delays"), client_ls.get("peer_input_delays")):
                v.failures.append(message)
        elif family == "invariance":
            data = v.json(root / "result.json")
            v.require(data.get("complete") is True and data.get("source_unchanged") is True and data.get("pass") is True, "invariance incomplete or failed")
            results = data.get("results", {})
            v.require(set(results) == {"reference", "clean", "lua_fault"}, "invariance arm coverage")
            for name, required in BASELINE["invariance"].items():
                arm = results.get(name, {})
                v.checks(arm, required)
                v.require(arm.get("binary") == sha, f"{name}: invariance binary differs")
        elif family == "discovery":
            launch = v.json(root / "launch.json")
            v.require(launch.get("exit_code") == 0, "discovery exit")
            v.require("[net-discovery-selftest] PASS" in launch.get("verdict_lines", []), "discovery selftest verdict missing")
        elif family == "lobby":
            validate_lobby(v, v.json(root / "result.json"), job.meta)
        elif family == "sound":
            data = v.json(root / "result.json")
            validate_sound(v, data, job.meta, sha)
            validate_chain_sound(v, data, job.meta, sha)
        elif family == "heal":
            data = v.json(root / "result.json")
            v.checks(data, BASELINE["heal"])
            v.require(data.get("complete") is True and data.get("source_unchanged") is True, "heal incomplete or changed source")
            for peer in ("host", "client"):
                report = v.json(root / f"fresh/e2e/resync_heal/{peer}_report.json")
                ls = lockstep(report)
                v.require(counter_is(ls.get("unresolved_observation_packets"), 0), f"{peer}: unresolved heal observations")
                v.require(numeric(ls.get("round_readoptions")) and ls["round_readoptions"] in (0, 1) and ls.get("peer_leave_frames") == {}, f"{peer}: heal round/leave counters")
                v.require(counter_is(report.get("resyncs"), 1), f"{peer}: heal did not resync exactly once")
        elif family == "compat":
            data = v.json(root / "summary.json")
            reference = BASELINE["compat"][job.meta["case"]]
            v.require(data.get("fixture_sha256") == reference["fixture_sha256"], "compat fixture changed")
            rows = data.get("results", [])
            v.require(len(rows) == 1, "compat must execute exactly one rung per driver")
            if rows:
                row = rows[0]
                v.require(row.get("label") == job.meta["rung"], "compat rung differs")
                v.require(row.get("exe_sha256") == (REFERENCE_SHA if job.meta["rung"] == "source22" else sha), "compat binary differs")
                v.require(row.get("exit_code") == reference["exit_code"] and row.get("timed_out") is False, "compat process termination differs from the retained reference")
                v.require(row.get("cases") == reference["cases"], "compat named case results differ from Source22")
                # report.md:1531-1534 and :1574: same exe printed uid 1049494 then 1049517
                v.require(compat_lines_match(row.get("lines"), reference["lines"]), COMPAT_LINES_ORACLE)
                scenario = v.json(root / job.meta["rung"] / "trace.json")
                runs = scenario.get("runs", [])
                v.require(len(runs) == 1 and runs[0].get("scenario") == "ActorStress" and runs[0].get("ticks") == reference["ticks"] + 1 and runs[0].get("numeric", {}).get("ended_externally") == 1,
                          "compat short-cap scenario completion is not evidenced")
        else:
            v.failures.append("unknown validator family")
    except Exception as error:
        v.failures.append(f"missing/invalid result: {type(error).__name__}: {error}")
    for inspector in (lambda: scan_logs(v, root), lambda: scan_log(v, log),
                      lambda: inspect_launches(v, root, sha, job.family == "compat" and job.meta["rung"] == "source22"),
                      lambda: inspect_counters(v, root, job.family in ("h4", "b1"))):
        try:
            inspector()
        except Exception as error:
            v.failures.append(f"evidence inspection failed: {type(error).__name__}: {error}")
    return {"id": job.id, "family": family, "passed": not v.failures, "exit": code,
            "failures": v.failures, "evidence": sorted(set(v.evidence)), "counters": v.counters}


def now():
    return time.strftime("%Y-%m-%dT%H:%M:%SZ", time.gmtime())


def validate_breadth_result(data: dict, *, source: int, build_manifest: str | Path,
                            exe_sha256: str) -> list[str]:
    """Pure aggregate validation for the family orchestrator; never reads files or launches."""
    errors = []
    if not isinstance(data, dict):
        return ["breadth result is not an object"]
    if data.get("source") != source:
        errors.append("breadth source differs")
    norm = lambda path: ntpath.normcase(ntpath.normpath(str(path)))
    if norm(data.get("build_manifest", "")) != norm(build_manifest):
        errors.append("breadth build manifest differs")
    if data.get("exe_sha256") != exe_sha256:
        errors.append("breadth executable pin differs")
    source_head = data.get("source_head")
    if not isinstance(source_head, str) or not re.fullmatch(r"[0-9a-f]{40}", source_head):
        errors.append("breadth source HEAD is missing or malformed")
    if data.get("execution_pass") is not True or data.get("complete") is not True:
        errors.append("breadth is not complete and passing")
    if not isinstance(data.get("finished"), str) or not data["finished"]:
        errors.append("breadth has no completion timestamp")
    if data.get("stop_reason") is not None or data.get("missing_jobs") != []:
        errors.append("breadth stopped or has missing jobs")
    if data.get("validation_errors"):
        errors.append("breadth records aggregate validation errors")
    if data.get("expected_jobs") != list(REQUIRED_CASES):
        errors.append("declared breadth inventory differs")
    steps = data.get("steps")
    if not isinstance(steps, list):
        return errors + ["breadth steps are missing"]
    if [row.get("id") if isinstance(row, dict) else None for row in steps] != list(REQUIRED_CASES):
        errors.append("breadth step coverage/order differs; missing, duplicate, or unexpected case")
    for row in steps:
        if not isinstance(row, dict):
            errors.append("malformed breadth step")
            continue
        name = row.get("id", "unnamed")
        if row.get("passed") is not True or row.get("failures") != [] or type(row.get("exit")) is not int or row["exit"] != 0:
            errors.append(f"{name}: failed or incomplete case verdict")
        for key in ("source_before", "source_after"):
            pin = row.get(key)
            if not isinstance(pin, dict) or pin.get("exe_sha256") != exe_sha256 or pin.get("head") != source_head:
                errors.append(f"{name}: missing or wrong {key} pin")
    return errors


def engine_count():
    value = subprocess.check_output(["pwsh", "-NoProfile", "-NonInteractive", "-Command", "@(Get-Process -Name 'Cortex Command' -ErrorAction SilentlyContinue).Count"], text=True)
    return int(value.strip())


def execute(job: Job, log: Path) -> int:
    environment = os.environ.copy()
    for name in list(environment):
        if name.startswith("CC_FAULT_") or name.startswith("CC_TEST_") or name in ("CC_SIM_DUMP", "CC_LANE_RUN_ROOT", "CC_H4_RUN_ROOT", "CC_LANE_MANIFEST", "CC_H4_BUILD_MANIFEST"):
            environment.pop(name)
    environment.update(PYTHONDONTWRITEBYTECODE="1", **job.env)
    with log.open("xb") as stream:
        return subprocess.run(job.command, cwd=REPO, env=environment, stdout=stream,
                              stderr=subprocess.STDOUT, creationflags=getattr(subprocess, "CREATE_NO_WINDOW", 0)).returncode


def run_jobs(jobs: list[Job], root: Path, pin, runner: Callable = execute,
             validator: Callable = validate_job, count_engines: Callable = engine_count):
    summary = {"source": pin.source, "build_manifest": str(pin.path), "exe_sha256": pin.sha, "source_head": pin.head,
               "started": now(), "complete": False, "passed": False, "execution_pass": False,
               "expected_jobs": [job.id for job in jobs], "steps": [], "stop_reason": None}
    (root / "logs").mkdir()
    write_json(root / "plan.json", [asdict(job) for job in jobs])
    write_json(root / "breadth.json", summary)
    for job in jobs:
        print(f"[{now()}] {len(summary['steps']) + 1}/{len(jobs)} {job.id}", flush=True)
        try:
            if Path(job.out).exists():
                raise PinError(f"refusing existing job output: {job.out}")
            if count_engines() != 0:
                raise PinError("an engine is already running; no further launch")
            before = pin.verify()
        except Exception as error:
            summary["stop_reason"] = str(error)
            break
        log = root / "logs" / f"{job.id}.log"
        started = now()
        try:
            code = runner(job, log)
        except Exception as error:
            code = -1
            if not log.exists():
                log.write_text(str(error), encoding="utf-8")
        try:
            result = validator(job, pin.sha, code, log)
        except Exception as error:
            result = {"id": job.id, "family": job.family, "passed": False, "exit": code,
                      "failures": [f"validator failed: {type(error).__name__}: {error}"], "evidence": []}
        result.update(started=started, finished=now(), source_before=before, log=str(log))
        try:
            result["source_after"] = pin.verify()
            if count_engines() != 0:
                raise PinError("driver returned with an engine still running")
        except Exception as error:
            result["passed"] = False
            result["failures"].append(str(error))
            summary["stop_reason"] = str(error)
        summary["steps"].append(result)
        write_json(root / "breadth.json", summary)
        if summary["stop_reason"]:
            break
    seen = [row["id"] for row in summary["steps"]]
    summary["missing_jobs"] = [job.id for job in jobs if job.id not in seen]
    summary["complete"] = seen == summary["expected_jobs"]
    summary["passed"] = bool(jobs) and len(set(summary["expected_jobs"])) == len(jobs) and summary["complete"] and not summary["stop_reason"] and all(row["passed"] is True for row in summary["steps"])
    summary["finished"] = now()
    summary["execution_pass"] = summary["passed"]
    summary["validation_errors"] = validate_breadth_result(summary, source=pin.source,
                                                          build_manifest=pin.path, exe_sha256=pin.sha)
    summary["execution_pass"] = summary["passed"] = not summary["validation_errors"]
    write_json(root / "breadth.json", summary)
    return summary


def load_driver(path: Path, name: str):
    spec = importlib.util.spec_from_file_location(name, path)
    module = importlib.util.module_from_spec(spec)
    sys.modules[name] = module
    spec.loader.exec_module(module)
    return module


def driver_inputs(jobs: list[Job]) -> dict[Path, str]:
    paths = {Path(arg) for job in jobs for arg in job.command if str(arg).endswith((".py", ".ps1"))}
    paths.update((SCRIPT, DRIVERS / "h4gates/common.py", STAGE / "harness_common.ps1",
                  STAGE / "recovery_e2e.py", STAGE / "check_fixture.py", STAGE / "recovery_expanded_mod.py",
                  LANES / "invariance-300/recovery_expanded_mod_t300.py",
                  LANES / "semantic-battery/driver/run_interp_e2e.ps1"))
    paths.update((REPO / "tools" / name) for name in ("run_sim_test.py", "win32_test_runner.py", "isolated_launch.py", "compare_sim_traces.py", "h4_gate_evidence.py"))
    paths.update(STAGE / "fixtures" / name for name in ("pickup_fire.ccreplay", "pickup_fire.txt"))
    paths.update(LANES / "compat-review/fixtures" / name for name in ("compat_review_alias.lua", "compat_review_alias2.lua", "compat_review_extra.lua"))
    paths.add(LANES / "sound-ai-deferral/fixtures/sound_ai_deferral.lua")
    result = {path.resolve(): digest(path) for path in sorted(paths)}
    result[REFERENCE_EXE.resolve()] = REFERENCE_SHA
    return result


def child(options, pin):
    # Re-root the existing guarded function; its checks and fixtures are unchanged.
    out = options.out
    out.mkdir(parents=True, exist_ok=False)
    pin.verify()
    path = LANES / "invariance-300/recovery_expanded_mod_t300.py" if options.child == "invariance" else STAGE / "recovery_expanded_mod.py"
    module = load_driver(path, "breadth_existing_driver")
    if module.REPO.resolve() != REPO.resolve():
        raise PinError("existing driver is not approved-tree configured")
    # Source40 20260909_112620_heal_26d39dc2/result.json input_delay
    module.INPUT_DELAY = 3 if options.child == "heal" else 0
    module.GLOBAL_SCRIPT = None
    sys.path.insert(0, str(STAGE))
    result = module.invariance(out) if options.child == "invariance" else module.heal(out)
    try:
        pin.verify()
        result["source_unchanged"] = True
    except Exception as error:
        result["source_unchanged"] = False
        result["pin_error"] = str(error)
    result["complete"] = True
    result["pass"] = result.get("pass") is True and result["source_unchanged"]
    write_json(out / "result.json", result)
    return 0 if result["pass"] else 1


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--source", type=int, required=True)
    parser.add_argument("--build-manifest", type=Path, required=True)
    parser.add_argument("--out", type=Path, required=True)
    parser.add_argument("--cases", nargs="+", metavar="ID")
    parser.add_argument("--child", choices=("invariance", "heal"), help=argparse.SUPPRESS)
    options = parser.parse_args()
    options.out = options.out.resolve()
    if options.source <= 0:
        parser.error("source must be positive")
    if os.name != "nt":
        parser.error("this driver requires the approved Windows tree")
    if options.out.exists():
        parser.error("output already exists; select a fresh short directory")
    if options.out.is_relative_to(REPO.resolve()) or REPO.resolve().is_relative_to(options.out):
        parser.error("output must be separate from the approved engine tree")
    if len(str(options.out)) > 48:
        parser.error("use a short output path (at most 48 characters) for the engine's module reader")
    pin = Pin(options.source, options.build_manifest)
    pin.verify(artifacts=True)
    if options.child:
        return child(options, pin)
    if digest(REFERENCE_EXE) != REFERENCE_SHA:
        raise PinError("Source22 reference executable changed or unavailable")
    jobs = plan(options.source, pin.path, options.out, pin.sha)
    if tuple(job.id for job in jobs) != REQUIRED_CASES or len(REQUIRED_CASES) != 81:
        raise RuntimeError("breadth coverage inventory is incomplete")
    try:
        jobs = select_jobs(jobs, options.cases)
    except ValueError as error:
        parser.error(str(error))
    options.out.mkdir(parents=True, exist_ok=False)
    pin.driver_inputs = driver_inputs(jobs)
    write_json(options.out / "drivers.json", {str(path): sha for path, sha in pin.driver_inputs.items()})
    write_json(options.out / "pin.json", {"build_manifest": str(pin.path), "build_manifest_sha256": pin.manifest_sha,
                                         "source": options.source, "source_head": pin.head, "exe_sha256": pin.sha})
    summary = run_jobs(jobs, options.out, pin)
    print(json.dumps({key: summary[key] for key in ("execution_pass", "complete", "missing_jobs", "stop_reason")}), flush=True)
    return 0 if summary["execution_pass"] else 1


if __name__ == "__main__":
    raise SystemExit(main())
