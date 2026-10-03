# MV Player

A music player for music videos. Point it at an audio library and it builds a
matching video library: it finds each track's music video on YouTube, checks by
ear that it really is that recording, downloads it at the best quality on
offer, and muxes in your own audio where it is better than YouTube's.

The audio library (one or more folders) is only ever read. Videos go to a
single separate folder (`~/Videos/MVs` by default).

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

## How importing works

For every track without a video (`src/core/ImportManager.cpp`):

1. **Search** YouTube through `yt-dlp` and rank the results by title, artist,
   channel and duration. Covers, live cuts, instrumentals, auto-generated
   "Topic" uploads and (unless enabled in Settings) unofficial uploads are
   dropped. Tracks that are themselves instrumentals are skipped.
2. **Verify** the best candidates by downloading only their audio and
   comparing Chromaprint fingerprints with the track. A video is accepted only
   if its soundtrack contains the recording, and its picture is not a still
   image. If nothing passes, nothing is imported for that track.
3. **Download** the best video stream and the YouTube thumbnail.
4. **Align** the track to the video's soundtrack (`src/core/AudioAlign.cpp`):
   fingerprints find where the track sits, cross-correlation makes that
   sample-accurate, and a frame-by-frame comparison marks the parts of the
   video that are not on the record (intros, inserted scenes, outros).
5. **Mux** (`src/core/Muxer.cpp`): if the track is higher quality than
   YouTube's audio, its samples are copied bit-for-bit into the matching
   regions and the video's own audio (level-matched) fills the gaps, with 20 ms
   crossfades. The result is the default FLAC audio track; YouTube's audio is
   kept as a second track. Otherwise the YouTube audio is used as is.

The music folders are watched; added tracks are picked up within a few
seconds, edited tags on the next periodic rescan. Tracks with no video are
retried after two weeks. Removing a folder in Settings drops its tracks but
keeps their videos.

Each library lives in its MV folder: videos and thumbnails under
`<Album Artist>/`, state in `.mvplayer/library.db`.

### Headless

```sh
./build/mvplayer-import --music-dir ~/Music --music-dir /mnt/nas/music --mv-dir ~/Videos/MVs
./build/mvplayer-import align track.flac video.mkv       # show the alignment
./build/mvplayer-import mux track.flac video.mkv out.mkv # audio replacement only
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

Scroll feel is set in `~/.config/mvplayer/mvplayer.conf` under `[ui]`:
`touchpadGain` (default 4: touchpad distance is multiplied by this, as browsers
do), `flickDeceleration` (default 4800 px/s²: higher means less glide after a
flick) and `wheelStep` (pixels per mouse-wheel notch, default 170).

Environment: `MVPLAYER_HWDEC` (mpv `hwdec` value, default `auto-safe`),
`MVPLAYER_MPV_OPTS` (`name=value,name=value` extra mpv options).

## Porting notes

Qt Quick, libmpv, TagLib, Chromaprint, yt-dlp and ffmpeg all run on Windows
and macOS. Platform-specific code is confined to `src/ui/IdleInhibitor.cpp`
(D-Bus screen-saver inhibit), `src/ui/SystemTheme.cpp` (the desktop portal's
dark/light preference) and the native-display hand-off in `src/ui/MpvItem.cpp`.

## Development

CI (`.github/workflows/ci.yml`) builds on Arch, runs `ci/smoke.sh` — an
end-to-end check of the audio alignment and muxing on synthetic files — and
builds the pacman package. Pushing a `vX.Y.Z` tag publishes that package as a
GitHub release (`.github/workflows/release.yml`).
