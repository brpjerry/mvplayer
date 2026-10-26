#!/usr/bin/env bash
# End-to-end check of the audio pipeline on synthetic files, no network needed:
# a "track" is embedded in a longer "video soundtrack" with an intro, an
# inserted scene and an outro; the aligner must find both segments at the
# right offsets and the muxer must produce a playable file with both audio
# tracks.
set -euo pipefail
export MVPLAYER_NO_MUSICBRAINZ=1
# What Python prints here is compared with text in this file, which is UTF-8;
# on Windows it would print in the console's code page otherwise.
export PYTHONUTF8=1
# The songs made up here run 40 to 50 seconds; tracks under 1:20 are not
# looked up otherwise (see "short tracks" below).
export MVPLAYER_MIN_TRACK_SECS=30

BUILD=${1:-build}
# On Windows "mvplayer" alone would name the QML module's build folder.
EXE=; if command -v cygpath >/dev/null; then EXE=.exe; fi
WORK=$(mktemp -d)
trap 'rm -rf "$WORK"' EXIT
export XDG_CONFIG_HOME="$WORK/config"
export XDG_CACHE_HOME="$WORK/cache"
export XDG_DATA_HOME="$WORK/data"
# The unit tests (ChatGPT authentication and provider against a local stand-in,
# the folder tree) use temporary settings.
# QtTest's default Windows logger may use the debugger instead of stdout.
# Always capture a text report and print it even when a test fails.
TEST_LOG="$WORK/unit-tests.txt"
TEST_LOG_ARG="$TEST_LOG"
if [[ -n "$EXE" ]]; then TEST_LOG_ARG=$(cygpath -m "$TEST_LOG"); fi
TEST_RESULT=0
for t in chatgpt foldertree database albummodel covers; do
    "$BUILD/mvplayer-$t-test$EXE" -o "$TEST_LOG_ARG,txt" || TEST_RESULT=$?
    cat "$TEST_LOG"
    if [[ "$TEST_RESULT" -ne 0 ]]; then exit "$TEST_RESULT"; fi
done

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

echo "== align: an interrupted song"
# A reaction video: the whole song, stopped twice for talk. Every part of it
# is the track's waveform, and it is still not the song's video. Time before
# and after the song is another matter.
ffmpeg -v error -y -i "$WORK/track.flac" -f lavfi -i "anoisesrc=d=60:c=pink:r=48000:a=0.3:seed=3" -filter_complex \
    "[0]atrim=0:17[a];[1]atrim=0:12[n1];[0]atrim=17:34,asetpts=N/SR/TB[b];[1]atrim=20:32,asetpts=N/SR/TB[n2];[0]atrim=34,asetpts=N/SR/TB[c];[a][n1][b][n2][c]concat=n=5:v=0:a=1" \
    -c:a libopus -b:a 96k "$WORK/reaction.opus"
ffmpeg -v error -y -i "$WORK/track.flac" -f lavfi -i "anoisesrc=d=20:c=pink:r=48000:a=0.3:seed=4" -filter_complex \
    "[1][0]concat=n=2:v=0:a=1" -c:a libopus -b:a 96k "$WORK/intro.opus"
SUMMARY=$("$BUILD/mvplayer-import" align "$WORK/track.flac" "$WORK/reaction.opus")
echo "$SUMMARY" | grep -o "interrupted[^,]*"
[[ "$SUMMARY" == *"interrupted 2 times by 23."* || "$SUMMARY" == *"interrupted 2 times by 24."* ]]
[[ "$("$BUILD/mvplayer-import" align "$WORK/track.flac" "$WORK/intro.opus")" != *"interrupted"* ]]
# A video that opens with a few bars from the middle of the song and then
# plays it through is all song, in another order: not interrupted.
ffmpeg -v error -y -i "$WORK/track.flac" -filter_complex \
    "[0]atrim=20:30,asetpts=N/SR/TB[t];[t][0]concat=n=2:v=0:a=1" -c:a libopus -b:a 96k "$WORK/teaser.opus"
SUMMARY=$("$BUILD/mvplayer-import" align "$WORK/track.flac" "$WORK/teaser.opus")
echo "$SUMMARY" | grep -o "waveform[^,]*"
[[ "$SUMMARY" == *"segment(s)"* && "$SUMMARY" != *"interrupted"* ]]
# Versions by name: another singer the track does not have, and a version
# that only the album is named after.
[[ $("$BUILD/mvplayer-import" same-version "Twins" "Twins" "Twins / Producer feat. Someone Else" | tr -d '\r') == different ]]
[[ $("$BUILD/mvplayer-import" same-version "Twins feat. Someone" "Twins" "Twins / Producer feat. Someone Else" | tr -d '\r') == same ]]
[[ $("$BUILD/mvplayer-import" same-version "Far" "Rain -3 nuits ver.-" "Artist - Far MUSIC VIDEO" | tr -d '\r') == same ]]
[[ $("$BUILD/mvplayer-import" same-version "Rain -3 nuits ver.-" "Rain -3 nuits ver.-" "Artist - Rain MUSIC VIDEO" | tr -d '\r') == different ]]
# Whose upload: the artist's own channel is trusted whatever the title says,
# a channel that merely has the artist's name in it is not for a fan's title.
rank() { "$BUILD/mvplayer-import" rank-check "$@" | tr -d '\r'; }
[[ $(rank "Prism" "Clara" "Clara || Prism || Lyrics & Vietsub" "We love Val & Clara") == "not trusted, names the artist" ]]
[[ $(rank "Prism" "Clara" "Prism (Lyric Video) lyrics" "Clara Official YouTube Channel") == "the artist's channel, names the artist" ]]
[[ $(rank "Prism" "Clara" "Prism MV" "We love Val & Clara") == "trusted, names the artist" ]]
[[ $(rank "Prism" "Clara" "Prism / Somebody MV" "Somebody Official") == "trusted, does not name the artist" ]]
[[ $(rank "Prism" "Clara" "Clara - Prism (Music Video)" "Somebody" verified) == "trusted, names the artist" ]]
# A tag that holds the name in two scripts: either will do.
[[ $(rank "Again" "Kizu Ai (キズアイ)" "キズアイ - Again (Official)" "Somebody Records") == "trusted, names the artist" ]]
# On the artist's own channel the title's version words are not held
# against the upload; elsewhere they are. A label's channel is examined.
[[ $(rank "propose" "Lana" "【MV】propose / Lana cover" "Lana") == "the artist's channel, names the artist" ]]
[[ $(rank "propose" "Lana" "【MV】propose / Lana cover" "Somebody") == "dropped: “cover” version, names the artist" ]]
[[ $(rank "escalate" "Aimee" "Aimee - escalate MUSIC VIDEO&CROSSFADE" "Aimee Official YouTube Channel") == "the artist's channel, names the artist" ]]
[[ $(rank "Twins" "Clara" "Clara 『Twins』 MV" "Sunny Music (Japan)") == "trusted, names the artist" ]]
# "LIVE Ver." names the live version, which both sides carry as a word already.
[[ $("$BUILD/mvplayer-import" same-version "Neuro (feat. Rime) [Live at Virtual, 2022]" "Live" "Neuro feat. Rime (from Live) - Rim (LIVE Ver.)" | tr -d '\r') == same ]]
# A channel named twice over is the artist's by either name; so is "A and B",
# how YouTube names an upload listed under several channels.
[[ $(rank "Comet" "Nayu" "Comet - Nayu ft.初音ミク MV" "Nayu Official / Nayutan Seijin") == "the artist's channel, names the artist" ]]
[[ $(rank "Comet" "Echo" "Echo - Comet (Official Music Video)" "Nayu and Echo") == "the artist's channel, names the artist" ]]
# Spaces aside: "DJGenki" is "DJ Genki".
[[ $(rank "Nekroz" "DJ Genki" "07.DJ Genki - Nekroz" "DJGenki") == "the artist's channel, names the artist" ]]
[[ $(rank "Nekroz" "DJ Genki" "07.DJ Genki - Nekroz" "DJGenki Fan Uploads") == "not trusted, names the artist" ]]
# A synthesised voice credited on the artist's own channel is not another version; a human singer is.
[[ $("$BUILD/mvplayer-import" same-version "Comet" "Comet" "Comet (ft.初音ミク) OFFICIAL MUSIC VIDEO" own | tr -d '\r') == same ]]
[[ $("$BUILD/mvplayer-import" same-version "Comet" "Comet" "Comet feat. Somebody OFFICIAL MUSIC VIDEO" own | tr -d '\r') == different ]]
# A rhythm-game replay by its title.
[[ $(rank "Dance" "Clara" "Player | Clara - Dance [Wild] HDHR 99.34% #1 308pp" "Circle People" verified) == "dropped: game replay, names the artist" ]]
# A synthesised voice under its other name is the same singer.
[[ $("$BUILD/mvplayer-import" same-version "Comet" "Comet" "Comet (ft.初音ミク) OFFICIAL MUSIC VIDEO" | tr -d '\r') == different ]]
[[ $("$BUILD/mvplayer-import" same-version "Comet feat. Hatsune Miku" "Comet" "Comet (ft.初音ミク) OFFICIAL MUSIC VIDEO" | tr -d '\r') == same ]]
# A producer's upload names the producer whoever sings: the singer counts.
[[ $(rank "Whip" "Works feat.Rio" "【MV】Whip／mona（CV：Shiina）【Works】" "Works OFFICIAL" verified) == "the artist's channel, does not name the artist" ]]
[[ $(rank "Whip" "Works feat.Rio" "【MV】Whip／Works feat. Rio" "Works OFFICIAL" verified) == "the artist's channel, names the artist" ]]
[[ $("$BUILD/mvplayer-import" same-version "Whip" "Songs" "【MV】Whip／mona（CV：Shiina）【Works】" | tr -d '\r') == different ]]

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
# nothing usable (a Topic channel's upload). The importer must pause (waits
# doubling), put the tracks back untouched, and finish them once requests
# succeed again.
mkdir -p "$WORK/lib/A" "$WORK/mvlib"
# Two different songs: files are told apart by their sound.
song() { echo "aevalsrc=0.3*sin(2*PI*t*($1+$2*mod(floor(t/1.5)\,7)))+0.2*sin(2*PI*t*($1*1.5+$2*mod(floor(t/2)\,5))):s=48000:d=40"; }
ffmpeg -v error -y -f lavfi -i "$(song 220 30)" -metadata title="Song one" -metadata artist=A "$WORK/lib/A/one.flac"
ffmpeg -v error -y -f lavfi -i "$(song 311 47)" -metadata title="Song two" -metadata artist=A "$WORK/lib/A/two.flac"
cat > "$WORK/fake-ytdlp" <<FAKE
#!/bin/sh
n=\$(cat "$WORK/calls" 2>/dev/null || echo 0); n=\$((n+1)); echo \$n > "$WORK/calls"
if [ \$n -le 3 ]; then echo "ERROR: [youtube] x: Sign in to confirm you’re not a bot" >&2; exit 1; fi
echo '{"entries": [{"id": "tpc", "ie_key": "Youtube", "title": "Song", "channel": "A - Topic", "duration": 40, "view_count": 1000}]}'
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
# B5: a search answered with nothing at all, and the probe after it
# likewise, is a refusal, not "no video": the lookup is postponed as for a
# pause, and the query asked again after it. Here the third answer has an
# upload (nothing usable, C2).
cat > "$WORK/fake-b5" <<FAKE
#!/bin/sh
n=\$(cat "$WORK/calls-b5" 2>/dev/null || echo 0); n=\$((n+1)); echo \$n > "$WORK/calls-b5"
if [ \$n -le 2 ]; then echo '{"entries": []}'; exit 0; fi
echo '{"entries": [{"id": "tpc", "ie_key": "Youtube", "title": "Song", "channel": "A - Topic", "duration": 40, "view_count": 1000}]}'
FAKE
chmod +x "$WORK/fake-b5"
FAKE5="$WORK/fake-b5"
if [[ -n "$EXE" ]]; then
    printf '@"%s" "%%~dp0fake-b5" %%*\r\n' "$(cygpath -w "$(command -v sh)")" > "$WORK/fake-b5.cmd"
    FAKE5="$WORK/fake-b5.cmd"
fi
OUT=$(MVPLAYER_YTDLP="$FAKE5" MVPLAYER_PAUSE_SECS=1 timeout 60 "$BUILD/mvplayer-import" \
    --music-dir "$WORK/lib" --mv-dir "$WORK/mvlib-b5" --jobs 1 2>&1)
echo "$OUT" | grep -E "paused|resuming|^done:" | cut -c1-110
[[ "$OUT" == *"paused for 0:01: YouTube answered the search"*"and the probe after it likewise"* && "$OUT" == *"done: 0 videos; tracks: not_found=2"* ]]
grep -q '"probe":true,"results":0' "$WORK/mvlib-b5/.mvplayer/import-log.jsonl"
# B6: the probe is answered: the query matched nothing, and counts as
# answered. Every query of both tracks does here, and the probe is asked
# fresh each time, never from the cache.
cat > "$WORK/fake-b6" <<FAKE
#!/bin/sh
echo "\$*" >> "$WORK/probe-args"
case "\$*" in *"official music video"*) echo '{"entries": [{"id": "any", "ie_key": "Youtube", "title": "Any", "channel": "Anyone", "duration": 200, "view_count": 1}]}' ;;
*) echo '{"entries": []}' ;; esac
FAKE
chmod +x "$WORK/fake-b6"
FAKE6="$WORK/fake-b6"
if [[ -n "$EXE" ]]; then
    printf '@"%s" "%%~dp0fake-b6" %%*\r\n' "$(cygpath -w "$(command -v sh)")" > "$WORK/fake-b6.cmd"
    FAKE6="$WORK/fake-b6.cmd"
fi
rm -f "$WORK/probe-args"
OUT=$(MVPLAYER_YTDLP="$FAKE6" MVPLAYER_PAUSE_SECS=1 timeout 60 "$BUILD/mvplayer-import" \
    --music-dir "$WORK/lib" --mv-dir "$WORK/mvlib-b6" --jobs 1 2>&1)
