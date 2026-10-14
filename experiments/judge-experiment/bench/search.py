# Stage-S benchmark: the judge writes the search query, the search is run as a
# guest, and the judge picks what to download. Scored by whether the upload the
# library ended up keeping (by your verdict, or by the rules outright) is among
# the picks. Usage:
#   python3 search.py --model claude-haiku-5-5 [--prompt v1] [--effort low] [--sets hand,auto,rejected,notfound] [--limit N] [--workers 4]
import json, os, sys, argparse, sqlite3, subprocess, hashlib, random, collections, time
from concurrent.futures import ThreadPoolExecutor
sys.path.insert(0, os.path.dirname(__file__))
import claude, prompts

SNAP = os.path.expanduser('~/.cache/mvplayer-judge/queue-2026-10-08')
DB = os.path.expanduser('~/Videos/MVs/.mvplayer/library.db')
SEARCHES = f'{claude.CACHE}/search'
os.makedirs(SEARCHES, exist_ok=True)

ap = argparse.ArgumentParser()
ap.add_argument('--model', required=True)
ap.add_argument('--prompt', default='v1')
ap.add_argument('--effort', default=None)
ap.add_argument('--sets', default='hand,auto,rejected,notfound')
ap.add_argument('--limit', type=int, default=0)
ap.add_argument('--workers', type=int, default=1)
ap.add_argument('--pace', type=float, default=4.0, help='seconds between searches')
ap.add_argument('--cookies', default=os.path.expanduser('~/.config/mvplayer/cookies.txt'), help="the app's cookie file; '' to search as a guest")
ap.add_argument('--show', action='store_true')
args = ap.parse_args()
PACE = args.pace
COOKIES = ['--cookies', args.cookies] if args.cookies and os.path.exists(args.cookies) else []

db = sqlite3.connect(f'file:{DB}?mode=ro', uri=True)
db.row_factory = sqlite3.Row
TRACK_COLS = 'id, title, artist, album_artist, album, year, duration, codec, lossless, bitrate, genre, path'


def track(tid):
    r = db.execute(f'select {TRACK_COLS} from tracks where id=?', (tid,)).fetchone()
    return dict(r) if r else None


# ---- the sample: (set, track, good ids, bad ids) ------------------------------
cases = json.load(open(f'{SNAP}/cases.json'))
labels = {l['group']: l for l in json.load(open(f'{SNAP}/labels.json'))}
sample = []
rng = random.Random(7)
for c in cases:
    l = labels[c['group']]
    if l['verdict'] == 'accept':
        chosen = c['options'][l['chosen'] - 1]
        # the track that has the video now, else the first candidate
        r = db.execute('select t.id from tracks t join videos v on v.id=t.video_id where v.yt_id=? and t.absent=0', (chosen['ytId'],)).fetchone()
        t = track(r['id']) if r else (track(c['tracks'][0]['id']) if c['tracks'] else None)
        if t:
            sample.append({'set': 'hand', 'group': c['group'], 'track': t, 'good': [chosen['ytId']], 'bad': [o['ytId'] for o in c['options'] if o is not chosen]})
    elif l['verdict'] == 'reject all' and c['tracks']:
        t = track(c['tracks'][0]['id'])
        if t:
            sample.append({'set': 'rejected', 'group': c['group'], 'track': t, 'good': [], 'bad': [o['ytId'] for o in c['options']]})
auto = [dict(r) for r in db.execute(f"select t.id as tid, v.yt_id from tracks t join videos v on v.id=t.video_id where t.absent=0 and v.review=0 and v.vouched=0")]
rng.shuffle(auto)
for r in auto[:60]:
    sample.append({'set': 'auto', 'group': 0, 'track': track(r['tid']), 'good': [r['yt_id']], 'bad': []})
nf = [dict(r) for r in db.execute("select id from tracks where state='not_found' and absent=0")]
rng.shuffle(nf)
for r in nf[:25]:
    sample.append({'set': 'notfound', 'group': 0, 'track': track(r['id']), 'good': [], 'bad': []})
sets = set(args.sets.split(','))
sample = [s for s in sample if s['set'] in sets]
if args.limit:
    by = collections.defaultdict(list)
    for s in sample:
        by[s['set']].append(s)
    sample = [s for k in by for s in by[k][:args.limit]]


