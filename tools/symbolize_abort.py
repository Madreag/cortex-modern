"""Resolve AbortCode exe+0x offsets through dbghelp. Optional dump supplies the faulting VA."""
from __future__ import annotations

import argparse
import ctypes
import re
import struct
import sys
import uuid
from ctypes import wintypes
from pathlib import Path

SYMOPT_UNDNAME = 0x00000002
SYMOPT_LOAD_LINES = 0x00000010
SYMOPT_FAIL_CRITICAL_ERRORS = 0x00000200
SYMOPT_NO_PROMPTS = 0x00080000
MAX_SYM_NAME = 2000
EXCEPTION_STREAM = 6
MODULE_LIST_STREAM = 4

FATAL_RE = re.compile(
    r"FATAL:\s+unhandled exception\s+0x([0-9A-Fa-f]+)\s+at\s+0x([0-9A-Fa-f]+)(?:\s+\(exe\+0x([0-9A-Fa-f]+)\))?",
    re.I,
)
STACK_RE = re.compile(r"exe\+0x([0-9A-Fa-f]+)", re.I)

dbghelp = ctypes.WinDLL("dbghelp", use_last_error=True)
kernel32 = ctypes.WinDLL("kernel32", use_last_error=True)


class SYMBOL_INFO(ctypes.Structure):
    _pack_ = 8
    _fields_ = [
        ("SizeOfStruct", ctypes.c_ulong),
        ("TypeIndex", ctypes.c_ulong),
        ("Reserved", ctypes.c_ulonglong * 2),
        ("Index", ctypes.c_ulong),
        ("Size", ctypes.c_ulong),
        ("ModBase", ctypes.c_ulonglong),
        ("Flags", ctypes.c_ulong),
        ("Value", ctypes.c_ulonglong),
        ("Address", ctypes.c_ulonglong),
        ("Register", ctypes.c_ulong),
        ("Scope", ctypes.c_ulong),
        ("Tag", ctypes.c_ulong),
        ("NameLen", ctypes.c_ulong),
        ("MaxNameLen", ctypes.c_ulong),
        ("Name", ctypes.c_char * (MAX_SYM_NAME + 1)),
    ]


class IMAGEHLP_LINE64(ctypes.Structure):
    _fields_ = [
        ("SizeOfStruct", ctypes.c_ulong),
        ("Key", ctypes.c_void_p),
        ("LineNumber", ctypes.c_ulong),
        ("FileName", ctypes.c_char_p),
        ("Address", ctypes.c_ulonglong),
    ]


class IMAGEHLP_MODULE64(ctypes.Structure):
    _fields_ = [
        ("SizeOfStruct", ctypes.c_ulong), ("BaseOfImage", ctypes.c_ulonglong),
        ("ImageSize", ctypes.c_ulong), ("TimeDateStamp", ctypes.c_ulong),
        ("CheckSum", ctypes.c_ulong), ("NumSyms", ctypes.c_ulong), ("SymType", ctypes.c_int),
        ("ModuleName", ctypes.c_char * 32), ("ImageName", ctypes.c_char * 256),
        ("LoadedImageName", ctypes.c_char * 256), ("LoadedPdbName", ctypes.c_char * 256),
        ("CVSig", ctypes.c_ulong), ("CVData", ctypes.c_char * 780),
        ("PdbSig", ctypes.c_ulong), ("PdbSig70", ctypes.c_ubyte * 16),
        ("PdbAge", ctypes.c_ulong), ("PdbUnmatched", wintypes.BOOL),
        ("DbgUnmatched", wintypes.BOOL), ("LineNumbers", wintypes.BOOL),
        ("GlobalSymbols", wintypes.BOOL), ("TypeInfo", wintypes.BOOL),
        ("SourceIndexed", wintypes.BOOL), ("Publics", wintypes.BOOL),
        ("MachineType", ctypes.c_ulong), ("Reserved", ctypes.c_ulong),
    ]


