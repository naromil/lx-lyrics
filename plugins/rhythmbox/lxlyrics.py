# SPDX-License-Identifier: GPL-3.0-only
# Copyright (c) 2026 naromil
#
# Rhythmbox adapter: the player half of the lx-lyrics player feed.
#
# It observes RBShellPlayer through the plugin API, spawns the display app as a direct
# child and streams newline-delimited v2 JSON to the child's stdin. Acquisition, parsing
# and rendering all belong to the app; Rhythmbox has no analyser API (the visualizer
# plugin was removed in 2017), so the handshake declares `spectrum: false`.
#
# Nothing here imports Rhythmbox's shared `rb` plugin (the `RB` typelib below comes from
# librhythmbox-core, not from `rb`), so `lxlyrics.plugin` declares no `Depends=`. Adding
# one back would make libpeas report the dependency instead of the real problem when a
# prerequisite is missing — see the README.

import json
import os
import shutil
import signal

import gi

gi.require_version("Gtk", "3.0")
gi.require_version("Peas", "1.0")
gi.require_version("PeasGtk", "1.0")
gi.require_version("RB", "3.0")
# pygobject requires the version pins above to run before this import, so it cannot be
# hoisted to the top of the file.
from gi.repository import Gio, GLib, GObject, Gtk, Peas, PeasGtk, RB  # noqa: E402

PROTOCOL_VERSION = 2
HOST_NAME = "rhythmbox"
APP_ARGUMENT = "--player-feed"

# The desktop-lyrics session is toggled from Rhythmbox's View menu; the plugin's own
# enable/disable is Rhythmbox's Plugins page and is not duplicated here.
ACTION_ID = "lxlyrics-toggle"
ACTION_PATH = "view"
ACTION_LABEL = "LX Lyrics"

# One `set_status` per 500 ms of playback. A jump of a second or more in the elapsed
# clock cannot be normal progress, so it is read as a user seek: Rhythmbox has no seek
# signal (same heuristic as the upstream webremote plugin).
STATUS_INTERVAL_NS = 500 * 1000 * 1000
SEEK_THRESHOLD_NS = 1000 * 1000 * 1000
NS_PER_MS = 1000 * 1000

# How long a child gets to exit on its own after its stdin is closed (protocol §2) before
# it is killed: the app flushes its debounced settings on the way out.
STOP_GRACE_MS = 1000

# Lines queued for a child that is not draining. Past this the oldest stale lines are
# dropped: the newest `set_info`/`set_play`/`set_status` supersede them, and a stalled app
# must not grow the plugin's memory.
MAX_PENDING_LINES = 64

# Protocol §2/§7: a line over 1 MiB is a protocol error. The cap counts the line's bytes
# without its newline, which is the same count the app and the other adapters use.
MAX_LINE_BYTES = 1024 * 1024

# The app's stdout is read in bounded chunks and assembled into lines here. PyGObject does
# not expose the `length` argument of `g_data_input_stream_read_upto_async()` (verified on
# PyGObject 3.58/GLib 2.90: the wrapper takes stop_chars only), so `read_upto_async()`
# itself would still grow GLib's buffer without bound before a newline arrives. A bounded
# `read_bytes_async()` accumulator keeps the cap in our hands and the main loop free.
READ_CHUNK_BYTES = 64 * 1024

CONFIG_GROUP = "lx-lyrics"
DEFAULT_APP_PATH = "lx-lyrics-app"

# The global shortcut is owned by the separate `lx-lyrics-hotkey` GIO helper
# (`lyrics-app/src/hotkey/`), the same fd-3 transport the C adapters use: the plugin
# passes the write end of a pipe as descriptor 3, the helper reports `ready <trigger>`,
# `unavailable <reason>` and one `toggle` per press. It runs while the plugin is loaded,
# not per lyrics session, so the shortcut can open the window as well as close it.
HOTKEY_NAME = "lx-lyrics-hotkey"
HOTKEY_PIPE_FD = 3
# Status lines are ours and tiny; anything longer than this is not a helper report and is
# dropped whole rather than buffered.
HOTKEY_STATUS_BYTES = 4096
HOTKEY_READ_CHUNK_BYTES = 4096
HOTKEY_STOP_GRACE_MS = 1000


