"""Hold this fork's own files to what a public repository may carry.

Scans every tracked file the fork added or changed since its upstream base (for an upstream file, only the lines the
fork added) - its contents and its path - for seven classes of text:

    tool-name      the name of a coding tool, a model or its vendor
    internal-name  a private folder, document, branch or scratch root of the maintainer's own workflow
    process-tag    a ticket-style tag of that workflow (worker, finding, ruling, note and row numbers)
    machine-name   a name, alias or hostname of one of the maintainer's machines, read from the box file
    user-name      a user name on those machines, read from the box file
    abs-path       an absolute path on a developer's machine: a drive-letter path outside the system folders or a
                   home directory, unless it is one of the box file example's neutral paths
    ipv4           an IPv4 address other than loopback, unspecified, a documentation address, a special range's own
                   prefix with a host number in its last octet, or an entry of ALLOWED_IPV4

The machine and user names come from the box file (tools/box_facts.py). Without one, the scan uses the tracked
example's neutral names and says the two classes were not checked against real machines.

    python tools/scan_public_tree.py [--base SHA] [--show] [--json OUT]
    python tools/scan_public_tree.py --self-test

Exit 0 with no hits, 1 with hits, 2 when the scan cannot run (for example a clone without the upstream base).
"""
from __future__ import annotations

import argparse
from collections import Counter
from dataclasses import dataclass
import ipaddress
import json
from pathlib import Path
import re
import subprocess
import sys
import tempfile

import box_facts

HERE = Path(__file__).resolve().parent
# Upstream's development branch where this fork's history starts.
UPSTREAM_BASE = '20dfb3ea57c916e94fdeb94a5cf871c80fd5fc36'
CLASSES = ('tool-name', 'internal-name', 'process-tag', 'machine-name', 'user-name', 'abs-path', 'ipv4')
# Third-party sources keep their own text.
EXCLUDED_ROOTS = ('external/',)
EDGE_L, EDGE_R = r'(?<![A-Za-z0-9])', r'(?![A-Za-z0-9])'


def words(*pieces: str) -> list[str]:
    """Words assembled at run time, so this file never spells what it hunts."""
    return [''.join(piece.split('+')) for piece in pieces]


def alternation(items: list[str]) -> str:
    return '|'.join(sorted(items, key=len, reverse=True))


TOOL_NAMES = words('cla+ude', 'anthr+opic', 'fa+ble', 'son+net', 'hai+ku', 'gr+ok', 'co+dex', 'open+ai', 'chat+gpt',
                   'as+tra', 'ki+mi', 'moon+shot', 'de+vin', 'co+pilot', 'gem+ini', 'deep+seek', 'q+wen', 'op+us')
TOOL_PATTERNS = [EDGE_L + '(?:' + alternation(TOOL_NAMES) + ')' + EDGE_R, EDGE_L + 'g' 'pt-?[0-9]',
                 EDGE_L + 's' 'we-2' + EDGE_R, EDGE_L + 'cur' 'sor[-_ ](?:agent|cli)' + EDGE_R]
INTERNAL_NAMES = words('cli+_runs', 'lead+-tools', 'lead+_tools', 'reviews/+takeover', 'takeover-+2026', 'fix+group',
                       'wave+-a', 'alpha/+v1', 'stage+2/', 'flag+ship/', 'live+-lanes', 'RE+PORT-final', 'inventory-+confirming',
                       'mx/+session1')
# The private documents, by the capitalised names they go by.
INTERNAL_DOCUMENTS = [name + '.md' for name in words('AGE+NTS', 'RES+UME', 'STA+TUS', 'ROLL+BACK', 'BOX+ES', 'HAND+OFF',
                      'MAC_+RESUME', 'V1-+GAPS', 'V1-+PLAN', 'V1-+ACCEPTANCE')] + \
                     words('RULE+BOOK', 'SPAWN+_LOG', 'LEAD-+REVIEW', 'LEAD_+PLAN', 'WORKER+_RULES', '_RUN+BOOK',
                           'ENGINE-+FOLLOWUPS', 'INTEGRATION+_33', 'STAGE2_+H4', 'SEAT-+ROSTER', 'REJOIN-+GRID', 'SWARM-+COMMON')
INTERNAL_PATTERNS = ['(?i)' + EDGE_L + '(?:' + alternation([re.escape(name) for name in INTERNAL_NAMES]) + ')',
                     EDGE_L + '(?:' + alternation([re.escape(name) for name in INTERNAL_DOCUMENTS]) + ')',
                     r'(?i)' + EDGE_L + r'[a-z]:[/\\]+mx' + EDGE_R]
