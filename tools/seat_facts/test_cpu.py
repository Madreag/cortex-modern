"""CPU roster oracle controls; no engine process is started."""
import unittest

from run_cpu import score


def log(cpu=1, *, legacy=None, enabled=True, director=False, state=4, native=True, write=False):
    legacy = cpu if legacy is None and enabled else (-1 if legacy is None else legacy)
    flags = ",".join(f"{team}:{int(enabled and team == cpu)}" for team in range(cpu + 1))
    native_rows = "".join(f"[e2e] TeamIsCPU team={team} value={int(enabled and team == cpu)}\n" for team in range(cpu + 1)) if native else ""
    rows = "".join(f"[cpu-facts] phase={phase} tick={tick} state={state} running={int(state == 4)} humans=2 legacy={legacy} "
        f"aiTeams={cpu if enabled else 'none'} directorTeams={cpu if director else 'none'} flags={flags}\n"
        for phase, tick in (("roster", 0), ("started", 0), ("first_tick", 1)))
    return native_rows + rows + (f"[cpu-write] derived={legacy} actual={legacy}\n" if write else "")


class CPUOracle(unittest.TestCase):
    def test_both_peers_have_cpu_at_first_tick(self):
        self.assertTrue(score({"host": log(), "client": log()}, "p4")["pass"])

    def test_baseline_empty_cpu_director_is_red(self):
        result = score({"host": log(enabled=False, native=False), "client": log(enabled=False, native=False)}, "p4")
        self.assertFalse(result["pass"])
        self.assertFalse(result["checks"]["host_first_tick_cpu"])
        self.assertFalse(result["checks"]["client_first_tick_cpu"])

    def test_contract_print_is_required(self):
        result = score({"host": log(), "client": log(native=False)}, "p4")
        self.assertFalse(result["checks"]["client_native_roster_prints"])

    def test_retained_battery_accepts_the_exact_print(self):
        import importlib.util
        from pathlib import Path
        path = Path("D:/Projects/reviews/takeover-20260909/grok-workers/fg6d-battery/check_fixture.py")
        spec = importlib.util.spec_from_file_location("cpu_contract_test", path)
        module = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(module)
        self.assertEqual(module.team_is_cpu_visible({}, "[e2e] TeamIsCPU team=1 value=1", 1),
                         (True, "[e2e] TeamIsCPU team=1 value=1"))
        self.assertFalse(module.team_is_cpu_visible({}, "[e2e] TeamIsCPU team=1 value=0", 1)[0])

    def test_missing_peer_or_duplicate_observation_is_red(self):
        self.assertFalse(score({"host": log()}, "p4")["pass"])
        self.assertFalse(score({"host": log(), "client": log() + log()}, "p4")["pass"])

    def test_stock_director_must_have_the_cpu_team(self):
        self.assertTrue(score({"host": log(director=True), "client": log(director=True)}, "stock")["pass"])
        self.assertFalse(score({"host": log(director=True), "client": log()}, "stock")["pass"])

    def test_brain_placement_fallback_requires_explicit_editing(self):
        result = score({"host": log(state=2), "client": log(state=2)}, "stock")
        self.assertTrue(result["needs_brain_placement"])
        self.assertFalse(result["pass"])
        self.assertFalse(score({"host": log(state=2), "client": log(state=2) + "Lua Error"}, "stock")["needs_brain_placement"])
        self.assertFalse(score({"host": "", "client": ""}, "stock")["needs_brain_placement"])

    def test_lua_write_is_shared_and_applied(self):
        self.assertTrue(score({"host": log(2, write=True), "client": log(2, write=True)}, "script")["pass"])
        bad = score({"host": log(2, legacy=1, write=True), "client": log(2, write=True)}, "script")
        self.assertFalse(bad["checks"]["shared_cpu_facts"])
        self.assertFalse(bad["checks"]["host_lua_property_write"])


if __name__ == "__main__":
    unittest.main()
