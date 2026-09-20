# SPDX-License-Identifier: GPL-3.0-only
# Copyright (c) 2026 naromil
"""Hermetic tests for the Quod Libet adapter.

The adapter is imported against stub `gi.repository`, `quodlibet` and
`quodlibet.plugins` modules, so no Quod Libet, GTK, PyGObject, display server, player or
app binary is needed: every assertion here is about the exact JSON lines the adapter puts
on the app's stdin, plus the handful of Quod Libet calls it makes around them — and about
how a bad line, a dead pipe or a child that will not leave ends the session.
"""

import contextlib
import io
import json
import os
import sys
import types
import unittest

PLUGIN_DIR = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))

HELLO_LINE = '{"v":2,"action":"hello","host":"quodlibet","spectrum":false}\n'
EMPTY_INFO_LINE = (
    '{"v":2,"action":"set_info","path":"","singer":"","name":"","album":"","lrc":"",'
    '"tlrc":"","rlrc":"","lxlrc":"","isPlay":false,"played_time":0}\n'
)
STOP_LINE = '{"v":2,"action":"set_stop"}\n'
FULLSCREEN_LINE = '{"v":2,"action":"set_fullscreen","isFullscreen":false}\n'


class StubError(Exception):
    """Stands in for GLib.Error, which the stub API raises where PyGObject does."""

    def __init__(self, message, domain=None, code=None):
        super().__init__(message)
        self.domain = domain
        self.code = code

    def matches(self, domain, code):
        return self.domain == domain and self.code == code


class _Env:
    """The stub environment: the fakes themselves plus the knobs the tests flip."""

    def __init__(self):
        self.processes = []
        self.sources = {}
        self.next_source_id = 0
        self.plugin_manager = None
        self.plugin_instance = None
        self.spawn_error = None
        self.write_error = None
        self.close_error = None
        self.short_write = None
        self.config_values = {}
        self.config_calls = []
        self.window_state = 0
        self.window_signal_ids = []
        self.disconnected = []

    def reset(self):
        for process in self.processes:
            os.close(process.stdin.fd)
        self.processes.clear()
        self.sources.clear()
        self.config_values.clear()
        self.config_calls.clear()
        self.window_signal_ids.clear()
        self.disconnected.clear()
        self.window_state = 0
        self.spawn_error = None
        self.write_error = None
        self.close_error = None
        self.short_write = None

    def add_source(self, interval, callback, args):
        self.next_source_id += 1
        source = (self.next_source_id, interval, callback, args)
        self.sources[self.next_source_id] = source
        return self.next_source_id

    def _run_sources(self, repeating):
        """Fire each armed source once, as the main loop would.

        A source whose callback returns a falsy value (`GLib.SOURCE_REMOVE`) is dropped,
        one that returns truthy (`GLib.SOURCE_CONTINUE`) stays armed.
        """
        pending = [source for source in self.sources.values() if bool(source[1]) == repeating]
        for source_id, _interval, callback, args in pending:
            if source_id not in self.sources:
                continue
            if not callback(*args):
                self.sources.pop(source_id, None)

    def run_timeouts(self):
        self._run_sources(repeating=True)

    def run_idles(self):
        self._run_sources(repeating=False)

    def armed_intervals(self):
        """The intervals of the repeating sources currently armed, in arm order."""
        return [source[1] for source in self.sources.values() if source[1]]

    def enable_plugin(self):
        """The Plugins window's toggle: `pm.enable(plugin, True)` (pluginwin.py)."""
        self.plugin_manager.enable(self.plugin_manager.plugins[0], True)

    def disable_plugin(self):
        self.plugin_manager.enable(self.plugin_manager.plugins[0], False)

    def fire(self, event, *args):
        """Deliver a player event the way EventPluginHandler does.

        Upstream only calls a hook the plugin class actually overrides
        (`quodlibet/plugins/events.py`, the `overridden()` check), so a hook the adapter
        never defined is never invoked here either.
        """
        name = "plugin_on_" + event
        if name not in type(self.plugin_instance).__dict__:
            raise AssertionError("%s is not implemented by the adapter" % name)
        getattr(self.plugin_instance, name)(*args)


