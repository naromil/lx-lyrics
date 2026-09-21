# SPDX-License-Identifier: GPL-3.0-only
# Copyright (c) 2026 naromil
#
# Quod Libet adapter: the player half of the lx-lyrics player feed.
#
# It observes the Quod Libet player through the event-plugin API, spawns the display app
# as a direct child and streams newline-delimited v2 JSON to the child's stdin.
# Acquisition, parsing and rendering all belong to the app; Quod Libet exposes no
# analyser to plugins, so the handshake declares `spectrum: false`.

import json
import os

import gi

# Quod Libet runs on GTK 3 (its own `qltk` pins Gtk 3.0), so the adapter pins the
# namespaces it touches with it — Gdk included, or `from gi.repository import Gdk` would
# load Gdk 4.0 first and Gtk 3.0 could not follow. pygobject requires the pins to run
# before the import, so it cannot be hoisted to the top of the file.
gi.require_version("Gdk", "3.0")
gi.require_version("Gtk", "3.0")
from gi.repository import Gdk, Gio, GLib, Gtk  # noqa: E402

from quodlibet import app  # noqa: E402
from quodlibet.plugins import PluginConfigMixin, PluginManager  # noqa: E402
from quodlibet.plugins.events import EventPlugin  # noqa: E402
from quodlibet.qltk import Icons  # noqa: E402
from quodlibet.util.dprint import print_d, print_e, print_w  # noqa: E402

PROTOCOL_VERSION = 2
HOST_NAME = "quodlibet"
APP_ARGUMENT = "--player-feed"

# One `set_status` per 500 ms of playback (protocol §5). Quod Libet has no position
# signal to hang that cadence on — `get_position()` is a plain getter, and the only
# periodic source it ships (`quodlibet.qltk.tracker.TimeTracker`) ticks once a second —
# so the session polls with a GLib timeout, the mechanism the in-tree synchronizedlyrics
# plugin uses for its own position-driven updates.
STATUS_INTERVAL_MS = 500
# A jump of a second or more between two polls cannot be normal progress, so it is read
# as a seek the player did not report (`plugin_on_seek` covers the ones it does).
SEEK_THRESHOLD_MS = 1000

# How long a child gets to exit on its own after its stdin is closed (protocol §2) before
# it is killed: the app flushes its debounced settings on the way out.
STOP_GRACE_MS = 1000

# Lines queued for a child that is not draining. Past this the oldest stale lines are
# dropped: the newest `set_info`/`set_play`/`set_status` supersede them, and a stalled app
# must not grow Quod Libet's memory.
MAX_PENDING_LINES = 64

# Protocol §2/§7: a line over 1 MiB is a protocol error. GLib's line reader reports the
# byte length without the newline, which is the same count the app and the other adapters
# cap on.
MAX_LINE_BYTES = 1024 * 1024

# The plugin's one setting, stored by Quod Libet itself: `PluginConfigMixin` derives the
# option name from PLUGIN_ID, so this lands in `[plugins] lxlyrics_app_path` of
# `$XDG_CONFIG_HOME/quodlibet/config`.
APP_PATH_OPTION = "app_path"
DEFAULT_APP_PATH = "lx-lyrics-app"


def _position_ms(player):
    """The player's position in milliseconds.

    `BasePlayer.get_position()` is documented as milliseconds (`player/_base.py`) and the
    GStreamer backend repeats it, so no unit conversion happens here. A backend that
    cannot answer — a C-backed getter failing as `GLib.Error`, an unimplemented or absent
    method — must never raise into Quod Libet's event loop.
    """
    try:
        position = player.get_position()
    except (AttributeError, GLib.Error, NotImplementedError, TypeError, ValueError):
        return 0
    return max(0, int(position))


def _is_playing(player):
    """Quod Libet players carry the pause flag as `paused`; missing means not playing."""
    return not getattr(player, "paused", True)


