# Stage-F benchmark: the judge's final decision on review groups whose hand
# verdict is known, from the 2026-10-08 snapshot and the labels read back from
# the import log. Usage:
#   python3 final.py --model claude-haiku-5-5 [--prompt v1] [--effort low] [--frames 5] [--limit N] [--groups 77,123] [--workers 6] [--show]
import json, base64, os, sys, argparse, sqlite3, collections
from concurrent.futures import ThreadPoolExecutor
sys.path.insert(0, os.path.dirname(__file__))
import claude, prompts

SNAP = os.path.expanduser('~/.cache/mvplayer-judge/queue-2026-10-08')
DB = os.path.expanduser('~/Videos/MVs/.mvplayer/library.db')
FRAME_SETS = {3: [1, 3, 5], 4: [0, 2, 4, 6], 5: [0, 2, 3, 4, 6], 7: [0, 1, 2, 3, 4, 5, 6]}

ap = argparse.ArgumentParser()
ap.add_argument('--model', required=True)
ap.add_argument('--prompt', default='v1')
ap.add_argument('--effort', default=None)
ap.add_argument('--frames', type=int, default=5)
ap.add_argument('--limit', type=int, default=0)
ap.add_argument('--groups', default='')
ap.add_argument('--workers', type=int, default=6)
ap.add_argument('--show', action='store_true', help='print every group, not only disagreements')
args = ap.parse_args()

cases = json.load(open(f'{SNAP}/cases.json'))
labels = {l['group']: l for l in json.load(open(f'{SNAP}/labels.json'))}
db = sqlite3.connect(f'file:{DB}?mode=ro', uri=True)
db.row_factory = sqlite3.Row
trackinfo = {r['id']: dict(r) for r in db.execute('select id, year, codec, bitrate, lossless, sample_rate, bits, genre from tracks')}

want = {int(g) for g in args.groups.split(',') if g}
labelled = [c for c in cases if labels[c['group']]['verdict'] != 'pending' and c['tracks'] and (not want or c['group'] in want)]
if args.limit:
    labelled = labelled[:args.limit]


def fmt_track(t):
    i = trackinfo.get(t['id'], {})
    audio = ('lossless' if i.get('lossless') else f"{i.get('codec') or '?'} {i.get('bitrate') or '?'} kbit/s")
    year = f", {i['year']}" if i.get('year') else ''
    artist = t['artist'] if t['artist'] == t['album_artist'] else f"{t['artist']} (album artist {t['album_artist']})"
    return f"“{t['title']}” by {artist}, album “{t['album']}”{year}, {t['duration']:.0f} s, {audio}"


def fmt_audio(o, track_sec):
    m = o['measured']
    if not m:
        return o['reason']
    parts = [f"fingerprints match {min(1, m['fingerprintCoverage']) * 100:.0f}% of the video", f"same waveform {min(1, m['sameWaveform']) * 100:.0f}% of the track"]
    if m.get('interruptedSec', 0) > 0:
        parts.append(f"{m['interruptedSec']:.0f} s of other audio inside the song")
    if m.get('placedByOffset'):
        parts.append('placed by offset (same performance in another mix)')
    return '; '.join(parts)