def install_stubs():
    """Install the stub `gi` / `quodlibet` modules and return the environment."""
    env = _Env()

    def timeout_add(interval, callback, *args):
        return env.add_source(interval, callback, args)

    def idle_add(callback, *args):
        return env.add_source(0, callback, args)

    def source_remove(source_id):
        env.sources.pop(source_id, None)

    class Bytes:
        def __init__(self, data):
            self.data = data

        @classmethod
        def new(cls, data):
            return cls(data)

        def get_data(self):
            return self.data

    GLib = types.SimpleNamespace(
        PRIORITY_DEFAULT=0,
        SOURCE_REMOVE=False,
        SOURCE_CONTINUE=True,
        Error=StubError,
        Bytes=Bytes,
        timeout_add=timeout_add,
        idle_add=idle_add,
        source_remove=source_remove,
    )

    class SubprocessFlags:
        STDIN_PIPE = 64
        STDOUT_PIPE = 16

    class OutputStream:
        """The app's stdin.

        GLib rejects a second async write while one is outstanding (`G_IO_ERROR_PENDING`),
        so this fake does too, and acknowledgements are deferred until `ack()` — the
        adapter has to queue its lines rather than fire writes off blindly.
        """

        def __init__(self):
            self.raw = b""
            self.pending = None
            self.accepted = 0
            self.closed = False
            # A real fd, so the adapter's `os.set_blocking` call is exercised for real.
            self.fd = os.open(os.devnull, os.O_WRONLY)

        def get_fd(self):
            return self.fd

        def write_bytes_async(self, data, _priority, _cancellable, callback):
            if self.pending is not None:
                raise AssertionError("Stream has outstanding operation")
            self.pending = (data.get_data(), callback)

        def write_bytes_finish(self, _result):
            if env.write_error is not None:
                raise env.write_error
            return self.accepted

        def close(self):
            if env.close_error is not None:
                raise env.close_error
            self.closed = True

        def ack(self):
            """Complete outstanding writes, as the pipe does when it takes bytes.

            `env.short_write` makes the next completion a partial one, the way a nearly
            full pipe accepts only part of a line.
            """
            while self.pending is not None:
                data, callback = self.pending
                self.pending = None
                if env.write_error is None:
                    take = len(data)
                    if env.short_write is not None and take > env.short_write:
                        take = env.short_write
                        env.short_write = None
                    self.accepted = take
                    self.raw += data[:take]
                if callback is not None:
                    callback(self, None)

    class InputStream:
        """The app's stdout: one armed read at a time, fed by the tests."""

        def __init__(self):
            self.callback = None
            self.result = None

        def read_line_async(self, _priority, _cancellable, callback):
            if self.callback is not None:
                raise AssertionError("a second read was armed while one is outstanding")
            self.callback = callback

        def read_line_finish_utf8(self, _result):
            return self.result

        def feed_line(self, line):
            self._deliver((line, len(line)))

        def feed_eof(self):
            self._deliver((None, 0))

        def _deliver(self, result):
            callback, self.callback = self.callback, None
            if callback is None:
                raise AssertionError("the app wrote without a read armed")
            self.result = result
            callback(self, None)

    class Subprocess:
        def __init__(self, argv, flags):
            self.argv = list(argv)
            self.flags = flags
            self.stdin = OutputStream()
            self.stdout = InputStream()
            self.killed = False
            self.wait_callback = None

        @staticmethod
        def new(argv, flags):
            if env.spawn_error is not None:
                raise env.spawn_error
            process = Subprocess(argv, flags)
            env.processes.append(process)
            return process

        def get_stdin_pipe(self):
            return self.stdin

        def get_stdout_pipe(self):
            return self.stdout

        def wait_async(self, _cancellable, callback):
            self.wait_callback = callback

        def feed_exit(self):
            """The child exited on its own, which is what `wait_async` reports."""
            callback, self.wait_callback = self.wait_callback, None
            if callback is not None:
                callback(self, None)

        def force_exit(self):
            self.killed = True

    Gio = types.SimpleNamespace(
        Subprocess=Subprocess,
        SubprocessFlags=SubprocessFlags,
        DataInputStream=types.SimpleNamespace(new=staticmethod(lambda stream: stream)),
        io_error_quark=staticmethod(lambda: 42),
        IOErrorEnum=types.SimpleNamespace(PENDING=20),
    )

    class GdkWindow:
        """The main window's Gdk window: `get_state()` is what the adapter queries."""

        def get_state(self):
            return env.window_state

    class WindowStateEvent:
        def __init__(self, new_window_state):
            self.new_window_state = new_window_state

    class MainWindow:
        """`quodlibet.qltk.window.Window` (app.window): signal plumbing plus Gdk state."""

        def __init__(self):
            self.handlers = {}

        def connect(self, signal, callback):
            signal_id = ("window", signal, len(self.handlers.get(signal, [])) + 1)
            self.handlers.setdefault(signal, []).append((signal_id, callback))
            env.window_signal_ids.append(signal_id)
            return signal_id

        def disconnect(self, signal_id):
            env.disconnected.append(signal_id)
            for signal, entries in self.handlers.items():
                self.handlers[signal] = [entry for entry in entries if entry[0] != signal_id]

        def get_window(self):
            return GdkWindow()

        def emit_state(self, new_window_state):
            env.window_state = new_window_state
            for _signal_id, callback in list(self.handlers.get("window-state-event", [])):
                callback(self, WindowStateEvent(new_window_state))

    Gdk = types.SimpleNamespace(
        WindowState=types.SimpleNamespace(FULLSCREEN=4, MAXIMIZED=2),
    )

    class Widget:
        def __init__(self, **kwargs):
            self.kwargs = kwargs
            self.handlers = {}
            self.properties = {}
            self.children = []

        def connect(self, signal, callback):
            self.handlers.setdefault(signal, []).append(callback)

        def emit(self, signal, *args):
            for callback in self.handlers.get(signal, []):
                callback(self, *args)

        def set_border_width(self, width):
            self.properties["border_width"] = width

        def set_alignment(self, xalign, yalign):
            self.properties["alignment"] = (xalign, yalign)

        def pack_start(self, child, expand, fill, padding):
            self.children.append(child)

    class Label(Widget):
        pass

    class Entry(Widget):
        text = ""

        def set_text(self, text):
            self.text = text
            self.emit("changed")

        def get_text(self):
            return self.text

    class Box(Widget):
        pass

    Gtk = types.SimpleNamespace(
        Box=Box,
        Label=Label,
        Entry=Entry,
        Orientation=types.SimpleNamespace(HORIZONTAL=0, VERTICAL=1),
    )

    class EventPlugin:
        """`quodlibet.plugins.events.EventPlugin`: the hooks the adapter overrides."""

        PLUGIN_INSTANCE = True

        def plugin_on_song_started(self, song):
            pass

        def plugin_on_song_ended(self, song, stopped):
            pass

        def plugin_on_paused(self):
            pass

        def plugin_on_unpaused(self):
            pass

        def plugin_on_seek(self, song, msec):
            pass

        def enabled(self):
            pass

        def disabled(self):
            pass

    class PluginConfigMixin:
        """`quodlibet.plugins.PluginConfigMixin`, including its option-name derivation.

        Upstream keys an option as `<CONFIG_SECTION or PLUGIN_ID.lower()>_<name>` in the
        `plugins` section of Quod Libet's own config, so the tests can pin the exact key
        the adapter writes.
        """

        CONFIG_SECTION = ""

        @classmethod
        def _config_key(cls, name):
            prefix = cls.CONFIG_SECTION or cls.PLUGIN_ID.lower().replace(" ", "_")
            return "%s_%s" % (prefix, name)

        @classmethod
        def config_get(cls, name, default=""):
            return env.config_values.get(("plugins", cls._config_key(name)), default)

        @classmethod
        def config_set(cls, name, value):
            key = ("plugins", cls._config_key(name))
            env.config_values[key] = value
            env.config_calls.append((key[0], key[1], value))

    class Plugin:
        """`quodlibet.plugins.Plugin`: the per-class wrapper the manager enables."""

        def __init__(self, cls, instance=None):
            self.cls = cls
            self.instance = instance

        @property
        def id(self):
            return self.cls.PLUGIN_ID

        def get_instance(self):
            """Event plugins are singletons (`PLUGIN_INSTANCE`)."""
            if self.instance is None:
                self.instance = self.cls()
            return self.instance

    class PluginManager:
        """`quodlibet.plugins.PluginManager`: enable/disable/save, faithfully ordered.

        Upstream's `enable()` calls `enabled()`/`disabled()` on the instance and keeps the
        active-id set that `save()` writes to the config; the Plugins window's toggle is
        exactly `enable(plugin, status)` + `save()`.
        """

        instance = None

        def __init__(self):
            self.entries = []
            self.active = set()
            self.saves = 0
            env.plugin_manager = self

        def add(self, cls, instance=None):
            entry = Plugin(cls, instance)
            self.entries.append(entry)
            return entry

        @property
        def plugins(self):
            return list(self.entries)

        def enabled(self, plugin):
            return plugin.id in self.active

        def enable(self, plugin, status, force=False):
            if not force and self.enabled(plugin) == bool(status):
                return
            instance = plugin.get_instance()
            if status:
                if instance is not None and hasattr(instance, "enabled"):
                    instance.enabled()
                self.active.add(plugin.id)
            else:
                self.active.discard(plugin.id)
                if instance is not None and hasattr(instance, "disabled"):
                    instance.disabled()

        def save(self):
            self.saves += 1

    class Song:
        """`quodlibet.formats.AudioFile`, or a `RemoteFile` when `is_file` is False.

        A remote file answers `~filename` with its URI — upstream's `sanitize()` stores
        exactly that — so `is_file` is what separates a path from a stream.
        """

        def __init__(self, path="", is_file=True, **tags):
            self._path = path
            self.is_file = is_file
            self._tags = tags

        def __call__(self, key, default=""):
            if key == "~filename":
                return self._path or default
            return self._tags.get(key, default)

        def comma(self, key):
            return self._tags.get(key, "")

    class Player:
        """`BasePlayer`: `paused`, `song`/`info` and the millisecond position getter."""

        def __init__(self):
            self.paused = True
            self.song = None
            self.info = None
            self.position = 0
            self.position_error = False

        def get_position(self):
            if self.position_error:
                raise StubError("no position")
            return self.position

    class Application:
        """`quodlibet.app`: the singleton the adapter reads the player and window from."""

        def __init__(self):
            self.player = Player()
            self.window = MainWindow()

    app = Application()

    class Icons:
        FORMAT_JUSTIFY_FILL = "format-justify-fill"

    def print_d(string, context=None):
        _write("D", string)

    def print_w(string, context=None):
        _write("W", string)

    def print_e(string, context=None):
        _write("E", string)

    def _write(level, string):
        # Quod Libet's own helpers print to stderr; the tests capture that stream.
        print("%s: %s" % (level, string), file=sys.stderr)

    quodlibet = types.ModuleType("quodlibet")
    quodlibet.app = app
    plugins = types.ModuleType("quodlibet.plugins")
    plugins.PluginConfigMixin = PluginConfigMixin
    plugins.PluginManager = PluginManager
    events = types.ModuleType("quodlibet.plugins.events")
    events.EventPlugin = EventPlugin
    qltk = types.ModuleType("quodlibet.qltk")
    qltk.Icons = Icons
    util = types.ModuleType("quodlibet.util")
    dprint = types.ModuleType("quodlibet.util.dprint")
    dprint.print_d = print_d
    dprint.print_w = print_w
    dprint.print_e = print_e
    quodlibet.plugins = plugins
    quodlibet.qltk = qltk
    quodlibet.util = util
    plugins.events = events
    util.dprint = dprint

    repository = types.ModuleType("gi.repository")
    repository.Gdk = Gdk
    repository.Gio = Gio
    repository.GLib = GLib
    repository.Gtk = Gtk

    gi_module = types.ModuleType("gi")
    gi_module.require_version = lambda *_args: None
    gi_module.repository = repository

    sys.modules["gi"] = gi_module
    sys.modules["gi.repository"] = repository
    sys.modules["quodlibet"] = quodlibet
    sys.modules["quodlibet.plugins"] = plugins
    sys.modules["quodlibet.plugins.events"] = events
    sys.modules["quodlibet.qltk"] = qltk
    sys.modules["quodlibet.util"] = util
    sys.modules["quodlibet.util.dprint"] = dprint

    env.app = app
    env.Gio = Gio
    env.Gdk = Gdk
    env.Gtk = Gtk
    env.Song = Song
    env.Player = Player
    env.MainWindow = MainWindow
    env.PluginManager = PluginManager
    return env