def _is_fullscreen(window):
    """The main window's fullscreen state, the same query Quod Libet makes itself.

    An unrealized window has no Gdk window yet, which is not fullscreen (`qltk/window.py`
    reads it exactly this way before toggling).
    """
    gdk_window = window.get_window() if window is not None else None
    if gdk_window is None:
        return False
    return bool(gdk_window.get_state() & Gdk.WindowState.FULLSCREEN)


def _song_path(song):
    """Local filesystem path of the playing file, empty for a stream.

    Quod Libet marks real local files with `AudioFile.is_file`; a stream is a
    `RemoteFile`, whose `~filename` holds the URI rather than a path.
    """
    if song is None or not getattr(song, "is_file", False):
        return ""
    return song("~filename") or ""


def _song_text(song, key):
    """A display string from the song's tags, empty when the song has none."""
    if song is None:
        return ""
    return song.comma(key) or ""


def _info_line(path, singer, name, album, is_play, played_time_ms):
    """A `set_info` line. The host sends no lyrics: the app reads the file at `path`."""
    return {
        "v": PROTOCOL_VERSION,
        "action": "set_info",
        "path": path,
        "singer": singer,
        "name": name,
        "album": album,
        "lrc": "",
        "tlrc": "",
        "rlrc": "",
        "lxlrc": "",
        "isPlay": bool(is_play),
        "played_time": int(played_time_ms),
    }


def _state_line(is_play, played_time_ms):
    """The `set_play`/`set_pause` half of a snapshot.

    `set_info` carries no playback state into the app's lyric timer — only these two
    actions (and `set_status`) do — so a snapshot is always info plus one of them.
    """
    if is_play:
        return {"v": PROTOCOL_VERSION, "action": "set_play", "time": int(played_time_ms)}
    return {"v": PROTOCOL_VERSION, "action": "set_pause"}


def _safe_text(value):
    """`value` as UTF-8-encodable text: song tags can carry lone surrogates.

    Quod Libet decodes file names with `surrogateescape`, so text taken from a song may
    hold characters that no UTF-8 encoder accepts.
    """
    if not isinstance(value, str):
        value = str(value)
    return value.encode("utf-8", "replace").decode("utf-8")


def _line_bytes(message):
    """One protocol line as UTF-8 bytes, replacing what UTF-8 cannot carry.

    The wire is UTF-8 (protocol §6), so an unencodable character is replaced — loudly,
    naming the track — rather than raising inside an event hook and losing the update.
    """
    line = json.dumps(message, ensure_ascii=False, separators=(",", ":")) + "\n"
    try:
        return line.encode("utf-8")
    except UnicodeEncodeError:
        label = message.get("name") or message.get("path") or message.get("action") or "?"
        print_w("lxlyrics: replacing unencodable characters for %s" % _safe_text(label))
        return line.encode("utf-8", "replace")


