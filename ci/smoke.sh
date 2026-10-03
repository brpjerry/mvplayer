#!/usr/bin/env bash
# End-to-end check of the audio pipeline on synthetic files, no network needed:
# a "track" is embedded in a longer "video soundtrack" with an intro, an
# inserted scene and an outro; the aligner must find both segments at the
# right offsets and the muxer must produce a playable file with both audio
# tracks.
set -euo pipefail

BUILD=${1:-build}
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
for n in one two; do
    ffmpeg -v error -y -f lavfi -i "sine=f=330:d=20" -metadata title="Song $n" -metadata artist=A "$WORK/lib/A/$n.flac"
done
cat > "$WORK/fake-ytdlp" <<FAKE
#!/bin/sh
n=\$(cat "$WORK/calls" 2>/dev/null || echo 0); n=\$((n+1)); echo \$n > "$WORK/calls"
if [ \$n -le 3 ]; then echo "ERROR: [youtube] x: Sign in to confirm you’re not a bot" >&2; exit 1; fi
echo '{"entries": []}'
FAKE
chmod +x "$WORK/fake-ytdlp"
OUT=$(MVPLAYER_YTDLP="$WORK/fake-ytdlp" MVPLAYER_PAUSE_SECS=1 timeout 60 "$BUILD/mvplayer-import" \
    --music-dir "$WORK/lib" --mv-dir "$WORK/mvlib" 2>&1)
echo "$OUT" | grep -E "paused|resuming|^done:"
[[ $(echo "$OUT" | grep -c "paused for 0:01") -eq 1 && $(echo "$OUT" | grep -c "paused for 0:02") -eq 1 ]]
[[ "$OUT" == *"done: 0 videos; tracks: not_found=2"* ]]
grep -q '"outcome":"postponed"' "$WORK/mvlib/.mvplayer/import-log.jsonl"
grep -q '"event":"paused"' "$WORK/mvlib/.mvplayer/import-log.jsonl"

echo "== binaries start"
"$BUILD/mvplayer-import" --help >/dev/null
QT_QPA_PLATFORM=offscreen "$BUILD/mvplayer" --help >/dev/null
echo "smoke ok"
