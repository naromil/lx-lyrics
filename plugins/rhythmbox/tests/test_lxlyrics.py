# SPDX-License-Identifier: GPL-3.0-only
# Copyright (c) 2026 naromil
"""Hermetic tests for the Rhythmbox adapter.

The adapter is imported against stub `gi.repository` namespaces and a stub
`rb`-less player, so no Rhythmbox, GTK, PyGObject or display server is needed: every
assertion here is about the exact JSON lines the adapter puts on the app's stdin, plus
the handful of Rhythmbox calls it makes around them — and about how a bad line, a dead
pipe or a child that will not leave ends the session.
"""

import contextlib
import io
import json
import os
import shutil
import sys
import tempfile
import types
import unittest
import urllib.parse

PLUGIN_DIR = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))

PROP_TITLE = "title"
PROP_ARTIST = "artist"
PROP_ALBUM = "album"

HELLO_LINE = '{"v":2,"action":"hello","host":"rhythmbox","spectrum":false}\n'
EMPTY_INFO_LINE = (
    '{"v":2,"action":"set_info","path":"","singer":"","name":"","album":"","lrc":"",'
    '"tlrc":"","rlrc":"","lxlrc":"","isPlay":false,"played_time":0}\n'
)
STOP_LINE = '{"v":2,"action":"set_stop"}\n'


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
        self.spawn_error = None
        self.write_error = None
        self.close_error = None
        self.short_write = None
        self.key_files = {}
        self.timeouts = []

    def reset(self):
        for process in self.processes:
            os.close(process.stdin.fd)
        self.processes.clear()
        self.key_files.clear()
        self.timeouts.clear()
        self.spawn_error = None
        self.write_error = None
        self.close_error = None
        self.short_write = None

    def run_timeouts(self):
        """Fire every armed timeout, as the main loop would when the interval elapses."""
        pending, self.timeouts = self.timeouts, []
        for _interval, callback, args in pending:
            callback(*args)


