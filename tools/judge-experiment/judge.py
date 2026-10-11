import json, base64, time, os, sys, urllib.request
S = os.environ['S']
cases = json.load(open(f"{S}/judge/cases.json"))
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
def img(path):
    return {"type": "image_url", "image_url": {"url": "data:image/jpeg;base64," + base64.b64encode(open(path, 'rb').read()).decode()}}
results = []
for c in cases:
    t = c['track']
    content = [{"type": "text", "text": f"Track: “{t['title']}” by {t['artist']} (album artist {t['albumArtist']}), album “{t['album']}”, {t['duration']} s.\nOptions:"}]
    for i, o in enumerate(c['options'], 1):
        m = o['measured']
        content.append({"type": "text", "text": f"\nOption {i}: “{o['ytTitle']}” on channel “{o['channel']}”, {o['duration']} s, {o['width']}x{o['height']}. Audio analysis: {o['reason']}. Three frames at 20%, 50%, 80%:"})
        for f in o['frames']: content.append(img(f))
    content.append({"type": "text", "text": "\nJudge each option's kind, then give a verdict: accept (with the index of the option to take), reject all, or needs a human. confidence 0..1."})
    body = {"model": "gemma", "messages": [{"role": "system", "content": SYSTEM}, {"role": "user", "content": content}],
            "response_format": {"type": "json_schema", "json_schema": {"name": "verdict", "schema": schema}}, "temperature": 0.1, "max_tokens": 700, "chat_template_kwargs": {"enable_thinking": False}}
    t0 = time.time()
    req = urllib.request.Request("http://127.0.0.1:8080/v1/chat/completions", data=json.dumps(body).encode(), headers={"Content-Type": "application/json"})
    try:
        r = json.load(urllib.request.urlopen(req, timeout=900))
        out = json.loads(r['choices'][0]['message']['content'])
        usage = r.get('usage', {})
    except Exception as e:
        out = {"error": str(e)}; usage = {}
    dt = time.time() - t0
    results.append({"case": c, "answer": out, "seconds": round(dt, 1), "usage": usage})
    print(f"{dt:6.1f}s  {t['albumArtist']} - {t['title']}: {out.get('verdict')} {out.get('chosen', '')} conf {out.get('confidence', '')} | {out.get('reason', out.get('error', ''))[:160]}")
    for o in out.get('options', []):
        print(f"          option {o['index']}: {o['kind']} — {o['note'][:120]}")
    sys.stdout.flush()
json.dump(results, open(f"{S}/judge/results.json", 'w'), ensure_ascii=False, indent=1)
