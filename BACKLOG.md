# Backlog — maintained by Claude's weekly planner. Move items between sections to steer it.

## Routine (Claude may build these without asking)

- [ ] Extra sidecar lookup locations (a `lyrics/` subfolder, `Artist - Title.lrc`) — changes the acquisition order that `docs/protocol.md` §5 specifies
- [ ] Global shortcuts for lock/unlock — extends the `lx-lyrics-hotkey` fd-3 lines and needs a new host→app message in `docs/protocol.md`
- [x] Translate the "No lyrics" placeholder in `LyricController::showNoLyricsPlaceholder()` (app-local i18n key in the three packs; controller-suite test) — it is a hardcoded English literal, so zh-CN/zh-TW users see English whenever a track has no lyrics
- [ ] Online lyric search/download (deferred by design in architecture.md)
- [x] Extend `tools/lint.sh`'s clang-format pass to the C/C++ adapter sources (`plugins/deadbeef`, `plugins/audacious`, `plugins/vlc`, `plugins/fooyin/src/hotkey.c`) — they are kept format-clean only by hand today (and pass), so drift goes unnoticed

## Proposed (needs the owner's OK — move to Routine to approve)

- [ ] Clamp `desktopLyric.isLockScreen` to the screen the window is on instead of the primary screen — on multi-monitor setups enabling it drags the window back onto the primary monitor; needs the owner because it changes the ported primary-screen behaviour, which may be deliberate lx-music parity
- [ ] System-tray icon with Lock/Unlock, Settings and Show/Hide — a locked click-through window can only be reached via `Ctrl+,` once it has focus; needs the owner because it adds a new UI surface lx-music's lyric window does not have
- [ ] Python style check (e.g. `ruff`) for the Rhythmbox and Quod Libet adapters in `tools/lint.sh` — needs the owner because it adds a new tool dependency to the gate AGENTS.md defines as C++-only

## Ideas for the owner to consider (never built automatically)

- Wayland layer-shell support for reliable always-on-top and click-through without KWin rules (new dependency)
- Drop a `.lrc` file onto the lyrics window to attach it to the playing track
- An mpv adapter (mpv has a C/Lua plugin API, unlike the dropped players)
- Remember the lyric-offset nudge per track across sessions (today it is session-only by design; would need a new persisted store)
- Two-line alternating layout (current and next line staggered left/right, as OSD Lyrics and Kugou-style desktop lyrics offer)
- Previous / play-pause / next buttons on the control bar (would need new app→host actions in the protocol and every adapter)

## Rejected (Claude won't re-propose these)
