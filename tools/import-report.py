#!/usr/bin/env python3
"""What the importer made of every track: a table from the import log and the
library database of an MV folder.

    tools/import-report.py <MV folder> [--tsv]

One row per video that was examined for a track, with the decision (accept,
review, reject), the reason, and the measurements it rests on. Tracks for
which nothing was examined get one row saying why.
"""
import json, os, sqlite3, sys

def main():
    sys.stdout.reconfigure(encoding="utf-8")  # titles are not ASCII, whatever the console thinks
    args = [a for a in sys.argv[1:] if not a.startswith("--")]
    if len(args) != 1:
        sys.exit(__doc__)
    tsv = "--tsv" in sys.argv
    data = os.path.join(args[0], ".mvplayer")
    events = []
    for name in ("import-log.jsonl.1", "import-log.jsonl"):
        path = os.path.join(data, name)
        if os.path.exists(path):
            with open(path, encoding="utf-8") as f:
                for line in f:
                    try:
                        events.append(json.loads(line))
                    except ValueError:
                        pass
    db = sqlite3.connect(f"file:{os.path.join(data, 'library.db')}?mode=ro", uri=True)
    db.row_factory = sqlite3.Row
    tracks = db.execute("SELECT id, title, artist, album, disc_no, track_no, state, message, recording, video_id, path"
                        " FROM tracks ORDER BY album, disc_no, track_no, title").fetchall()
    videos = {v["id"]: v for v in db.execute("SELECT id, yt_id, yt_title, review, audio_source, audio_detail FROM videos")}

    lookups, notes, verdicts, examined = {}, {}, {}, {}
    for e in events:
        kind = e.get("event", "lookup" if "track" in e and "outcome" in e else None)
        if kind == "lookup" and e.get("outcome") != "postponed":
            lookups[e.get("trackId")] = e          # the latest one counts
            # Every video ever examined for the track, each with the latest
            # entry that says something about the video itself: a later look
            # may only note that it is already waiting, or was turned down.
            seen = examined.setdefault(e.get("trackId"), {})
            for c in e.get("checked", []):
                if c.get("result") in ("option", "rejected") and c.get("id") in seen:
                    continue
                seen[c.get("id")] = c
        elif kind == "track":
            notes[e.get("trackId")] = e
        elif kind == "verdict":
            verdicts[e.get("id")] = e
    by_recording = {}
    for t in tracks:
        if t["id"] in lookups and t["recording"]:
            by_recording.setdefault(t["recording"], lookups[t["id"]])

    def pct(x):
        return "" if x is None else f"{round(100 * x)}%"

    rows, summary = [], {}
    for t in tracks:
        look = lookups.get(t["id"]) or by_recording.get(t["recording"])
        note = notes.get(t["id"])
        video = videos.get(t["video_id"]) if t["video_id"] else None
        if t["state"] == "skipped":
            decision = "skipped"
        elif t["state"] == "pending":
            decision = "pending"
        elif video is not None:
            decision = "review" if video["review"] else "accept"
        elif t["state"] == "failed":
            decision = "undecided"
        else:
            decision = "reject"
        summary[decision] = summary.get(decision, 0) + 1
        base = [t["title"], t["album"] or "", decision]
        own = look is not None and look.get("trackId") == t["id"]
        checked = list(examined.get(t["id"], {}).values()) if look else []
        if not checked or not own:
            if not own and look is not None:
                why = (note or {}).get("reason") or f"the same recording as “{look.get('track')}”: shares its result"
            elif look is not None:
                dropped = [c for c in look.get("candidates", []) if c.get("rejected")]
                why = look.get("message") or ""
                if dropped:
                    why += f" ({len(dropped)} results dropped by title: " + ", ".join(sorted({c['rejected'] for c in dropped})) + ")"
            else:
                why = (note or {}).get("reason") or t["message"] or ""
            rows.append(base + ["", "", why, "", "", ""])
            continue
        for c in checked:
            m = c.get("measured") or {}
            verdict = verdicts.get(c.get("id"))
            cd = c.get("decision", "")
            if verdict:
                cd += " → you: " + verdict["verdict"]
            rows.append(base + [
                f"{c.get('title', '')} https://youtu.be/{c.get('id')}",
                cd,
                c.get("reason", ""),
                pct(m.get("fingerprintCoverage")),
                pct(m.get("sameWaveform")),
                "" if "loudnessCorrelation" not in m else f"{m['loudnessCorrelation']:.2f}",
            ])
            base = ["", "", ""]

    head = ["Track", "Album", "Track decision", "Video examined", "Video decision", "Reason", "Fingerprint coverage", "Same waveform", "Loudness correlation"]
    if tsv:
        print("\t".join(head))
        for r in rows:
            print("\t".join(str(x).replace("\t", " ") for x in r))
        return
    print(f"{len(tracks)} tracks: " + ", ".join(f"{n} {k}" for k, n in sorted(summary.items())) + "\n")
    print("| " + " | ".join(head) + " |")
    print("|" + "---|" * len(head))
    for r in rows:
        print("| " + " | ".join(str(x).replace("|", "\\|") for x in r) + " |")

if __name__ == "__main__":
    main()