def config_file_path():
    """`$XDG_CONFIG_HOME/lx-lyrics/rhythmbox.conf` (never touching Rhythmbox's own files)."""
    base = os.environ.get("XDG_CONFIG_HOME")
    if not base:
        base = os.path.join(os.path.expanduser("~"), ".config")
    return os.path.join(base, "lx-lyrics", "rhythmbox.conf")


def _playing_time_ms(player):
    """Position in milliseconds.

    `rb_shell_player_get_playing_time()` introspects as `(success, seconds)` and raises
    while nothing is playing.
    """
    try:
        _success, seconds = player.get_playing_time()
    except (GLib.Error, TypeError, ValueError):
        return 0
    return int(seconds) * 1000


def _is_playing(player):
    """`rb_shell_player_get_playing()` introspects as `(success, playing)`."""
    try:
        _success, playing = player.get_playing()
    except (GLib.Error, TypeError, ValueError):
        return False
    return bool(playing)


def _entry_string(entry, prop):
    """A RhythmDB string property, empty when the entry has none."""
    if entry is None:
        return ""
    try:
        value = entry.get_string(prop)
    except (KeyError, TypeError):
        return ""
    return value or ""


def _entry_uri(entry):
    if entry is None:
        return ""
    try:
        uri = entry.get_playback_uri()
    except (AttributeError, TypeError):
        return ""
    return uri or ""


def _path_from_uri(uri):
    """Local filesystem path of a playback uri, empty for anything non-local."""
    if not uri:
        return ""
    try:
        path, _host = GLib.filename_from_uri(uri)
    except (GLib.Error, TypeError, ValueError):
        print("lxlyrics: no local path for %r" % uri)
        return ""
    return path or ""


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


class SessionConfig:
    """The plugin's two settings, in a GLib.KeyFile.

    `Gio.Settings` would mean shipping and compiling a GSettings schema for what is a
    user-level plugin, so the session state lives in a plain key file.
    """

    def __init__(self, path=None):
        self.path = path or config_file_path()
        self.enabled = False
        self.app_path = DEFAULT_APP_PATH

    def load(self):
        keyfile = GLib.KeyFile.new()
        try:
            keyfile.load_from_file(self.path, GLib.KeyFileFlags.NONE)
        except GLib.Error:
            return  # no key file yet: the defaults above are the initial state
        try:
            self.enabled = keyfile.get_boolean(CONFIG_GROUP, "enabled")
        except GLib.Error:
            pass
        try:
            app_path = keyfile.get_string(CONFIG_GROUP, "app-path")
        except GLib.Error:
            app_path = ""
        if app_path:
            self.app_path = app_path

    def save(self):
        keyfile = GLib.KeyFile.new()
        keyfile.set_boolean(CONFIG_GROUP, "enabled", self.enabled)
        keyfile.set_string(CONFIG_GROUP, "app-path", self.app_path)
        try:
            os.makedirs(os.path.dirname(self.path), exist_ok=True)
            keyfile.save_to_file(self.path)
        except (GLib.Error, OSError) as err:
            print("lxlyrics: cannot write %s: %s" % (self.path, err))


def _safe_text(value):
    """`value` as UTF-8-encodable text: GObject strings can carry lone surrogates.

    A file name that is not valid UTF-8 is decoded with `surrogateescape`, so text taken
    from Rhythmbox may hold characters that no UTF-8 encoder accepts — and PyGObject
    marshals every string handed to a C function as strict UTF-8, `print()` included.
    """
    if not isinstance(value, str):
        value = str(value)
    return value.encode("utf-8", "replace").decode("utf-8")


