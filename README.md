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
