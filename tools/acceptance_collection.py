"""Receipts for declared acceptance steps outside run_split; this is not a share scheduler.

The RUN chain starts one full schedule before any row, wraps each existing driver,
and collects the completed receipts. POSIX streams use begin/finish around their
own commands and retain paths relative to their share for lossless fetching.
"""
import argparse
import importlib
import json
import os
from pathlib import Path
import re
import subprocess
import sys
import uuid

from verdict_artifact import sha256


def write(path, value):
    path = Path(path); path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(json.dumps(value, indent=2) + '\n', encoding='utf-8')


def read(path):
    return json.loads(Path(path).read_text(encoding='utf-8-sig'))


def inventory(path):
    sys.path.insert(0, str(Path(path).resolve()))
    return importlib.import_module('acceptance_manifest'), importlib.import_module('extract_defects')


def start(root, plan_path, schedule_path, source, exe, ledger, inventory_root):
    reader, _ = inventory(inventory_root)
    from run_split import register_collection
    root = Path(root).resolve()
    plan, schedule = read(plan_path), read(schedule_path)
    if schedule.get('source_sha') != source or plan['generated']['head'] != source:
        raise ValueError('predeclared plan/schedule source differs from the acceptance tip')
    if (root/'split-plan.json').exists(): raise ValueError('this root already has an acceptance start; use a new run root')
    root.mkdir(parents=True, exist_ok=True)
    run_id = uuid.uuid4().hex
    sequence = register_collection(Path(ledger), run_id, source, root)
    schedule.update(collection_id=run_id, source_sha=source, exe_sha256=exe, sequence=sequence, written=reader.stamp())
    write(root/'split-plan.json', schedule)
    write(root/'acceptance-plan.json', plan)
    return schedule


def start_section(root, section, *, marker=None, inventory_root=None, optional_boxes=None):
    """Freeze this section's authorization before any of its commands start."""
    root = Path(root).resolve()
    if inventory_root is not None:
        inventory(inventory_root)
    import run_split
    plan, schedule = read(root/'acceptance-plan.json'), read(root/'split-plan.json')
    path = root/'sections'/f'{section}.json'
    if path.exists() or str(section) in schedule.get('section_receipts', {}):
        raise ValueError(f'section {section} already started; its window choice cannot be replaced')
    rows = [row for row in plan['rows'] if row.get('section') == section]
    if not rows:
        raise ValueError(f'section {section} has no declared rows')
    if not schedule.get('collection_id') or not schedule.get('sequence'):
        raise ValueError('section has no collection start')
    window = run_split.window_variant(marker)
    capabilities = {}
    ref = root/'capabilities/edith-readback.json'
    if any(row.get('blocked_reason') for row in rows):
        try:
            proof = read(ref)
            passed = (proof.get('pass') is True and proof.get('box') == 'EDITH' and proof.get('engine_row') == 'A57.2'
                      and proof.get('source_sha') == schedule['source_sha'] and proof.get('exe_sha256') == schedule.get('exe_sha256'))
            capabilities['EDITH.readback'] = dict(passed=passed, reference=dict(path='capabilities/edith-readback.json', sha256=sha256(ref)))
        except (OSError, ValueError):
            capabilities['EDITH.readback'] = dict(passed=False)
    if optional_boxes is None:
        optional_boxes = {}
        if inventory_root is not None:
            boxes, _ = run_split.load_manifest(Path(inventory_root)/'boxes.json')
            participating = {box for row in rows for box, count in row.get('engine_boxes', {}).items() if count}
            optional_boxes = run_split.probe_optional_boxes([box for box in boxes if box.name in participating])
    decisions = []
    for row in rows:
        reason = ''
        if row.get('window_required') and window['variant'] == 'WITHOUT':
            reason = run_split.WINDOW_REASON
        elif row.get('blocked_reason') and not capabilities.get('EDITH.readback', {}).get('passed'):
            reason = row['blocked_reason']
        else:
            absent = sorted(box for box, count in row.get('engine_boxes', {}).items() if count and optional_boxes.get(box) is False)
            if absent:
                reason = ', '.join(absent) + ' absent: required game peer deferred'
        decisions.append(dict(share=row['share'], id=row['id'], state='AWAITING' if reason else 'READY', reason=reason))
    document = dict(schema=1, section=section, collection_id=schedule['collection_id'], source_sha=schedule['source_sha'],
                    window=window, capabilities=capabilities, optional_boxes=optional_boxes, rows=decisions, started=run_split.stamp())
    write(path, document)
    schedule.setdefault('section_receipts', {})[str(section)] = dict(path=f'sections/{section}.json', sha256=sha256(path))
    write(root/'split-plan.json', schedule)
    return document


