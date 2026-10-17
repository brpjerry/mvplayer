# UX pilot findings — 2026-10-17

Two agents drove one instance each (offscreen: music library and settings; cage: video playback) over the IPC socket, about 100 commands and 30 window pictures apiece. No crashes. Every item below marked **confirmed** was reproduced by hand afterwards. Pictures and the agents' full write-ups are under `scratchpad/uxtest/agent-1/` and `agent-2/` (tmpfs, gone after a reboot).

## Wrong behaviour

1. **F in music mode brings the video up over the albums.** _confirmed_
   With a video playing after Ctrl+M, F goes fullscreen; Escape then leaves `view=player` while `mode=music`, so the video sits between the sidebar and the music panel. The panel still says "Click an album to play it" and the title line is squeezed to "N…". Neither `setFullscreen` nor `showPlayer` in `qml/Main.qml` checks `App.mode`.
   Steps: `play 15`, `key Ctrl+M`, `key F`, `key Escape`.

2. **Escape closes neither the Settings panel nor the album menu.** _confirmed_
   Both are modal `Popup`s that never take focus, and Qt honours `CloseOnEscape` only for a popup with active focus. While the album menu is open, Space is swallowed too. A click outside closes both. `qml/SettingsPanel.qml:8`, `qml/AlbumGrid.qml:154`.
   Steps: `key Ctrl+,` then `key Escape`; `album-menu 4` then `key Escape`.

3. **Next is greyed on one-track albums while N works.** _confirmed_
   The button (and MPRIS `canStep`) is enabled only when the queue is longer than one (`Main.qml`, `canStep: player.queue.length > 1`). Autoplay now crosses albums, so Next should be enabled whenever a next album exists. 19 of the fixture's 23 albums are one track.

4. **P never returns to the previous album.** _confirmed by code_
   On a one-track album `previous()` always seeks to 0 (`queue.length === 1`), so N then P does not bring you back, although N crosses albums.

5. **A failed video load walks the grid and loses the music.** _confirmed offscreen_
   From a track, the Music video button switches to video mode; when the video fails to load, `onLoadFailed` calls `next(true)` for each failure, so it ran through eight videos and stopped with nothing playing. Falling back to the track in music mode would be better. (Offscreen, every video fails; on the desktop this needs a broken file.)

## Layout at the minimum window size (960×600), music mode

6. **The top bar overflows into the sidebar.** _confirmed_
   The "All Albums / 23 albums" heading vanishes and the mode pill lands in the sidebar next to the logo. Video mode fits; music mode loses 320 px to the panel. `qml/TopBar.qml` anchors the headings to the tools, which neither shrink nor clip.

7. **The album grid keeps its column count.** _confirmed_
   At 960 wide with 5 across and the panel on, covers are about 64 px; the "1 / 1" video badge covers the duration badge and sticks out past the card's left edge; titles shrink to one word. At 960×1300 with 3 across the badges touch and the year is cut off.

8. **The queue list collapses to zero height at 600 px tall.** _confirmed_
   The queue row's clapperboard icon still floats beside the track title.

9. **The panel button's tooltip is clipped at the right window edge** ("Show the queue ar"). _reported_

10. **"To Review" is cut to "To Rev…" at 1280 wide** in the Review section. _confirmed_

## Visual and minor

11. **The seek bar's hover time bubble covers the pause button** when the pointer is near the middle of the bar. _confirmed_

12. **Card and seek-bar lengths differ by one second.** _confirmed by code_
    `formatDuration` in `src/core/Util.cpp` rounds; `Theme.formatTime` in `qml/Theme.qml` floors. "End of the Summer" is 3:24 on the card and 3:23 on the seek bar.

15. **The music panel ignores a video playing in music mode.** _confirmed_
    After Ctrl+M with a video playing, the control bar shows the video but the panel says "Queue / Click an album to play it" with an empty cover.

16. **Auto accent from a pale frame makes checked toggles look unchecked.** _reported_
    An accent of rgb(115,158,165) left the checked Autoplay icon nearly the same grey as the unchecked shuffle icon.

17. **A CJK album title ("真空都市") sits out of line.** _reported_
    It renders bolder in the fallback font and its artist/year line is 4 px lower than its neighbours'.

## Unverified

19. **Stray placeholder icons over the control bar and seek bar.** Distorted copies of the album cards' placeholder note and badge icons, at the grid's column positions, even in video mode with the album grid hidden. Seen offscreen only (both agents and by hand); may be the offscreen renderer with `Shape` items. One check under cage with an album that has no cover would settle it.

21. **One mpv "Audio/Video desynchronisation detected!" warning** after expand → hide (4 s) → show, seen once; mpv prints it once per process.

## Harness, before the full run

- Offscreen draws no covers, thumbnails or video (the shader-based images do not render there) and every video load fails. Offscreen agents can test state, layout and settings; anything judging pictures of cards or playback needs cage, which in turn cannot resize. Split the ten accordingly and say so in the brief.
- `--mute` pins the volume bar drawn empty, so mute and the bar's fill went untested. A test-only way to see the real bar is needed, or that area is excluded.
- Both agents ran about 25 % over budget; one did a read-only listing outside the allowed paths. Make the budget a hard stop and restate the allowed paths.
- Add a right-click-at-coordinates command; `album-menu` opens at a fixed spot.

## Do not fix
13. **The top bar's audio chip keeps "FLAC 24/44.1" after A switches to the YouTube audio.** _confirmed by code_
    `qml/TopBar.qml` builds its chip from the video's static `audioSource`/`audioDetail`; the control bar's chip follows the live track.

14. **The seek bar shows the previous video's time while the next one loads.** _reported_
    For about 0.3–0.5 s after `play <other>`, the title is the new video's while `position` and `duration` are still the old one's. From idle it shows 0:00 / 0:00 correctly.

18. **Controls move when the search box takes focus.** _reported, may be intended_
    The box widens and pushes the mode pill and the sort buttons 80 px left.

20. **Autoplay stops at the grid's last video while N wraps to the first.** Probably intended.
