#!/usr/bin/env python3
# Per-op cross-platform divergence summary of two luaprobe outputs.
# Lines: op,index,argbits,resultbits  (raw 8-byte LE hex of doubles).
# Usage: luaprobe_diff.py <linux.txt> <arm64.txt>
import sys, struct
def decode(hx):  # 8-byte LE hex -> double
    return struct.unpack('<d', bytes.fromhex(hx))[0]
def load(p):
    d = {}
    for ln in open(p):
        ln = ln.strip()
        if not ln: continue
        op, i, a, r = ln.split(',')
        d[(op, int(i))] = (a, r)
    return d
A = load(sys.argv[1]); B = load(sys.argv[2])
keys = sorted(set(A) & set(B))
ops = {}
for k in keys:
    op = k[0]
    a_arg, a_res = A[k]; b_arg, b_res = B[k]
    s = ops.setdefault(op, {'total':0,'argdiff':0,'resdiff':0,'ex':None})
    s['total'] += 1
    if a_arg != b_arg:
        s['argdiff'] += 1
    if a_res != b_res:
        s['resdiff'] += 1
        if s['ex'] is None:
            s['ex'] = (decode(a_arg), a_res, b_res)
print(f"{'op':10s} {'verdict':10s} {'res-diff/total':16s} example (arg: linux_res vs arm64_res)")
print('-'*92)
divergent = []
for op in ['sin','cos','tan','asin','acos','atan','tanh','exp','log','sqrt','powfn2.5','pow_op25','pow_op3']:
    if op not in ops: continue
    s = ops[op]
    if s['resdiff'] == 0:
        v = 'IDENTICAL'
    else:
        v = 'DIVERGES'; divergent.append(op)
    ex = ''
    if s['ex']:
        arg, ar, br = s['ex']
        ex = f"x={arg:.9g}: L={decode(ar):.17g} A={decode(br):.17g} (0x{ar} vs 0x{br})"
    note = f"  [arg-bits differ on {s['argdiff']}!]" if s['argdiff'] else ""
    print(f"{op:10s} {v:10s} {str(s['resdiff'])+'/'+str(s['total']):16s} {ex}{note}")
print()
print("DIVERGENT OP SET (Linux vs arm64):", divergent if divergent else "(none — all identical)")
