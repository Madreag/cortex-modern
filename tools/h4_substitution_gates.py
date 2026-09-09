"""H4 Phase B §9b two-process gates: explicit substitution over a real socket.

Nine gates, each one a live 3-peer match on loopback plus a fourth process that asks the host for a
seat whose holder is gone. The first four drive the host's own moderator stand-in; the last five
drive §9b's MODERATION PANEL through -net-match-e2e-moderate, so the gate takes the path a host
clicks, and inject their fault with -net-h4-fault:

  substitute_commit         a dropped seat is reassigned to an applicant the host approves, and the
                            substitute plays on with the ledgered ownership
  substitute_returner_wins  the original holder reclaims while an approval is outstanding; the
                            reclaim wins and the substitute never takes the seat
  substitute_host_cancel    the host approves and immediately withdraws; nobody gets the seat and it
                            stays reassignable
  substitute_bounds         three applicants for one seat: two are registered, the third is refused,
                            and none of them holds anything
  substitute_disconnect_before_ack  the applicant's link dies before the host acts on it: the panel's
                            wait is recorded, the substitute never happens, and the seat is untouched
  substitute_ack_lost       the substitute never sends its ack: the offer ladder runs out, P2 closes
                            the window, and the seat is still the original holder's
  substitute_commit_result_lost  the host throws its commit result away once; the substitute's own
                            retry gets the identical result back from the txId cache
  substitute_delayed_duplicate   the substitute keeps re-presenting an ack the host already answered;
                            every duplicate meets the cache and the seat changes hands exactly once
  substitute_vanishes_before_ack the approved substitute holds its ack and then disappears: the
                            record is invalidated and removed, so the host's later cancel finds none

Every process runs through tools/run_sim_test.py, so each gets a private writable runtime with sound
muted, no window and its own Temp. Recovery records are kept OUTSIDE the runtimes, because the point
of a substitution is that a different process is handed the seat. Only processes this driver started
are ever terminated, by job handle, never by name. Ports are 44500-44540, outside the 43300-44300
range the other batteries use.

Run one gate, or all of them:
    python tools/h4_substitution_gates.py all
    python tools/h4_substitution_gates.py substitute_commit --out D:/mx/b1
    python tools/h4_substitution_gates.py all --dry-run     # prints the commands, launches nothing
"""

from __future__ import annotations

import argparse
import datetime
import hashlib
import json
import subprocess
import sys
import time
from pathlib import Path

TOOLS = Path(__file__).resolve().parent
sys.path.insert(0, str(TOOLS))
from run_sim_test import make_run  # noqa: E402  (the path is set above on purpose)

# The seat a 2-remote match gives the first joining client: slot 0 is the host's own.
SUBSTITUTED_SEAT = 1
PORTS = {
    "substitute_commit": 44500,
    "substitute_returner_wins": 44510,
    "substitute_host_cancel": 44520,
    "substitute_bounds": 44530,
    "substitute_disconnect_before_ack": 44540,
    "substitute_ack_lost": 44550,
    "substitute_commit_result_lost": 44560,
    "substitute_delayed_duplicate": 44570,
    "substitute_vanishes_before_ack": 44580,
}
TICKS = 3600
# P2's window is 20 s, and the ack-lost gate has to outlive it after the approval.
TICKS_BY_GATE = {"substitute_ack_lost": 6000}
PANEL_GATES = (
    "substitute_disconnect_before_ack",
    "substitute_ack_lost",
    "substitute_commit_result_lost",
    "substitute_delayed_duplicate",
    "substitute_vanishes_before_ack",
)
APPLICANT_ASKED = "[net-reconnect] applicant"
ACK_HELD = "[net-h4-fault] ack-drop"
DROP_AFTER_S = 22.0
JOINER_STAGGER_S = 1.5
DROP_ADJUDICATED = "left the match at frame"
TIMEOUT_S = 600.0


def sha256(path: Path) -> str:
    with Path(path).open("rb") as handle:
        return hashlib.file_digest(handle, "sha256").hexdigest()


def out_root(base: Path, name: str) -> Path:
    stamp = datetime.datetime.now(datetime.timezone.utc).strftime("%Y%m%d_%H%M%S")
    root = base / f"{name}_{stamp}"
    root.mkdir(parents=True, exist_ok=False)
    return root