def load_plugin():
    """Import the adapter against the stub modules. Must run before `import lxlyrics`."""
    env = install_stubs()
    if PLUGIN_DIR not in sys.path:
        sys.path.insert(0, PLUGIN_DIR)
    import lxlyrics

    return env, lxlyrics


STUBS, lxlyrics = load_plugin()


class PluginTestCase(unittest.TestCase):
    """Starts from an enabled-able plugin with no session, in a throwaway environment."""

    def setUp(self):
        STUBS.reset()

        self.player = STUBS.Player()
        self.window = STUBS.MainWindow()
        STUBS.app.player = self.player
        STUBS.app.window = self.window
        self.manager = STUBS.PluginManager()
        STUBS.PluginManager.instance = self.manager
        self.plugin = lxlyrics.LxLyrics()
        self.entry = self.manager.add(lxlyrics.LxLyrics, self.plugin)
        STUBS.plugin_instance = self.plugin

    @property
    def proc(self):
        return STUBS.processes[-1]

    def written(self):
        """The complete lines the adapter has handed to the pipe, in order."""
        self.proc.stdin.ack()
        return self.proc.stdin.raw.decode("utf-8").splitlines(keepends=True)

    def clear_written(self):
        self.proc.stdin.ack()
        self.proc.stdin.raw = b""

    def stored_config(self):
        return dict(STUBS.config_values)

    def start_session(self):
        STUBS.enable_plugin()
        self.clear_written()
        return self.proc

    def assert_plugin_off(self):
        self.assertFalse(self.manager.enabled(self.entry), "the plugin toggle is off")
        self.assertIsNone(self.plugin.session)