class FeedWriter:
    """The app process and the host end of the feed.

    GLib allows only one async write per stream: a second one fails with
    `G_IO_ERROR_PENDING`, so writes are queued here and pumped one at a time, which is
    also what keeps `hello` ahead of the first snapshot. Reads are armed one at a time
    for the same reason, so neither direction ever blocks Quod Libet's main loop.

    The child's stdin is non-blocking, so a child that stops draining leaves the pending
    write waiting for `POLLOUT` instead of parking the main loop, and the queue is capped
    at `MAX_PENDING_LINES` so such a child cannot grow the plugin's memory either.
    """

    def __init__(self, app_path, on_line, on_exit):
        self.app_path = app_path
        self.on_line = on_line
        self.on_exit = on_exit
        self.proc = None
        self.stdin = None
        self.stdout = None
        self.pending = []
        self.writing = False
        self.child_exited = False
        self.stalled = False

    def start(self):
        """Spawn the app and write the handshake. Raises GLib.Error if it cannot run.

        The argv array is passed to GLib directly (never a shell, protocol §8), and a
        relative `app_path` is resolved against `$PATH`; the child's stderr is left
        inherited, so the app's own logs land in Quod Libet's console.
        """
        flags = Gio.SubprocessFlags.STDIN_PIPE | Gio.SubprocessFlags.STDOUT_PIPE
        self.proc = Gio.Subprocess.new([self.app_path, APP_ARGUMENT], flags)
        self.stdin = self.proc.get_stdin_pipe()
        # A blocking pipe parks the main loop inside `write_bytes_async` as soon as the app
        # stops draining; with a non-blocking fd GIO's pollable path waits for POLLOUT.
        os.set_blocking(self.stdin.get_fd(), False)
        self.stdout = Gio.DataInputStream.new(self.proc.get_stdout_pipe())
        self.send({"v": PROTOCOL_VERSION, "action": "hello", "host": HOST_NAME, "spectrum": False})
        self.read_line()

    def send(self, message):
        if self.stdin is None:
            return
        self.pending.append(_line_bytes(message))
        overflow = len(self.pending) - MAX_PENDING_LINES
        if overflow > 0:
            # Never drop the line being written (its remaining bytes are tracked here) or
            # the handshake that leads the queue — the app rejects anything before `hello`.
            first = 1 if self.writing else 0
            del self.pending[first : first + overflow]
            if not self.stalled:
                self.stalled = True
                print_w("lxlyrics: the app is not reading; dropping stale lines")
        self._write_next()

    def read_line(self):
        if self.stdout is None:
            return
        self.stdout.read_line_async(GLib.PRIORITY_DEFAULT, None, self._line_read)

    def stop(self, force=False):
        """End the feed and drop the pipes.

        The child is told to go the protocol's way — its stdin is closed, which is EOF and
        makes the app quit immediately (§2), flushing its debounced settings — and it is
        force-killed only if it has not exited within `STOP_GRACE_MS`. `force` skips the
        grace period, for a child that is already gone or has broken the protocol.
        """
        proc, self.proc = self.proc, None
        stdin, self.stdin = self.stdin, None
        self.stdout = None
        self.pending = []
        self.on_line = None
        self.on_exit = None
        if proc is None:
            return
        if stdin is not None:
            try:
                stdin.close()
            except GLib.Error as err:
                # G_IO_ERROR_PENDING means an async write is still armed because the app
                # stopped draining; GLib refuses to close such a stream. That is not a
                # failure — the pipe goes away with the child, which the grace window below
                # kills anyway — so only a real close error is worth reporting.
                if not err.matches(Gio.io_error_quark(), Gio.IOErrorEnum.PENDING):
                    print_e("lxlyrics: closing the app's stdin failed: %s" % err)
        if force:
            proc.force_exit()
            return
        # The wait completes when the child is gone; until then the grace window runs.
        self.child_exited = False
        proc.wait_async(None, self._child_exited)
        GLib.timeout_add(STOP_GRACE_MS, self._kill_lingering, proc)

    def _child_exited(self, _proc, _result):
        """The child exited inside its grace window: nothing left to kill."""
        self.child_exited = True

    def _kill_lingering(self, proc):
        """A child that ignored its stdin EOF gets killed, once."""
        if not self.child_exited:
            proc.force_exit()
        return GLib.SOURCE_REMOVE

    def detach(self):
        """Stop notifying, but let an app that is closing itself exit on its own."""
        self.on_line = None
        self.on_exit = None

    def _write_next(self):
        if self.writing or not self.pending or self.stdin is None:
            return
        self.writing = True
        self.stdin.write_bytes_async(
            GLib.Bytes.new(self.pending[0]),
            GLib.PRIORITY_DEFAULT,
            None,
            self._write_done,
        )

    def _write_done(self, stream, result):
        self.writing = False
        try:
            written = stream.write_bytes_finish(result)
        except GLib.Error as err:
            if self.stdin is not None:
                # The pipe is gone, so the child is too: end the session (which kills it)
                # rather than leaving a half-dead session with the plugin still enabled.
                print_e("lxlyrics: writing to the app failed: %s" % err)
                self._finished()
            return
        if self.pending:
            if written < len(self.pending[0]):
                # A full pipe takes part of a line: the rest stays at the head, so two
                # lines can never interleave.
                self.pending[0] = self.pending[0][written:]
            else:
                self.pending.pop(0)
        if not self.pending:
            self.stalled = False
        self._write_next()

    def _line_read(self, stream, result):
        try:
            line, length = stream.read_line_finish_utf8(result)
        except GLib.Error as err:
            print_e("lxlyrics: reading from the app failed: %s" % err)
            self._finished()
            return
        if line is None:
            self._finished()
            return
        # §7: the cap is checked before the line is interpreted.
        size = length or len(line.encode("utf-8", "replace"))
        if size > MAX_LINE_BYTES:
            self.fail("app→host line is %d bytes (over the %d cap)" % (size, MAX_LINE_BYTES))
            return
        if self.on_line is not None:
            self.on_line(line)
        self.read_line()

    def _finished(self):
        """The child is gone: drop the pipes and report it exactly once."""
        on_exit, self.on_exit = self.on_exit, None
        self.stop(force=True)
        if on_exit is not None:
            on_exit()

    def fail(self, reason):
        """A protocol error from the child (§7): log loudly, end the session, no respawn."""
        print_e("lxlyrics: protocol error from the app: %s" % reason)
        self._finished()


