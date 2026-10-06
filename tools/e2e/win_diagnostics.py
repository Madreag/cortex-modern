"""Observe the unchanged duel in a private runtime and retain native death events."""
import hashlib
import json
from pathlib import Path
import re
import stat


OBSERVER = r'''

local originalStart, originalUpdate, originalEnd = P4AlphaDuel.StartActivity, P4AlphaDuel.UpdateActivity, P4AlphaDuel.EndActivity
local observedActivity, brains, hadBrain = nil, {}, {}

local function brainState(actor)
    local head = IsAHuman(actor) and ToAHuman(actor).Head or nil
    return {uid=actor.UniqueID, preset=actor.PresetName, team=actor.Team,
        health=actor.Health, status=actor.Status, age=actor.Age,
        wounds=actor.WoundCount, deleting=actor.ToDelete,
        brain=actor:HasObjectInGroup("Brains"), head=head ~= nil,
        headUID=head and head.UniqueID or -1, headWounds=head and head.WoundCount or -1}
end

local function describe(value)
    return " uid="..value.uid.." preset="..value.preset.." health="..value.health..
        " status="..value.status.." age_ms="..value.age.." wounds="..value.wounds..
        " deleting="..tostring(value.deleting).." brain_group="..tostring(value.brain)..
        " head="..tostring(value.head).." head_uid="..value.headUID.." head_wounds="..value.headWounds
end

local function observe(self)
    if observedActivity ~= self then
        observedActivity, brains, hadBrain = self, {}, {}
    end
    local alive, lost = {}, false
    for team = Activity.TEAM_1, Activity.MAXTEAMCOUNT - 1 do
        if self:TeamActive(team) then
            local actor = MovableMan:GetFirstBrainActor(team)
            if actor then
                local value = brainState(actor)
                if not brains[team] or brains[team].uid ~= value.uid then
                    print("[win-cause] brain-live team="..team..describe(value))
                end
                brains[team], hadBrain[team] = value, true
                table.insert(alive, team)
            elseif hadBrain[team] then
                lost = true
                if brains[team] then
                    local previous = brains[team]
                    local object = MovableMan:FindObjectByUniqueID(previous.uid)
                    local current = object and IsActor(object) and brainState(ToActor(object)) or nil
                    local reason = "actor-removed"
                    if current then
                        if not current.head then reason = "head-missing"
                        elseif current.health <= 0 then reason = "health-exhausted"
                        elseif current.deleting then reason = "deletion-requested"
                        elseif current.team ~= team then reason = "team-changed"
                        elseif not current.brain then reason = "brain-group-missing"
                        else reason = "absent-from-live-actor-roster" end
                    end
                    print("[win-cause] brain-lost team="..team.." uid="..previous.uid..
                        " reason="..reason.." last={"..describe(previous).."} current={"..
                        (current and describe(current) or "removed").."}")
                    brains[team] = nil
                end
            end
        end
    end
    if lost and #alive <= 1 then
        print("[win-cause] winner="..(#alive == 1 and alive[1] or -1)..
            " reason=last-opposing-brain-lost alive_teams="..#alive)
    end
end

function P4AlphaDuel:StartActivity(...)
    observedActivity, brains, hadBrain = self, {}, {}
    originalStart(self, ...)
    if self.ActivityState == Activity.RUNNING then observe(self) end
end

function P4AlphaDuel:UpdateActivity(...)
    if self.ActivityState == Activity.RUNNING then observe(self) end
    return originalUpdate(self, ...)
end

function P4AlphaDuel:EndActivity(...)
    print("[win-cause] activity-end winner="..tostring(self.WinnerTeam))
    return originalEnd(self, ...)
end
'''


def diagnostic_script(stock):
    return stock + OBSERVER.encode('utf-8')


def stage(repo, runtime):
    runtime = Path(runtime).resolve()
    relative = Path('Data/Base.rte/Activities/P4AlphaDuel.lua')
    target = runtime / relative
    for ancestor in (target, *target.parents):
        if ancestor == runtime:
            break
        if ancestor.exists() and (ancestor.is_symlink() or
                getattr(ancestor.lstat(), 'st_file_attributes', 0) & getattr(stat, 'FILE_ATTRIBUTE_REPARSE_POINT', 0x400)):
            raise ValueError('win diagnostics require private Data overlay ancestors')
    stock = (Path(repo) / relative).read_bytes()
    data = diagnostic_script(stock)
    target.parent.mkdir(parents=True, exist_ok=True)
    target.write_bytes(data)
    return dict(path=relative.as_posix(), original_sha256=hashlib.sha256(stock).hexdigest(),
                staged_sha256=hashlib.sha256(data).hexdigest(), observer_only=True)


def configure_environment(environment, peer_root, scenario, peer, execution):
    """Share an existing case stream, retaining its path and process identity."""
    configured = dict(environment)
    defaults = dict(CC_TEST_CROSS_RECORDS=str(Path(peer_root) / 'win-events.jsonl'),
                    CC_TEST_CROSS_RUN=scenario, CC_TEST_CROSS_INSTANCE=peer,
                    CC_TEST_CROSS_EXECUTION=execution, CC_TEST_CROSS_EVENT_RAW_LIMIT='16777216')
    for key, value in defaults.items():
        configured.setdefault(key, value)
    return configured


def collect(peer_root):
    from cross_report import event_paths, source_rows
    root = Path(peer_root)
    lines = list(dict.fromkeys(line[line.index('[win-cause]'):] for name in ('stdout.log', 'console.log')
                             if (root / name).is_file()
                             for line in (root / name).read_text(encoding='utf-8-sig', errors='replace').splitlines()
                             if '[win-cause]' in line))
    ids = {int(value) for line in lines for value in re.findall(r'(?:uid|head_uid)=(-?\d+)', line) if int(value) >= 0}
    events, errors = [], []
    paths = [path for filename in ('events.jsonl', 'win-events.jsonl') for path in event_paths(root, filename)]
    for path in paths:
        for row in source_rows(path, root):
            kind = row.get('event', row.get('type'))
            if kind == 'malformed_record':
                errors.append(row)
            elif kind in ('wound_added', 'wound_damage', 'impact_damage', 'gibbed', 'dying', 'dead') and (
                    row.get('actor') in ids or row.get('object') in ids):
                events.append(row)
    result = dict(lines=lines, brain_loss_logged=any('brain-lost' in line for line in lines),
                  native_events=events, native_events_path=str(root / 'win-events.jsonl'),
                  native_events_paths=[str(path) for path in paths], native_errors=errors,
                  cause_attribution='Native unattributed damage does not identify a killer.')
    (root / 'win-cause.json').write_text(json.dumps(result, indent=2) + '\n', encoding='utf-8')
    return result