TAG_PATTERNS = [
    EDGE_L + r'(?:ENGINE|NOTE|ADDENDUM|RESUME|RULING)\s+[0-9]+' + EDGE_R,
    EDGE_L + r'[Rr]ows?\s+[0-9]{3}' + EDGE_R,
    EDGE_L + r'(?:[Mm]erge|[Ll]anding)\s+[0-9]{3}' + EDGE_R,
    EDGE_L + r'W[0-9]{2,3}' + EDGE_R,
    EDGE_L + r'F(?:1[3-9]|[2-9][0-9]|[0-9]{3})[a-z]?(?:-[A-Z])?' + EDGE_R,
    EDGE_L + r'A[0-9]{2}\.[0-9]+',
    EDGE_L + r'R-[A-Z]{1,2}' + EDGE_R,
    EDGE_L + r'(?:CF|HL|RT|DS|LN|RD|MW|PK)[0-9]' + EDGE_R,
    EDGE_L + r'L' r'4P(?:-[A-Z])?' + EDGE_R,
    EDGE_L + r'L[0-9]{2}' + EDGE_R,
    EDGE_L + r'Source[0-9]{2}' + EDGE_R,
    EDGE_L + r'R[0-9](?:F[0-9]+|D[0-9]+|WAY[0-9]+|-[0-9]{3})',
    # A ruling named by its letters, and a numbered rule of the maintainer's own rule list cited in parentheses.
    r'(?i)' + EDGE_L + r'rulings?\s+(?:[a-z]{1,4}|[A-Z]?[0-9]{1,2}(?:\.[0-9]+)?)' + EDGE_R,
    EDGE_L + r'A(?:1[0-6]|[0-9])' + EDGE_R + r'(?=[:)])',
]
DRIVE_PATH = re.compile(r'(?<![A-Za-z0-9])([A-Za-z]):[/\\]{1,2}([^/\\"\'`\s,;:)\]}*?<>|]+)')
HOME_PATH = re.compile(r'(?:(?<![A-Za-z0-9])[A-Za-z]:[/\\]{1,2}Users[/\\]{1,2}|(?<![A-Za-z0-9.~])/(?:Users|home)/)([A-Za-z0-9._-]+)')
IPV4 = re.compile(r'(?<![0-9.])([0-9]{1,3})\.([0-9]{1,3})\.([0-9]{1,3})\.([0-9]{1,3})(?![0-9])(?!\.[0-9])')
DOCUMENTATION = [ipaddress.ip_network(net) for net in ('192.0.2.0/24', '198.51.100.0/24', '203.0.113.0/24')]
# Ranges whose own prefix, with a host number in the last octet, a test may use to stand for "an address in this range".
SPECIAL_RANGES = [ipaddress.ip_network(net) for net in ('10.0.0.0/8', '172.16.0.0/12', '192.168.0.0/16', '100.64.0.0/10',
                                                         '169.254.0.0/16')]
# Literals that are not addresses, or a public service's published address a tool truly needs, each with its reason.
ALLOWED_IPV4 = {
    '4.4.3.1': "a version number (Allegro 4.4.3.1), not an address",
    '4.2.3.1': "a version number, not an address",
    '0.9.9.8': "a version number, not an address",
    '239.255.255.250': "the SSDP multicast group UPnP discovery sends to",
    '8.8.8.8': "a public DNS resolver a route probe aims at (no packet is sent)",
    '1.1.1.1': "a public DNS resolver a route probe aims at (no packet is sent)",
}
# Cloudflare's published IPv4 ranges (https://www.cloudflare.com/ips-v4): the relay checks classify routes by them.
CLOUDFLARE_RANGES = ('173.245.48.0/20', '103.21.244.0/22', '103.22.200.0/22', '103.31.4.0/22', '141.101.64.0/18',
                     '108.162.192.0/18', '190.93.240.0/20', '188.114.96.0/20', '197.234.240.0/22', '198.41.128.0/17',
                     '162.158.0.0/15', '104.16.0.0/13', '104.24.0.0/14', '172.64.0.0/13', '131.0.72.0/22')
for _net in CLOUDFLARE_RANGES:
    ALLOWED_IPV4[_net.split('/')[0]] = "a Cloudflare published range's prefix"
