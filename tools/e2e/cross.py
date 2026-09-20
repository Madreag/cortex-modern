"""Select one local peer and join retained capture contracts by live match identity."""

import copy
import hashlib
import json
from pathlib import Path


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


def rebase_paths(value, old, new):
    old = str(old).replace('\\', '/').rstrip('/')
    if isinstance(value, str):
        normalized = value.replace('\\', '/')
        return str(Path(new) / normalized[len(old) + 1:]) if normalized.startswith(old + '/') else value
    if isinstance(value, list):
        return [rebase_paths(item, old, new) for item in value]
    if isinstance(value, dict):
        return {key: item if key in ('command', 'args', 'env', 'source', 'record') else rebase_paths(item, old, new) for key, item in value.items()}
    return value


def merge_halves(roots, out):
    import e2e_video as driver
    out = Path(out).resolve()
    out.mkdir(parents=True, exist_ok=False)
    documents, manifests, sources, identities, selections = [], [], [], [], []
    for root in map(Path, roots):
        root = root.resolve()
        capture = json.loads((root / 'capture.json').read_text(encoding='utf-8'))
        if capture['scenario'] != 'mp-host-join-cross':
            raise ValueError('Both halves must be mp-host-join-cross captures')
        selection = capture['scenario_definition'].get('peer_selection', {})
        selections.append(selection)
        original = capture.get('out', str(root))
        document = rebase_paths(json.loads((root / 'review.json').read_text(encoding='utf-8')), original, root)
        manifest = rebase_paths(json.loads((root / 'manifest.json').read_text(encoding='utf-8')), original, root)
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
    findings = [finding for document in documents for finding in document.get('run_findings', [])]
    if not agreed:
        findings.append({'class': 'harness', 'reason': 'Capture halves do not name one shared live match, round and contract', 'evidence': gate})
    peers = [peer for manifest in manifests for peer in manifest.get('peers', [])]
    for peer in peers:
        for kind in ('video', 'contact_sheet'):
            saved = peer.get(kind)
            if not saved or not Path(saved['path']).is_file() or driver.file_evidence(saved['path']).get('sha256') != saved.get('sha256'):
                findings.append({'class': 'harness', 'peer': peer['peer'], 'reason': f'Missing or changed {kind} in transferred capture', 'evidence': saved})
    items = [item for document in documents for item in document['checklist']]
    for item in items:
        if not agreed:
            item['finding'] = {'class': 'harness', 'reason': 'The pair identity gate failed; this is an unrelated or incomplete half'}
    manifest = {'schema': 1, 'scenario': 'mp-host-join-cross', 'source_captures': sources, 'sources': [value['source'] for value in manifests], 'frame_count': sum(value['frame_count'] for value in manifests), 'peers': peers, 'match_identity_gate': gate, 'finished': driver.stamp(), 'findings': findings}
    document = {'schema': 1, 'scenario': 'mp-host-join-cross', 'verdict': 'agent-review-required' if cross_platform else 'local-pair-validation-only', 'manifest': str(out / 'manifest.json'), 'checklist': items, 'run_findings': findings, 'match_identity_gate': gate, 'reviews': [str(Path(root).resolve() / 'review.json') for root in roots]}
    driver.write_json(out / 'manifest.json', manifest)
    driver.write_json(out / 'review.json', document)
    return not findings and all(item.get('frames') is not None and not item.get('finding') and item.get('probe') not in ('fail', 'not-reached', 'not-run') for item in items)