def fmt_picture(o):
    # The picture check (L4): how much of the picture ever changes between the sampled frames,
    # and whether that change sits only along an edge (a spectrum bar, a ticker, a progress bar).
    from PIL import Image, ImageChops
    frames = [Image.open(f"{SNAP}/frames/{f}").convert('L').resize((192, 108)) for f in o['frames']]
    acc = Image.new('L', frames[0].size, 0)
    for i in range(len(frames)):
        for j in range(i + 1, len(frames)):
            acc = ImageChops.lighter(acc, ImageChops.difference(frames[i], frames[j]))
    px = list(acc.get_flattened_data()) if hasattr(acc, 'get_flattened_data') else list(acc.getdata())
    w, h = acc.size
    moved = [k for k, v in enumerate(px) if v > 24]
    share = len(moved) / len(px)
    inner = sum(1 for k in moved if 0.1 * h <= k // w < 0.85 * h) / len(px)
    s = f"between the {len(frames)} frames sampled across the video, {share * 100:.1f}% of the picture ever changes"
    if share == 0:
        return s + " (the frames are identical)"
    if inner < 0.005:
        return s + ", only along the top or bottom edge"
    return s


def img(name):
    data = base64.b64encode(open(f'{SNAP}/frames/{name}', 'rb').read()).decode()
    return {'type': 'image', 'source': {'type': 'base64', 'media_type': 'image/jpeg', 'data': data}}


def build(c):
    t = c['tracks'][0]
    others = c['tracks'][1:]
    head = f"Track: {fmt_track(t)}."
    if others:
        head += "\nOther tracks in the library that the audio check also matched to these uploads (the track above is the one to judge for): " + '; '.join(fmt_track(x) for x in others)
    content = [{'type': 'text', 'text': head + f"\n\n{len(c['options'])} upload(s) that contain the song:"}]
    idx = FRAME_SETS[args.frames]
    for i, o in enumerate(c['options'], 1):
        picture = f"\nPicture check: {fmt_picture(o)}." if args.prompt >= 'v6' else ''
        content.append({'type': 'text', 'text': f"\nUpload {i}: “{o['ytTitle']}” on channel “{o['channel']}”, {o['duration']:.0f} s, {o['width']}x{o['height']}.\nAudio check: {fmt_audio(o, t['duration'])}.{picture}\n{len(idx)} frames at " + ', '.join(f"{o['framePositions'][j] * 100:.0f}%" for j in idx) + ' of the video:'})
        for j in idx:
            content.append(img(o['frames'][j]))
    content.append({'type': 'text', 'text': "\nJudge each upload, then give the verdict for the track: accept (with the index of the upload to keep), reject all, or unsure."})
    return content


def run(c):
    answer, usage, dt, cached = claude.ask(args.model, prompts.FINAL[args.prompt], build(c), prompts.SCHEMA_FINAL,
                                           effort=args.effort, tag=f'final-{args.prompt}-g{c["group"]}')
    return {'group': c['group'], 'answer': answer, 'usage': usage, 'seconds': round(dt, 1), 'cached': cached}


with ThreadPoolExecutor(args.workers) as pool:
    results = list(pool.map(run, labelled))

# ---- scoring -----------------------------------------------------------
tag = f"{args.model}-{args.prompt}" + (f"-{args.effort}" if args.effort else '') + f"-f{args.frames}"
os.makedirs(f'{claude.CACHE}/results', exist_ok=True)
json.dump(results, open(f'{claude.CACHE}/results/final-{tag}.json', 'w'), ensure_ascii=False, indent=1)

cost = sum(claude.cost_of(args.model, r['usage']) for r in results)
secs = [r['seconds'] for r in results if not r['cached']]
errors = [r for r in results if 'error' in r['answer']]
rows = []  # (label verdict, label chosen, judge verdict, judge chosen, conf, group, gated)
def gated(c, a):
    # the pipeline's own gates: an accepted upload that is a cut (<90% of the track) or interrupted (>15 s) waits for a verdict
    if a['verdict'] != 'accept' or not (1 <= a.get('chosen', 0) <= len(c['options'])):
        return False
    o = c['options'][a['chosen'] - 1]; m = o['measured']; t = c['tracks'][0]
    return o['duration'] < 0.9 * t['duration'] or m.get('interruptedSec', 0) > 15
opt_rows = []  # (label yes/no/passed, judge official_mv, conf)
for c, r in zip(labelled, results):
    a = r['answer']
    if 'error' in a:
        continue
    l = labels[c['group']]
    lv, lc = l['verdict'], l.get('chosen', 0)
    jv, jc = a['verdict'], a.get('chosen', 0)
    rows.append((lv, lc, jv, jc, a['confidence'], c['group'], gated(c, a)))
    byidx = {o['index']: o for o in a['options']}
    for i in range(1, len(c['options']) + 1):
        o = byidx.get(i)
        if not o:
            continue
        if lv == 'accept':
            lab = 'yes' if i == lc else 'passed'
        elif lv == 'reject all':
            lab = 'no'
        else:
            lab = 'no' if i in l.get('rejected', []) else 'pending'
        opt_rows.append((lab, o['official_mv'], o['confidence'], c['group'], i))


def agrees(lv, lc, jv, jc):
    if lv == 'partial':
        return None
    if lv == 'accept':
        return jv == 'accept' and jc == lc
    return jv == 'reject all'


print(f"\n== {tag}: {len(results)} groups, {len(errors)} errors, ${cost:.3f}" + (f", {sum(secs) / len(secs):.1f} s/group" if secs else ' (all cached)'))
for e in errors[:5]:
    print('  error', e['group'], e['answer']['error'][:200])

decided = [r for r in rows if r[0] != 'partial']
n_ok = sum(1 for r in decided if agrees(*r[:4]))
n_unsure = sum(1 for r in decided if r[2] == 'unsure')
print(f"Group verdicts (mode 1), {len(decided)} labelled accept/reject-all: agree {n_ok}, disagree {len(decided) - n_ok - n_unsure}, judge unsure {n_unsure}")
conf = collections.Counter()
for lv, lc, jv, jc, cf, g, gt in decided:
    conf[(lv, 'agree' if agrees(lv, lc, jv, jc) else jv if jv == 'unsure' else 'wrong ' + jv)] += 1
for k, v in sorted(conf.items()):
    print(f"   label {k[0]:10} → {k[1]:18} {v}")
print(f"At a confidence gate: auto-decided share, and accuracy of those ({sum(1 for r in decided if r[6])} accepts of a cut or interrupted upload go to review whatever the confidence)")
for T in (0.5, 0.7, 0.8, 0.85, 0.9, 0.95):
    auto = [r for r in decided if r[2] != 'unsure' and r[4] >= T and not r[6]]
    ok = sum(1 for r in auto if agrees(*r[:4]))
    print(f"   ≥{T:.2f}: {len(auto):3}/{len(decided)} auto ({len(auto) / max(1, len(decided)) * 100:.0f}%), right {ok}/{len(auto)} ({ok / max(1, len(auto)) * 100:.0f}%), wrong {len(auto) - ok}")

print(f"Per upload (mode 2 'is this the official MV?'):")
for lab in ('yes', 'no', 'passed'):
    xs = [r for r in opt_rows if r[0] == lab]
    if not xs:
        continue
    said_yes = sum(1 for r in xs if r[1])
    mean_c = sum(r[2] if r[1] else 1 - r[2] for r in xs) / len(xs)
    print(f"   hand {lab:6}: {len(xs):3} uploads, judge says MV for {said_yes} ({said_yes / len(xs) * 100:.0f}%), mean P(MV) {mean_c:.2f}")
for T in (0.7, 0.8, 0.9):
    xs = [r for r in opt_rows if r[0] in ('yes', 'no') and (r[2] >= T)]
    ok = sum(1 for r in xs if (r[0] == 'yes') == r[1])
    print(f"   confidence ≥{T:.1f}: {len(xs)} of {sum(1 for r in opt_rows if r[0] in ('yes', 'no'))} labelled uploads, right {ok} ({ok / max(1, len(xs)) * 100:.0f}%)")

print("\nDisagreements" + (" and everything else" if args.show else "") + ":")
for c, r in zip(labelled, results):
    a = r['answer']
    if 'error' in a:
        continue
    l = labels[c['group']]
    ok = agrees(l['verdict'], l.get('chosen', 0), a['verdict'], a.get('chosen', 0))
    if ok and not args.show:
        continue
    t = c['tracks'][0]
    mark = '✓' if ok else '?' if ok is None else '✗'
    print(f"{mark} g{c['group']} {t['album_artist']} – {t['title']} ({t['duration']:.0f}s) | hand: {l['verdict']} {l.get('chosen', l.get('rejected', ''))} | judge: {a['verdict']} {a.get('chosen', '')} conf {a['confidence']:.2f}: {a['reason'][:260]}")
    for i, o in enumerate(c['options'], 1):
        j = next((x for x in a['options'] if x['index'] == i), {})
        print(f"     {i}. “{o['ytTitle'][:70]}” on “{o['channel'][:30]}” {o['duration']:.0f}s | {fmt_audio(o, t['duration'])[:80]}")
        extra = ', '.join(str(j[k]) for k in ('source', 'version', 'whole') if j.get(k))
        print(f"        → {j.get('kind')}{' [' + extra + ']' if extra else ''}, MV={j.get('official_mv')} {j.get('confidence', 0):.2f}: {j.get('note', '')[:200]}")
print(f"\nspent in all so far: ${claude.spent():.2f}")
