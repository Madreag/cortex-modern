#!/usr/bin/env python3
"""Census of engine API names a mod's Lua scripts use vs what the Lua bindings expose.

Parses Source/Lua/LuaBindings*.cpp and the luabind::module block in
Source/Managers/LuaMan.cpp for the registered surface (class members,
properties, enum constants, and the To/Is/Create/Random/Clone globals generated
per concrete type), reads the manager singleton globals from LuaMan.cpp, then
scans a data module's *.lua for `Object:Member(` calls and `Object.Member`
accesses and reports every member name the current bindings do not expose. A call
directly under `if recv.member [~= nil] then` is the mod's own optional hook and is
listed as a note, not a row.

Usage:
    python tools/mod_api_census.py --repo <repo> --module Data/VoidWanderers.rte --out <json>
    python tools/mod_api_census.py --self-test
"""
import argparse
import glob
import json
import os
import re
import sys

RE_CLASS_MACRO = re.compile(r'\b(Concrete|Abstract)TypeLuaClassDefinition\(\s*([A-Za-z_][\w:]*)\s*,\s*([A-Za-z_][\w:]*)\s*\)')
RE_CLASS_RAW = re.compile(r'class_<\s*([A-Za-z_][\w:]*)\s*(?:,\s*([A-Za-z_][\w:]*))?\s*>\s*\(\s*"([^"]+)"')
RE_MEMBER = re.compile(r'\.\s*(?:def|property|def_readwrite|def_readonly)\s*\(\s*"([^"]+)"')
RE_ENUMVAL = re.compile(r'luabind::value\(\s*"([^"]+)"')
RE_GLOBAL = re.compile(r'luabind::globals\(\s*m_State\s*\)\s*\[\s*"([^"]+)"\s*\]\s*=\s*([^;\s]+)')
RE_GLOBALFN = re.compile(r'luabind::def\(\s*"([^"]+)"')
RE_REGISTER = re.compile(r'RegisterLuaBindingsOf(Concrete|Abstract)Type\([^,]+,\s*([A-Za-z_][\w:]*)\s*\)')

LUA_KEYWORDS = {
    'and', 'break', 'do', 'else', 'elseif', 'end', 'false', 'for', 'function',
    'if', 'in', 'local', 'nil', 'not', 'or', 'repeat', 'return', 'then',
    'true', 'until', 'while', 'goto', 'self',
}
LUA_BUILTINS = {
    'assert', 'collectgarbage', 'dofile', 'error', 'getmetatable', 'ipairs',
    'load', 'loadfile', 'loadstring', 'next', 'pairs', 'pcall', 'print',
    'rawequal', 'rawget', 'rawlen', 'rawset', 'require', 'select',
    'setmetatable', 'tonumber', 'tostring', 'type', 'unpack', 'xpcall',
    'coroutine', 'string', 'table', 'math', 'io', 'os', 'debug', 'utf8',
    'package', 'cls', 'SelectRand', 'RangeRand', 'PosRand', 'NormalRand',
}
# Method names from the Lua standard libraries used in colon form
# (file:read/write, str:find/..., co:resume/...) — never engine members.
LUA_STDLIB_METHODS = {
    'read', 'write', 'lines', 'close', 'seek', 'flush',
    'find', 'format', 'gsub', 'gmatch', 'match', 'byte', 'char', 'dump',
    'len', 'lower', 'rep', 'reverse', 'sub', 'upper',
    'resume', 'yield', 'status', 'wrap', 'isyieldable',
    'insert', 'remove', 'concat', 'sort', 'pack', 'move',
}


def strip_cpp_comments(src):
    src = re.sub(r'/\*.*?\*/', lambda m: '\n' * m.group(0).count('\n'), src, flags=re.S)
    return re.sub(r'//[^\n]*', '', src)


