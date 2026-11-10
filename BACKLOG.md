# Backlog — maintained by Claude's weekly planner. Move items between sections to steer it.

## Routine (Claude may build these without asking)

- [x] Lyric-offset controls in the settings dialog (−0.5 s / +0.5 s / reset with the current value, driving the existing `LyricController` nudge; app-local i18n keys; controller-suite test) — the nudge is keyboard-only today, with no visible control or hint, so most users never find it
- [ ] Per-word karaoke fill for `lxlrc` lyrics (lx-music parity; `desktopLyric.style.isFontWeightFont` is currently a no-op)
- [ ] Global shortcuts for lock/unlock — extends the `lx-lyrics-hotkey` fd-3 lines and needs a new host→app message in `docs/protocol.md`
- [ ] Extra sidecar lookup locations (a `lyrics/` subfolder, `Artist - Title.lrc`) — changes the acquisition order that `docs/protocol.md` §5 specifies
- [x] Document the in-window keyboard shortcuts (`Ctrl+,` settings, `Ctrl+Up`/`Ctrl+Down`/`Ctrl+0` offset nudge) in `README.md` and `README.zh-CN.md` — neither README mentions them, so the offset fix and the locked-window escape hatch are undiscoverable
- [ ] Online lyric search/download (deferred by design in architecture.md)
- [ ] Add controller-suite tests for `LyricWindow` geometry (`desktopLyric.isLockScreen` drag/resize clamping and the off-screen stored-position rescue in `applyWindowGeometry`) — that code has no test coverage

## Proposed (needs the owner's OK — move to Routine to approve)

- [ ] Clamp `desktopLyric.isLockScreen` to the screen the window is on instead of the primary screen — on multi-monitor setups enabling it drags the window back onto the primary monitor; needs the owner because it changes the ported primary-screen behaviour, which may be deliberate lx-music parity

## Ideas for the owner to consider (never built automatically)

- Wayland layer-shell support for reliable always-on-top and click-through without KWin rules (new dependency)
- Drop a `.lrc` file onto the lyrics window to attach it to the playing track
- An mpv adapter (mpv has a C/Lua plugin API, unlike the dropped players)
- Remember the lyric-offset nudge per track across sessions (today it is session-only by design; would need a new persisted store)

## Rejected (Claude won't re-propose these)