class Share:
    def __init__(self, root, share, inventory_root, *, share_root=None, plan=None, schedule=None):
        self.root = Path(root).resolve()
        self.reader, self.extractor = inventory(inventory_root)
        self.plan = read(plan or self.root/'acceptance-plan.json')
        self.schedule = read(schedule or self.root/'split-plan.json')
        self.label = share
        self.out = Path(share_root).resolve() if share_root else self.root/share
        self.out.mkdir(parents=True, exist_ok=True)
        shares = [row for row in self.schedule['shares'] if row['label'] == share]
        if len(shares) != 1: raise ValueError(f'share {share!r} is absent or repeated in the declared schedule')
        self.declaration = shares[0]
        self.rows = {row['id']: row for row in self.plan['rows'] if row['share'] == share}
        if set(self.rows) != set(self.declaration['ids']): raise ValueError('share commands differ from the predeclared plan')
        self.run_id, self.source = self.schedule['collection_id'], self.schedule['source_sha']
        if not self.run_id or not self.schedule.get('sequence'): raise ValueError('the acceptance collection has no sequence-ledger start')

    def receipt_dir(self, command_id):
        if command_id not in self.rows or not re.fullmatch(r'[A-Za-z0-9_.-]+', command_id):
            raise ValueError(f'undeclared or unsafe command id: {command_id!r}')
        return self.out/'commands'/command_id

    def product(self, command_id):
        value = self.rows[command_id]['product']['path']
        prefix = self.label + '/'
        if not value.startswith(prefix): raise ValueError(f'product is outside its declared share: {value}')
        relative = Path(value[len(prefix):])
        target = (self.out/relative).resolve()
        if not target.is_relative_to(self.out): raise ValueError('declared product escapes its share')
        return target

    def begin(self, command_id, argv):
        directory = self.receipt_dir(command_id)
        path = directory/'command.json'
        if path.exists(): raise ValueError(f'command already started: {command_id}; retain it and use a new collection')
        product = self.product(command_id)
        write(path, dict(schema=1, id=command_id, share=self.label, collection_id=self.run_id, source_sha=self.source,
            box=self.declaration['box'], sequence=self.schedule['sequence'], attempt=1, argv=argv,
            products=[product.relative_to(self.out).as_posix(), (directory/'collection-error.json').relative_to(self.out).as_posix()],
            started=self.reader.stamp()))
        return path

    def defer(self, command_id):
        spec = self.rows[command_id]
        choice = self.reader.section_choice(spec, self.schedule, self.reader.Evidence(self.root))
        if not choice or not choice['reason']:
            raise ValueError(f'{command_id}: no section deferral authorizes withholding this row')
        directory = self.receipt_dir(command_id)
        if (directory/'command.json').exists() or (directory/'terminal.json').exists():
            raise ValueError(f'{command_id}: already started or completed; cannot defer it')
        entry = dict(id=command_id, state='awaiting', exit_code=3, reason=choice['reason'], engine_started=False,
                     section_receipt=choice['reference'], collection_id=self.run_id, source_sha=self.source)
        write(directory/'terminal.json', entry)
        return entry

    def finish(self, command_id, code, log, reason='', *, not_run=False):
        directory = self.receipt_dir(command_id)
        command = read(directory/'command.json')
        if command.get('collection_id') != self.run_id or command.get('source_sha') != self.source:
            raise ValueError('command start belongs to another collection/source')
        product = self.product(command_id)
        log = Path(log)
        native = None
        try:
            if product.stat().st_size > self.reader.MAX_JSON_BYTES: raise ValueError('native product exceeds the declared JSON read limit')
            native = read(product)
        except (OSError, ValueError) as error:
            reason = reason or f'promised product {product}: {type(error).__name__}: {error}'
            product = directory/'collection-error.json'
            write(product, dict(schema=1, passed=False, status='FAIL', failure_class='harness', reason=reason,
                promised_product=str(self.product(command_id)), engine_started=False if not_run else None,
                counts=dict(executed=int(not not_run), failed=1), log=str(log),
                input_sha256={str(directory/'command.json'): sha256(directory/'command.json')},
                source_sha=self.source, collection_id=self.run_id))
        actual_code = code
        if code in (0, 3) and (native is None or self.extractor.file_verdict(native, product.name) is False): code = 1
        entry = dict(id=command_id, driver=self.rows[command_id].get('driver', self.rows[command_id].get('runner')),
            state='done', exit_code=code, actual_exit_code=actual_code, reason=reason, dir='.', attempt=1,
            product=dict(path=product.relative_to(self.out).as_posix()),
            command_receipt=(directory/'command.json').relative_to(self.out).as_posix(),
            command_sha256=sha256(directory/'command.json'), log=str(log), log_sha256=sha256(log) if log.is_file() else None,
            collection_id=self.run_id, source_sha=self.source, ended=self.reader.stamp())
        write(directory/'terminal.json', entry)
        return entry

    def import_progress(self, path):
        progress = read(path)
        if progress.get('collection_id') != self.run_id or progress.get('source_sha', progress.get('head')) != self.source:
            raise ValueError('imported stream belongs to another collection/source')
        for row in progress['commands']:
            if row['id'] not in self.rows: raise ValueError(f'undeclared imported command {row["id"]}')
            entry = dict(row)
            attempts = entry.get('attempts') or [dict(entry, attempt=1, product=dict(path=str(self.product(row['id']))))]
            for attempt in attempts:
                for key in ('dir', 'command_receipt'):
                    if attempt.get(key): attempt[key] = Path(attempt[key]).resolve().relative_to(self.out).as_posix()
                if attempt.get('product'): attempt['product']['path'] = Path(attempt['product']['path']).resolve().relative_to(self.out).as_posix()
                cause = attempt.get('harness_condition') or {}
                if cause.get('evidence'): cause['evidence']['path'] = Path(cause['evidence']['path']).resolve().relative_to(self.out).as_posix()
            entry.update(dir=Path(row['dir']).resolve().relative_to(self.out).as_posix(), attempts=attempts,
                         collection_id=self.run_id, source_sha=self.source)
            write(self.receipt_dir(row['id'])/'terminal.json', entry)

    def collect(self):
        commands, hashes, missing = [], {}, []
        for command_id in self.declaration['ids']:
            path = self.receipt_dir(command_id)/'terminal.json'
            if not path.is_file(): missing.append(command_id); continue
            entry = read(path)
            if entry.get('collection_id') != self.run_id or entry.get('source_sha') != self.source:
                raise ValueError(f'{command_id}: terminal receipt belongs to another collection/source')
            if entry.get('state') == 'awaiting':
                choice = self.reader.section_choice(self.rows[command_id], self.schedule, self.reader.Evidence(self.root))
                if not choice or choice['reason'] != entry.get('reason'):
                    raise ValueError(f'{command_id}: invalid deferred terminal')
                commands.append(entry)
                continue
            for attempt in entry.get('attempts') or [entry]:
                product = (self.out/attempt['product']['path']).resolve()
                if not product.is_relative_to(self.out): raise ValueError('collected product escapes its share')
                hashes[product.relative_to(self.out).as_posix()] = sha256(product)
            commands.append(entry)
        native_roots = {self.out/self.product(command_id).relative_to(self.out).parts[0] for command_id in self.rows}
        receipt_root = self.out/'commands'
        scans = []
        for directory in sorted(native_roots | {receipt_root}):
            if not directory.is_dir(): continue
            scan = self.extractor.Extractor(directory, True, None).run()
            if directory == receipt_root:
                # This directory declares launches and retains logs; native verdicts are required at the plan's product paths.
                scan['collection_errors'] = [row for row in scan['collection_errors'] if row['reason'] != 'no readable verdict evidence']
                scan['collection_complete'] = not scan['collection_errors']
            scans.append(scan)
        scanned = dict(schema=1, root=str(self.out), collection_complete=bool(scans) and all(row['collection_complete'] for row in scans),
            collection_errors=[item for row in scans for item in row['collection_errors']],
            hard_count=sum(row['hard_count'] for row in scans), defect_count=sum(row['defect_count'] for row in scans),
            defects=[item for row in scans for item in row['defects']], observations=[item for row in scans for item in row['observations']],
            awaiting_review=[item for row in scans for item in row.get('awaiting_review', [])],
            evidence_sha256={key: value for row in scans for key, value in row['evidence_sha256'].items()})
        if not scans and commands and all(entry.get('state') == 'awaiting' for entry in commands):
            scanned['collection_complete'] = True
        scanned.update(schema=1, path_basis='collection', collection_id=self.run_id, source_sha=self.source,
            box=self.declaration['box'], sequence=self.schedule['sequence'], commands=commands,
            collection_complete=not missing and scanned['collection_complete'], missing_commands=missing,
            evidence_sha256={**{Path(key).resolve().relative_to(self.out).as_posix(): value
                               for key, value in scanned.get('evidence_sha256', {}).items()}, **hashes})
        for entry in commands:
            if entry.get('state') == 'awaiting':
                scanned['awaiting_review'].append(dict(id=entry['id'], state='AWAITING', reason=entry['reason']))
                continue
            spec = self.rows[entry['id']]
            if not spec.get('review') or entry.get('exit_code') not in (0, 3): continue
            state, reason = self.reader.review_state(spec['review'], entry['id'], self.source, self.reader.Evidence(self.root))
            if state != 'APPROVED':
                scanned['awaiting_review'].append(dict(id=entry['id'], state=state, reason=reason, reference=spec['review']))
        write(self.out/'DEFECTS.json', scanned)
        identity_paths = set()
        for spec in self.rows.values():
            for ref in spec.get('identities', []):
                value = ref['path'] if isinstance(ref, dict) else ref
                path = self.out/value[len(self.label)+1:] if value.startswith(self.label+'/') else self.root/value
                if path.is_file(): identity_paths.add(path.resolve())
        identities = [dict(path=Path(os.path.relpath(path, self.out)).as_posix(), sha256=sha256(path)) for path in sorted(identity_paths)]
        code = 1 if missing or scanned['hard_count'] or any(row.get('exit_code') not in (0, 3) for row in commands) else 3 if scanned.get('awaiting_review') or any(row.get('exit_code') == 3 for row in commands) else 0
        write(self.out/'terminal.json', dict(schema=1, label=self.label, box=self.declaration['box'], ids=self.declaration['ids'],
            collection_id=self.run_id, source_sha=self.source, sequence=self.schedule['sequence'], exit_code=code,
            state='done' if code == 0 else 'pending' if code == 3 else 'failed',
            collection=dict(path='DEFECTS.json', sha256=sha256(self.out/'DEFECTS.json')), identities=identities,
            products=hashes, ended=self.reader.stamp()))
        return scanned

    def fail_missing(self, reason, log):
        for command_id in self.declaration['ids']:
            directory = self.receipt_dir(command_id)
            if (directory/'terminal.json').is_file(): continue
            choice = self.reader.section_choice(self.rows[command_id], self.schedule, self.reader.Evidence(self.root))
            if choice and choice['reason']:
                self.defer(command_id)
                continue
            if not (directory/'command.json').is_file(): self.begin(command_id, [])
            self.finish(command_id, 1, log, reason, not_run=True)