def _line_bytes(message):
    """One protocol line as UTF-8 bytes, replacing what UTF-8 cannot carry.

    The wire is UTF-8 (protocol §6), so an unencodable character is replaced — loudly,
    naming the track — rather than raising inside a signal handler and losing the update.
    """
    line = json.dumps(message, ensure_ascii=False, separators=(",", ":")) + "\n"
    try:
        return line.encode("utf-8")
    except UnicodeEncodeError:
        label = message.get("name") or message.get("path") or message.get("action") or "?"
        print("lxlyrics: replacing unencodable characters for %s" % _safe_text(label))
        return line.encode("utf-8", "replace")


def resolve_hotkey_path(app_path):
    """The helper next to the resolved app executable, else `lx-lyrics-hotkey` on PATH."""
    resolved = app_path or DEFAULT_APP_PATH
    if os.sep not in resolved:
        resolved = shutil.which(resolved) or resolved
    directory = os.path.dirname(resolved)
    if directory:
        candidate = os.path.join(directory, HOTKEY_NAME)
        if os.access(candidate, os.X_OK):
            return candidate
    return shutil.which(HOTKEY_NAME)


def _pipe_input_stream(fd):
    """A GIO input stream over `fd`, closing it when the stream is closed."""
    try:
        # GLib >= 2.80/PyGObject >= 3.50 expose the non-deprecated namespace.
        from gi.repository import GioUnix  # noqa: F401

        stream = GioUnix.InputStream.new(fd, True)
    except (ImportError, AttributeError):
        stream = Gio.UnixInputStream.new(fd, True)
    return Gio.DataInputStream.new(stream)


