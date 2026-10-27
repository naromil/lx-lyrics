# Backlog — maintained by Claude's weekly planner. Move items between sections to steer it.

## Routine (Claude may build these without asking)

- [x] Read ID3v2 `SYLT` synced-lyrics frames in `TrackLyrics` when no `LYRICS`/USLT property yields text — TagLib's `PropertyMap` skips SYLT, so MP3s tagged with synced lyrics only show "No lyrics" (millisecond timestamps only; MPEG-frame timestamps skipped with a warning; feed-suite test with a TagLib-built fixture)
- [ ] Make `ColorPickerButton` reuse `DesktopLyricConfig::parseRgbaString`/`rgbaStringFromColor` and add config-suite edge cases (malformed, out-of-range, named colours) — the picker carries a verbatim copy of both helpers that can silently drift from the config's format
- [ ] In the audacious plugin: the text in the setting is too long in one line, and the setting window auto adjust length to the text, making the window too long.
- [ ] Translate the hardcoded English strings in the settings dialog ("Desktop Lyric Settings", "Lyric Style", "Opacity", the spacing-reset tooltip) and the gear tooltip — adds app-local i18n keys beyond lx-music's language packs
- [ ] Global shortcuts for lock/unlock — extends the `lx-lyrics-hotkey` fd-3 lines and needs a new host→app message in `docs/protocol.md`

## Proposed (needs the owner's OK — move to Routine to approve)

- [ ] User lyric offset adjustment (e.g. ±0.5 s from the control bar or keyboard, applied via the existing `setOffset` path) — no adapter or UI lets the user fix out-of-sync lyrics today; needs a UI addition and a decision on persistence (new config key vs per-track store)
- [ ] Fix the seven pre-existing clang-tidy findings (`lyricselector.cpp` use-std-move ×2, `pipeio.cpp` ×2, `lyricrenderer.cpp` ×3 signed-bitwise) — AGENTS.md says not to edit those files just to silence the gate
- [ ] Pin the clang-format version used by `tools/lint.sh` — clang-format 18 reports 3 violations on main (`lrcparser.cpp:47`, `orphansweep.cpp:91,190`) that a newer version accepts, so the gate's result depends on the toolchain
- [ ] Extra sidecar lookup locations (a `lyrics/` subfolder, `Artist - Title.lrc`) — changes the acquisition order that `docs/protocol.md` §5 specifies

## Ideas for the owner to consider (never built automatically)

- Per-word karaoke fill for `lxlrc` lyrics (lx-music parity; `desktopLyric.style.isFontWeightFont` is currently a no-op)
- Online lyric search/download (deferred by design in architecture.md)
- Wayland layer-shell support for reliable always-on-top and click-through without KWin rules (new dependency)
- Drop a `.lrc` file onto the lyrics window to attach it to the playing track
- An mpv adapter (mpv has a C/Lua plugin API, unlike the dropped players)

## Rejected (Claude won't re-propose these)
