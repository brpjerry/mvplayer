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
failing use **Settings → Update yt-dlp** to fetch the latest version.

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
mpv taglib chromaprint cmake ninja`, plus `yt-dlp` and `ffmpeg` at run time.

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
pacman -S --needed mingw-w64-ucrt-x86_64-{gcc,cmake,ninja,pkgconf,qt6-base,qt6-declarative,qt6-shadertools,mpv,taglib,chromaprint,ffmpeg,ntldd,python}
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

## Changing the music folders

A track whose file is in none of the music folders any more — its folder was
removed in Settings, or the file deleted — is not forgotten: it is kept as
absent, out of every count and never looked up. When the file is in a music
folder again, by the same path (a parent folder added in place of its
subfolders) or recognised by its sound at another, the track is back with
its video and everything found out about it, without a search.

The videos of absent tracks stay in the library and play as before. Settings
counts them as untracked and offers to delete them: the video, its thumbnail
and subtitles, and what was remembered of its tracks, so that those are
looked up afresh should they ever return. Headless: `--delete-untracked`.

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

For every track without a video (`src/core/ImportManager.cpp`):

1. **Search** YouTube through `yt-dlp` and rank the results by title, artist,
   channel and duration. Covers, live cuts, instrumentals, auto-generated
   "Topic" uploads and (unless enabled in Settings) unofficial uploads are
   dropped — except that on the artist's own channel the title is not held
   against an upload: the artist's cover of a song is the video of a track
   that is that cover, and the audio decides. A label's channel that names
   the artist is examined as well. Tracks that are themselves instrumentals are skipped. So is
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
   video outright. The channel is the artist's when it is named after the
   artist and nothing else ("Artist", "Artist Official"), when MusicBrainz
   lists it for the artist (asked once per artist, remembered in the
   library), or when you approved one of its videos for the artist in
   review. Anything else that matches — a label's upload, a verified
   re-upload channel, a video titled "Music Video" by anyone — goes to
   review, where the card shows the channel.
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
   the same waveform — nearly all of it where track and video name different
   versions, since a version made over the original keeps most of it. The same performance in another mix
   (reverb added, the voice at another level) shows less than that, as does
   a cover over the same backing; there the fingerprints have to cover the
   song, the loudness of the two has to move together at the offset found,
   and neither may be marked as a version the other is not (live, remix,
   cover, by title or album). The track is then placed whole at that offset.
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
  these videos in place of the library. They are in no other view.
- Playing one, the sound changes every ten seconds between YouTube's audio
  and your track's, level-matched; the chip in the bottom bar says which.
- When several uploads could be the track's video, they are options on one
  card: the arrows on the thumbnail step through them. Accepting one drops the
  others; turning one down leaves the rest to choose from.
- ✓ on its thumbnail accepts it: it joins the library with your track's audio.
  ✗ turns it down: the video is deleted, its track counts as having no video,
  and that upload is not offered for it again.

Headless: `--approve <youtube id>` and `--reject <youtube id>`.

Turning off "Use my library's audio" in Settings lifts all of this.

The music folders are watched; added tracks are picked up within a few
seconds, edited tags on the next periodic rescan. Tracks with no video are
retried after two weeks. Lookups that could not be completed (a download
error, typically YouTube throttling) are not treated as "no video": they are
retried after 30 minutes, then at doubling intervals.

If YouTube starts refusing requests altogether (HTTP 429, a bot check, or
eight downloads failing in a row), the whole queue pauses instead of failing
track after track: 10 minutes at first, doubling up to two hours while the
block lasts, then one job tests the water before the rest follow. The import
panel shows the pause and has a "Resume now" button; `pauseSeconds` under
`[import]` in the config file changes the first wait.

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
- `paused` / `resumed`: the request circuit breaker.

`tools/import-report.py <MV folder>` turns the log and the database into a
table of tracks, the videos examined for each, the decisions, reasons and
numbers (`--tsv` for a spreadsheet).

Importing can be interrupted at any point — closing the window, logging out,
Ctrl+C. Unfinished tracks stay queued and are picked up on the next start;
partial downloads are discarded. Removing a folder in Settings drops its tracks but
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
copies — count as one: one lookup, one video, and the video carries the tags
of the best-quality file. An instrumental, a live take, another language
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

The account is used for nothing else. Searches and video downloads stay
anonymous (the "1080p Premium" picture needs no account, and signed-in
clients are not offered it); the cookies are only tried for a video that
cannot be had without signing in, such as an age-restricted one. Headless:
`--cookies cookies.txt` with `--check-quality`, or with the `check-cookies`
command.

### Headless

```sh
./build/mvplayer-import --music-dir ~/Music --music-dir /mnt/nas/music --mv-dir ~/Videos/MVs
./build/mvplayer-import align track.flac video.mkv       # show the alignment
./build/mvplayer-import mux track.flac video.mkv out.mkv # audio replacement only
./build/mvplayer-import check-video video.mkv            # still image or real video?
```

## Using it

- Clicking a video grows its thumbnail into the player. `Esc` (or the back
  arrow) returns to the library with the video still playing inside its own
  thumbnail; click it to bring the player back.
- The sidebar lists one tag at a time. The selector above the list switches
  between album artists, artists, genres, albums and years, and the box under
  it filters the listed values. The search box at the top right searches every
  tag of every video.
- Settings → Appearance: dark, light, or **Auto**, which follows the operating
  system's preference and switches with it while the app is running.
- Settings → Accent colour: a fixed colour (presets or the hue strip) or
  **Auto**, which takes the colour from the playing video. Auto looks at one
  downscaled frame a second, blends it with the previous samples and fades
  between colours, so cuts do not make the interface flicker. Black, white
  and grey frames give a neutral accent (light grey to white on dark, dark
  grey to black on light).

## Keys

| Key | Action |
| --- | --- |
| Space | Play / pause |
| ← → (Shift: 30 s) | Seek 5 s |
| ↑ ↓ | Volume |
| M | Mute |
| N / P | Next / previous |
| F, F11, double-click | Fullscreen |
| Esc | Leave fullscreen, then back to the library |
| Ctrl+F, / | Search |
| Ctrl+, | Settings |
| F12 | Frame rate counter |

## Options

`mvplayer --help` lists them. `--music-dir` (repeatable) / `--mv-dir` override
the saved folders for one run, `--mute` silences a run, `--fps` shows the frame counter,
`--ipc <socket>` opens a line-based control socket for UI automation (see
`ipc()` in `qml/Main.qml`).

Scroll feel is set in `~/.config/mvplayer/mvplayer.conf`
(`%APPDATA%\mvplayer\mvplayer.ini` on Windows) under `[ui]`:
`touchpadGain` (default 4: touchpad distance is multiplied by this, as browsers
do), `flickDeceleration` (default 4800 px/s²: higher means less glide after a
flick) and `wheelStep` (pixels per mouse-wheel notch, default 170).

Environment: `MVPLAYER_HWDEC` (mpv `hwdec` value, default `auto-safe`),
`MVPLAYER_MPV_OPTS` (`name=value,name=value` extra mpv options),
`MVPLAYER_YTDLP` (the yt-dlp program to run instead of the default one).

## Porting notes

The application runs on Linux and Windows; Qt Quick, libmpv, TagLib,
Chromaprint, yt-dlp and ffmpeg are also available on macOS.
Platform-specific code is confined to:

- `src/core/Util.cpp`: stopping a helper program together with everything it
  started (process groups on Linux, job objects on Windows), reacting to
  Ctrl+C and logout, console output, and where the helper programs are found.
- `src/ui/IdleInhibitor.cpp`: keeping the screen awake (D-Bus on Linux,
  `SetThreadExecutionState` on Windows).
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