def strip_lua(src):
    """Remove comments and string literals, keeping line numbers intact."""
    out = []
    i, n = 0, len(src)
    while i < n:
        c = src[i]
        if src[i:i + 2] == '--':
            m = re.match(r'--\[(=*)\[', src[i:])
            if m:  # long comment
                close = ']' + m.group(1) + ']'
                end = src.find(close, i + 4)
                chunk = src[i:end + len(close) if end >= 0 else n]
                out.append(''.join(ch if ch == '\n' else ' ' for ch in chunk))
                i += len(chunk)
                continue
            end = src.find('\n', i)
            end = n if end < 0 else end
            out.append(' ' * (end - i))
            i = end
            continue
        if c in '"\'':
            j = i + 1
            while j < n and src[j] != c:
                j += 2 if src[j] == '\\' else 1
            j = min(j + 1, n)
            out.append(''.join(ch if ch == '\n' else ' ' for ch in src[i:j]))
            i = j
            continue
        m = re.match(r'\[(=*)\[', src[i:])
        if m:  # long string
            close = ']' + m.group(1) + ']'
            end = src.find(close, i + 2)
            chunk = src[i:end + len(close) if end >= 0 else n]
            out.append(''.join(ch if ch == '\n' else ' ' for ch in chunk))
            i += len(chunk)
            continue
        out.append(c)
        i += 1
    return ''.join(out)


def statement_end(src, start):
    """Index where the luabind chain starting at `start` ends: a depth-0 ; , ] or }."""
    depth = 0
    i = start
    while i < len(src):
        c = src[i]
        if c in '([<':
            depth += 1
        elif c in ')]>':
            if depth == 0:
                return i
            depth -= 1
        elif depth == 0 and c in ';,}':
            return i
        i += 1
    return i


def parse_bindings(repo):
    """Return classes {lua name -> {members, bases, cpp}} and global function names."""
    classes = {}
    global_functions = set()
    files = sorted(glob.glob(os.path.join(repo, 'Source', 'Lua', 'LuaBindings*.cpp')))
    files.append(os.path.join(repo, 'Source', 'Managers', 'LuaMan.cpp'))
    for path in files:
        src = strip_cpp_comments(open(path, encoding='utf-8', errors='replace').read())
        marks = []  # (pos, cpp_name, lua_name, base_cpp, concrete)
        for m in RE_CLASS_MACRO.finditer(src):
            kind, cpp, base = m.group(1), m.group(2), m.group(3)
            lua_name = cpp.split('::')[-1]
            marks.append((m.start(), cpp, lua_name, base, kind == 'Concrete'))
        for m in RE_CLASS_RAW.finditer(src):
            cpp, base, lua_name = m.group(1), m.group(2), m.group(3)
            marks.append((m.start(), cpp, lua_name, base, False))
        for pos, cpp, lua_name, base, concrete in marks:
            stmt = src[pos:statement_end(src, pos)]
            entry = classes.setdefault(lua_name, {'members': set(), 'bases': [], 'cpp': cpp})
            entry['members'] |= set(RE_MEMBER.findall(stmt))
            entry['members'] |= set(RE_ENUMVAL.findall(stmt))
            entry['members'].add('ClassName')
            if concrete:
                entry['members'].add('Clone')
            if base:
                entry['bases'].append(base)
        global_functions |= set(RE_GLOBALFN.findall(src))
    return classes, global_functions


def parse_luaman(repo, classes):
    """Global singleton name -> lua class name, plus macro-generated global fns."""
    path = os.path.join(repo, 'Source', 'Managers', 'LuaMan.cpp')
    src = strip_cpp_comments(open(path, encoding='utf-8', errors='replace').read())
    cpp_to_lua = {v['cpp']: k for k, v in classes.items()}
    globals_ = {}
    for m in RE_GLOBAL.finditer(src):
        expr = m.group(2).lstrip('&')
        if expr == 'this':
            # the sole class registered in this file's module block is the global's type
            lua_cls = next(iter(cpp_to_lua.values()), None) if len(cpp_to_lua) == 1 else 'LuaManager'
            globals_[m.group(1)] = lua_cls
            continue
        # The manager globals bind InstanceOrNull<X>() (nil before X is constructed), older builds &g_X; both name X.
        instance = re.fullmatch(r'InstanceOrNull<\s*(\w+)\s*>\(\)', expr)
        cpp = instance.group(1) if instance else expr[2:] if expr.startswith('g_') else expr
        globals_[m.group(1)] = cpp_to_lua.get(cpp, cpp)
    global_functions = set()
    for m in RE_REGISTER.finditer(src):
        kind, t = m.group(1), m.group(2).split('::')[-1]
        global_functions.update(('To' + t, 'Is' + t))
        if kind == 'Concrete':
            global_functions.update(('Create' + t, 'Random' + t))
    return globals_, global_functions