def read_image(exe):
    data = Path(exe).read_bytes()
    pe = struct.unpack_from("<I", data, 0x3C)[0]
    if data[:2] != b"MZ" or data[pe:pe + 4] != b"PE\0\0":
        raise ValueError(f"not a PE image: {exe}")
    sections = struct.unpack_from("<H", data, pe + 6)[0]
    optional_size = struct.unpack_from("<H", data, pe + 20)[0]
    optional = pe + 24
    is64 = struct.unpack_from("<H", data, optional)[0] == 0x20B
    base = struct.unpack_from("<Q" if is64 else "<I", data, optional + (24 if is64 else 28))[0]
    size = struct.unpack_from("<I", data, optional + 56)[0]
    debug_rva, debug_size = struct.unpack_from("<II", data, optional + (112 if is64 else 96) + 6 * 8)

    def file_offset(rva):
        for index in range(sections):
            header = optional + optional_size + index * 40
            virtual_size, virtual, raw_size, raw = struct.unpack_from("<IIII", data, header + 8)
            if virtual <= rva < virtual + max(virtual_size, raw_size):
                return raw + rva - virtual
        raise ValueError(f"unmapped PE RVA: 0x{rva:X}")

    result = {"base": base, "size": size}
    for offset in range(0, debug_size, 28):
        entry = struct.unpack_from("<IIHHIIII", data, file_offset(debug_rva) + offset)
        if entry[4] == 2:
            cv = data[entry[7]:entry[7] + entry[5]]
            if cv[:4] == b"RSDS":
                result.update(guid=cv[4:20], age=struct.unpack_from("<I", cv, 20)[0],
                              pdb=cv[24:].split(b"\0", 1)[0].decode("mbcs"))
                break
    if "guid" not in result:
        raise ValueError(f"image has no PDB identity: {exe}")
    return result


def _sym_info():
    info = SYMBOL_INFO()
    info.SizeOfStruct = 88
    info.MaxNameLen = MAX_SYM_NAME
    return info


dbghelp.SymInitialize.argtypes = [wintypes.HANDLE, ctypes.c_char_p, wintypes.BOOL]
dbghelp.SymInitialize.restype = wintypes.BOOL
dbghelp.SymSetOptions.argtypes = [ctypes.c_ulong]
dbghelp.SymSetOptions.restype = ctypes.c_ulong
dbghelp.SymLoadModuleEx.argtypes = [
    wintypes.HANDLE,
    wintypes.HANDLE,
    ctypes.c_char_p,
    ctypes.c_char_p,
    ctypes.c_ulonglong,
    ctypes.c_ulong,
    ctypes.c_void_p,
    ctypes.c_ulong,
]
dbghelp.SymLoadModuleEx.restype = ctypes.c_ulonglong
dbghelp.SymFromAddr.argtypes = [
    wintypes.HANDLE,
    ctypes.c_ulonglong,
    ctypes.POINTER(ctypes.c_ulonglong),
    ctypes.POINTER(SYMBOL_INFO),
]
dbghelp.SymFromAddr.restype = wintypes.BOOL
dbghelp.SymFromName.argtypes = [wintypes.HANDLE, ctypes.c_char_p, ctypes.POINTER(SYMBOL_INFO)]
dbghelp.SymFromName.restype = wintypes.BOOL
dbghelp.SymGetLineFromAddr64.argtypes = [
    wintypes.HANDLE,
    ctypes.c_ulonglong,
    ctypes.POINTER(ctypes.c_ulong),
    ctypes.POINTER(IMAGEHLP_LINE64),
]
dbghelp.SymGetLineFromAddr64.restype = wintypes.BOOL
dbghelp.SymCleanup.argtypes = [wintypes.HANDLE]
dbghelp.SymCleanup.restype = wintypes.BOOL
dbghelp.SymGetModuleInfo64.argtypes = [wintypes.HANDLE, ctypes.c_ulonglong, ctypes.POINTER(IMAGEHLP_MODULE64)]
dbghelp.SymGetModuleInfo64.restype = wintypes.BOOL
kernel32.GetCurrentProcess.restype = wintypes.HANDLE


