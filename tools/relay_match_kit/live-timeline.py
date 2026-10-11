"""Strict full-tick timelines; optionally compare all three relay participants.
Usage: live-timeline.py LIVE [LIVE ...] --out summary.json
Reports every >50-ms visible frame gap, paused rows, full-match/60-second rates and all common hashes.
This analyzer never labels a failed or incomplete probe as a successful match.
"""
from pathlib import Path
import argparse
import bisect
import json
import statistics

def read(path):
    rows = [json.loads(line) for line in Path(path).read_text(encoding='utf-8-sig').splitlines() if line.strip()]
    if not rows: raise ValueError(f'No live rows: {path}')
    for a,b in zip(rows,rows[1:]):
        if b['tick'] != a['tick'] + 1 or b['wall_ms'] < a['wall_ms']:
            raise ValueError(f'Nonconsecutive or reversed live timeline: {path} at {b["tick"]}')
    return rows

def summarize(path):
    rows = read(path)
    playing = [r for r in rows if r.get('paused') is False and r.get('player_visible', True)]
    gaps = [{'ending_tick':b['tick'], 'gap_ms':b['wall_ms']-a['wall_ms'], 'in_play':not a['paused'] and not b['paused']}
            for a,b in zip(rows,rows[1:]) if b['wall_ms']-a['wall_ms'] > 50]
    span = (rows[-1]['wall_ms']-rows[0]['wall_ms'])/1000
    play_span = (playing[-1]['wall_ms']-playing[0]['wall_ms'])/1000 if len(playing)>1 else 0
    windows=[];j=0
    for i in range(len(playing)):
        j=max(j,i+1)
        while j<len(playing) and playing[j]['wall_ms']-playing[i]['wall_ms']<60000: j+=1
        if j==len(playing):break
        windows.append({'from_tick':playing[i]['tick'], 'tps':1000*(playing[j]['tick']-playing[i]['tick'])/(playing[j]['wall_ms']-playing[i]['wall_ms'])})
    times=[r['wall_ms'] for r in playing]
    consecutive=[]
    boundary=times[0] if times else 0
    while times and boundary+60000<=times[-1]:
        i=bisect.bisect_left(times,boundary)
        j=bisect.bisect_left(times,boundary+60000)
        elapsed=(times[j]-times[i])/1000
        consecutive.append({'from_tick':playing[i]['tick'],'through_tick':playing[j]['tick'],
                            'span_s':elapsed,'tps':(playing[j]['tick']-playing[i]['tick'])/elapsed})
        boundary+=60000
    intervals=[b['wall_ms']-a['wall_ms'] for a,b in zip(rows,rows[1:])
               if not a['paused'] and not b['paused'] and a.get('player_visible',True) and b.get('player_visible',True)]
    grouped=[(playing[i+60]['wall_ms']-playing[i]['wall_ms'])/(playing[i+60]['tick']-playing[i]['tick'])
             for i in range(0,len(playing)-60,60)]
    return rows, {'path':str(path),'peer':rows[0]['peer'],'rows':len(rows),'first_tick':rows[0]['tick'],'last_tick':rows[-1]['tick'],
        'span_s':span,'tps':(rows[-1]['tick']-rows[0]['tick'])/span if span else None,'paused_rows':sum(r['paused'] for r in rows),
        'in_play_first':playing[0]['tick'] if playing else None,'in_play_span_s':play_span,
        'in_play_tps':(playing[-1]['tick']-playing[0]['tick'])/play_span if play_span else None,
        'consecutive_60s':consecutive,
        'median_single_tick_interval_ms':statistics.median(intervals) if intervals else None,
        'median_60_tick_interval_ms':statistics.median(grouped) if grouped else None,
        'gaps_over_50ms':gaps,'worst_60s':min(windows,key=lambda x:x['tps']) if windows else None,
        'best_60s':max(windows,key=lambda x:x['tps']) if windows else None}

def main():
    p=argparse.ArgumentParser(description=__doc__);p.add_argument('live',type=Path,nargs='+');p.add_argument('--out',type=Path)
    a=p.parse_args(); pairs=[summarize(path) for path in a.live]
    indexes=[{r['tick']:r for r in rows} for rows,_ in pairs]
    common=sorted(set.intersection(*(set(i) for i in indexes)))
    mismatches=[t for t in common if len({i[t]['total'] for i in indexes}) != 1]
    result={'peers':[s for _,s in pairs],'common_ticks':len(common),'hash_mismatches':mismatches,
            'complete_three_peer_evidence':len(pairs)==3 and len({s['peer'] for _,s in pairs})==3,
            'match_pass':None,'remaining':'The relay gate also requires successful probes, all nine placement confirmations, relay routes, a host hold and automatic return.'}
    if a.out:a.out.write_text(json.dumps(result,indent=2)+'\n')
    print(json.dumps(result,indent=2));return int(bool(mismatches))
if __name__=='__main__':raise SystemExit(main())
