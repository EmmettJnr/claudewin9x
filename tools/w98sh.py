#!/usr/bin/env python3
"""w98sh - a DOS prompt for a retro Windows machine, from a modern terminal.

Talks to the ClaudeWin9x server's remote (MCP) endpoint, the same one Claude
Code uses, so ClaudeWin9x-Remote must be running and connected on the retro
machine. Commands run through COMMAND.COM there; their output comes back
when they finish.

    w98sh                      interactive prompt
    w98sh dir C:\\WINDOWS       run one command (or a w98sh command) and exit
    w98sh -d C:\\SRC nmake      ...in a given directory

Configuration, from the environment or ~/.config/w98sh.ini:

    [w98sh]
    url = http://server:5003/mcp
    token = <remote_token from the server's server.ini>

W98SH_URL and W98SH_TOKEN override the file.
"""

import argparse
import base64
import configparser
import json
import ntpath
import os
import re
import shlex
import shutil
import subprocess
import sys
import tempfile
import urllib.error
import urllib.request

try:
    import readline
except ImportError:  # e.g. Windows without pyreadline
    readline = None

CONFIG_PATH = os.path.join(
    os.environ.get("XDG_CONFIG_HOME") or os.path.expanduser("~/.config"), "w98sh.ini")
HISTORY_PATH = os.path.join(
    os.environ.get("XDG_STATE_HOME") or os.path.expanduser("~/.local/state"), "w98sh_history")

DEFAULT_TIMEOUT = 120
# Largest base64 page the server returns per read_file call
READ_CHUNK = 262144
# COMMAND.COM keeps ~127 characters of a command line, and the client adds
# its own wrapper and output redirection
LONG_COMMAND = 85

HELP = """\
Anything else you type runs on the retro machine through COMMAND.COM.

  cd [dir], X:          change directory or drive (kept here, like DOS)
  get REMOTE [LOCAL]    copy a file from the retro machine to this one
  put LOCAL [REMOTE]    copy a file from this machine to the retro one
  edit REMOTE           edit a retro file in $EDITOR here, then save it back
  lcd [dir]             change or show the local directory
  !command              run a command on this machine
  status                show the retro machine's connection status
  cls                   clear the screen
  help                  this help
  exit                  leave (or Ctrl-D)

Not like a real DOS prompt: programs get no keyboard input (so no EDIT,
games or prompts), output appears when a command finishes, and Ctrl-C only
stops waiting - the command keeps running there (the client kills it after
10 minutes). Exit codes from COMMAND.COM are unreliable."""

BUILTINS = ["cd", "chdir", "get", "put", "edit", "lcd", "status", "cls", "help", "exit", "quit"]


class RemoteError(Exception):
    pass


class Remote:
    """Calls the server's MCP tools (JSON-RPC over HTTP, bearer token)."""

    def __init__(self, url, token):
        self.url = url
        self.token = token
        self._next_id = 0

    def _post(self, method, params, timeout):
        self._next_id += 1
        body = json.dumps({"jsonrpc": "2.0", "id": self._next_id, "method": method,
                           "params": params}).encode()
        request = urllib.request.Request(self.url, body, {
            "Content-Type": "application/json",
            "Accept": "application/json, text/event-stream",
            "Authorization": "Bearer " + self.token,
        })
        try:
            with urllib.request.urlopen(request, timeout=timeout) as response:
                reply = json.load(response)
        except urllib.error.HTTPError as e:
            if e.code == 401:
                raise RemoteError("the server rejected the token") from None
            raise RemoteError(f"the server returned HTTP {e.code}") from None
        except urllib.error.URLError as e:
            raise RemoteError(f"cannot reach {self.url}: {e.reason}") from None
        except TimeoutError:
            raise RemoteError("timed out waiting for the server") from None

        if "error" in reply:
            raise RemoteError(reply["error"].get("message", "unknown error"))
        return reply["result"]

    def initialize(self):
        self._post("initialize", {"protocolVersion": "2025-06-18", "capabilities": {},
                                  "clientInfo": {"name": "w98sh", "version": "1.0"}}, 15)

    def call(self, tool, arguments=None, timeout=90):
        result = self._post("tools/call", {"name": tool, "arguments": arguments or {}}, timeout)
        text = "".join(c.get("text", "") for c in result.get("content", []) if c.get("type") == "text")
        if result.get("isError"):
            raise RemoteError(text)
        return text


