#!/usr/bin/env bash
# End-to-end check of the audio pipeline on synthetic files, no network needed:
# a "track" is embedded in a longer "video soundtrack" with an intro, an
# inserted scene and an outro; the aligner must find both segments at the
# right offsets and the muxer must produce a playable file with both audio
# tracks.
set -euo pipefail

BUILD=${1:-build}
# On Windows "mvplayer" alone would name the QML module's build folder.
EXE=; if command -v cygpath >/dev/null; then EXE=.exe; fi
WORK=$(mktemp -d)
trap 'rm -rf "$WORK"' EXIT

# A 50 s "song": a slow sequence of chords, so fingerprints have structure.
ffmpeg -v error -y -f lavfi -i "aevalsrc=0.3*sin(2*PI*t*(220+30*mod(floor(t/1.5)\,7)))+0.2*sin(2*PI*t*(330+45*mod(floor(t/2)\,5)))+0.1*sin(2*PI*t*880*(1+0.5*mod(floor(t*4)\,2))):s=48000:d=50" \
    -ar 48000 -ac 2 -c:a flac "$WORK/track.flac"
# The video's soundtrack: 3 s of other sound, track[0:25], 4 s of other sound,
# track[25:], 3 s of other sound; everything 3 dB quieter and lossy.
ffmpeg -v error -y -i "$WORK/track.flac" -f lavfi -i "aevalsrc=0.25*sin(2*PI*t*140)*sin(2*PI*t*3):s=48000:d=10" -filter_complex "
[1:a]atrim=0:3,asetpts=PTS-STARTPTS[i];
[0:a]atrim=0:25,asetpts=PTS-STARTPTS[t1];
[1:a]atrim=3:7,asetpts=PTS-STARTPTS[m];
[0:a]atrim=25,asetpts=PTS-STARTPTS[t2];
[1:a]atrim=7:10,asetpts=PTS-STARTPTS[o];
[i][t1][m][t2][o]concat=n=5:v=0:a=1,aformat=channel_layouts=stereo,volume=-3dB[a]" -map "[a]" -c:a libopus -b:a 96k "$WORK/mv.opus"
ffmpeg -v error -y -f lavfi -i "testsrc2=s=160x90:r=10:d=60" -i "$WORK/mv.opus" -c:v libx264 -preset ultrafast -c:a copy "$WORK/mv.mkv"

echo "== align"
SUMMARY=$("$BUILD/mvplayer-import" align "$WORK/track.flac" "$WORK/mv.mkv")
echo "$SUMMARY"
python3 - "$SUMMARY" <<'PY'
import re, sys
s = sys.argv[1]
segs = [(float(a), float(b), float(c)) for a, b, c in re.findall(r"\[video ([\d.]+)–([\d.]+)s ← track ([\d.]+)s", s)]
assert len(segs) == 2, f"expected 2 segments, got {segs}"
(a0, a1, t0), (b0, b1, t1) = segs
assert abs(a0 - 3.0) < 0.15 and abs(t0) < 0.15, f"segment 1 off: {segs[0]}"
assert abs(a1 - 28.0) < 0.15, f"segment 1 end off: {segs[0]}"
assert abs(b0 - 32.0) < 0.15 and abs(t1 - 25.0) < 0.15, f"segment 2 off: {segs[1]}"
assert abs(b1 - 57.0) < 0.15, f"segment 2 end off: {segs[1]}"
gain = float(re.search(r"gain ([-\d.]+) dB", s).group(1))
assert abs(gain - 3.0) < 0.2, f"gain {gain}"
print("alignment ok")
PY

echo "== align: another master"
# The upload of a song is often not the album master: quieter, with the
# treble rolled off, of inverted polarity, and a few milliseconds longer over
# its length. It is still the same recording, and has to be found as one piece.
ffmpeg -v error -y -i "$WORK/track.flac" \
    -af "asetrate=48000*0.99998,aresample=48000,lowpass=f=5000,volume=-4dB,aeval=-val(0)|-val(1)" \
    -c:a libopus -b:a 96k "$WORK/master.opus"
