# Same cases, prompt and schema as judge.py, sent to Claude through the `ant` CLI.
# Usage: S=<scratchpad> ANTHROPIC_WORKSPACE_ID=wrkspc_... python3 judge_claude.py <model> [effort]
import json, base64, time, os, sys, subprocess
from concurrent.futures import ThreadPoolExecutor

S = os.environ['S']
MODEL = sys.argv[1]
EFFORT = sys.argv[2] if len(sys.argv) > 2 else None
PRICE = {'claude-opus-5-5': (4, 20), 'claude-sonnet-5-5': (2, 10), 'claude-haiku-5-5': (0.10, 0.50)}
cases = json.load(open(f"{S}/judge/cases.json"))
# Prompt and schema copied verbatim from judge.py.
SYSTEM = """You help curate a music video library of Japanese pop, Vocaloid, doujin and anime music. A music file from the user's library has been matched by audio fingerprint to one or more YouTube uploads; the audio is settled, it IS the song. Judge the PICTURE and the UPLOAD only.

Conventions of this music scene, which you must apply:
- Official music videos here are very often animated illustrations or motion graphics, sometimes with the lyrics drawn into the picture. Animation is NOT a sign of a fan edit. An official MV can be a slideshow-like illustration film.
- A channel named after the artist (the artist's name, possibly with "official", "channel", or Japanese/romanised spellings) is the artist's own channel; anything it uploads titled MV / Music Video / オリジナルMV is the official music video. Accept it unless the picture is plainly live footage or a static cover image.
- Signs of a NON-official upload: an audio-spectrum/visualiser bar or waveform overlay, a "Now playing"/time/channel watermark, "Nightcore", a single still cover image, concert or stage footage, game characters (Gacha, MMD dance, Minecraft), a channel branded as a music compilation channel.
- A third-party re-upload of the real official MV (same picture, no overlays; titles like "Removed MV", "MV", "Official") is acceptable when no upload from the artist's own channel is among the options: pick the cleanest re-upload.
- A live performance, lyric-only video, visualiser, slideshow or fan edit is not the track's music video; reject unless the track's own title says it is a live or special version.
Answer only with the JSON asked for. Be decisive; use "needs a human" only when the options are genuinely ambiguous."""
schema = {"type": "object", "properties": {
    "options": {"type": "array", "items": {"type": "object", "properties": {
        "index": {"type": "integer"}, "kind": {"type": "string", "enum": ["official music video", "re-upload of the music video", "live performance", "lyric or visualiser video", "still image or slideshow", "fan edit or other"]},
        "note": {"type": "string"}}, "required": ["index", "kind", "note"]}},
    "verdict": {"type": "string", "enum": ["accept", "reject all", "needs a human"]},
    "chosen": {"type": "integer"}, "confidence": {"type": "number"}, "reason": {"type": "string"}},
    "required": ["options", "verdict", "chosen", "confidence", "reason"]}

def strict(s):
    # Structured outputs need every object closed and fully required.
    if s.get('type') == 'object':
        s = dict(s, additionalProperties=False, properties={k: strict(v) for k, v in s['properties'].items()})
    elif s.get('type') == 'array':
        s = dict(s, items=strict(s['items']))
    return s

def img(path):
    data = base64.b64encode(open(f"{S}/judge/frames/{os.path.basename(path)}", 'rb').read()).decode()
    return {"type": "image", "source": {"type": "base64", "media_type": "image/jpeg", "data": data}}

def judge(c):
    t = c['track']
    content = [{"type": "text", "text": f"Track: “{t['title']}” by {t['artist']} (album artist {t['albumArtist']}), album “{t['album']}”, {t['duration']} s.\nOptions:"}]
    for i, o in enumerate(c['options'], 1):
        content.append({"type": "text", "text": f"\nOption {i}: “{o['ytTitle']}” on channel “{o['channel']}”, {o['duration']} s, {o['width']}x{o['height']}. Audio analysis: {o['reason']}. Three frames at 20%, 50%, 80%:"})
        for f in o['frames']: content.append(img(f))
    content.append({"type": "text", "text": "\nJudge each option's kind, then give a verdict: accept (with the index of the option to take), reject all, or needs a human. confidence 0..1."})
    output_config = {"format": {"type": "json_schema", "schema": strict(schema)}}
    if EFFORT: output_config["effort"] = EFFORT
    body = {"model": MODEL, "max_tokens": 8000, "system": SYSTEM, "messages": [{"role": "user", "content": content}], "output_config": output_config}
    t0 = time.time()
    p = subprocess.run(['ant', 'messages', 'create', '--format', 'json', '--format-error', 'json'], input=json.dumps(body), capture_output=True, text=True)
    dt = time.time() - t0
    try:
        r = json.loads(p.stdout or p.stderr)
        usage = r.get('usage', {})
        if r.get('stop_reason') != 'end_turn':
            out = {"error": f"stop_reason {r.get('stop_reason')} {r.get('stop_details') or r.get('error', '')}"}
        else:
            out = json.loads(next(b['text'] for b in r['content'] if b['type'] == 'text'))
    except Exception as e:
        out = {"error": f"{e}: {(p.stdout or p.stderr)[:400]}"}; usage = {}
    return {"case": c, "answer": out, "seconds": round(dt, 1), "usage": usage}

with ThreadPoolExecutor(5) as pool:
    results = list(pool.map(judge, cases))
cost = 0
for res in results:
    t, out, u = res['case']['track'], res['answer'], res['usage']
    pin, pout = PRICE.get(MODEL, (0, 0))
    cost += (u.get('input_tokens', 0) * pin + u.get('output_tokens', 0) * pout) / 1e6
    print(f"{res['seconds']:6.1f}s {u.get('input_tokens', 0):5} in {u.get('output_tokens', 0):5} out  {t['albumArtist']} - {t['title']}: {out.get('verdict')} {out.get('chosen', '')} conf {out.get('confidence', '')} | {out.get('reason', out.get('error', ''))[:220]}")
    for o in out.get('options', []):
        print(f"          option {o['index']}: {o['kind']} — {o['note'][:160]}")
print(f"total ${cost:.4f}")
tag = MODEL + (f"-{EFFORT}" if EFFORT else "")
json.dump(results, open(f"{S}/judge/results-{tag}.json", 'w'), ensure_ascii=False, indent=1)