# Hits that stay, each with its reason: (path, class, matched text) -> reason.
ALLOWED_HITS = {
    ('Source/Activities/GameActivity.cpp', 'process-tag', 'F' '21'):
        "part of a test lever's environment name the harness sets; renaming it changes engine behaviour, not text",
    ('Data/Base.rte/GUIs/MainMenuSubMenuGUI.ini', 'process-tag', 'L' '20'):
        'a comment inside the Data module, which this pass leaves untouched',
}
BINARY_RUN = re.compile(rb'[\x20-\x7e]{6,}')


@dataclass(frozen=True)
class Hit:
    path: str
    line: int
    kind: str
    text: str


def git(repo: Path, *args: str) -> str:
    done = subprocess.run(['git', '-C', str(repo), *args], capture_output=True, text=True, encoding='utf-8', errors='replace')
    if done.returncode != 0:
        raise RuntimeError(f"git {' '.join(args)}: {done.stderr.strip()}")
    return done.stdout


def path_roots(facts: box_facts.Facts) -> tuple[set[tuple[str, str]], set[str]]:
    """The (drive, first folder) pairs of every drive-letter path in a box file, and the users of its home paths."""
    roots, users = set(), set(facts.get('users', []))
    def walk(value):
        if isinstance(value, dict):
            for item in value.values():
                walk(item)
        elif isinstance(value, list):
            for item in value:
                walk(item)
        elif isinstance(value, str):
            for match in DRIVE_PATH.finditer(value):
                if match.group(2).lower() != 'users':
                    roots.add((match.group(1).lower(), match.group(2).lower()))
            for match in HOME_PATH.finditer(value):
                users.add(match.group(1))
    walk(facts)
    return roots, users


def ip_allowed(octets: list[int]) -> bool:
    if any(octet > 255 for octet in octets):
        return True
    address = ipaddress.ip_address('.'.join(map(str, octets)))
    if address.is_loopback or address.is_unspecified or any(address in net for net in DOCUMENTATION):
        return True
    if str(address) in ALLOWED_IPV4:
        return True
    for net in SPECIAL_RANGES:
        if address in net:
            base = int(net.network_address)
            return int(address) - base < 256
    return False


class Scanner:
    def __init__(self, machine_names: set[str], user_names: set[str], machine_roots: set[tuple[str, str]],
                 example: box_facts.Facts) -> None:
        self.patterns: list[tuple[str, re.Pattern]] = []
        for pattern in TOOL_PATTERNS:
            self.patterns.append(('tool-name', re.compile('(?i)' + pattern)))
        for pattern in INTERNAL_PATTERNS:
            self.patterns.append(('internal-name', re.compile(pattern)))
        for pattern in TAG_PATTERNS:
            self.patterns.append(('process-tag', re.compile(pattern)))
        if machine_names:
            self.patterns.append(('machine-name', re.compile('(?i)' + EDGE_L + '(?:' + alternation(
                [re.escape(name) for name in machine_names]) + ')' + EDGE_R)))
        if user_names:
            self.patterns.append(('user-name', re.compile('(?i)' + EDGE_L + '(?:' + alternation(
                [re.escape(name) for name in user_names]) + ')' + EDGE_R)))
        self.machine_roots = machine_roots
        _, self.neutral_users = path_roots(example)

    def line(self, path: str, number: int, text: str) -> list[Hit]:
        hits = []
        for kind, pattern in self.patterns:
            for match in pattern.finditer(text):
                hits.append(Hit(path, number, kind, match.group(0)))
        for match in DRIVE_PATH.finditer(text):
            if (match.group(1).lower(), match.group(2).lower()) in self.machine_roots:
                hits.append(Hit(path, number, 'abs-path', match.group(0)))
        for match in HOME_PATH.finditer(text):
            if match.group(1) not in self.neutral_users and not match.group(1).startswith(('<', '$', '{', '%')):
                hits.append(Hit(path, number, 'abs-path', match.group(0)))
        for match in IPV4.finditer(text):
            if not ip_allowed([int(octet) for octet in match.groups()]):
                hits.append(Hit(path, number, 'ipv4', match.group(0)))
        return hits


def scope(repo: Path, base: str, rev: str | None = None) -> list[tuple[str, str]]:
    """(status, path) of every tracked file the fork added (A) or changed (M) since the base, outside third-party roots:
    in the work tree, or in the commit `rev`."""
    git(repo, 'cat-file', '-e', base + '^{commit}')
    listing = ('ls-tree', '-r', '--name-only', '-z', rev) if rev else ('ls-files', '-z')
    tracked = set(git(repo, *listing).split('\0'))
    rows = []
    for line in git(repo, 'diff', '--name-status', '--no-renames', base, *([rev] if rev else [])).splitlines():
        status, _, path = line.partition('\t')
        if path in tracked and not path.startswith(EXCLUDED_ROOTS) and status[:1] in 'AM':
            rows.append((status[:1], path))
    return rows