SUMMARY=$("$BUILD/mvplayer-import" align "$WORK/track.flac" "$WORK/master.opus")
echo "$SUMMARY"
python3 - "$SUMMARY" <<'PY'
import re, sys
s = sys.argv[1]
segs = re.findall(r"\[video ([\d.]+)–([\d.]+)s", s)
assert len(segs) == 1 and float(segs[0][1]) - float(segs[0][0]) > 49, segs
assert "polarity inverted" in s, s
assert int(re.search(r"(\d+)% of it plainly the same", s).group(1)) >= 90, s
assert abs(float(re.search(r"gain ([-\d.]+) dB", s).group(1)) - 4.0) < 1.0, s
print("another master ok")
PY

echo "== mux"
"$BUILD/mvplayer-import" mux "$WORK/track.flac" "$WORK/mv.mkv" "$WORK/out.mkv"
STREAMS=$(ffprobe -v error -show_entries stream=codec_type,codec_name -of csv=p=0 "$WORK/out.mkv" | tr '\n' ' ')
echo "streams: $STREAMS"
[[ "$STREAMS" == *"h264,video"* && "$STREAMS" == *"flac,audio"* && "$STREAMS" == *"opus,audio"* ]]

echo "== still-image detection"
# A still picture as low-bitrate streams deliver it: the image shimmers by a
# level or two every few seconds. It must count as a still; real motion must not.
ffmpeg -v error -y -f lavfi -i "smptebars=s=256x144:r=10:d=40" \
    -vf "eq=brightness='if(lt(mod(t\,7)\,2)\,0.006\,0)':eval=frame" -c:v libx264 -preset ultrafast "$WORK/still.mkv"
for mode in "" keyframes; do
    S=$("$BUILD/mvplayer-import" check-video "$WORK/still.mkv" $mode); echo "still.mkv  ${mode:-full}: $S"; [[ "$S" == still:* ]]
    M=$("$BUILD/mvplayer-import" check-video "$WORK/mv.mkv" $mode);    echo "mv.mkv     ${mode:-full}: $M"; [[ "$M" == moving:* ]]
done

echo "== circuit breaker"
# A yt-dlp that is bot-checked for its first three calls, then answers with
# no results. The importer must pause (waits doubling), put the tracks back
# untouched, and finish them once requests succeed again.
mkdir -p "$WORK/lib/A" "$WORK/mvlib"
# Two different songs: files are told apart by their sound.
song() { echo "aevalsrc=0.3*sin(2*PI*t*($1+$2*mod(floor(t/1.5)\,7)))+0.2*sin(2*PI*t*($1*1.5+$2*mod(floor(t/2)\,5))):s=48000:d=20"; }
ffmpeg -v error -y -f lavfi -i "$(song 220 30)" -metadata title="Song one" -metadata artist=A "$WORK/lib/A/one.flac"
ffmpeg -v error -y -f lavfi -i "$(song 311 47)" -metadata title="Song two" -metadata artist=A "$WORK/lib/A/two.flac"
cat > "$WORK/fake-ytdlp" <<FAKE
#!/bin/sh
n=\$(cat "$WORK/calls" 2>/dev/null || echo 0); n=\$((n+1)); echo \$n > "$WORK/calls"
if [ \$n -le 3 ]; then echo "ERROR: [youtube] x: Sign in to confirm you’re not a bot" >&2; exit 1; fi
echo '{"entries": []}'
FAKE
chmod +x "$WORK/fake-ytdlp"
FAKE="$WORK/fake-ytdlp"
if [[ -n "$EXE" ]]; then
    # Windows cannot start a "#!" script; a batch file hands it to sh.
    printf '@"%s" "%%~dp0fake-ytdlp" %%*\r\n' "$(cygpath -w "$(command -v sh)")" > "$WORK/fake-ytdlp.cmd"
    FAKE="$WORK/fake-ytdlp.cmd"
