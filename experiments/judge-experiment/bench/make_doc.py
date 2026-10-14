# Builds docs/import-rules-judge.html from the body template, the stylesheet of
# the main rules page, the prompts as adopted, and the measured results.
# Usage: python3 make_doc.py --final v3 --query v1 --pick v1 --gate 0.9 [--search-tag claude-haiku-5-5-v1-low]
import json, os, sys, glob, argparse, html, string, datetime, collections
sys.path.insert(0, os.path.dirname(__file__))
import claude, prompts

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.abspath(f'{HERE}/../../..')
SNAP = os.path.expanduser('~/.cache/mvplayer-judge/queue-2026-10-08')

ap = argparse.ArgumentParser()
ap.add_argument('--final', default='v3')
ap.add_argument('--query', default='v1')
ap.add_argument('--pick', default='v1')
ap.add_argument('--gate', type=float, default=0.9)
ap.add_argument('--out', default=f'{ROOT}/docs/import-rules-judge.html')
args = ap.parse_args()

cases = {c['group']: c for c in json.load(open(f'{SNAP}/cases.json'))}
labels = {l['group']: l for l in json.load(open(f'{SNAP}/labels.json'))}
NAMES = {'claude-haiku-5-5': 'Haiku 5.5', 'claude-sonnet-5-5': 'Sonnet 5.5', 'claude-opus-5-5': 'Opus 5.5'}


def gated(c, a):
    if a['verdict'] != 'accept' or not (1 <= a.get('chosen', 0) <= len(c['options'])):
        return False
    o = c['options'][a['chosen'] - 1]
    return o['duration'] < 0.9 * c['tracks'][0]['duration'] or o['measured'].get('interruptedSec', 0) > 15


def score(path):
    results = json.load(open(path))
    n = ok = unsure = auto = auto_ok = review = cost = 0
    secs = []
    for r in results:
        a = r['answer']
        if 'error' in a:
            continue
        c, l = cases[r['group']], labels[r['group']]
        cost += claude.cost_of(json.load(open(path))[0].get('model', '') or '', {}) if False else 0
        if l['verdict'] == 'partial':
            continue
        n += 1
        agree = (a['verdict'] == 'accept' and a.get('chosen') == l.get('chosen')) if l['verdict'] == 'accept' else a['verdict'] == 'reject all'
        ok += agree
        if a['verdict'] == 'unsure':
            unsure += 1
        if a['verdict'] != 'unsure' and a['confidence'] >= args.gate and not gated(c, a):
            auto += 1
            auto_ok += agree
        else:
            review += 1
        if not r['cached']:
            secs.append(r['seconds'])
    return dict(n=n, ok=ok, unsure=unsure, auto=auto, auto_ok=auto_ok, review=review, secs=secs, results=results)


rows = []
for path in sorted(glob.glob(f'{claude.CACHE}/results/final-*.json')):
    tag = os.path.basename(path)[6:-5]
    model = next((m for m in NAMES if tag.startswith(m)), None)
    if not model:
        continue
    rest = tag[len(model) + 1:]
    version = rest.split('-')[0]
    s = score(path)
    usage = [r['usage'] for r in s['results'] if r['usage']]
    cost = sum(claude.cost_of(model, u) for u in usage) / max(1, len(usage))
    tin = sum(u.get('input_tokens', 0) for u in usage) / max(1, len(usage))
    rows.append((version, model, s, cost, tin))
rows.sort(key=lambda r: (r[0], list(NAMES).index(r[1])))
adopted = args.final
measured = ['<table><tr><th>Words</th><th>Model</th><th>Agrees with you</th><th>Decided alone at the gate</th><th>Of those, right</th><th>Left for your verdict</th><th>Per group</th></tr>']
for version, model, s, cost, tin in rows:
    mark = ' <b>(adopted)</b>' if version == adopted else ''
    measured.append(f"<tr><td class=\"n\">{version}{mark}</td><td>{NAMES[model]}</td><td class=\"n\">{s['ok']}/{s['n']} ({s['ok'] / max(1, s['n']) * 100:.0f}%)</td>"
                    f"<td class=\"n\">{s['auto']} ({s['auto'] / max(1, s['n']) * 100:.0f}%)</td><td class=\"n\">{s['auto_ok']}/{s['auto']} ({s['auto_ok'] / max(1, s['auto']) * 100:.0f}%)</td>"
                    f"<td class=\"n\">{s['review']}</td><td class=\"n\">${cost:.3f} · {tin / 1000:.1f}k tokens in</td></tr>")