class HotkeyHelper:
    """The `lx-lyrics-hotkey` child and the plugin's end of its fd-3 pipe.

    The helper is optional: a missing binary, a failed spawn or a pipe error logs one line
    and leaves the plugin fully usable without a shortcut. `toggle` flips the same session
    switch the View menu drives; the one-shot `ready`/`unavailable` reports are logged
    once, and an overlong or malformed line is ignored, so the helper can never take the
    plugin down with it.
    """

    def __init__(self, app_path, on_toggle, log=print, warn=print):
        self.app_path = app_path
        self.on_toggle = on_toggle
        self.log = log
        self.warn = warn
        self.proc = None
        self.stream = None
        self.read_fd = None
        self.buffer = bytearray()
        self.discarding = False
        self.child_exited = False
        self.ready_reported = False
        self.unavailable_reported = False
        self.eof_reported = False

    def start(self):
        """Spawn the helper. False (with one log line) when it cannot run."""
        if self.proc is not None or self.stream is not None:
            return True
        path = resolve_hotkey_path(self.app_path)
        if path is None:
            self.warn(
                "lxlyrics: %s not found; the global shortcut is unavailable" % HOTKEY_NAME
            )
            return False
        try:
            read_fd, write_fd = os.pipe()
        except OSError as err:
            self.warn("lxlyrics: cannot create the shortcut pipe: %s" % err)
            return False
        launcher = Gio.SubprocessLauncher.new(Gio.SubprocessFlags.NONE)
        launcher.take_fd(write_fd, HOTKEY_PIPE_FD)
        try:
            self.proc = launcher.spawnv([path, "--pipe-fd", str(HOTKEY_PIPE_FD)])
        except GLib.Error as err:
            self.warn(
                "lxlyrics: cannot run %s: %s; the global shortcut is unavailable" % (path, err)
            )
            os.close(read_fd)
            return False
        finally:
            # `take_fd` leaves the parent's descriptor open; closing it here makes the
            # child's fd 3 the only write end, so its EOF is also the helper's quit trigger.
            try:
                os.close(write_fd)
            except OSError:
                pass
        self.read_fd = read_fd
        self.stream = _pipe_input_stream(read_fd)
        self.read_status()
        return True

    def read_status(self):
        if self.stream is None:
            return
        self.stream.read_bytes_async(
            HOTKEY_READ_CHUNK_BYTES, GLib.PRIORITY_DEFAULT, None, self._status_read
        )

    def _status_read(self, stream, result):
        if self.stream is None:
            return
        try:
            chunk = stream.read_bytes_finish(result)
        except GLib.Error as err:
            self.warn("lxlyrics: reading from %s failed: %s" % (HOTKEY_NAME, err))
            self.stop()
            return
        data = chunk.get_data() if chunk is not None else b""
        if not data:
            if not self.eof_reported:
                self.eof_reported = True
                self.log("lxlyrics: the global shortcut helper exited")
            self.stop()
            return
        self._consume_status(data)
        self.read_status()

    def _consume_status(self, data):
        """Split raw fd-3 bytes into bounded lines, dropping anything over the cap."""
        index = 0
        while index < len(data):
            newline = data.find(b"\n", index)
            if newline < 0:
                piece = data[index:]
                if not self.discarding:
                    if len(self.buffer) + len(piece) > HOTKEY_STATUS_BYTES:
                        self.buffer.clear()
                        self.discarding = True
                    else:
                        self.buffer += piece
                return
            piece = data[index:newline]
            index = newline + 1
            if self.discarding:
                self.discarding = False
                continue
            if len(self.buffer) + len(piece) > HOTKEY_STATUS_BYTES:
                self.buffer.clear()
                continue
            line = bytes(self.buffer) + piece
            self.buffer.clear()
            self.handle_status_line(line)

    def handle_status_line(self, line):
        """One fd-3 line, already bounded: dispatch, or ignore what is not a report."""
        if isinstance(line, bytes):
            try:
                text = line.decode("utf-8")
            except UnicodeDecodeError:
                self.log("lxlyrics: ignoring a non-UTF-8 shortcut line")
                return
        else:
            text = line
        text = text.rstrip("\r")
        if not text:
            return
        if text == "toggle":
            self.on_toggle()
            return
        if text.startswith("ready "):
            if not self.ready_reported:
                self.ready_reported = True
                self.log("lxlyrics: global shortcut bound to %s" % text[len("ready ") :])
            return
        if text.startswith("unavailable "):
            if not self.unavailable_reported:
                self.unavailable_reported = True
                self.warn(
                    "lxlyrics: global shortcut unavailable (%s)" % text[len("unavailable ") :]
                )
            return
        self.log("lxlyrics: shortcut helper: %s" % text)

    def stop(self):
        """Quit the helper: close the pipe, terminate, grace, kill. Idempotent."""
        proc, self.proc = self.proc, None
        stream, self.stream = self.stream, None
        fd, self.read_fd = self.read_fd, None
        self.buffer.clear()
        self.discarding = False
        closed = False
        if stream is not None:
            try:
                stream.close()
                closed = True
            except GLib.Error as err:
                self.warn("lxlyrics: closing the shortcut pipe failed: %s" % err)
        if not closed and fd is not None:
            try:
                os.close(fd)
            except OSError:
                pass
        if proc is None:
            return
        try:
            proc.send_signal(signal.SIGTERM)
        except GLib.Error as err:
            self.log("lxlyrics: terminating the shortcut helper failed: %s" % err)
        self.child_exited = False
        proc.wait_async(None, self._child_exited)
        GLib.timeout_add(HOTKEY_STOP_GRACE_MS, self._kill_lingering, proc)

    def _child_exited(self, _proc, _result):
        """The helper exited inside its grace window: nothing left to kill."""
        self.child_exited = True

    def _kill_lingering(self, proc):
        """A helper that ignored SIGTERM gets killed, once."""
        if not self.child_exited:
            proc.force_exit()
        return GLib.SOURCE_REMOVE