class Session:
    def __init__(self, exe, load_base=0, pdb=None):
        self.exe = str(Path(exe).resolve())
        image = read_image(self.exe)
        sidecar = Path(self.exe).with_suffix(".pdb")
        symbols = Path(pdb).resolve() if pdb else sidecar if sidecar.is_file() else Path(image["pdb"])
        if not symbols.is_file():
            raise FileNotFoundError(f"PDB not found: {symbols}; supply --pdb")
        self.handle = kernel32.GetCurrentProcess()
        search = str(symbols.parent).encode("mbcs", "replace")
        self.options = SYMOPT_LOAD_LINES | SYMOPT_UNDNAME | SYMOPT_FAIL_CRITICAL_ERRORS | SYMOPT_NO_PROMPTS
        dbghelp.SymSetOptions(self.options)
        if not dbghelp.SymInitialize(self.handle, search, False):
            raise OSError(f"SymInitialize failed: {ctypes.get_last_error()}")
        self.base = dbghelp.SymLoadModuleEx(
            self.handle,
            None,
            str(symbols).encode("mbcs", "replace"),
            None,
            ctypes.c_ulonglong(load_base or image["base"]),
            image["size"],
            None,
            0,
        )
        if not self.base:
            dbghelp.SymCleanup(self.handle)
            raise OSError(f"SymLoadModuleEx failed: {ctypes.get_last_error()}")
        module = IMAGEHLP_MODULE64()
        module.SizeOfStruct = ctypes.sizeof(module)
        if not dbghelp.SymGetModuleInfo64(self.handle, self.base, ctypes.byref(module)):
            self.close()
            raise OSError(f"SymGetModuleInfo64 failed: {ctypes.get_last_error()}")
        if bytes(module.PdbSig70) != image["guid"] or module.PdbAge != image["age"]:
            self.close()
            raise ValueError(f"PDB identity mismatch: {symbols}")
        self.provenance = (f"PDB: {module.LoadedPdbName.decode('mbcs')} "
                           f"guid={uuid.UUID(bytes_le=image['guid'])} age={image['age']} "
                           f"base=0x{self.base:X} size=0x{image['size']:X} options=0x{self.options:X} "
                           f"lines={bool(module.LineNumbers)} types={bool(module.TypeInfo)}")

    def close(self):
        dbghelp.SymCleanup(self.handle)

    def resolve(self, address):
        info = _sym_info()
        displacement = ctypes.c_ulonglong(0)
        name = None
        if dbghelp.SymFromAddr(self.handle, address, ctypes.byref(displacement), ctypes.byref(info)):
            name = info.Name.decode("ascii", "replace")
        line = IMAGEHLP_LINE64()
        line.SizeOfStruct = ctypes.sizeof(IMAGEHLP_LINE64)
        line_disp = ctypes.c_ulong(0)
        path = None
        number = None
        if dbghelp.SymGetLineFromAddr64(self.handle, address, ctypes.byref(line_disp), ctypes.byref(line)):
            path = line.FileName.decode("ascii", "replace") if line.FileName else None
            number = int(line.LineNumber)
        return {
            "address": address,
            "offset": address - self.base,
            "name": name,
            "file": path,
            "line": number,
            "displacement": int(displacement.value),
        }


def lookup_name(exe, name):
    session = Session(exe)
    try:
        for candidate in (name, f"{Path(exe).stem}!{name}", f"Cortex Command!{name}"):
            info = _sym_info()
            if dbghelp.SymFromName(session.handle, candidate.encode("ascii", "replace"), ctypes.byref(info)):
                return {
                    "name": info.Name.decode("ascii", "replace"),
                    "address": int(info.Address),
                    "offset": int(info.Address) - session.base,
                    "base": session.base,
                    "query": candidate,
                }
        return None
    finally:
        session.close()


def parse_abort(text):
    fatal = None
    match = FATAL_RE.search(text)
    if match:
        fatal = {"code": int(match.group(1), 16), "address": int(match.group(2), 16)}
        if match.group(3):
            fatal["offset"] = int(match.group(3), 16)
    frames = []
    for line in text.splitlines():
        found = STACK_RE.search(line)
        if found:
            frames.append({"offset": int(found.group(1), 16), "raw": line})
    return fatal, frames