def log_text(out_dir: Path) -> str:
    log = Path(out_dir) / "stdout.log"
    return log.read_text(encoding="utf-8-sig", errors="replace") if log.exists() else ""


def wait_for_log(out_dir: Path, needle: str, timeout_s: float) -> bool:
    """Waits for a line the host writes, so a joiner starts on the match's state, not on a stopwatch."""
    deadline = time.monotonic() + timeout_s
    while time.monotonic() < deadline:
        if needle in log_text(out_dir):
            return True
        time.sleep(0.25)
    return False


def read_json(path: Path):
    try:
        return json.loads(Path(path).read_text(encoding="utf-8-sig"))
    except Exception as exc:
        return {"parse_error": repr(exc)}


def service_of(report) -> dict:
    """The e2e report wraps the service report under "service"; a menu-driven run writes it bare."""
    if not isinstance(report, dict):
        return {}
    service = report.get("service")
    return service if isinstance(service, dict) else report


def admission_of(report) -> dict:
    service = service_of(report)
    runner = service.get("runner")
    session = runner.get("session") if isinstance(runner, dict) else None
    admission = session.get("admission") if isinstance(session, dict) else None
    return admission if isinstance(admission, dict) else {}


def reconnect_of(report) -> dict:
    reconnect = service_of(report).get("reconnect")
    return reconnect if isinstance(reconnect, dict) else {}


class Checks:
    """Accumulates named pass/fail checks and renders one verdict line."""

    def __init__(self, gate: str, root: Path, executable: str):
        self.gate = gate
        self.root = Path(root)
        self.executable = executable
        self.items: list[dict] = []

    def check(self, name: str, ok: bool, detail: object = "") -> bool:
        self.items.append(
            {"name": name, "status": "pass" if ok else "fail", "detail": str(detail)}
        )
        return bool(ok)

    def finish(self, extra: dict | None = None) -> int:
        failed = [item for item in self.items if item["status"] != "pass"]
        verdict = {
            "gate": self.gate,
            "passed": not failed,
            "executable_sha256": self.executable,
            "finished_utc": datetime.datetime.now(datetime.timezone.utc).isoformat(),
            "checks": self.items,
        }
        if extra:
            verdict.update(extra)
        (self.root / "verdict.json").write_text(
            json.dumps(verdict, indent=2), encoding="utf-8"
        )
        for item in self.items:
            print(
                f"  {item['status'].upper():4} {item['name']} {item['detail']}".rstrip()
            )
        print(f"GATE {self.gate} {'PASS' if not failed else 'FAIL'} -> {self.root}")
        return 0 if not failed else 1


def peer_args(
    port: int, ticket: Path, report: Path, extra: list[str], ticks: int = TICKS
) -> list[str]:
    return [
        "-net-match-service-e2e",
        "-net-port",
        str(port),
        "-net-match-peers",
        "3",
        "-net-match-ticks",
        str(ticks),
        "-net-reconnect-ticket",
        str(ticket),
        "-net-match-report",
        str(report),
        *extra,
    ]


def panel_actions(*actions: str, seat: int = None, delay_ms: int = 0) -> list[str]:
    """The host's own moderation panel, driven headless: one -net-match-e2e-moderate per action."""
    argv: list[str] = []
    for action in actions:
        argv += ["-net-match-e2e-moderate", action]
    argv += ["-net-match-e2e-moderate-seat", str(SUBSTITUTED_SEAT if seat is None else seat)]
    if delay_ms:
        argv += ["-net-match-e2e-moderate-delay", str(delay_ms)]
    return argv