def member_closure(classes, lua_name, seen=None):
    seen = seen or set()
    if lua_name in seen:
        return set()
    seen.add(lua_name)
    entry = classes.get(lua_name)
    if not entry:
        return set()
    out = set(entry['members'])
    cpp_to_lua = {v['cpp']: k for k, v in classes.items()}
    for b in entry['bases']:
        out |= member_closure(classes, cpp_to_lua.get(b, b), seen)
    return out


def lua_line(src, pos):
    return src.count('\n', 0, pos) + 1


def guarded_call(lines, line, recv, member):
    """True when `recv:member(` on `line` directly follows `if recv.member [~= nil] then`, on the same line or the block's opening line above."""
    guard = rf'\bif\s+{re.escape(recv)}\s*\.\s*{re.escape(member)}\s*(?:~=\s*nil\s*)?then\b'
    call = rf'{re.escape(recv)}\s*:\s*{re.escape(member)}\s*\('
    text = lines[line - 1]
    if re.search(guard + r'\s*' + call, text):
        return True
    before_call = re.split(call, text, maxsplit=1)[0]
    above = next((prev for prev in reversed(lines[:line - 1]) if prev.strip()), '')
    return re.fullmatch(r'\s*' + guard + r'\s*', above) is not None and not re.search(r'\bend\b', before_call)


def scan_module(mod_root, repo, classes, globals_, global_functions):
    union_members = set()
    cpp_to_lua = {v['cpp']: k for k, v in classes.items()}
    for name in classes:
        union_members |= member_closure(classes, name)

    files = sorted(glob.glob(os.path.join(repo, mod_root.replace('/', os.sep), '**', '*.lua'), recursive=True))
    method_calls = {}
    dot_uses = {}
    bare_calls = {}
    lua_defined = set()      # names the module defines as functions (cross-file)
    file_locals = {}         # rel path -> local names (file-scoped shadows)
    file_lines = {}          # rel path -> comment- and string-stripped lines
    for fp in files:
        rel = os.path.relpath(fp, repo).replace(os.sep, '/')
        src = strip_lua(open(fp, encoding='utf-8', errors='replace').read())
        file_lines[rel] = src.split('\n')
        for m in re.finditer(r'function\s+[\w\.]+:(\w+)|function\s+([\w\.]+)\s*\(|([\w\.]+)\s*=\s*function', src):
            for g in m.groups():
                if g:
                    lua_defined.add(g.split('.')[-1])
        # Name aliases the module installs globally (sub = string.sub); a call to
        # the alias is a call to the aliased function, never an engine global.
        for m in re.finditer(r'(?<![\w\.])([A-Za-z_]\w*)\s*=\s*([A-Za-z_][\w\.]*)\s*[\n;,)]', src):
            lua_defined.add(m.group(1))
        # Locals shadow any global of the same name: local f = ..., local sub = string.sub
        file_locals[rel] = set(
            m.group(1) for m in re.finditer(r'\blocal\s+(?:function\s+)?([A-Za-z_]\w*)', src)
        )
        for m in re.finditer(r'([A-Za-z_][\w\.\)\]]*)\s*:\s*([A-Za-z_]\w*)\s*\(', src):
            method_calls.setdefault(m.group(2), []).append((rel, lua_line(src, m.start(2)), m.group(1)))
        for m in re.finditer(r'\b([A-Za-z_]\w*)\s*\.\s*([A-Za-z_]\w*)', src):
            if m.group(1) != 'self' and m.group(2) != 'self':
                dot_uses.setdefault(m.group(1), []).append((rel, lua_line(src, m.start(2)), m.group(2)))
        for m in re.finditer(r'(?<![\w\.:])\b([A-Za-z_]\w*)\s*\(', src):
            bare_calls.setdefault(m.group(1), []).append((rel, lua_line(src, m.start(1))))

    misses, notes = [], []
    class_names = set(classes)

    def recv_root(recv):
        return recv.split('.')[0].split('(')[0].rstrip(')]')

    # 1. :Member( on engine singleton globals: exact class-member check. Only a
    # bare `Global:` receiver is the global itself; a chained `Global.Prop:`
    # or `Global:Method():` receiver calls the member on the leaf's own type.
    for member, sites in method_calls.items():
        for rel, line, recv in sites:
            root = recv_root(recv)
            if root in globals_ and recv == root:
                closure = member_closure(classes, globals_[root])
                if member not in closure:
                    misses.append({'kind': 'method', 'object': root, 'class': globals_[root],
                                   'member': member, 'file': rel, 'line': line,
                                   'call': f'{recv}:{member}('})

    # 2. ClassName.X / GlobalName.X constant or property access
    for root, uses in dot_uses.items():
        if root in class_names:
            closure = member_closure(classes, root)
            kind = 'class_member'
        elif root in globals_:
            closure = member_closure(classes, globals_[root])
            kind = 'property'
        else:
            continue
        for rel, line, member in uses:
            if member not in closure:
                misses.append({'kind': kind, 'object': root, 'class': globals_.get(root, root),
                               'member': member, 'file': rel, 'line': line,
                               'call': f'{root}.{member}'})

    # 3. :Member( on other receivers: flag only names no bound class exposes and
    #    the mod never defines as a Lua function or stdlib method. Bare-global
    #    receivers were already checked exactly in step 1.
    for member, sites in method_calls.items():
        if member in union_members or member in lua_defined or member in LUA_BUILTINS or member in LUA_STDLIB_METHODS:
            continue
        for rel, line, recv in sites:
            if recv == recv_root(recv) and recv in globals_:
                continue
            # The mod's own optional hook: nil unless a script defines it, so the guard skips the call.
            if guarded_call(file_lines[rel], line, recv, member):
                notes.append({'kind': 'guarded_optional_hook', 'object': recv, 'member': member, 'file': rel, 'line': line})
                continue
            misses.append({'kind': 'unresolved_method', 'object': recv, 'class': None,
                           'member': member, 'file': rel, 'line': line,
                           'call': f'{recv}:{member}('})

    # 4. bare global calls neither builtin, mod-defined, bound nor class names
    for name, sites in bare_calls.items():
        if name in LUA_KEYWORDS or name in LUA_BUILTINS or name in global_functions or name in lua_defined or name in class_names:
            continue
        for rel, line in sites:
            if name in file_locals.get(rel, ()):
                continue
            notes.append({'kind': 'unresolved_global_call', 'name': name, 'file': rel, 'line': line})

    return misses, notes, len(files)


