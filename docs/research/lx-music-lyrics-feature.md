# lx-music Desktop Lyrics — Engineering Reference

Condensed from the research delegation `physical-sapphire-peacock` (2026-08-02), verified against the lx-music-desktop v2.12.2 source tree in `references/`. This document is the reference for the C++ port in `lyrics-app/`.

## 1. Feature overview

The desktop lyrics feature (`desktopLyric`) is a Vue 3 renderer app (`src/renderer-lyric/`) that runs in its own frameless, transparent Electron `BrowserWindow`. It is decoupled from the main window: raw lyric text and playback state arrive over an Electron `MessageChannelMain` port, while a whitelisted subset of settings is synced over IPC. Rendering is done by the `lyric-font-player` engine (LRC line parsing + word-level karaoke fill).

## 2. Renderer-lyric app structure

The app is a single-window Vue 3 application:

- `main.ts` — entry: reads settings, sets language, wires `onSettingChanged`, sends the connect-main-window event, mounts `App`.
- `App.vue` — root: lock/hide classes, control bar, vertical/horizontal lyric views, audio visualizer, 8 resize handles (non-Windows).
- `store/` — `state.ts` (reactive `setting`, `isPlay`, `musicInfo`), `lyric.ts` (raw lyric strings + reactive `lyric` with `lines/text/line/offset/tempOffset`), `action.ts` (setting merge/update IPC, `setMusicInfo`, `setIsPlay`).
- `core/mainWindowChannel.ts` — MessagePort bridge (see §3).
- `core/lyric.ts` — wraps the `Lyric` engine instance; `setLyric/setLyricOffset/setPlaybackRate/play/pause/stop/setVertical`.
- `useApp/` — composables: `useCommon` (lang), `useHoverHide`, `useLyric` (mirrors player display toggles), `usePauseHide`, `useTheme`, `useWindowSize`.
- `components/layout/` — `ControlBar.vue` (close/lock/font±/opacity±/zoom/always-on-top), `LyricHorizontal/`, `LyricVertical/`, `useDrag.js`.
- `components/common/AudioVisualizer.vue` — canvas spectrum bars fed over the port.
- `utils/ipc.ts` — IPC wrappers for this window.
- `assets/styles/` — CSS vars `--color-lyric-unplay/-played/-shadow`; `stroke` mixins render the font outline.

Path aliases (`@lyric`, `@common`, `@root`, `@static`) map into `src/`; webpack config lives in `build-config/renderer-lyric/`.

## 3. MessagePort bridge

The port is created in the main process (`new MessageChannelMain()`), one end sent to the lyric window (`win_lyric_provide_main_window_channel`, port in `event.ports`), the other to the main window (`win_main_process_new_desktop_lyric_client`). Data never round-trips through main.

Actions posted main-window → lyric window:

| Action | Payload / effect |
|---|---|
| `set_info` | music info (`id/singer/name/album`) + lyric strings + `isPlay` + `line` + `played_time`; triggers `setLyric()` |
| `set_lyric` | full lyric strings (`lyric/tlyric/rlyric/lxlyric`); re-parse |
| `set_status` | `isPlay` + `played_time` → `play()`/`pause()` (receiver recomputes the line) |
| `set_offset` | lyric delay offset |
| `set_playbackRate` | playback rate |
| `set_play` / `set_pause` / `set_stop` | play state control |
| `send_analyser_data_array` | `Uint8Array` byte-frequency data (transferred buffer) |

Actions lyric window → main window: `get_info`, `get_status`, `get_analyser_data_array` (pull; the visualizer re-requests inside `requestAnimationFrame` while playing).

## 4. Settings schema (`desktopLyric.*`)

Defaults are authoritative in `src/common/defaultSetting.ts`; the lyric window only ever sees the whitelist in `winLyric/utils.ts` `watchConfigKeys` (all keys below except `fullscreenHide`).

| Key | Default | Type / range |
|---|---|---|
| `desktopLyric.enable` | `false` | boolean |
| `desktopLyric.isLock` | `false` | boolean |
| `desktopLyric.isAlwaysOnTop` | `false` | boolean |
| `desktopLyric.isAlwaysOnTopLoop` | `false` | boolean |
| `desktopLyric.isShowTaskbar` | `false` | boolean |
| `desktopLyric.audioVisualization` | `false` | boolean |
| `desktopLyric.fullscreenHide` | `true` | boolean |
| `desktopLyric.pauseHide` | `true` | boolean |
| `desktopLyric.width` / `height` | `450` / `300` | number |
| `desktopLyric.x` / `y` | `null` | number \| null |
| `desktopLyric.isLockScreen` | `isWin` | boolean |
| `desktopLyric.isDelayScroll` | `true` | boolean |
| `desktopLyric.scrollAlign` | `'center'` | `'top' \| 'center'` |
| `desktopLyric.isHoverHide` | `false` | boolean |
| `desktopLyric.direction` | `'horizontal'` | `'horizontal' \| 'vertical'` |
| `desktopLyric.style.align` | `'center'` | `'left' \| 'center' \| 'right'` |
| `desktopLyric.style.font` | `''` | string |
| `desktopLyric.style.fontSize` | `20` | number |
| `desktopLyric.style.lineGap` | `15` | number |
| `desktopLyric.style.lyricUnplayColor` | `rgba(255,255,255,1)` | rgba string |
| `desktopLyric.style.lyricPlayedColor` | `rgba(7,197,86,1)` | rgba string |
| `desktopLyric.style.lyricShadowColor` | `rgba(0,0,0,0.18)` | rgba string |
| `desktopLyric.style.opacity` | `95` | number (6–100) |
| `desktopLyric.style.ellipsis` | `false` | boolean |
| `desktopLyric.style.isZoomActiveLrc` | `false` | boolean |
| `desktopLyric.style.isFontWeightFont/Line/Extended` | `true` | boolean |

