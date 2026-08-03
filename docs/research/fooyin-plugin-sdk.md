# Fooyin Plugin SDK — Reference

Condensed from the research delegation `content-coffee-reindeer` (2026-08-02), compiled from `fooyin/fooyin` (master, v0.12.1), `fooyin/fooyin-plugin-examples`, and `docs.fooyin.org`. Cross-checked in the review delegation `unpleasant-blush-spider` against installed Fooyin headers and CMake config. This is the reference for `fooyin-plugin/`.

## 1. Plugin model and registration

- Plugins are Qt `MODULE` shared libraries loaded with `QPluginLoader`. The plugin class subclasses `QObject` and implements one or more abstract interfaces, declared with `Q_PLUGIN_METADATA` + `Q_INTERFACES`.
- `Fooyin::Plugin` (IID `org.fooyin.fooyin.plugin/1.0`) — required base; single `virtual void shutdown()` hook called just before fooyin closes.
- `Fooyin::CorePlugin` (IID `org.fooyin.fooyin.plugin.core`) — `initialise(const CorePluginContext&)`, called after core init; gives playback/settings/engine access.
- `Fooyin::GuiPlugin` (IID `org.fooyin.fooyin.plugin.gui`) — `initialise(const GuiPluginContext&)`, called after core plugins; gives widgets/actions/themes.
- A widget plugin that needs playback state implements all three (`QObject + Plugin + CorePlugin + GuiPlugin`), exactly like the built-in lyrics plugin.
- `PluginManager` scans plugin directories recursively, accepts any `QLibrary::isLibrary()` file whose metadata contains `MetaData`, and skips plugins listed in the `DisabledPlugins` setting. There is no version check on load; compatibility is by ABI against the fooyin build you link.

## 2. Contexts

`CorePluginContext`: `playerController`, `libraryManager`, `library`, `playlistHandler`, `settingsManager`, `engine`, `audioLoader`, `playlistLoader`, `sortingRegistry`, `networkAccess` (shared `NetworkAccessManager` honoring proxy settings).

`GuiPluginContext`: `actionManager`, `layoutProvider`, `trackSelection`, `searchController`, `playlistSelection`, `propertiesDialog`, `scriptCommandHandler`, `widgetProvider`, `editableLayout`, `windowController`, `themeRegistry`, `styleProvider`, `advancedSettingsRegistry`, `coverRepository`. Fields grow between releases — compile against the same fooyin version you run.

## 3. Playback state — `PlayerController` (`CorePluginContext::playerController`)

Getters: `playState()` (`Player::PlayState` — `Playing|Paused|Stopped`), `currentPosition()` (ms), `currentTimeListened()`, `bitrate()`, `currentTrackSeekable()`, `currentTrack()` (invalid when stopped), `currentTrackId()`, `currentPlaylistTrack()`, `playbackSnapshot()`.

Control: `play()`, `playPause()`, `pause()`, `stop()`, `previous()`, `next()`, `randomTrack()`, `seek(ms)`, `seekForward/seekBackward`, `setPlayMode()`, `startPlayback()`.

Signals (the wiring a lyrics bridge needs):

- `playStateChanged(PlayState, PlayState)` — pause/stop scrolling on Paused/Stopped, resume on Playing
- `currentTrackChanged(const Track&)` — real track switch → reload lyrics
- `currentTrackUpdated(const Track&)` — in-place metadata refresh
- `positionChanged(uint64_t ms)` — fires continuously (~100–200 ms) while playing; drives the karaoke cursor
- `positionMoved(uint64_t)` — after user seek
- also `bitrateChanged`, `playlistTrackChanged/Updated`, `trackPlayed`, `playbackSnapshotChanged`

**Playback rate is NOT exposed** in any public header (only the internal SoundTouch DSP). The bridge should ship rate 1.0 or expose its own setting.

## 4. Track metadata — `Fooyin::Track`

`title()`, `artists()`, `artist()`, `primaryArtist()` (artist→albumartist→composer→performer), `album()`, `albumArtists()`, `trackNumber()`, `genres()`, `date()`, `duration()` (ms), `filepath()`, `uniqueFilepath()`, `isValid()`.

Extra tags: `hasExtraTag(tag)`, `extraTag(tag)` (case-insensitive) — embedded lyrics live here; also `extraTags()`, `metaValue(name)`, `metadata()`.

## 5. Engine — `EngineController` (`CorePluginContext::engine`)

`engineState()` (`Stopped|Playing|Paused|Error`). Audio analysis: `visualisationService()` → `createSession()` returns a pull-based session (`getPcmWindow()` / `getSpectrumWindow()` → `std::vector<float>` magnitudes, `binCount = fftSize/2 + 1`); push-based `levelReady(LevelFrame)` / `pcmReady(PcmFrame)` signals also exist. Fooyin spectrum is float magnitudes — a byte-frequency conversion must be defined; it is not raw-compatible with lx-music's 128-bar `Uint8Array`.

## 6. UI from a plugin