def file_lines(repo: Path, base: str, status: str, path: str, rev: str | None = None) -> list[tuple[int, str]]:
    if rev:
        data = subprocess.run(['git', '-C', str(repo), 'show', f'{rev}:{path}'], capture_output=True).stdout
    else:
        data = (repo / path).read_bytes()
    if b'\0' in data[:8192]:
        # A binary file's printable runs, numbered -1: they can carry metadata (paths, hosts) but no line numbers.
        return [(-1, run.decode('ascii')) for run in BINARY_RUN.findall(data)]
    lines = data.decode('utf-8', errors='replace').splitlines()
    if status == 'M':
        old = set(subprocess.run(['git', '-C', str(repo), 'show', f'{base}:{path}'], capture_output=True).stdout
                  .decode('utf-8', errors='replace').splitlines())
        return [(number, text) for number, text in enumerate(lines, 1) if text not in old]
    return list(enumerate(lines, 1))


def scan(repo: Path, base: str, scanner: Scanner, rev: str | None = None) -> list[Hit]:
    hits = []
    for status, path in scope(repo, base, rev):
        hits += scanner.line(path, 0, path)
        for number, text in file_lines(repo, base, status, path, rev):
            found = scanner.line(path, number, text)
            # Tag-shaped runs in a binary file are noise.
            hits += [hit for hit in found if (number >= 0 or hit.kind != 'process-tag') and
                     (hit.path, hit.kind, hit.text) not in ALLOWED_HITS]
    return hits


def masked(hit: Hit) -> str:
    if hit.kind == 'ipv4':
        return hit.text.split('.')[0] + '.x.x.x'
    if hit.kind in ('machine-name', 'user-name'):
        return hit.text[:1] + '*' * (len(hit.text) - 1)
    return hit.text


def make_scanner(box_file: Path | None) -> tuple[Scanner, str]:
    """A scanner hunting the box file's machine and user names; the example's own neutral names are never hits."""
    example = box_facts.load(box_facts.EXAMPLE)
    source = box_file if box_file and box_file.is_file() and box_file.resolve() != box_facts.EXAMPLE else None
    facts = box_facts.load(source) if source else example
    machines = box_facts.identifying_names(facts) - box_facts.identifying_names(example)
    users = box_facts.user_names(facts) - box_facts.user_names(example)
    roots = path_roots(facts)[0] - path_roots(example)[0]
    if source:
        note = f'machine names, user names and path roots from {source}'
    else:
        note = ('no box file: machine-name, user-name and drive-letter paths were not checked against real machines '
                '(the example values only, which are allowed)')
    return Scanner(machines, users, roots, example), note


