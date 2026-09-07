"""Positive and negative controls for complete and shared snapshot comparison."""

import base64
import copy
import contextlib
import io
from pathlib import Path
import sys
import tempfile
import unittest
from unittest.mock import patch
import zipfile


if __package__:
    from . import compare_snapshots as checker
else:
    import compare_snapshots as checker

CHECKER = Path(checker.__file__)
runtime = checker.snapshot_runtime


def string(value):
    return "s" + str(len(value.encode())) + ":" + value


def table(index, pairs, meta="z;"):
    return f"T{index};P-;M{meta}k{len(pairs)};" + "".join(key + value for key, value in pairs)


def graph(nodes=(), roots=(), globals=()):
    named = lambda entries: str(len(entries)) + ";" + "".join(string(key) + value for key, value in entries)
    return ("SG3;r" + named(roots) + "G" + named(globals) + "L0;E0;Rz;X0;N" + str(len(nodes)) + ";" + "".join(nodes)).encode()


def graph_line(data, index=0):
    return f"LuaStateGraph = {index}|" + base64.urlsafe_b64encode(data).decode().replace("=", ".") + "\n"


BASE = "Activity = GameActivity\n\tActivityState = 3\nScene = Scene\n\tPresetName = Shared\n"
BASE += graph_line(graph(globals=(("first", "n1;"),)), 0) + graph_line(graph(globals=(("second", "n2;"),)), 1)
ACTOR = "Activity = GameActivity\n\tActivityState = 4\nScene = Scene\n\tPlaceSceneObject = AHuman\n\t\tUniqueID = 1\n"
AI_GRAPH = graph((table(1, ((string("AI"), "#2;"), (string("shared"), "n10;"))), table(2, ((string("counter"), "n1;"),))), roots=(("1", "#1;"),))


