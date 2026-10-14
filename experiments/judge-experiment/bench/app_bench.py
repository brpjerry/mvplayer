#!/usr/bin/env python3
"""The judge in the app, measured on a re-import of tracks the rules accepted.

Reads the library's import log and the sample drawn for the run, and writes an
HTML page: for every track of the sample, what the judge made of the video the
rules had kept (kept outright, sent to review, dropped), what it cost and how
long it took, and the disagreements listed for a human verdict.

    app_bench.py --log <mv dir>/.mvplayer/import-log.jsonl --sample sample.json \
                 --since 2026-10-11T19:38 --out report.html
"""
import argparse, html, json, sys
from collections import defaultdict

ap = argparse.ArgumentParser()
ap.add_argument('--log', required=True)
ap.add_argument('--sample', required=True)
ap.add_argument('--since', required=True, help='ISO time the run started; earlier events are ignored')
ap.add_argument('--out', required=True)
ap.add_argument('--title', default='The judge in the app')
args = ap.parse_args()

sample = json.load(open(args.sample, encoding='utf-8'))
by_track = {e['track']: e for e in sample}

# Files of one recording are one lookup, logged under one of their tracks:
# a sample track is matched by its id or by its recording.
lookups = {}          # sample track id -> last lookup event of the run
judge = defaultdict(list)  # logged trackId -> judge events
pauses = []
for line in open(args.log, encoding='utf-8'):
    try:
        d = json.loads(line)
    except ValueError:
        continue
    if d.get('t', '') < args.since:
        continue
    ev = d.get('event')
    if ev == 'lookup':
        for e in sample:
            if d.get('trackId') == e['track'] or (e.get('recording') and d.get('recording') == e['recording']):
                lookups[e['track']] = d
    elif ev == 'judge':
        judge[d.get('trackId')].append(d)
    elif ev == 'paused':
        pauses.append(d)

rows = []
for e in sample:
    tid = e['track']
    lk = lookups.get(tid)
    js = judge.get((lk or {}).get('trackId'), []) if lk else []
    verdicts = [j for j in js if j.get('question') == 'verdict']
    v = verdicts[-1] if verdicts else None
    ans = (v or {}).get('answer') or {}
    opts = {o.get('index'): o for o in ans.get('options', [])}
    chosen_idx = ans.get('chosen') or 0
    uploads = (v or {}).get('uploads') or []
    chosen_id = uploads[chosen_idx - 1].get('id') if 0 < chosen_idx <= len(uploads) else None
    prior_opt = None
    for i, u in enumerate(uploads, 1):
        if u.get('id') == e['yt']:
            prior_opt = opts.get(i)
    r = {
        'e': e,
        'lookup': lk,
        'outcome': (lk or {}).get('outcome'),
        'message': (lk or {}).get('message', ''),
        'prior_fate': ((lk or {}).get('prior') or {}).get('fate'),
        'verdict': ans.get('verdict'),
        'confidence': ans.get('confidence'),
        'chosen_id': chosen_id,
        'prior_opt': prior_opt,
        'uploads': len(uploads),
        'queries': len((lk or {}).get('queries') or []),
        'cost': sum(float(j.get('cost') or 0) for j in js),
        'judge_seconds': sum(float(j.get('seconds') or 0) for j in js),
        'seconds': float((lk or {}).get('seconds') or 0),
        'questions': len(js),
        'reason': ans.get('reason', ''),
    }
    if lk is None:
        r['class'] = 'pending'
    elif r['outcome'] == 'done' and r['prior_fate'] == 'kept':
        r['class'] = 'kept'
    elif r['outcome'] == 'done':
        r['class'] = 'replaced'
    elif r['outcome'] == 'review':
        r['class'] = 'review'
    elif r['outcome'] == 'not_found':
        r['class'] = 'dropped'
    else:
        r['class'] = r['outcome'] or 'other'
    rows.append(r)

counts = defaultdict(int)
for r in rows:
    counts[r['class']] += 1
done = [r for r in rows if r['class'] != 'pending']
cost = sum(r['cost'] for r in done)
secs = sum(r['seconds'] for r in done)
jsecs = sum(r['judge_seconds'] for r in done)
same_pick = sum(1 for r in done if r['chosen_id'] == r['e']['yt'])
disagree = [r for r in done if r['class'] != 'kept']

def esc(s):
    return html.escape(str(s if s is not None else ''))

def pct(n, d):
    return f'{100 * n / d:.0f}%' if d else '–'

def link(yt, text):
    return f'<a href="https://www.youtube.com/watch?v={esc(yt)}">{esc(text)}</a>'

label = {'kept': 'kept outright', 'review': 'to review', 'dropped': 'dropped', 'replaced': 'replaced', 'pending': 'not yet'}