class FeedWriter:
    """The app process and the host end of the feed.

    GLib allows only one async write per stream: a second one fails with
    `G_IO_ERROR_PENDING`, so writes are queued here and pumped one at a time, which is
    also what keeps `hello` ahead of the first snapshot. Reads are armed one at a time
    for the same reason, so neither direction ever blocks Rhythmbox's main loop.

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
        self.line_buffer = bytearray()
        self.writing = False
        self.child_exited = False
        self.stalled = False

    def start(self):
        """Spawn the app and write the handshake. Raises GLib.Error if it cannot run."""
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
                print("lxlyrics: the app is not reading; dropping stale lines")
        self._write_next()

    def read_line(self):
        if self.stdout is None:
            return
        self.stdout.read_bytes_async(
            READ_CHUNK_BYTES, GLib.PRIORITY_DEFAULT, None, self._line_read
        )

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
        self.line_buffer.clear()
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
                    print("lxlyrics: closing the app's stdin failed: %s" % err)
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
                # rather than leaving a half-dead session with a checked toggle.
                print("lxlyrics: writing to the app failed: %s" % err)
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
        if self.stdout is None:
            return
        try:
            chunk = stream.read_bytes_finish(result)
        except GLib.Error as err:
            print("lxlyrics: reading from the app failed: %s" % err)
            self._finished()
            return
        data = chunk.get_data() if chunk is not None else b""
        if not data:
            # EOF. A final line without its newline is delivered like the old line reader
            # did, then the session ends.
            if self.line_buffer:
                line = bytes(self.line_buffer)
                self.line_buffer.clear()
                if not self._deliver_line(line):
                    return
            self._finished()
            return
        self.line_buffer += data
        if not self._process_lines():
            return
        self.read_line()

    def _process_lines(self):
        """Deliver every complete line buffered so far. False when the feed has ended."""
        buffer = self.line_buffer
        while True:
            newline = buffer.find(b"\n")
            if newline < 0:
                break
            line = bytes(buffer[:newline])
            del buffer[: newline + 1]
            if not self._deliver_line(line):
                return False
        # §7: a line still without a newline cannot grow past the cap. Exactly cap bytes
        # are fine — the next chunk decides whether it ends here or is an over-cap line.
        if len(buffer) > MAX_LINE_BYTES:
            self.fail(
                "app→host line is %d bytes without a newline (over the %d cap)"
                % (len(buffer), MAX_LINE_BYTES)
            )
            return False
        return True

    def _deliver_line(self, line):
        """One complete app→host line: cap first, then strict UTF-8, then the handler."""
        if len(line) > MAX_LINE_BYTES:
            self.fail("app→host line is %d bytes (over the %d cap)" % (len(line), MAX_LINE_BYTES))
            return False
        try:
            text = line.decode("utf-8")
        except UnicodeDecodeError as err:
            self.fail("app→host line is not valid UTF-8: %s" % err)
            return False
        if self.on_line is not None:
            self.on_line(text)
        # A handler that ended the feed (protocol error, child gone) must not re-arm.
        return self.stdout is not None

    def _finished(self):
        """The child is gone: drop the pipes and report it exactly once."""
        on_exit, self.on_exit = self.on_exit, None
        self.stop(force=True)
        if on_exit is not None:
            on_exit()

    def fail(self, reason):
        """A protocol error from the child (§7): log loudly, end the session, no respawn."""
        print("lxlyrics: protocol error from the app: %s" % reason)
        self._finished()


class LyricsSession:
    """Maps Rhythmbox playback events onto v2 feed lines."""

    def __init__(self, player, config, on_close_requested, on_exit):
        self.player = player
        self.config = config
        self.on_close_requested = on_close_requested
        self.on_exit = on_exit
        self.feed = None
        self.signal_ids = []
        self.last_status_ns = None
        self.last_elapsed_ns = None

    def start(self):
        """Spawn the app and start streaming. False if the app cannot be run."""
        self.feed = FeedWriter(self.config.app_path, self._app_line, self._app_exited)
        try:
            self.feed.start()
        except GLib.Error as err:
            print("lxlyrics: cannot run %s: %s" % (self.config.app_path, err))
            self.feed = None
            return False
        self.signal_ids = [
            self.player.connect("playing-changed", self._playing_changed),
            self.player.connect("playing-song-changed", self._playing_song_changed),
            self.player.connect("playing-uri-changed", self._playing_uri_changed),
            self.player.connect("elapsed-nano-changed", self._elapsed_nano_changed),
        ]
        # `hello` is already queued, so the app reads the snapshot after the handshake.
        self._push_info(self.player.get_playing_entry())
        return True

    def stop(self, kill=True):
        player, self.player = self.player, None
        if player is not None:
            for signal_id in self.signal_ids:
                player.disconnect(signal_id)
        self.signal_ids = []
        feed, self.feed = self.feed, None
        if feed is not None:
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
            # (a Qt component writing to stdout must not break the feed).
            print("lxlyrics: ignoring non-JSON line from the app: %s" % line)
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
        """§7: log loudly and end the session — no respawn, no stale toggle state."""
        if self.feed is not None:
            self.feed.fail(reason)

    def _app_exited(self):
        print("lxlyrics: the app exited")
        self.on_exit()

    def _playing_changed(self, _player, playing):
        if playing:
            self._send(
                {
                    "v": PROTOCOL_VERSION,
                    "action": "set_play",
                    "time": _playing_time_ms(self.player),
                }
            )
        else:
            self._send({"v": PROTOCOL_VERSION, "action": "set_pause"})

    def _playing_song_changed(self, _player, entry):
        self._push_info(entry)

    def _playing_uri_changed(self, _player, uri):
        # The uri can change before the entry is in place; the uri alone still gives the
        # app a path, and `playing-song-changed` fills in the metadata next.
        entry = self.player.get_playing_entry() if uri else None
        self._push_info(entry, uri)

    def _push_info(self, entry, uri=None):
        self.last_status_ns = None
        self.last_elapsed_ns = None
        if entry is None and not uri:
            self._send(_info_line("", "", "", "", False, 0))
            self._send({"v": PROTOCOL_VERSION, "action": "set_stop"})
            return
        path = _path_from_uri(_entry_uri(entry) or uri)
        self._send(
            _info_line(
                path,
                _entry_string(entry, RB.RhythmDBPropType.ARTIST),
                _entry_string(entry, RB.RhythmDBPropType.TITLE),
                _entry_string(entry, RB.RhythmDBPropType.ALBUM),
                _is_playing(self.player),
                _playing_time_ms(self.player),
            )
        )

    def _elapsed_nano_changed(self, _player, elapsed_ns):
        elapsed_ns = int(elapsed_ns)
        previous_ns, self.last_elapsed_ns = self.last_elapsed_ns, elapsed_ns
        if previous_ns is not None and abs(elapsed_ns - previous_ns) >= SEEK_THRESHOLD_NS:
            self.last_status_ns = elapsed_ns
            self._send(
                {
                    "v": PROTOCOL_VERSION,
                    "action": "set_play",
                    "time": elapsed_ns // NS_PER_MS,
                }
            )
            return
        if (
            self.last_status_ns is None
            or abs(elapsed_ns - self.last_status_ns) >= STATUS_INTERVAL_NS
        ):
            self.last_status_ns = elapsed_ns
            self._send(
                {
                    "v": PROTOCOL_VERSION,
                    "action": "set_status",
                    "isPlay": _is_playing(self.player),
                    "played_time": elapsed_ns // NS_PER_MS,
                }
            )


class LxLyrics(GObject.Object, Peas.Activatable):
    """The activatable: one checkable View-menu action owning one lyrics session."""

    __gtype_name__ = "LxLyrics"

    object = GObject.property(type=GObject.Object)

    def __init__(self):
        GObject.Object.__init__(self)
        self.shell = None
        self.shell_player = None
        self.shell_window = None
        self.application = None
        self.action = None
        self.session = None
        self.hotkey = None
        self.config = SessionConfig()

    def do_activate(self):
        shell = self.object
        self.shell = shell
        self.shell_player = shell.props.shell_player
        self.shell_window = shell.props.window
        self.application = shell.props.application
        self._add_menu_action()
        self.config.load()
        # The shortcut belongs to the plugin, not the session: it must open the window
        # while no session is running, and a missing helper is not an activation failure.
        hotkey = HotkeyHelper(self.config.app_path, self._on_toggle)
        if hotkey.start():
            self.hotkey = hotkey
        if self.config.enabled:
            self._start_session()
        self._update_action_state()

    def do_deactivate(self):
        # Stop the session, but remember whether it was on: disabling the plugin in
        # Rhythmbox's Plugins page is not the same as turning desktop lyrics off.
        self._stop_session(remember_off=False)
        hotkey, self.hotkey = self.hotkey, None
        if hotkey is not None:
            hotkey.stop()
        if self.application is not None:
            self.application.remove_plugin_menu_item(ACTION_PATH, ACTION_ID)
            self.application.remove_action(ACTION_ID)
        self.action = None
        # Holding shell references past deactivate keeps Rhythmbox from exiting.
        self.application = None
        self.shell_window = None
        self.shell_player = None
        self.shell = None

    def _add_menu_action(self):
        # `SimpleAction.new()` makes a *stateless* action, and `set_state()` on one is a
        # no-op (g_simple_action_set_state asserts state_type != NULL), which Rhythmbox
        # would render as a plain menu item. `new_stateful` is what makes it checkable.
        self.action = Gio.SimpleAction.new_stateful(
            ACTION_ID, None, GLib.Variant.new_boolean(False)
        )
        self.action.connect("activate", self._on_toggle)
        self.application.add_action(self.action)
        self.application.add_plugin_menu_item(
            ACTION_PATH,
            ACTION_ID,
            Gio.MenuItem.new(
                label=ACTION_LABEL,
                detailed_action="app.%s" % ACTION_ID,
            ),
        )

    def _on_toggle(self, _action=None, _parameter=None):
        """The View-menu action and the global shortcut share this one switch."""
        if self.session is None:
            self._start_session()
        else:
            self._stop_session(remember_off=True)
        self._update_action_state()

    def _update_action_state(self):
        if self.action is not None:
            self.action.set_state(GLib.Variant.new_boolean(self.session is not None))

    def _start_session(self):
        if self.session is not None:
            return
        # Re-read: the preferences pane edits the app path outside this instance.
        self.config.load()
        session = LyricsSession(
            self.shell_player,
            self.config,
            on_close_requested=self._on_close_requested,
            on_exit=self._on_app_exited,
        )
        if session.start():
            self.session = session
            self.config.enabled = True
        else:
            self.config.enabled = False
        self.config.save()

    def _stop_session(self, remember_off, kill=True):
        session, self.session = self.session, None
        if session is not None:
            session.stop(kill=kill)
        if remember_off:
            self.config.enabled = False
            self.config.save()
        self._update_action_state()

    def _on_close_requested(self):
        # The user closed the lyric window: it is quitting by itself, so do not kill it.
        print("lxlyrics: the lyrics window was closed")
        self._stop_session(remember_off=True, kill=False)

    def _on_app_exited(self):
        self._stop_session(remember_off=True)


class LxLyricsConfig(GObject.Object, PeasGtk.Configurable):
    """Preferences pane: the executable the adapter spawns."""

    __gtype_name__ = "LxLyricsConfig"

    object = GObject.property(type=GObject.Object)

    def __init__(self):
        GObject.Object.__init__(self)
        self.config = SessionConfig()
        self.app_path_entry = None

    def do_create_configure_widget(self):
        self.config.load()
        grid = Gtk.Grid()
        grid.set_row_spacing(6)
        grid.set_column_spacing(12)
        grid.set_border_width(12)

        label = Gtk.Label(label="Lyrics app executable")
        label.set_halign(Gtk.Align.START)

        self.app_path_entry = Gtk.Entry()
        self.app_path_entry.set_hexpand(True)
        self.app_path_entry.set_text(self.config.app_path)
        self.app_path_entry.connect("changed", self._app_path_changed)

        hint = Gtk.Label(label="The app is started as <executable> %s." % APP_ARGUMENT)
        hint.set_halign(Gtk.Align.START)

        grid.attach(label, 0, 0, 1, 1)
        grid.attach(self.app_path_entry, 1, 0, 1, 1)
        grid.attach(hint, 0, 1, 2, 1)
        return grid

    def _app_path_changed(self, entry):
        path = entry.get_text().strip()
        if path and path != self.config.app_path:
            self.config.app_path = path
            self.config.save()


GObject.type_register(LxLyricsConfig)