def self_test() -> int:
    """Planted files in a throwaway repository, scanned with a made-up box file: each class must be found, the example's
    neutral names and the allowed address and path forms must not be."""
    failures = []
    example = box_facts.load(box_facts.EXAMPLE)
    machine, user = 'harbor' + '-pc', 'jd' + 'oe'
    made_up = {'users': [user], 'boxes': [{'role': 'pc', 'name': machine.upper(), 'instance': 'harbor', 'ssh': None,
                                           'user': user, 'home': 'C:/Users/' + user, 'tree': 'E' + ':/Work/tree'}]}
    planted = {
        'tool-name': 'Written with ' + TOOL_NAMES[0].capitalize() + '.',
        'internal-name': 'see ' + 'lead' + '-tools/run.sh',
        'process-tag': 'fixes W' + '97 and F' + '85',
        'machine-name': f'ssh {machine} hostname',
        'user-name': f'owner {user}',
        'abs-path': 'E' + ':/' + 'Work/tree/file.txt and /home/' + 'someone/x',
        'ipv4': 'connect to ' + '.'.join(['10', '9', '8', '7']) + ' and ' + '.'.join(['172', '20', '10', '1']),
    }
    clean = ('loopback 127.0.0.1, any 0.0.0.0, documentation 192.0.2.10 and 203.0.113.7, range prefix 10.0.0.8, '
             'version 4.4.3.1, SSDP 239.255.255.250, F6 opens the panel, C:/Windows/System32/tar.exe, /usr/bin/python3, '
             'a synthetic E:/x/out, the example home C:/Users/player')
    with tempfile.TemporaryDirectory() as temp:
        repo = Path(temp)
        def run(*args):
            subprocess.run(['git', '-C', str(repo), *args], check=True, capture_output=True)
        run('init', '-q')
        run('config', 'user.email', 'scan@example.invalid')
        run('config', 'user.name', 'scan')
        upstream = 'an upstream line with ' + '.'.join(['10', '9', '8', '7']) + '\n'
        (repo / 'upstream.txt').write_text(upstream, encoding='utf-8')
        run('add', '-A')
        run('commit', '-qm', 'base')
        base = git(repo, 'rev-parse', 'HEAD').strip()
        (repo / 'upstream.txt').write_text(upstream + clean + '\n', encoding='utf-8')
        for kind, text in planted.items():
            (repo / f'{kind}.txt').write_text(text + '\n', encoding='utf-8')
        (repo / 'clean.txt').write_text(clean + '\n', encoding='utf-8')
        (repo / 'external').mkdir()
        (repo / 'external' / 'vendor.txt').write_text(planted['tool-name'] + '\n', encoding='utf-8')
        (repo / 'neutral.txt').write_text(' '.join(sorted(box_facts.identifying_names(example))) + '\n', encoding='utf-8')
        run('add', '-A')
        run('commit', '-qm', 'fork')
        box_file = repo.parent / (repo.name + '-boxes.json')
        box_file.write_text(json.dumps(made_up), encoding='utf-8')
        try:
            scanner, _ = make_scanner(box_file)
        finally:
            box_file.unlink()
        hits = scan(repo, base, scanner)
        found = {(hit.path, hit.kind) for hit in hits}
        for kind in CLASSES:
            if (f'{kind}.txt', kind) not in found:
                failures.append(f'{kind}: the planted case was not found')
        wrong = [hit for hit in hits if hit.path in ('clean.txt', 'upstream.txt', 'neutral.txt') or hit.path.startswith('external/')]
        failures += [f'flagged an allowed form: {hit.path}:{hit.line} {hit.kind} {hit.text!r}' for hit in wrong]
        tags = [hit.text for hit in hits if hit.path == 'process-tag.txt']
        if len(tags) != 2:
            failures.append(f'process-tag: expected both tags, got {tags}')
        paths = [hit.text for hit in hits if hit.path == 'abs-path.txt' and hit.kind == 'abs-path']
        if len(paths) != 2:
            failures.append(f'abs-path: expected the drive path and the home path, got {paths}')
        addresses = [hit.text for hit in hits if hit.path == 'ipv4.txt' and hit.kind == 'ipv4']
        if len(addresses) != 2:
            failures.append(f'ipv4: expected both addresses, got {addresses}')
    for failure in failures:
        print(f'[scan-public-tree-selftest] FAIL {failure}')
    print('[scan-public-tree-selftest] ' + ('FAIL' if failures else 'PASS'))
    return 1 if failures else 0


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument('--repo', type=Path, default=HERE.parent)
    parser.add_argument('--base', default=UPSTREAM_BASE)
    parser.add_argument('--rev', help='scan this commit instead of the work tree')
    parser.add_argument('--box-file', type=Path, help='default: the box file tools/box_facts.py finds')
    parser.add_argument('--show', action='store_true', help='print the matched text unmasked')
    parser.add_argument('--json', type=Path, help='write every hit as JSON here')
    parser.add_argument('--self-test', action='store_true')
    args = parser.parse_args()
    if args.self_test:
        return self_test()
    scanner, note = make_scanner(args.box_file or box_facts.file())
    try:
        hits = scan(args.repo.resolve(), args.base, scanner, args.rev)
    except RuntimeError as error:
        print(f'[scan-public-tree] cannot scan: {error}')
        return 2
    for hit in hits:
        print(f'{hit.path}:{hit.line}: {hit.kind}: {hit.text if args.show else masked(hit)}')
    counts = Counter(hit.kind for hit in hits)
    print(f'[scan-public-tree] {note}')
    print('[scan-public-tree] ' + ' '.join(f'{kind}={counts.get(kind, 0)}' for kind in CLASSES) +
          f' files={len({hit.path for hit in hits})}')
    if args.json:
        args.json.write_text(json.dumps([hit.__dict__ for hit in hits], indent=1), encoding='utf-8')
    return 1 if hits else 0


if __name__ == '__main__':
    sys.exit(main())