def parse_dump(path, exe=None):
    data = Path(path).read_bytes()
    if data[:4] != b"MDMP":
        raise ValueError(f"not a minidump: {path}")
    _sig, _ver, nstreams, dir_rva = struct.unpack_from("<IIII", data, 0)
    exception_address = None
    module_base = None
    modules = []
    for index in range(nstreams):
        stream_type, _size, rva = struct.unpack_from("<III", data, dir_rva + index * 12)
        if stream_type == EXCEPTION_STREAM and rva + 32 <= len(data):
            exception_address = struct.unpack_from("<Q", data, rva + 24)[0]
        if stream_type == MODULE_LIST_STREAM and rva + 12 <= len(data):
            count = struct.unpack_from("<I", data, rva)[0]
            for module_index in range(count):
                entry = rva + 4 + module_index * 108
                base, size = struct.unpack_from("<QI", data, entry)
                name_rva = struct.unpack_from("<I", data, entry + 20)[0]
                length = struct.unpack_from("<I", data, name_rva)[0]
                name = data[name_rva + 4:name_rva + 4 + length].decode("utf-16-le")
                modules.append((base, size, name))
    candidates = [module for module in modules if module[2].lower().endswith(".exe")]
    if exe:
        named = [module for module in candidates if Path(module[2]).name.lower() == Path(exe).name.lower()]
        candidates = named or candidates
    if len(candidates) != 1:
        raise ValueError(f"cannot identify the executable module in {path}")
    module_base = candidates[0][0]
    return {"exception_address": exception_address, "module_base": module_base}


def format_row(label, resolved):
    name = resolved.get("name") or "?"
    path = resolved.get("file")
    number = resolved.get("line")
    where = f"{path}:{number}" if path and number is not None else "?:?"
    return f"{label:<22} 0x{resolved['address']:016X}  {name} ({where})"


def symbolize(exe, abort_path, dump_path=None, pdb=None):
    text = Path(abort_path).read_text(encoding="utf-8-sig", errors="replace")
    fatal, frames = parse_abort(text)
    dump = parse_dump(dump_path, exe) if dump_path else None
    load_base = dump["module_base"] if dump and dump.get("module_base") else 0
    session = Session(exe, load_base, pdb)
    rows = []
    unresolved = 0
    notes = [session.provenance]
    try:
        for frame in frames:
            resolved = session.resolve(session.base + frame["offset"])
            label = f"exe+0x{frame['offset']:X}"
            rows.append(format_row(label, resolved))
            if not resolved["name"]:
                unresolved += 1
        if fatal:
            if "offset" in fatal:
                resolved = session.resolve(session.base + fatal["offset"])
                rows.append(format_row(f"FATAL 0x{fatal['address']:X}", resolved))
                if not resolved["name"]:
                    unresolved += 1
            elif dump and dump.get("module_base"):
                resolved = session.resolve(fatal["address"])
                rows.append(format_row(f"FATAL 0x{fatal['address']:X}", resolved))
                if not resolved["name"]:
                    unresolved += 1
            elif dump and dump.get("exception_address"):
                resolved = session.resolve(dump["exception_address"])
                rows.append(format_row(f"dump 0x{dump['exception_address']:X}", resolved))
                if not resolved["name"]:
                    unresolved += 1
            else:
                notes.append(
                    "NOTE: AbortCode FATAL prints the raw VA only; no module base, so "
                    "the handler should print exe+0x… for the faulting address (RTEError.cpp). Not done here."
                )
        if dump and dump.get("exception_address") and not (fatal and dump.get("module_base")):
            resolved = session.resolve(dump["exception_address"])
            rows.append(format_row(f"dump 0x{dump['exception_address']:X}", resolved))
            if not resolved["name"]:
                unresolved += 1
    finally:
        session.close()
    return {"rows": rows, "unresolved": unresolved, "notes": notes, "fatal": fatal, "frames": frames, "dump": dump}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--exe", required=True)
    parser.add_argument("--abort", required=True)
    parser.add_argument("--dump")
    parser.add_argument("--pdb", help="matching PDB, including a renamed retained copy")
    options = parser.parse_args()
    result = symbolize(options.exe, options.abort, options.dump, options.pdb)
    print("offset                  address           symbol (file:line)")
    for row in result["rows"]:
        print(row)
    for note in result["notes"]:
        print(note)
    return 1 if result["unresolved"] else 0


if __name__ == "__main__":
    raise SystemExit(main())