class ToggleTests(PluginTestCase):
    def test_enabling_the_plugin_spawns_the_app_and_writes_hello_then_the_snapshot(self):
        self.assertEqual(STUBS.processes, [])
        self.assertFalse(self.manager.enabled(self.entry))

        STUBS.enable_plugin()

        self.assertEqual(self.proc.argv, ["lx-lyrics-app", "--player-feed"])
        self.assertEqual(
            self.proc.flags,
            STUBS.Gio.SubprocessFlags.STDIN_PIPE | STUBS.Gio.SubprocessFlags.STDOUT_PIPE,
        )
        self.assertEqual(
            self.written(), [HELLO_LINE, EMPTY_INFO_LINE, STOP_LINE, FULLSCREEN_LINE]
        )
        self.assertTrue(self.manager.enabled(self.entry))

    def test_a_playing_track_snapshot_carries_its_position(self):
        self.player.paused = False
        self.player.song = STUBS.Song("/music/song.mp3", title="Song")
        self.player.position = 12_500

        STUBS.enable_plugin()

        self.assertEqual(
            self.written(),
            [
                HELLO_LINE,
                '{"v":2,"action":"set_info","path":"/music/song.mp3","singer":"","name":"Song",'
                '"album":"","lrc":"","tlrc":"","rlrc":"","lxlrc":"","isPlay":true,'
                '"played_time":12500}\n',
                '{"v":2,"action":"set_play","time":12500}\n',
                FULLSCREEN_LINE,
            ],
        )

    def test_a_paused_snapshot_pauses_the_lyric_timer(self):
        self.player.song = STUBS.Song("/music/song.mp3", title="Song")
        self.player.position = 4_000

        STUBS.enable_plugin()

        self.assertEqual(
            self.written(),
            [
                HELLO_LINE,
                '{"v":2,"action":"set_info","path":"/music/song.mp3","singer":"","name":"Song",'
                '"album":"","lrc":"","tlrc":"","rlrc":"","lxlrc":"","isPlay":false,'
                '"played_time":4000}\n',
                '{"v":2,"action":"set_pause"}\n',
                FULLSCREEN_LINE,
            ],
        )

    def test_disabling_the_plugin_closes_the_app_stdin_and_ends_the_session(self):
        self.start_session()

        STUBS.disable_plugin()

        self.assertTrue(self.proc.stdin.closed, "EOF is how the adapter ends a session (§2)")
        self.assertFalse(self.proc.killed, "the app gets the grace window to flush and exit")
        self.assert_plugin_off()
        self.assertNotIn(
            lxlyrics.STATUS_INTERVAL_MS, STUBS.armed_intervals(), "the position poll is gone too"
        )

    def test_a_child_that_ignores_the_stdin_eof_is_killed_after_the_grace_window(self):
        self.start_session()
        STUBS.disable_plugin()

        self.assertIn(lxlyrics.STOP_GRACE_MS, STUBS.armed_intervals())

        STUBS.run_timeouts()

        self.assertTrue(self.proc.killed)

    def test_a_child_that_exits_on_its_own_is_not_killed(self):
        self.start_session()
        STUBS.disable_plugin()
        self.proc.feed_exit()

        STUBS.run_timeouts()

        self.assertFalse(self.proc.killed)

    def test_a_remembered_plugin_starts_the_session_when_quod_libet_restores_it(self):
        STUBS.config_values[("plugins", "lxlyrics_app_path")] = "/opt/lx-lyrics/lx-lyrics-app"

        # `PluginManager.__restore()` enables remembered plugins with force=True.
        self.manager.enable(self.entry, True, force=True)

        self.assertEqual(self.proc.argv, ["/opt/lx-lyrics/lx-lyrics-app", "--player-feed"])
        self.assertTrue(self.manager.enabled(self.entry))

    def test_an_app_that_cannot_run_turns_the_plugin_off(self):
        STUBS.spawn_error = StubError("Failed to execute child process")

        output = io.StringIO()
        with contextlib.redirect_stderr(output):
            STUBS.enable_plugin()
            STUBS.run_idles()  # the self-disable is deferred past `enable(..., True)`

        self.assertEqual(STUBS.processes, [])
        self.assertIn("cannot run lx-lyrics-app", output.getvalue())
        self.assert_plugin_off()
        self.assertTrue(self.manager.saves, "the off state is written like the toggle's")