Related `player.*` / `common.*` keys: `player.playbackRate` (1), `player.isPlayLxlrc` (true non-Mac), `player.isShowLyricTranslation` (false), `player.isShowLyricRoma` (false), `player.isSwapLyricTranslationAndRoma` (false), `common.langId` (null/auto).

## 5. Lyric engine (`lyric-font-player`)

- `Lyric` (`index.js`) — facade over `LinePlayer` plus DOM building. Constructor takes raw `lyric`, `extendedLyrics`, `offset`, `rate`, CSS class names, `shadowContent`, `isVertical`, and callbacks `onPlay/onSetLyric/onUpdateLyric`. API: `play(curTime)`, `pause()`, `setOffset(offset)`, `setLyric(lyric, extendedLyrics)`, `setPlaybackRate(rate)`, `setVertical(isVertical)`, `setDisabledAutoPause(autoPause)`.
- `LinePlayer` (`line-player.js`) — parses LRC text and tags (`[ti:] [ar:] [al:] [offset:] [by:]`), builds time-sorted `lines` with attached extended lyrics, incorporates `[offset:]` plus the instance offset via `_performanceTime`, and drives `play/pause/setPlaybackRate/setLyric/setDisabledAutoPause`. Line-mode adds a 60 ms offset.
- `FontPlayer` (`font-player.js`) — per-line DOM: `.line-content` → `.line` → `.font-lrc` word spans + `.extended` blocks (optional `.shadow` layer). `_parseLyric` splits word-level `<start,duration>` tags and animates each word with a Web Animation `backgroundSize` fill. API: `play/pause/finish/reset/setPlaybackRate`.
- `utils.js` — `TimeoutTools`: hybrid `requestAnimationFrame`/80 ms `setTimeout` scheduler used to avoid timer drift for lyric timing.

## 6. Main-process `winLyric` module

- `main.ts` — window creation: frameless, transparent, `hasShadow:false`, `resizable:isWin`, `alwaysOnTop`, `skipTaskbar`, `webPreferences` with `nodeIntegration:true/contextIsolation:false/sandbox:false`; dev URL `http://localhost:9081/lyric.html`, prod `file://…/lyric.html?os=&dark=&theme=`. On `move` Windows snaps back to stored bounds; on `resize` bounds are saved debounced.
- `rendererEvent.ts` — IPC handlers: `win_lyric_set_config` (write-through), `win_lyric_get_config` (whitelisted config), `win_lyric_set_win_bounds`, `win_lyric_request_main_window_channel` (creates the port), `win_lyric_mouse_enter_leave`.
- `config.ts` — `setLrcConfig(keys, setting)`: pushes config change to the window, then imperatively applies `isLock` (`setIgnoreMouseEvents`), `isHoverHide` (forward mouse), `isAlwaysOnTop` (+ loop), `isShowTaskbar`, `isLockScreen` (clamp bounds), `enable` (create/close window), null `x` (recenter).
- `index.ts` — lifecycle: create window on `main_window_inited` when enabled; handle `updated_config`, `main_window_close`, `main_window_fullscreen` (per `fullscreenHide`), and global hotkeys toggling enable/lock/always-on-top.
- `utils.ts` — bounds clamping (min 38×38), `watchConfigKeys` whitelist, `buildLyricConfig`, `initWindowSize`.
- `mouseCheckTools.ts` — 500 ms cursor poll; when locked + `isHoverHide`, hides the window once the cursor leaves bounds.
- Main-window side: `src/renderer/core/lyric.ts` maintains its own `Lyric` instance, answers port requests, and pushes `set_lyric`/`set_play`/`set_pause`/`set_stop`/`set_offset` as playback changes; the audio analyser (`AnalyserNode`, fftSize 256 → 128 bins) is read in the main renderer and posted as `send_analyser_data_array`.

## 7. Settings UI controls

`SettingDesktopLyric.vue` exposes: enable, lock, fullscreen-hide, pause-hide, audio visualization, delay-scroll, always-on-top, show-taskbar, always-on-top-loop, lock-screen, hover-hide, ellipsis, zoom-active-line, font-weight group, direction, scrollAlign, align, lineGap (0–25), three color pickers, system font list, and reset-window-position. The on-window control bar offers close, lock, font ±, opacity ±, zoom-active toggle, and always-on-top toggle.

## 8. Build configuration

- Target `electron-renderer`; entry `src/renderer-lyric/main.ts`; output `dist/` with `lyric.html`; dev server port 9081.
- Loaders: ts-loader, vue-loader, less, svg-sprite-loader; plugins: HTMLPlugin, MiniCssExtractPlugin, Terser.
- The lyric window keeps `nodeIntegration` enabled so it can use `@common/rendererIpc`; the built `dist/lyric.html` is loaded via `file://` from the main process.

## 9. Lyric sources and formats

- Sources dispatch (`getLyricInfo`): online APIs (source-specific `getLyric`), download cache, or embedded file lyrics; cross-source fallback and traditional-Chinese conversion are applied.
- Formats: `lyric` (LRC), `tlyric` (translation), `rlyric` (romaji), `lxlyric` (word-level `<start,duration>` tags); the `[awlrc:base64,…]` container embeds all four in one LRC string and is decoded by the music worker, not by `line-player`.
- Per-source decoders exist for KRC (zlib/XOR) and other proprietary formats; the app consumes the final parsed strings over the port.
