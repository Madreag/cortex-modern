"""Read a raw contract-audit state document as records rather than as lines.

ContractAudit::Write emits `key + " = " + value + "\\n"` (ContractAudit.h:1645) and a string value rides
std::quoted (ContractAudit.h:89), which escapes only the quote and the escape character: a newline or a
NUL inside a string is written through unchanged, and a container renders its elements the same way, so
a record ends at the first newline outside a quoted run, not at the first newline. A key carries quoted
runs too, because a map element's path is `...[key="<key>"]`.
"""

import gzip
import re

_ESCAPE = re.compile(rb"\\.", re.S)
_SEPARATOR = b" = "


def open_document(path):
    """Documents are retained gzipped and written plain."""
    return gzip.open(path, "rb") if str(path).endswith(".gz") else open(path, "rb")


def records(stream):
    """Yield every record of the document, each with the bytes the writer wrote for it."""
    pending, open_quote = [], False
    for line in stream:
        if b'"' in line:
            open_quote ^= bool(_ESCAPE.sub(b"", line).count(b'"') & 1)
        pending.append(line)
        if open_quote:
            continue
        yield pending[0] if len(pending) == 1 else b"".join(pending)
        pending = []
    if pending:
        # A quoted run the writer never closed: hand the tail back so the caller reports it.
        yield b"".join(pending)


def field(record):
    """The path a record names, or None when it holds no separator outside its quoted runs."""
    position = 0
    while True:
        index = record.find(_SEPARATOR, position)
        if index < 0:
            return None
        head = record[:index]
        if b'"' not in head or not _ESCAPE.sub(b"", head).count(b'"') & 1:
            return head.decode("utf-8", "replace")
        position = index + 1


def single_line(record):
    """True when the whole record is one line, so no quoted run can hide a field inside it."""
    return record.count(b"\n") <= (1 if record.endswith(b"\n") else 0)


def text(record):
    """The record as text, without the newline the writer terminated it with."""
    return record.decode("utf-8", "replace").removesuffix("\n")
