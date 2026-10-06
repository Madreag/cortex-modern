"""Check win diagnostics without starting an engine or changing the duel rules."""
from pathlib import Path
import tempfile
import unittest
import spread_peers

from e2e.win_diagnostics import diagnostic_script, stage

try:
    from lupa.lua51 import LuaRuntime
except ImportError:
    LuaRuntime = None


@unittest.skipIf(LuaRuntime is None, "Lua 5.1 runtime is unavailable")
class WinDiagnosticsTests(unittest.TestCase):
    def duel(self, diagnostic):
        lua = LuaRuntime()
        lua.execute('''
            require = function() end
            Activity = {RUNNING=1, TEAM_1=0, MAXTEAMCOUNT=2, PLAYER_1=0, MAXPLAYERCOUNT=0}
            P4AlphaDuel = {}
            Actor = {DYING=3, DEAD=4}
            brains = {}
            objects = {}
            messages = {}
            decisions = {}
            print = function(line) table.insert(messages, line) end
            MovableMan = {
                GetFirstBrainActor=function(_, team) return brains[team] end,
                FindObjectByUniqueID=function(_, uid) return objects[uid] end,
                KillAllEnemyActors=function(_, team) table.insert(decisions, 'kill:'..team) end
            }
            ActivityMan = {EndActivity=function() table.insert(decisions, 'end') end}
            IsActor = function(value) return value ~= nil end
            IsAHuman = IsActor
            ToActor = function(value) return value end
            ToAHuman = ToActor
            activity = {
                ActivityState=1,
                TeamActive=function() return true end,
                BrainlessHumansSpectate=function() return false end
            }
            function brain(uid, team)
                local value = {UniqueID=uid, Team=team, PresetName='Brain Robot', Health=100,
                    Status=0, Age=0, WoundCount=0, ToDelete=false,
                    Head={UniqueID=uid+100, WoundCount=0}}
                value.HasObjectInGroup=function() return true end
                objects[uid] = value
                brains[team] = value
            end
            brain(10, 0)
            brain(20, 1)
        ''')
        source = Path(__file__).resolve().parents[1] / 'Data/Base.rte/Activities/P4AlphaDuel.lua'
        stock = source.read_bytes()
        lua.execute((diagnostic_script(stock) if diagnostic else stock).decode())
        lua.execute('''
            P4AlphaDuel.UpdateActivity(activity)
            brains[1] = nil
            objects[20].Health = -1
            objects[20].Head = nil
            objects[20].Status = Actor.DYING
            P4AlphaDuel.UpdateActivity(activity)
        ''')
        return lua

    def test_head_loss_names_the_actor_and_preserves_the_original_win(self):
        stock, diagnostic = self.duel(False), self.duel(True)
        self.assertEqual(list(stock.globals().decisions.values()),
                         list(diagnostic.globals().decisions.values()))
        self.assertEqual(stock.globals().activity.WinnerTeam, diagnostic.globals().activity.WinnerTeam)
        lines = list(diagnostic.globals().messages.values())
        self.assertTrue(any('brain-lost team=1 uid=20' in line and 'reason=head-missing' in line for line in lines))
        self.assertTrue(any('winner=0' in line and 'reason=last-opposing-brain-lost' in line for line in lines))

    def test_removed_actor_is_named_without_inventing_a_killer(self):
        lua = self.duel(True)
        lua.execute('brains[0]=nil; objects[10]=nil; P4AlphaDuel.UpdateActivity(activity)')
        self.assertTrue(any('brain-lost team=0 uid=10' in line and 'reason=actor-removed' in line
                            for line in lua.globals().messages.values()))


class PrivateStagingTests(unittest.TestCase):
    def test_overlay_does_not_modify_the_source_or_follow_a_data_link(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            source = root / 'repo/Data/Base.rte/Activities/P4AlphaDuel.lua'
            source.parent.mkdir(parents=True)
            original = b'function UpdateActivity(self) end\n'
            source.write_bytes(original)
            runtime = root / 'runtime'
            runtime.mkdir()
            receipt = stage(root / 'repo', runtime)
            self.assertEqual(source.read_bytes(), original)
            self.assertTrue((runtime / receipt['path']).read_bytes().startswith(original))
            linked = root / 'linked'
            linked.mkdir()
            spread_peers.link_directory(root / 'repo/Data', linked / 'Data')
            try:
                with self.assertRaisesRegex(ValueError, 'private Data'):
                    stage(root / 'repo', linked)
            finally:
                (linked / 'Data').rmdir()
            self.assertEqual(source.read_bytes(), original)


if __name__ == '__main__':
    unittest.main()