def split_args(text):
    """Splits on spaces, keeping "quoted strings" (DOS style, no escapes)."""
    return [quoted or bare for quoted, bare in re.findall(r'"([^"]*)"|(\S+)', text)]


class Shell:
    def __init__(self, remote, timeout):
        self.remote = remote
        self.timeout = timeout
        self.drive = "C:"
        self.cwd = {"C:": "\\"}  # current directory per drive, as DOS keeps it
        self._listings = {}  # directory -> (dirs, files), until the next command
        self._real_names = {}  # lower-case path -> path as the retro machine spells it

    @property
    def path(self):
        return self.drive + self.cwd.get(self.drive, "\\")

    def resolve(self, path):
        """Absolute retro path for a path typed relative to the current directory."""
        drive, rest = ntpath.splitdrive(path.replace("/", "\\"))
        drive = drive.upper() or self.drive
        if not rest.startswith("\\"):
            rest = ntpath.join(self.cwd.get(drive, "\\"), rest)
        full = ntpath.normpath(drive + rest)
        return full if not full.endswith(":") else full + "\\"

    # --- remote helpers

    def listing(self, path):
        """(dirs, files) in a retro directory; cached until the next command."""
        if path not in self._listings:
            dirs, files = [], []
            for line in self.remote.call("list_directory", {"path": path}).splitlines():
                m = re.match(r"^\s+(<DIR>|\d+)  (.+)$", line)
                if m:
                    (dirs if m.group(1) == "<DIR>" else files).append(m.group(2))
            self._listings[path] = (dirs, files)
        return self._listings[path]

    def read_bytes(self, path):
        data = bytearray()
        while True:
            text = self.remote.call("read_file", {"path": path, "encoding": "base64",
                                                  "offset": len(data), "limit": READ_CHUNK}, 330)
            m = re.match(r"\[bytes \d+-\d+ of (\d+);[^\]]*\]\n", text)
            if m:
                data += base64.b64decode(text[m.end():])
                if len(data) >= int(m.group(1)):
                    return bytes(data)
            else:
                return bytes(data + base64.b64decode(text))

    def write_bytes(self, path, data):
        return self.remote.call("write_file", {"path": path, "encoding": "base64",
                                               "content": base64.b64encode(data).decode()}, 330)

    # --- commands

    def run_line(self, line):
        """Runs one line; returns an exit status."""
        line = line.strip()
        if not line:
            return 0
        self._listings.clear()

        if line.startswith("!"):
            return subprocess.call(line[1:], shell=True)

        m = re.match(r"^([a-zA-Z]):$", line)
        if m:
            return self.change_drive(m.group(1).upper() + ":")

        m = re.match(r"^(cd|chdir)(?=$|[\s\\.])\s*(.*)$", line, re.IGNORECASE)
        if m:
            return self.change_dir(m.group(2).strip().strip('"'))

        word, _, rest = line.partition(" ")
        handler = {
            "get": self.get, "put": self.put, "edit": self.edit, "lcd": self.lcd,
            "status": self.status, "cls": self.cls, "help": self.help,
        }.get(word.lower())
        if handler:
            return handler(split_args(rest))
        return self.run(line)

    def run(self, line):
        if len(line) > LONG_COMMAND:
            print("w98sh: long command lines get cut off by COMMAND.COM; "
                  "a batch file (edit X.BAT) may work better", file=sys.stderr)

        text = self.remote.call("run_command", {
            "command": line, "working_directory": self.path, "timeout_seconds": self.timeout,
        }, self.timeout + 30)

        first, _, output = text.partition("\n")
        m = re.match(r"Exit code: (-?\d+)", first)
        code = int(m.group(1)) if m else 0
        if output != "(no output)":
            sys.stdout.write(output if output.endswith("\n") else output + "\n")
        if code:
            print(f"[exit code {code}]")
        return code

    def change_drive(self, drive):
        try:
            self.listing(drive + "\\")
        except RemoteError:
            print("Invalid drive specification")
            return 1
        self.drive = drive
        self.cwd.setdefault(drive, "\\")
        return 0

    def change_dir(self, arg):
        if not arg:
            print(self.path)
            return 0
        if re.match(r"^[a-zA-Z]:$", arg):
            drive = arg.upper()
            print(drive + self.cwd.get(drive, "\\"))
            return 0

        try:
            target = self.real_dir(self.resolve(arg))
        except RemoteError:
            target = None
        if target is None:
            print("Invalid directory")
            return 1

        self.cwd[target[:2]] = target[2:]
        return 0

    def real_dir(self, path):
        """Checks a directory exists and returns it with its real capitalisation."""
        drive, rest = ntpath.splitdrive(path)
        real = drive + "\\"
        if not rest.strip("\\"):
            self.listing(real)
            return real
        for name in rest.strip("\\").split("\\"):
            child = ntpath.join(real, name)
            if child.lower() not in self._real_names:
                dirs, _ = self.listing(real)
                match = next((d for d in dirs if d.lower() == name.lower()), None)
                if match is None:
                    return None
                self._real_names[child.lower()] = ntpath.join(real, match)
            real = self._real_names[child.lower()]
        return real

    def get(self, args):
        if not 1 <= len(args) <= 2:
            print("usage: get REMOTE [LOCAL]")
            return 1
        remote = self.resolve(args[0])
        local = args[1] if len(args) == 2 else ntpath.basename(remote)
        if os.path.isdir(local):
            local = os.path.join(local, ntpath.basename(remote))
        data = self.read_bytes(remote)
        with open(local, "wb") as f:
            f.write(data)
        print(f"{len(data)} bytes -> {local}")
        return 0

    def put(self, args):
        if not 1 <= len(args) <= 2:
            print("usage: put LOCAL [REMOTE]")
            return 1
        local = args[0]
        name = os.path.basename(local)
        if len(args) == 1:
            remote = self.resolve(name)
        elif args[1].endswith(("\\", "/", ":")):
            remote = self.resolve(args[1] + name)
        else:
            remote = self.resolve(args[1])
        with open(local, "rb") as f:
            data = f.read()
        self.write_bytes(remote, data)
        print(f"{len(data)} bytes -> {remote}")
        return 0

    def edit(self, args):
        if len(args) != 1:
            print("usage: edit REMOTE")
            return 1
        remote = self.resolve(args[0])
        try:
            original = self.read_bytes(remote)
        except RemoteError as e:
            if "File not found" not in str(e):
                raise
            original = b""
            print(f"(new file {remote})")

        editor = os.environ.get("VISUAL") or os.environ.get("EDITOR") or "vi"
        tmpdir = tempfile.mkdtemp(prefix="w98sh-")
        try:
            local = os.path.join(tmpdir, ntpath.basename(remote))
            with open(local, "wb") as f:
                f.write(original)
            subprocess.call(shlex.split(editor) + [local])
            with open(local, "rb") as f:
                edited = f.read()
        finally:
            shutil.rmtree(tmpdir, ignore_errors=True)

        if edited == original:
            print("No changes")
            return 0
        # New files come out of a Unix editor with LF; DOS wants CRLF
        if b"\n" not in original and b"\r\n" not in edited:
            edited = edited.replace(b"\n", b"\r\n")
        self.write_bytes(remote, edited)
        print(f"Saved {len(edited)} bytes to {remote}")
        return 0

    def lcd(self, args):
        if args:
            os.chdir(os.path.expanduser(args[0]))
        print(os.getcwd())
        return 0

    def status(self, args):
        print(self.remote.call("status").rstrip())
        return 0

    def cls(self, args):
        print("\033[H\033[2J", end="", flush=True)
        return 0

    def help(self, args):
        print(HELP)
        return 0

    # --- interactive

    def complete(self, text, state):
        if state == 0:
            try:
                self._matches = self._completions(text)
            except Exception:  # never let completion break the prompt
                self._matches = []
        return self._matches[state] if state < len(self._matches) else None

    def _completions(self, text):
        if readline.get_begidx() == 0 and "\\" not in text and ":" not in text:
            return [b + " " for b in BUILTINS if b.startswith(text.lower())]

        head, sep, prefix = text.rpartition("\\")
        if not sep and re.match(r"^[a-zA-Z]:", text):
            head, prefix = text[:2], text[2:]
        directory = self.resolve(head + "\\" if sep else head or ".")
        dirs, files = self.listing(directory)
        lead = head + sep if sep else head
        matches = [lead + d + "\\" for d in dirs if d.lower().startswith(prefix.lower())]
        matches += [lead + f for f in files if f.lower().startswith(prefix.lower())]
        return sorted(matches, key=str.lower)

    def interact(self):
        if readline:
            readline.set_completer(self.complete)
            readline.set_completer_delims(' \t"')
            readline.parse_and_bind("tab: complete")
            try:
                readline.read_history_file(HISTORY_PATH)
            except OSError:
                pass

        try:
            while True:
                try:
                    line = input(f"{self.path}> ")
                except KeyboardInterrupt:
                    print()
                    continue
                except EOFError:
                    print()
                    break
                if line.strip().lower() in ("exit", "quit"):
                    break
                try:
                    self.run_line(line)
                except RemoteError as e:
                    print(f"w98sh: {e}", file=sys.stderr)
                except OSError as e:
                    print(f"w98sh: {e}", file=sys.stderr)
                except KeyboardInterrupt:
                    print("\n^C - stopped waiting; it may still be running on the retro machine",
                          file=sys.stderr)
        finally:
            if readline:
                os.makedirs(os.path.dirname(HISTORY_PATH), exist_ok=True)
                readline.set_history_length(1000)
                readline.write_history_file(HISTORY_PATH)