def resolved_commands(spec, root, repo, here, exe_sha256=''):
    directory = root/spec['share']/spec['id']
    replacements = dict(PY=sys.executable, ROOT=str(root), REPO=str(repo), HERE=str(here), DIR=str(directory), RUN_NAME=root.name, EXE=exe_sha256)
    commands = spec.get('argv') or []
    commands = commands if commands and isinstance(commands[0], list) else [commands]
    return [[re.sub(r'\{(PY|ROOT|REPO|HERE|DIR|RUN_NAME|EXE)\}', lambda match: replacements[match[1]], str(value)) for value in command]
            for command in commands if command]


def run(share, command_id, repo, here):
    spec = share.rows[command_id]
    choice = share.reader.section_choice(spec, share.schedule, share.reader.Evidence(share.root))
    if choice and choice['reason']:
        share.defer(command_id)
        return 3
    if spec.get('window_required'):
        import run_split
        if run_split.window_variant()['variant'] != 'WITH':
            raise ValueError(f'{command_id}: EROL-PC window was withdrawn before launch')
    commands = resolved_commands(spec, share.root, repo, here, share.schedule.get('exe_sha256', ''))
    share.begin(command_id, commands)
    log = share.receipt_dir(command_id)/'driver-stdout.log'
    code = 0
    if not commands:
        log.write_text('no executable driver is declared for this owner row\n', encoding='utf-8')
        return share.finish(command_id, 1, log, 'owner driver is not available', not_run=True)['exit_code']
    (share.root/share.label/command_id).mkdir(parents=True, exist_ok=True)
    with log.open('w', encoding='utf-8') as output:
        for command in commands:
            output.write(json.dumps(command) + '\n'); output.flush()
            try:
                result = subprocess.run(command, cwd=repo, stdout=output, stderr=subprocess.STDOUT,
                                        env=dict(os.environ, CCCP_HEADLESS='1', PYTHONDONTWRITEBYTECODE='1'))
                if result.returncode and not code: code = result.returncode
            except OSError as error:
                output.write(f'{type(error).__name__}: {error}\n'); code = code or 1
    return share.finish(command_id, code, log)['exit_code']


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('action', choices=('start', 'section', 'defer', 'begin', 'finish', 'run', 'collect', 'fail-missing', 'import-progress'))
    parser.add_argument('--root', type=Path, required=True)
    parser.add_argument('--inventory', type=Path, required=True)
    parser.add_argument('--plan', type=Path)
    parser.add_argument('--schedule', type=Path)
    parser.add_argument('--share-root', type=Path)
    parser.add_argument('--share')
    parser.add_argument('--id')
    parser.add_argument('--source-sha')
    parser.add_argument('--exe-sha256')
    parser.add_argument('--sequence-ledger', type=Path)
    parser.add_argument('--exit-code', type=int)
    parser.add_argument('--log', type=Path)
    parser.add_argument('--reason', default='')
    parser.add_argument('--progress', type=Path)
    parser.add_argument('--repo', type=Path)
    parser.add_argument('--here', type=Path)
    parser.add_argument('--argv-json', default='[]')
    parser.add_argument('--section', type=int)
    parser.add_argument('--window-marker', type=Path)
    options = parser.parse_args(argv)
    if options.action == 'start':
        document = start(options.root, options.plan, options.schedule, options.source_sha, options.exe_sha256,
                         options.sequence_ledger, options.inventory)
        print(document['collection_id']); return 0
    if options.action == 'section':
        document = start_section(options.root, options.section, marker=options.window_marker, inventory_root=options.inventory)
        print(f'section {options.section}: window {document["window"]["variant"]}; '
              f'{sum(row["state"] == "AWAITING" for row in document["rows"])} deferred')
        return 0
    share = Share(options.root, options.share, options.inventory, share_root=options.share_root, plan=options.plan, schedule=options.schedule)
    if options.action == 'defer': share.defer(options.id)
    elif options.action == 'begin': share.begin(options.id, json.loads(options.argv_json))
    elif options.action == 'finish': share.finish(options.id, options.exit_code, options.log, options.reason)
    elif options.action == 'run': return run(share, options.id, options.repo.resolve(), options.here.resolve())
    elif options.action == 'import-progress': share.import_progress(options.progress)
    elif options.action == 'fail-missing': share.fail_missing(options.reason, options.log)
    else:
        document = share.collect()
        print(f'{share.label}: {len(document["commands"])}/{len(share.declaration["ids"])} commands collected')
        return 0 if document['collection_complete'] else 1
    return 0


if __name__ == '__main__':
    raise SystemExit(main())