echo "$OUT" | grep -E "paused|^\[import\] A|^done:" | cut -c1-110
[[ "$OUT" != *"paused"* && "$OUT" == *"the search matched nothing"* && "$OUT" == *"done: 0 videos; tracks: not_found=2"* ]]
[[ $(grep -c "official music video" "$WORK/probe-args") -ge 2 ]]
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
[[ "$OUT" == *"2 tracks: 0 new, 2 changed, 0 back, 0 gone"* && "$OUT" == *"done: 1 videos;"* ]]
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
[[ "$OUT" == *"3 tracks: 1 new, 2 changed, 0 back, 0 gone"* && "$OUT" == *"done: 1 videos;"* ]]
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
# passed over: it is brought in to wait for the user, with the track's audio
# and YouTube's side by side to compare. Accepted, it joins the library with
# the track's audio; turned down, it is deleted, its track has no video and
# is not offered that upload again.
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
    *audio.*) cp "$WORK/site-audio.opus" "\$dir/audio.opus"; echo '{"abr": 96, "acodec": "opus", "upload_date": "20230307", "release_date": "20230308", "description": "Dee - Five\\nOut now on every streaming service.\\n\\nDirected by Dee."}' > "\$dir/audio.info.json" ;;
    *video.*) cp "$WORK/site-video.mkv" "\$dir/video.mkv" ;;
    *preview.*) cp "$WORK/site-video.mkv" "\$dir/preview.mkv" ;;
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
OUT=$(MVPLAYER_YTDLP="$SITE" timeout 120 "$BUILD/mvplayer-import" --allow-still-images --music-dir "$WORK/lib5" --mv-dir "$WORK/mvlib5" 2>&1)
echo "$OUT" | grep -E "^\[import\]|^done:"
[[ "$OUT" == *"for your review (1 video)"* && "$OUT" == *"done: 0 videos and 1 for review; tracks: review=1"* ]]
[[ "$(streams)" == "h264 flac opus " && "$(review_state)" == "[(1, 'library')]" ]]
# Where the track sits in the video is kept with the video, and with its
# candidate for the candidate's stream: "start-end@lag,…" in seconds of the
# video, for the player to go from the track into its video at the same moment.
SPANS=$(python3 -c "import sqlite3, sys; db = sqlite3.connect(sys.argv[1]); print(db.execute('SELECT spans FROM videos').fetchone()[0], db.execute('SELECT spans FROM candidates').fetchone()[0])" "$WORK/mvlib5/.mvplayer/library.db")
echo "spans: $SPANS"
[[ "$SPANS" =~ ^([0-9]+\.[0-9]{3}-[0-9]+\.[0-9]{3}@-?[0-9]+\.[0-9]{3},?)+\ ([0-9]+\.[0-9]{3}-[0-9]+\.[0-9]{3}@-?[0-9]+\.[0-9]{3},?)+$ ]]
# When the upload was published is kept with the video: its premiere date, before its upload date.
[[ "$(python3 -c "import sqlite3, sys; print(sqlite3.connect(sys.argv[1]).execute(\"SELECT yt_date FROM videos WHERE yt_id = 'liv'\").fetchall())" "$WORK/mvlib5/.mvplayer/library.db")" == "[('2023-03-08',)]" ]]
# search, audio to listen to, the video. (The look at the picture is left out:
# its format selector has a ">" in it, which the Windows stand-in, a batch
# file, would take for a redirection.)
[[ $(wc -l < "$WORK/site-args") -eq 3 ]]
# A video from when review had a third stream, played by default, that
# alternated between the two: accepting it takes that stream out.
(cd "$WORK/mvlib5/Dee" && ffmpeg -v error -y -i "Five [liv].mkv" -map 0:v:0 -map 0:a:0 -map 0:a:1 -map 0:a:0 -c copy \
    -disposition:a:0 0 -disposition:a:2 default -f matroska old.part && mv old.part "Five [liv].mkv")
[[ "$(streams)" == "h264 flac opus flac " ]]
OUT=$(MVPLAYER_YTDLP="$SITE" timeout 120 "$BUILD/mvplayer-import" --allow-still-images --music-dir "$WORK/lib5" --mv-dir "$WORK/mvlib5" --approve liv 2>&1)
echo "$OUT" | grep -E "^\[review\]|^done:"
[[ "$OUT" == *"done: 1 videos; tracks: done=1"* ]]
[[ "$(streams)" == "h264 flac opus " && "$(review_state)" == "[(0, 'library')]" ]]
# The approval vouches for that upload, and for nothing else of its channel.
[[ "$(python3 -c "import sqlite3, sys; db = sqlite3.connect(sys.argv[1]); print(db.execute('SELECT vouched FROM videos').fetchall(), db.execute(\"SELECT COUNT(*) FROM artist_channels WHERE source = 'approved'\").fetchall())" "$WORK/mvlib5/.mvplayer/library.db")" == "[(1,)] [(0,)]" ]]
# Put back under review, and turned down this time.
python3 - "$WORK/mvlib5/.mvplayer/library.db" <<'PY'
import sqlite3, sys
db = sqlite3.connect(sys.argv[1])
db.execute("UPDATE videos SET review = 1")
db.execute("INSERT INTO candidates (video_id, track_key, stream, fit, audio_detail) SELECT v.id, t.recording, 1, 0.5, v.audio_detail FROM videos v, tracks t")
db.execute("UPDATE tracks SET state = 'review', video_id = NULL")
db.commit()
PY
OUT=$(MVPLAYER_YTDLP="$SITE" timeout 120 "$BUILD/mvplayer-import" --allow-still-images --music-dir "$WORK/lib5" --mv-dir "$WORK/mvlib5" --reject liv 2>&1)
echo "$OUT" | grep -E "^\[review\]|^done:"
[[ "$OUT" == *"done: 0 videos; tracks: not_found=1"* ]]
[[ ! -e "$WORK/mvlib5/Dee/Five [liv].mkv" ]]
# Looking again finds the same upload and leaves it alone: one search, nothing fetched.
OUT=$(MVPLAYER_YTDLP="$SITE" timeout 120 "$BUILD/mvplayer-import" --allow-still-images --music-dir "$WORK/lib5" --mv-dir "$WORK/mvlib5" --retry 2>&1)
[[ "$OUT" == *"done: 0 videos; tracks: not_found=1"* ]]
[[ $(wc -l < "$WORK/site-args") -eq 4 ]]
python3 - "$WORK/mvlib5/.mvplayer/library.db" <<'PY'
import sqlite3, sys
db = sqlite3.connect(sys.argv[1])
assert db.execute("SELECT COUNT(*) FROM videos").fetchone()[0] == 0
assert [y for _, y in db.execute("SELECT key, yt_id FROM rejected_videos")] == ["liv"]
assert "turned down" in db.execute("SELECT message FROM tracks").fetchone()[0]
print("review ok")
PY
# The log says what was decided about the upload, why, and on what numbers;
# the report puts it in a table.
python3 - "$WORK/mvlib5/.mvplayer/import-log.jsonl" <<'PY'
import json, sys
events = [json.loads(l) for l in open(sys.argv[1], encoding="utf-8")]
look = [e for e in events if e.get("event") == "lookup"][0]
c = look["checked"][0]
assert look["decision"] == "review" and c["decision"] == "review" and c["reason"], look
m = c["measured"]
assert m["fingerprintCoverage"] > 0.8 and 0.2 < m["sameWaveform"] < 0.72 and "loudnessCorrelation" in m, m
assert [e["verdict"] for e in events if e.get("event") == "verdict"] == ["accept", "reject"]
assert any(e.get("event") == "track" and e["what"] == "new" for e in events)
PY
# What was fetched for the candidate is remembered for the next track that
# meets it, and the log says how much of a lookup needed no request.
[[ -f "$WORK/mvlib5/.mvplayer/cache/videos/liv/audio.opus" && -f "$WORK/mvlib5/.mvplayer/cache/videos/liv/audio.info.json" ]]
grep -q '"fromCache"' "$WORK/mvlib5/.mvplayer/import-log.jsonl"
REPORT=$(python3 "$(dirname "$0")/../tools/import-report.py" "$WORK/mvlib5")
echo "$REPORT" | tail -1 | cut -c1-200
[[ "$REPORT" == *"review → you: reject"* && "$REPORT" == *"46%"* ]]
# The same upload for a lossy copy of the track, the lesser audio next to
# YouTube's: it waits for the user all the same (D1.14), with YouTube's
# audio alone. Being the song is not enough to take it outright.
mkdir -p "$WORK/lib5b/Dee"
ffmpeg -v error -y -i "$WORK/lib5/Dee/five.flac" -c:a libopus -b:a 32k -metadata title="Five" -metadata artist=Dee "$WORK/lib5b/Dee/five.opus"
touch -d "10 seconds ago" "$WORK/lib5b/Dee/five.opus"
OUT=$(MVPLAYER_YTDLP="$SITE" timeout 120 "$BUILD/mvplayer-import" --allow-still-images --music-dir "$WORK/lib5b" --mv-dir "$WORK/mvlib5b" 2>&1)
echo "$OUT" | grep -E "^\[import\]|^done:"
[[ "$OUT" == *"done: 0 videos and 1 for review; tracks: review=1"* ]]
[[ "$(cd "$WORK/mvlib5b/Dee" && ffprobe -v error -show_entries stream=codec_name -of csv=p=0 "Five [liv].mkv" | tr -d '\r' | tr '\n' ' ')" == "h264 opus " ]]
[[ "$(python3 -c "import sqlite3, sys; print(sqlite3.connect(sys.argv[1]).execute('SELECT review, audio_source FROM videos').fetchall())" "$WORK/mvlib5b/.mvplayer/library.db")" == "[(1, 'youtube')]" ]]
# The track's own recording on the artist's channel, but titled as another
# version: a live performance or a dance practice carries the studio audio
# whole, and is not the music video. Not taken outright, however much of the
# waveform is the track's (D1.13, R3): it waits for the user.
ffmpeg -v error -y -i "$WORK/lib5/Dee/five.flac" -c:a libopus -b:a 96k "$WORK/site-audio.opus"
echo '{"entries": [{"id": "lve", "ie_key": "Youtube", "title": "Dee - Five (Live at the Hall)", "channel": "Dee", "duration": 40, "view_count": 1000, "channel_is_verified": true}]}' > "$WORK/site-search.json"
OUT=$(MVPLAYER_YTDLP="$SITE" timeout 120 "$BUILD/mvplayer-import" --allow-still-images --music-dir "$WORK/lib5" --mv-dir "$WORK/mvlib5c" 2>&1)
echo "$OUT" | grep -E "^\[import\]|^done:"
[[ "$OUT" == *"done: 0 videos and 1 for review; tracks: review=1"* ]]
grep -q "marked as different versions" "$WORK/mvlib5c/.mvplayer/import-log.jsonl"
# Titled as the song, it is taken outright.
echo '{"entries": [{"id": "stu", "ie_key": "Youtube", "title": "Dee - Five (Official Video)", "channel": "Dee", "duration": 40, "view_count": 1000, "channel_is_verified": true}]}' > "$WORK/site-search.json"
OUT=$(MVPLAYER_YTDLP="$SITE" timeout 120 "$BUILD/mvplayer-import" --allow-still-images --music-dir "$WORK/lib5" --mv-dir "$WORK/mvlib5d" 2>&1)
echo "$OUT" | grep -E "^\[import\]|^done:"
[[ "$OUT" == *"done: 1 videos; tracks: done=1"* ]]
grep -q "the track's recording: 100% of it" "$WORK/mvlib5d/.mvplayer/import-log.jsonl"
# (The stand-in site back to the upload the tests below expect.)
ffmpeg -v error -y -i "$WORK/lib5/Dee/five.flac" -af atempo=1.004 -c:a libopus -b:a 96k "$WORK/site-audio.opus"
echo '{"entries": [{"id": "liv", "ie_key": "Youtube", "title": "Dee - Five", "channel": "Dee", "duration": 40, "view_count": 1000, "channel_is_verified": true}]}' > "$WORK/site-search.json"

echo "== an empty video file"
# A machine that goes down while a video is being written can leave its file
# empty. Looked up again, the track lets that video go and brings the upload
# in anew.
OUT=$(MVPLAYER_YTDLP="$SITE" timeout 120 "$BUILD/mvplayer-import" --allow-still-images --music-dir "$WORK/lib5" --mv-dir "$WORK/mvlib11" 2>&1)
[[ "$OUT" == *"done: 0 videos and 1 for review; tracks: review=1"* ]]
: > "$WORK/mvlib11/Dee/Five [liv].mkv"
OUT=$(MVPLAYER_YTDLP="$SITE" timeout 120 "$BUILD/mvplayer-import" --allow-still-images --music-dir "$WORK/lib5" --mv-dir "$WORK/mvlib11" \
    --reimport "$WORK/lib5/Dee/five.flac" 2>&1)
echo "$OUT" | grep -E "^\[import\]|^done:"
[[ "$OUT" == *"deleted “Dee - Five”, whose file is empty"* && "$OUT" == *"done: 0 videos and 1 for review; tracks: review=1"* ]]
[[ -s "$WORK/mvlib11/Dee/Five [liv].mkv" ]]
grep -q '"event":"broken-video"' "$WORK/mvlib11/.mvplayer/import-log.jsonl"

echo "== a cut of the song"
# The video is the first four fifths of the track: all of it the track's
# waveform, and still a cut. It waits for the user rather than standing in
# for the whole song.
mkdir -p "$WORK/lib10/Hal" "$WORK/mvlib10"
ffmpeg -v error -y -f lavfi -i "$(song 410 29 | sed 's/d=40/d=50/')" -metadata title="Seven" -metadata artist=Hal "$WORK/lib10/Hal/seven.flac"
touch -d "10 seconds ago" "$WORK/lib10/Hal/seven.flac"
ffmpeg -v error -y -i "$WORK/lib10/Hal/seven.flac" -t 40 -c:a libopus -b:a 96k "$WORK/site-audio.opus"
echo '{"entries": [{"id": "cut", "ie_key": "Youtube", "title": "Hal - Seven (Official Video)", "channel": "Hal", "duration": 40, "view_count": 1000, "channel_is_verified": true}]}' > "$WORK/site-search.json"
OUT=$(MVPLAYER_YTDLP="$SITE" timeout 120 "$BUILD/mvplayer-import" --allow-still-images --music-dir "$WORK/lib10" --mv-dir "$WORK/mvlib10" --jobs 1 2>&1)
echo "$OUT" | grep -E "^\[import\]|^done:" | cut -c1-150
[[ "$OUT" == *"for your review (1 video)"* && "$OUT" == *"done: 0 videos and 1 for review"* ]]
grep -q "a cut of the song: 40 s of 50 s" "$WORK/mvlib10/.mvplayer/import-log.jsonl"
# Under half the track it is not examined at all: the opening of a show
# against the whole song. Nothing of it is downloaded.
mkdir -p "$WORK/mvlib12"
rm -f "$WORK/site-args"
echo '{"entries": [{"id": "tvs", "ie_key": "Youtube", "title": "Hal - Seven (Official Video)", "channel": "Hal", "duration": 24, "view_count": 1000, "channel_is_verified": true}]}' > "$WORK/site-search.json"
OUT=$(MVPLAYER_YTDLP="$SITE" timeout 120 "$BUILD/mvplayer-import" --allow-still-images --music-dir "$WORK/lib10" --mv-dir "$WORK/mvlib12" --jobs 1 2>&1)
echo "$OUT" | grep -E "^\[import\]|^done:" | cut -c1-150
[[ "$OUT" == *"done: 0 videos; tracks: not_found=1"* ]]
grep -q "under half the track's length" "$WORK/mvlib12/.mvplayer/import-log.jsonl"
! grep -q -v ytsearch "$WORK/site-args"
# Nor is one over twice the track's length, or more than four minutes
# longer: the whole song against its opening cut, a live video of two songs.
mkdir -p "$WORK/lib21/Lee" "$WORK/mvlib21" "$WORK/mvlib22"
ffmpeg -v error -y -f lavfi -i "$(song 380 27 | sed 's/d=40/d=300/')" -metadata title="Epic" -metadata artist=Lee "$WORK/lib21/Lee/epic.flac"
touch -d "10 seconds ago" "$WORK/lib21/Lee/epic.flac"
toolong() { rm -f "$WORK/site-args"; MVPLAYER_YTDLP="$SITE" timeout 120 "$BUILD/mvplayer-import" --allow-still-images --jobs 1 "$@" 2>&1 | grep -E "^\[import\]|^done:" | cut -c1-150; }
echo '{"entries": [{"id": "dbl", "ie_key": "Youtube", "title": "Hal - Seven (Official Video)", "channel": "Hal", "duration": 104, "view_count": 1000, "channel_is_verified": true}]}' > "$WORK/site-search.json"
OUT=$(toolong --music-dir "$WORK/lib10" --mv-dir "$WORK/mvlib21"); echo "$OUT"
[[ "$OUT" == *"done: 0 videos; tracks: not_found=1"* ]] && grep -q '"rejected":"duration mismatch"' "$WORK/mvlib21/.mvplayer/import-log.jsonl"
! grep -q -v ytsearch "$WORK/site-args"
echo '{"entries": [{"id": "two", "ie_key": "Youtube", "title": "Lee - Epic (Official Video)", "channel": "Lee", "duration": 545, "view_count": 1000, "channel_is_verified": true}]}' > "$WORK/site-search.json"
OUT=$(toolong --music-dir "$WORK/lib21" --mv-dir "$WORK/mvlib22"); echo "$OUT"
[[ "$OUT" == *"done: 0 videos; tracks: not_found=1"* ]] && grep -q '"rejected":"duration mismatch"' "$WORK/mvlib22/.mvplayer/import-log.jsonl"
! grep -q -v ytsearch "$WORK/site-args"
echo '{"entries": [{"id": "tvs", "ie_key": "Youtube", "title": "Hal - Seven (Official Video)", "channel": "Hal", "duration": 24, "view_count": 1000, "channel_is_verified": true}]}' > "$WORK/site-search.json"
# A video that waits for review from before that rule: its track is no
# longer a candidate of it once the track is re-imported, and with no
# candidate left it goes. Here the cut above, taken for 20 s long (and
# yesterday's search results, which have it at 40 s, forgotten).
python3 -c "import sqlite3, sys; db = sqlite3.connect(sys.argv[1]); db.execute('UPDATE videos SET duration = 20'); db.commit()" "$WORK/mvlib10/.mvplayer/library.db"
rm -rf "$WORK/mvlib10/.mvplayer/cache"
[[ -s "$WORK/mvlib10/Hal/Seven [cut].mkv" ]]
OUT=$(MVPLAYER_YTDLP="$SITE" timeout 120 "$BUILD/mvplayer-import" --allow-still-images --music-dir "$WORK/lib10" --mv-dir "$WORK/mvlib10" --jobs 1 --reimport "$WORK/lib10" 2>&1)
echo "$OUT" | grep -E "^\[import\]|^done:" | cut -c1-150
[[ "$OUT" == *"no longer a candidate of “Hal - Seven (Official Video)”, which waits for review: under half the track's length"* ]]
[[ "$OUT" == *"done: 0 videos; tracks: not_found=1"* ]]
[[ ! -e "$WORK/mvlib10/Hal/Seven [cut].mkv" ]]
grep -q '"event":"outdated-option"' "$WORK/mvlib10/.mvplayer/import-log.jsonl"

echo "== a video changes hands"
# "Sixth" is the song "Six" with something else over it for one second in
# four, and a little longer (another language's version, as it were).
# Looked up first, it is given the video: three quarters of it are the
# video's audio.
# When "Six" itself turns up, it is the closer fit to the upload: a video has
# one track, so "Six" takes it, the video plays "Six" from then on, and
# "Sixth" goes back to the queue. Looked up again it finds the same upload,
# which has its track now, and no other: it has no video.
mkdir -p "$WORK/lib8/Gil" "$WORK/lib9/Gil" "$WORK/mvlib9"
ffmpeg -v error -y -f lavfi -i "$(song 350 47 | sed 's/d=40/d=50/')" -metadata title="Six" -metadata artist=Gil "$WORK/lib9/Gil/six.flac"
ffmpeg -v error -y -i "$WORK/lib9/Gil/six.flac" -f lavfi -i "$(song 520 31 | sed 's/d=40/d=55/')" -filter_complex \
    "[1]volume='if(lt(mod(t\,4)\,1)\,6\,0)':eval=frame[g];[0]apad=whole_dur=55[p];[p][g]amix=inputs=2:normalize=0:duration=longest,volume=0.25" \
    -metadata title="Sixth" -metadata artist=Gil "$WORK/lib8/Gil/sixth.flac"
touch -d "10 seconds ago" "$WORK/lib8/Gil/sixth.flac" "$WORK/lib9/Gil/six.flac"
ffmpeg -v error -y -i "$WORK/lib9/Gil/six.flac" -c:a libopus -b:a 96k "$WORK/site-audio.opus"
ffmpeg -v error -y -i "$WORK/mv.mkv" -map 0:v:0 -c copy -t 50 "$WORK/site-video.mkv"
echo '{"entries": [{"id": "own", "ie_key": "Youtube", "title": "Gil - Six (Official Video)", "channel": "Gil", "duration": 50, "view_count": 1000, "channel_is_verified": true}]}' > "$WORK/site-search.json"
OUT=$(MVPLAYER_YTDLP="$SITE" timeout 120 "$BUILD/mvplayer-import" --allow-still-images --music-dir "$WORK/lib8" --mv-dir "$WORK/mvlib9" --jobs 1 2>&1)
echo "$OUT" | grep -E "^\[import\]|^done:" | cut -c1-150
[[ "$OUT" == *"done: 1 videos; tracks: done=1"* ]]
OUT=$(MVPLAYER_YTDLP="$SITE" timeout 120 "$BUILD/mvplayer-import" --allow-still-images --music-dir "$WORK/lib8" --music-dir "$WORK/lib9" --mv-dir "$WORK/mvlib9" --jobs 1 2>&1)
echo "$OUT" | grep -E "^\[import\]|^done:" | cut -c1-170
[[ "$OUT" == *"takes “Gil - Six (Official Video)” from “Sixth”: the closer fit"* && "$OUT" == *"done: 1 videos; tracks:"* ]]
python3 - "$WORK/mvlib9/.mvplayer/library.db" "$WORK/mvlib9/.mvplayer/import-log.jsonl" <<'PY'
import json, sqlite3, sys
db = sqlite3.connect(sys.argv[1])
rows = {t: (s, v, m) for t, s, v, m in db.execute("SELECT title, state, video_id IS NOT NULL, message FROM tracks")}
assert rows["Six"][:2] == ("done", 1) and "the closer fit" in rows["Six"][2], rows
assert rows["Sixth"][:2] == ("not_found", 0) and "is the video of “Six”" in rows["Sixth"][2], rows
assert db.execute("SELECT title, audio_source, yt_stream FROM videos").fetchall() == [("Six", "library", 1)]
assert db.execute("SELECT COUNT(*) FROM rejected_videos").fetchone()[0] == 0
events = [json.loads(l) for l in open(sys.argv[2], encoding="utf-8")]
take = [e for e in events if e.get("event") == "takeover"]
assert len(take) == 1 and take[0]["track"] == "Six" and take[0]["fit"] > take[0]["holderFit"] > 0.1, take
assert [r["track"] for r in take[0]["requeued"]] == ["Sixth"], take
# "Sixth", looked up again: the video has its track, the closer fit.
again = [e for e in events if e.get("event") == "lookup" and e["track"] == "Sixth"][-1]
assert again["checked"][0]["result"] == "has-its-track" and again["decision"] == "reject", again
print("takeover ok")
PY
# The video now plays "Six": all of it the same waveform.
# (From inside the folder: a bracketed path is not translated for ffmpeg on Windows.)
BIN=$(cd "$BUILD" && pwd)
TAKEN=$(cd "$WORK/mvlib9/Gil" && "$BIN/mvplayer-import" align "$WORK/lib9/Gil/six.flac" "$(ls *.mkv)")
[[ "$TAKEN" == *"100% of it plainly the same"* && "$TAKEN" == *"in 1 segment(s)"* ]]

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
*" -J "*) if [ -f "$WORK/expired" ]; then
        echo "WARNING: [youtube] The provided YouTube account cookies are no longer valid." >&2
        echo '{"formats": [{"format_id": "251", "vcodec": "none", "acodec": "opus", "abr": 130}]}'
    else
        echo '{"formats": [{"format_id": "774", "vcodec": "none", "acodec": "opus", "abr": 250}]}'
    fi ;;
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
# Cookies the browser has rotated since: yt-dlp only warns and lists what
# anyone is offered. That must be said, not pass for "nothing better".
touch "$WORK/expired"
C=$(MVPLAYER_YTDLP="$PREMIUM" "$BUILD/mvplayer-import" --cookies "$WORK/cookies.txt" check-cookies || true); echo "$C"
[[ "$C" == expired:* ]]
OUT=$(MVPLAYER_YTDLP="$PREMIUM" timeout 120 "$BUILD/mvplayer-import" --music-dir "$WORK/lib4" --mv-dir "$WORK/mvlib4" \
    --cookies "$WORK/cookies.txt" --check-quality 2>&1)
echo "$OUT" | grep -E "^\[quality\]"
[[ "$OUT" == *"0 upgraded, 1 failed; stopped because the account's cookies have expired"* ]]
rm "$WORK/expired" "$WORK/premium-args"
C=$(MVPLAYER_YTDLP="$PREMIUM" "$BUILD/mvplayer-import" --cookies "$WORK/cookies.txt" check-cookies); echo "$C"
[[ "$C" == valid:*"250 kbit/s" ]]
rm "$WORK/premium-args"
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

echo "== subtitles"
# YouTube's subtitle format: plain text becomes .srt for the player to style;
# colours and positions are kept as .ass.
cat > "$WORK/plain.srv3" <<'XML'
<?xml version="1.0" encoding="utf-8" ?><timedtext format="3"><body>
<p t="1000" d="2000">First &amp; second
line</p>
<p t="3000" d="500"> </p>
<p t="4000" d="1500">Last</p>
</body></timedtext>
XML
cat > "$WORK/styled.srv3" <<'XML'
<?xml version="1.0" encoding="utf-8" ?><timedtext format="3"><head>
<pen id="1" b="1" fc="#FF5577"/><wp id="1" ap="1" ah="50" av="10"/>
</head><body>
<p t="1000" d="2000" wp="1"><s p="1">Red on top</s></p>
</body></timedtext>
XML
"$BUILD/mvplayer-import" convert-subs "$WORK/plain.srv3" "$WORK/plain.en" | tr -d '\r'
grep -q "00:00:01,000 --> 00:00:03,000" "$WORK/plain.en.srt"
grep -q "First & second" "$WORK/plain.en.srt"
[[ $(grep -c -- "-->" "$WORK/plain.en.srt") -eq 2 ]]
"$BUILD/mvplayer-import" convert-subs "$WORK/styled.srv3" "$WORK/styled.en" | tr -d '\r'
grep -q 'an8\\pos(640,84)' "$WORK/styled.en.ass"
grep -q 'c&H7755FF&' "$WORK/styled.en.ass"
# A yt-dlp that has British English subtitles only: they serve for "en", and
# land beside the video that lacked them.
cat > "$WORK/fake-subs" <<FAKE
#!/bin/sh
out=; prev=
for a in "\$@"; do [ "\$prev" = "-o" ] && out=\$a; prev=\$a; done
case " \$* " in *" --write-subs "*) cp "$WORK/plain.srv3" "\$(dirname "\$out")/subs.en-GB.srv3" ;; esac
FAKE
chmod +x "$WORK/fake-subs"
SUBS="$WORK/fake-subs"
if [[ -n "$EXE" ]]; then
    printf '@"%s" "%%~dp0fake-subs" %%*\r\n' "$(cygpath -w "$(command -v sh)")" > "$WORK/fake-subs.cmd"
    SUBS="$WORK/fake-subs.cmd"
fi
OUT=$(MVPLAYER_YTDLP="$SUBS" timeout 120 "$BUILD/mvplayer-import" --music-dir "$WORK/lib4" --mv-dir "$WORK/mvlib4" \
    --subtitles en --fetch-subtitles 2>&1)
echo "$OUT" | grep -E "^\[subtitles\]"
[[ "$OUT" == *"1 with subtitles, 0 failed"* ]]
grep -q "First & second" "$WORK/mvlib4/C/Low [xyz].en.srt"

echo "== talk tracks"
# Talk between songs is skipped without asking YouTube, when its title says
# so and it sounds like it: bursts with gaps and no pulse. A song that is
# merely titled like talk is looked up, and so is talk under a song's title.
mkdir -p "$WORK/lib6/E" "$WORK/mvlib6"
TALK="volume='if(lt(mod(t*1.7+2*sin(t*0.9)+1.3*sin(t*2.3)\,1.3)\,0.55)\,1\,0.02)':eval=frame"
ffmpeg -v error -y -f lavfi -i "anoisesrc=d=30:c=pink:r=48000:a=0.5:seed=1" -af "$TALK" -metadata title="MC1" -metadata artist=Eve "$WORK/lib6/E/mc1.flac"
ffmpeg -v error -y -f lavfi -i "anoisesrc=d=31:c=pink:r=48000:a=0.5:seed=2" -af "$TALK" -metadata title="Talk to Me" -metadata artist=Eve "$WORK/lib6/E/song-title.flac"
ffmpeg -v error -y -f lavfi -i "$(song 300 37)" -metadata title="MC 2" -metadata artist=Eve "$WORK/lib6/E/mc2.flac"
touch -d "10 seconds ago" "$WORK"/lib6/E/*.flac
CHECK=$("$BUILD/mvplayer-import" talk-check "$WORK/lib6/E/mc1.flac" "$WORK/lib6/E/song-title.flac" "$WORK/lib6/E/mc2.flac" | tr -d '\r')
echo "$CHECK" | cut -c1-44
[[ $(echo "$CHECK" | sed -n 1p) == talk*"title talk"* ]]
[[ $(echo "$CHECK" | sed -n 2p) == talk*"title -"* ]]
[[ $(echo "$CHECK" | sed -n 3p) == music*"title talk"* ]]
cat > "$WORK/fake-empty" <<FAKE
#!/bin/sh
echo "\$*" >> "$WORK/empty-args"
echo '{"entries": [{"id": "tpc", "ie_key": "Youtube", "title": "Song", "channel": "E - Topic", "duration": 40, "view_count": 1000}]}'
FAKE
chmod +x "$WORK/fake-empty"
EMPTY="$WORK/fake-empty"
if [[ -n "$EXE" ]]; then
    printf '@"%s" "%%~dp0fake-empty" %%*\r\n' "$(cygpath -w "$(command -v sh)")" > "$WORK/fake-empty.cmd"
    EMPTY="$WORK/fake-empty.cmd"
fi
MVPLAYER_YTDLP="$EMPTY" timeout 120 "$BUILD/mvplayer-import" --music-dir "$WORK/lib6" --mv-dir "$WORK/mvlib6" --jobs 1 2>&1 | grep -E "^\[import\]|^done:"
python3 - "$WORK/mvlib6/.mvplayer/library.db" "$WORK/empty-args" <<'PY'
import sqlite3, sys
db = sqlite3.connect(sys.argv[1])
state = dict(db.execute("SELECT title, state FROM tracks"))
assert state == {"MC1": "skipped", "Talk to Me": "not_found", "MC 2": "not_found"}, state
assert "talk, not a song" in db.execute("SELECT message FROM tracks WHERE title = 'MC1'").fetchone()[0]
asked = open(sys.argv[2], encoding="utf-8").read()
assert "MC1" not in asked and "Talk to Me" in asked and "MC 2" in asked, asked
print("talk tracks ok")
PY

echo "== music folders removed and added"
# A track whose folder leaves the library is kept as absent, its video stays
# and counts as untracked. When the folder is back, the track has its video
# again without a lookup. Deleting untracked videos takes file and subtitles.
tracks_low() { python3 -c "import sqlite3, sys; print(sqlite3.connect(sys.argv[1]).execute(\"SELECT absent, state, video_id IS NOT NULL FROM tracks WHERE title = 'Low'\").fetchall())" "$WORK/mvlib4/.mvplayer/library.db"; }
[[ "$(tracks_low)" == "[(0, 'done', 1)]" && -f "$WORK/mvlib4/C/Low [xyz].en.srt" ]]
: > "$WORK/empty-args"
OUT=$(MVPLAYER_YTDLP="$EMPTY" timeout 120 "$BUILD/mvplayer-import" --music-dir "$WORK/lib6" --mv-dir "$WORK/mvlib4" --jobs 1 2>&1)
echo "$OUT" | grep -E "^\[scan\]"
[[ "$OUT" == *"1 gone"* && "$(tracks_low)" == "[(1, 'done', 1)]" && -f "$WORK/mvlib4/C/Low [xyz].mkv" ]]
OUT=$(MVPLAYER_YTDLP="$EMPTY" timeout 120 "$BUILD/mvplayer-import" --music-dir "$WORK/lib6" --music-dir "$WORK/lib4" --mv-dir "$WORK/mvlib4" --jobs 1 2>&1)
echo "$OUT" | grep -E "^\[scan\]"
[[ "$OUT" == *"1 back"* && "$(tracks_low)" == "[(0, 'done', 1)]" ]]
! grep -q "Low" "$WORK/empty-args"
OUT=$(MVPLAYER_YTDLP="$EMPTY" timeout 120 "$BUILD/mvplayer-import" --music-dir "$WORK/lib6" --mv-dir "$WORK/mvlib4" --jobs 1 --delete-untracked 2>&1)
echo "$OUT" | grep -E "^deleted|^\[library\]"
[[ "$OUT" == *"deleted 1 untracked videos"* && "$(tracks_low)" == "[]" ]]
[[ ! -e "$WORK/mvlib4/C/Low [xyz].mkv" && ! -e "$WORK/mvlib4/C/Low [xyz].en.srt" && ! -d "$WORK/mvlib4/C" ]]

echo "== music library wider than the video folders"
# A12: the music library (--library-dir) is read for the player; only the
# tracks under a video folder (--music-dir) or on an album asked for are
# fingerprinted and looked up. A13 and H9: asking for the album, or a
# re-import, brings a track in.
mkdir -p "$WORK/lib-ml/Look/Al" "$WORK/lib-ml/Only/Bl" "$WORK/lib-ml/Only/Cl" "$WORK/mvlib-ml"
ffmpeg -v error -y -f lavfi -i "$(song 262 31)" -metadata title="Looked" -metadata artist=Gia -metadata album_artist=Gia -metadata album=Al -metadata track=1 "$WORK/lib-ml/Look/Al/01.flac"
ffmpeg -v error -y -f lavfi -i "$(song 294 41)" -metadata title="Indexed" -metadata artist=Gia -metadata album_artist=Gia -metadata album=Bl -metadata track=1 -metadata disc=1 "$WORK/lib-ml/Only/Bl/01.flac"
ffmpeg -v error -y -f lavfi -i "$(song 349 29)" -metadata title="Indexed two" -metadata artist=Gia -metadata album_artist=Gia -metadata album=Bl -metadata track=2 -metadata disc=1 "$WORK/lib-ml/Only/Bl/02.flac"
ffmpeg -v error -y -f lavfi -i "$(song 392 53)" -metadata title="Indexed three" -metadata artist=Gia -metadata album_artist=Gia -metadata album=Cl -metadata track=1 "$WORK/lib-ml/Only/Cl/01.flac"
touch -d "10 seconds ago" "$WORK"/lib-ml/*/*/*.flac
tracks_ml() { python3 -c "import sqlite3, sys; print(sqlite3.connect(sys.argv[1]).execute(\"SELECT title, lookup, fingerprint IS NOT NULL, state FROM tracks ORDER BY title\").fetchall())" "$WORK/mvlib-ml/.mvplayer/library.db"; }
echo 100 > "$WORK/calls"
OUT=$(MVPLAYER_YTDLP="$FAKE" timeout 120 "$BUILD/mvplayer-import" --library-dir "$WORK/lib-ml" --music-dir "$WORK/lib-ml/Look" --mv-dir "$WORK/mvlib-ml" --jobs 1 2>&1)
echo "$OUT" | grep -E "^\[scan\]|^done:"
[[ "$OUT" == *"4 tracks: 4 new"* && "$OUT" == *"done: 0 videos; tracks: not_found=1"* ]]
[[ "$(tracks_ml)" == "[('Indexed', 0, 0, 'pending'), ('Indexed three', 0, 0, 'pending'), ('Indexed two', 0, 0, 'pending'), ('Looked', 1, 1, 'not_found')]" ]]
grep -q '"what":"indexed"' "$WORK/mvlib-ml/.mvplayer/import-log.jsonl"
# The video folders alone would take the rest of the library for gone: refused.
! OUT=$("$BUILD/mvplayer-import" --music-dir "$WORK/lib-ml/Look" --mv-dir "$WORK/mvlib-ml" 2>&1)
[[ "$OUT" == *"--library-dir"* ]]
# An album asked for is looked up wherever its files are (A13).
echo 100 > "$WORK/calls"
OUT=$(MVPLAYER_YTDLP="$FAKE" timeout 120 "$BUILD/mvplayer-import" --library-dir "$WORK/lib-ml" --music-dir "$WORK/lib-ml/Look" --mv-dir "$WORK/mvlib-ml" --jobs 1 --lookup-album "Gia/Bl" 2>&1)
echo "$OUT" | grep -E "^looking|^\[scan\]|^done:"
[[ "$OUT" == *"4 tracks: 0 new, 2 changed"* && "$OUT" == *"tracks: not_found=3"* ]]
[[ "$(tracks_ml)" == "[('Indexed', 1, 1, 'not_found'), ('Indexed three', 0, 0, 'pending'), ('Indexed two', 1, 1, 'not_found'), ('Looked', 1, 1, 'not_found')]" ]]
# A re-import names the track: looked up from now on (H9).
echo 100 > "$WORK/calls"
OUT=$(MVPLAYER_YTDLP="$FAKE" timeout 120 "$BUILD/mvplayer-import" --library-dir "$WORK/lib-ml" --music-dir "$WORK/lib-ml/Look" --mv-dir "$WORK/mvlib-ml" --jobs 1 --reimport "$WORK/lib-ml/Only/Cl" 2>&1)
echo "$OUT" | grep -E "^re-importing|^done:"
[[ "$OUT" == *"re-importing 1 tracks"* && "$OUT" == *"tracks: not_found=4"* ]]
[[ "$(python3 -c "import sqlite3, sys; print(sqlite3.connect(sys.argv[1]).execute(\"SELECT lookup, state FROM tracks WHERE title = 'Indexed three'\").fetchall())" "$WORK/mvlib-ml/.mvplayer/library.db")" == "[(2, 'not_found')]" ]]

echo "== a download that trickles"
# YouTube sometimes serves a download at a crawl. It is given up after two
# intervals without progress instead of holding a job for half an hour.
mkdir -p "$WORK/lib7/F" "$WORK/mvlib7"
ffmpeg -v error -y -f lavfi -i "$(song 330 43)" -metadata title="Slow" -metadata artist=Fay "$WORK/lib7/F/slow.flac"
touch -d "10 seconds ago" "$WORK/lib7/F/slow.flac"
cat > "$WORK/fake-slow" <<FAKE
#!/bin/sh
case " \$* " in
*" -J "*) echo '{"entries": [{"id": "slw", "ie_key": "Youtube", "title": "Fay - Slow (Official Video)", "channel": "Fay", "duration": 20, "channel_is_verified": true}]}' ;;
*) sleep 60 ;;
esac
FAKE
chmod +x "$WORK/fake-slow"
SLOW="$WORK/fake-slow"
if [[ -n "$EXE" ]]; then
    printf '@"%s" "%%~dp0fake-slow" %%*\r\n' "$(cygpath -w "$(command -v sh)")" > "$WORK/fake-slow.cmd"
    SLOW="$WORK/fake-slow.cmd"
fi
START=$SECONDS
OUT=$(MVPLAYER_STALL_SECS=1 MVPLAYER_YTDLP="$SLOW" timeout 100 "$BUILD/mvplayer-import" --music-dir "$WORK/lib7" --mv-dir "$WORK/mvlib7" --jobs 1 2>&1)
echo "$OUT" | grep -E "^\[import\]|^done:" | cut -c1-160
[[ "$OUT" == *"too slow"* && $((SECONDS - START)) -lt 40 ]]

echo "== searches work, videos are refused"
# YouTube's bot check applies to video pages, not to searches. A search that
# works must not count as the end of the pause: the waits have to grow.
mkdir -p "$WORK/mvlib8"
echo 0 > "$WORK/refused"
cat > "$WORK/fake-refuse" <<FAKE
#!/bin/sh
case " \$* " in
*" -J "*) echo '{"entries": [{"id": "ref", "ie_key": "Youtube", "title": "Fay - Slow (Official Video)", "channel": "Fay", "duration": 20, "channel_is_verified": true}]}' ;;
*)  n=\$(cat "$WORK/refused"); echo \$((n + 1)) > "$WORK/refused"
    if [ "\$n" -lt 3 ]; then echo "ERROR: [youtube] ref: Sign in to confirm you’re not a bot" >&2; else echo "ERROR: [youtube] ref: Video unavailable" >&2; fi
    exit 1 ;;
esac
FAKE
chmod +x "$WORK/fake-refuse"
REFUSE="$WORK/fake-refuse"
if [[ -n "$EXE" ]]; then
    printf '@"%s" "%%~dp0fake-refuse" %%*\r\n' "$(cygpath -w "$(command -v sh)")" > "$WORK/fake-refuse.cmd"
    REFUSE="$WORK/fake-refuse.cmd"
fi
OUT=$(MVPLAYER_PAUSE_SECS=1 MVPLAYER_YTDLP="$REFUSE" timeout 100 "$BUILD/mvplayer-import" --music-dir "$WORK/lib7" --mv-dir "$WORK/mvlib8" --jobs 1 2>&1)
echo "$OUT" | grep -E "^\[import\] paused|^done:" | cut -c1-70
[[ "$OUT" == *"paused for 0:01"* && "$OUT" == *"paused for 0:02"* && "$OUT" == *"paused for 0:04"* ]]

echo "== one track, one video; review by video"
# "Ten" has an upload waiting for review. "Tenth" is the same song with
# something else over it for one second in four: the song by its
# fingerprints, another recording by its waveform (another language's
# version, a live take). Whose video it is is for the user to say: "Tenth"
# becomes one more candidate of that video, and its audio is put in the file
# beside that of "Ten".
mkdir -p "$WORK/lib17/Jay" "$WORK/lib18/Jay" "$WORK/mvlib17"
ffmpeg -v error -y -f lavfi -i "$(song 430 33 | sed 's/d=40/d=50/')" -metadata title="Ten" -metadata artist=Jay "$WORK/lib17/Jay/ten.flac"
ffmpeg -v error -y -i "$WORK/lib17/Jay/ten.flac" -f lavfi -i "$(song 560 29 | sed 's/d=40/d=55/')" -filter_complex \
    "[1]volume='if(lt(mod(t\,4)\,1)\,6\,0)':eval=frame[g];[0]apad=whole_dur=55[p];[p][g]amix=inputs=2:normalize=0:duration=longest,volume=0.25" \
    -metadata title="Tenth" -metadata artist=Jay "$WORK/lib18/Jay/tenth.flac"
touch -d "10 seconds ago" "$WORK/lib17/Jay/ten.flac" "$WORK/lib18/Jay/tenth.flac"
ffmpeg -v error -y -i "$WORK/lib17/Jay/ten.flac" -af atempo=1.004 -c:a libopus -b:a 96k "$WORK/site-audio.opus"
ffmpeg -v error -y -i "$WORK/mv.mkv" -map 0:v:0 -c copy -t 50 "$WORK/site-video.mkv"
echo '{"entries": [{"id": "ten", "ie_key": "Youtube", "title": "Jay - Ten (Official Video)", "channel": "Jay", "duration": 50, "view_count": 1000, "channel_is_verified": true}]}' > "$WORK/site-search.json"
one() { local mv=$1; shift; MVPLAYER_YTDLP="$SITE" timeout 120 "$BUILD/mvplayer-import" --allow-still-images --mv-dir "$WORK/$mv" --jobs 1 --music-dir "$WORK/lib17" "$@" 2>&1 | grep -E "^\[import\]|^\[review\]|^\[db\]|^done:" | cut -c1-170; }
sql() { python3 -c "import sqlite3, sys; db = sqlite3.connect(sys.argv[1]); db.executescript(sys.argv[2]); db.commit()" "$WORK/$1/.mvplayer/library.db" "$2"; }
ask() { python3 -c "import sqlite3, sys; print(sqlite3.connect(sys.argv[1]).execute(sys.argv[2]).fetchall())" "$WORK/$1/.mvplayer/library.db" "$2"; }
STATES="SELECT t.title, t.state, (SELECT v.yt_id FROM videos v WHERE v.id = t.video_id) FROM tracks t ORDER BY t.title"
CANDS="SELECT v.yt_id, t.title, c.stream, c.fit > 0 FROM candidates c JOIN videos v ON v.id = c.video_id JOIN tracks t ON t.recording = c.track_key ORDER BY v.yt_id, c.rowid"
codecs() { (cd "$WORK/$1/Jay" && ffprobe -v error -show_entries stream=codec_name -of csv=p=0 "$2" | tr -d '\r' | tr '\n' ' '); }
one mvlib17
[[ "$(ask mvlib17 "$STATES")" == "[('Ten', 'review', None)]" && "$(ask mvlib17 "$CANDS")" == "[('ten', 'Ten', 1, 1)]" ]]
# (As it stands now: the start of a library from before, further down.)
cp -r "$WORK/mvlib17" "$WORK/mvlib24"
one mvlib17 --music-dir "$WORK/lib18"
[[ "$(ask mvlib17 "$STATES")" == "[('Ten', 'review', None), ('Tenth', 'review', None)]" ]]
[[ "$(ask mvlib17 "$CANDS")" == "[('ten', 'Ten', 1, 1), ('ten', 'Tenth', 2, 1)]" ]]
[[ "$(codecs mvlib17 "Ten [ten].mkv")" == "h264 flac flac opus " && "$(ask mvlib17 "SELECT review, yt_stream FROM videos")" == "[(1, 2)]" ]]
grep -q '"result":"candidate"' "$WORK/mvlib17/.mvplayer/import-log.jsonl"
# Each candidate has a stream of its own: the first is that of "Ten", the second that of "Tenth".
sound() { (cd "$WORK/$1/Jay" && ffmpeg -v error -i "Ten [ten].mkv" -map "0:a:$2" -f md5 - | tr -d '\r'); }
FIRST=$(sound mvlib17 0); SECOND=$(sound mvlib17 1)
[[ -n "$FIRST" && -n "$SECOND" && "$FIRST" != "$SECOND" ]]
# Looked up again, "Tenth" stays the candidate it is: nothing is added twice.
one mvlib17 --music-dir "$WORK/lib18" --reimport "$WORK/lib18"
[[ "$(ask mvlib17 "$CANDS")" == "[('ten', 'Ten', 1, 1), ('ten', 'Tenth', 2, 1)]" && "$(codecs mvlib17 "Ten [ten].mkv")" == "h264 flac flac opus " ]]
# The same with both turned down (a copy): the upload is deleted and is not
# offered to either of them again.
cp -r "$WORK/mvlib17" "$WORK/mvlib23"
one mvlib23 --music-dir "$WORK/lib18" --reject ten
[[ "$(ask mvlib23 "$STATES")" == "[('Ten', 'not_found', None), ('Tenth', 'not_found', None)]" ]]
[[ "$(ask mvlib23 "SELECT COUNT(*) FROM videos")" == "[(0,)]" && "$(ask mvlib23 "SELECT COUNT(*) FROM rejected_videos")" == "[(2,)]" ]]
[[ ! -e "$WORK/mvlib23/Jay/Ten [ten].mkv" ]]
# Accepted, it is the video of one track: the one named here, "Tenth". Its
# file keeps that track's audio and YouTube's; "Ten" was a candidate of
# nothing else, and has no video.
one mvlib17 --music-dir "$WORK/lib18" --approve "ten=$WORK/lib18/Jay/tenth.flac"
[[ "$(ask mvlib17 "$STATES")" == "[('Ten', 'not_found', None), ('Tenth', 'done', 'ten')]" ]]
[[ "$(ask mvlib17 "SELECT title, review, audio_source, yt_stream FROM videos")" == "[('Tenth', 0, 'library', 1)]" && "$(ask mvlib17 "SELECT COUNT(*) FROM candidates")" == "[(0,)]" ]]
[[ "$(codecs mvlib17 "Ten [ten].mkv")" == "h264 flac opus " ]]
[[ "$(sound mvlib17 0)" == "$SECOND" ]]
[[ "$(ask mvlib17 "SELECT message FROM tracks WHERE title = 'Ten'")" == *"you accepted “Jay - Ten (Official Video)” for “Tenth”"* ]]

# A library from before review went by video: "Ten" and "Tenth" both wait on
# one group of options, each pointing at the video; the file has the audio of
# the track it was made for. Opened by this version, the video has the two
# of them as candidates, and "Tenth" is given its audio in the file.
sql mvlib24 "ALTER TABLE videos ADD COLUMN review_group INTEGER; UPDATE videos SET review_group = id;
    DROP TABLE candidates; ALTER TABLE videos DROP COLUMN yt_stream; ALTER TABLE videos DROP COLUMN vouched;
    DROP TABLE channel_lists; DROP INDEX tracks_lookup; ALTER TABLE tracks DROP COLUMN lookup; DROP TABLE lookup_albums; PRAGMA user_version = 4;
    UPDATE tracks SET state = 'done', video_id = (SELECT id FROM videos)"
python3 - "$WORK/mvlib24/.mvplayer/library.db" "$WORK/lib18/Jay/tenth.flac" <<'PY'
import sqlite3, sys
db = sqlite3.connect(sys.argv[1])
cols = [c[1] for c in db.execute("PRAGMA table_info(tracks)")]
row = list(db.execute("SELECT * FROM tracks").fetchone())
row[cols.index("id")] = None; row[cols.index("path")] = sys.argv[2]; row[cols.index("title")] = "Tenth"
row[cols.index("recording")] = 99; row[cols.index("fingerprint")] = None; row[cols.index("fp_size")] = None
db.execute("INSERT INTO tracks VALUES (%s)" % ",".join("?" * len(cols)), row)
db.commit()
PY
OUT=$(one mvlib24 --music-dir "$WORK/lib18"); echo "$OUT"
[[ "$OUT" == *"library brought up from layout 4"* ]]
[[ "$OUT" == *"library brought up from layout 5"* ]]
[[ "$(ask mvlib24 "PRAGMA user_version")" == "[(8,)]" && "$(ask mvlib24 "$STATES")" == "[('Ten', 'review', None), ('Tenth', 'review', None)]" ]]
[[ "$(ask mvlib24 "SELECT v.yt_id, t.title, c.stream, c.fit > 0 FROM candidates c JOIN videos v ON v.id = c.video_id JOIN tracks t ON t.recording = c.track_key ORDER BY t.title")" == "[('ten', 'Ten', 1, 1), ('ten', 'Tenth', 2, 1)]" ]]
[[ "$(codecs mvlib24 "Ten [ten].mkv")" == "h264 flac flac opus " ]]

# A track can be a candidate of several videos: two uploads that could be
# "Ten". Accepting one makes it one to one, and the other, left with no
# candidate, goes.
mkdir -p "$WORK/mvlib25"
echo '{"entries": [{"id": "tenA", "ie_key": "Youtube", "title": "Jay - Ten (Official Video)", "channel": "Jay", "duration": 50, "view_count": 1000, "channel_is_verified": true},
  {"id": "tenB", "ie_key": "Youtube", "title": "Jay - Ten (Lyric Video)", "channel": "Jay", "duration": 50, "view_count": 900, "channel_is_verified": true}]}' > "$WORK/site-search.json"
OUT=$(one mvlib25); echo "$OUT"
[[ "$OUT" == *"for your review (2 videos)"* && "$(ask mvlib25 "$CANDS")" == "[('tenA', 'Ten', 1, 1), ('tenB', 'Ten', 1, 1)]" ]]
one mvlib25 --approve tenB
[[ "$(ask mvlib25 "$STATES")" == "[('Ten', 'done', 'tenB')]" && "$(ask mvlib25 "SELECT yt_id, review FROM videos")" == "[('tenB', 0)]" ]]
[[ ! -e "$WORK/mvlib25/Jay/Ten [tenA].mkv" && -s "$WORK/mvlib25/Jay/Ten [tenB].mkv" ]]
grep -q '"event":"withdrawn"' "$WORK/mvlib25/.mvplayer/import-log.jsonl"

# The same song under two titles (the original and its English version, as
# it were): the upload is titled "Twelve", and "Dozen", looked up first, is
# its candidate. When "Twelve" is looked up it is one more; and of the two
# it is the one the upload is titled after, so the one shown first, and the
# one a verdict without a name is for.
mkdir -p "$WORK/lib19/Kay" "$WORK/lib20/Kay" "$WORK/mvlib19"
ffmpeg -v error -y -f lavfi -i "$(song 470 37 | sed 's/d=40/d=50/')" -metadata title="Dozen" -metadata artist=Kay "$WORK/lib19/Kay/dozen.flac"
ffmpeg -v error -y -i "$WORK/lib19/Kay/dozen.flac" -f lavfi -i "$(song 610 23 | sed 's/d=40/d=55/')" -filter_complex \
    "[1]volume='if(lt(mod(t\,4)\,1)\,6\,0)':eval=frame[g];[0]apad=whole_dur=55[p];[p][g]amix=inputs=2:normalize=0:duration=longest,volume=0.25" \
    -metadata title="Twelve" -metadata artist=Kay "$WORK/lib20/Kay/twelve.flac"
touch -d "10 seconds ago" "$WORK/lib19/Kay/dozen.flac" "$WORK/lib20/Kay/twelve.flac"
ffmpeg -v error -y -i "$WORK/lib19/Kay/dozen.flac" -af atempo=1.004 -c:a libopus -b:a 96k "$WORK/site-audio.opus"
ffmpeg -v error -y -i "$WORK/mv.mkv" -map 0:v:0 -c copy -t 50 "$WORK/site-video.mkv"
echo '{"entries": [{"id": "twelve", "ie_key": "Youtube", "title": "Kay - Twelve (Official Video)", "channel": "Kay", "duration": 50, "view_count": 1000, "channel_is_verified": true}]}' > "$WORK/site-search.json"
titled() { MVPLAYER_YTDLP="$SITE" timeout 120 "$BUILD/mvplayer-import" --allow-still-images --mv-dir "$WORK/mvlib19" --jobs 1 --music-dir "$WORK/lib19" "$@" 2>&1 | grep -E "^\[import\]|^\[review\]|^done:" | cut -c1-150; }
titled
titled --music-dir "$WORK/lib20"
[[ "$(ask mvlib19 "$CANDS")" == "[('twelve', 'Dozen', 1, 1), ('twelve', 'Twelve', 2, 1)]" ]]
titled --music-dir "$WORK/lib20" --approve twelve
[[ "$(ask mvlib19 "$STATES")" == "[('Dozen', 'not_found', None), ('Twelve', 'done', 'twelve')]" && "$(ask mvlib19 "SELECT title FROM videos")" == "[('Twelve',)]" ]]

# A library from before a video had one track: "Six" and "Sixth" both have
# the video of "Six". Opened by this version, their tracks are looked up
# again; the closest fit to the upload keeps it, and the other has none.
sql mvlib9 "UPDATE tracks SET state = 'done', video_id = (SELECT id FROM videos), message = '' WHERE title = 'Sixth'"
OUT=$(MVPLAYER_YTDLP="$SITE" timeout 120 "$BUILD/mvplayer-import" --allow-still-images --music-dir "$WORK/lib8" --music-dir "$WORK/lib9" --mv-dir "$WORK/mvlib9" --jobs 1 2>&1)
echo "$OUT" | grep -E "^\[import\]|^done:" | cut -c1-170
[[ "$OUT" == *"is the video of “Six” [] alone now, the closest fit to it"* ]]
[[ "$(ask mvlib9 "$STATES")" == "[('Six', 'done', 'own'), ('Sixth', 'not_found', None)]" ]]
grep -q '"event":"shared-settled"' "$WORK/mvlib9/.mvplayer/import-log.jsonl"

echo "== small channels"
# Somebody's re-upload of a song comes from a small channel: an upload from
# a channel with fewer subscribers than the limit (5,000 unless another is
# given) is dropped once its page has come with its audio, and nothing more
# of it is fetched. Not where the channel is verified, nor where YouTube
# lists the upload under several channels ("A and B"): the page then gives
# the first one's count in words, "7.43 million", which reads as 743.
cat > "$WORK/fake-chan" <<FAKE
#!/bin/sh
echo "\$*" >> "$WORK/site-args"
case " \$* " in
*ytsearch*) cat "$WORK/site-search.json" ;;
*" -J "*) if [ -f "$WORK/site-page.json" ]; then cat "$WORK/site-page.json"; else echo "ERROR: [youtube] x: Video unavailable" >&2; exit 1; fi ;;
*)  out=; prev=
    for a in "\$@"; do [ "\$prev" = "-o" ] && out=\$a; prev=\$a; done
    dir=\$(dirname "\$out")
    case "\$out" in
    *audio.*) cp "$WORK/site-audio.opus" "\$dir/audio.opus"; cp "$WORK/site-info.json" "\$dir/audio.info.json" ;;
    *video.*) cp "$WORK/site-video.mkv" "\$dir/video.mkv" ;;
    esac ;;
esac
FAKE
chmod +x "$WORK/fake-chan"
CHAN="$WORK/fake-chan"
if [[ -n "$EXE" ]]; then
    printf '@"%s" "%%~dp0fake-chan" %%*\r\n' "$(cygpath -w "$(command -v sh)")" > "$WORK/fake-chan.cmd"
    CHAN="$WORK/fake-chan.cmd"
fi
chan() { local mv=$1; shift; rm -f "$WORK/site-args"; MVPLAYER_YTDLP="$CHAN" timeout 120 "$BUILD/mvplayer-import" --allow-still-images --mv-dir "$WORK/$mv" --jobs 1 "$@" 2>&1 | grep -E "^\[import\]|^\[review\]|^\[channel\]|^\[db\]|^done:" | cut -c1-200; }
ffmpeg -v error -y -i "$WORK/lib17/Jay/ten.flac" -c:a libopus -b:a 96k "$WORK/site-audio.opus"
ffmpeg -v error -y -i "$WORK/mv.mkv" -map 0:v:0 -c copy -t 50 "$WORK/site-video.mkv"
echo '{"entries": [{"id": "reup", "ie_key": "Youtube", "title": "Jay - Ten (Official Video)", "channel": "Reup Music", "duration": 50, "view_count": 1000}]}' > "$WORK/site-search.json"
echo '{"abr": 96, "acodec": "opus", "channel": "Reup Music", "channel_id": "UCreup", "channel_follower_count": 120}' > "$WORK/site-info.json"
OUT=$(chan mvlib26 --music-dir "$WORK/lib17"); echo "$OUT"
[[ "$OUT" == *"is from a small channel (120 subscribers)"* && "$OUT" == *"done: 0 videos; tracks: not_found=1"* ]]
grep -q '"result":"small-channel"' "$WORK/mvlib26/.mvplayer/import-log.jsonl"
# The search, and its audio for the page that comes with it: no picture, no video.
[[ $(wc -l < "$WORK/site-args") -eq 2 ]]
# With a lower limit it is examined like any upload, and the library keeps
# what the page said of its channel.
OUT=$(chan mvlib27 --music-dir "$WORK/lib17" --min-subscribers 100); echo "$OUT"
[[ "$OUT" == *"for your review (1 video)"* && "$(ask mvlib27 "SELECT yt_channel_id, yt_channel_subs, yt_channel_verified FROM videos")" == "[('UCreup', 120, 0)]" ]]
# A verified channel is not dropped, whatever its size.
echo '{"abr": 96, "acodec": "opus", "channel": "Reup Music", "channel_id": "UCreup", "channel_follower_count": 120, "channel_is_verified": true}' > "$WORK/site-info.json"
OUT=$(chan mvlib28 --music-dir "$WORK/lib17"); echo "$OUT"
[[ "$OUT" == *"for your review (1 video)"* && "$(ask mvlib28 "SELECT yt_channel_subs, yt_channel_verified FROM videos")" == "[(120, 1)]" ]]
# Nor an upload listed under several channels: there is no count to go by.
echo '{"entries": [{"id": "duo", "ie_key": "Youtube", "title": "Jay - Ten (Official Video)", "channel": "Jay and Echo", "duration": 50, "view_count": 1000}]}' > "$WORK/site-search.json"
echo '{"abr": 96, "acodec": "opus", "channel": "Jay", "channel_id": "UCjay", "channel_follower_count": 743, "creators": ["Jay", "Echo"]}' > "$WORK/site-info.json"
OUT=$(chan mvlib29 --music-dir "$WORK/lib17"); echo "$OUT"
[[ "$OUT" == *"done: "*"tracks: "* && "$OUT" != *"small channel"* && "$(ask mvlib29 "SELECT yt_channel, yt_channel_subs FROM videos")" == "[('Jay and Echo', -1)]" ]]
# A video that waits for review from before the rule (here: from under a
# lower limit): its track is no longer a candidate of it once the track is
# re-imported, at once and by the count the library kept; and looked up
# again, the upload is dropped like any from a small channel.
echo '{"entries": [{"id": "reup", "ie_key": "Youtube", "title": "Jay - Ten (Official Video)", "channel": "Reup Music", "duration": 50, "view_count": 1000}]}' > "$WORK/site-search.json"
echo '{"abr": 96, "acodec": "opus", "channel": "Reup Music", "channel_id": "UCreup", "channel_follower_count": 120}' > "$WORK/site-info.json"
OUT=$(chan mvlib27 --music-dir "$WORK/lib17" --reimport "$WORK/lib17"); echo "$OUT"
[[ "$OUT" == *"no longer a candidate of “Jay - Ten (Official Video)”, which waits for review: its channel has 120 subscribers, under the limit of 5000"* ]]
[[ "$(ask mvlib27 "$STATES")" == "[('Ten', 'not_found', None)]" && "$(ask mvlib27 "SELECT COUNT(*) FROM videos")" == "[(0,)]" ]]
rm -f "$WORK/site-page.json"

echo "== channels you allow"
# "Reup Music" has 120 subscribers and is not Jay's by its name: under a
# lower limit its upload comes in for review, on the one question of whose
# channel it is. Allowed, the channel is an artist's channel for every
# track and is never ignored as a small one: the video is accepted at once,
# from its file, with nothing asked of YouTube. The channel is named by its
# handle (which the library kept with the video), by the address of its
# page, or by its id.
echo '{"abr": 96, "acodec": "opus", "channel": "Reup Music", "channel_id": "UCreupreupreupreupreup-_", "uploader_id": "@ReupMusic", "channel_follower_count": 120}' > "$WORK/site-info.json"
OUT=$(chan mvlib33 --music-dir "$WORK/lib17" --min-subscribers 100); echo "$OUT"
[[ "$OUT" == *"for your review (1 video)"* && "$(ask mvlib33 "SELECT yt_channel_id, yt_channel_handle, yt_channel_subs FROM videos")" == "[('UCreupreupreupreupreup-_', '@ReupMusic', 120)]" ]]
for n in 34 35 44 45 46; do cp -r "$WORK/mvlib33" "$WORK/mvlib$n"; done
OUT=$(chan mvlib33 --music-dir "$WORK/lib17" --allow-channel @reupmusic); echo "$OUT"
[[ "$OUT" == *"[channel] allowed: “Reup Music”"* && "$OUT" == *"is accepted: its channel is one you allowed"* ]]
[[ "$(ask mvlib33 "$STATES")" == "[('Ten', 'done', 'reup')]" && "$(ask mvlib33 "SELECT review, vouched FROM videos")" == "[(0, 0)]" ]]
[[ "$(ask mvlib33 "SELECT channel_id, list, name, handle FROM channel_lists")" == "[('UCreupreupreupreupreup-_', 'allow', 'Reup Music', '@ReupMusic')]" ]]
[[ ! -e "$WORK/site-args" ]]
OUT=$(chan mvlib34 --music-dir "$WORK/lib17" --allow-channel "https://www.youtube.com/@ReupMusic/videos"); echo "$OUT"
[[ "$(ask mvlib34 "$STATES")" == "[('Ten', 'done', 'reup')]" ]]
OUT=$(chan mvlib35 --music-dir "$WORK/lib17" --allow-channel "youtube.com/channel/UCreupreupreupreupreup-_"); echo "$OUT"
[[ "$(ask mvlib35 "$STATES")" == "[('Ten', 'done', 'reup')]" ]]
# A name will not do (two channels can have the same one), nor a handle the
# library has no video of: that does not say which channel it is.
OUT=$(MVPLAYER_YTDLP="$CHAN" timeout 120 "$BUILD/mvplayer-import" --allow-still-images --mv-dir "$WORK/mvlib35" --jobs 1 --music-dir "$WORK/lib17" --allow-channel "Reup Music" --allow-channel @nobodyknown 2>&1); echo "$OUT" | grep "channel" | cut -c1-200
[[ "$OUT" == *"not a channel's id (UC…), handle (@name) or address: Reup Music"* && "$OUT" == *"the library has no video of the channel @nobodyknown"* ]]
[[ "$(ask mvlib35 "SELECT COUNT(*) FROM channel_lists")" == "[(1,)]" ]]
# At a re-import the video is kept, small as its channel is: the channel is
# allowed. Taken off the list, it is a small channel like any other, and
# nobody's: the video goes.
OUT=$(chan mvlib33 --music-dir "$WORK/lib17" --reimport "$WORK/lib17"); echo "$OUT"
[[ "$OUT" == *"Re-imported (Jay - Ten (Official Video) · kept)"* && "$(ask mvlib33 "$STATES")" == "[('Ten', 'done', 'reup')]" ]]
OUT=$(chan mvlib33 --music-dir "$WORK/lib17" --unlist-channel UCreupreupreupreupreup-_ --reimport "$WORK/lib17"); echo "$OUT"
[[ "$OUT" == *"its channel has 120 subscribers, under the limit of 5000"* && "$(ask mvlib33 "$STATES")" == "[('Ten', 'not_found', None)]" ]]
[[ "$(ask mvlib33 "SELECT COUNT(*) FROM channel_lists")" == "[(0,)]" && "$(ask mvlib33 "SELECT COUNT(*) FROM videos")" == "[(0,)]" ]]
# Allowed before anything of it is in the library (by its id, then): its
# upload is not dropped as a small channel's, and is taken outright.
OUT=$(chan mvlib36 --music-dir "$WORK/lib17" --allow-channel UCreupreupreupreupreup-_); echo "$OUT"
[[ "$OUT" == *"done: 1 videos; tracks: done=1"* && "$(ask mvlib36 "SELECT review, vouched FROM videos")" == "[(0, 0)]" ]]
# The channel goes by its own name, whatever its uploads are listed under:
# one that YouTube lists under several channels carries "Name and Other".
sql mvlib36 "UPDATE videos SET yt_channel = 'Reup Music and Echo';
    INSERT INTO videos (yt_id, path, title, review, yt_channel, yt_channel_id) VALUES ('reup2', 'Jay/none.mkv', 'x', 0, 'Reup Music and 2 more', 'UCreupreupreupreupreup-_')"
OUT=$(chan mvlib36 --music-dir "$WORK/lib17" --allow-channel UCreupreupreupreupreup-_); echo "$OUT"
[[ "$OUT" == *"[channel] allowed: “Reup Music” [UCreupreupreupreupreup-_]"* ]]
# Whose channel it is was only one of the questions. Here the upload's audio
# runs a little faster than the track: too little of it is the same
# waveform, and the video stays under review, allowed channel or not.
ffmpeg -v error -y -i "$WORK/lib17/Jay/ten.flac" -af atempo=1.004 -c:a libopus -b:a 96k "$WORK/site-audio.opus"
OUT=$(chan mvlib37 --music-dir "$WORK/lib17" --min-subscribers 100); echo "$OUT"
[[ "$OUT" == *"for your review (1 video)"* ]]
OUT=$(chan mvlib37 --music-dir "$WORK/lib17" --allow-channel @reupmusic); echo "$OUT"
[[ "$OUT" == *"stays under review: "*"waveform"* && "$(ask mvlib37 "$STATES")" == "[('Ten', 'review', None)]" && "$(ask mvlib37 "SELECT review FROM videos")" == "[(1,)]" ]]
grep -q '"event":"channel-kept-in-review"' "$WORK/mvlib37/.mvplayer/import-log.jsonl"
ffmpeg -v error -y -i "$WORK/lib17/Jay/ten.flac" -c:a libopus -b:a 96k "$WORK/site-audio.opus"

echo "== channels you deny"
# Denied, every video the library has from the channel goes. One under
# review is rejected for all its candidates, and is not offered to them
# again.
OUT=$(chan mvlib44 --music-dir "$WORK/lib17" --deny-channel @reupmusic); echo "$OUT"
[[ "$OUT" == *"[channel] denied: “Reup Music”"* && "$OUT" == *"is deleted: its channel is one you denied"* ]]
[[ "$(ask mvlib44 "$STATES")" == "[('Ten', 'not_found', None)]" && "$(ask mvlib44 "SELECT COUNT(*) FROM videos")" == "[(0,)]" && "$(ask mvlib44 "SELECT yt_id FROM rejected_videos")" == "[('reup',)]" ]]
[[ "$(ask mvlib44 "SELECT list FROM channel_lists")" == "[('deny',)]" && ! -e "$WORK/mvlib44/Jay/Ten [reup].mkv" ]]
# One that a track has is deleted, and the track is looked up again, to see
# whether another upload is its. The channel's upload is found again and
# dropped: here on its page, which comes with its audio, the search results
# not saying whose it is.
chan mvlib45 --music-dir "$WORK/lib17" --approve reup
[[ "$(ask mvlib45 "$STATES")" == "[('Ten', 'done', 'reup')]" && "$(ask mvlib45 "SELECT vouched FROM videos")" == "[(1,)]" ]]
cp -r "$WORK/mvlib45" "$WORK/mvlib47"; cp -r "$WORK/mvlib45" "$WORK/mvlib48"
OUT=$(chan mvlib45 --music-dir "$WORK/lib17" --deny-channel UCreupreupreupreupreup-_); echo "$OUT"
[[ "$OUT" == *"“Ten” (Jay - Ten (Official Video)) is deleted: its channel is one you denied"* && "$OUT" == *"is from a channel you denied"* ]]
[[ "$(ask mvlib45 "$STATES")" == "[('Ten', 'not_found', None)]" && "$(ask mvlib45 "SELECT COUNT(*) FROM videos")" == "[(0,)]" && ! -e "$WORK/mvlib45/Jay/Ten [reup].mkv" ]]
grep -q '"event":"denied-video"' "$WORK/mvlib45/.mvplayer/import-log.jsonl"
grep -q '"result":"denied-channel"' "$WORK/mvlib45/.mvplayer/import-log.jsonl"
# Where the search results say whose upload it is, it is dropped on sight:
# searches, and nothing fetched.
echo '{"entries": [{"id": "reup", "ie_key": "Youtube", "title": "Jay - Ten (Official Video)", "channel": "Reup Music", "channel_id": "UCreupreupreupreupreup-_", "duration": 50, "view_count": 1000}]}' > "$WORK/site-search.json"
OUT=$(chan mvlib45 --music-dir "$WORK/lib17" --retry); echo "$OUT"
[[ "$OUT" == *"done: 0 videos; tracks: not_found=1"* ]] && ! grep -qv ytsearch "$WORK/site-args"
python3 - "$WORK/mvlib45/.mvplayer/import-log.jsonl" <<'PY'
import json, sys
look = [json.loads(l) for l in open(sys.argv[1], encoding="utf-8") if '"event":"lookup"' in l][-1]
assert [(c["id"], c.get("rejected")) for c in look["candidates"]] == [("reup", "from a channel you denied")], look["candidates"]
PY
# A video from before channel ids were kept is not told when the channel is
# denied: it goes when a re-import reads its page.
old_library() { sql "$1" "UPDATE videos SET yt_channel_id = '', yt_channel_subs = NULL, yt_channel_verified = 0, yt_channel_handle = NULL"; rm -rf "$WORK/$1/.mvplayer/cache"; }
old_library mvlib47
echo '{"entries": [{"id": "reup", "ie_key": "Youtube", "title": "Jay - Ten (Official Video)", "channel": "Reup Music", "duration": 50, "view_count": 1000}]}' > "$WORK/site-search.json"
echo '{"id": "reup", "title": "Jay - Ten (Official Video)", "channel": "Reup Music", "channel_id": "UCreupreupreupreupreup-_", "uploader_id": "@ReupMusic", "channel_follower_count": 120000}' > "$WORK/site-page.json"
OUT=$(chan mvlib47 --music-dir "$WORK/lib17" --deny-channel UCreupreupreupreupreup-_); echo "$OUT"
[[ "$(ask mvlib47 "$STATES")" == "[('Ten', 'done', 'reup')]" ]]
OUT=$(chan mvlib47 --music-dir "$WORK/lib17" --reimport "$WORK/lib17"); echo "$OUT"
[[ "$OUT" == *"its video until now (from a channel you denied)"* && "$(ask mvlib47 "$STATES")" == "[('Ten', 'not_found', None)]" && "$(ask mvlib47 "SELECT COUNT(*) FROM videos")" == "[(0,)]" ]]
[[ $(grep -v ytsearch "$WORK/site-args" | grep -c -- " -J ") -eq 1 ]]
rm -f "$WORK/site-page.json"

echo "== a library from before channels were allowed or denied"
# Accepting a video used to vouch for its channel, for the artists of the
# track. Those channels no longer count by that; the videos that are in the
# library on the strength of it keep it, each as an upload you accepted.
sql mvlib48 "ALTER TABLE videos DROP COLUMN vouched; DROP TABLE channel_lists; DROP INDEX tracks_lookup; ALTER TABLE tracks DROP COLUMN lookup; DROP TABLE lookup_albums; PRAGMA user_version = 5;
    INSERT INTO artist_channels (artist, channel_id, channel, source) VALUES ('Jay', 'UCreupreupreupreupreup-_', '', 'approved')"
OUT=$(chan mvlib48 --music-dir "$WORK/lib17"); echo "$OUT"
[[ "$OUT" == *"library brought up from layout 5"* && "$OUT" == *"library brought up from layout 6"* && "$(ask mvlib48 "PRAGMA user_version")" == "[(8,)]" ]]
[[ "$(ask mvlib48 "SELECT vouched FROM videos")" == "[(1,)]" && "$(ask mvlib48 "SELECT COUNT(*) FROM artist_channels WHERE source = 'approved'")" == "[(0,)]" ]]
[[ "$(ask mvlib48 "SELECT COUNT(*) FROM channel_lists")" == "[(0,)]" ]]

echo "== a track's own video at a re-import"
# "Eleven" is tagged with its artist's name in one script, "Elle"; the
# artist's channel goes by it in another. The video came in through review:
# accepting it vouched for that upload as the track's video, and for
# nothing else of its channel.
mkdir -p "$WORK/lib27/Elle" "$WORK/mvlib30"
ffmpeg -v error -y -f lavfi -i "$(song 390 31 | sed 's/d=40/d=50/')" -metadata title="Eleven" -metadata artist=Elle "$WORK/lib27/Elle/eleven.flac"
touch -d "10 seconds ago" "$WORK/lib27/Elle/eleven.flac"
ffmpeg -v error -y -i "$WORK/lib27/Elle/eleven.flac" -c:a libopus -b:a 96k "$WORK/site-audio.opus"
echo '{"entries": [{"id": "elv", "ie_key": "Youtube", "title": "Eleven", "channel": "エル OFFICIAL", "channel_id": "UCelle_elle_elle_elle_01", "duration": 50, "view_count": 1000, "channel_is_verified": true}]}' > "$WORK/site-search.json"
echo '{"abr": 96, "acodec": "opus", "channel": "エル OFFICIAL", "channel_id": "UCelle_elle_elle_elle_01", "channel_follower_count": 250000, "channel_is_verified": true}' > "$WORK/site-info.json"
chan mvlib30 --music-dir "$WORK/lib27"
chan mvlib30 --music-dir "$WORK/lib27" --approve elv
[[ "$(ask mvlib30 "SELECT t.state, v.yt_id, v.review FROM tracks t JOIN videos v ON v.id = t.video_id")" == "[('done', 'elv', 0)]" ]]
[[ "$(ask mvlib30 "SELECT vouched FROM videos")" == "[(1,)]" && "$(ask mvlib30 "SELECT COUNT(*) FROM artist_channels WHERE source = 'approved'")" == "[(0,)]" ]]
# As a library from before a channel's id and subscribers were kept, and on
# a day the search does not return the upload: the video is judged from
# what the library kept about it. "エル OFFICIAL" is not Elle's channel by
# its name, but the upload is one you accepted: it still fits outright, and
# is kept. Its page is read for what was not kept, once.
cp -r "$WORK/mvlib30" "$WORK/mvlib31"; cp -r "$WORK/mvlib30" "$WORK/mvlib32"; cp -r "$WORK/mvlib30" "$WORK/mvlib43"
echo '{"entries": [{"id": "tpc", "ie_key": "Youtube", "title": "Eleven", "channel": "Elle - Topic", "duration": 50, "view_count": 1000}]}' > "$WORK/site-search.json"
echo '{"id": "elv", "title": "Eleven", "channel": "エル OFFICIAL", "channel_id": "UCelle_elle_elle_elle_01", "channel_follower_count": 250000, "channel_is_verified": true}' > "$WORK/site-page.json"
old_library mvlib30
OUT=$(chan mvlib30 --music-dir "$WORK/lib27" --reimport "$WORK/lib27"); echo "$OUT"
[[ "$OUT" == *"Re-imported (Eleven · kept)"* && "$(ask mvlib30 "SELECT review, yt_channel_id, yt_channel_subs, yt_channel_verified FROM videos")" == "[(0, 'UCelle_elle_elle_elle_01', 250000, 1)]" ]]
[[ $(grep -v ytsearch "$WORK/site-args" | grep -c -- " -J ") -eq 1 ]]
grep -q '"fate":"kept"' "$WORK/mvlib30/.mvplayer/import-log.jsonl"
# Asked once: the next re-import has it from the library.
OUT=$(chan mvlib30 --music-dir "$WORK/lib27" --reimport "$WORK/lib27"); echo "$OUT"
[[ "$OUT" == *"Re-imported (Eleven · kept)"* ]] && ! grep -v ytsearch "$WORK/site-args" | grep -q -- " -J "
# Where the upload is gone from YouTube there is no page to read, and the
# video is judged on what is known: that you accepted it, for one. Kept.
rm "$WORK/site-page.json"
old_library mvlib43
OUT=$(chan mvlib43 --music-dir "$WORK/lib27" --reimport "$WORK/lib27"); echo "$OUT"
[[ "$OUT" == *"Re-imported (Eleven · kept)"* && "$(ask mvlib43 "SELECT review, yt_channel_subs, vouched FROM videos")" == "[(0, -1, 1)]" ]]
# A video that nobody accepted, on a channel that is not known as the
# artist's (one that was allowed when the video was taken, say, and is not
# any more): listened to all the same, and since the channel cannot be told
# to be the artist's, it goes back to review rather than out of the library.
sql mvlib31 "UPDATE videos SET vouched = 0"
old_library mvlib31
OUT=$(chan mvlib31 --music-dir "$WORK/lib27" --reimport "$WORK/lib27"); echo "$OUT"
[[ "$OUT" == *"kept, now for review"* && "$(ask mvlib31 "SELECT review, yt_channel_subs FROM videos")" == "[(1, -1)]" ]]
[[ "$(ask mvlib31 "$STATES")" == "[('Eleven', 'review', None)]" && -s "$WORK/mvlib31/Elle/Eleven [elv].mkv" ]]
# And a track's own video from a channel that turns out to be a small one
# goes, like any other today's rules turn down: also one you accepted. (A
# channel you allowed is not ignored as a small one.)
echo '{"id": "elv", "title": "Eleven", "channel": "エル OFFICIAL", "channel_id": "UCelle_elle_elle_elle_01", "channel_follower_count": 120}' > "$WORK/site-page.json"
old_library mvlib32
OUT=$(chan mvlib32 --music-dir "$WORK/lib27" --reimport "$WORK/lib27"); echo "$OUT"
[[ "$OUT" == *"its channel has 120 subscribers, under the limit of 5000"* && "$(ask mvlib32 "SELECT COUNT(*) FROM videos")" == "[(0,)]" ]]
[[ "$(ask mvlib32 "$STATES")" == "[('Eleven', 'not_found', None)]" && ! -e "$WORK/mvlib32/Elle/Eleven [elv].mkv" ]]
rm -f "$WORK/site-page.json"

echo "== a re-import judges every video the track waits on"
# "Eleven" waits for review as a candidate of its video (the library where
# the upload was gone, above). On a re-import that video goes on the
# shortlist whether or not the search returns it, as a track's own video
# does. Its page is read for what the library did not keep: the channel's
# id, by which it is one you allowed. With that it has become the track's
# own recording: accepted, out of review. (Allowing the channel changed
# nothing by itself here: the library did not know the video was its.)
for n in 38 39 40 41; do cp -r "$WORK/mvlib31" "$WORK/mvlib$n"; old_library mvlib$n; done
for n in 38 39 41; do
    OUT=$(chan mvlib$n --music-dir "$WORK/lib27" --allow-channel UCelle_elle_elle_elle_01); echo "$OUT"
    [[ "$(ask mvlib$n "$STATES")" == "[('Eleven', 'review', None)]" ]]
done
echo '{"entries": [{"id": "tpc", "ie_key": "Youtube", "title": "Eleven", "channel": "Elle - Topic", "duration": 50, "view_count": 1000}]}' > "$WORK/site-search.json"
echo '{"id": "elv", "title": "Eleven", "channel": "エル OFFICIAL", "channel_id": "UCelle_elle_elle_elle_01", "uploader_id": "@elle_official", "channel_follower_count": 250000, "channel_is_verified": true}' > "$WORK/site-page.json"
OUT=$(chan mvlib38 --music-dir "$WORK/lib27" --reimport "$WORK/lib27"); echo "$OUT"
[[ "$OUT" == *"(from review)"* && "$(ask mvlib38 "$STATES")" == "[('Eleven', 'done', 'elv')]" ]]
[[ "$(ask mvlib38 "SELECT review, yt_channel_id, yt_channel_subs, yt_channel_handle FROM videos")" == "[(0, 'UCelle_elle_elle_elle_01', 250000, '@elle_official')]" ]]
[[ $(grep -v ytsearch "$WORK/site-args" | grep -c -- " -J ") -eq 1 ]]
# Nor does it take a place among the best four, or need one: four uploads
# on a channel under the artist's name score higher and are listened to
# (none of them the track's recording), and the video the track waits on
# is judged beside them.
ffmpeg -v error -y -i "$WORK/lib17/Jay/ten.flac" -c:a libopus -b:a 96k "$WORK/site-audio.opus"
python3 - "$WORK/site-search.json" <<'PY'
import json, sys
others = [{"id": f"e{n}", "ie_key": "Youtube", "title": "Elle - Eleven (Official Video)", "channel": "Elle", "channel_id": "UCother",
           "duration": 50, "view_count": 1000, "channel_is_verified": True} for n in range(1, 5)]
held = {"id": "elv", "ie_key": "Youtube", "title": "Eleven", "channel": "エル OFFICIAL", "channel_id": "UCelle_elle_elle_elle_01", "duration": 50, "view_count": 10}
json.dump({"entries": others + [held]}, open(sys.argv[1], "w"))
PY
OUT=$(chan mvlib39 --music-dir "$WORK/lib27" --reimport "$WORK/lib27"); echo "$OUT"
[[ "$OUT" == *"(from review)"* && "$(ask mvlib39 "$STATES")" == "[('Eleven', 'done', 'elv')]" ]]
python3 - "$WORK/mvlib39/.mvplayer/import-log.jsonl" <<'PY'
import json, sys
look = [json.loads(l) for l in open(sys.argv[1], encoding="utf-8") if '"event":"lookup"' in l][-1]
listed = [c["id"] for c in look["candidates"] if c["shortlisted"]]
assert listed == ["e1", "e2", "e3", "e4", "elv"], listed
heard = [(c["id"], c["result"] == "match") for c in look["checked"]]
assert heard == [("e1", False), ("e2", False), ("e3", False), ("e4", False), ("elv", True)], heard
PY
# The page shows a small channel: the track is no longer a candidate of the
# video, and the video goes. Unless the channel is one you allowed.
echo '{"entries": [{"id": "tpc", "ie_key": "Youtube", "title": "Eleven", "channel": "Elle - Topic", "duration": 50, "view_count": 1000}]}' > "$WORK/site-search.json"
echo '{"id": "elv", "title": "Eleven", "channel": "エル OFFICIAL", "channel_id": "UCelle_elle_elle_elle_01", "uploader_id": "@elle_official", "channel_follower_count": 120}' > "$WORK/site-page.json"
OUT=$(chan mvlib40 --music-dir "$WORK/lib27" --reimport "$WORK/lib27"); echo "$OUT"
[[ "$OUT" == *"is from a small channel (120 subscribers)"* && "$(ask mvlib40 "$STATES")" == "[('Eleven', 'not_found', None)]" && "$(ask mvlib40 "SELECT COUNT(*) FROM videos")" == "[(0,)]" ]]
OUT=$(chan mvlib41 --music-dir "$WORK/lib27" --reimport "$WORK/lib27"); echo "$OUT"
[[ "$OUT" != *"small channel"* && "$(ask mvlib41 "$STATES")" == "[('Eleven', 'done', 'elv')]" ]]
rm -f "$WORK/site-page.json"
# A track that today's rules skip waits for nothing. "Eleven" runs 50
# seconds, and under the limit as it is outside these tests (1:20) it is an
# interlude: skipped, no longer a candidate of its video, and the video,
# left with none, goes.
cp -r "$WORK/mvlib31" "$WORK/mvlib42"
OUT=$(unset MVPLAYER_MIN_TRACK_SECS; chan mvlib42 --music-dir "$WORK/lib27" --reimport "$WORK/lib27"); echo "$OUT"
[[ "$(ask mvlib42 "SELECT title, state, message FROM tracks")" == "[('Eleven', 'skipped', '50-second track')]" ]]
[[ "$(ask mvlib42 "SELECT COUNT(*) FROM videos")" == "[(0,)]" && "$(ask mvlib42 "SELECT COUNT(*) FROM candidates")" == "[(0,)]" && ! -e "$WORK/mvlib42/Elle/Eleven [elv].mkv" ]]
grep -q '"event":"withdrawn"' "$WORK/mvlib42/.mvplayer/import-log.jsonl"
! grep -q ytsearch "$WORK/site-args" 2>/dev/null

echo "== short tracks, and the TV size"
# A track under 1:20 is not looked up: an interlude, a jingle. A show's
# opening cut of a song runs a minute and a half and is, whatever its title
# calls it — also one an earlier rule had skipped: the next scan takes that
# back.
mkdir -p "$WORK/lib16/Ivy" "$WORK/mvlib16"
ffmpeg -v error -y -f lavfi -i "$(song 240 37 | sed 's/d=40/d=60/')" -metadata title="Interlude" -metadata artist=Ivy "$WORK/lib16/Ivy/interlude.flac"
ffmpeg -v error -y -f lavfi -i "$(song 290 43 | sed 's/d=40/d=90/')" -metadata title="Nine (TV size)" -metadata artist=Ivy "$WORK/lib16/Ivy/nine.flac"
touch -d "10 seconds ago" "$WORK/lib16/Ivy/interlude.flac" "$WORK/lib16/Ivy/nine.flac"
ffmpeg -v error -y -i "$WORK/lib16/Ivy/nine.flac" -c:a libopus -b:a 96k "$WORK/site-audio.opus"
ffmpeg -v error -y -f lavfi -i "testsrc2=s=160x90:r=10:d=90" -c:v libx264 -preset ultrafast "$WORK/site-video.mkv"
echo '{"entries": [{"id": "tvs", "ie_key": "Youtube", "title": "Ivy - Nine (Official Video)", "channel": "Ivy", "duration": 90, "view_count": 1000, "channel_is_verified": true}]}' > "$WORK/site-search.json"
short() { MVPLAYER_YTDLP="$SITE" timeout 120 "$BUILD/mvplayer-import" --allow-still-images --music-dir "$WORK/lib16" --mv-dir "$WORK/mvlib16" --jobs 1 2>&1 | grep -E "^\[import\]|^done:" | cut -c1-150; }
short_states() { python3 -c "import sqlite3, sys; print(sorted(sqlite3.connect(sys.argv[1]).execute('SELECT title, state, message FROM tracks').fetchall()))" "$WORK/mvlib16/.mvplayer/library.db"; }
MVPLAYER_MIN_TRACK_SECS=120 short
[[ "$(short_states)" == "[('Interlude', 'skipped', '60-second track'), ('Nine (TV size)', 'skipped', '90-second track')]" ]]
(unset MVPLAYER_MIN_TRACK_SECS; short)
STATES=$(short_states); echo "$STATES" | cut -c1-150
[[ "$STATES" == "[('Interlude', 'skipped', '60-second track'), ('Nine (TV size)', 'done', "* ]]
grep -q '"what":"unskipped"' "$WORK/mvlib16/.mvplayer/import-log.jsonl"

echo "== signed in past the bot check"
# YouTube refuses this connection's guests every video page ("Sign in to
# confirm you're not a bot") and serves the account. There is nothing to
# solve: the sign-in is what it asks for. Without the user's say the account
# is left out of it and the queue pauses; with it, the refused request is
# made again signed in, and the next ones go to the account straight away.
mkdir -p "$WORK/mvlib13" "$WORK/mvlib14" "$WORK/mvlib15"
ffmpeg -v error -y -i "$WORK/lib9/Gil/six.flac" -c:a libopus -b:a 96k "$WORK/site-audio.opus"
ffmpeg -v error -y -i "$WORK/mv.mkv" -map 0:v:0 -c copy -t 50 "$WORK/site-video.mkv"
echo '{"entries": [{"id": "own", "ie_key": "Youtube", "title": "Gil - Six (Official Video)", "channel": "Gil", "duration": 50, "view_count": 1000, "channel_is_verified": true}]}' > "$WORK/site-search.json"
cat > "$WORK/fake-guard" <<FAKE
#!/bin/sh
echo "\$*" >> "$WORK/guard-args"
refuse() {
    n=\$(cat "$WORK/guarded"); echo \$((n + 1)) > "$WORK/guarded"
    if [ "\$n" -lt 2 ]; then echo "ERROR: [youtube] own: Sign in to confirm you’re not a bot" >&2; else echo "ERROR: [youtube] own: Video unavailable" >&2; fi
    exit 1
}
case " \$* " in
*ytsearch*) ;;
*" --load-info-json "*) echo "ERROR: unable to download video data: HTTP Error 403: Forbidden" >&2; exit 1 ;;
*" --cookies "*) if [ -f "$WORK/guard-expired" ]; then
        echo "WARNING: [youtube] The provided YouTube account cookies are no longer valid." >&2
        refuse
    fi ;;
*)  refuse ;;
esac
exec sh "$WORK/fake-site" "\$@"
FAKE
chmod +x "$WORK/fake-guard"
GUARD="$WORK/fake-guard"
if [[ -n "$EXE" ]]; then
    printf '@"%s" "%%~dp0fake-guard" %%*\r\n' "$(cygpath -w "$(command -v sh)")" > "$WORK/fake-guard.cmd"
    GUARD="$WORK/fake-guard.cmd"
fi
guard() { echo 0 > "$WORK/guarded"; rm -f "$WORK/guard-args"; MVPLAYER_PAUSE_SECS=1 MVPLAYER_YTDLP="$GUARD" timeout 100 "$BUILD/mvplayer-import" --allow-still-images --music-dir "$WORK/lib9" --jobs 1 --cookies "$WORK/cookies.txt" "$@" 2>&1; }
OUT=$(guard --mv-dir "$WORK/mvlib13")
echo "$OUT" | grep -E "^\[import\] paused|^done:" | cut -c1-70
[[ "$OUT" == *"paused for 0:01"* && "$OUT" == *"done: 0 videos"* ]]
! grep -q -e "--cookies" "$WORK/guard-args"
OUT=$(guard --mv-dir "$WORK/mvlib14" --sign-in-on-bot-check)
echo "$OUT" | grep -E "^\[import\] paused|^done:" | cut -c1-70
[[ "$OUT" != *"paused for"* && "$OUT" == *"done: 1 videos; tracks: done=1"* ]]
# One request was refused as a guest; the video's page was asked signed in
# without trying that again.
[[ $(cat "$WORK/guarded") -eq 1 ]]
[[ $(grep -c -e "--cookies" "$WORK/guard-args") -ge 2 ]]
! grep -q -e "--cookies $WORK/cookies.txt" "$WORK/guard-args"
# Cookies the browser has rotated since: yt-dlp carries on as a guest and is
# refused like one. The pause says what is wrong.
touch "$WORK/guard-expired"
OUT=$(guard --mv-dir "$WORK/mvlib15" --sign-in-on-bot-check)
echo "$OUT" | grep -E "^\[import\] paused|^done:" | cut -c1-70
[[ "$OUT" == *"paused for 0:01"* && "$OUT" == *"done: 0 videos"* ]]
grep '"event":"paused"' "$WORK/mvlib15/.mvplayer/import-log.jsonl" | grep -q "the account's cookies have expired"


echo "== judge mode"
# The judge runs the lookup (docs/import-rules-judge.html): a stand-in for
# the Claude API answers the three questions from a file, so that each run
# below can be given another verdict.
JUDGE_PORT=$(python3 -c 'import socket; s = socket.socket(); s.bind(("127.0.0.1", 0)); print(s.getsockname()[1])')
cat > "$WORK/fake-judge.py" <<'PY'
import json, sys, http.server
port, answers = int(sys.argv[1]), sys.argv[2]
class Handler(http.server.BaseHTTPRequestHandler):
    def log_message(self, *a): pass
    def reply(self, body):
        data = json.dumps(body).encode()
        self.send_response(200); self.send_header('content-type', 'application/json'); self.send_header('content-length', str(len(data))); self.end_headers()
        self.wfile.write(data)
    def do_GET(self):
        self.reply({'data': [{'id': 'claude-sonnet-5-5'}], 'models': [{'slug': 'test-model', 'display_name': 'Test model', 'visibility': 'list'}]})
    def do_POST(self):
        body = json.loads(self.rfile.read(int(self.headers.get('content-length', 0))))
        chatgpt = self.path == '/v1/responses'
        if chatgpt:
            assert self.headers.get('Authorization') == 'Bearer smoke-account-token'
            assert body['store'] is False and body['stream'] is True
            assert body['text']['format']['strict'] is True
            content = body['input'][0]['content']
        else:
            content = body['messages'][0]['content']
        words = body.get('instructions' if chatgpt else 'system', '')
        key = 'query' if words.startswith('You help a music-video library find') else 'pick' if words.startswith('You help a music-video library choose') else 'verdict'
        with open(answers + '.asked', 'a') as f:
            f.write(key + ' ' + body['model'] + ' images=' + str(sum(1 for b in content if b.get('type') in ('image', 'input_image')))
                    + ' sound=' + ('yes' if any('Sound:' in b.get('text', '') for b in content) else 'no')
                    + ' desc=' + ('yes' if any('Description:' in b.get('text', '') for b in content) else 'no') + '\n')
        if chatgpt:
            text = json.dumps(json.load(open(answers))[key])
            item = {'type': 'message', 'role': 'assistant', 'status': 'completed',
                    'content': [{'type': 'output_text', 'text': text}]}
            # A streamed message need not be repeated in the terminal event.
            response = {'status': 'completed', 'usage': {'input_tokens': 1000, 'output_tokens': 50},
                        'output': []}
            events = [{'type': 'response.output_item.done', 'output_index': 0, 'item': item},
                      {'type': 'response.completed', 'response': response}]
            data = ''.join('data: ' + json.dumps(event) + '\n\n' for event in events).encode()
            self.send_response(200); self.send_header('content-type', 'text/event-stream'); self.send_header('content-length', str(len(data))); self.end_headers()
            self.wfile.write(data)
            return
        self.reply({'content': [{'type': 'text', 'text': json.dumps(json.load(open(answers))[key])}], 'usage': {'input_tokens': 1000, 'output_tokens': 50},
                    'stop_reason': 'end_turn', 'model': body['model']})
http.server.HTTPServer(('127.0.0.1', port), Handler).serve_forever()
PY
python3 "$WORK/fake-judge.py" "$JUDGE_PORT" "$WORK/judge-answers.json" &
JUDGE_PID=$!
trap 'kill $JUDGE_PID 2>/dev/null; rm -rf "$WORK"' EXIT
python3 - "$JUDGE_PORT" <<'PY'
import sys, time, urllib.request
for i in range(100):
    try:
        urllib.request.urlopen(f'http://127.0.0.1:{sys.argv[1]}/v1/models', timeout=1); break
    except Exception:
        time.sleep(0.1)
PY
echo "sk-ant-test-00000000000000000000" > "$WORK/claude-key"
# The stand-in serves Dee's "Five" again (later tests gave it other songs).
ffmpeg -v error -y -i "$WORK/lib5/Dee/five.flac" -af atempo=1.004 -c:a libopus -b:a 96k "$WORK/site-audio.opus"
ffmpeg -v error -y -i "$WORK/mv.mkv" -map 0:v:0 -c copy -t 40 "$WORK/site-video.mkv"
echo '{"entries": [{"id": "liv", "ie_key": "Youtube", "title": "Dee - Five", "channel": "Dee", "duration": 40, "view_count": 1000, "channel_is_verified": true}]}' > "$WORK/site-search.json"
# The judge is shown frames of the picture, so the look at the picture cannot
# be left out here, and its format selector has a ">" in it, which a batch
# file would take for a redirection. On Windows the stand-in is a program for
# these runs: it starts sh on the script named like itself, with its own
# command line after it as it came.
if [[ -n "$EXE" ]]; then
    SH_EXE=$(cygpath -m "$(command -v sh)"); SH_EXE=${SH_EXE%.exe}.exe
    cat > "$WORK/standin.c" <<C
#include <windows.h>
#include <wchar.h>

static wchar_t self[32768], line[32768];

int main(void)
{
    const wchar_t *sh = L"$SH_EXE";
    const wchar_t *rest = GetCommandLineW();
    STARTUPINFOW si;
    PROCESS_INFORMATION pi;
    DWORD code = 1;
    DWORD n = GetModuleFileNameW(NULL, self, 32768);
    if (n < 5 || n >= 32768)
        return 127;
    self[n - 4] = 0; /* without ".exe": the script */
    if (*rest == L'"') {
        rest = wcschr(rest + 1, L'"');
        rest = rest ? rest + 1 : L"";
    } else {
        while (*rest && *rest != L' ' && *rest != L'\t')
            ++rest;
    }
    if (wcslen(sh) + wcslen(self) + wcslen(rest) + 6 > 32767)
        return 127;
    wcscpy(line, L"\"");
    wcscat(line, sh);
    wcscat(line, L"\" \"");
    wcscat(line, self);
    wcscat(line, L"\"");
    wcscat(line, rest);
    ZeroMemory(&si, sizeof si);
    si.cb = sizeof si;
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdInput = GetStdHandle(STD_INPUT_HANDLE);
    si.hStdOutput = GetStdHandle(STD_OUTPUT_HANDLE);
    si.hStdError = GetStdHandle(STD_ERROR_HANDLE);
    if (!CreateProcessW(NULL, line, NULL, NULL, TRUE, 0, NULL, NULL, &si, &pi))
        return 127;
    WaitForSingleObject(pi.hProcess, INFINITE);
    GetExitCodeProcess(pi.hProcess, &code);
    return (int)code;
}
C
    gcc -O1 -o "$WORK/fake-site.exe" "$WORK/standin.c"
    SITE="$WORK/fake-site.exe"
fi
answers() { python3 - "$WORK/judge-answers.json" "$1" "$2" "$3" <<'PY'
import json, sys
path, verdict, chosen, confidence = sys.argv[1], sys.argv[2], int(sys.argv[3]), float(sys.argv[4])
keep = verdict == 'accept'
json.dump({
    'query': {'lookup': True, 'skip_reason': '', 'queries': ['Dee Five', 'Dee - Five official video'], 'song_known': 'unknown'},
    'pick': {'picks': [{'id': 'liv', 'why': 'the artist\'s own upload'}], 'reason': 'one result, on the artist\'s channel'},
    'verdict': {'options': [{'index': 1, 'kind': 'official music video', 'source': "the artist's channel", 'version': 'the same recording',
                             'whole': 'the whole song', 'official_mv': keep, 'confidence': confidence if keep else 1 - confidence, 'note': 'a moving picture on the artist\'s channel'}],
                'verdict': verdict, 'chosen': chosen, 'confidence': confidence, 'reason': 'the artist\'s own upload of the song' if keep else 'not the official video'},
}, open(path, 'w'))
PY
}
judge() { MVPLAYER_YTDLP="$SITE" MVPLAYER_CLAUDE_API="http://127.0.0.1:$JUDGE_PORT" MVPLAYER_PAUSE_SECS=1 timeout 120 "$BUILD/mvplayer-import" --music-dir "$WORK/lib5" --jobs 1 --judge judge --claude-key-file "$WORK/claude-key" "$@" 2>&1; }
judge_db() { python3 -c "import sqlite3, sys; print(sqlite3.connect(sys.argv[1]).execute(sys.argv[2]).fetchall())" "$WORK/$1/.mvplayer/library.db" "$2"; }

# W1: accepted at the gate, and the upload passes the gates: the track's video, vouched for.
answers accept 1 0.95
rm -f "$WORK/judge-answers.json.asked"
OUT=$(judge --mv-dir "$WORK/mvlib-j1")
echo "$OUT" | grep -E "^\[import\] Dee|^\[judge\]|^done:" | cut -c1-120
[[ "$OUT" == *"Imported"* && "$OUT" == *"the judge: the artist's own upload"* ]]
[[ "$(judge_db mvlib-j1 'SELECT review, vouched, audio_source FROM videos')" == "[(0, 1, "* ]]
# The three questions were asked once each, the verdict with five frames.
cat "$WORK/judge-answers.json.asked"
[[ "$(grep -c . "$WORK/judge-answers.json.asked")" == 3 ]]
grep -q "^verdict claude-sonnet-5-5 images=5" "$WORK/judge-answers.json.asked"
[[ "$(grep -c '"event":"judge"' "$WORK/mvlib-j1/.mvplayer/import-log.jsonl")" == 3 ]]
grep -q '"question":"verdict"' "$WORK/mvlib-j1/.mvplayer/import-log.jsonl"
# The upload's description, as the page gave it, is kept with the video and told to the judge (V1).
grep -q "^verdict claude-sonnet-5-5 images=5 sound=no desc=yes" "$WORK/judge-answers.json.asked"
[[ "$(judge_db mvlib-j1 'SELECT yt_description FROM videos')" == "[('Dee - Five | Out now on every streaming service. | Directed by Dee.',)]" ]]
# ChatGPT uses the same three questions, with images and structured answers,
# and records account usage without applying Claude's prices.
python3 - "$WORK/chatgpt.json" <<'PY'
import json, os, sys, time
with open(sys.argv[1], 'w') as f:
    json.dump({'selected': 'smoke-account', 'accounts': {'smoke-account': {
        'client_id': 'smoke-account', 'subject': 'test', 'access_token': 'smoke-account-token',
        'expires_at': int(time.time()) + 3600, 'scopes': ['resource.invoke', 'chatgpt.tokens.use.direct'],
        'model': 'test-model'}}}, f)
os.chmod(sys.argv[1], 0o600)
PY
rm -f "$WORK/judge-answers.json.asked"
OUT=$(MVPLAYER_YTDLP="$SITE" MVPLAYER_CHATGPT_API="http://127.0.0.1:$JUDGE_PORT/v1" timeout 120 "$BUILD/mvplayer-import" --music-dir "$WORK/lib5" --mv-dir "$WORK/mvlib-chatgpt" --jobs 1 --judge judge --judge-provider chatgpt --chatgpt-auth-file "$WORK/chatgpt.json" 2>&1)
[[ "$OUT" == *"Imported"* && "$OUT" == *"the judge: the artist's own upload"* ]]
[[ "$(judge_db mvlib-chatgpt 'SELECT review, vouched FROM videos')" == "[(0, 1)]" ]]
[[ "$(grep -c . "$WORK/judge-answers.json.asked")" == 3 ]]
grep -q '^verdict test-model images=5 sound=no desc=yes' "$WORK/judge-answers.json.asked"
python3 - "$WORK/mvlib-chatgpt/.mvplayer/import-log.jsonl" <<'PY'
import json, sys
raw = open(sys.argv[1]).read()
rows = [r for r in map(json.loads, raw.splitlines()) if r.get('event') == 'judge']
assert len(rows) == 3 and all(r['provider'] == 'chatgpt' and r['cost'] is None for r in rows)
assert 'smoke-account-token' not in raw
PY
OUT=$(MVPLAYER_CHATGPT_API="http://127.0.0.1:$JUDGE_PORT/v1" "$BUILD/mvplayer-import" --chatgpt-auth-file "$WORK/chatgpt.json" --chatgpt-models --mv-dir "$WORK/account-command-no-library")
[[ "$OUT" == *"test-model"* && ! -e "$WORK/account-command-no-library" ]]

# Another model, by name.
rm -f "$WORK/judge-answers.json.asked"
OUT=$(judge --mv-dir "$WORK/mvlib-j1b" --judge-model opus)
grep -q "^query claude-opus-5-5" "$WORK/judge-answers.json.asked"

# Keeping everything (README): a lookup with --capture keeps the searches it
# made, the video's audio, page, preview and video, and the judge's three
# answers; a replay from the capture, with yt-dlp and the API both out of
# reach, brings the same video in from the capture alone.
OUT=$(judge --mv-dir "$WORK/mvlib-cap" --capture "$WORK/cap"); echo "$OUT" | grep -E "^done:"
[[ "$OUT" == *"done: 1 videos"* ]]
ls "$WORK/cap/search" "$WORK/cap/video/liv" "$WORK/cap/judge" | tr '\n' ' '; echo
[[ $(ls "$WORK/cap/search"/*.json | wc -l) -ge 1 && -s "$WORK/cap/search/index.jsonl" ]]
[[ -f "$WORK/cap/video/liv/audio.opus" && -f "$WORK/cap/video/liv/audio.info.json" && -f "$WORK/cap/video/liv/preview.mkv" && -f "$WORK/cap/video/liv/video.mkv" ]]
[[ $(ls "$WORK/cap/judge"/*.json | wc -l) -eq 3 ]]
cat > "$WORK/fake-none" <<FAKE
#!/bin/sh
echo "\$*" >> "$WORK/none-args"
echo "ERROR: not to be asked" >&2
exit 1
FAKE
chmod +x "$WORK/fake-none"
NONE="$WORK/fake-none"
if [[ -n "$EXE" ]]; then
    printf '@"%s" "%%~dp0fake-none" %%*\r\n' "$(cygpath -w "$(command -v sh)")" > "$WORK/fake-none.cmd"
    NONE="$WORK/fake-none.cmd"
fi
rm -f "$WORK/none-args" "$WORK/judge-answers.json.asked"
OUT=$(MVPLAYER_YTDLP="$NONE" MVPLAYER_CLAUDE_API="http://127.0.0.1:1" MVPLAYER_PAUSE_SECS=1 timeout 120 "$BUILD/mvplayer-import" --music-dir "$WORK/lib5" --mv-dir "$WORK/mvlib-rep" --jobs 1 --judge judge --claude-key-file "$WORK/claude-key" --replay "$WORK/cap" 2>&1); echo "$OUT" | grep -E "^\[import\] Dee|^done:" | cut -c1-120
[[ "$OUT" == *"done: 1 videos"* && ! -e "$WORK/none-args" && ! -e "$WORK/judge-answers.json.asked" ]]
[[ "$(judge_db mvlib-rep 'SELECT yt_id, vouched FROM videos')" == "[('liv', 1)]" ]]

# W2: accepted below the gate: a card under review, with the judge's advice on it.
answers accept 1 0.6
OUT=$(judge --mv-dir "$WORK/mvlib-j2")
echo "$OUT" | grep -E "^\[import\] Dee|^done:" | cut -c1-120
[[ "$OUT" == *"For your review"* && "$OUT" == *"the judge:"* ]]
[[ "$(judge_db mvlib-j2 'SELECT review, vouched FROM videos')" == "[(1, 0)]" ]]
[[ "$(judge_db mvlib-j2 'SELECT mode, keep, probability, kind FROM judge_advice')" == "[('judge', 1, 0.6, 'official music video')]" ]]
# Your verdict on the card stands as on the main page (X1).
OUT=$(judge --mv-dir "$WORK/mvlib-j2" --approve liv)
[[ "$(judge_db mvlib-j2 'SELECT review FROM videos')" == "[(0,)]" ]]

# W3: rejected at the gate: no video, and the upload is remembered as turned down for the track.
answers "reject all" 0 0.9
OUT=$(judge --mv-dir "$WORK/mvlib-j3")
echo "$OUT" | grep -E "^\[import\] Dee|^done:" | cut -c1-120
[[ "$OUT" == *"No music video found (the judge: not the official video)"* ]]
[[ "$(judge_db mvlib-j3 'SELECT count(*) FROM videos')" == "[(0,)]" ]]
[[ "$(judge_db mvlib-j3 'SELECT count(*) FROM rejected_videos')" == "[(1,)]" ]]
[[ "$(judge_db mvlib-j3 "SELECT state FROM tracks")" == "[('not_found',)]" ]]

# W4: unsure: a card under review, since the upload is the song.
answers unsure 0 0.5
OUT=$(judge --mv-dir "$WORK/mvlib-j4")
[[ "$OUT" == *"For your review"* ]]
[[ "$(judge_db mvlib-j4 'SELECT review FROM videos')" == "[(1,)]" ]]

# Q2: the judge says there is nothing to look for.
python3 - "$WORK/judge-answers.json" <<'PY'
import json, sys
a = json.load(open(sys.argv[1])); a['query'] = {'lookup': False, 'skip_reason': 'an instrumental', 'queries': [], 'song_known': 'unknown'}
json.dump(a, open(sys.argv[1], 'w'))
PY
OUT=$(judge --mv-dir "$WORK/mvlib-j5")
echo "$OUT" | grep -E "^\[import\] Dee|^done:" | cut -c1-120
[[ "$OUT" == *"Skipped (the judge: an instrumental)"* ]]

# Q2, Q4, Q5 on a library the rules had scanned: "MC1" (a talk title, and
# sounds like it) is skipped by A4, the other two were searched. Judge mode
# asks the judge once about the rules' skip (Q5), telling it how the track
# sounds (Q2); the judge's own skip then stands (Q4): the next run asks
# nothing. A track without a video is not asked again either (A6).
OUT=$(MVPLAYER_YTDLP="$EMPTY" timeout 120 "$BUILD/mvplayer-import" --music-dir "$WORK/lib6" --mv-dir "$WORK/mvlib-j9" --jobs 1 2>&1)
[[ "$(judge_db mvlib-j9 "SELECT state FROM tracks WHERE title = 'MC1'")" == "[('skipped',)]" ]]
python3 - "$WORK/judge-answers.json" <<'PY'
import json, sys
a = json.load(open(sys.argv[1])); a['query'] = {'lookup': False, 'skip_reason': 'a talk track', 'queries': [], 'song_known': 'unknown'}
json.dump(a, open(sys.argv[1], 'w'))
PY
rm -f "$WORK/judge-answers.json.asked"
OUT=$(MVPLAYER_YTDLP="$EMPTY" MVPLAYER_CLAUDE_API="http://127.0.0.1:$JUDGE_PORT" timeout 120 "$BUILD/mvplayer-import" --music-dir "$WORK/lib6" --mv-dir "$WORK/mvlib-j9" --jobs 1 --judge judge --claude-key-file "$WORK/claude-key" 2>&1)
echo "$OUT" | grep -E "^\[scan\]|^\[import\] Eve|^done:" | cut -c1-120
[[ "$OUT" == *"MC1: Skipped (the judge: a talk track)"* ]]
grep -q '"what":"unskipped"' "$WORK/mvlib-j9/.mvplayer/import-log.jsonl"
cat "$WORK/judge-answers.json.asked"
[[ "$(cat "$WORK/judge-answers.json.asked")" == "query claude-sonnet-5-5 images=0 sound=yes desc=no" ]]
rm -f "$WORK/judge-answers.json.asked"
OUT=$(MVPLAYER_YTDLP="$EMPTY" MVPLAYER_CLAUDE_API="http://127.0.0.1:$JUDGE_PORT" timeout 120 "$BUILD/mvplayer-import" --music-dir "$WORK/lib6" --mv-dir "$WORK/mvlib-j9" --jobs 1 --judge judge --claude-key-file "$WORK/claude-key" 2>&1)
[[ ! -e "$WORK/judge-answers.json.asked" && "$(judge_db mvlib-j9 "SELECT state, message FROM tracks WHERE title = 'MC1'")" == "[('skipped', 'the judge: a talk track')]" ]]
[[ $(grep -c '"what":"unskipped"' "$WORK/mvlib-j9/.mvplayer/import-log.jsonl") -eq 1 ]]
# A song's title can mention talk: the judge is told how it sounds, and no
# "Sound:" line goes with a title that does not look like talk.
rm -f "$WORK/judge-answers.json.asked"
OUT=$(judge --mv-dir "$WORK/mvlib-j10")
[[ "$(cat "$WORK/judge-answers.json.asked")" == "query claude-sonnet-5-5 images=0 sound=no desc=no" ]]

# V5: the judge cannot answer: the lookup is postponed as for a pause, and the pause says why.
OUT=$(MVPLAYER_CLAUDE_API="http://127.0.0.1:1" MVPLAYER_YTDLP="$SITE" MVPLAYER_PAUSE_SECS=1 timeout 60 "$BUILD/mvplayer-import" --music-dir "$WORK/lib5" --mv-dir "$WORK/mvlib-j6" --jobs 1 --judge judge --claude-key-file "$WORK/claude-key" 2>&1 || true)
echo "$OUT" | grep -E "^\[import\] paused|^done:" | cut -c1-110
[[ "$OUT" == *"paused for 0:01: The judge cannot answer"* ]]
# No key: refused up front.
! "$BUILD/mvplayer-import" --music-dir "$WORK/lib5" --mv-dir "$WORK/mvlib-j7" --judge judge --claude-key-file "$WORK/no-such-key" >/dev/null 2>&1
# Off, the rules decide as before: the judge is not asked.
rm -f "$WORK/judge-answers.json.asked"
OUT=$(MVPLAYER_YTDLP="$SITE" MVPLAYER_CLAUDE_API="http://127.0.0.1:$JUDGE_PORT" timeout 120 "$BUILD/mvplayer-import" --allow-still-images --music-dir "$WORK/lib5" --mv-dir "$WORK/mvlib-j8" --jobs 1 2>&1)
[[ ! -e "$WORK/judge-answers.json.asked" ]]

echo "== tags written into a file"
# The tag editor's writer, headless: tags go into the file and read back,
# an empty value takes a tag out, and with --mv-dir the library's row takes
# them at once; a track without a video that has a new title is looked up
# again under it (A10).
mkdir -p "$WORK/libt/T"
ffmpeg -v error -y -f lavfi -i "$(song 247 29)" -metadata title="Before" -metadata artist=Tee -metadata genre=Rock "$WORK/libt/T/t.flac"
OUT=$("$BUILD/mvplayer-import" tags "$WORK/libt/T/t.flac" TITLE="After" ALBUM="An Album" GENRE= 2>/dev/null)
echo "$OUT" | grep -E "^(TITLE|ALBUM|GENRE|ARTIST)="
[[ "$OUT" == *"TITLE=After"* && "$OUT" == *"ALBUM=An Album"* && "$OUT" == *"ARTIST=Tee"* && "$OUT" != *"GENRE="* ]]
! "$BUILD/mvplayer-import" tags "$WORK/libt/T/t.flac" nonsense >/dev/null 2>&1
MVPLAYER_YTDLP="$FAKE" MVPLAYER_PAUSE_SECS=1 timeout 60 "$BUILD/mvplayer-import" --music-dir "$WORK/libt" --mv-dir "$WORK/mvlib-t" >/dev/null 2>&1
[[ "$(python3 -c "import sqlite3, sys; print(sqlite3.connect(sys.argv[1]).execute('SELECT title, state FROM tracks').fetchall())" "$WORK/mvlib-t/.mvplayer/library.db")" == "[('After', 'not_found')]" ]]
OUT=$("$BUILD/mvplayer-import" tags "$WORK/libt/T/t.flac" TITLE="Later" --mv-dir "$WORK/mvlib-t" 2>/dev/null)
echo "$OUT" | grep -E "^library:"
[[ "$OUT" == *"library: updated"* ]]
[[ "$(python3 -c "import sqlite3, sys; print(sqlite3.connect(sys.argv[1]).execute('SELECT title, state FROM tracks').fetchall())" "$WORK/mvlib-t/.mvplayer/library.db")" == "[('Later', 'pending')]" ]]

echo "== an album cover from a file's tags"
# CoverCache reads the picture embedded in a music file (the unit test skips
# this without a file to read; ffmpeg makes one here).
ffmpeg -v error -y -f lavfi -i "color=c=orange:s=640x640:d=1" -frames:v 1 "$WORK/cover.png"
ffmpeg -v error -y -f lavfi -i "$(song 262 31)" -i "$WORK/cover.png" -map 0:a -map 1:v -c:a flac -c:v png -disposition:v:0 attached_pic -metadata title="Art" -metadata artist=Hal "$WORK/art.flac"
MVPLAYER_COVER_FILE="$WORK/art.flac" "$BUILD/mvplayer-covers-test$EXE" embedded -o "$TEST_LOG_ARG,txt" || TEST_RESULT=$?
grep -E "^(PASS|FAIL|SKIP) .*embedded" "$TEST_LOG"
if [[ "$TEST_RESULT" -ne 0 ]]; then cat "$TEST_LOG"; exit "$TEST_RESULT"; fi
grep -q "PASS   : CoverCacheTest::embedded()" "$TEST_LOG"
# The tag editor's cover art: the embedded picture read back, and another
# put in its place (a file without one gets none unless asked).
OUT=$("$BUILD/mvplayer-import" tags "$WORK/art.flac" 2>/dev/null)
[[ "$OUT" == *"PICTURE=image/png"* ]]
ffmpeg -v error -y -f lavfi -i "color=c=blue:s=320x320:d=1" -frames:v 1 "$WORK/cover2.jpg"
OUT=$("$BUILD/mvplayer-import" tags "$WORK/art.flac" PICTURE="$WORK/cover2.jpg" 2>/dev/null)
echo "$OUT" | grep "^PICTURE="
[[ "$OUT" == *"PICTURE=image/jpeg, $(stat -c %s "$WORK/cover2.jpg") bytes"* ]]
[[ "$("$BUILD/mvplayer-import" tags "$WORK/libt/T/t.flac" 2>/dev/null)" != *"PICTURE="* ]]

echo "== binaries start"
"$BUILD/mvplayer-import" --help >/dev/null
QT_QPA_PLATFORM=offscreen "$BUILD/mvplayer$EXE" --help >/dev/null
echo "smoke ok"
