"""Select one local peer and join retained capture contracts by live match identity."""

import copy
import hashlib
import json
from pathlib import Path, PurePosixPath, PureWindowsPath


def select_peer(scenario, peer):
    if not scenario.get('cross_machine'):
        raise ValueError('--peer requires a cross-machine scenario')
    selected = copy.deepcopy(scenario)
    names = {node['name'] for run in selected.get('runs', [selected]) for node in run.get('peers', [])}
    if peer not in names:
        raise ValueError(f'No peer named {peer}')
    contract = json.dumps(scenario['checklist'], sort_keys=True).encode()
    selected['peer_selection'] = {'local': peer, 'external': sorted(names - {peer}), 'contract_sha256': hashlib.sha256(contract).hexdigest(), 'scope': 'local peer only; merge both halves before pair review'}
    for run in selected.get('runs', [selected]):
        run['peers'] = [node for node in run['peers'] if node['name'] == peer]
        if not run['peers']:
            raise ValueError('The selected peer is absent from a named run')
    selected['checklist'] = [item for item in selected['checklist'] if not item.get('peer') or item['peer'] == peer]
    return selected


def identity_agrees(identities):
    if len(identities) != 2 or any(not item or not item.get('session_id') or not item.get('round') or not item.get('config_hash') for item in identities):
        return False
    keys = ('session_id', 'round', 'config_hash')
    return all(identities[0].get(key) == identities[1].get(key) for key in keys) and {item.get('host') for item in identities} == {True, False} and len({item.get('peer_id') for item in identities}) == 2


def source_path(value):
    text = str(value).replace('\\', '/')
    if '..' in text.split('/') or '\0' in text:
        raise ValueError(f'Capture path contains traversal: {value}')
    path = PureWindowsPath(text) if PureWindowsPath(text).drive else PurePosixPath(text)
    if not path.is_absolute():
        raise ValueError(f'Capture path is not absolute: {value}')
    return path


class CaptureRelocation:
    path_keys = {'path', 'root', 'out', 'video', 'contact_sheet', 'launch', 'manifest', 'events', 'probe_dir',
                 'stage', 'video_dir', 'runtime', 'directory', 'gameplay_signal', 'reviews', 'trace', 'traces',
                 'identity_file', 'evidence'}
    provenance_keys = {'command', 'args', 'env', 'source', 'record', 'exe', 'scenario_definition', 'script', 'original_path'}

    def __init__(self, original, local):
        self.original = source_path(original)
        self.local = Path(local).resolve()
        self.paths = []
        self.checked = {}

    def path(self, value, field):
        try:
            relative = source_path(value).relative_to(self.original)
        except ValueError as error:
            raise ValueError(f'{field}: path is outside its original capture root: {value}') from error
        if any(':' in part for part in relative.parts):
            raise ValueError(f'{field}: invalid transferred path component: {value}')
        target = self.local.joinpath(*relative.parts).resolve()
        if not target.is_relative_to(self.local):
            raise ValueError(f'{field}: transferred path resolves outside its local capture root: {value}')
        self.paths.append({'field': field, 'original_path': value, 'path': str(target)})
        return str(target)

    def walk(self, value, field='', key=''):
        import e2e_video as driver
        if isinstance(value, str):
            text = value.replace('\\', '/')
            absolute = PureWindowsPath(text).is_absolute() or PurePosixPath(text).is_absolute()
            return self.path(value, field) if key in self.path_keys and absolute else value
        if isinstance(value, list):
            return [self.walk(item, f'{field}[{index}]', key) for index, item in enumerate(value)]
        if not isinstance(value, dict):
            return value
        result = {name: copy.deepcopy(item) if name in self.provenance_keys else self.walk(item, f'{field}.{name}', name)
                  for name, item in value.items()}
        if isinstance(value.get('path'), str):
            if result['path'] == value['path']:
                result['path'] = self.path(value['path'], f'{field}.path')
            result['original_path'] = value['path']
            if value.get('sha256'):
                target = result['path']
                if target not in self.checked:
                    self.checked[target] = driver.file_evidence(target)
                actual = self.checked[target]
                if actual.get('sha256') != value['sha256']:
                    raise ValueError(f'{field}: missing or changed transferred file: {value["path"]}')
        return result


def record_auxiliary_evidence(capture, peers):
    import e2e_video as driver
    for peer in peers:
        run = next(run for run in capture['runs'] if run['name'] == peer['run'])
        original = next(row for row in run['peers'] if row['peer'] == peer['peer'])
        peer['identity_file'] = driver.file_evidence(Path(original['probe_dir']) / 'match-identity.json')
        args = original.get('args', [])
        trace = args[args.index('-out') + 1] if '-out' in args and args.index('-out') + 1 < len(args) else None
        peer['trace'] = driver.file_evidence(trace) if trace else {'exists': False, 'reason': 'No trace output was declared'}
    return {'schema': 1, 'required': ['identity_file', 'trace']}


