"""Generate read-only diagnostic access from declarations; engine behavior stays frozen."""
from pathlib import Path
import argparse
import collections
import difflib
import hashlib
import json
import re
import sys

DEFAULT_INVENTORY = Path("D:/Projects/reviews/recovery-2026-09-07/contract-audit/native-fields.json")

from observer_definitions import (
    ENTITY_VALUE,
    EXTRA_FIELDS,
    EXTRA_FIELDS_AFTER,
    EXTRA_INCLUDES_AFTER,
    EXTRA_STD_INCLUDES_AFTER,
    EXTRA_VISITS_AFTER,
    OMIT_MEMBERS,
    POST_INVENTORY_VISITS,
    TAIL,
    VALUE_AND_FIELD,
)


def parse_args(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--repo", type=Path, required=True, help="engine tree that owns ContractAudit.h")
    parser.add_argument("--inventory", type=Path, default=DEFAULT_INVENTORY)
    parser.add_argument("--header-only", action="store_true",
                        help="rewrite ContractAudit.h only; leave class headers untouched")
    parser.add_argument("--output", type=Path,
                        help="write ContractAudit.h here instead of the repo path")
    return parser.parse_args(argv)


def extend_includes(lines, extras_after):
    out = []
    for line in lines:
        out.append(line)
        for key, extras in extras_after.items():
            if line in (f'#include "{key}"', f'#include <{key}>'):
                for extra in extras:
                    token = extra if extra[:1] in '"<' else f'"{extra}"'
                    out.append(f'#include {token}')
    return out


FIELD_LINE = re.compile(r"^\s*(Field\(|void Visit\()")


def hand_fields(text):
    return {line for line in text.splitlines() if FIELD_LINE.match(line)}


def generate(options):
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
    planned = []
    for relative, offsets in by_header.items():
        path = repo / relative
        original_path = root / 'observer-originals' / relative
        if original_path.exists():
            original = original_path.read_bytes()
            save_original = None
        else:
            original = path.read_bytes()
            if not header_only:
                assert hashlib.sha256(original).hexdigest() == inventory['header_hashes'][relative], relative
                save_original = original
            else:
                save_original = None
        modified = original
        for offset in sorted(offsets, reverse=True):
            modified = modified[:offset] + b'\n\t\tfriend struct ContractAudit;\n' + modified[offset:]
        planned.append((path, original_path, modified, save_original))
        manifest[relative] = {'original_sha256': hashlib.sha256(original).hexdigest(),
                              'instrumented_sha256': hashlib.sha256(modified).hexdigest(),
                              'header_written': not header_only}

    header = ['#pragma once', '', '// Read-only audit instrumentation; generated from the retained declaration inventory.']
    inventory_includes = [f'#include "{Path(path).name}"' for path in sorted(by_header)]
    header += extend_includes(inventory_includes, EXTRA_INCLUDES_AFTER)
    std_includes = ['#include "LuaMan.h"', '#include "TerrainLayerSnapshot.h"']
    std_includes = extend_includes(std_includes, EXTRA_INCLUDES_AFTER)
    std_includes += ['#include <bit>', '#include <fstream>', '#include <iomanip>', '#include <map>', '#include <set>', '#include <sstream>', '#include <type_traits>', '#include <typeinfo>', '#include <utility>']
    header += extend_includes(std_includes, EXTRA_STD_INCLUDES_AFTER)
    header += ['', 'namespace RTE {', 'struct ContractAudit {', 'using State = std::map<std::string, std::string>;', 'State values;', 'std::map<const void*, std::string> visited;', 'std::set<std::string> gaps;', '', '']
    header.extend(VALUE_AND_FIELD.strip('\n').splitlines())
    header.append('')

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
            if member in ('SerializableOverrideMethods', 'ClassInfoGetters', 'SerializableClassNameGetter', 'EntityAllocation') or (name == 'RTE::Actor' and member == 'm_pHitBody') or (name, member) in OMIT_MEMBERS:
                omitted.append(field)
                continue
            header.append(f'Field(path + ".{short}.{member}", object.{member});')
            header.extend(EXTRA_FIELDS_AFTER.get(short, {}).get(member, []))
        header.extend(EXTRA_FIELDS.get(short, []))
        header.append('}')
        extra_visit = EXTRA_VISITS_AFTER.get(short)
        if extra_visit:
            header.extend(extra_visit.strip('\n').splitlines())

    if POST_INVENTORY_VISITS.strip():
        header.append('')
        header.extend(POST_INVENTORY_VISITS.strip('\n').splitlines())
    header.append('')
    header.extend(TAIL.strip('\n').splitlines())
    header.extend(ENTITY_VALUE.strip('\n').splitlines())
    header.append('')

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
    generated = '\n'.join(header)
    dest = options.output.resolve() if options.output else repo / 'Source/System/ContractAudit.h'
    if dest.exists():
        existing = dest.read_text(encoding='utf-8')
        dropped = hand_fields(existing) - hand_fields(generated)
        if dropped:
            sys.stdout.write(''.join(difflib.unified_diff(
                existing.splitlines(True), generated.splitlines(True),
                fromfile=str(dest), tofile='generated')))
            print('refusing overwrite: generated header drops hand fields')
            return 1
    if not header_only:
        for path, original_path, modified, save_original in planned:
            if save_original is not None:
                original_path.parent.mkdir(parents=True, exist_ok=True)
                original_path.write_bytes(save_original)
            path.write_bytes(modified)
    if not generated.endswith('\n'):
        generated += '\n'
    dest.parent.mkdir(parents=True, exist_ok=True)
    dest.write_text(generated, encoding='utf-8', newline='\n')
    if not header_only:
        (root / 'observer-manifest.json').write_text(json.dumps({'status': 'read-only field access instrumentation; opaque fields explicitly reported', 'classes': sorted(selected), 'headers': manifest, 'omitted_method_macros': omitted, 'field_count': sum(map(len, fields.values())), 'observer_sha256': hashlib.sha256(dest.read_bytes()).hexdigest()}, indent=2))
    print(json.dumps({'classes': len(selected), 'fields': sum(map(len, fields.values())), 'out': str(dest), 'header_only': header_only}))
    return 0


def main(argv=None):
    return generate(parse_args(argv))


if __name__ == "__main__":
    raise SystemExit(main())