fi
OUT=$(MVPLAYER_YTDLP="$FAKE" MVPLAYER_PAUSE_SECS=1 timeout 60 "$BUILD/mvplayer-import" \
    --music-dir "$WORK/lib" --mv-dir "$WORK/mvlib" 2>&1)
echo "$OUT" | grep -E "paused|resuming|^done:"
[[ $(echo "$OUT" | grep -c "paused for 0:01") -eq 1 && $(echo "$OUT" | grep -c "paused for 0:02") -eq 1 ]]
[[ "$OUT" == *"done: 0 videos; tracks: not_found=2"* ]]
grep -q '"outcome":"postponed"' "$WORK/mvlib/.mvplayer/import-log.jsonl"
grep -q '"event":"paused"' "$WORK/mvlib/.mvplayer/import-log.jsonl"

echo "== moved library"
# A library whose MV folder and music folder are both somewhere else now: the
# video is still found (its path is kept relative to the MV folder) and its
# track keeps it, with no new lookup.
mkdir -p "$WORK/mvlib/A"
# (Moved into place: MSYS2 does not translate a path with brackets for ffmpeg.)
cp "$WORK/out.mkv" "$WORK/mvlib/A/Song one [abc].mkv"
python3 - "$WORK/mvlib/.mvplayer/library.db" <<'PY'
import sqlite3, sys
db = sqlite3.connect(sys.argv[1])
db.execute("INSERT INTO videos (yt_id, path, thumb, title, audio_source, added_at) "
           "VALUES ('abc', 'A/Song one [abc].mkv', 'A/Song one [abc].jpg', 'Song one', 'library', 1)")
db.execute("UPDATE tracks SET state = 'done', video_id = (SELECT id FROM videos) WHERE title = 'Song one'")
db.commit()
PY
mv "$WORK/mvlib" "$WORK/mvlib2"
mv "$WORK/lib" "$WORK/lib2"
# From here on yt-dlp answers at once; the count shows whether it was asked.
echo 100 > "$WORK/calls"
OUT=$(MVPLAYER_YTDLP="$FAKE" MVPLAYER_PAUSE_SECS=1 timeout 60 "$BUILD/mvplayer-import" \
    --music-dir "$WORK/lib2" --mv-dir "$WORK/mvlib2" 2>&1)
echo "$OUT" | grep -E "^\[scan\]|^done:"
[[ "$OUT" == *"2 tracks: 0 new, 2 changed, 0 removed"* && "$OUT" == *"done: 1 videos;"* ]]
[[ $(cat "$WORK/calls") -eq 100 ]]
python3 - "$WORK/mvlib2/.mvplayer/library.db" <<'PY'
import sqlite3, sys
db = sqlite3.connect(sys.argv[1])
assert db.execute("SELECT path, thumb FROM videos").fetchall() == [("A/Song one [abc].mkv", "A/Song one [abc].jpg")]
states = dict(db.execute("SELECT title, state FROM tracks"))
assert states == {"Song one": "done", "Song two": "not_found"}, states
assert db.execute("SELECT COUNT(*) FROM tracks WHERE video_id IS NOT NULL AND path LIKE '%/lib2/A/one.flac'").fetchone()[0] == 1
print("moved library ok")
PY

# A database of another layout is refused, not converted.
mkdir -p "$WORK/mvold/.mvplayer"
python3 -c "import sqlite3, sys; db = sqlite3.connect(sys.argv[1]); db.execute('CREATE TABLE videos (id INTEGER PRIMARY KEY)'); db.execute('PRAGMA user_version = 3'); db.commit()" "$WORK/mvold/.mvplayer/library.db"
! OUT=$("$BUILD/mvplayer-import" --music-dir "$WORK/lib2" --mv-dir "$WORK/mvold" 2>&1)
[[ "$OUT" == *"written by another version"* ]]