class EventMappingTests(PluginTestCase):
    def test_a_track_start_sends_the_path_and_metadata(self):
        self.start_session()
        self.player.paused = False

        STUBS.fire(
            "song_started",
            STUBS.Song("/music/Artist/01 - Song.mp3", artist="Artist", title="Song", album="Album"),
        )

        self.assertEqual(
            self.written(),
            [
                '{"v":2,"action":"set_info","path":"/music/Artist/01 - Song.mp3",'
                '"singer":"Artist","name":"Song","album":"Album","lrc":"","tlrc":"","rlrc":"",'
                '"lxlrc":"","isPlay":true,"played_time":0}\n',
                '{"v":2,"action":"set_play","time":0}\n',
            ],
        )

    def test_a_song_without_metadata_still_sends_its_path(self):
        self.start_session()

        STUBS.fire("song_started", STUBS.Song("/music/plain.mp3"))

        self.assertEqual(
            self.written(),
            [
                '{"v":2,"action":"set_info","path":"/music/plain.mp3","singer":"","name":"",'
                '"album":"","lrc":"","tlrc":"","rlrc":"","lxlrc":"","isPlay":false,'
                '"played_time":0}\n',
                '{"v":2,"action":"set_pause"}\n',
            ],
        )

    def test_a_stream_has_no_path_even_though_quod_libet_stores_its_uri_there(self):
        self.start_session()

        STUBS.fire("song_started", STUBS.Song("https://radio.example/stream", is_file=False))

        self.assertEqual(
            self.written(),
            [
                '{"v":2,"action":"set_info","path":"","singer":"","name":"","album":"","lrc":"",'
                '"tlrc":"","rlrc":"","lxlrc":"","isPlay":false,"played_time":0}\n',
                '{"v":2,"action":"set_pause"}\n',
            ],
        )

    def test_no_song_sends_empty_info_then_stop(self):
        self.start_session()

        STUBS.fire("song_started", None)

        self.assertEqual(self.written(), [EMPTY_INFO_LINE, STOP_LINE])

    def test_a_non_utf8_path_is_replaced_rather_than_losing_the_track(self):
        self.start_session()

        output = io.StringIO()
        with contextlib.redirect_stderr(output):
            STUBS.fire("song_started", STUBS.Song("/music/bad\udcffname.mp3", title="Bad"))

        self.assertEqual(
            self.written(),
            [
                '{"v":2,"action":"set_info","path":"/music/bad?name.mp3","singer":"","name":"Bad",'
                '"album":"","lrc":"","tlrc":"","rlrc":"","lxlrc":"","isPlay":false,'
                '"played_time":0}\n',
                '{"v":2,"action":"set_pause"}\n',
            ],
        )
        self.assertIn("replacing unencodable characters", output.getvalue())
        self.assertTrue(self.manager.enabled(self.entry), "the session survives an odd path")

    def test_a_stopped_song_ends_playback(self):
        self.start_session()

        STUBS.fire("song_ended", STUBS.Song("/music/a.mp3"), True)

        self.assertEqual(self.written(), [STOP_LINE])

    def test_a_gapless_move_to_the_next_song_is_not_a_stop(self):
        self.start_session()

        STUBS.fire("song_ended", STUBS.Song("/music/a.mp3"), False)

        self.assertEqual(self.written(), [], "the following song-started carries the track")

    def test_pausing_and_resuming_use_the_player_position(self):
        self.start_session()
        self.player.position = 30_000

        STUBS.fire("paused")
        self.player.paused = False
        STUBS.fire("unpaused")

        self.assertEqual(
            self.written(),
            ['{"v":2,"action":"set_pause"}\n', '{"v":2,"action":"set_play","time":30000}\n'],
        )

    def test_a_player_seek_is_forwarded_in_milliseconds(self):
        self.start_session()

        STUBS.fire("seek", STUBS.Song("/music/a.mp3"), 42_000)

        self.assertEqual(self.written(), ['{"v":2,"action":"set_play","time":42000}\n'])

    def test_the_position_falls_back_to_zero_when_the_player_refuses_to_report_one(self):
        self.start_session()
        self.player.position_error = True
        self.player.paused = False

        STUBS.fire("unpaused")

        self.assertEqual(self.written(), ['{"v":2,"action":"set_play","time":0}\n'])

    def test_only_host_actions_ever_leave_the_adapter(self):
        self.start_session()
        self.player.paused = False
        self.player.song = STUBS.Song("/music/a.mp3", title="A")
        STUBS.fire("song_started", self.player.song)
        STUBS.fire("seek", self.player.song, 5_000)
        STUBS.fire("paused")
        STUBS.fire("unpaused")
        STUBS.run_timeouts()
        self.window.emit_state(STUBS.Gdk.WindowState.FULLSCREEN)
        self.proc.stdout.feed_line('{"v":2,"action":"close_requested"}')

        host_actions = {
            "hello",
            "set_info",
            "set_status",
            "set_play",
            "set_pause",
            "set_stop",
            "set_fullscreen",
        }
        lines = self.written()
        self.assertTrue(lines)
        for line in lines:
            message = json.loads(line)
            self.assertEqual(message["v"], 2)
            self.assertIn(message["action"], host_actions)
        self.assertNotIn("get_analyser_data_array", "".join(lines))