class SnapshotComparisonTests(unittest.TestCase):
    def compare(self, first, second, full=False, entries=None):
        with tempfile.TemporaryDirectory() as directory:
            paths = [Path(directory) / name for name in ("a.ccsave", "b.ccsave")]
            for path, text in zip(paths, (first, second)):
                with zipfile.ZipFile(path, "w") as archive:
                    archive.writestr("Save.ini", text)
                    for name, data in (entries or {}).items():
                        archive.writestr(name, data)
            with patch.object(sys, "argv", [str(CHECKER), *map(str, paths), *( ["--full"] if full else [])]), contextlib.redirect_stdout(io.StringIO()):
                return checker.main()

    def test_matching(self):
        self.assertEqual(self.compare(BASE, BASE), 0)

    def test_every_graph_is_compared(self):
        first = graph_line(graph(globals=(("first", "n1;"),)), 0)
        for value in (BASE.replace(first, graph_line(graph(globals=(("first", "n3;"),)), 0)), BASE.replace(first, "")):
            with self.subTest(value=value):
                self.assertEqual(self.compare(BASE, value), 1)

    def test_required_blocks(self):
        for value in ("", BASE.replace("Scene = Scene\n\tPresetName = Shared\n", ""), BASE + "Scene = Scene\n"):
            with self.subTest(value=value):
                self.assertEqual(self.compare(BASE, value), 1)

    def test_only_filename_metadata_is_normalized(self):
        original = "Scene = Scene\n\tPresetName = a\n\tTerrain = SLTerrain\n\t\tPresetName = a\n\tPlaceSceneObject = AHuman\n\t\tPresetName = a\n\t\tDescription = a\n"
        expected = original.replace("\tPresetName = a\n\tTerrain", "\tPresetName = SNAPSHOT\n\tTerrain").replace("SLTerrain\n\t\tPresetName = a", "SLTerrain\n\t\tPresetName = SNAPSHOT")
        self.assertEqual(checker.normalize_snapshot_name(original, "a"), expected)

    def test_local_activity_binding_is_full_state(self):
        first = BASE.replace("ActivityState = 3", "ActivityState = 3\n\tTeamOfPlayer1 = 0")
        second = first.replace("TeamOfPlayer1 = 0", "TeamOfPlayer1 = 1")
        self.assertEqual(self.compare(first, second), 0)
        self.assertEqual(self.compare(first, second, full=True), 1)
        self.assertEqual(self.compare(BASE, BASE.replace("GameActivity", "ScriptedActivity")), 1)

    def test_local_activity_binding_must_be_present_on_both_peers(self):
        first = BASE.replace("ActivityState = 3", "ActivityState = 3\n\tTeamOfPlayer1 = 0")
        self.assertEqual(self.compare(first, BASE), 1)

    def test_configured_start_activity_is_compared_with_local_slot_scope(self):
        first = BASE + "HasCheckpointStartActivity = 1\nCheckpointStartActivity = GameActivity\n\tTeamOfPlayer1 = 0\n\tStartingGold = 100\n"
        second = first.replace("TeamOfPlayer1 = 0", "TeamOfPlayer1 = 1")
        self.assertEqual(self.compare(first, second), 0)
        self.assertEqual(self.compare(first, second, full=True), 1)
        self.assertEqual(self.compare(first, second.replace("StartingGold = 100", "StartingGold = 200")), 1)
        self.assertEqual(self.compare(first, second.replace("CheckpointStartActivity = GameActivity", "CheckpointStartActivity = GAScripted")), 1)

    def test_configured_start_activity_presence_is_validated(self):
        valid = BASE + "HasCheckpointStartActivity = 0\n"
        self.assertEqual(self.compare(valid, valid), 0)
        malformed = [valid.replace("= 0\n", "= 1\n"), valid + "CheckpointStartActivity = GameActivity\n",
            BASE + "CheckpointStartActivity = GameActivity\n", valid + "HasCheckpointStartActivity = 0\n"]
        for value in malformed:
            with self.subTest(value=value):
                self.assertEqual(self.compare(value, value), 1)

    def test_malformed_graph_never_matches_itself(self):
        examples = (b"SG3;r0;G0;L0;E0;Rz;X0;N1;", graph() + b"trailing",
            graph((table(1, ()),)), graph(globals=(("missing", "#1;"),)),
            graph(globals=(("same", "n1;"), ("same", "n1;"))),
            graph((table(1, (("n1;", "t;"), ("n1.0;", "f;"))),), globals=(("table", "#1;"),)))
        for data in examples:
            with self.subTest(data=data):
                value = ACTOR + graph_line(data)
                self.assertEqual(self.compare(value, value), 1)

    def test_vm_indexes_must_be_unique_and_complete(self):
        for value in (ACTOR + graph_line(graph(), 1), ACTOR + graph_line(graph(), 0) * 2):
            self.assertEqual(self.compare(value, value), 1)

    def test_local_ai_is_only_projected_at_an_actor_root(self):
        first = ACTOR + graph_line(AI_GRAPH)
        second = ACTOR + graph_line(AI_GRAPH.replace(b"counter" + b"n1;", b"counter" + b"n2;"))
        self.assertNotEqual(first, second)
        self.assertEqual(self.compare(first, second), 0)
        self.assertEqual(self.compare(first, second, full=True), 1)
        self.assertEqual(self.compare(first.replace("AHuman", "MOSParticle"), second.replace("AHuman", "MOSParticle")), 1)

    def test_shared_root_field_and_extra_global_are_not_ai(self):
        first = ACTOR + graph_line(AI_GRAPH)
        changed = ACTOR + graph_line(AI_GRAPH.replace(b"n10;", b"n11;"))
        extra = AI_GRAPH.replace(b"G0;", b"G1;s5:extran1;")
        self.assertEqual(self.compare(first, changed), 1)
        self.assertEqual(self.compare(first, ACTOR + graph_line(extra)), 1)

    def test_global_alias_into_ai_promotes_it_to_shared(self):
        data = AI_GRAPH.replace(b"G0;", b"G1;s5:alias#2;")
        first = ACTOR + graph_line(data)
        second = ACTOR + graph_line(data.replace(b"countern1;", b"countern2;"))
        self.assertEqual(self.compare(first, second), 1)

    def test_alias_to_ai_descendant_cannot_be_split(self):
        nodes = (table(1, ((string("AI"), "#2;"),)), table(2, ((string("counter"), "#3;"),)), table(3, ((string("value"), "n1;"),)))
        first = graph(nodes, roots=(("1", "#1;"),), globals=(("alias", "#3;"),))
        second = graph((*nodes, table(4, ((string("value"), "n1;"),))), roots=(("1", "#1;"),), globals=(("alias", "#4;"),))
        self.assertEqual(self.compare(ACTOR + graph_line(first), ACTOR + graph_line(second)), 1)

    def test_native_projection_has_exact_owners(self):
        for owner, name, allowed in (("AEmitter", "SpecialBehaviour_AvgImpulse", True),
                ("SoundSet", "SpecialBehaviour_CurrentSelectionIndex", True),
                ("AHuman", "SpecialBehaviour_AvgImpulse", False), ("AHuman", "Frame", False)):
            first = ACTOR + f"\t\tChild = {owner}\n\t\t\t{name} = 1\n"
            second = first.replace(name + " = 1", name + " = 2")
            with self.subTest(owner=owner, name=name):
                self.assertEqual(self.compare(first, second), 0 if allowed else 1)
                self.assertEqual(self.compare(first, second, full=True), 1)

    def test_muzzle_frame_does_not_hide_other_sprite_or_physics_fields(self):
        first = ACTOR + "\t\tHeldDevice = HDFirearm\n\t\t\tFlash = Attachable\n\t\t\t\tFrame = 1\n\t\t\t\tMass = 1\n"
        second = first.replace("Frame = 1", "Frame = 4")
        self.assertEqual(self.compare(first, second), 0)
        self.assertEqual(self.compare(first, second, full=True), 1)
        self.assertEqual(self.compare(first, second.replace("Mass = 1", "Mass = 2")), 1)
        self.assertEqual(self.compare(first.replace("Flash =", "Magazine ="), second.replace("Flash =", "Magazine =")), 1)

    def test_limb_projection_preserves_sim_time_and_timer_limits(self):
        fields = "LP2 0 0 0 4999800 4999800 1 0 0 4 16 0 -1 0.5 2.5 1 1 1 1 1 1 4000 880 755 0 -0.15263844 0.049958397 0 0 1 0 0 1 0 0 16 0 0 5000225 -1 -1 5000225 -1 -1 0 0 0".split()
        first = ACTOR + "\t\tLimbPathState = " + " ".join(fields) + "\n"
        for index in (4, 38, 39, 40, 41, 42, 43):
            changed = list(fields)
            changed[index] = "123"
            second = ACTOR + "\t\tLimbPathState = " + " ".join(changed) + "\n"
            with self.subTest(index=index):
                self.assertEqual(self.compare(first, second), 0 if index in (38, 41) else 1)
                self.assertEqual(self.compare(first, second, full=True), 1)