class LyricsSession:
    """Maps Quod Libet playback events onto v2 feed lines."""

    def __init__(self, player, app_path, on_close_requested, on_exit):
        self.player = player
        self.app_path = app_path
        self.on_close_requested = on_close_requested
        self.on_exit = on_exit
        self.feed = None
        self.tick_id = None
        self.window = None
        self.window_signal_id = None
        self.last_position_ms = None
        self.last_fullscreen = None

    def start(self):
        """Spawn the app and start streaming. False if the app cannot be run."""
        feed = FeedWriter(self.app_path, self._app_line, self._app_exited)
        try:
            feed.start()
        except GLib.Error as err:
            print_e("lxlyrics: cannot run %s: %s" % (self.app_path, err))
            return False
        self.feed = feed
        self.tick_id = GLib.timeout_add(STATUS_INTERVAL_MS, self._tick)
        # `hello` is already queued, so the app reads the snapshot after the handshake.
        self._push_info(self.player.song, _position_ms(self.player))
        self._watch_fullscreen()
        return True

    def stop(self, kill=True):
        if self.tick_id is not None:
            GLib.source_remove(self.tick_id)
            self.tick_id = None
        self._unwatch_fullscreen()
        self.player = None
        feed, self.feed = self.feed, None
        if feed is None:
            return
        if kill:
            feed.stop()
        else:
            # The app is closing the window itself (§4): let it exit on its own.
            feed.detach()

    def _send(self, message):
        if self.feed is not None:
            self.feed.send(message)

    def _app_line(self, line):
        line = line.strip()
        if not line:
            return
        try:
            message = json.loads(line)
        except ValueError:
            # stdout carries protocol lines only; stray non-JSON output is skipped, loudly
            # (a library writing to stdout must not break the feed).
            print_w("lxlyrics: ignoring non-JSON line from the app: %s" % line)
            return
        if not isinstance(message, dict) or message.get("v") != PROTOCOL_VERSION:
            # §9: a message with an unsupported `v` is a protocol error, never half-accepted.
            self._app_protocol_error("unsupported message %s" % line)
            return
        action = message.get("action")
        if action == "close_requested":
            self.on_close_requested()
        else:
            # §7: an unknown action ends the session. That covers
            # `get_analyser_data_array`, which §4 forbids from a host that declared
            # `spectrum: false`.
            self._app_protocol_error("unknown app action %r" % (action,))

    def _app_protocol_error(self, reason):
        """§7: log loudly and end the session — no respawn, no stale plugin state."""
        if self.feed is not None:
            self.feed.fail(reason)

    def _app_exited(self):
        print_d("lxlyrics: the app exited")
        self.on_exit()

    def _push_info(self, song, position_ms):
        """A full `set_info` + state snapshot at `position_ms`."""
        self.last_position_ms = None
        if song is None:
            self._send(_info_line("", "", "", "", False, 0))
            self._send({"v": PROTOCOL_VERSION, "action": "set_stop"})
            return
        is_play = _is_playing(self.player)
        self._send(
            _info_line(
                _song_path(song),
                _song_text(song, "artist"),
                _song_text(song, "title"),
                _song_text(song, "album"),
                is_play,
                position_ms,
            )
        )
        self._send(_state_line(is_play, position_ms))

    def song_started(self, song):
        """A new track (or nothing at all) started.

        Quod Libet clears its player's current song and rebuilds the pipeline before it
        announces the new one, so the position it would report here can still belong to
        the previous track (the same stale-position hazard the in-tree synchronizedlyrics
        plugin works around with a 5 ms delay). A track starts at 0: the 500 ms poll
        re-anchors the app to the live position on its next tick, and a restored start
        position arrives as the player's own `seek` signal.
        """
        self._push_info(song, 0)

    def song_ended(self, _song, stopped):
        """`stopped` marks playback ending, not a gapless move to the next song."""
        if stopped:
            self._send({"v": PROTOCOL_VERSION, "action": "set_stop"})

    def paused(self):
        self.last_position_ms = None
        self._send({"v": PROTOCOL_VERSION, "action": "set_pause"})

    def unpaused(self):
        self.last_position_ms = None
        self._send(
            {
                "v": PROTOCOL_VERSION,
                "action": "set_play",
                "time": _position_ms(self.player),
            }
        )

    def seeked(self, msec):
        """`plugin_on_seek` reports the player's own seeks, in milliseconds."""
        msec = int(msec)
        self.last_position_ms = msec
        self._send({"v": PROTOCOL_VERSION, "action": "set_play", "time": msec})

    def _tick(self):
        """One `set_status` per `STATUS_INTERVAL_MS` of playback (protocol §5).

        Nothing is sent while paused: the pause/unpause hooks push that state, and the
        app has no use for a stream of unchanged positions.
        """
        if self.feed is None:
            return GLib.SOURCE_REMOVE
        if not _is_playing(self.player):
            return GLib.SOURCE_CONTINUE
        position = _position_ms(self.player)
        previous, self.last_position_ms = self.last_position_ms, position
        if previous is not None and abs(position - previous) >= SEEK_THRESHOLD_MS:
            # A jump of a second or more in the elapsed clock cannot be normal progress,
            # so it is read as a seek the player did not report.
            self._send({"v": PROTOCOL_VERSION, "action": "set_play", "time": position})
        else:
            self._send(
                {
                    "v": PROTOCOL_VERSION,
                    "action": "set_status",
                    "isPlay": True,
                    "played_time": position,
                }
            )
        return GLib.SOURCE_CONTINUE

    def _watch_fullscreen(self):
        """`set_fullscreen` mirrors the main window's fullscreen state (§5)."""
        window = getattr(app, "window", None)
        if window is None:
            return
        self.window = window
        self.window_signal_id = window.connect("window-state-event", self._window_state_changed)
        self._send_fullscreen(_is_fullscreen(window))

    def _unwatch_fullscreen(self):
        window, self.window = self.window, None
        signal_id, self.window_signal_id = self.window_signal_id, None
        if window is not None and signal_id is not None:
            window.disconnect(signal_id)

    def _window_state_changed(self, _window, event):
        self._send_fullscreen(bool(event.new_window_state & Gdk.WindowState.FULLSCREEN))

    def _send_fullscreen(self, is_fullscreen):
        if is_fullscreen == self.last_fullscreen:
            return
        self.last_fullscreen = is_fullscreen
        self._send(
            {
                "v": PROTOCOL_VERSION,
                "action": "set_fullscreen",
                "isFullscreen": bool(is_fullscreen),
            }
        )