class StatusTests(PluginTestCase):
    def test_one_status_line_per_500_ms_of_playback(self):
        self.start_session()
        self.player.paused = False
        self.assertEqual(STUBS.armed_intervals(), [lxlyrics.STATUS_INTERVAL_MS])

        self.player.position = 0
        STUBS.run_timeouts()
        self.player.position = 200
        STUBS.run_timeouts()

        self.assertEqual(
            self.written(),
            [
                '{"v":2,"action":"set_status","isPlay":true,"played_time":0}\n',
                '{"v":2,"action":"set_status","isPlay":true,"played_time":200}\n',
            ],
        )

    def test_a_jump_in_the_elapsed_clock_is_reported_as_a_seek(self):
        self.start_session()
        self.player.paused = False

        self.player.position = 30_000
        STUBS.run_timeouts()
        self.player.position = 44_000
        STUBS.run_timeouts()

        self.assertEqual(
            self.written(),
            [
                '{"v":2,"action":"set_status","isPlay":true,"played_time":30000}\n',
                '{"v":2,"action":"set_play","time":44000}\n',
            ],
        )

    def test_nothing_is_sent_while_paused(self):
        self.start_session()
        self.player.paused = True
        self.player.position = 9_000

        STUBS.run_timeouts()

        self.assertEqual(self.written(), [])

    def test_the_next_track_restarting_at_zero_is_not_a_seek(self):
        self.start_session()
        self.player.paused = False
        self.player.position = 90_000
        STUBS.run_timeouts()
        self.clear_written()

        STUBS.fire("song_started", STUBS.Song("/music/next.mp3"))
        self.clear_written()

        self.player.position = 0
        STUBS.run_timeouts()

        self.assertEqual(
            self.written(), ['{"v":2,"action":"set_status","isPlay":true,"played_time":0}\n']
        )

    def test_a_seek_does_not_make_the_next_poll_look_like_one(self):
        self.start_session()
        self.player.paused = False

        STUBS.fire("seek", STUBS.Song("/music/a.mp3"), 60_000)
        self.clear_written()
        self.player.position = 60_200
        STUBS.run_timeouts()

        self.assertEqual(
            self.written(),
            ['{"v":2,"action":"set_status","isPlay":true,"played_time":60200}\n'],
        )


class AppToHostTests(PluginTestCase):
    def test_close_requested_ends_the_session_and_turns_the_plugin_off(self):
        self.start_session()

        self.proc.stdout.feed_line('{"v":2,"action":"close_requested"}')

        self.assert_plugin_off()
        self.assertFalse(self.proc.killed, "the app closes itself; do not kill the animation")
        self.assertFalse(
            self.proc.stdin.closed, "the app exits on its own: the pipes are left alone (§4)"
        )
        self.assertEqual(self.written(), [])
        self.assertNotIn(
            lxlyrics.STATUS_INTERVAL_MS, STUBS.armed_intervals(), "the poll is disarmed too"
        )

    def test_close_requested_is_remembered_by_the_plugin_manager(self):
        self.start_session()

        self.proc.stdout.feed_line('{"v":2,"action":"close_requested"}')

        self.assertFalse(self.manager.enabled(self.entry))
        self.assertTrue(self.manager.saves, "the toggle's own persistence runs")

    def test_the_plugin_can_be_enabled_again_after_a_close(self):
        self.start_session()
        self.proc.stdout.feed_line('{"v":2,"action":"close_requested"}')

        STUBS.enable_plugin()

        self.assertEqual(len(STUBS.processes), 2, "a new session is a new child")
        self.assertEqual(self.written(), [HELLO_LINE, EMPTY_INFO_LINE, STOP_LINE, FULLSCREEN_LINE])
        self.assertTrue(self.manager.enabled(self.entry))

    def test_a_vanished_app_turns_the_plugin_off(self):
        self.start_session()

        self.proc.stdout.feed_eof()

        self.assert_plugin_off()
        self.assertTrue(self.proc.killed)

    def test_an_over_cap_app_line_is_a_protocol_error(self):
        self.start_session()

        over_cap = "x" * (lxlyrics.MAX_LINE_BYTES + 1)
        output = io.StringIO()
        with contextlib.redirect_stderr(output):
            self.proc.stdout.feed_line(over_cap)

        self.assertIn("protocol error", output.getvalue())
        self.assertIn(str(lxlyrics.MAX_LINE_BYTES + 1), output.getvalue())
        self.assert_plugin_off()
        self.assertTrue(self.proc.killed, "the child is not left running")

    def test_a_line_exactly_at_the_cap_is_not_a_protocol_error(self):
        self.start_session()

        at_cap = "x" * lxlyrics.MAX_LINE_BYTES
        output = io.StringIO()
        with contextlib.redirect_stderr(output):
            self.proc.stdout.feed_line(at_cap)

        self.assertNotIn("protocol error", output.getvalue())
        self.assertIn("ignoring non-JSON line", output.getvalue())
        self.assertTrue(self.manager.enabled(self.entry), "the cap is a limit, not an error")

    def test_stray_output_on_stdout_is_ignored(self):
        self.start_session()
        self.player.paused = False

        self.proc.stdout.feed_line("warning: something leaked to stdout")
        self.player.position = 0
        STUBS.run_timeouts()

        self.assertTrue(self.manager.enabled(self.entry))
        self.assertEqual(
            self.written(), ['{"v":2,"action":"set_status","isPlay":true,"played_time":0}\n']
        )

    def _assert_protocol_error_ends_the_session(self, line):
        self.start_session()

        output = io.StringIO()
        with contextlib.redirect_stderr(output):
            self.proc.stdout.feed_line(line)

        self.assertIn("protocol error", output.getvalue())
        self.assert_plugin_off()
        self.assertTrue(self.proc.killed, "a child that broke the protocol is not left running")

    def test_an_unsupported_version_is_a_protocol_error(self):
        self._assert_protocol_error_ends_the_session('{"v":3,"action":"close_requested"}')

    def test_a_missing_version_is_a_protocol_error(self):
        self._assert_protocol_error_ends_the_session('{"action":"close_requested"}')

    def test_an_unknown_action_is_a_protocol_error(self):
        self._assert_protocol_error_ends_the_session('{"v":2,"action":"who_knows"}')

    def test_a_missing_action_is_a_protocol_error(self):
        self._assert_protocol_error_ends_the_session('{"v":2}')

    def test_a_non_object_json_line_is_a_protocol_error(self):
        self._assert_protocol_error_ends_the_session('["close_requested"]')

    def test_an_unrequested_analyser_request_is_a_protocol_error(self):
        self._assert_protocol_error_ends_the_session('{"v":2,"action":"get_analyser_data_array"}')