class GraphIdentityTests(unittest.TestCase):
    def compare(self, first, second):
        return checker.compare_graphs(checker.parse_graph(first), checker.parse_graph(second))

    def test_distinct_equal_values_cannot_merge_or_split(self):
        alias = graph((table(1, ((string("a"), "#2;"), (string("b"), "#2;"))), table(2, ())), globals=(("test", "#1;"),))
        copies = graph((table(1, ((string("a"), "#2;"), (string("b"), "#3;"))), table(2, ()), table(3, ())), globals=(("test", "#1;"),))
        for first, second in ((alias, copies), (copies, alias)):
            with self.assertRaises(checker.GraphMismatch):
                self.compare(first, second)

    def test_cycle_renumbering_preserves_identity(self):
        first = graph((table(1, ((string("self"), "#1;"), (string("other"), "#2;"))), table(2, ((string("back"), "#1;"),))), globals=(("test", "#1;"),))
        second = graph((table(1, ((string("back"), "#2;"),)), table(2, ((string("other"), "#1;"), (string("self"), "#2;")))), globals=(("test", "#2;"),))
        self.assertEqual(self.compare(first, second)["matched_nodes"], 2)

    def test_reference_table_keys_are_bijective_and_order_independent(self):
        nodes = (table(1, (("#2;", "#3;"), ("#3;", "#2;"))), table(2, ()), table(3, ()))
        first = graph(nodes, globals=(("map", "#1;"), ("key", "#2;")))
        second = graph((table(1, (("#3;", "#2;"), ("#2;", "#3;"))), *nodes[1:]), globals=(("key", "#3;"), ("map", "#1;")))
        self.assertEqual(self.compare(first, second)["matched_nodes"], 3)
        broken = graph((table(1, (("#3;", "#3;"), ("#2;", "#3;"))), *nodes[1:]), globals=(("key", "#3;"), ("map", "#1;")))
        with self.assertRaises(checker.GraphMismatch):
            self.compare(first, broken)

    def test_shared_upvalue_cells_cannot_be_duplicated(self):
        function = lambda index, cell: f"F{index};Ds4:codeEz;u1;c{cell};"
        first = graph((function(1, 3), function(2, 3), "C3;n4;"), globals=(("a", "#1;"), ("b", "#2;")))
        second = graph((function(1, 3), function(2, 4), "C3;n4;", "C4;n4;"), globals=(("a", "#1;"), ("b", "#2;")))
        with self.assertRaises(checker.GraphMismatch):
            self.compare(first, second)

    def test_native_userdata_identity_cannot_be_duplicated(self):
        first = graph((table(1, ((string("a"), "#2;"), (string("b"), "#2;"))), "U2;v1,2;Iz;"), globals=(("test", "#1;"),))
        second = graph((table(1, ((string("a"), "#2;"), (string("b"), "#3;"))), "U2;v1,2;Iz;", "U3;v1,2;Iz;"), globals=(("test", "#1;"),))
        with self.assertRaises(checker.GraphMismatch):
            self.compare(first, second)

    def test_native_runtime_inside_lua_userdata_is_validated(self):
        malformed = b"17 HDFirearmRuntime1 0 "
        native = "ScriptGraphEntity = HDFirearm\n\tSpecialBehaviour_HDFirearmRuntime = " + base64.urlsafe_b64encode(malformed).decode().replace("=", ".") + "\n"
        node = "U1;o" + "".join(string(value) for value in ("HDFirearm", "test", "Base.rte", native)) + "Iz;"
        data = graph((node,), globals=(("native", "#1;"),))
        with self.assertRaises(ValueError):
            self.compare(data, data)