def merge_halves(roots, out):
    import e2e_video as driver
    out = Path(out).resolve()
    out.mkdir(parents=True, exist_ok=False)
    documents, manifests, sources, identities, selections, relocations, provenance, coverage = [], [], [], [], [], [], [], []
    findings = []
    for root in map(Path, roots):
        root = root.resolve()
        capture = json.loads((root / 'capture.json').read_text(encoding='utf-8'))
        if capture['scenario'] != 'mp-host-join-cross':
            raise ValueError('Both halves must be mp-host-join-cross captures')
        selection = capture['scenario_definition'].get('peer_selection', {})
        selections.append(selection)
        original = capture.get('out', str(root))
        raw_document = json.loads((root / 'review.json').read_text(encoding='utf-8'))
        raw_manifest = json.loads((root / 'manifest.json').read_text(encoding='utf-8'))
        provenance.append({'original_root': original, 'local_root': str(root),
                           'files': [driver.file_evidence(root / leaf) for leaf in ('capture.json', 'manifest.json', 'review.json')],
                           'command': raw_document.get('command', capture.get('command'))})
        try:
            relocation = CaptureRelocation(original, root)
            document = relocation.walk(raw_document, 'review')
            manifest = relocation.walk(raw_manifest, 'manifest')
            relocations.append({'original_root': original, 'local_root': str(root), 'paths': relocation.paths})
        except ValueError as error:
            finding = {'class': 'harness', 'reason': str(error), 'capture': str(root)}
            findings.append(finding)
            document = {'checklist': [{**item, 'video': None, 'contact_sheet': None, 'frames': None, 'probe': 'fail', 'finding': finding}
                                      for item in raw_document.get('checklist', [])]}
            manifest = {**raw_manifest, 'peers': []}
        documents.append(document)
        manifests.append(manifest)
        sources.append(str(root / 'capture.json'))
        peers = manifest.get('peers', [])
        identities.append(peers[0].get('match_identity') if len(peers) == 1 else None)
    same_contract = {item.get('local') for item in selections} == {'host', 'client'} and len({item.get('contract_sha256') for item in selections}) == 1 and all(item.get('contract_sha256') for item in selections)
    agreed = same_contract and identity_agrees(identities)
    match = f"{identities[0]['session_id']:016x}-{identities[0]['round']:016x}" if agreed else None
    platforms = {selection.get('local'): manifest.get('platform') for selection, manifest in zip(selections, manifests)}
    cross_platform = platforms.get('host') == 'win32' and platforms.get('client') == 'darwin'
    gate = {'status': 'PASS' if agreed else 'FAIL', 'match_id': match, 'identities': identities, 'same_contract': same_contract, 'halves': sources, 'platforms': platforms, 'cross_platform': cross_platform}
    findings += [finding for document in documents for finding in document.get('run_findings', [])]
    if not agreed:
        findings.append({'class': 'harness', 'reason': 'Capture halves do not name one shared live match, round and contract', 'evidence': gate})
    peers = [peer for manifest in manifests for peer in manifest.get('peers', [])]
    for manifest in manifests:
        for peer in manifest.get('peers', []):
            auxiliary = ('identity_file', 'trace')
            required = set(manifest.get('cross_auxiliary', {}).get('required', []))
            coverage.append({'peer': peer['peer'], 'required': sorted(required),
                             'recorded': [kind for kind in auxiliary if peer.get(kind, {}).get('sha256')],
                             'missing_historical_checksums': [kind for kind in auxiliary if kind not in required and not peer.get(kind, {}).get('sha256')]})
            for kind in ('video', 'contact_sheet', *auxiliary):
                saved = peer.get(kind)
                if kind in auxiliary and kind not in required and saved is None:
                    continue
                if not saved or not saved.get('path') or not saved.get('sha256') or not Path(saved['path']).is_file() or driver.file_evidence(saved['path']).get('sha256') != saved['sha256']:
                    findings.append({'class': 'harness', 'peer': peer['peer'], 'reason': f'Missing or changed {kind} in transferred capture', 'evidence': saved})
                elif kind == 'identity_file':
                    native = json.loads(Path(saved['path']).read_text(encoding='utf-8'))
                    if native != peer.get('match_identity'):
                        findings.append({'class': 'harness', 'peer': peer['peer'], 'reason': 'Native identity file differs from the recorded match identity', 'evidence': saved})
    items = [item for document in documents for item in document['checklist']]
    for item in items:
        peer = next((peer for peer in peers if peer['peer'] == item.get('peer')), None)
        for kind in ('video', 'contact_sheet'):
            if item.get(kind) and (not peer or item[kind] != (peer.get(kind) or {}).get('path')):
                item['finding'] = {'class': 'harness', 'reason': f'Review {kind} does not reference its verified peer media'}
        if not agreed:
            item['finding'] = {'class': 'harness', 'reason': 'The pair identity gate failed; this is an unrelated or incomplete half'}
    manifest = {'schema': 1, 'scenario': 'mp-host-join-cross', 'source_captures': sources, 'sources': [value['source'] for value in manifests], 'frame_count': sum(value['frame_count'] for value in manifests), 'peers': peers, 'match_identity_gate': gate, 'finished': driver.stamp(), 'findings': findings,
                'relocations': relocations, 'provenance': provenance, 'auxiliary_checksum_coverage': coverage}
    document = {'schema': 1, 'scenario': 'mp-host-join-cross', 'verdict': 'agent-review-required' if cross_platform else 'local-pair-validation-only', 'manifest': str(out / 'manifest.json'), 'checklist': items, 'run_findings': findings, 'match_identity_gate': gate, 'reviews': [str(Path(root).resolve() / 'review.json') for root in roots],
                'relocations': relocations, 'provenance': provenance, 'auxiliary_checksum_coverage': coverage}
    driver.write_json(out / 'manifest.json', manifest)
    driver.write_json(out / 'review.json', document)
    # The assert-dialog row reads the peer's log, not frames: its own probe verdict decides it.
    return not findings and all((item.get('frames') is not None or item.get('state') == 'checked') and not item.get('finding')
                                and item.get('probe') not in ('fail', 'not-reached', 'not-run') for item in items)