class WritePathTests(PluginTestCase):
    """The bounded, never-blocking write path (the fd itself is non-blocking; these cases
    pin the queueing behaviour around it)."""

    def _emit_states(self, count):
        self.player.paused = False
        for index in range(1, count + 1):
            self.player.position = index * lxlyrics.STATUS_INTERVAL_MS
            STUBS.run_timeouts()

    def test_a_stalled_child_keeps_the_queue_bounded_and_the_session_alive(self):
        self.start_session()

        output = io.StringIO()
        with contextlib.redirect_stderr(output):
            self._emit_states(lxlyrics.MAX_PENDING_LINES * 3)

        feed = self.plugin.session.feed
        self.assertEqual(len(feed.pending), lxlyrics.MAX_PENDING_LINES)
        self.assertIn(b'"played_time":96000}', feed.pending[-1], "the newest state is kept")
        self.assertNotIn(b'"played_time":1000}', feed.pending, "the oldest state is dropped")
        self.assertEqual(output.getvalue().count("not reading"), 1, "one line per episode")
        self.assertTrue(self.manager.enabled(self.entry), "a stalled app does not end the session")

    def test_the_overflow_warning_returns_after_the_queue_drains(self):
        self.start_session()

        output = io.StringIO()
        with contextlib.redirect_stderr(output):
            self._emit_states(lxlyrics.MAX_PENDING_LINES * 2)
            self.written()  # the app catches up: the queue empties
            self.assertEqual(self.plugin.session.feed.pending, [])
            self._emit_states(lxlyrics.MAX_PENDING_LINES * 2)

        self.assertEqual(output.getvalue().count("not reading"), 2)

    def test_the_handshake_is_never_dropped_by_an_overflow(self):
        self.player.paused = False
        STUBS.enable_plugin()  # nothing is acknowledged: `hello` is still in flight

        self._emit_states(lxlyrics.MAX_PENDING_LINES * 3)

        feed = self.plugin.session.feed
        self.assertEqual(len(feed.pending), lxlyrics.MAX_PENDING_LINES)
        self.assertTrue(
            feed.pending[0].startswith(HELLO_LINE.encode()),
            "`hello` still leads the queue the app reads (§9)",
        )

    def test_a_stalled_child_can_still_be_stopped(self):
        self.start_session()
        self._emit_states(5)
        self.assertIsNotNone(self.proc.stdin.pending, "a write is stuck on the app")

        STUBS.disable_plugin()  # toggle off while that write waits

        self.assertTrue(self.proc.stdin.closed)
        STUBS.run_timeouts()  # the grace timer still runs while the write is pending
        self.assertTrue(self.proc.killed)
        self.assert_plugin_off()

    def test_a_short_write_keeps_the_line_in_one_piece(self):
        self.start_session()
        STUBS.short_write = 10  # a nearly full pipe takes only part of the line
        self.player.paused = False

        STUBS.fire("unpaused")

        self.assertEqual(self.written(), ['{"v":2,"action":"set_play","time":0}\n'])
        self.assertEqual(
            self.proc.stdin.raw,
            b'{"v":2,"action":"set_play","time":0}\n',
            "no byte lost, duplicated or interleaved",
        )
        self.assertEqual(self.plugin.session.feed.pending, [])

    def test_a_stalled_write_does_not_make_the_close_look_like_a_failure(self):
        self.start_session()
        self._emit_states(5)
        STUBS.close_error = StubError(
            "Stream has outstanding operation",
            domain=STUBS.Gio.io_error_quark(),
            code=STUBS.Gio.IOErrorEnum.PENDING,
        )

        output = io.StringIO()
        with contextlib.redirect_stderr(output):
            STUBS.disable_plugin()

        self.assertNotIn("closing the app's stdin failed", output.getvalue())
        self.assert_plugin_off()
        STUBS.run_timeouts()
        self.assertTrue(self.proc.killed)

    def test_a_real_close_failure_is_still_reported(self):
        self.start_session()
        STUBS.close_error = StubError("Input/output error")

        output = io.StringIO()
        with contextlib.redirect_stderr(output):
            STUBS.disable_plugin()

        self.assertIn("closing the app's stdin failed", output.getvalue())
        self.assert_plugin_off()

    def test_a_failed_write_ends_the_session_instead_of_wedging(self):
        self.start_session()
        self.player.paused = False
        STUBS.write_error = StubError("Broken pipe")

        output = io.StringIO()
        with contextlib.redirect_stderr(output):
            STUBS.fire("unpaused")
            self.written()

        self.assertIn("writing to the app failed", output.getvalue())
        self.assert_plugin_off()
        self.assertTrue(self.proc.killed)


