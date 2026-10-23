# MV Player

A music player for music videos. Point it at an audio library and it builds a
matching video library: it finds each track's music video on YouTube, checks by
ear that it really is that recording, downloads it at the best quality on
offer, and muxes in your own audio where it is better than YouTube's.

The audio library (one or more folders) is only ever read. Videos go to a
single separate folder (`~/Videos/MVs` by default; `Videos\MVs` in your user
folder on Windows).

## Install (Windows)

Download `mvplayer-<version>-setup.exe` from the
[latest release](https://github.com/brpjerry/mvplayer/releases/latest) and run
it. It installs for the current user only, so it needs no administrator
rights. The installer is not code-signed; Windows SmartScreen will ask before
running it.

ffmpeg is included. yt-dlp, and the Deno runtime it uses for some YouTube
formats, are downloaded during setup from their own release pages into
`%LOCALAPPDATA%\mvplayer\tools`. YouTube changes often, so when imports start
failing use **Settings → Library → Update yt-dlp** to fetch the latest version.

## Install (Arch Linux)

Packages are built by CI for every tagged release. Add the release feed to
`/etc/pacman.conf` and install as usual:

```ini
[mvplayer]
SigLevel = Optional TrustAll
Server = https://github.com/brpjerry/mvplayer/releases/latest/download
```

```sh
sudo pacman -Sy mvplayer
```

Or build the package yourself from `packaging/PKGBUILD` with `makepkg -si`.

## Build from source

Dependencies (Arch package names): `qt6-base qt6-declarative qt6-shadertools
openssl mpv libpulse taglib chromaprint cmake ninja`, plus `yt-dlp` and `ffmpeg` at run time.

```sh
cmake -S . -B build -G Ninja
cmake --build build
./build/mvplayer
```

`sudo cmake --install build` also installs the desktop entry and icon.
`-DMV_BUILD_GUI=OFF` builds only the headless importer.

### Windows

The Windows build uses [MSYS2](https://www.msys2.org)'s UCRT64 environment,
which packages the same libraries as Arch. In a UCRT64 shell:

```sh
pacman -S --needed mingw-w64-ucrt-x86_64-{gcc,cmake,ninja,pkgconf,openssl,qt6-base,qt6-declarative,qt6-shadertools,mpv,taglib,chromaprint,ffmpeg,ntldd,python}
cmake -S . -B build -G Ninja
cmake --build build
./build/mvplayer
```

`ci/winpkg.sh` builds the installer (it needs
[Inno Setup](https://jrsoftware.org/isinfo.php) 6.5 or later): it gathers the
programs and every library they load into `packaging/windows/stage` and
compiles `packaging/windows/mvplayer.iss`.

## What is remembered between tracks

The single, the album cut, the live take and the remix of a song all turn up
the same candidates. In `<MV folder>/.mvplayer/cache` the importer keeps, per
candidate video, the audio it listened to, YouTube's description of the video
and whether its picture is a still image, and the results of each search for
a day. A video is then fetched and judged once; and while the description is
fresh (two hours) its picture and the video itself are downloaded from it
without opening the page again. Entries are dropped after two weeks, the
oldest first beyond 2 GB; "Retry tracks without a video" searches afresh.
The import log's `requests` counts what each lookup asked for, `fromCache`
how much of that needed no request.

## Choosing the music folders

Settings → Library starts from a **base directory** (the folder that holds
your music). Every music file under it is the music library, read for its
tags. The folders under it are shown as a tree with a check box each: the
ticked ones are the **video folders**, whose tracks are looked up for
videos, everything under a ticked folder included; a part-filled box means
some folders inside are ticked. A track outside every video folder is
indexed and never fingerprinted or looked up, until its folder is ticked,
its album is asked for, or a re-import names it. Unticking a folder inside
a ticked one keeps the rest of that one ticked. Choosing another base
directory drops the ticked folders that lie outside it (their videos stay,
as below). Headless, `--library-dir` names the music library and
`--music-dir` the video folders; without `--library-dir` the video folders
are the whole library, and a run is refused where the library holds tracks
outside them. `--lookup-album "<album artist>/<album>"` asks for an album.

A track whose file is in the music library no more — the base directory
was moved away from it, or the file deleted — is not forgotten: it is kept
as absent, out of every count and never looked up. When the file is in the
music library again, by the same path or recognised by its sound at
another, the track is back with its video and everything found out about
it, without a search.

The videos of absent tracks stay in the library and play as before. The
icon with a struck-through note in the top bar (next to the review one, with
a count) shows them on their own, and the sidebar narrows them as it does
the library. Their thumbnails carry the review buttons: ✓ keeps the video
for good — it is no longer counted as untracked — and ✗ deletes it: the
video, its thumbnail and subtitles, and what was remembered of its tracks,
so that those are looked up afresh should they ever return. ✗ waits while
the music folders are being read. Settings counts the untracked videos and
offers to delete them all, the kept ones excepted. Headless:
`--delete-untracked`.

## Subtitles

Where the uploader of a video provides subtitles in one of the languages set
in Settings (by default the language of the desktop), they are fetched with
the video and kept beside it as `<video>.<language>.srt`. Machine-made
captions and translations are not fetched. Plain subtitles are shown in the
player's own style; those that use YouTube's styling (colours, sizes,
positions) are converted to `.ass` and keep it. "Fetch for my videos" gets
them for the videos already in the library; the button in the control bar
switches them off. The outline and the drop shadow of plain subtitles are
set in Settings. Headless: `--subtitles en,ja`, with `--fetch-subtitles`
for existing videos.

## How importing works

The rules, every one of them, are drawn as decision trees in
[`docs/import-rules.html`](docs/import-rules.html): that page is the
reference, and a rule changes there before it changes in the code. In
outline, for every track without a video
(`src/core/ImportManager.cpp`):

1. **Search** YouTube through `yt-dlp` and rank the results by title, artist,
   channel and duration. Covers, live cuts, instrumentals, auto-generated
   "Topic" uploads and (unless enabled in Settings) unofficial uploads are
   dropped — except that on the artist's own channel the title is not held
   against an upload: the artist's cover of a song is the video of a track
   that is that cover, and the audio decides. A label's channel that names
   the artist is examined as well. An upload under half the track's length
   is not examined either: the ninety seconds used as a show's opening are
   a cut of the song, not its video. Tracks that are themselves instrumentals are skipped, and so is
   anything under 1:20: a jingle, a skit, an interlude (a show's opening
   cut of a song, its "TV size", runs a minute and a half and is looked up
   like any track). So is
   talk between songs — a stage announcement, an interview — when the title
   says so ("MC", "MC06", "Talk 2", "… (Interview)"; the word has to be the
   whole title or a tag) and the track also sounds like it: full of pauses
   and without a pulse. Either alone is not enough.
2. **Verify** the best candidates by downloading only their audio and
   comparing Chromaprint fingerprints with the track. A video is accepted only
   if its soundtrack contains the recording, and its picture is not a still
   image (judged on YouTube's smallest stream by how many pixels really
   change between samples). The track also has to make up at least half of
   the video: a short edit of a song (the cut used as a show's opening, say)
   does not take the video of the full version, only one of about its own
   length. A video shorter than nine tenths of the track — the opening clip
   of a show, a "short ver." — is a cut of the song and goes to review.
   Nor is a video taken that stops the song and carries on with it
   later — a reaction video pausing to talk — however much of it is the
   track: more than 15 seconds of other audio inside the song sends it to
   review. If nothing passes, nothing is imported for that track.
   Only an upload on the artist's own channel is taken as the track's
   video outright, and only when it is the same version by its title (not
   a live take, a dance practice or a remix of the song) and at least 72%
   of it is demonstrably the track's waveform. The channel is the artist's
   when it is named after the artist and nothing else ("Artist", "Artist
   Official", "DJGenki" for "DJ Genki", either half of "A and B"), when
   MusicBrainz lists it for the artist (asked once per artist, remembered
   in the library), or when you allowed it. Anything else that is the song
   — a label's upload, a verified re-upload channel, a video titled "Music
   Video" by anyone, the artist's own video over another mix — goes to
   review, where the card shows the channel. What audio your file has does
   not come into it: a lossy track gets a video on the same terms as a
   lossless one, and only which audio the video plays depends on it.
   A video already in the library plays the audio of the track that got
   it first. When a later track turns out to be the video's own audio — at
   least 90% of the waveform, and clearly more than the track that holds it
   (the Japanese track against a video first given to the song's English
   version, say) — the video plays the later track from then on. The
   earlier track keeps the video as well: whether it is another master of
   the same performance or another version of the song cannot be measured.
3. **Download** the best video stream and the YouTube thumbnail.
4. **Align** the track to the video's soundtrack (`src/core/AudioAlign.cpp`):
   fingerprints find where the track sits, cross-correlation makes that
   sample-accurate, and a frame-by-frame comparison marks the parts of the
   video that are not on the record (intros, inserted scenes, outros).
   An upload is often not the album master — quieter, re-equalised, of
   inverted polarity, a few milliseconds longer over the song — so the
   comparison is made in the mid band, by the size of the correlation, at an
   offset that is followed as it drifts. Where the two merely match poorly
   and the video adds nothing of its own, the track still goes in.
   The track's audio takes the video's place when 72% of it is demonstrably
   the same waveform. The same performance in another mix (reverb added,
   the voice at another level) shows less than that, as does a cover over
   the same backing, and the waveforms cannot tell those apart: such a
   video waits for you, with the track laid whole at the offset where the
   loudness of the two moves together, so that you can hear both.
5. **Mux** (`src/core/Muxer.cpp`): if the track is higher quality than
   YouTube's audio, its samples are copied bit-for-bit into the matching
   regions and the video's own audio (level-matched) fills the gaps, with 20 ms
   crossfades. The result is the default FLAC audio track; YouTube's audio is
   kept as a second track.

YouTube's audio is only ever a video's main audio when the track is the
lesser of the two (a low-bitrate file). A video that is the song by its
fingerprints, while the waveforms cannot show it to be the track's recording
— a live take against the release of that concert on disc, a cover, another
mix — is not for the importer to decide. It is taken only when no candidate
fits outright, and then waits for you:

- The review icon in the top bar (next to the import one, with a count) shows
  these videos in place of the library. They are in no other view. While
  they are shown, the sidebar lists the tags of the videos waiting and
  narrows them as it does the library; the tag picked stays when you go
  between the two, unless the other side has nothing under it.
- A card is one video, and it has candidate tracks: the tracks whose video
  it could be. Mostly that is one. A second track whose search turns up a
  video that already waits for review becomes one more candidate of it,
  where it is the song too (the original and its English version, a single
  and a live take of it); and a track can be a candidate of several videos,
  when several uploads could be its video. The thumbnail says how many
  candidates a video has when it is more than one.
- Playing one, you hear your track's audio put to the video. `A`, or a click
  on the chip in the bottom bar, goes on to the next audio the file has and
  round again at the same place: each candidate's that is better than
  YouTube's, then YouTube's own. The chip says which is playing — "Library
  audio 1/2 (FLAC 24/96)", "YouTube audio" — and carries the key. The change
  is gapless: every audio track is decoded at once and the chip only changes
  which one is heard. Where the track's audio does not reach (the video has
  an intro the track lacks, or runs on after it), the library track carries
  YouTube's audio instead; there the chip says so ("YouTube audio · library
  from 0:12") and the key does nothing, and the seek bar shows the stretch
  the track's audio covers.
- While the queue is shown, a panel beside the sidebar takes the place of
  the bottom bar's music-file popup. It is about the video that plays, or
  until one does, the first in the queue, and shows one candidate at a
  time: its title and artist, its music file with every tag, and the
  video's page on YouTube with the Re-import and Replace… buttons. With
  several candidates, arrows at its top (and in the bottom bar's corner,
  for the playing video) go to the next one and back; on the playing video
  the audio goes with them, and while YouTube's audio plays the corner
  stays on the candidate it showed. First comes the candidate the upload is
  titled after (and no other), then the ones whose waveform is closest to
  the upload's. A candidate that is a candidate of other videos as well
  carries a "+1" button, which brings the next of those into view; the
  queue is ordered by track, so they are mostly side by side.
- ✓ on its thumbnail accepts it for the candidate on show: it joins the
  library as that track's video, with that track's audio, and the other
  candidates' audio is taken out of the file. That track is then a candidate
  of nothing else, and a video left without a candidate is deleted. The
  video's other candidates wait on whatever else they are candidates of, or
  have no video. Your verdict vouches for that upload as the track's video,
  so a re-import does not ask again whose channel it is on; it says nothing
  of the channel's other uploads.
  ✗ turns it down for all of its candidates: the video is deleted, that
  upload is not offered to any of them again, and each waits on whatever
  else it is a candidate of or counts as having no video.
- Either can be taken back: until the next verdict (or the app closes), an
  Undo button in the review panel names the last one and puts things back
  as they were — the video in the queue with its candidates, the videos
  deleted with it, the files as they were. What a verdict deletes is only
  set aside in `.mvplayer/undo` for that long. Channel verdicts are not
  undone this way (unlist the channel in Settings), and the headless
  importer's verdicts are final.
- Rest the pointer on ✓ or ✗ for a second and a second button comes up under
  it, for the upload's channel as a whole. They are offered where the
  channel is not an artist's channel already.
  The double tick allows the channel: it counts as an artist's channel from
  then on, for every track (a label's channel has the videos of many), and
  is never ignored as a small one. Its videos under review are judged again
  at once, from their files and with nothing asked of YouTube: one that
  waited only on whose channel it is on is accepted, and one that waits for
  another reason (too little of the same waveform, a cut, another version)
  stays for your verdict, the card the button was on included.
  The double cross denies the channel: every video the library has from it
  is deleted, the ones under review as turned down for all their candidates,
  and its uploads are never taken again. Tracks that had one of its videos
  are looked up again for another.
  Either way the app first lists what would change, video by video, and
  asks. The Channels tab of Settings lists the channels you allowed and
  denied; taken off there, a channel is judged like any other again, and
  nothing in the library changes with that.

In the library there is no sharing: a track has one video and a video one
track. When a track's search turns up a video that another track has, the
two are laid against the upload's own audio (the file keeps it) and the one
whose waveform is the closer fit has the video — by however little,
whichever came first, and however it came to have it. The correlation of
the two waveforms over the whole of both decides: another master, another
singer, a longer or shorter cut all count against a track. The video is not
taken where its length rules it out for the newcomer (under half the track,
over twice it or four minutes longer, or a cut under 90% of it). The track
that had the video goes back to the queue, to see whether another video is
its; the same recording in another master, on another release, has no
video unless another upload is found for it.

Somebody's re-upload of a song comes from a small channel. With "Ignore
small channels" on in Settings (it is, to start with), an upload from a
channel with fewer subscribers than the limit there — 5,000 unless you set
another — is not taken. The count is on the upload's page, not in the search
results, so the upload is dropped once its audio has arrived, before it is
listened to. A verified channel is not dropped whatever its size (an
artist's own can be a small one), nor is an upload that YouTube lists under
several channels ("A and B"): one of them meeting the limit is enough, and
for such an upload the page gives no count to go by. A count that is not
known, or that the channel hides, is no reason either. Nor is a channel you
allowed ignored: a label's channel, or an artist's own, can be a small one
that is not verified. The library keeps what the page said for every video
it brings in. Headless: `--min-subscribers <n>`, `0` for no limit.

Headless: `--approve <youtube id>` accepts for the candidate shown first,
`--approve <youtube id>=<music file>` for that file's track, and
`--reject <youtube id>`. `--allow-channel <channel>`, `--deny-channel
<channel>` and `--unlist-channel <channel>` take a channel's id (`UC…`), or
its handle (`@name`) or the address of its page where the library has a
video of it: a name will not do, since two channels can have the same one.
Headless: `--approve <youtube id>` and `--reject <youtube id>`.

Turning off "Use my library's audio" in Settings lifts all of this.

### Re-importing

The rules change, and a video imported under yesterday's may not pass
today's: an upload from a third party's channel that was once accepted
outright now waits for review. Re-importing looks a track up again as if
for the first time, with its video judged by today's rules as one candidate
among the search results. The upload's audio and picture are in the file
already, so nothing is downloaded for it:

- Where it still fits outright, it is kept as it is. Where the track's audio
  was not in it and now belongs there, it is put in.
- Where it is the song but can no longer be taken outright, it goes back to
  the review queue with the track as its candidate, its file rebuilt with
  your track's audio put in, alongside whatever else the search turns up.
- Where another upload fits outright and ranks above it, that one is
  imported in its place, and the old file is deleted.
- Where today's rules turn it down — a still image, a different recording,
  a title that marks it as another version — it is deleted likewise, and
  the track counts as having no video.
- A lookup that could not finish (YouTube refusing requests, the file
  unreadable) changes nothing: the track keeps its video.
- It is listened to whether or not it looks official: that test picks which
  search results are worth listening to, and this video is in the library
  already. Its length and a version word in its title still count.
- What the library did not keep about its channel — the channel's id, by
  which it is known as the artist's whatever name the track's tags carry,
  and its subscribers — is read from the upload's page, once (one request,
  nothing downloaded). That goes for videos from before these were kept,
  and for the ones under review the track is a candidate of. A video from
  a channel that turns out to be a small one goes like any other that
  today's rules turn down.
- A track that waits for your verdict has every video it is a candidate of
  judged again, beside the best four of the search and whether or not the
  search returns it: what today's search happens to return does not decide
  whether a video is judged by today's rules. One that has become the
  track's own recording (its channel is known as the artist's now, say) is
  accepted; the others stay for your verdict, unless their length or their
  channel's size rules them out.
- A video whose file is empty or missing — a machine that goes down while
  a video is being written can leave it empty — is deleted first, whether it
  is the track's video or one under review that it is a candidate of. Its
  upload is then found and downloaded again like any other.

A video under review that today's rules rule out for the track by its
length — under half the track — or by its channel's size, where the library
kept the count, loses that track as a candidate as soon as the re-import is
asked for, without a request to YouTube; with no candidate left, it is
deleted.

A library from before a video had one track is brought in line when this
version first opens it, by itself: the tracks of every video that several
tracks have are re-imported. The one that is the closest fit to the upload
keeps the video (and the video plays that track's audio, or YouTube's); the
others go back to the queue. Videos that waited for review as several
options for one track are a card each from then on.

Tracks without a video are simply looked up again. Re-import one video
from the Re-import button in the music-file popup (the bottom bar's
title; the review panel, for a video under review), every video under a
sidebar entry from its right-click menu, or
the music files in a folder or one file from Settings. Headless:
`--reimport <file or folder>` (repeatable), and `--reimport-before <time>`
for every track with a video whose last lookup was before then (ISO 8601:
`2026-10-05T16:35` or `2026-10-05`). The import log's `lookup` entries
carry `reimport: true` and, under `prior`, the video the track had and its
fate: `kept`, `review`, `removed`, `released` (a track that has left the
music folders has it too) or `unjudged`.

### Replacing a video

When the importer's pick is not the one you want, Replace… in the
music-file popup (beside Re-import) puts another upload in its place. The
dialog searches as an import does and listens to the results, but asks
nothing about whose channel they are on: up to three uploads that are the
track by ear are offered with their channel and how much of the waveform
is the same. You can also paste a YouTube link or id. An upload that does
not sound like the track asks "Are you sure?" first; taken all the same,
it keeps its own audio. The chosen upload is downloaded and built like any
import (your track's audio goes in where it is the same recording), it
counts as accepted, the old video goes, and the track is no longer a
candidate of any video under review. Headless: there is no equivalent. The import log records a
`replace` event.

The music folders are watched; added tracks are picked up within a few
seconds, edited tags on the next periodic rescan. Tracks with no video are
retried after two weeks. Lookups that could not be completed (a download
error, typically YouTube throttling) are not treated as "no video": they are
retried after 30 minutes, then at doubling intervals.

If YouTube starts refusing requests altogether (HTTP 429, a bot check, a
search answered with no results at all when a probe search right after it
gets none either, which is how a guest is refused after about a hundred
searches in a few minutes, or eight downloads failing in a row), the whole
queue pauses instead of failing
track after track: 10 minutes at first, doubling up to two hours while the
block lasts, then one job tests the water before the rest follow. The import
panel shows the pause and has a "Resume now" button; `pauseSeconds` under
`[import]` in the config file changes the first wait.

One kind of refusal does not pass by waiting: "Sign in to confirm you're not
a bot". YouTube then serves a connection's guests no video at all — whatever
the client, for days — and there is nothing to solve: what it asks for is the
sign-in. Searches still work. The pause says so, and with an account set up
(below) offers "Carry on signed in": the refused request is made again with
the account, and while guests are refused the downloads go to the account
straight away, with a guest trying again every half hour. That is yours to
switch on (also under Settings → Import → YouTube Premium), because an account that
downloads a great deal is one YouTube may restrict; videos fetched that way
also miss the "1080p Premium" picture. Where the account's cookies have
expired the pause says that instead. Headless: `--sign-in-on-bot-check` with
`--cookies`.

Everything the importer decides is recorded in
`<MV folder>/.mvplayer/import-log.jsonl`, one JSON object per line:

- `lookup`: one per track looked up — the queries, every search result with
  its score and why it was dropped or shortlisted, and for each video that
  was examined the decision (`accept`, `review`, `reject`, `undecided`), the
  reason in words, and the measurements behind it: fingerprint coverage, the
  share of the track that is demonstrably the same waveform, the loudness
  correlation and offset, segments, gain, polarity. Also what was brought into
  the library and how many requests it took.
- `track`: what the scan made of a file new to the library — queued, skipped
  by its title, another file of a known recording, a known track at a new place.
- `verdict`: a video under review accepted or turned down.
- `reimport`: tracks queued for a lookup by today's rules (see Re-importing).
- `broken-video`: a video deleted at a re-import because its file was empty
  or missing.
- `takeover`: a video in the library that changed hands — a track that is
  the closer fit to the upload took it (`fit` against `holderFit`), and the
  track that had it was queued again (`requeued`).
- `shared-settled`: a video that several tracks had, from before a video had
  one track, left with the closest fit of them (`measured`, `keeps`).
- `withdrawn`: a track that has its video is no longer a candidate of
  another one under review, which is deleted if it has no candidate left.
- `outdated-option`: at a re-import, a track no longer a candidate of a
  video under review that today's rules rule out for it by its length
  (under half the track, say) or as one from a small channel.
- `untracked-kept` / `untracked-deleted`: a video whose tracks are gone kept,
  or deleted (one at a time, or all of them from Settings).
- `paused` / `resumed`: the request circuit breaker.

`tools/import-report.py <MV folder>` turns the log and the database into a
table of tracks, the videos examined for each, the decisions, reasons and
numbers (`--tsv` for a spreadsheet).

Importing can be interrupted at any point — closing the window, logging out,
Ctrl+C. Unfinished tracks stay queued and are picked up on the next start;
partial downloads are discarded. Unticking a folder in Settings drops its tracks but
keeps their videos.

Each library lives in its MV folder: videos and thumbnails under
`<Album Artist>/`, state in `.mvplayer/library.db`.

The folder is self-contained. To move the library to another disk or another
device (Linux or Windows), copy the whole folder, `.mvplayer` included, and
choose it as the music video library in Settings.

### Which file is which track

A track is known by how it sounds, not by its path or tags
(`src/core/AudioPrint.cpp`): the scan keeps a Chromaprint fingerprint of every
file. So a file keeps its video, and is not looked up again, when it is moved
or renamed, retagged (a romanised title, another album artist), or replaced
by a copy in another format — the music library on the other device can be an
Opus conversion of the FLAC one the videos were imported from.

Files that hold the same recording — a single and its album cut, or two
copies — count as one track: one lookup, one video, and the video carries
the tags of the best-quality file. An instrumental, a live take, another language
version or a short edit is a different recording with a lookup of its own.

When a recording has no video and one of its files carries a title that has
not been searched for — the original script in one place, romanised in
another — it is searched once more under that title.

Fingerprinting costs about half a minute for 30 hours of FLAC on a laptop.

A library database is tied to the version that wrote it: there is no
conversion between layouts. A database of another layout is refused with a
message saying so; move it away or delete it (`.mvplayer/library.db` in the
MV folder) to start a new library.

### YouTube Premium

With the cookies of a Premium account YouTube offers audio at about twice the
usual bitrate (around 250 kbit/s Opus instead of 130). Export a `cookies.txt`
from a browser signed in to the account and add it under Settings → YouTube
Premium; the app keeps its own copy beside its settings file, readable by you
only. From then on every imported video is built from that audio — as its
"YouTube audio" track, and as its main audio where the library's own does not
replace it.

"Check cookies" asks YouTube whether the cookies still work. Browsers replace
them every so often, and an export goes stale with that; exporting from a
private window that is then closed keeps them alive longer.

"Check videos for better quality" goes through the videos already in the
library, one at a time, and rebuilds those the account is offered something
better for: the audio is fetched again and put into the existing file, the
picture is only downloaded again if a higher resolution has appeared.

The account is used for nothing else unless you say so. Searches and video
downloads stay anonymous (the "1080p Premium" picture needs no account, and
signed-in clients are not offered it); the cookies are only tried for a video
that cannot be had without signing in, such as an age-restricted one, and —
once switched on — where YouTube refuses guests with its bot check. Headless:
`--cookies cookies.txt` with `--check-quality`, or with the `check-cookies`
command.

### Headless

```sh
./build/mvplayer-import --music-dir ~/Music --music-dir /mnt/nas/music --mv-dir ~/Videos/MVs
./build/mvplayer-import align track.flac video.mkv       # show the alignment
./build/mvplayer-import mux track.flac video.mkv out.mkv # audio replacement only
./build/mvplayer-import check-video video.mkv            # still image or real video?
```

## The judge

The judge can use a Claude API key or your ChatGPT account. In Settings →
Judge, select **ChatGPT account**, click **Continue with ChatGPT**, finish
signing in and granting plan access in your browser, then choose an available
model and enable Advice or Judge. Sign-in alone does not start library work.
Claude remains the default provider for existing settings.
Its rules are a page of their own, `docs/import-rules-judge.html`, with the
words it is given and how it measured against hand verdicts. Two modes:

- **Advice**: lookups run by the rules as before, and each card under review
  shows whether the judge would keep the video for the candidate on show, and
  how sure it is ("Judge: keep · 92%"), with its reasons when the pointer
  rests on it. It is asked once per video and candidate, from the video's own
  file; "Ask again" asks anew. Nothing is decided by it.
- **Judge**: it runs each lookup — writes the search, picks what to listen to
  from the results, and gives the verdict once every pick has been listened to
  and looked at (five frames of each), told each upload's channel, its
  subscribers, upload date and the first 600 characters of its description,
  which the library keeps with every video from now on. A verdict it is at least 85% sure of
  stands by itself; anything less, a cut, an interrupted video, one the audio
  check doubts or a still picture is left for you, with its advice on the
  card. Your verdicts on cards stand as before. It also decides, from the
  tags, which tracks are not worth a search (an instrumental, a talk track),
  told how a talk-titled track sounds; turning judge mode on asks it once
  about every track the rules had skipped, and what it skips stays skipped
  until the file's tags change.
- In advice mode, "Review all" above the queue asks about every card at
  once, each card wears the answer for its candidate on show, and "Accept
  all" carries out the answers the judge is at least 85% sure of as
  verdicts, after saying what it will do. How many questions run at once is
  a setting on the Judge tab (one unless you raise it).

Claude offers Haiku 5.5, Sonnet 5.5 (the default) or Opus 5.5. ChatGPT's
model choices come from your account. The app remembers the Claude model and
each ChatGPT account's selected model separately. Existing advice remains
until you use “Ask again”. The accuracy measurements here are for Claude;
ChatGPT models have not been benchmarked against those verdicts.

ChatGPT requests use your plan allowance. **ChatGPT usage** opens your account's
usage settings. Judge events record the provider and token counts; Claude also
records estimated API cost, while ChatGPT's `cost` is `null`. Missing access,
expired sessions that cannot be renewed, or unavailable models report an error;
they do not switch providers automatically.

ChatGPT uses OpenAI's [public-client sign-in flow](https://developers.openai.com/siwc/token-sharing-open-source/sign-in)
and [Responses API](https://developers.openai.com/siwc/token-sharing-open-source/models-and-inference).
The app stores its own registrations and renewable credentials beside its
settings in `chatgpt.json`, with owner-only file permissions. It does not use
another app's session. Tokens are renewed automatically and excluded from
captures and logs. **Sign out** clears local tokens and requests remote
revocation; if that fails, the app says to disconnect MV Player in ChatGPT
Settings. Saved account registrations remain available for signing in again.

The headless importer uses the same account store. These account commands exit
without opening a library:

```sh
mvplayer-import --chatgpt-login
mvplayer-import --chatgpt-models
mvplayer-import --chatgpt-logout
```

Use `--chatgpt-account new --chatgpt-login` to add another account, or
`--chatgpt-account <client-id>` to select a saved registration (the login command
prints its ID). `--chatgpt-auth-file <file>` selects a separate account store.
To import with ChatGPT, add `--judge-provider chatgpt --judge judge
--judge-model <model-slug>` to the normal library arguments. The model slug
comes from `--chatgpt-models`; without an explicit slug the saved GUI selection
is used. Claude retains `--judge-provider claude` (the default),
`--judge-model haiku|sonnet|opus` and `--claude-key-file`. Both providers support
`--judge off|advice|judge`.

## Keeping everything for a replay

Settings → Import → "Keep everything for a replay" (headless: `--capture
<dir>`) keeps, for good, everything a lookup touches: every search YouTube
answers (`search/<key>.json`, listed in `search/index.jsonl`), and for every
video its audio with the page that came with it, its page where one was read
on its own, its preview, the video with its thumbnail, its subtitles, and the
account's answers (`video/<id>/`); and every question to the judge with its
answer (`judge/<hash>.json`, the request as sent, frames included). The
folder is `.mvplayer/capture` under the videos folder; the Import tab says
how much it holds. Nothing is pruned: it is yours to delete.

A replay runs a lookup again from a capture: `mvplayer-import --replay <dir>`
answers searches, pages, downloads and the judge from the folder wherever it
has the item, and asks YouTube or the API as usual where it has not (keeping
that too, with `--capture` on). To run a past lookup again as it went, point
a scratch library at the capture and ask for the track:

```
mvplayer-import --replay ~/Videos/MVs/.mvplayer/capture --mv-dir /tmp/replay \
    --music-dir ~/Music --reimport "~/Music/Artist/Album/01 Song.flac" --judge judge
```

With the judge's words unchanged its answers come from the capture; with new
words the same frames and facts go to the API again, which is how a change to
the words is tried against what the judge once saw.

## Using it

- Clicking a video grows its thumbnail into the player. `Esc` (or the back
  arrow) returns to the library with the video still playing inside its own
  thumbnail; click it to bring the player back.
- In the bottom bar, the playing video's picture switches between the player
  and the library. Its title shows the music file the video was found for —
  where it is, how long it plays, its format and every tag in it, with arrows
  when several files hold the recording — and the video's own length, its
  channel and the day it was published, with a link to it on YouTube. For a video under review the corner shows its
  candidate track on show, with arrows when it has several, and the
  music file and YouTube page are in the review panel beside the sidebar
  instead (see above); the track's length is in the bar itself, beside the
  artist: the seek bar has the video's, and a video much shorter than its
  track is a cut of the song.
- The pill at the left of the top bar's tools switches between the two
  libraries: **Videos**, the music videos, and **Music**, every album under
  the base directory as a grid of covers (the picture beside the album's
  first file, else the one in its tags). Clicking an album plays it from its
  first track, the rest of the album queued after it, from the music files
  themselves and without a gap between them; the bottom bar's controls work
  as for a video, and in place of
  the fullscreen button a switch hides the panel right of the albums (the
  queue, the playing track and its cover) until it is pressed again. A chip
  on a cover of an album in
  a video folder counts its tracks that have a video. The sidebar, the sort
  menu (by album, by artist, chronologically, by the files' modification
  date; each library keeps its own sort across restarts) and the search box
  narrow the
  albums as they do the videos; the mode is remembered. A right click on
  an album offers **Find videos for this album**, which looks its tracks up
  wherever their files are (Settings → Library lists such albums, each with
  a button to take it off again), and **Re-import album videos**. With
  Settings → Playback → **Play the music video when a track has one** on, a
  track that has its video plays the video, grown over the albums, and the
  next track without one goes back to the music.
- The sort button in the top bar orders the grid by title, by artist,
  chronologically by the track's year ("Chronological - Audio") or by the day
  the upload was published ("Chronological - Video", the year where the day
  is not known), or by when the video was added. The arrow beside it flips
  the direction; each order starts its natural way round, names A to Z and
  dates newest first.
- The sidebar lists one tag at a time. The selector above the list switches
  between album artists, artists, genres, albums and years, and the box under
  it filters the listed values. The search box at the top right searches every
  tag of every video.
- Settings → Appearance: dark, light, or **Auto**, which follows the operating
  system's preference and switches with it while the app is running.
- Settings → Appearance also has two sliders, **Thumbnail size** and **Cover
  art size**: the widest a card gets in the video grid and in the album grid
  (180 to 550 px in six steps, 350 by default). A grid fits as many cards
  across as keep within that width and they share the row, so they grow with
  the window up to the chosen width, and one more goes across once they would
  pass it. The cover you see is never under half of it.
- **Closing the window keeps the music playing.** With Settings → Playback →
  **Keep playing when the window is closed** on (it is by default) and a tray
  on the desktop, a close puts the app in the tray instead: its icon there
  shows the window again on a click, and its menu has play/pause, next,
  previous and quit. Starting MV Player again while it runs brings up the
  running window (out of the tray if it is there) rather than a second copy,
  as does the desktop's "raise" through MPRIS. Without a tray, a close quits
  as before. Ctrl+Q, or Quit in the tray menu, ends the run.
- Settings → Playback → **The volume bar sets the system volume**: the bar
  shows and moves the output device's level (the default sink through
  PulseAudio or PipeWire on Linux, the default playback device on Windows),
  as the desktop's own volume keys do, and follows it when it is changed
  elsewhere; the app itself then plays at full level. Off, the bar is the
  app's own volume, as before.
- Settings → Accent colour: a colour (presets or the hue strip) and the
  **Auto** switch. Auto off, the colour is the interface's. Auto on, it
  takes the colour from the playing video, or from the album's cover for a
  track played from its file, and the chosen colour is the one while
  nothing plays. Auto looks at one
  downscaled frame a second, blends it with the previous samples and fades
  between colours, so cuts do not make the interface flicker. Black, white
  and grey frames give a neutral accent (light grey to white on dark, dark
  grey to black on light).
- Settings has a tab for each of: the library (the base directory and its
  folder tree, re-import and the untracked videos), appearance, playback (subtitles), import behaviour
  (with YouTube Premium), the channels you allowed and denied, and the
  judge. Settings → Judge offers Claude API keys and ChatGPT account sign-in
  for advice on review cards or judging lookups, as described above.
- The right-hand transport button says what follows the album that plays
  (in the music library) or the video (in the video library): **Autoplay**
  goes on to the next album in the grid's order, or the next video;
  **Repeat** plays the album, or the video, again; **Stop after** ends
  playback after it. Shuffle draws the album's tracks, or the grid's
  videos, in a random order, each once. An album that follows another
  starts without a gap.
- On Linux the playing video is offered to the desktop's media controls
  (MPRIS): media keys, `playerctl`, panel and lock-screen widgets show it and
  can play, pause, skip, seek and change the volume, shuffle and repeat. The
  app shows up there only while a video is loaded, so when idle it leaves the
  media keys to other players.

## Keys

| Key | Action |
| --- | --- |
| Space | Play / pause |
| ← → (Shift: 30 s) | Seek 5 s |
| ↑ ↓ | Volume |
| M | Mute |
| Ctrl+M, Ctrl+Tab, Ctrl+Shift+Tab | Videos / Music |
| A | Your library's audio / YouTube's, where a video has both |
| N / P | Next / previous |
| F, F11, double-click | Fullscreen |
| Esc | Leave fullscreen, then back to the library |
| Ctrl+F, / | Search |
| Ctrl+, | Settings |
| F12 | Frame rate counter |

## Options

`mvplayer --help` lists them. `--music-dir` (repeatable) / `--mv-dir` override
the saved folders for one run (the base directory is then the folder that
holds every `--music-dir`), `--mute` silences a run, `--fps` shows the frame counter,
`--ipc <socket>` opens a line-based control socket for UI automation (see
`ipc()` in `qml/Main.qml`).

Scroll feel is set in `~/.config/mvplayer/mvplayer.conf`
(`%APPDATA%\mvplayer\mvplayer.ini` on Windows) under `[ui]`:
`touchpadGain` (default 4: touchpad distance is multiplied by this, as browsers
do), `flickDeceleration` (default 4800 px/s²: higher means less glide after a
flick) and `wheelStep` (pixels per mouse-wheel notch, default 170).

Environment: `MVPLAYER_HWDEC` (mpv `hwdec` value, default `auto-safe`),
`MVPLAYER_MPV_OPTS` (`name=value,name=value` extra mpv options),
`MVPLAYER_YTDLP` (the yt-dlp program to run instead of the default one),
`MVPLAYER_CLAUDE_API` (another server in place of the Claude API, for tests).
`MVPLAYER_CHATGPT_AUTH` and `MVPLAYER_CHATGPT_API` select test servers for
ChatGPT authentication and inference; only HTTP loopback addresses are accepted
as overrides. Tests use temporary account stores and a local server, with no
live credentials or library. Run `ctest --test-dir build --output-on-failure`
for those tests, or `ci/smoke.sh build` for the full importer checks as well.

## Porting notes

The application runs on Linux and Windows; Qt Quick, libmpv, TagLib,
Chromaprint, yt-dlp and ffmpeg are also available on macOS.
Platform-specific code is confined to:

- `src/core/Util.cpp`: stopping a helper program together with everything it
  started (process groups on Linux, job objects on Windows), reacting to
  Ctrl+C and logout, console output, and where the helper programs are found.
- `src/ui/IdleInhibitor.cpp`: keeping the screen awake (D-Bus on Linux,
  `SetThreadExecutionState` on Windows).
- `src/ui/MediaSession.cpp`: the desktop's media controls, MPRIS on Linux
  (`src/ui/Mpris.cpp`); Windows would need System Media Transport Controls.
- `src/ui/SystemTheme.cpp`: the desktop portal's dark/light preference, with
  Qt's own as the fallback.
- `src/ui/MpvItem.cpp`: the native-display hand-off to mpv.
- `src/ui/YtDlpUpdater.cpp`: the yt-dlp download, offered only on Windows.
- `src/ui/WindowFrame.cpp`: on Windows the system title bar is removed and the
  top bar carries the window controls; Linux keeps the window manager's
  decorations.
- `src/ui/PointerPacer.cpp`: on Windows, presents every frame while the mouse
  moves over the window, so the pointer stays smooth with G-Sync in windowed
  mode.

On Windows a video cannot be replaced or deleted while it is playing, and
very long artist or title names can run into the 260-character path limit.

## Development

CI (`.github/workflows/ci.yml`) builds on Arch and on Windows, runs
`ci/smoke.sh` — an end-to-end check of the audio alignment and muxing on
synthetic files — and builds the pacman package and the Windows installer.
Pushing a `vX.Y.Z` tag publishes both as a GitHub release
(`.github/workflows/release.yml`).
