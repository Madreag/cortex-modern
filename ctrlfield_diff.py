#!/usr/bin/env python3
# Byte-diff two controller-field dumps by (tick,uniqueID) -> first diverging field.
# Usage: ctrlfield_diff.py <linux.csv> <arm64.csv>
import sys, struct
FIELDS = ['moveX','moveY','aimX','aimY','cursorX','cursorY']
def decode(hexbits):
    return struct.unpack('<f', struct.pack('<I', int(hexbits, 16)))[0]
def load(p):
    rows = {}
    order = []
    for ln in open(p):
        ln = ln.strip()
        if not ln: continue
        parts = ln.split(',')
        if len(parts) != 8: continue
        tick = int(parts[0]); uid = int(parts[1]); bits = parts[2:8]
        rows[(tick, uid)] = bits
        order.append((tick, uid))
    return rows, order
A, oa = load(sys.argv[1])
B, ob = load(sys.argv[2])
print(f"linux rows={len(A)} arm64 rows={len(B)}")
keys = sorted(set(A) & set(B))
onlyA = sorted(set(A)-set(B)); onlyB = sorted(set(B)-set(A))
if onlyA or onlyB:
    print(f"  key mismatch: only-linux={onlyA[:5]} only-arm64={onlyB[:5]}")
first = None
per_tick = {}
for (tick, uid) in keys:
    for i, f in enumerate(FIELDS):
        if A[(tick,uid)][i] != B[(tick,uid)][i]:
            per_tick.setdefault(tick, []).append((uid, f, A[(tick,uid)][i], B[(tick,uid)][i]))
            if first is None:
                first = (tick, uid, f, A[(tick,uid)][i], B[(tick,uid)][i])
if not first:
    print("NO FIELD DIVERGENCE in common rows (linux==arm64 for all dumped analog fields)")
else:
    t,u,f,a,b = first
    print(f"\nFIRST DIVERGENCE: tick={t} uniqueID={u} field={f}")
    print(f"   linux=0x{a} ({decode(a):.9g})   arm64=0x{b} ({decode(b):.9g})")
    print("\nall diverging fields by tick:")
    for tick in sorted(per_tick):
        for uid,f,a,b in per_tick[tick]:
            print(f"   t{tick} uid={uid} {f}: L=0x{a}({decode(a):.6g}) A=0x{b}({decode(b):.6g})")