def fmt_track(t):
    audio = 'lossless' if t.get('lossless') else f"{t.get('codec') or '?'} {t.get('bitrate') or '?'} kbit/s"
    year = f", released {t['year']}" if t.get('year') else ''
    artist = t['artist'] if t['artist'] == t['album_artist'] else f"{t['artist']} (album artist {t['album_artist']})"
    genre = f", genre tag “{t['genre']}”" if t.get('genre') else ''
    folder = '/'.join(t['path'].split('/')[-3:-1])
    return f"“{t['title']}” by {artist}, album “{t['album']}”{year}, {t['duration']:.0f} s, {audio}{genre}; file under “{folder}”"


def search(q):
    h = hashlib.sha256(q.encode()).hexdigest()[:20]
    path = f'{SEARCHES}/{h}.json'
    if os.path.exists(path):
        return json.load(open(path))
    p = subprocess.run(['yt-dlp', *COOKIES, f'ytsearch10:{q}', '--flat-playlist', '--no-warnings', '--print',
                        '%(.{id,title,channel,channel_id,duration,view_count,channel_is_verified})j'], capture_output=True, text=True, timeout=120)
    if p.returncode != 0 and 'bot' in p.stderr:
        raise RuntimeError('bot check on search: ' + p.stderr[:200])
    rows = []
    for l in p.stdout.splitlines():
        try:
            rows.append(json.loads(l))
        except ValueError:
            pass
    time.sleep(PACE)
    if not rows:
        # YouTube answers a guest it has had enough of with an empty list, not an error. A query that
        # really has no results is told apart by one that always has: throttled, the run waits.
        for attempt in range(12):
            probe = subprocess.run(['yt-dlp', *COOKIES, 'ytsearch1:YOASOBI アイドル', '--flat-playlist', '--no-warnings', '--print', '%(id)s'], capture_output=True, text=True, timeout=120)
            if probe.stdout.strip():
                break
            print(f'throttled at “{q}”: waiting 10 min ({attempt + 1})', file=sys.stderr, flush=True)
            time.sleep(600)
        else:
            raise RuntimeError('still throttled after two hours')
        if attempt:
            return search(q)  # throttled, now back: ask again
    json.dump({'q': q, 'results': rows, 'error': p.stderr[-300:] if p.returncode else ''}, open(path, 'w'), ensure_ascii=False)
    return json.load(open(path))


def views(n):
    if not n:
        return 'views unknown'
    return f"{n / 1e6:.1f}M views" if n >= 1e6 else f"{n / 1e3:.0f}K views" if n >= 1e3 else f"{n} views"


def run(s):
    try:
        return run1(s)
    except Exception as e:
        return {'set': s['set'], 'group': s['group'], 'trackId': s['track']['id'], 'query': {'error': str(e)}, 'cost': 0}


def run1(s):
    t = s['track']
    q_ans, q_usage, _, _ = claude.ask(args.model, prompts.QUERY[args.prompt], [{'type': 'text', 'text': f"Track: {fmt_track(t)}."}],
                                      prompts.SCHEMA_QUERY, effort=args.effort, tag=f'query-{args.prompt}', max_tokens=1500)
    out = {'set': s['set'], 'group': s['group'], 'trackId': t['id'], 'query': q_ans, 'cost': claude.cost_of(args.model, q_usage)}
    if 'error' in q_ans or not q_ans.get('lookup') or not q_ans.get('queries'):
        return out
    results, seen = [], set()
    for qi, q in enumerate(q_ans['queries'][:2]):
        for rank, r in enumerate(search(q)['results'], 1):
            if r['id'] not in seen:
                seen.add(r['id'])
                results.append(dict(r, query=qi + 1, rank=rank))
        if qi == 0 and any(x['id'] in s['good'] for x in results):
            break  # the second query is only for when the first finds nothing: judged on the first alone where it did
    out['results'] = results
    if not results:
        return out
    listing = '\n'.join(f"{i}. id {r['id']}: “{r['title']}” — channel “{r['channel']}”{' ✓verified' if r.get('channel_is_verified') else ''}, {r['duration'] or 0:.0f} s, {views(r.get('view_count'))} (query {r['query']}, result {r['rank']})"
                        for i, r in enumerate(results, 1))
    p_ans, p_usage, _, _ = claude.ask(args.model, prompts.PICK[args.prompt],
                                      [{'type': 'text', 'text': f"Track: {fmt_track(t)}.\n\nSearch results:\n{listing}\n\nWhich uploads should be downloaded and listened to, best first?"}],
                                      prompts.SCHEMA_PICK, effort=args.effort, tag=f'pick-{args.prompt}', max_tokens=1500)
    out['pick'] = p_ans
    out['cost'] += claude.cost_of(args.model, p_usage)
    return out