class LxLyrics(EventPlugin, PluginConfigMixin):
    """The event plugin: Quod Libet's own plugin toggle owns the one lyrics session.

    Enabling the plugin in *File → Plugins* starts the session and disabling it ends
    it, because an event plugin has no menu action of its own to hang a second toggle
    on (Quod Libet's UI plugin hooks are sidebars and song context menus only). When the
    user closes the lyric window the plugin turns itself off through the plugin manager
    — the same call the Plugins window's toggle makes — so nothing respawns and the user
    re-enables it from that window.
    """

    PLUGIN_ID = "lxlyrics"
    PLUGIN_NAME = "LX Lyrics"
    PLUGIN_DESC = (
        "Shows the standalone lx-lyrics desktop lyrics window for the track Quod Libet "
        "is playing."
    )
    PLUGIN_ICON = Icons.FORMAT_JUSTIFY_FILL

    def __init__(self):
        super().__init__()
        self.session = None

    @classmethod
    def PluginPreferences(cls, window):
        """Preferences pane: the executable the adapter spawns."""
        box = Gtk.Box(orientation=Gtk.Orientation.VERTICAL, spacing=6)
        box.set_border_width(6)

        label = Gtk.Label(label="Lyrics app executable")
        label.set_alignment(0.0, 0.5)

        entry = Gtk.Entry()
        entry.set_text(cls.config_get(APP_PATH_OPTION, DEFAULT_APP_PATH))
        entry.connect("changed", cls._app_path_changed)

        hint = Gtk.Label(label="The app is started as <executable> %s." % APP_ARGUMENT)
        hint.set_alignment(0.0, 0.5)

        box.pack_start(label, False, True, 0)
        box.pack_start(entry, False, True, 0)
        box.pack_start(hint, False, True, 0)
        return box

    @classmethod
    def _app_path_changed(cls, entry):
        path = entry.get_text().strip()
        if path and path != cls.config_get(APP_PATH_OPTION, DEFAULT_APP_PATH):
            cls.config_set(APP_PATH_OPTION, path)

    def enabled(self):
        """Quod Libet enabled (or restored) the plugin: start the lyrics session."""
        self._start_session()

    def disabled(self):
        """Quod Libet disabled the plugin or is quitting: end the session."""
        self._stop_session()

    def plugin_on_song_started(self, song):
        if self.session is not None:
            self.session.song_started(song)

    def plugin_on_song_ended(self, song, stopped):
        if self.session is not None:
            self.session.song_ended(song, stopped)

    def plugin_on_paused(self):
        if self.session is not None:
            self.session.paused()

    def plugin_on_unpaused(self):
        if self.session is not None:
            self.session.unpaused()

    def plugin_on_seek(self, song, msec):
        if self.session is not None:
            self.session.seeked(msec)

    def _start_session(self):
        if self.session is not None:
            return
        session = LyricsSession(
            app.player,
            self.config_get(APP_PATH_OPTION, DEFAULT_APP_PATH),
            on_close_requested=self._on_close_requested,
            on_exit=self._on_app_exited,
        )
        if session.start():
            self.session = session
        else:
            # The app could not be run: leave no enabled-but-dead toggle behind. The
            # disable is deferred to an idle callback because this runs inside
            # `PluginManager.enable(..., True)`, whose tail would re-register us.
            GLib.idle_add(self._disable_plugin)

    def _stop_session(self, kill=True):
        session, self.session = self.session, None
        if session is not None:
            session.stop(kill=kill)

    def _on_close_requested(self):
        # The user closed the lyric window: it is quitting by itself, so do not kill it.
        print_d("lxlyrics: the lyrics window was closed")
        self._stop_session(kill=False)
        self._disable_plugin()

    def _on_app_exited(self):
        self._stop_session()
        self._disable_plugin()

    def _disable_plugin(self):
        """Turn the plugin off exactly like the Plugins window's toggle does."""
        manager = PluginManager.instance
        if manager is not None:
            for plugin in manager.plugins:
                if plugin.cls is type(self):
                    manager.enable(plugin, False)
                    manager.save()
                    break
        # The idle callback that runs this must not be re-armed.
        return GLib.SOURCE_REMOVE