out = []
out.append(f'''<!doctype html><html lang="en"><head><meta charset="utf-8"><title>{esc(args.title)}</title>
<style>
:root {{ --text:#1b1b1b; --muted:#666; --line:#ddd; --bg:#fff; --ok:#1a7f37; --warn:#9a6700; --bad:#b42318; --soft:#f5f5f5; }}
@media (prefers-color-scheme: dark) {{ :root:not([data-theme="light"]) {{ --text:#e6e6e6; --muted:#9a9a9a; --line:#333; --bg:#141414; --ok:#3fb950; --warn:#d29922; --bad:#f85149; --soft:#1f1f1f; }} }}
body {{ font: 15px/1.5 system-ui, sans-serif; color: var(--text); background: var(--bg); margin: 0 auto; max-width: 70rem; padding: 1.5rem 16px; }}
h1 {{ font-size: 1.5rem; }} h2 {{ font-size: 1.15rem; margin-top: 2rem; }}
table {{ border-collapse: collapse; width: 100%; font-size: 14px; }}
th, td {{ text-align: left; vertical-align: top; padding: .35rem .5rem; border-bottom: 1px solid var(--line); }}
th {{ color: var(--muted); font-weight: 600; }}
.num {{ text-align: right; font-variant-numeric: tabular-nums; }}
.kept {{ color: var(--ok); }} .review {{ color: var(--warn); }} .dropped, .replaced {{ color: var(--bad); }} .pending {{ color: var(--muted); }}
.note {{ color: var(--muted); font-size: 13px; }}
.sum {{ display: grid; grid-template-columns: repeat(auto-fit, minmax(10rem, 1fr)); gap: .6rem; margin: 1rem 0; }}
.sum div {{ background: var(--soft); border-radius: 6px; padding: .6rem .8rem; }}
.sum b {{ display: block; font-size: 1.4rem; }}
a {{ color: inherit; }}
</style></head><body>
<h1>{esc(args.title)}</h1>
<p>A re-import of {len(sample)} tracks whose video the rules had accepted outright, with the judge running the lookup (judge mode, Sonnet). The judge searched afresh, listened, looked at frames and gave its verdict on the video the track had beside whatever else it picked. Agreement means the judge kept that same video outright.</p>
<div class="sum">
<div>Judged<b>{len(done)} / {len(sample)}</b></div>
<div>Kept outright<b class="kept">{counts["kept"]}</b>{pct(counts["kept"], len(done))}</div>
<div>Sent to review<b class="review">{counts["review"]}</b>{pct(counts["review"], len(done))}</div>
<div>Dropped or replaced<b class="dropped">{counts["dropped"] + counts["replaced"]}</b>{pct(counts["dropped"] + counts["replaced"], len(done))}</div>
<div>Same upload chosen<b>{same_pick}</b>{pct(same_pick, len(done))}</div>
<div>Cost<b>${cost:.2f}</b>${cost / len(done) if done else 0:.3f} a track</div>
<div>Time<b>{secs / 60:.0f} min</b>{secs / len(done) if done else 0:.0f} s a track, {jsecs / len(done) if done else 0:.0f} s of it the judge</div>
<div>Pauses<b>{len(pauses)}</b></div>
</div>
''')

if disagree:
    out.append('<h2>Where the judge did not keep the video outright</h2><p class="note">For your verdict: the rules accepted these; the judge did not, or not confidently. Its note on the track\'s video is quoted where it saw it.</p><table><tr><th>Track</th><th>The track\'s video</th><th>Judge</th><th>Note</th></tr>')
    for r in disagree:
        e = r['e']
        po = r['prior_opt'] or {}
        conf = f'{r["confidence"]:.2f}' if isinstance(r['confidence'], (int, float)) else ''
        what = f'<span class="{r["class"]}">{label.get(r["class"], r["class"])}</span> · {esc(r["verdict"])} {conf}'
        if r['chosen_id'] and r['chosen_id'] != e['yt']:
            what += f'<br><span class="note">chose {link(r["chosen_id"], r["chosen_id"])} instead</span>'
        note = esc(po.get('note') or r['reason'] or r['message'])
        if po:
            note = f'{esc(po.get("kind"))} · {esc(po.get("source"))} · {esc(po.get("version"))} · {esc(po.get("whole"))} · {po.get("confidence", 0):.2f}<br>' + note
        out.append(f'<tr><td>{esc(e["artist"])}<br>{esc(e["title"])}</td><td>{link(e["yt"], e["channel"] or e["yt"])}<br><span class="note">{esc(e["yt"])}</span></td><td>{what}</td><td class="note">{note}</td></tr>')
    out.append('</table>')

out.append('<h2>Every track</h2><table><tr><th>Track</th><th>Video</th><th>Judge</th><th class="num">Uploads</th><th class="num">Questions</th><th class="num">Cost</th><th class="num">Seconds</th></tr>')
for r in rows:
    e = r['e']
    conf = f' {r["confidence"]:.2f}' if isinstance(r['confidence'], (int, float)) else ''
    cost_cell = f'${r["cost"]:.3f}' if r['cost'] else ''
    out.append(f'<tr><td>{esc(e["artist"])} — {esc(e["title"])}<br><span class="note">{esc(e["side"])}</span></td><td>{link(e["yt"], e["channel"] or e["yt"])}</td>'
               f'<td class="{r["class"]}">{label.get(r["class"], r["class"])}{conf}</td><td class="num">{r["uploads"] or ""}</td><td class="num">{r["questions"] or ""}</td>'
               f'<td class="num">{cost_cell}</td><td class="num">{r["seconds"]:.0f}</td></tr>')
out.append('</table>')

if pauses:
    out.append('<h2>Pauses during the run</h2><ul>')
    for p in pauses:
        out.append(f'<li>{esc(p.get("t", ""))} — {esc(p.get("reason", ""))}</li>')
    out.append('</ul>')
out.append('</body></html>')
open(args.out, 'w', encoding='utf-8').write('\n'.join(out))
print(f'{len(done)}/{len(sample)} judged: kept {counts["kept"]}, review {counts["review"]}, dropped {counts["dropped"]}, replaced {counts["replaced"]}, pending {counts["pending"]}; ${cost:.2f}; {len(pauses)} pauses')