def load_config():
    url = os.environ.get("W98SH_URL")
    token = os.environ.get("W98SH_TOKEN")
    if not (url and token) and os.path.exists(CONFIG_PATH):
        config = configparser.ConfigParser()
        config.read(CONFIG_PATH)
        section = config["w98sh"] if config.has_section("w98sh") else {}
        url = url or section.get("url")
        token = token or section.get("token")
    if not (url and token):
        sys.exit(f"w98sh: set url and token in {CONFIG_PATH} (or W98SH_URL and W98SH_TOKEN)")
    return url, token


def main():
    parser = argparse.ArgumentParser(
        description="A DOS prompt on a retro Windows machine, through ClaudeWin9x.",
        epilog="With no command, starts an interactive prompt (type help there).")
    parser.add_argument("-d", "--dir", help="retro directory to start in, e.g. C:\\SRC")
    parser.add_argument("-t", "--timeout", type=int, default=DEFAULT_TIMEOUT,
                        help=f"seconds to wait for each command (default {DEFAULT_TIMEOUT}, max 600)")
    parser.add_argument("command", nargs=argparse.REMAINDER, help="command to run, then exit")
    args = parser.parse_args()

    remote = Remote(*load_config())
    shell = Shell(remote, max(5, min(args.timeout, 600)))

    try:
        remote.initialize()
        if args.dir and shell.change_dir(args.dir):
            return 1

        if args.command:
            return shell.run_line(" ".join(args.command))

        status = remote.call("status")
        name = re.search(r"Computer name:\s+(.+)", status)
        version = re.search(r"Windows version:\s+(.+)", status)
        if status.startswith("Connected"):
            print(f"Connected to {name.group(1) if name else 'the retro machine'}"
                  f" ({version.group(1) if version else 'Windows'}). Type help for w98sh commands.")
        else:
            print(status.splitlines()[0])
        shell.interact()
        return 0
    except RemoteError as e:
        print(f"w98sh: {e}", file=sys.stderr)
        return 1
    except KeyboardInterrupt:
        print(file=sys.stderr)
        return 130


if __name__ == "__main__":
    sys.exit(main())