- `FyWidget` (`include/gui/fywidget.h`) — a `QWidget` participating in fooyin layouts; must implement `name()` and `layoutName()`; can override `saveLayoutData/loadLayoutData` and `openConfigDialog()`.
- Register with `WidgetProvider::registerWidget(key, instantiator, displayName)` from `GuiPluginContext::widgetProvider`.
- **Standalone window**: `FyWidget::showStandaloneWindow(title, stateKey, defaultSize)` shows the widget as a self-owned top-level window whose geometry and layout data persist across sessions — the natural host for an embedded desktop-lyrics window. Frameless/always-on-top are plain Qt flags (`Qt::FramelessWindowHint | Qt::WindowStaysOnTopHint`) you set yourself; drag/close handling must be reimplemented.
- `ActionManager` (`GuiPluginContext::actionManager`): `registerAction(QAction*, const Id&, const Context&)`, `createMenu/createMenuBar/command`. Registered actions appear in fooyin's shortcuts settings; main menu ids are in `include/gui/guiconstants.h` (`Menus::…`).
- `TrackSelectionController` adds track context-menu entries; `WindowController` gives `mainWindow()`/`raise()` for dialogs.
- Styling is `QPalette`-based (`GuiStyleProvider::style()`, `ResolvedAppStyle`); there is no user-facing QSS. Register theme fonts via `ThemeRegistry::registerFontEntry()`.

## 7. Settings

- `SettingsManager` (`CorePluginContext::settingsManager`): file-level `fileValue/fileSet/fileContains/fileRemove` (raw `fooyin.conf` access); registered `createSetting(key, default)` plus typed `value(key)`/`set(key, v)`/`subscribe(key, obj, fn)` with change notifications. `subscribe()` is the pattern for live font/color updates.
- `SettingsPage` + `SettingsPageWidget` (`include/utils/settings/settingspage.h`): `setId/setName/setCategory/setRelativePosition/setWidgetCreator`; the widget implements `load()/apply()/reset()`. Construct pages in `GuiPlugin::initialise`; open programmatically via `settingsDialog()->openAtPage(id)`.
- Per-widget config: `ConfigDialog` + `WidgetConfigDialog<WidgetType, ConfigType>` (v0.9+) — see the built-in lyrics widget's `ConfigData`/`LyricsConfigDialog`.
- Plugin-level "Configure" dialog (v0.10+): optional `PluginConfigGuiPlugin` (IID `org.fooyin.fooyin.plugin.gui.config/1.0`) returning a `PluginSettingsProvider`.

## 8. Lyrics internals (built-in plugin — reference only)

- The built-in lyrics plugin (`src/plugins/lyrics/`) is a full widget plugin: `lyricsplugin` (entry), `lyricswidget` (FyWidget), `lyricsview/model/delegate` (custom `QPainter` rendering), `lyricsparser` (LRC), `lyricsfinder/saver`, and sources (`taglyrics`, `locallyrics`, `lrclib`, `netease`, `qq`, `kugou`, `darklyrics`).
- **Its data model and parser are internal, not public headers** — a third-party plugin must reimplement LRC parsing and search, or read raw text itself.
- Internal model: `Lyrics{Type(Unknown|Unsynced|Synced|SyncedWords), data, source, isLocal, tag, filepath, metadata, offset, lines}` where `ParsedLine{timestamp, duration, words}`. There is **no translation or romaji field** — `tlyric`/`rlyric` handling must be added by your plugin.
- Tag source: `Track::extraTag(tag)` over the configured search tags — defaults `LYRICS`, `SYNCEDLYRICS`, `UNSYNCEDLYRICS`, `UNSYNCED LYRICS` (settings key `Lyrics/SearchTags`).
- Local source: sidecar files via `Fooyin::ScriptParser` expansion of the path template `%path%/%filename%.lrc` (default; settings key `Lyrics/Paths`), wildcard file lookup, UTF-8 read.
- Online sources use `NetworkAccessManager` (`context.networkAccess`) with a `getJsonFromReply()` pattern.

## 9. Build and install

- Requirements: C++23, Qt6 6.4+ (6.8+ on Windows), CMake 3.19+, TagLib ≥1.12, FFmpeg ≥4.4, ICU, zlib, one audio backend. Install fooyin with `-DINSTALL_HEADERS=ON` so `find_package(Fooyin)` works.
- `FooyinConfig.cmake` provides targets `Fooyin::Core`, `Fooyin::Gui`, `Fooyin::Utils`, the `create_fooyin_plugin()` macro, and `FOOYIN_PLUGIN_VERSION` / `FOOYIN_PLUGIN_INSTALL_DIR`. Note its `find_package(Qt6 … COMPONENTS Core Widgets Sql Concurrent Network)` does **not** include WebSockets — the plugin must add its own `find_package(Qt6 COMPONENTS WebSockets)`.
- `create_fooyin_plugin(name DEPENDS … SOURCES …)`: builds `add_library(MODULE)`, sets `OUTPUT_NAME fyplugin_<lowercase>`, `PREFIX ""`, hidden visibility, rpath to the plugin dir, configures `<name>.json.in` → `<name>.json` metadata, installs to `FOOYIN_PLUGIN_INSTALL_DIR` (`lib/fooyin/plugins`).
- Install paths scanned at load: `<install>/lib/fooyin/plugins`, `<app>/plugins`, `~/.local/lib/fooyin/plugins/`. Distribution is a single `.so` with embedded metadata JSON (no separate manifest).
- API stability: pre-1.0 and evolving; current release 0.12.1. Build against the same fooyin release you target. Fooyin is GPLv3 — plan plugin licensing accordingly.