with ThreadPoolExecutor(args.workers) as pool:
    results = list(pool.map(run, sample))
tag = f"{args.model}-{args.prompt}" + (f"-{args.effort}" if args.effort else '')
os.makedirs(f'{claude.CACHE}/results', exist_ok=True)
json.dump({'sample': sample, 'results': results}, open(f'{claude.CACHE}/results/search-{tag}.json', 'w'), ensure_ascii=False, indent=1)

print(f"\n== search {tag}: {len(results)} tracks, ${sum(r['cost'] for r in results):.3f}")
for name in ('hand', 'auto', 'rejected', 'notfound'):
    rs = [(s, r) for s, r in zip(sample, results) if s['set'] == name]
    if not rs:
        continue
    skipped = sum(1 for s, r in rs if not r['query'].get('lookup', True))
    picked = [(s, r, [p['id'] for p in r.get('pick', {}).get('picks', [])]) for s, r in rs]
    n_picks = [len(p) for s, r, p in picked]
    if name in ('hand', 'auto'):
        in_results = sum(1 for s, r, p in picked if any(x['id'] in s['good'] for x in r.get('results', [])))
        first = sum(1 for s, r, p in picked if p[:1] and p[0] in s['good'])
        anywhere = sum(1 for s, r, p in picked if any(x in s['good'] for x in p))
        print(f"  {name:9} {len(rs):3}: the kept upload was in the results {in_results}, picked first {first}, picked at all {anywhere}; skipped {skipped}; picks/track {sum(n_picks) / len(rs):.1f}")
    else:
        none = sum(1 for s, r, p in picked if not p)
        bad = sum(1 for s, r, p in picked if any(x in s['bad'] for x in p))
        print(f"  {name:9} {len(rs):3}: picked nothing {none}, picked an upload you turned down {bad}; skipped {skipped}; picks/track {sum(n_picks) / len(rs):.1f}")

print("\nMisses" + (" and everything else" if args.show else "") + ":")
for s, r in zip(sample, results):
    t = s['track']
    picks = [p['id'] for p in r.get('pick', {}).get('picks', [])]
    hit = any(x in s['good'] for x in picks)
    bad = any(x in s['bad'] for x in picks)
    if s['set'] in ('hand', 'auto') and hit and not args.show:
        continue
    if s['set'] == 'rejected' and not bad and not args.show:
        continue
    if s['set'] == 'notfound' and not args.show and not picks:
        continue
    q = r['query']
    print(f"[{s['set']}] {t['album_artist']} – {t['title']} ({t['duration']:.0f}s) good={s['good']} | queries {q.get('queries')} skip={q.get('skip_reason', '')} | known: {q.get('song_known', '')[:120]}")
    byid = {x['id']: x for x in r.get('results', [])}
    for x in r.get('results', [])[:12]:
        mark = 'G' if x['id'] in s['good'] else 'B' if x['id'] in s['bad'] else ' '
        pk = f"#{picks.index(x['id']) + 1}" if x['id'] in picks else '  '
        print(f"   {mark}{pk} {x['id']} “{x['title'][:70]}” — {x['channel'][:28]} {x['duration'] or 0:.0f}s {views(x.get('view_count'))}")
    for p in r.get('pick', {}).get('picks', []):
        if p['id'] not in byid:
            print(f"   ?? picked an id not in the results: {p['id']}")
    print(f"   → {r.get('pick', {}).get('reason', '')[:200]}")
print(f"\nspent in all so far: ${claude.spent():.2f}")
