# UX run 1 findings — 2026-10-17

Ten Opus agents, one instance each (six offscreen, four under cage), about 80 commands and up to 20 pictures apiece, on main 3bde5ab. No crashes. Every item was then checked by hand (2026-10-17, later the same day); the status after each title is the outcome. **Fixed in PR #67** (one commit each, re-checked with the harness): items 1–15, 18–22 and 24–30. Left as they were: 16 and 17 (not reproduced), the "by design or IPC-only" items, and the uneven top-of-tab spacing in 26. Agents are given in brackets; _confirmed_ means reproduced by hand afterwards, _confirmed by code_ means the cause is plain in the source, _reported_ means the agent's evidence only. Pictures and the agents' write-ups are under `scratchpad/uxtest/agent-<n>/` (tmpfs).

## Wrong behaviour

1. **N or P after a stop.** _confirmed_ [3, 7]
   `stop()` keeps `queue` and `history`. Music: `stop`, `key N` restarts the album, `key P` pops the pushed -1 and `load(-1)` throws at `Main.qml:330`, leaving the track playing with `playing` null, every control disabled and Space dead; a burst of TypeErrors (ControlBar, PlayerView, ReviewPanel, Main). Video: after Stop after ends a video, N or P starts another whose player layer never reopens (`playerView.close()` on stop, `beginLoad()` never shows it): sound with no picture until `expand`. MPRIS and the tray's Next/Previous take the same path.

2. **A reveal issued while the grid is settling its relayout is undone 60 ms later.** _confirmed_ [8, 9]
   The now-playing art clicked from video mode switches to music and calls `albumGrid.reveal()` as the grid becomes visible; the `settled` timer's `keepRows()` then puts `contentY` back against a shifted `originY`: `albumContentY` -774, the first five albums drawn, an empty sixth cell, the playing album out of view. The panel's Music video button has the same fault on the video grid: the video grows from the fallback rectangle, and after `back` its card is out of view.

3. **The grids' row-keeping breaks in three ways.** _confirmed_ [2, 5, 10]
   - A column change during a re-sort's displaced transition (`album-sort title` then `cover-columns 2` within 0.3 s) leaves cards at the old spacing, overlapping, until the next width change.
   - A narrower facet after a scroll, then a mode round trip: the kept `rowOffset` is restored beyond the content (the agent saw `contentY` 1900 in a 4-row grid: blank), nothing clamps it.
   - A mode switch and a section change back to back leave the video grid with three of four columns filled (`gridColumns` 4 in state).
   - Not reproduced: a fresh instance's album grid laid out at the width without the panel right after `mode music; play-album 0` [6].

4. **Clicking outside a text field does not release the keyboard focus.** _confirmed_ [2, 5, 6]
   After Ctrl+F, a click on shuffle, the heading, empty sidebar space or the control bar leaves `focus` at the TextInput, so Space types a space, N, P, M, F and / are dead, and Ctrl+F, / and Ctrl+M (gated by `!window.typing`) do nothing. `releaseTextFocus()` forces focus onto `contentItem`, the root scope, which hands it straight back; the PointHandler never sees presses that MouseAreas accept. The same from the sidebar filter: Ctrl+F types into the filter.

5. **The sidebar filter survives a switch of the tag selector.** _confirmed_ [2]
   Artists → type "nana" → Years: the box keeps "nana" and the list says "No match". `shownKey` is a binding on `App.sidebarFacet`, already equal when `onAppearanceChanged` compares, so `swap()` (clear, cross-fade, scroll reset) never runs.

6. **Three TypeErrors on every stop of a track.** _confirmed_ [3, 5, 9]
   `NowPlayingPanel.qml:273`, `:58`, `:267`: `track` goes null before `hasTrack` re-evaluates. Console only.

7. **An orphan verdict leaves the player's queue stale.** _confirmed by code_ [10]
   `approveVideo`/`rejectVideo` never touch `player.queue`: after deleting the playing orphan, N loads the deleted file (mpv "No such file", image warnings), and Autoplay then plays a kept video that is in no grid.

8. **Re-ticking a folder never folds back into the base.** _confirmed by code_ [1]
   `FolderTreeModel::withChecked(on=true)` only appends: untick Aria, re-tick it → the root stays part-ticked, `musicDirs` is 23 sub-folders, and an album folder added under the base later is not looked up.

9. **Non-modal popups ignore Escape and block every shortcut while open.** _confirmed by code_ [1, 4, 6, 10]
   The sort menu, the import panel, the sidebar's tag selector and the source popup have CloseOnEscape without `focus: true`; while one is open Qt also swallows the window's shortcuts (F12, Ctrl+Q did nothing). The import pill's tooltip draws over the open sort menu.

10. **A video-only facet is carried into music.** _confirmed_ [5]
    Album artist "sato noco" in video mode, Ctrl+M: "sato noco / 0 albums", "Nothing here yet.", nothing selected in the music sidebar. `Sidebar.select()` sets the facet on both libraries.

11. **Entering Orphans stops a playing music track.** _confirmed_ [5] (by design for videos; a track need not stop)