def install_stubs():
    """Install the stub `gi` / `gi.repository` modules and return the environment."""
    env = _Env()

    class KeyFile:
        """GLib.KeyFile over a process-wide dict keyed by path."""

        def __init__(self):
            self.path = None
            self.values = {}

        @classmethod
        def new(cls):
            return cls()

        def load_from_file(self, path, _flags):
            if path not in env.key_files:
                raise StubError("key file %s does not exist" % path)
            self.path = path
            self.values = dict(env.key_files[path])

        def save_to_file(self, path):
            self.path = path
            env.key_files[path] = dict(self.values)

        def set_boolean(self, group, key, value):
            self.values[(group, key)] = bool(value)

        def set_string(self, group, key, value):
            self.values[(group, key)] = value

        def get_boolean(self, group, key):
            if (group, key) not in self.values:
                raise StubError("no %s in group %s" % (key, group))
            return bool(self.values[(group, key)])

        def get_string(self, group, key):
            if (group, key) not in self.values:
                raise StubError("no %s in group %s" % (key, group))
            return str(self.values[(group, key)])

    class Bytes:
        def __init__(self, data):
            self.data = data

        @classmethod
        def new(cls, data):
            return cls(data)

        def get_data(self):
            return self.data

    class Variant:
        def __init__(self, value):
            self.value = value

        @classmethod
        def new_boolean(cls, value):
            return cls(value)

        def get_boolean(self):
            return bool(self.value)

    def filename_from_uri(uri, *_args):
        if not isinstance(uri, str) or not uri.startswith("file://"):
            raise StubError("conversion from URI to filename failed")
        # PyGObject decodes the returned file name with surrogateescape, so a name that is
        # not valid UTF-8 arrives as lone surrogates (verified against GLib 3.56).
        return (urllib.parse.unquote(uri[len("file://") :], errors="surrogateescape"), None)

    def timeout_add(interval, callback, *args):
        env.timeouts.append((interval, callback, args))
        return len(env.timeouts)

    GLib = types.SimpleNamespace(
        PRIORITY_DEFAULT=0,
        SOURCE_REMOVE=False,
        Error=StubError,
        KeyFile=KeyFile,
        KeyFileFlags=types.SimpleNamespace(NONE=0),
        Bytes=Bytes,
        Variant=Variant,
        filename_from_uri=filename_from_uri,
        timeout_add=timeout_add,
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

    class SimpleAction:
        def __init__(self, name, parameter_type, state=None, stateful=False):
            self.name = name
            self.parameter_type = parameter_type
            self.state = state
            self.stateful = stateful
            self.enabled = True
            self.handlers = []

        @classmethod
        def new(cls, name, parameter_type):
            return cls(name, parameter_type)

        @classmethod
        def new_stateful(cls, name, parameter_type, state):
            return cls(name, parameter_type, state, stateful=True)

        def connect(self, signal, callback):
            self.handlers.append((signal, callback))

        def set_state(self, state):
            # GLib: `g_simple_action_set_state: assertion 'state_type != NULL' failed`.
            if not self.stateful:
                raise AssertionError("set_state on a stateless action")
            self.state = state

        def set_enabled(self, enabled):
            self.enabled = enabled

        def activate(self):
            """Simulate the user picking the menu item."""
            for signal, callback in list(self.handlers):
                if signal == "activate":
                    callback(self, None)

        def is_checked(self):
            return self.state is not None and self.state.get_boolean()

    class MenuItem:
        def __init__(self, label, detailed_action):
            self.label = label
            self.detailed_action = detailed_action

        @classmethod
        def new(cls, label=None, detailed_action=None):
            return cls(label, detailed_action)

    Gio = types.SimpleNamespace(
        Subprocess=Subprocess,
        SubprocessFlags=SubprocessFlags,
        DataInputStream=types.SimpleNamespace(new=staticmethod(lambda stream: stream)),
        SimpleAction=SimpleAction,
        MenuItem=MenuItem,
        io_error_quark=staticmethod(lambda: 42),
        IOErrorEnum=types.SimpleNamespace(PENDING=20),
    )

    class Object:
        pass

    class Property:
        def __init__(self, **kwargs):
            self.kwargs = kwargs

    GObject = types.SimpleNamespace(
        Object=Object,
        property=lambda **kwargs: Property(**kwargs),
        type_register=lambda cls: cls,
    )

    class Widget:
        def __init__(self, **kwargs):
            self.kwargs = kwargs
            self.handlers = {}
            self.properties = {}

        def connect(self, signal, callback):
            self.handlers.setdefault(signal, []).append(callback)

        def emit(self, signal, *args):
            for callback in self.handlers.get(signal, []):
                callback(self, *args)

        def set_halign(self, align):
            self.properties["halign"] = align

        def set_hexpand(self, expand):
            self.properties["hexpand"] = expand

        def set_row_spacing(self, spacing):
            self.properties["row_spacing"] = spacing

        def set_column_spacing(self, spacing):
            self.properties["column_spacing"] = spacing

        def set_border_width(self, width):
            self.properties["border_width"] = width

    class Label(Widget):
        pass

    class Entry(Widget):
        text = ""

        def set_text(self, text):
            self.text = text
            self.emit("changed")

        def get_text(self):
            return self.text

    class Grid(Widget):
        def __init__(self, **kwargs):
            super().__init__(**kwargs)
            self.children = []

        def attach(self, child, left, top, width, height):
            self.children.append(child)

    Gtk = types.SimpleNamespace(
        Grid=Grid,
        Label=Label,
        Entry=Entry,
        Align=types.SimpleNamespace(START=0, FILL=1, END=2, CENTER=3),
    )

    class Activatable:
        """The libpeas extension point the adapter implements."""

    class Configurable:
        """The libpeas-gtk extension point the preferences pane implements."""

    Peas = types.SimpleNamespace(Activatable=Activatable)
    PeasGtk = types.SimpleNamespace(Configurable=Configurable)

    RB = types.SimpleNamespace(
        RhythmDBPropType=types.SimpleNamespace(
            TITLE=PROP_TITLE,
            ARTIST=PROP_ARTIST,
            ALBUM=PROP_ALBUM,
        )
    )

    class Player:
        """RBShellPlayer: signals plus the getters the adapter reads."""

        def __init__(self):
            self.handlers = {}
            self.connected = []
            self.disconnected = []
            self.entry = None
            self.playing = False
            self.playing_time = (True, 0)
            self.playing_time_error = False

        def connect(self, signal, handler):
            self.connected.append((signal, handler))
            self.handlers.setdefault(signal, []).append(handler)
            return len(self.connected)

        def disconnect(self, signal_id):
            self.disconnected.append(signal_id)

        def emit(self, signal, *args):
            for handler in list(self.handlers.get(signal, [])):
                handler(self, *args)

        def get_playing_entry(self):
            return self.entry

        def get_playing(self):
            return (True, self.playing)

        def get_playing_time(self):
            if self.playing_time_error:
                raise StubError("not playing")
            return self.playing_time

    class Application:
        """RBShellApplication: where the adapter registers its menu action."""

        def __init__(self):
            self.actions = {}
            self.menu_items = []
            self.removed_menu_items = []
            self.removed_actions = []

        def add_action(self, action):
            self.actions[action.name] = action

        def remove_action(self, name):
            self.removed_actions.append(name)
            self.actions.pop(name, None)

        def add_plugin_menu_item(self, path, item_id, item):
            self.menu_items.append((path, item_id, item))

        def remove_plugin_menu_item(self, path, item_id):
            self.removed_menu_items.append((path, item_id))

    class Shell:
        """RBShell: the object libpeas hands to do_activate."""

        def __init__(self):
            self.props = types.SimpleNamespace(
                shell_player=Player(),
                window=types.SimpleNamespace(name="window"),
                application=Application(),
            )

    class Entry_:
        """RhythmDBEntry: string properties raise KeyError when the entry has none."""

        def __init__(self, uri, **props):
            self._uri = uri
            self._props = props

        def get_playback_uri(self):
            return self._uri

        def get_string(self, prop):
            return self._props[prop]

    repository = types.ModuleType("gi.repository")
    repository.Gio = Gio
    repository.GLib = GLib
    repository.GObject = GObject
    repository.Gtk = Gtk
    repository.Peas = Peas
    repository.PeasGtk = PeasGtk
    repository.RB = RB

    gi_module = types.ModuleType("gi")
    gi_module.require_version = lambda *_args: None
    gi_module.repository = repository

    sys.modules["gi"] = gi_module
    sys.modules["gi.repository"] = repository

    env.Gio = Gio
    env.Gtk = Gtk
    env.Shell = Shell
    env.Player = Player
    env.Application = Application
    env.Entry = Entry_
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
    """Starts from an activated plugin with no session, in a throwaway config home."""

    def setUp(self):
        STUBS.reset()
        previous = os.environ.get("XDG_CONFIG_HOME")
        self.xdg_dir = tempfile.mkdtemp(prefix="lxlyrics-test-")
        os.environ["XDG_CONFIG_HOME"] = self.xdg_dir
        self.addCleanup(self._restore_config_home, previous)

        self.shell = STUBS.Shell()
        self.player = self.shell.props.shell_player
        self.application = self.shell.props.application
        self.plugin = lxlyrics.LxLyrics()
        self.plugin.object = self.shell

    def _restore_config_home(self, previous):
        shutil.rmtree(self.xdg_dir, ignore_errors=True)
        if previous is None:
            os.environ.pop("XDG_CONFIG_HOME", None)
        else:
            os.environ["XDG_CONFIG_HOME"] = previous

    @property
    def action(self):
        return self.application.actions["lxlyrics-toggle"]

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
        """The persisted state, read back the way a restarted Rhythmbox would."""
        config = lxlyrics.SessionConfig()
        config.load()
        return config

    def start_session(self):
        self.plugin.do_activate()
        self.action.activate()
        self.clear_written()
        return self.proc


class ToggleTests(PluginTestCase):
    def test_toggle_spawns_the_app_and_writes_hello_then_the_snapshot(self):
        self.plugin.do_activate()
        self.assertTrue(self.action.stateful, "only a stateful action renders as checkable")
        self.assertEqual(
            [
                (path, item_id, item.detailed_action)
                for path, item_id, item in self.application.menu_items
            ],
            [("view", "lxlyrics-toggle", "app.lxlyrics-toggle")],
        )
        self.assertFalse(self.action.is_checked())
        self.assertEqual(STUBS.processes, [])

        self.action.activate()

        self.assertEqual(self.proc.argv, ["lx-lyrics-app", "--player-feed"])
        self.assertEqual(
            self.proc.flags,
            STUBS.Gio.SubprocessFlags.STDIN_PIPE | STUBS.Gio.SubprocessFlags.STDOUT_PIPE,
        )
        self.assertEqual(self.written(), [HELLO_LINE, EMPTY_INFO_LINE, STOP_LINE])
        self.assertTrue(self.action.is_checked())
        self.assertTrue(self.stored_config().enabled)

    def test_toggle_off_closes_the_app_stdin_and_forgets_the_session(self):
        self.start_session()

        self.action.activate()

        self.assertTrue(self.proc.stdin.closed, "EOF is how the adapter ends a session (§2)")
        self.assertFalse(self.proc.killed, "the app gets the grace window to flush and exit")
        self.assertFalse(self.action.is_checked())
        self.assertFalse(self.stored_config().enabled)
        self.assertIsNone(self.plugin.session)

    def test_a_child_that_ignores_the_stdin_eof_is_killed_after_the_grace_window(self):
        self.start_session()
        self.action.activate()

        self.assertEqual(
            [interval for interval, _callback, _args in STUBS.timeouts],
            [lxlyrics.STOP_GRACE_MS],
        )

        STUBS.run_timeouts()

        self.assertTrue(self.proc.killed)

    def test_a_child_that_exits_on_its_own_is_not_killed(self):
        self.start_session()
        self.action.activate()
        self.proc.feed_exit()

        STUBS.run_timeouts()

        self.assertFalse(self.proc.killed)

    def test_a_remembered_session_starts_when_the_plugin_activates(self):
        config = lxlyrics.SessionConfig()
        config.app_path = "/opt/lx-lyrics/lx-lyrics-app"
        config.enabled = True
        config.save()

        self.plugin.do_activate()

        self.assertEqual(self.proc.argv, ["/opt/lx-lyrics/lx-lyrics-app", "--player-feed"])
        self.assertTrue(self.action.is_checked())

    def test_an_app_that_cannot_run_leaves_the_toggle_off(self):
        STUBS.spawn_error = StubError("Failed to execute child process")

        self.plugin.do_activate()
        self.action.activate()

        self.assertEqual(STUBS.processes, [])
        self.assertFalse(self.action.is_checked())
        self.assertIsNone(self.plugin.session)
        self.assertFalse(self.stored_config().enabled)


class EventMappingTests(PluginTestCase):
    def test_a_track_change_sends_the_file_path_and_metadata(self):
        self.start_session()
        self.player.playing = True
        self.player.playing_time = (True, 10)

        self.player.emit(
            "playing-song-changed",
            STUBS.Entry(
                "file:///music/Artist/01%20-%20Song.mp3",
                title="Song",
                artist="Artist",
                album="Album",
            ),
        )

        self.assertEqual(
            self.written(),
            [
                '{"v":2,"action":"set_info","path":"/music/Artist/01 - Song.mp3",'
                '"singer":"Artist","name":"Song","album":"Album","lrc":"","tlrc":"",'
                '"rlrc":"","lxlrc":"","isPlay":true,"played_time":10000}\n'
            ],
        )

    def test_an_entry_without_metadata_still_sends_its_path(self):
        self.start_session()

        self.player.emit("playing-song-changed", STUBS.Entry("file:///music/plain.mp3"))

        self.assertEqual(
            self.written(),
            [
                '{"v":2,"action":"set_info","path":"/music/plain.mp3","singer":"","name":"",'
                '"album":"","lrc":"","tlrc":"","rlrc":"","lxlrc":"","isPlay":false,'
                '"played_time":0}\n'
            ],
        )

    def test_a_uri_change_without_an_entry_yet_does_not_stop_the_lyrics(self):
        self.start_session()

        self.player.emit("playing-uri-changed", "file:///music/streamed.mp3")

        self.assertEqual(
            self.written(),
            [
                '{"v":2,"action":"set_info","path":"/music/streamed.mp3","singer":"","name":"",'
                '"album":"","lrc":"","tlrc":"","rlrc":"","lxlrc":"","isPlay":false,'
                '"played_time":0}\n'
            ],
        )

    def test_no_entry_sends_empty_info_then_stop(self):
        self.start_session()

        self.player.emit("playing-song-changed", None)

        self.assertEqual(self.written(), [EMPTY_INFO_LINE, STOP_LINE])

    def test_a_non_utf8_path_is_replaced_rather_than_losing_the_track(self):
        self.start_session()

        output = io.StringIO()
        with contextlib.redirect_stdout(output):
            self.player.emit(
                "playing-song-changed",
                STUBS.Entry("file:///music/bad%FFname.mp3", title="Bad"),
            )

        self.assertEqual(
            self.written(),
            [
                '{"v":2,"action":"set_info","path":"/music/bad?name.mp3","singer":"",'
                '"name":"Bad","album":"","lrc":"","tlrc":"","rlrc":"","lxlrc":"",'
                '"isPlay":false,"played_time":0}\n'
            ],
        )
        self.assertIn("replacing unencodable characters", output.getvalue())
        self.assertTrue(self.action.is_checked(), "the session survives an unencodable path")

    def test_a_non_local_uri_has_no_path(self):
        self.start_session()

        self.player.emit("playing-uri-changed", "https://radio.example/stream")

        self.assertEqual(
            self.written(),
            [
                '{"v":2,"action":"set_info","path":"","singer":"","name":"","album":"",'
                '"lrc":"","tlrc":"","rlrc":"","lxlrc":"","isPlay":false,"played_time":0}\n'
            ],
        )

    def test_playing_resumes_at_the_position_rhythmbox_reports(self):
        self.start_session()
        self.player.playing_time = (True, 10)

        self.player.emit("playing-changed", True)

        self.assertEqual(self.written(), ['{"v":2,"action":"set_play","time":10000}\n'])

    def test_a_pause_is_sent_when_playback_stops(self):
        self.start_session()

        self.player.emit("playing-changed", False)

        self.assertEqual(self.written(), ['{"v":2,"action":"set_pause"}\n'])

    def test_the_position_falls_back_to_zero_when_rhythmbox_refuses_to_report_one(self):
        self.start_session()
        self.player.playing_time_error = True

        self.player.emit("playing-changed", True)

        self.assertEqual(self.written(), ['{"v":2,"action":"set_play","time":0}\n'])

    def test_status_updates_are_coalesced_to_one_per_500_ms(self):
        self.start_session()
        self.player.playing = True

        for elapsed_ns in (0, 100_000_000, 200_000_000, 600_000_000):
            self.player.emit("elapsed-nano-changed", elapsed_ns)

        self.assertEqual(
            self.written(),
            [
                '{"v":2,"action":"set_status","isPlay":true,"played_time":0}\n',
                '{"v":2,"action":"set_status","isPlay":true,"played_time":600}\n',
            ],
        )

    def test_a_jump_in_the_elapsed_clock_is_reported_as_a_seek(self):
        self.start_session()
        self.player.playing = True

        self.player.emit("elapsed-nano-changed", 30_000_000_000)
        self.player.emit("elapsed-nano-changed", 44_000_000_000)

        self.assertEqual(
            self.written(),
            [
                '{"v":2,"action":"set_status","isPlay":true,"played_time":30000}\n',
                '{"v":2,"action":"set_play","time":44000}\n',
            ],
        )

    def test_the_next_track_restarting_at_zero_is_not_a_seek(self):
        self.start_session()
        self.player.playing = True
        self.player.emit("elapsed-nano-changed", 90_000_000_000)
        self.clear_written()

        self.player.emit("playing-song-changed", STUBS.Entry("file:///music/next.mp3"))
        self.clear_written()

        self.player.emit("elapsed-nano-changed", 0)

        self.assertEqual(
            self.written(),
            ['{"v":2,"action":"set_status","isPlay":true,"played_time":0}\n'],
        )

    def test_only_host_actions_ever_leave_the_adapter(self):
        self.start_session()
        self.player.playing = True
        self.player.playing_time = (True, 3)
        self.player.emit("playing-song-changed", STUBS.Entry("file:///music/a.mp3", title="A"))
        self.player.emit("playing-changed", True)
        self.player.emit("elapsed-nano-changed", 0)
        self.player.emit("elapsed-nano-changed", 5_000_000_000)
        self.player.emit("playing-changed", False)
        self.player.emit("playing-song-changed", None)
        self.proc.stdout.feed_line('{"v":2,"action":"close_requested"}')

        host_actions = {"hello", "set_info", "set_status", "set_play", "set_pause", "set_stop"}
        lines = self.written()
        self.assertTrue(lines)
        for line in lines:
            message = json.loads(line)
            self.assertEqual(message["v"], 2)
            self.assertIn(message["action"], host_actions)
        self.assertNotIn("get_analyser_data_array", "".join(lines))


class AppToHostTests(PluginTestCase):
    def test_close_requested_clears_the_toggle_and_the_remembered_session(self):
        self.start_session()

        self.proc.stdout.feed_line('{"v":2,"action":"close_requested"}')

        self.assertFalse(self.action.is_checked())
        self.assertFalse(self.stored_config().enabled)
        self.assertIsNone(self.plugin.session)
        self.assertFalse(self.proc.killed, "the app closes itself; do not kill the animation")
        self.assertEqual(self.written(), [])

    def test_a_vanished_app_clears_the_toggle(self):
        self.start_session()

        self.proc.stdout.feed_eof()

        self.assertFalse(self.action.is_checked())
        self.assertFalse(self.stored_config().enabled)
        self.assertIsNone(self.plugin.session)

    def test_an_over_cap_app_line_is_a_protocol_error(self):
        self.start_session()

        over_cap = "x" * (lxlyrics.MAX_LINE_BYTES + 1)
        output = io.StringIO()
        with contextlib.redirect_stdout(output):
            self.proc.stdout.feed_line(over_cap)

        self.assertIn("protocol error", output.getvalue())
        self.assertIn(str(lxlyrics.MAX_LINE_BYTES + 1), output.getvalue())
        self.assertFalse(self.action.is_checked())
        self.assertFalse(self.stored_config().enabled)
        self.assertIsNone(self.plugin.session)
        self.assertTrue(self.proc.killed, "the child is not left running")

    def test_a_line_exactly_at_the_cap_is_not_a_protocol_error(self):
        self.start_session()

        at_cap = "x" * lxlyrics.MAX_LINE_BYTES
        output = io.StringIO()
        with contextlib.redirect_stdout(output):
            self.proc.stdout.feed_line(at_cap)

        self.assertNotIn("protocol error", output.getvalue())
        self.assertIn("ignoring non-JSON line", output.getvalue())
        self.assertTrue(self.action.is_checked(), "the cap is a limit, not a boundary error")

    def test_stray_output_on_stdout_is_ignored(self):
        self.start_session()
        self.player.playing = True

        self.proc.stdout.feed_line("warning: something leaked to stdout")
        self.player.emit("elapsed-nano-changed", 0)

        self.assertTrue(self.action.is_checked())
        self.assertEqual(
            self.written(),
            ['{"v":2,"action":"set_status","isPlay":true,"played_time":0}\n'],
        )

    def _assert_protocol_error_ends_the_session(self, line):
        self.start_session()

        output = io.StringIO()
        with contextlib.redirect_stdout(output):
            self.proc.stdout.feed_line(line)

        self.assertIn("protocol error", output.getvalue())
        self.assertFalse(self.action.is_checked())
        self.assertFalse(self.stored_config().enabled)
        self.assertIsNone(self.plugin.session)
        self.assertTrue(self.proc.killed, "a child that broke the protocol is not left running")

    def test_an_unsupported_version_is_a_protocol_error(self):
        self._assert_protocol_error_ends_the_session('{"v":3,"action":"close_requested"}')

    def test_an_unknown_action_is_a_protocol_error(self):
        self._assert_protocol_error_ends_the_session('{"v":2,"action":"who_knows"}')

    def test_an_unrequested_analyser_request_is_a_protocol_error(self):
        self._assert_protocol_error_ends_the_session('{"v":2,"action":"get_analyser_data_array"}')


class WritePathTests(PluginTestCase):
    """The bounded, never-blocking write path (the fd itself is non-blocking in Rhythmbox;
    these cases pin the queueing behaviour around it)."""

    def _emit_states(self, count):
        for index in range(1, count + 1):
            self.player.emit("elapsed-nano-changed", index * lxlyrics.STATUS_INTERVAL_NS)

    def test_a_stalled_child_keeps_the_queue_bounded_and_the_session_alive(self):
        self.start_session()
        self.player.playing = True

        output = io.StringIO()
        with contextlib.redirect_stdout(output):
            self._emit_states(lxlyrics.MAX_PENDING_LINES * 3)

        feed = self.plugin.session.feed
        self.assertEqual(len(feed.pending), lxlyrics.MAX_PENDING_LINES)
        self.assertIn(b'"played_time":96000}', feed.pending[-1], "the newest state is kept")
        self.assertNotIn(b'"played_time":1000}', feed.pending, "the oldest state is dropped")
        self.assertEqual(output.getvalue().count("not reading"), 1, "one line per episode")
        self.assertTrue(self.action.is_checked(), "a stalled app does not end the session")

    def test_the_overflow_warning_returns_after_the_queue_drains(self):
        self.start_session()
        self.player.playing = True

        output = io.StringIO()
        with contextlib.redirect_stdout(output):
            self._emit_states(lxlyrics.MAX_PENDING_LINES * 2)
            self.written()  # the app catches up: the queue empties
            self.assertEqual(self.plugin.session.feed.pending, [])
            self._emit_states(lxlyrics.MAX_PENDING_LINES * 2)

        self.assertEqual(output.getvalue().count("not reading"), 2)

    def test_a_stalled_child_can_still_be_stopped(self):
        self.start_session()
        self.player.playing = True
        self._emit_states(5)
        self.assertIsNotNone(self.proc.stdin.pending, "a write is stuck on the app")

        self.action.activate()  # toggle off while that write waits

        self.assertTrue(self.proc.stdin.closed)
        STUBS.run_timeouts()  # the grace timer still runs while the write is pending
        self.assertTrue(self.proc.killed)
        self.assertIsNone(self.plugin.session)
        self.assertFalse(self.stored_config().enabled)

    def test_a_short_write_keeps_the_line_in_one_piece(self):
        self.start_session()
        STUBS.short_write = 10  # a nearly full pipe takes only part of the line
        self.player.playing = True

        self.player.emit("playing-changed", True)

        self.assertEqual(self.written(), ['{"v":2,"action":"set_play","time":0}\n'])
        self.assertEqual(
            self.proc.stdin.raw,
            b'{"v":2,"action":"set_play","time":0}\n',
            "no byte lost, duplicated or interleaved",
        )
        self.assertEqual(self.plugin.session.feed.pending, [])

    def test_a_stalled_write_does_not_make_the_close_look_like_a_failure(self):
        self.start_session()
        self.player.playing = True
        self._emit_states(5)
        STUBS.close_error = StubError(
            "Stream has outstanding operation",
            domain=STUBS.Gio.io_error_quark(),
            code=STUBS.Gio.IOErrorEnum.PENDING,
        )

        output = io.StringIO()
        with contextlib.redirect_stdout(output):
            self.action.activate()

        self.assertNotIn("closing the app's stdin failed", output.getvalue())
        self.assertIsNone(self.plugin.session)
        self.assertFalse(self.stored_config().enabled)
        STUBS.run_timeouts()
        self.assertTrue(self.proc.killed)

    def test_a_real_close_failure_is_still_reported(self):
        self.start_session()
        STUBS.close_error = StubError("Input/output error")

        output = io.StringIO()
        with contextlib.redirect_stdout(output):
            self.action.activate()

        self.assertIn("closing the app's stdin failed", output.getvalue())
        self.assertIsNone(self.plugin.session)


class LifecycleTests(PluginTestCase):
    def test_deactivate_stops_the_app_and_drops_every_shell_reference(self):
        self.start_session()

        self.plugin.do_deactivate()

        self.assertTrue(self.proc.stdin.closed)
        self.assertEqual(self.application.removed_menu_items, [("view", "lxlyrics-toggle")])
        self.assertEqual(self.application.removed_actions, ["lxlyrics-toggle"])
        self.assertIsNone(self.plugin.action)
        self.assertIsNone(self.plugin.application)
        self.assertIsNone(self.plugin.shell_window)
        self.assertIsNone(self.plugin.shell_player)
        self.assertIsNone(self.plugin.shell)

    def test_a_failed_write_ends_the_session_instead_of_wedging(self):
        self.start_session()
        self.player.playing = True
        STUBS.write_error = StubError("Broken pipe")

        output = io.StringIO()
        with contextlib.redirect_stdout(output):
            self.player.emit("playing-changed", True)
            self.written()

        self.assertIn("writing to the app failed", output.getvalue())
        self.assertFalse(self.action.is_checked())
        self.assertFalse(self.stored_config().enabled)
        self.assertIsNone(self.plugin.session)
        self.assertTrue(self.proc.killed)

    def test_deactivate_remembers_that_the_session_was_on(self):
        self.start_session()

        self.plugin.do_deactivate()

        self.assertTrue(self.stored_config().enabled)

    def test_the_session_is_absent_until_it_is_toggled(self):
        self.plugin.do_activate()

        self.assertIsNone(self.plugin.session)
        self.assertEqual(STUBS.processes, [])


class ConfigTests(PluginTestCase):
    def test_the_key_file_lives_in_the_lx_lyrics_config_directory(self):
        self.assertEqual(
            lxlyrics.config_file_path(),
            os.path.join(self.xdg_dir, "lx-lyrics", "rhythmbox.conf"),
        )

    def test_the_preferences_pane_changes_the_executable_that_gets_spawned(self):
        pane = lxlyrics.LxLyricsConfig()
        pane.do_create_configure_widget()
        self.assertEqual(pane.app_path_entry.get_text(), "lx-lyrics-app")

        pane.app_path_entry.set_text("/usr/local/bin/lx-lyrics-app")

        self.assertEqual(self.stored_config().app_path, "/usr/local/bin/lx-lyrics-app")
        self.plugin.do_activate()
        self.action.activate()
        self.assertEqual(self.proc.argv[0], "/usr/local/bin/lx-lyrics-app")

        pane.app_path_entry.set_text("")
        self.assertEqual(self.stored_config().app_path, "/usr/local/bin/lx-lyrics-app")


if __name__ == "__main__":
    unittest.main()
