# Backlog — maintained by Claude's weekly planner. Move items between sections to steer it.

## Routine (Claude may build these without asking)

- [ ] User lyric offset adjustment (e.g. ±0.5 s from the control bar or keyboard, applied via the existing `setOffset` path) — no adapter or UI lets the user fix out-of-sync lyrics today; needs a UI addition and a decision on persistence (new config key vs per-track store)
- [x] Add settings-dialog checkboxes for `player.isShowLyricTranslation`, `player.isShowLyricRoma`, `player.isSwapLyricTranslationAndRoma` and `player.isPlayLxlrc` (existing keys, controller already honours them; controller-suite test) — today translated/romanized lines can only be switched by hand-editing config.json
- [x] Translate the hardcoded English strings in the settings dialog ("Desktop Lyric Settings", "Lyric Style", "Opacity", the spacing-reset tooltip) and the gear tooltip — adds app-local i18n keys beyond lx-music's language packs
- [x] Add a language selector (follow system / 简体中文 / 繁體中文 / English) for the existing `common.langId` key to the settings dialog — the UI language can currently only be changed through the system locale or config.json
- [ ] Global shortcuts for lock/unlock — extends the `lx-lyrics-hotkey` fd-3 lines and needs a new host→app message in `docs/protocol.md`
- [ ] Per-word karaoke fill for `lxlrc` lyrics (lx-music parity; `desktopLyric.style.isFontWeightFont` is currently a no-op)
- [ ] Extra sidecar lookup locations (a `lyrics/` subfolder, `Artist - Title.lrc`) — changes the acquisition order that `docs/protocol.md` §5 specifies
- [ ] Online lyric search/download (deferred by design in architecture.md)
- [ ] Add tests for `TranslationManager` (system-locale mapping incl. zh-HK/zh-SG, unsupported `common.langId` falling back to en-us, live switch emitting `languageChanged`, and the three packs keeping identical key sets) — the i18n path has no direct test coverage

## Proposed (needs the owner's OK — move to Routine to approve)



## Ideas for the owner to consider (never built automatically)

- Wayland layer-shell support for reliable always-on-top and click-through without KWin rules (new dependency)
- Drop a `.lrc` file onto the lyrics window to attach it to the playing track
- An mpv adapter (mpv has a C/Lua plugin API, unlike the dropped players)

## Rejected (Claude won't re-propose these)