12. **A seek sent right after an album starts is lost.** _confirmed_ [9] (`play-album`, `play-mode`, `seek 100` back to back: position 2.8 s three seconds later)

13. **Shuffle and the disabled Next.** _confirmed_ (the shuffle part: a two-track album, both played, N goes back to the first) [3]
    With shuffle on, once every track has played N stays inside the album although the album's end would go on to the next; N restarts the album after Stop after has ended it, and on the last album reloads the track while the button is greyed.

14. **The Music video fallback restarts the track from 0:00** instead of where it was. _confirmed_ (1:01 before, 0:04 after) [3]

15. **"Re-import N videos" in the Orphans sidebar menu does nothing** (orphans have no tracks; no feedback, counts unchanged). _confirmed_ [10]

## Visual

16. **The old video's last frame shows through the new card as it grows** (about 220 ms, the poster fading in from 0 over the paused frame). _reported, not reproduced_: my first-frame pictures were inconclusive; the control bar showing the old length under the new title is the pilot's "do not fix" item [7]
17. **A 4:3 video playing in its card half-covers the badges** ("LOSS"). _reported, not reproduced_: a 640×480 video (row 13) in its card at 1280 wide left both badges readable [7]
18. **A tooltip is off-centre after its text changes on click** (play mode, and by code shuffle, play/pause, mute, panel). _confirmed_ ("Repeat this album" 43 px left of the button, "Stop after this album" 8 px right) [3]
19. **At 960×600 in the Review section the tools spill over the review panel** and the heading is gone: the 140 px search floor exceeds the 393 px left. _confirmed_ [4]
20. **The empty Review section reads "No videos match / Nothing here yet."** with a search icon; the panel says "Nothing waits for review." _confirmed_ [4, 10]
21. **A section switch keeps the other section's scroll offset**: the library opens with its first row clipped; back in Orphans the verdict buttons sit under the top bar. _confirmed by code_ (`rowOffset` is not reset on a section change; at 1440×900 the Orphans grid has nothing to scroll, so not seen by hand) [10]
22. **In Recently Added the dimmed direction button shows the stored sort's arrow**, not newest first. _confirmed_ (arrow up, `sortDescending` false, the list newest first) [2, 4]
23. **The size sliders say "6 across" / "8 across" while the grid caps at 4 or 5** (the 200/150 px card minimum). _confirmed by state_ (`coverColumns` 8, `albumColumns` 5 at 1440×900 with the panel) [2, 4, 7]
24. **Card captions cut the year and leave a stray "·"** ("Nanatsukaze, PIKASONIC, nakotanmaru · 20…" at 4 across). _confirmed_ [4]
25. **A CJK title shifts the lines in the panel and the control bar** (cards were fixed, these were not). _confirmed_ (panel title 4 px higher and its artist line 3 px higher than "Android"'s; the bar's artist line 2 px lower) [3]
26. **Settings:** the folder tree's last row is cut through its check box (320 px cap, also at 1100×1300); tight spacing after two-line hints (8 px against 30); "Select base directory" wording once a base is set. _confirmed_; the uneven top-of-tab spacing _reported_ [1]
27. **Up/Down while muted change the volume with no feedback**; a drag unmutes, the keys do not. _confirmed_ (0.30 → 0.35, still muted) [8]
28. **A whitespace-only search looks empty but stays active** (wide box, clear button, tools shifted). _confirmed by code_ (`implicitWidth` keys on `text.length > 0`; the filter itself trims, 23 albums shown) [6]
29. **The control bar's art does nothing in music mode while a video plays**, with a pointing cursor. _confirmed_ [9]
30. **After a verdict the next card's buttons land under the pointer**; a second click deletes a second video. _confirmed_ (the keep tooltip shows again over the next card right after the click) [10]

## By design, or IPC-only

- A resize that changes the column count keeps the row number, not the albums on show [2].
- Hovering the playing album shows Play and clicking restarts it [3].
- Autoplay stops at the grid's last video while N wraps [7] (the pilot's "do not fix").
- `channel-dialog` opens for any video; `lookup-album` adds an album already in scope; the album menu stays open over the video grid after an IPC mode change [5, 10].

## Not bugs

- Shape icons escaping the grid's and the settings panel's clips in pictures [1, 4]: offscreen renderer only; clean under cage with the same scroll.
- The transparent window background in `grab-window` pictures: harness (fixed below).

## Harness, before the next run

- `grab-window` composites onto the window's colour (done in the next IPC change).
- Imports are not paused at start (`MVPLAYER_PAUSE_SECS` applies after a block); the stand-in yt-dlp's answer pauses them for an hour at the first job.
- `scroll`/`wheel`/`sort`/`order`/`facet` act on the video model only; the album grid can only be scrolled by `reveal-album` or `drag`; `state` lacks the album facet value, the search text, the import panel and the sidebar menu; `order` has no ids.
- The Write tool refused two agents' findings files ("report files"); the brief should ask for the report in the final message and let the orchestrator save it.
- Keys sent right after closing Settings land during its fade-out; agents should wait 0.5 s.
- The 80-command budget was the limit for most agents; a command that returns state with its reply would stretch it.