class FullscreenTests(PluginTestCase):
    def test_the_snapshot_carries_the_initial_fullscreen_state(self):
        STUBS.window_state = STUBS.Gdk.WindowState.FULLSCREEN

        STUBS.enable_plugin()

        self.assertEqual(
            self.written(),
            [
                HELLO_LINE,
                EMPTY_INFO_LINE,
                STOP_LINE,
                '{"v":2,"action":"set_fullscreen","isFullscreen":true}\n',
            ],
        )

    def test_a_window_state_change_is_forwarded_once(self):
        self.start_session()

        self.window.emit_state(STUBS.Gdk.WindowState.FULLSCREEN)
        self.window.emit_state(STUBS.Gdk.WindowState.FULLSCREEN)
        self.window.emit_state(0)

        self.assertEqual(
            self.written(),
            [
                '{"v":2,"action":"set_fullscreen","isFullscreen":true}\n',
                '{"v":2,"action":"set_fullscreen","isFullscreen":false}\n',
            ],
        )

    def test_the_window_handler_is_dropped_with_the_session(self):
        self.start_session()
        STUBS.disable_plugin()
        self.clear_written()

        self.window.emit_state(STUBS.Gdk.WindowState.FULLSCREEN)

        self.assertEqual(self.written(), [], "a dead session hears nothing")
        self.assertEqual(
            STUBS.disconnected, STUBS.window_signal_ids, "and leaks no handler per session"
        )


class ConfigTests(PluginTestCase):
    def test_the_app_path_lives_in_quod_libets_own_config_under_the_plugin_id(self):
        pane = lxlyrics.LxLyrics.PluginPreferences(None)
        entry = next(child for child in pane.children if isinstance(child, STUBS.Gtk.Entry))
        self.assertEqual(entry.get_text(), "lx-lyrics-app")

        entry.set_text("/usr/local/bin/lx-lyrics-app")

        self.assertEqual(
            STUBS.config_calls, [("plugins", "lxlyrics_app_path", "/usr/local/bin/lx-lyrics-app")]
        )

    def test_the_preferences_pane_ignores_an_empty_executable(self):
        pane = lxlyrics.LxLyrics.PluginPreferences(None)
        entry = next(child for child in pane.children if isinstance(child, STUBS.Gtk.Entry))

        entry.set_text("")

        self.assertEqual(STUBS.config_calls, [])
        self.assertEqual(self.stored_config(), {})

    def test_the_preferences_pane_replaces_the_default_it_started_from(self):
        pane = lxlyrics.LxLyrics.PluginPreferences(None)
        entry = next(child for child in pane.children if isinstance(child, STUBS.Gtk.Entry))

        entry.set_text("/usr/local/bin/lx-lyrics-app")
        self.start_session()

        self.assertEqual(self.proc.argv[0], "/usr/local/bin/lx-lyrics-app")

    def test_the_default_is_the_bare_name_resolved_from_path(self):
        self.start_session()

        self.assertEqual(
            self.proc.argv, ["lx-lyrics-app", "--player-feed"], "argv[0] stays unqualified"
        )
        self.assertEqual(STUBS.config_calls, [], "a default is never written back")


class LifecycleTests(PluginTestCase):
    def test_quitting_quod_libet_closes_the_app_stdin(self):
        self.start_session()

        # `quodlibet.plugins.quit()` -> `PluginManager.quit()` disables every plugin.
        STUBS.disable_plugin()

        self.assertTrue(self.proc.stdin.closed)
        self.assertIsNone(self.plugin.session)

    def test_the_session_is_absent_until_the_plugin_is_enabled(self):
        self.assertIsNone(self.plugin.session)
        self.assertEqual(STUBS.processes, [])

    def test_a_session_is_never_started_twice(self):
        self.start_session()
        proc = self.proc

        # `PluginManager.__restore()` can enable an already-enabled plugin with force.
        self.plugin.enabled()

        self.assertEqual(STUBS.processes, [proc])
        self.assertEqual(self.written(), [])

    def test_the_instance_survives_a_disable_enable_cycle(self):
        self.start_session()
        STUBS.disable_plugin()

        STUBS.enable_plugin()

        self.assertIs(self.manager.entries[0].instance, self.plugin, "event plugins are singletons")
        self.assertIsNotNone(self.plugin.session)
        self.assertEqual(len(STUBS.processes), 2)


if __name__ == "__main__":
    unittest.main()