class RuntimeProjectionTests(unittest.TestCase):
    def assert_field(self, original, field_path, allowed, replacement=999, **kwargs):
        changed = copy.deepcopy(original)
        owner = changed
        for key in field_path[:-1]:
            owner = owner[key]
        owner[field_path[-1]] = replacement
        self.assertNotEqual(runtime.project(original), runtime.project(changed))
        if allowed:
            self.assertEqual(runtime.project(original, True, **kwargs), runtime.project(changed, True, **kwargs))
        else:
            self.assertNotEqual(runtime.project(original, True, **kwargs), runtime.project(changed, True, **kwargs))

    def test_clock_projection_preserves_simulation_and_limits(self):
        timer = dict(sim_start=100, sim_limit=20, real_start=101, real_limit=30)
        for key in timer:
            with self.subTest(key=key):
                self.assert_field(timer, (key,), key == "real_start")
        manager = dict(version="TimerMan1", real_time=12, sim_time=15, sim_accumulator=3,
            delta_time=16, delta_time_seconds=12345, time_scale=1, sim_paused=0, sim_update_count=40)
        for key in manager.keys() - {"version"}:
            with self.subTest(key=key):
                self.assert_field(manager, (key,), key in {"real_time", "sim_accumulator"})

    def test_sim_rng_and_new_fields_are_always_shared(self):
        value = dict(version="RuntimeGlobals4", sim_rng=b"simulation", render_rng=b"visual", extra_shared=b"new")
        for key in ("sim_rng", "render_rng", "extra_shared"):
            with self.subTest(key=key):
                self.assert_field(value, (key,), key == "render_rng", b"changed")

    def test_native_hud_and_ai_caches_do_not_mask_physical_fields(self):
        examples = [("ActorRuntime1", "hud_stack", "health"),
            ("HDFirearmRuntime1", "ai_fire_velocity", "rounds_fired"),
            ("AEmitterRuntime1", "average_impulse", "enabled")]
        for version, local, shared in examples:
            value = dict(version=version, **{local: 1, shared: 2})
            with self.subTest(version=version):
                self.assert_field(value, (local,), True)
                self.assert_field(value, (shared,), False)

    def test_terrain_view_offsets_have_exact_layer_scope(self):
        layer = dict(version="TerrainLayer1", offset=[1, 2], origin=[3, 4])
        value = dict(terrain_metadata=dict(terrain_layers=[layer], unseen=[copy.deepcopy(layer)], material_copy=b"terrain"), gravity=[10, 20])
        self.assert_field(value, ("terrain_metadata", "terrain_layers", 0, "offset", 0), True)
        self.assert_field(value, ("terrain_metadata", "terrain_layers", 0, "origin", 0), False)
        self.assert_field(value, ("terrain_metadata", "unseen", 0, "offset", 0), False)
        self.assert_field(value, ("terrain_metadata", "material_copy"), False, b"changed")
        self.assert_field(value, ("gravity", 1), False)

    def test_background_scratch_projection_preserves_source_pixels_and_configuration(self):
        bitmap = dict(version="GUIBitmap1", value=dict(width=1, height=1, pixels=b"a"))
        value = dict(version="SLBackground2", offset=[1, 2], auto_offset=[3, 4], scroll_interval=8,
            scroll_timer=dict(sim_start=100, sim_limit=20, real_start=101, real_limit=30),
            back_bitmap=bitmap, frames=[copy.deepcopy(bitmap)], main_bitmap=copy.deepcopy(bitmap))
        self.assert_field(value, ("back_bitmap", "value", "pixels"), True, b"b")
        self.assert_field(value, ("frames", 0, "value", "pixels"), False, b"b")
        self.assert_field(value, ("main_bitmap", "value", "pixels"), False, b"b")
        self.assert_field(value, ("back_bitmap", "value", "width"), False)
        self.assert_field(value, ("scroll_interval",), False)
        self.assert_field(value, ("scroll_timer", "sim_start"), True)
        self.assert_field(value, ("scroll_timer", "sim_limit"), False)

    def test_only_local_activity_slot_is_projected(self):
        value = dict(version="Activity1", **{key: [1, 2, 3, 4] for key in
            ("player_team", "team_funds_share", "funds_contribution", "human", "actor_links")}, team_funds=[20, 20, 20, 20])
        for key in ("player_team", "team_funds_share", "funds_contribution", "human", "actor_links"):
            self.assert_field(value, (key, 0), True)
            self.assert_field(value, (key, 1), False)
        self.assert_field(value, ("team_funds", 0), False)

    def test_local_inventory_references_follow_world_ownership_and_preserve_aliases(self):
        scene = "Scene = Scene\n"
        for actor, item in ((10, 11), (20, 21)):
            scene += f"\tPlaceSceneObject = AHuman\n\t\tUniqueID = {actor}\n\t\tHeldDevice = HDFirearm\n\t\t\tUniqueID = {item}\n"
        state = lambda uid: dict(version="Activity1", actor_links=[[uid, uid, 0], [0, 0, 0], [0, 0, 0], [0, 0, 0]])
        roles_a = checker.inventory_reference_roles(scene, state(10))
        roles_b = checker.inventory_reference_roles(scene, state(20))
        entity = lambda uid: dict(version="GUIEntity1", value=dict(uid=uid, **{"class": b"HDFirearm", "preset": b"same", "module": b"Base.rte"}))
        inventory = lambda uid: dict(version="InventoryMenuGUI2", center=[1, 2], actor=entity(uid), equipment=[entity(uid)],
            controls=b"exact gui topology", carousel_bitmap=b"exact pixels")
        path = ("player_ui", 0, "inventory")
        a, b = inventory(11), inventory(21)
        self.assertEqual(runtime.project(a, True, path=path, local_roles=roles_a), runtime.project(b, True, path=path, local_roles=roles_b))
        for bad in (10, 11, 999):
            broken = copy.deepcopy(b)
            broken["equipment"][0]["value"]["uid"] = bad
            self.assertNotEqual(runtime.project(a, True, path=path, local_roles=roles_a), runtime.project(broken, True, path=path, local_roles=roles_b))
        for key in ("controls", "carousel_bitmap"):
            self.assert_field(a, (key,), False, b"changed", path=path, local_roles=roles_a)
        self.assertNotEqual(runtime.project(a, True, path=("player_ui", 1, "inventory"), local_roles=roles_a),
            runtime.project(b, True, path=("player_ui", 1, "inventory"), local_roles=roles_b))
        with self.assertRaises(ValueError):
            checker.inventory_reference_roles(scene, state(999))

    def test_bitmap_parser_rejects_malformed_data_before_scratch_projection(self):
        good = b"10 GUIBitmap1 1 8 2 1 -1 0 2 0 1 2 a\x00 "
        self.assertEqual(runtime.decode(good)["value"]["pixels"], b"a\x00")
        malformed = [good[:-1], good + b"trailing", good.replace(b"2 a\x00", b"3 a\x00"),
            good.replace(b"1 8 2", b"1 9 2"), good.replace(b"0 2 0 1", b"0 3 0 1"), b"10 GUIBitmap1 2 "]
        for data in malformed:
            with self.subTest(data=data), self.assertRaises(ValueError):
                runtime.decode(data)

    def test_unknown_checkpoint_versions_stay_byte_exact(self):
        first = b"7 Future1 1 2 3 "
        second = b"7 Future1 1 2 4 "
        self.assertNotEqual(runtime.project(runtime.decode(first), True), runtime.project(runtime.decode(second), True))


if __name__ == "__main__":
    unittest.main()
