"""Generate read-only diagnostic access from declarations; engine behavior stays frozen."""
from pathlib import Path
import argparse
import collections
import hashlib
import json
import re

DEFAULT_INVENTORY = Path("D:/Projects/reviews/recovery-2026-09-07/contract-audit/native-fields.json")


def parse_args(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--repo", type=Path, required=True, help="engine tree that owns ContractAudit.h")
    parser.add_argument("--inventory", type=Path, default=DEFAULT_INVENTORY)
    parser.add_argument("--header-only", action="store_true",
                        help="rewrite ContractAudit.h only; leave class headers untouched")
    return parser.parse_args(argv)


options = parse_args()
root = Path(__file__).resolve().parent
repo = options.repo.resolve()
inventory = json.loads(Path(options.inventory).read_text())
header_only = options.header_only
names = '''Entity SceneObject MovableObject MOSprite MOSRotating Actor AHuman ACrab ACraft ACDropShip ACRocket ADoor ADSensor Attachable Arm Leg Turret HeldDevice ThrownDevice TDExplosive HDFirearm Magazine Round MOPixel MOSParticle PEmitter AEmitter AEJetpack AtomGroup LimbPath Emission Gib PieMenu PieSlice SoundContainer SoundSet SoundData Activity GameActivity GAScripted GlobalScript Scene Material Icon Box Vector Matrix Timer PersistedTimerAnchor Controller Atom ContentFile RandomGenerator AlarmEvent'''.split()
selected = {'RTE::' + name for name in names}
selected.update('RTE::' + name for name in ('GameActivity::Delivery', 'GameActivity::PurchaseOrder', 'GameActivity::ObjectivePoint', 'ACraft::Exit', 'Arm::HandTarget', 'AHuman::DeferredEquip', 'Scene::Area', 'MovableObject::LuaFunction'))
managers = {'RTE::MovableMan', 'RTE::ActivityMan', 'RTE::SceneMan', 'RTE::TimerMan'}
access = selected | managers
assert access <= inventory['classes'].keys(), access - inventory['classes'].keys()
by_header = collections.defaultdict(list)
for name in access:
    data = inventory['classes'][name]
    by_header[data['path']].append(data['body_start'] + 1)
manifest = {}
for relative, offsets in by_header.items():
    path = repo / relative
    original_path = root / 'observer-originals' / relative
    original_path.parent.mkdir(parents=True, exist_ok=True)
    if original_path.exists():
        original = original_path.read_bytes()
    else:
        original = path.read_bytes()
        if not header_only:
            assert hashlib.sha256(original).hexdigest() == inventory['header_hashes'][relative], relative
            original_path.write_bytes(original)
    modified = original
    for offset in sorted(offsets, reverse=True):
        modified = modified[:offset] + b'\n\t\tfriend struct ContractAudit;\n' + modified[offset:]
    if not header_only:
        path.write_bytes(modified)
    manifest[relative] = {'original_sha256': hashlib.sha256(original).hexdigest(),
                          'instrumented_sha256': hashlib.sha256(modified).hexdigest(),
                          'header_written': not header_only}

header = ['#pragma once', '', '// Read-only audit instrumentation; generated from the retained declaration inventory.']
header += [f'#include "{Path(path).name}"' for path in sorted(by_header)]
header += ['#include "LuaMan.h"', '#include "TerrainLayerSnapshot.h"', '#include <bit>', '#include <fstream>', '#include <iomanip>', '#include <map>', '#include <set>', '#include <sstream>', '#include <type_traits>', '#include <typeinfo>', '#include <utility>', '', 'namespace RTE {', 'struct ContractAudit {', 'using State = std::map<std::string, std::string>;', 'State values;', 'std::map<const void*, std::string> visited;', 'std::set<std::string> gaps;', '']

header.append(r'''
template <class T> std::string Value(const T& value, const std::string& path) {
    using V = std::remove_cvref_t<T>;
    if constexpr (std::is_same_v<V, std::string> || std::is_same_v<V, std::string_view>) {
        std::ostringstream out; out << std::quoted(std::string(value)); return out.str();
    } else if constexpr (std::is_arithmetic_v<V> || std::is_enum_v<V>) {
        std::ostringstream out;
        if constexpr (std::is_floating_point_v<V>) {
            out << std::hexfloat << value << " bits:" << std::hex;
            if constexpr (sizeof(V) == 4) out << std::bit_cast<uint32_t>(value);
            else if constexpr (sizeof(V) == 8) out << std::bit_cast<uint64_t>(value);
        }
        else if constexpr (std::is_enum_v<V>) out << static_cast<std::underlying_type_t<V>>(value);
        else out << +value;
        return out.str();
    } else if constexpr (std::is_pointer_v<V>) {
        if (!value) return "null";
        using P = std::remove_pointer_t<V>;
        if constexpr (requires { sizeof(P); } && !std::is_function_v<P>) {
            if constexpr (std::is_base_of_v<MovableObject, P>) return "uid:" + std::to_string(value->GetUniqueID());
            else if constexpr (std::is_base_of_v<Entity, P>) return EntityValue(value, path);
            else if constexpr (requires { Visit(*value, path); }) {
                auto [it, added] = visited.emplace(value, path);
                if (added) Visit(*value, path);
                return "ref:" + it->second;
            }
        }
        gaps.insert(path + " opaque pointer " + typeid(V).name());
        return "opaque:present";
    } else if constexpr (requires { value.get(); }) {
        return Value(value.get(), path);
    } else if constexpr (requires { value.first; value.second; }) {
        return "(" + Value(value.first, path + ".first") + "," + Value(value.second, path + ".second") + ")";
    } else if constexpr (requires { typename V::mapped_type; std::begin(value); std::end(value); }) {
        std::vector<std::pair<std::string, const typename V::value_type*>> entries;
        for (const auto& element: value) entries.emplace_back(Value(element.first, path + ".key"), &element);
        std::sort(entries.begin(), entries.end(), [](const auto& a, const auto& b) { return a.first < b.first; });
        std::string out = "[";
        for (const auto& [key, element]: entries) out += key + ":" + Value(element->second, path + "[key=" + key + "]") + ";";
        return out + "]";
    } else if constexpr (requires { std::begin(value); std::end(value); }) {
        std::vector<std::string> elements;
        size_t index = 0;
        for (const auto& element: value) elements.push_back(Value(element, path + "[" + std::to_string(index++) + "]"));
        if constexpr (requires { typename V::hasher; }) std::sort(elements.begin(), elements.end());
        std::string out = "[";
        for (const auto& element: elements) out += element + ";";
        return out + "]";
    } else if constexpr (requires { Visit(value, path); }) {
        Visit(value, path);
        return "fields";
    } else {
        gaps.insert(path + " opaque value " + typeid(V).name());
        return "opaque";
    }
}
template <class T> void Field(const std::string& path, const T& value) { values[path] = Value(value, path); }
std::string EntityValue(const Entity* entity, const std::string& path);
void VisitEntity(const Entity& entity, const std::string& path);
''')

fields = collections.defaultdict(list)
for field in inventory['fields']:
    if field['class'] in selected and not field['static']:
        fields[field['class']].append(field)

omitted = []
for name in sorted(selected):
    short = name[5:]
    header.append(f'void Visit(const {short}& object, const std::string& path) {{')
    for bases in inventory['classes'][name]['bases']:
        for base in re.findall(r'public\s+([\w:]+)', bases):
            if 'RTE::' + base in selected:
                header.append(f'Visit(static_cast<const {base}&>(object), path);')
    for field in fields[name]:
        member = field['name']
        # Parser placeholders are method macros rather than C++ data members.
        if member in ('SerializableOverrideMethods', 'ClassInfoGetters', 'SerializableClassNameGetter', 'EntityAllocation') or (name == 'RTE::Actor' and member == 'm_pHitBody'):
            omitted.append(field)
            continue
        header.append(f'Field(path + ".{short}.{member}", object.{member});')
    header.append('}')

header.append(r'''
static void Write(const State& state, const std::string& path) {
    std::ofstream out(path, std::ios::binary);
    for (const auto& [key, value]: state) out << key << " = " << value << "\n";
}
static size_t Compare(const State& before, const State& after, const std::string& path) {
    std::ofstream out(path, std::ios::binary);
    std::set<std::string> keys;
    for (const auto& [key, value]: before) keys.insert(key);
    for (const auto& [key, value]: after) keys.insert(key);
    size_t count = 0;
    for (const auto& key: keys) {
        const auto a = before.find(key), b = after.find(key);
        if (a != before.end() && b != after.end() && a->second == b->second) continue;
        ++count;
        out << key << "\n  before " << (a == before.end() ? "<absent>" : a->second)
            << "\n  after  " << (b == after.end() ? "<absent>" : b->second) << "\n";
    }
    return count;
}
static State Observe(const std::string& gapPath) {
    ContractAudit audit;
    std::map<long int, MovableObject*> objects;
    { std::lock_guard<std::mutex> guard(g_MovableMan.m_ObjectRegisteredMutex); objects = g_MovableMan.m_KnownObjects; }
    size_t invalid = 0;
    for (const auto& [uid, object]: objects) {
        if (!object || uid <= 0 || object->GetUniqueID() != uid) {
            ++invalid;
            audit.Field("registry.invalid[" + std::to_string(uid) + "]", object ? object->GetUniqueID() : -1);
            continue;
        }
    }
    std::cout << "[contract-audit] registry=" << objects.size() << " invalid=" << invalid << std::endl;
    Write(audit.values, gapPath + ".registry.txt");
    for (const auto& [uid, object]: objects) {
        if (object && uid > 0 && object->GetUniqueID() == uid) audit.VisitEntity(*object, "mo[" + std::to_string(uid) + "]");
    }
    if (const Activity* activity = g_ActivityMan.GetActivity()) audit.VisitEntity(*activity, "activity");
    if (const Scene* scene = g_SceneMan.GetScene()) audit.Visit(*scene, "scene");
    audit.Field("rng.sim", g_SimRNG.SerializeCheckpoint());
    audit.Field("rng.render", g_RenderRNG.SerializeCheckpoint());
    audit.Field("clock.sim_count", g_TimerMan.GetSimUpdateCount());
    audit.Field("clock.sim_time", g_TimerMan.GetSimTimeTicks());
    audit.Field("clock.real_time", g_TimerMan.GetRealTickCount());
    audit.Field("clock.ticks_per_second", g_TimerMan.GetTicksPerSecond());
    audit.Field("clock.sim_accumulator", g_TimerMan.GetSimAccumulator());
    audit.Field("clock.dt", g_TimerMan.GetDeltaTimeSecs());
    audit.Field("allocator.uid", MovableObject::GetUniqueIDCounter());
    audit.Field("allocator.lua_cursor", g_LuaMan.GetScriptStateCursor());
    audit.Field("world.actors", g_MovableMan.m_Actors);
    audit.Field("world.items", g_MovableMan.m_Items);
    audit.Field("world.particles", g_MovableMan.m_Particles);
    audit.Field("world.added_actors", g_MovableMan.m_AddedActors);
    audit.Field("world.added_items", g_MovableMan.m_AddedItems);
    audit.Field("world.added_particles", g_MovableMan.m_AddedParticles);
    audit.Field("world.alarms", g_MovableMan.m_AlarmEvents);
    audit.Field("world.added_alarms", g_MovableMan.m_AddedAlarmEvents);
    audit.Field("world.rosters", g_MovableMan.m_ActorRoster);
    audit.Field("world.sort_rosters", g_MovableMan.m_SortTeamRoster);
    audit.Field("world.quarantine", g_MovableMan.m_LockstepJoinQuarantine);
    audit.Field("world.moid_index", g_MovableMan.m_MOIDIndex);
    audit.Field("world.update_number", g_MovableMan.m_SimUpdateFrameNumber);
    audit.Field("world.roster", g_MovableMan.DescribeTeamRosters());
    audit.Field("scene.global_acc", g_SceneMan.GetScene()->GetGlobalAcc());
    TerrainLayerSnapshot terrain;
    if (terrain.Capture()) {
        const auto hash = [](const std::vector<uint8_t>& bytes) {
            uint64_t result = 1469598103934665603ULL;
            for (uint8_t value: bytes) result = (result ^ value) * 1099511628211ULL;
            return result;
        };
        audit.Field("terrain.material", hash(terrain.mat));
        audit.Field("terrain.foreground", hash(terrain.fg));
        audit.Field("terrain.background", hash(terrain.bg));
    } else audit.Field("terrain.capture_failed", true);
    std::ofstream gapsOut(gapPath, std::ios::binary);
    for (const auto& gap: audit.gaps) gapsOut << gap << "\n";
    return std::move(audit.values);
}
static State Identity() {
    State state;
    state["activity"] = std::to_string(reinterpret_cast<uintptr_t>(g_ActivityMan.GetActivity()));
    state["scene"] = std::to_string(reinterpret_cast<uintptr_t>(g_SceneMan.GetScene()));
    state["lua"] = g_MovableMan.DescribeLuaIdentity();
    std::lock_guard<std::mutex> guard(g_MovableMan.m_ObjectRegisteredMutex);
    for (const auto& [uid, object]: g_MovableMan.m_KnownObjects) state["uid:" + std::to_string(uid)] = std::to_string(reinterpret_cast<uintptr_t>(object));
    return state;
}
};
inline std::string ContractAudit::EntityValue(const Entity* entity, const std::string& path) {
    auto [it, added] = visited.emplace(entity, path);
    if (added) VisitEntity(*entity, path);
    return "ref:" + it->second;
}
inline void ContractAudit::VisitEntity(const Entity& entity, const std::string& path) {
    values[path + ".class"] = entity.GetClassName();
''')

def depth(name):
    bases = [base for clause in inventory['classes'][name]['bases'] for base in re.findall(r'public\s+([\w:]+)', clause)]
    return 1 + max((depth('RTE::' + base) for base in bases if 'RTE::' + base in selected), default=0)

def is_entity(name):
    if name == 'RTE::Entity': return True
    return any(is_entity('RTE::' + base) for clause in inventory['classes'][name]['bases'] for base in re.findall(r'public\s+([\w:]+)', clause) if 'RTE::' + base in selected)

for name in sorted((name for name in selected if is_entity(name) and name != 'RTE::Entity'), key=lambda name: (-depth(name), name)):
    short = name[5:]
    header.append(f'if (const auto* typed = dynamic_cast<const {short}*>(&entity)) {{ Visit(*typed, path); return; }}')
header += ['Visit(entity, path);', '}', '} // namespace RTE', '']
out = repo / 'Source/System/ContractAudit.h'
out.write_text('\n'.join(header))
if not header_only:
    (root / 'observer-manifest.json').write_text(json.dumps({'status': 'read-only field access instrumentation; opaque fields explicitly reported', 'classes': sorted(selected), 'headers': manifest, 'omitted_method_macros': omitted, 'field_count': sum(map(len, fields.values())), 'observer_sha256': hashlib.sha256(out.read_bytes()).hexdigest()}, indent=2))
print(json.dumps({'classes': len(selected), 'fields': sum(map(len, fields.values())), 'out': str(out), 'header_only': header_only}))