SELF_TEST_LUA = """function Fixture:Update()
\tif self.OptionalA ~= nil then
\t\tself:OptionalA()
\tend
\tif self.OptionalB then

\t\tlocal value = self:OptionalB(1)
\tend
\tif sock.optionalC then sock:optionalC(1) end
\tif self.OptionalH ~= nil then -- a trailing comment
\t\tself:OptionalH()
\tend
\tif self.OptionalA ~= nil then
\t\tself:NotTheGuardedName()
\tend
\tif self.OptionalD then other() end
\tself:OptionalD()
\tif self.OptionalE == nil then
\t\tself:OptionalE()
\tend
\tif not self.OptionalF then
\t\tself:OptionalF()
\tend
\tif self.WasPaused then
\t\tself:RestoreHook()
\tend
\tif self.OptionalG ~= nil then
\t\tlocal unused = 1
\t\tself:OptionalG()
\tend
\tself:Unguarded()
end
"""
SELF_TEST_ROWS = {'NotTheGuardedName', 'OptionalD', 'OptionalE', 'OptionalF', 'RestoreHook', 'OptionalG', 'Unguarded'}
SELF_TEST_GUARDED = {'OptionalA', 'OptionalB', 'optionalC', 'OptionalH'}


def self_test():
    """A call directly under `if recv.member [~= nil] then` is a note; every other unbound call stays a row."""
    import tempfile
    failures = []
    with tempfile.TemporaryDirectory() as repo:
        for rel, text in (('Source/Lua/LuaBindingsFixture.cpp', 'ConcreteTypeLuaClassDefinition(Fixture, Entity)\n\t.def("Bound", &Fixture::Bound);\n'),
                          ('Source/Managers/LuaMan.cpp', ''), ('Data/Fixture.rte/Hooks.lua', SELF_TEST_LUA)):
            os.makedirs(os.path.dirname(os.path.join(repo, rel)), exist_ok=True)
            with open(os.path.join(repo, rel), 'w', encoding='utf-8', newline='\n') as f:
                f.write(text)
        classes, global_functions = parse_bindings(repo)
        globals_, macro_functions = parse_luaman(repo, classes)
        misses, notes, _ = scan_module('Data/Fixture.rte', repo, classes, globals_, global_functions | macro_functions)
        rows = {m['member'] for m in misses}
        guarded = {n['member'] for n in notes if n['kind'] == 'guarded_optional_hook'}
        if rows != SELF_TEST_ROWS:
            failures.append(f'rows {sorted(rows)}; hidden {sorted(SELF_TEST_ROWS - rows)}, extra {sorted(rows - SELF_TEST_ROWS)}')
        if guarded != SELF_TEST_GUARDED:
            failures.append(f'guarded notes {sorted(guarded)}; expected {sorted(SELF_TEST_GUARDED)}')
    # A manager global maps to its bound class whichever form LuaMan binds it with.
    with tempfile.TemporaryDirectory() as repo:
        for rel, text in (('Source/Lua/LuaBindingsManagers.cpp', 'luabind::class_<SceneMan>("SceneManager")\n\t.def("Bound", &SceneMan::Bound);\n'
                                                                 'luabind::class_<TimerMan>("TimerManager")\n\t.def("Bound", &TimerMan::Bound);\n'),
                          ('Source/Managers/LuaMan.cpp', 'luabind::globals(m_State)["SceneMan"] = InstanceOrNull<SceneMan>();\n'
                                                         'luabind::globals(m_State)["TimerMan"] = &g_TimerMan;\n')):
            os.makedirs(os.path.dirname(os.path.join(repo, rel)), exist_ok=True)
            with open(os.path.join(repo, rel), 'w', encoding='utf-8', newline='\n') as f:
                f.write(text)
        globals_, _ = parse_luaman(repo, parse_bindings(repo)[0])
        if globals_ != {'SceneMan': 'SceneManager', 'TimerMan': 'TimerManager'}:
            failures.append(f'manager globals {globals_}; expected SceneMan -> SceneManager and TimerMan -> TimerManager')
    for failure in failures:
        print(f'[mod_api_census self-test] FAIL {failure}')
    print(f'[mod_api_census self-test] {"PASS" if not failures else "FAIL"} {len(failures)} failure(s)')
    return 0 if not failures else 1