echo "== recordings"
# The same library on a device that keeps it as Opus, under other file names,
# with one title romanised, plus a second copy of a song under another title.
# Every file is recognised by its sound. The only new search is for the title
# that has not been tried on a recording without a video, and only once.
[[ $("$BUILD/mvplayer-import" same-recording "$WORK/lib2/A/one.flac" "$WORK/lib2/A/two.flac") == different:* ]]
mkdir -p "$WORK/lib3/B"
ffmpeg -v error -y -i "$WORK/lib2/A/one.flac" -c:a libopus -b:a 64k -metadata title="Uta ichi" "$WORK/lib3/B/01.opus"
ffmpeg -v error -y -i "$WORK/lib2/A/two.flac" -c:a libopus -b:a 64k "$WORK/lib3/B/02.opus"
ffmpeg -v error -y -i "$WORK/lib2/A/two.flac" -c:a flac -metadata title="Song two (album cut)" "$WORK/lib3/B/03.flac"
[[ $("$BUILD/mvplayer-import" same-recording "$WORK/lib2/A/one.flac" "$WORK/lib3/B/01.opus") == same:* ]]
# "Song two" as a library from before fingerprints were kept: it is matched by its tags.
python3 - "$WORK/mvlib2/.mvplayer/library.db" <<'PY'
import sqlite3, sys
db = sqlite3.connect(sys.argv[1])
assert db.execute("SELECT COUNT(*) FROM tracks WHERE fingerprint IS NOT NULL AND recording IS NOT NULL").fetchone()[0] == 2
db.execute("UPDATE tracks SET fingerprint = NULL, fp_size = NULL, recording = NULL WHERE title = 'Song two'")
db.commit()
PY
touch -d "10 seconds ago" "$WORK"/lib3/B/*
OUT=$(MVPLAYER_YTDLP="$FAKE" MVPLAYER_PAUSE_SECS=1 timeout 60 "$BUILD/mvplayer-import" \
    --music-dir "$WORK/lib3" --mv-dir "$WORK/mvlib2" 2>&1)
echo "$OUT" | grep -E "^\[scan\]|^done:"
[[ "$OUT" == *"3 tracks: 1 new, 2 changed, 0 removed"* && "$OUT" == *"done: 1 videos;"* ]]
# One lookup: "Song two" has no video, and the album cut's title was never tried.
[[ $(cat "$WORK/calls") -eq 104 ]]
[[ "$OUT" == *"Song two (album cut): No music video found"* ]]
python3 - "$WORK/mvlib2/.mvplayer/library.db" <<'PY'
import sqlite3, sys
db = sqlite3.connect(sys.argv[1])
rows = {t: (s, v, r) for t, s, v, r in db.execute("SELECT title, state, video_id, recording FROM tracks")}
assert set(rows) == {"Uta ichi", "Song two", "Song two (album cut)"}, rows
assert rows["Uta ichi"][0] == "done" and rows["Uta ichi"][1] is not None, rows
assert rows["Song two"][0] == rows["Song two (album cut)"][0] == "not_found", rows
assert rows["Song two"][2] == rows["Song two (album cut)"][2] != rows["Uta ichi"][2], rows
# The video follows its track's new title.
assert db.execute("SELECT title FROM videos").fetchall() == [("Uta ichi",)]
print("recordings ok")
PY
OUT=$(MVPLAYER_YTDLP="$FAKE" timeout 60 "$BUILD/mvplayer-import" --music-dir "$WORK/lib3" --mv-dir "$WORK/mvlib2" 2>&1)
[[ $(cat "$WORK/calls") -eq 104 && "$OUT" == *"tracks:"*"not_found=2"* ]]

echo "== review"
# A stand-in for YouTube with one upload for the track "Five": the same notes
# 0.4% faster, so the song by its fingerprints but not demonstrably the
# track's recording. It is neither given the track's audio outright nor
# passed over: it is brought in to wait for the user, with a default audio
# stream that alternates between YouTube's and the track's. Accepted, it
# joins the library with the track's audio; turned down, it is deleted, its
# track has no video and is not offered that upload again.
mkdir -p "$WORK/lib5/Dee"
ffmpeg -v error -y -f lavfi -i "$(song 277 41 | sed 's/d=20/d=40/')" -metadata title="Five" -metadata artist=Dee "$WORK/lib5/Dee/five.flac"
touch -d "10 seconds ago" "$WORK/lib5/Dee/five.flac"
ffmpeg -v error -y -i "$WORK/lib5/Dee/five.flac" -af atempo=1.004 -c:a libopus -b:a 96k "$WORK/site-audio.opus"
ffmpeg -v error -y -i "$WORK/mv.mkv" -map 0:v:0 -c copy -t 40 "$WORK/site-video.mkv"
echo '{"entries": [{"id": "liv", "ie_key": "Youtube", "title": "Dee - Five", "channel": "Dee", "duration": 40, "view_count": 1000, "channel_is_verified": true}]}' > "$WORK/site-search.json"
cat > "$WORK/fake-site" <<FAKE
#!/bin/sh
echo "\$*" >> "$WORK/site-args"
case " \$* " in
*ytsearch*) cat "$WORK/site-search.json" ;;
*)  out=; prev=
    for a in "\$@"; do [ "\$prev" = "-o" ] && out=\$a; prev=\$a; done
    dir=\$(dirname "\$out")
    case "\$out" in
    *audio.*) cp "$WORK/site-audio.opus" "\$dir/audio.opus"; echo '{"abr": 96, "acodec": "opus"}' > "\$dir/audio.info.json" ;;
    *preview.*) cp "$WORK/site-video.mkv" "\$dir/preview.mkv" ;;
    *video.*) cp "$WORK/site-video.mkv" "\$dir/video.mkv" ;;
    esac ;;
esac
FAKE
chmod +x "$WORK/fake-site"
SITE="$WORK/fake-site"
if [[ -n "$EXE" ]]; then
    printf '@"%s" "%%~dp0fake-site" %%*\r\n' "$(cygpath -w "$(command -v sh)")" > "$WORK/fake-site.cmd"
    SITE="$WORK/fake-site.cmd"
fi
streams() { (cd "$WORK/mvlib5/Dee" && ffprobe -v error -show_entries stream=codec_name -of csv=p=0 "Five [liv].mkv" | tr -d '\r' | tr '\n' ' '); }
review_state() { python3 -c "import sqlite3, sys; print(sqlite3.connect(sys.argv[1]).execute('SELECT review, audio_source FROM videos').fetchall())" "$WORK/mvlib5/.mvplayer/library.db"; }
OUT=$(MVPLAYER_YTDLP="$SITE" timeout 120 "$BUILD/mvplayer-import" --music-dir "$WORK/lib5" --mv-dir "$WORK/mvlib5" 2>&1)
echo "$OUT" | grep -E "^\[import\]|^done:"
[[ "$OUT" == *"for your review (1 option)"* && "$OUT" == *"done: 0 videos and 1 for review; tracks: done=1"* ]]
[[ "$(streams)" == "h264 flac opus flac " && "$(review_state)" == "[(1, 'library')]" ]]
# search, audio to listen to, the picture to look at, the video
[[ $(wc -l < "$WORK/site-args") -eq 4 ]]
OUT=$(MVPLAYER_YTDLP="$SITE" timeout 120 "$BUILD/mvplayer-import" --music-dir "$WORK/lib5" --mv-dir "$WORK/mvlib5" --approve liv 2>&1)
echo "$OUT" | grep -E "^\[review\]|^done:"
[[ "$OUT" == *"done: 1 videos; tracks: done=1"* ]]
[[ "$(streams)" == "h264 flac opus " && "$(review_state)" == "[(0, 'library')]" ]]
# Put back under review, and turned down this time.
python3 -c "import sqlite3, sys; db = sqlite3.connect(sys.argv[1]); db.execute('UPDATE videos SET review = 1, review_group = id'); db.commit()" "$WORK/mvlib5/.mvplayer/library.db"
OUT=$(MVPLAYER_YTDLP="$SITE" timeout 120 "$BUILD/mvplayer-import" --music-dir "$WORK/lib5" --mv-dir "$WORK/mvlib5" --reject liv 2>&1)
echo "$OUT" | grep -E "^\[review\]|^done:"
[[ "$OUT" == *"done: 0 videos; tracks: not_found=1"* ]]
[[ ! -e "$WORK/mvlib5/Dee/Five [liv].mkv" ]]
# Looking again finds the same upload and leaves it alone: one search, nothing fetched.
OUT=$(MVPLAYER_YTDLP="$SITE" timeout 120 "$BUILD/mvplayer-import" --music-dir "$WORK/lib5" --mv-dir "$WORK/mvlib5" --retry 2>&1)
[[ "$OUT" == *"done: 0 videos; tracks: not_found=1"* ]]
[[ $(wc -l < "$WORK/site-args") -eq 5 ]]
python3 - "$WORK/mvlib5/.mvplayer/library.db" <<'PY'
import sqlite3, sys
db = sqlite3.connect(sys.argv[1])
assert db.execute("SELECT COUNT(*) FROM videos").fetchone()[0] == 0
assert [y for _, y in db.execute("SELECT key, yt_id FROM rejected_videos")] == ["liv"]
assert "turned down" in db.execute("SELECT message FROM tracks").fetchone()[0]
print("review ok")
PY

echo "== premium account"
# A yt-dlp that knows an account by its cookies: it lists audio at 250 kbit/s
# and hands it out. The check of existing videos must rebuild the one video
# from it, and must give yt-dlp a copy of the cookies, not the stored file.
printf '# Netscape HTTP Cookie File\n.youtube.com\tTRUE\t/\tTRUE\t0\tSID\tsecret\n' > "$WORK/cookies.txt"
cat > "$WORK/fake-premium" <<FAKE
#!/bin/sh
echo "\$*" >> "$WORK/premium-args"
case " \$* " in *" --cookies "*) ;; *) echo "ERROR: no cookies given" >&2; exit 1 ;; esac
case " \$* " in
*" -J "*) echo '{"formats": [{"format_id": "774", "vcodec": "none", "acodec": "opus", "abr": 250}]}' ;;
*)  out=; prev=
    for a in "\$@"; do [ "\$prev" = "-o" ] && out=\$a; prev=\$a; done
    dir=\$(dirname "\$out")
    cp "$WORK/mv.opus" "\$dir/premium.opus"
    echo '{"abr": 250, "acodec": "opus"}' > "\$dir/premium.info.json" ;;
esac
FAKE
chmod +x "$WORK/fake-premium"
PREMIUM="$WORK/fake-premium"
if [[ -n "$EXE" ]]; then
    printf '@"%s" "%%~dp0fake-premium" %%*\r\n' "$(cygpath -w "$(command -v sh)")" > "$WORK/fake-premium.cmd"
    PREMIUM="$WORK/fake-premium.cmd"
fi
! "$BUILD/mvplayer-import" --music-dir "$WORK/lib3" --mv-dir "$WORK/mvlib2" --cookies "$WORK/track.flac" >/dev/null
# A track of lesser quality than YouTube's audio, so its video plays YouTube's.
mkdir -p "$WORK/lib4/C" "$WORK/mvlib4/C"
ffmpeg -v error -y -i "$WORK/lib2/A/one.flac" -c:a libopus -b:a 32k -metadata title="Low" "$WORK/lib4/C/low.opus"
touch -d "10 seconds ago" "$WORK/lib4/C/low.opus"
ffmpeg -v error -y -i "$WORK/mv.mkv" -i "$WORK/lib2/A/one.flac" -map 0:v:0 -map 1:a:0 -c:v copy -c:a libopus -b:a 96k \
    -shortest "$WORK/low.mkv"
mv "$WORK/low.mkv" "$WORK/mvlib4/C/Low [xyz].mkv"
echo 200 > "$WORK/calls"
MVPLAYER_YTDLP="$FAKE" timeout 60 "$BUILD/mvplayer-import" --music-dir "$WORK/lib4" --mv-dir "$WORK/mvlib4" >/dev/null 2>&1
python3 - "$WORK/mvlib4/.mvplayer/library.db" <<'PY'
import sqlite3, sys
db = sqlite3.connect(sys.argv[1])
db.execute("INSERT INTO videos (yt_id, path, title, audio_source, added_at) "
           "VALUES ('xyz', 'C/Low [xyz].mkv', 'Low', 'youtube', 1)")
db.execute("UPDATE tracks SET state = 'done', video_id = (SELECT id FROM videos)")
db.commit()
PY
OUT=$(MVPLAYER_YTDLP="$PREMIUM" timeout 120 "$BUILD/mvplayer-import" --music-dir "$WORK/lib4" --mv-dir "$WORK/mvlib4" \
    --cookies "$WORK/cookies.txt" --check-quality 2>&1)
echo "$OUT" | grep -E "^\[quality\]|^\[audio\]|warning"
[[ "$OUT" == *"1 upgraded, 0 failed"* ]]
[[ $(wc -l < "$WORK/premium-args") -eq 2 ]]
! grep -q -e "--cookies $WORK/cookies.txt" "$WORK/premium-args"
grep -q "secret" "$WORK/cookies.txt"
# (By its relative name: MSYS2 does not translate a path with brackets for ffprobe.)
STREAMS=$(cd "$WORK/mvlib4/C" && ffprobe -v error -show_entries stream=codec_type,codec_name -of csv=p=0 "Low [xyz].mkv" | tr -d '\r' | tr '\n' ' ')
echo "streams: $STREAMS"
[[ "$STREAMS" == "h264,video opus,audio " ]]
python3 - "$WORK/mvlib4/.mvplayer/library.db" <<'PY'
import sqlite3, sys
db = sqlite3.connect(sys.argv[1])
assert db.execute("SELECT yt_abr, audio_source FROM videos").fetchall() == [(250.0, "youtube")]
PY
# Already built from the account's audio: nothing is asked again.
MVPLAYER_YTDLP="$PREMIUM" timeout 120 "$BUILD/mvplayer-import" --music-dir "$WORK/lib4" --mv-dir "$WORK/mvlib4" \
    --cookies "$WORK/cookies.txt" --check-quality >/dev/null 2>&1
[[ $(wc -l < "$WORK/premium-args") -eq 2 ]]
# The library's FLAC is never traded for the account's audio: "Uta ichi" has
# it in its video, and only an Opus copy of the track is left to rebuild from.
OUT=$(MVPLAYER_YTDLP="$PREMIUM" timeout 120 "$BUILD/mvplayer-import" --music-dir "$WORK/lib3" --mv-dir "$WORK/mvlib2" \
    --cookies "$WORK/cookies.txt" --check-quality 2>&1)
[[ "$OUT" == *"0 upgraded, 0 failed"* ]]
[[ $(cd "$WORK/mvlib2/A" && ffprobe -v error -show_entries stream=codec_name -of csv=p=0 "Song one [abc].mkv" | tr -d '\r' | tr '\n' ' ') == "h264 flac opus " ]]

echo "== binaries start"
"$BUILD/mvplayer-import" --help >/dev/null
QT_QPA_PLATFORM=offscreen "$BUILD/mvplayer$EXE" --help >/dev/null
echo "smoke ok"
