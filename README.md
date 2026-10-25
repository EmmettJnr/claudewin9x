# ClaudeWin9x

Claude Code CLI access on Windows 95/98/2000/XP+ via a bridge server.

[![Build](https://github.com/ryandeering/claude-win9x/actions/workflows/build.yml/badge.svg)](https://github.com/ryandeering/claude-win9x/actions/workflows/build.yml)
[![GitHub Release](https://img.shields.io/github/v/release/ryandeering/claude-win9x)](https://github.com/ryandeering/claude-win9x/releases/latest)
[![License: MIT](https://img.shields.io/badge/License-MIT-yellow.svg)](LICENSE)
[![ko-fi](https://ko-fi.com/img/githubbutton_sm.svg)](https://ko-fi.com/ryandeering)

**[Download latest release](https://github.com/ryandeering/claude-win9x/releases/latest)** | **[Read the blog post](https://ryandeering.ie/posts/claudewin9x/)**

<br>

> "Your scientists were so preoccupied with whether they could, they didn’t stop to think if they should.”
> - Dr. Ian Malcolm, *Jurassic Park* (1993)

<br>

![Claude Code running on Windows 95](misc/win95.jpg)

<video width="800" height="290" src="https://github.com/user-attachments/assets/776ec3cf-317b-4458-a58e-050e199ef1df"></video>

```
┌─────────────────────┐  HTTP   ┌─────────────────────┐  stdio  ┌─────────────────────┐
│   Windows 9x/NT     │  :5000  │       Server        │         │   Claude Code CLI   │
│      Client         │◄───────►│     (.NET 10)       │◄───────►│                     │
│       (C99)         │   TCP   │                     │         │                     │
│                     │◄───────►│  :5001/:5002 files  │         │                     │
└─────────────────────┘         └─────────────────────┘         └─────────────────────┘
```

- Server spawns Claude Code CLI and communicates via JSON over stdio
- Claude Code requests file/command operations via HTTP to the server
- Server queues these operations for the client to poll and execute locally
- All connections initiated by client

## Prerequisites

**Server:** [Claude Code CLI](https://docs.anthropic.com/en/docs/claude-code)

**Client:** Win95 needs Winsock 2 + MSVCRT (get Internet Explorer 5, Winsock 2 update), beyond that shouldn't need any dependency to run the executable but will need TCP/IP networking set up.

## Quick Start

1. **Server:** Run `ClaudeWin9x-Server.exe` (with optional `server.ini`)
2. **Client:** Copy `ClaudeWin9x-Client.exe` and `client.ini` to Win9x/NT, then:
   ```
   C:\> ClaudeWin9x-Client.exe
   > /connect C:\MYPROJECT
   ```

Open ports 5000-5002 between machines.

## Notes

**Security:** All traffic is unencrypted as there is no HTTPS on Win9x (obviously!). Only use on trusted networks.

Command output uses temp files because stdout pipe redirection doesn't work reliably on Windows 9x. As a result, **ClaudeWin9x is less effective than it is on NT-based systems**. It is what it is.

## Configuration

### Client (`client.ini`)
```ini
[server]
ip=192.168.1.100
api_port=5000
download_port=5001
upload_port=5002
skip_permissions=false
```

### Server (`server.ini`)
```ini
[server]
api_port=5000
download_port=5001
upload_port=5002
```

Put these next to their respective executables.

Environment variables (if Claude Code is in non-standard location):
- `CLAUDE_CLI_PATH` - Claude CLI path
- `CLAUDE_NODE_DIR` - Node directory

## Commands

| Command | Description |
|--------|-------------|
| `/connect [path]` | Start session |
| `/disconnect` | End session |
| `/server ip:port` | Set server address |
| `/download <remote> <local>` | Download file |
| `/upload <local> <remote>` | Upload file |
| `/poll` | Check for output |
| `/status` | Connection status |
| `/log [on\|off\|view]` | Logging |
| `/clear` | Clear screen |
| `/help` | Help |
| `/quit` | Exit |

## Build

### Client

```bash
cd client
wmake
```

Builds a standalone executable targeting Win32, 386 and above. Minimum supported OS is Windows 95 OSR 2. Build using [Open Watcom V2](https://github.com/open-watcom/open-watcom-v2/releases/tag/Current-build).

#### Visual C++ 6.0

The client also builds with Microsoft Visual C++ 6.0, either from the IDE or the command line:

- **IDE:** open `client\ClaudeWin9xClient.dsw` and build. Output goes to `Release\` or `Debug\`.
- **Command line:** run `VCVARS32.BAT` (in `VC98\Bin`), then:
  ```
  cd client
  nmake /f Makefile.vc6
  ```

Both link the static multithreaded CRT (`/MT`), so the exe only depends on `KERNEL32.DLL` and `WS2_32.DLL`. The `.dsp`/`.dsw`/`Makefile.vc6` files must keep CRLF line endings for VC6 to load them (see `.gitattributes`).

#### GUI client (MFC)

`client\gui` contains `ClaudeWin9x-Gui.exe`, an MFC front end built with Visual C++ 6.0. It shares the networking, file operation and command code with the console client, and replaces the console with a resizable window:

- Chat transcript (rich edit, colour-coded: your messages, Claude, client activity, errors)
- Multi-line input with normal cursor editing: Enter sends, Shift+Enter adds a line, Ctrl+Up/Down recalls earlier messages
- Allow/Deny dialog for tool requests (Deny is the default button), and an auto-approve checkbox
- Commands run in a hidden window rather than flashing a DOS box
- UTF-8 from the server is converted to the Windows code page (Windows 98 and later)

It reads the same `client.ini`, from the folder the exe is in. Build it from the `ClaudeWin9xGui` project in `client\ClaudeWin9xClient.dsw`, or from the command line after `VCVARS32.BAT`:

```
cd client\gui
nmake /f Makefile.vc6
```

MFC and the CRT are linked statically, so it runs without `MFC42.DLL` or `MSVCRT.DLL`.

#### Remote control client

`client\remote` contains `ClaudeWin9x-Remote.exe`, which flips things around: instead of chatting with Claude on the retro machine, you run a normal Claude Code session on a modern computer and it drives the retro machine through the server.

```
┌──────────────────┐  MCP over HTTP  ┌──────────┐  HTTP/TCP (polled)  ┌────────────────────┐
│ Claude Code      │ ──────────────► │  Server  │ ◄────────────────── │ ClaudeWin9x-Remote │
│ (modern machine) │  :5003, token   │          │  :5000-5002         │ (Win9x/NT)         │
└──────────────────┘                 └──────────┘                     └────────────────────┘
```

The client is a small window: server address, **Connect**, an **Allow anything** checkbox, a status line and an activity log. It lives in the notification area as two dial-up style monitors, which flicker while Claude is running commands or moving files; closing or minimising the window hides it there, and the icon's right-click menu has Connect, Allow anything and Exit. With Allow anything off, every command and file write pops up an Allow/Deny dialog on the retro machine (Deny is the default). It reconnects by itself if the server restarts. In a `[remote]` section of `client.ini`, `auto_connect=true` connects at startup, `allow_all=true` starts with Allow anything on (for this client only, unlike `skip_permissions`) and `start_hidden=true` starts in the tray. The tray icons are generated by `build/icons/make_tray_icons.py`.

It is plain Win32 C (no MFC), reads the same `client.ini`, and builds with Visual C++ 6.0 or Open Watcom (including Open Watcom V2 on a Linux host):

```
cd client\remote
nmake /f Makefile.vc6       (VC6, after VCVARS32.BAT)
wmake                       (Open Watcom)
```

On the server, enable the remote API in `server.ini` with its own port and a long random token:

```ini
remote_port = 5003
remote_token = <e.g. output of: openssl rand -hex 24>
```

Only `/mcp` is served on that port, and only with `Authorization: Bearer <token>`; the retro clients' fixed key does not work there. Allow only the modern machine to reach it (`ufw allow from <modern-ip> to any port 5003 proto tcp`). Optional notes for Claude about your setup (installed tools, shares) go in `remote_instructions.txt` next to the server executable.

Then on the modern machine:

```bash
claude mcp add --transport http --scope user win98 http://<server>:5003/mcp \
  --header "Authorization: Bearer <token>"
```

Tools: `status`, `run_command`, `list_directory`, `read_file`, `write_file` (text is converted to CRLF and Windows-1252; `encoding: base64` for binary) and `make_directory`. File contents go over the TCP transfer ports, so they are byte-exact and not limited by the client's 32 KB HTTP buffer. Paths can use any drive letter; without one they are relative to `C:\`.

#### w98sh: a DOS prompt from a modern terminal

`tools/w98sh.py` uses the same remote endpoint to give you a DOS prompt on the retro machine, a bit like ssh. It needs only Python 3:

```
$ w98sh
Connected to RETROPC (Windows 98). Type help for w98sh commands.
C:\> cd projects\game
C:\Projects\GAME> nmake
C:\Projects\GAME> get GAME.EXE
C:\Projects\GAME> edit MAIN.C        (opens in $EDITOR here, saves it back)
```

Directories and drives are tracked like DOS (`cd`, `cd..`, `D:`), Tab completes retro file names, and history (up arrow, Ctrl-R) is kept in `~/.local/state/w98sh_history`. `get`, `put` and `edit` move files; `lcd` and `!command` work on the local machine. `w98sh dir C:\` runs a single command and exits (`-d DIR` sets the directory).

Configure it in `~/.config/w98sh.ini` (or `W98SH_URL` / `W98SH_TOKEN`):

```ini
[w98sh]
url = http://<server>:5003/mcp
token = <remote_token>
```

It is not a real console: programs get no keyboard input (no EDIT, games or prompts), output appears when a command finishes, and Ctrl-C only stops waiting.

### Server

```bash
cd server/ClaudeWin9x
dotnet run
dotnet publish -r win-x64       # Or linux-x64, osx-arm64, etc
```

Published as a native AOT binary - there's no runtime required (woo!). Requires .NET 10 SDK and Visual Studio Build Tools (Desktop C++ workload) to build on Windows.

#### Running on a locked-down Linux VM

The server runs Claude Code with `--dangerously-skip-permissions` on the server host, the API key is a fixed value in the source, and the file transfer ports are unauthenticated. Anything that can reach ports 5000-5002 can effectively drive Claude on the server, so running it in a dedicated VM with a firewall is recommended.

1. Install the .NET 10 SDK and [Claude Code](https://docs.anthropic.com/en/docs/claude-code), then run `claude` once as the service user to log in.
2. Build. On low-memory VMs a framework-dependent build avoids the AOT compiler:
   ```bash
   cd server/ClaudeWin9x
   dotnet publish -c Release -r linux-x64 --self-contained false -p:PublishAot=false -o /opt/claudewin9x
   ```
3. Install `server/deploy/claudewin9x.service` as a systemd unit (edit `User` and paths first).
4. Only allow the retro client to reach the server ports, e.g. with ufw:
   ```bash
   sudo ufw default deny incoming
   sudo ufw allow OpenSSH
   sudo ufw allow from <client-ip> to any port 5000:5002 proto tcp
   sudo ufw enable
   ```

The path given to `/connect` is sent to the server as Claude's working directory. If it doesn't exist on the server (e.g. a Windows path when the server runs on Linux), the server falls back to its temp directory.

## Third Party

- [cJSON](https://github.com/DaveGamble/cJSON) - MIT
- Special thanks to Romane for making the icons in theme with the old Windows style.