def main():
    if '--self-test' in sys.argv[1:]:
        return self_test()
    ap = argparse.ArgumentParser()
    ap.add_argument('--repo', default='.')
    ap.add_argument('--module', required=True, help='module dir relative to repo, e.g. Data/VoidWanderers.rte')
    ap.add_argument('--out', help='write JSON report here (default stdout)')
    args = ap.parse_args()

    classes, global_functions = parse_bindings(args.repo)
    globals_, macro_functions = parse_luaman(args.repo, classes)
    global_functions |= macro_functions
    misses, notes, nfiles = scan_module(args.module, args.repo, classes, globals_, global_functions)

    seen, unique = set(), []
    for m in misses:
        key = (m['object'], m['member'], m['file'], m['line'])
        if key not in seen:
            seen.add(key)
            unique.append(m)

    report = {
        'module': args.module,
        'files_scanned': nfiles,
        'missing': sorted(unique, key=lambda m: (str(m['object']), m['member'], m['file'], m['line'])),
        'unresolved_global_calls': [n for n in notes if n['kind'] == 'unresolved_global_call'],
        'guarded_optional_hooks': [n for n in notes if n['kind'] == 'guarded_optional_hook'],
    }
    text = json.dumps(report, indent=2)
    if args.out:
        os.makedirs(os.path.dirname(os.path.abspath(args.out)), exist_ok=True)
        with open(args.out, 'w') as f:
            f.write(text + '\n')
    print(text)
    return 0


if __name__ == '__main__':
    sys.exit(main())