measured.append('</table>')

# the search stage
search_rows = []
for path in sorted(glob.glob(f'{claude.CACHE}/results/search-*.json')):
    tag = os.path.basename(path)[7:-5]
    model = next((m for m in NAMES if tag.startswith(m)), None)
    if not model:
        continue
    sversion = tag[len(model) + 1:].split('-')[0]
    d = json.load(open(path))
    by = collections.defaultdict(list)
    for s, r in zip(d['sample'], d['results']):
        by[s['set']].append((s, r))
    cells = {}
    for name, rs in by.items():
        picks = [(s, [p['id'] for p in r.get('pick', {}).get('picks', [])], r) for s, r in rs]
        if name in ('hand', 'auto'):
            found = sum(1 for s, p, r in picks if any(x['id'] in s['good'] for x in r.get('results', [])))
            first = sum(1 for s, p, r in picks if p[:1] and p[0] in s['good'])
            anywhere = sum(1 for s, p, r in picks if any(x in s['good'] for x in p))
            skipped = sum(1 for s, p, r in picks if not r['query'].get('lookup', True))
            cells[name] = f"{len(rs)} tracks: the search found the kept upload for {found}, the judge picked it first for {first}, at all for {anywhere}; not searched for {skipped}"
        elif name == 'rejected':
            none = sum(1 for s, p, r in picks if not p)
            bad = sum(1 for s, p, r in picks if any(x in s['bad'] for x in p))
            cells[name] = f"{len(rs)} tracks: picked nothing for {none}, picked an upload you had turned down for {bad}"
        else:
            none = sum(1 for s, p, r in picks if not p)
            cells[name] = f"{len(rs)} tracks: picked nothing for {none}, something to listen to for {len(rs) - none}"
    npicks = sum(len(r.get('pick', {}).get('picks', [])) for r in d['results']) / max(1, len(d['results']))
    cost = sum(r['cost'] for r in d['results']) / max(1, len(d['results']))
    smark = ' <b>(adopted)</b>' if sversion == args.query else ''
    search_rows.append(f"<tr><td class=\"n\">{sversion}{smark}</td><td>{NAMES[model]}</td><td>{cells.get('hand', '')}</td><td>{cells.get('auto', '')}</td><td>{cells.get('rejected', '')}</td><td>{cells.get('notfound', '')}</td><td class=\"n\">{npicks:.1f} · ${cost:.4f}</td></tr>")
search_table = ('<table><tr><th>Words</th><th>Model</th><th>Tracks whose video you accepted</th><th>Tracks the rules accepted outright</th><th>Tracks you rejected every option of</th><th>Tracks the rules found nothing for</th><th>Picks per track · cost</th></tr>'
                + ''.join(search_rows) + '</table>') if search_rows else '<p class="lede">Not measured yet.</p>'

n_lab = sum(1 for l in labels.values() if l['verdict'] != 'pending')
n_acc = sum(1 for l in labels.values() if l['verdict'] == 'accept')
n_rej = sum(1 for l in labels.values() if l['verdict'] == 'reject all')
n_part = sum(1 for l in labels.values() if l['verdict'] == 'partial')
n_opt = sum(len(cases[g]['options']) for g, l in labels.items() if l['verdict'] != 'pending')

body = string.Template(open(f'{HERE}/doc-body.html').read()).substitute(
    style=open(f'{HERE}/style.css.fragment').read(),
    date=datetime.date.today().isoformat(),
    gate=f'{args.gate:.2f}', gate_pct=f'{args.gate * 100:.0f}%',
    prompt_query=html.escape(prompts.QUERY[args.query]), prompt_pick=html.escape(prompts.PICK[args.pick]), prompt_final=html.escape(prompts.FINAL[args.final]),
    measured='\n'.join(measured), search_table=search_table,
    n_lab=n_lab, n_acc=n_acc, n_rej=n_rej, n_part=n_part, n_opt=n_opt, spent=f'{claude.spent():.2f}')
open(args.out, 'w').write(body)
print(f'wrote {args.out}: {len(body)} bytes; {len(rows)} measured rows; spent ${claude.spent():.2f}')