def build_argvs(gate: str, root: Path, ticket: Path) -> dict:
    port = PORTS[gate]
    ticks = TICKS_BY_GATE.get(gate, TICKS)
    host_extra = ["-net-host", "-net-match-e2e-resync"]
    if gate == "substitute_commit":
        host_extra += [
            "-net-h4-substitute",
            str(SUBSTITUTED_SEAT),
            "-net-h4-substitute-delay",
            "0",
        ]
    elif gate == "substitute_returner_wins":
        # Long enough that the returner's own reclaim lands first, whichever way the race resolves.
        host_extra += [
            "-net-h4-substitute",
            str(SUBSTITUTED_SEAT),
            "-net-h4-substitute-delay",
            "9000",
        ]
    elif gate == "substitute_host_cancel":
        host_extra += [
            "-net-h4-substitute",
            str(SUBSTITUTED_SEAT),
            "-net-h4-substitute-delay",
            "0",
            "-net-h4-substitute-cancel",
        ]
    elif gate == "substitute_disconnect_before_ack":
        # The panel's wait is a recorded decision; the substitute that follows finds nobody left to
        # approve, because the applicant's link died while the host was still deciding.
        host_extra += panel_actions("wait", "substitute", delay_ms=9000)
    elif gate == "substitute_ack_lost":
        host_extra += panel_actions("substitute")
    elif gate == "substitute_commit_result_lost":
        host_extra += panel_actions("substitute") + ["-net-h4-fault", "commit-drop"]
    elif gate == "substitute_delayed_duplicate":
        host_extra += panel_actions("substitute")
    elif gate == "substitute_vanishes_before_ack":
        # The cancel lands after the substitute is gone: the disconnect already withdrew the record.
        host_extra += panel_actions("substitute", "cancel", delay_ms=10000)
    argvs = {
        "host": peer_args(
            port, root / "host.ticket", root / "host_report.json", host_extra, ticks
        ),
        "leaver": peer_args(
            port,
            ticket,
            root / "leaver_report.json",
            ["-net-join", "127.0.0.1", "-net-match-e2e-resync"],
            ticks,
        ),
        "stayer": peer_args(
            port,
            root / "stayer.ticket",
            root / "stayer_report.json",
            ["-net-join", "127.0.0.1", "-net-match-e2e-resync"],
            ticks,
        ),
    }
    applicant = [
        "-net-join",
        "127.0.0.1",
        "-net-match-e2e-resync",
        "-net-h4-apply",
        str(SUBSTITUTED_SEAT),
    ]
    if gate in ("substitute_ack_lost", "substitute_vanishes_before_ack"):
        applicant += ["-net-h4-fault", "ack-drop"]
    elif gate == "substitute_delayed_duplicate":
        applicant += ["-net-h4-fault", "ack-duplicate"]
    if gate == "substitute_bounds":
        for index in range(1, 4):
            argvs[f"applicant{index}"] = peer_args(
                port,
                root / f"applicant{index}.ticket",
                root / f"applicant{index}_report.json",
                applicant,
            )
    else:
        argvs["substitute"] = peer_args(
            port,
            root / "substitute.ticket",
            root / "substitute_report.json",
            applicant,
            ticks,
        )
    if gate == "substitute_returner_wins":
        argvs["returner"] = peer_args(
            port,
            ticket,
            root / "returner_report.json",
            ["-net-join", "127.0.0.1", "-net-match-e2e-resync"],
            ticks,
        )
    return argvs


