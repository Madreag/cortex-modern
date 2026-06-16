#!/usr/bin/env python3
# Diff two CONTROLSTATE dumps by (tick,uniqueID) -> first divergent state bit (named) or inputMode.
# Row: tick,uniqueID,<57-char 0/1 bitmap>,inputMode
import sys
NAMES = ["PRIMARY_ACTION","SECONDARY_ACTION","MOVE_IDLE","MOVE_RIGHT","MOVE_LEFT","MOVE_UP","MOVE_DOWN",
"MOVE_FAST","MOVE_FAST_TOGGLE","BODY_JUMPSTART","BODY_JUMP","BODY_CROUCH","BODY_PRONE","AIM_UP","AIM_DOWN",
"AIM_SHARP","WEAPON_FIRE","WEAPON_RELOAD","WEAPON_RELOADHELD","PIE_MENU_OPENED","PIE_MENU_ACTIVE",
"PIE_MENU_ACTIVE_ANALOG","PIE_MENU_ACTIVE_DIGITAL","WEAPON_CHANGE_NEXT","WEAPON_CHANGE_PREV","WEAPON_PICKUP",
"WEAPON_DROP","WEAPON_PRIMARY_HOTKEYSTART","WEAPON_AUXILIARY_HOTKEYSTART","ACTOR_PRIMARY_HOTKEYSTART",
"ACTOR_AUXILIARY_HOTKEYSTART","WEAPON_PRIMARY_HOTKEY","WEAPON_AUXILIARY_HOTKEY","ACTOR_PRIMARY_HOTKEY",
"ACTOR_AUXILIARY_HOTKEY","ACTOR_NEXT","ACTOR_PREV","ACTOR_BRAIN","ACTOR_NEXT_PREP","ACTOR_PREV_PREP",
"HOLD_RIGHT","HOLD_LEFT","HOLD_UP","HOLD_DOWN","PRESS_PRIMARY","PRESS_SECONDARY","PRESS_RIGHT","PRESS_LEFT",
"PRESS_UP","PRESS_DOWN","RELEASE_PRIMARY","RELEASE_SECONDARY","PRESS_FACEBUTTON","RELEASE_FACEBUTTON",
"SCROLL_UP","SCROLL_DOWN","DEBUG_ONE"]
def load(p):
    d={}
    for ln in open(p):
        ln=ln.strip()
        if not ln: continue
        parts=ln.split(',')
        if len(parts)<4: continue
        d[(int(parts[0]),int(parts[1]))]=(parts[2],parts[3])
    return d
def name(i): return NAMES[i] if i<len(NAMES) else f"bit{i}"
A=load(sys.argv[1]); B=load(sys.argv[2])
keys=sorted(set(A)&set(B))
print(f"rows A={len(A)} B={len(B)} common={len(keys)}")
first=None; alldiffs=[]
for k in keys:
    (ba,ia),(bb,ib)=A[k],B[k]
    diffs=[]
    for i in range(min(len(ba),len(bb))):
        if ba[i]!=bb[i]: diffs.append((name(i),ba[i],bb[i]))
    if ia!=ib: diffs.append((f"inputMode",ia,ib))
    if diffs:
        alldiffs.append((k,diffs))
        if first is None: first=(k,diffs)
if not first:
    print("NO STATE/inputMode divergence in common rows (identical)")
else:
    (t,u),diffs=first
    print(f"\nFIRST divergence: tick={t} uniqueID={u}")
    for nm,va,vb in diffs: print(f"   {nm}: A={va} B={vb}")
    print(f"\nall divergent (tick,uid): {len(alldiffs)}")
    for (t,u),diffs in alldiffs[:12]:
        print(f"   t{t} uid={u}: " + ", ".join(f"{nm}({va}->{vb})" for nm,va,vb in diffs))