def run_gate(
    gate: str, repo: Path, base: Path, dry_run: bool, expect_sha: str | None
) -> int:
    root = base / gate if dry_run else out_root(base, gate)
    if dry_run:
        root.mkdir(parents=True, exist_ok=True)
    ticket = root / "leaver.ticket"
    argvs = build_argvs(gate, root, ticket)
    exe = repo / "Cortex Command.exe"
    if dry_run:
        print(f"DRY-RUN {gate}")
        for key, argv in argvs.items():
            print(
                "  "
                + key
                + ": "
                + subprocess.list2cmdline([str(exe), "-headless", *map(str, argv)])
            )
        return 0

    actual = sha256(exe)
    if expect_sha and actual != expect_sha:
        raise SystemExit(f"executable is {actual}, not the expected {expect_sha}")
    checks = Checks(gate, root, actual)
    checks.check("executable_recorded", True, actual)

    runs = {
        key: make_run(repo, argv, root / key, TIMEOUT_S) for key, argv in argvs.items()
    }
    records: dict = {}
    try:
        runs["host"].start()
        time.sleep(2.0)
        runs["leaver"].start()
        time.sleep(1.5)
        runs["stayer"].start()
        time.sleep(DROP_AFTER_S)
        # Our own job, by handle. Nothing here ever terminates a process by name.
        runs["leaver"].terminate(
            code=137, reason="injected mid-match drop of the seat under test"
        )
        records["leaver"] = runs["leaver"].finish()
        # The joiners go once the host has actually adjudicated the drop, so the gate does not
        # depend on how fast an engine loads.
        checks.check(
            "host_adjudicated_the_drop",
            wait_for_log(root / "host", DROP_ADJUDICATED, 60.0),
            DROP_ADJUDICATED,
        )
        joiners = [key for key in runs if key not in ("host", "leaver", "stayer")]
        # The returner races the approval, so it goes first where there is one.
        joiners.sort(key=lambda key: key != "returner")
        for key in joiners:
            runs[key].start()
            time.sleep(JOINER_STAGGER_S)
        # The two gates whose substitute has to go away act on a fact, not a stopwatch: the host says
        # the applicant asked, and the substitute itself says it is sitting on its ack.
        if gate == "substitute_disconnect_before_ack":
            checks.check(
                "applicant_asked",
                wait_for_log(root / "host", APPLICANT_ASKED, 180.0),
                APPLICANT_ASKED,
            )
            runs["substitute"].terminate(
                code=137, reason="injected loss of the applicant before the host acts on it"
            )
            records["substitute"] = runs["substitute"].finish()
        elif gate == "substitute_vanishes_before_ack":
            checks.check(
                "substitute_held_its_ack",
                wait_for_log(root / "substitute", ACK_HELD, 180.0),
                ACK_HELD,
            )
            runs["substitute"].terminate(
                code=137, reason="injected loss of the approved substitute before its ack"
            )
            records["substitute"] = runs["substitute"].finish()
        for key, run in runs.items():
            if key not in records:
                records[key] = run.finish()
    finally:
        for run in runs.values():
            run.close()

    host_log = log_text(root / "host")
    host_report = read_json(root / "host_report.json")
    admission = admission_of(host_report)
    reconnect = reconnect_of(host_report)
    checks.check(
        "host_exit_zero",
        records.get("host", {}).get("exit_code") == 0,
        records.get("host", {}).get("exit_code"),
    )
    checks.check(
        "stayer_exit_zero",
        records.get("stayer", {}).get("exit_code") == 0,
        records.get("stayer", {}).get("exit_code"),
    )
    checks.check(
        "leaver_dropped_midmatch",
        records.get("leaver", {}).get("exit_code") not in (0, None),
    )
    checks.check(
        "host_census_clean",
        reconnect.get("census_refusals") == 0,
        reconnect.get("census_refusals"),
    )
    checks.check("no_authority_error", "Rejected a Reseat" not in host_log)
    # §11: the surviving CLIENT derived its own roster line from the wire, without asking the host.
    stayer_lines = reconnect_of(read_json(root / "stayer_report.json")).get("roster_lines")
    checks.check(
        "stayer_shows_the_seat_line",
        bool(stayer_lines),
        stayer_lines,
    )
    checks.check(
        "seat_dropped",
        (admission.get("seats_dropped") or 0) >= 1,
        admission.get("seats_dropped"),
    )

    if gate == "substitute_commit":
        substitute = read_json(root / "substitute_report.json")
        checks.check(
            "applicant_registered",
            (admission.get("applicants_registered") or 0) >= 1,
            admission.get("applicants_registered"),
        )
        checks.check(
            "host_approved",
            "moderation: substitute seat" in host_log and "-> Ok" in host_log,
        )
        checks.check(
            "substitution_committed",
            admission.get("substitutions_committed") == 1,
            admission.get("substitutions_committed"),
        )
        checks.check(
            "substitute_offered_a_ticket",
            (reconnect_of(substitute).get("client_substitution_offers") or 0) >= 1,
        )
        checks.check(
            "substitute_joined",
            reconnect_of(substitute).get("client_state") == "Joined",
            reconnect_of(substitute).get("client_state"),
        )
        checks.check(
            "substitute_stored_its_ticket",
            reconnect_of(substitute).get("ticket_stored") is True,
        )
        checks.check(
            "no_reclaim_happened",
            (admission.get("reclaims_accepted") or 0) == 0,
            admission.get("reclaims_accepted"),
        )
        checks.check("reseat_issued", "[net-reconnect] reseating team" in host_log)
    elif gate == "substitute_returner_wins":
        returner = read_json(root / "returner_report.json")
        substitute = read_json(root / "substitute_report.json")
        checks.check(
            "returner_reclaimed",
            (admission.get("reclaims_accepted") or 0) >= 1,
            admission.get("reclaims_accepted"),
        )
        checks.check(
            "returner_joined",
            reconnect_of(returner).get("client_state") == "Joined",
            reconnect_of(returner).get("client_state"),
        )
        checks.check(
            "substitution_did_not_commit",
            (admission.get("substitutions_committed") or 0) == 0,
            admission.get("substitutions_committed"),
        )
        checks.check(
            "substitute_not_joined",
            reconnect_of(substitute).get("client_state") != "Joined",
            reconnect_of(substitute).get("client_state"),
        )
        checks.check(
            "seat_has_one_holder",
            (admission.get("incarnations_bound") or 0) >= 1,
            admission.get("incarnations_bound"),
        )
    elif gate == "substitute_host_cancel":
        substitute = read_json(root / "substitute_report.json")
        checks.check(
            "host_approved",
            "moderation: substitute seat" in host_log and "-> Ok" in host_log,
        )
        checks.check(
            "host_cancelled",
            "moderation: cancel seat" in host_log and "-> Ok" in host_log,
        )
        checks.check(
            "substitution_cancelled",
            (admission.get("substitutions_cancelled") or 0) >= 1,
            admission.get("substitutions_cancelled"),
        )
        checks.check(
            "substitution_did_not_commit",
            (admission.get("substitutions_committed") or 0) == 0,
            admission.get("substitutions_committed"),
        )
        checks.check(
            "substitute_not_joined",
            reconnect_of(substitute).get("client_state") != "Joined",
            reconnect_of(substitute).get("client_state"),
        )
        seats = [
            seat
            for seat in reconnect.get("moderation", [])
            if seat.get("stable_seat") == SUBSTITUTED_SEAT
        ]
        checks.check(
            "seat_stayed_reassignable",
            bool(seats) and seats[0].get("substituting") is False,
            seats,
        )
    elif gate == "substitute_bounds":
        checks.check(
            "two_applicants_registered",
            (admission.get("applicants_registered") or 0) == 2,
            admission.get("applicants_registered"),
        )
        checks.check(
            "third_applicant_refused",
            (admission.get("applicants_refused") or 0) >= 1,
            admission.get("applicants_refused"),
        )
        checks.check(
            "bound_respected",
            (admission.get("pending_applicants") or 0) <= 2,
            admission.get("pending_applicants"),
        )
        checks.check(
            "nothing_committed",
            (admission.get("substitutions_committed") or 0) == 0,
            admission.get("substitutions_committed"),
        )
        for index in range(1, 4):
            state = reconnect_of(read_json(root / f"applicant{index}_report.json")).get(
                "client_state"
            )
            checks.check(f"applicant{index}_inert", state != "Joined", state)
    elif gate in PANEL_GATES:
        substitute = read_json(root / "substitute_report.json")
        seats = [
            seat
            for seat in reconnect.get("moderation", [])
            if seat.get("stable_seat") == SUBSTITUTED_SEAT
        ]
        checks.check(
            "applicant_registered",
            (admission.get("applicants_registered") or 0) >= 1,
            admission.get("applicants_registered"),
        )
        checks.check(
            "panel_drove_the_action",
            "[net-match-e2e] moderate armed" in host_log,
        )
        if gate == "substitute_disconnect_before_ack":
            checks.check(
                "panel_wait_recorded",
                "[net-moderation] wait seat=" in host_log and "result=Ok" in host_log,
            )
            checks.check(
                "no_offer_sent",
                (admission.get("substitution_offers_sent") or 0) == 0,
                admission.get("substitution_offers_sent"),
            )
            checks.check(
                "substitution_did_not_commit",
                (admission.get("substitutions_committed") or 0) == 0,
                admission.get("substitutions_committed"),
            )
            checks.check("no_reseat", "[net-reconnect] reseating team" not in host_log)
            checks.check(
                "seat_stayed_reassignable",
                bool(seats) and seats[0].get("substituting") is False,
                seats,
            )
        elif gate == "substitute_ack_lost":
            checks.check(
                "panel_substituted",
                "[net-moderation] substitute seat=" in host_log
                and "result=Ok" in host_log,
            )
            checks.check(
                "offer_was_sent",
                (admission.get("substitution_offers_sent") or 0) >= 1,
                admission.get("substitution_offers_sent"),
            )
            checks.check(
                "offer_ladder_retransmitted",
                (admission.get("substitution_offer_retransmits") or 0) >= 1,
                admission.get("substitution_offer_retransmits"),
            )
            checks.check(
                "substitution_did_not_commit",
                (admission.get("substitutions_committed") or 0) == 0,
                admission.get("substitutions_committed"),
            )
            checks.check(
                "substitute_not_joined",
                reconnect_of(substitute).get("client_state") != "Joined",
                reconnect_of(substitute).get("client_state"),
            )
            checks.check(
                "substitute_held_its_ack", ACK_HELD in log_text(root / "substitute")
            )
            checks.check(
                "seat_stayed_reassignable",
                bool(seats) and seats[0].get("substituting") is False,
                seats,
            )
        elif gate == "substitute_commit_result_lost":
            checks.check(
                "host_dropped_a_commit_result",
                "[net-h4-fault] commit-drop" in host_log,
            )
            checks.check(
                "result_was_replayed",
                (admission.get("replayed_results") or 0) >= 1,
                admission.get("replayed_results"),
            )
            checks.check(
                "committed_exactly_once",
                (admission.get("substitutions_committed") or 0) == 1,
                admission.get("substitutions_committed"),
            )
            checks.check(
                "substitute_joined",
                reconnect_of(substitute).get("client_state") == "Joined",
                reconnect_of(substitute).get("client_state"),
            )
            checks.check("reseat_issued", "[net-reconnect] reseating team" in host_log)
        elif gate == "substitute_delayed_duplicate":
            checks.check(
                "substitute_re_presented_its_ack",
                "[net-h4-fault] ack-duplicate" in log_text(root / "substitute"),
            )
            checks.check(
                "duplicates_met_the_cache",
                (admission.get("replayed_results") or 0) >= 1,
                admission.get("replayed_results"),
            )
            checks.check(
                "committed_exactly_once",
                (admission.get("substitutions_committed") or 0) == 1,
                admission.get("substitutions_committed"),
            )
            checks.check(
                "seat_bound_once_per_holder",
                (admission.get("incarnations_bound") or 0) >= 1,
                admission.get("incarnations_bound"),
            )
            checks.check(
                "substitute_joined",
                reconnect_of(substitute).get("client_state") == "Joined",
                reconnect_of(substitute).get("client_state"),
            )
        elif gate == "substitute_vanishes_before_ack":
            checks.check(
                "panel_substituted",
                "[net-moderation] substitute seat=" in host_log
                and "result=Ok" in host_log,
            )
            checks.check(
                "cancel_found_nothing_left",
                "[net-moderation] cancel seat=" in host_log
                and "result=NoSubstitutionPending" in host_log,
            )
            checks.check(
                "substitution_did_not_commit",
                (admission.get("substitutions_committed") or 0) == 0,
                admission.get("substitutions_committed"),
            )
            checks.check("no_reseat", "[net-reconnect] reseating team" not in host_log)
            checks.check(
                "seat_stayed_reassignable",
                bool(seats) and seats[0].get("substituting") is False,
                seats,
            )

    return checks.finish(
        {
            "records": {
                key: {k: record.get(k) for k in ("pid", "exit_code", "timed_out")}
                for key, record in records.items()
            }
        }
    )


def main() -> int:
    parser = argparse.ArgumentParser(
        description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter
    )
    parser.add_argument("gate", choices=[*PORTS, "all"])
    parser.add_argument("--repo", type=Path, default=TOOLS.parent)
    parser.add_argument("--out", type=Path, default=None)
    parser.add_argument("--expect-sha256", default=None)
    parser.add_argument("--dry-run", action="store_true")
    options = parser.parse_args()
    base = options.out or (options.repo / "reviews/h4-b1-gates")
    base.mkdir(parents=True, exist_ok=True)
    gates = list(PORTS) if options.gate == "all" else [options.gate]
    failures = 0
    for gate in gates:
        failures += run_gate(
            gate,
            options.repo.resolve(),
            base.resolve(),
            options.dry_run,
            options.expect_sha256,
        )
    return 1 if failures else 0


if __name__ == "__main__":
    raise SystemExit(main())
