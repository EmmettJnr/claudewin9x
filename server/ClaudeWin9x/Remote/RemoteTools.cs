using System.Text;
using System.Text.Json.Nodes;
using ClaudeWin9xServer.Infrastructure;
using ClaudeWin9xServer.Services;
using ClaudeWin9xServer.Services.Interfaces;

namespace ClaudeWin9xServer.Remote;

public record ToolResult(string Text, bool IsError)
{
    public static ToolResult Ok(string text) => new(text, false);
    public static ToolResult Fail(string text) => new(text, true);
}

public class ToolArgumentException(string message) : Exception(message);

/// <summary>
/// The tools exposed over MCP. Each one is queued for the remote client on the retro machine,
/// which polls for work exactly as the chat clients do. File contents travel over the raw TCP
/// transfer ports, so they are byte-exact and not limited by the client's 32 KB HTTP buffer.
/// </summary>
public class RemoteTools(
    RemoteClientService remoteClients,
    ICommandService commandService,
    IFileSystemService fileSystemService,
    IApprovalService approvalService,
    FileTransferService fileTransferService,
    ILogger<RemoteTools> logger)
{
    public const string InstructionsFile = "remote_instructions.txt";

    private const int DefaultCommandTimeoutSeconds = 120;
    // The client kills a hidden command after 10 minutes
    private const int MaxCommandTimeoutSeconds = 600;
    private const int DefaultTextLimit = 60_000;
    private const int MaxTextLimit = 200_000;
    private const int DefaultBinaryLimit = 32_768;
    private const int MaxBinaryLimit = 262_144;
    private const int ApprovalPreviewChars = 1500;

    private static readonly TimeSpan ApprovalTimeout = TimeSpan.FromSeconds(120);
    private static readonly TimeSpan QuickOpTimeout = TimeSpan.FromSeconds(60);
    private static readonly TimeSpan TransferTimeout = TimeSpan.FromSeconds(300);

    // Windows 9x is not Unicode; text files are in the ANSI code page
    private static readonly Encoding AnsiEncoding = Encoding.GetEncoding(
        1252, new EncoderReplacementFallback("?"), DecoderFallback.ReplacementFallback);

    public static string Instructions()
    {
        var text = """
            These tools control a retro Windows PC (Windows 95/98/NT era) through the ClaudeWin9x bridge.
            The ClaudeWin9x-Remote client must be running and connected on that machine; check with `status`.

            - Commands run via COMMAND.COM /C (CMD.EXE on NT) with output redirected to a file. There is no
              stdin: interactive programs hang until the timeout, so pass answers on the command line or
              script them. No `&&`, no `2>&1` on 9x, and exit codes from COMMAND.COM are unreliable.
            - COMMAND.COM truncates long command lines (~127 characters). For anything long or multi-step,
              write a .BAT file with write_file and run that.
            - Paths without a drive letter are relative to C:\. Use 8.3 names if long names misbehave.
            - Text written with write_file is converted to CRLF and Windows-1252 by default; use
              encoding=base64 for binary data.
            - Each tool call is a round trip to an old machine that polls about once a second: batch work
              into fewer, larger calls where possible.
            - The user may have "Allow anything" switched off, in which case they approve each command and
              write on the retro machine; a denial comes back as an error. Do not retry denied actions.
            - Be careful with system files (WINDOWS, SYSTEM, IO.SYS, MSDOS.SYS, CONFIG.SYS, AUTOEXEC.BAT,
              the registry): warn before changing them and keep a backup copy.
            """;

        var extraPath = Path.Combine(AppContext.BaseDirectory, InstructionsFile);
        if (File.Exists(extraPath))
        {
            text += "\n" + File.ReadAllText(extraPath).Trim() + "\n";
        }

        return text;
    }

    public static JsonArray Definitions() => (JsonArray)JsonNode.Parse("""
        [
          {
            "name": "status",
            "title": "Retro PC status",
            "description": "Shows whether the retro Windows machine is connected, its Windows version, computer name, IP address and whether 'Allow anything' is on.",
            "inputSchema": { "type": "object", "properties": {} },
            "annotations": { "readOnlyHint": true }
          },
          {
            "name": "run_command",
            "title": "Run command on retro PC",
            "description": "Runs a command on the retro Windows machine via COMMAND.COM /C (CMD.EXE on NT) and returns its output. Non-interactive only. Keep the line short (COMMAND.COM truncates around 127 characters); write a .BAT file for longer scripts. Exit codes are unreliable on Windows 9x.",
            "inputSchema": {
              "type": "object",
              "properties": {
                "command": { "type": "string", "description": "Command line, e.g. \"dir C:\\WINDOWS\" or \"C:\\BUILD\\MAKE.BAT\"." },
                "working_directory": { "type": "string", "description": "Directory to run in, e.g. \"C:\\PROJECTS\\GAME\". Defaults to the client's own directory." },
                "timeout_seconds": { "type": "integer", "minimum": 5, "maximum": 600, "description": "How long to wait for the command (default 120). The client kills commands after 600 seconds." }
              },
              "required": ["command"]
            }
          },
          {
            "name": "list_directory",
            "title": "List directory on retro PC",
            "description": "Lists a directory on the retro Windows machine (long file names, sizes in bytes).",
            "inputSchema": {
              "type": "object",
              "properties": {
                "path": { "type": "string", "description": "Directory, e.g. \"C:\\WINDOWS\" or \"D:\\\". Without a drive letter it is relative to C:\\. Defaults to C:\\." }
              }
            },
            "annotations": { "readOnlyHint": true }
          },
          {
            "name": "read_file",
            "title": "Read file from retro PC",
            "description": "Reads a file from the retro Windows machine. Text is decoded from Windows-1252. Use encoding=base64 for binary files; large results are paged with offset/limit.",
            "inputSchema": {
              "type": "object",
              "properties": {
                "path": { "type": "string", "description": "File path, e.g. \"C:\\AUTOEXEC.BAT\"." },
                "encoding": { "type": "string", "enum": ["text", "base64"], "description": "text (default) or base64 for binary data." },
                "offset": { "type": "integer", "minimum": 0, "description": "Characters (text) or bytes (base64) to skip." },
                "limit": { "type": "integer", "minimum": 1, "description": "Maximum characters (text, default 60000) or bytes (base64, default 32768) to return." }
              },
              "required": ["path"]
            },
            "annotations": { "readOnlyHint": true }
          },
          {
            "name": "write_file",
            "title": "Write file to retro PC",
            "description": "Creates or overwrites a file on the retro Windows machine. The directory must already exist (see make_directory). Text is converted to CRLF line endings and Windows-1252 unless told otherwise.",
            "inputSchema": {
              "type": "object",
              "properties": {
                "path": { "type": "string", "description": "File path, e.g. \"C:\\BUILD\\MAKE.BAT\"." },
                "content": { "type": "string", "description": "File contents: text, or base64 when encoding is base64." },
                "encoding": { "type": "string", "enum": ["text", "base64"], "description": "text (default) or base64 for binary data." },
                "line_endings": { "type": "string", "enum": ["crlf", "keep"], "description": "For text: crlf (default) converts line breaks to CRLF, keep writes them unchanged." }
              },
              "required": ["path", "content"]
            },
            "annotations": { "destructiveHint": true }
          },
          {
            "name": "make_directory",
            "title": "Create directory on retro PC",
            "description": "Creates a directory on the retro Windows machine. The parent must exist. Succeeds if it already exists.",
            "inputSchema": {
              "type": "object",
              "properties": {
                "path": { "type": "string", "description": "Directory to create, e.g. \"C:\\PROJECTS\\NEW\"." }
              },
              "required": ["path"]
            }
          }
        ]
        """)!;

    public Task<ToolResult> CallAsync(string name, JsonObject args, CancellationToken cancellationToken) => name switch
    {
        "status" => Task.FromResult(Status()),
        "run_command" => RunCommandAsync(args, cancellationToken),
        "list_directory" => ListDirectoryAsync(args, cancellationToken),
        "read_file" => ReadFileAsync(args, cancellationToken),
        "write_file" => WriteFileAsync(args, cancellationToken),
        "make_directory" => MakeDirectoryAsync(args, cancellationToken),
        _ => Task.FromResult(ToolResult.Fail($"Unknown tool: {name}"))
    };

    private ToolResult Status()
    {
        var current = remoteClients.Current;
        var known = current ?? remoteClients.LastKnown;

        if (known == null)
        {
            return ToolResult.Ok("Not connected. Start ClaudeWin9x-Remote on the retro machine and click Connect.");
        }

        var sb = new StringBuilder();
        sb.AppendLine(current != null
            ? "Connected"
            : $"Not responding (last seen {(DateTime.UtcNow - known.LastSeen).TotalSeconds:0}s ago)");
        sb.AppendLine($"Windows version: {known.WindowsVersion ?? "unknown"}");
        sb.AppendLine($"Computer name:   {known.ComputerName ?? "unknown"}");
        sb.AppendLine($"Address:         {known.Address ?? "unknown"}");
        sb.AppendLine($"Allow anything:  {(known.AllowAll ? "on (no prompts)" : "off (user approves commands and writes)")}");
        sb.AppendLine($"Connected since: {known.ConnectedAt:u}");
        return ToolResult.Ok(sb.ToString());
    }

    private async Task<ToolResult> RunCommandAsync(JsonObject args, CancellationToken cancellationToken)
    {
        var command = RequiredString(args, "command");
        var workingDirectory = OptionalString(args, "working_directory");
        var timeoutSeconds = Math.Clamp(OptionalInt(args, "timeout_seconds") ?? DefaultCommandTimeoutSeconds,
            5, MaxCommandTimeoutSeconds);

        if (RequireClient(out var client) is { } notConnected)
        {
            return notConnected;
        }
        using var op = remoteClients.BeginOperation();

        var description = workingDirectory == null ? command : $"{command}\r\n\r\n(in {workingDirectory})";
        if (!await ApproveAsync(client, "Run command", description, cancellationToken))
        {
            return Denied();
        }

        var result = await commandService.QueueCommandAsync(command, workingDirectory, null,
            TimeSpan.FromSeconds(timeoutSeconds), cancellationToken);

        if (result == null)
        {
            return ToolResult.Fail($"No result after {timeoutSeconds}s. The command may still be running on the " +
                "retro machine (the client kills it after 600s); further calls queue behind it.");
        }

        var output = string.IsNullOrEmpty(result.Stdout) ? "(no output)" : result.Stdout.Replace("\r\n", "\n");
        return ToolResult.Ok($"Exit code: {result.ExitCode}\n{output}");
    }

    private async Task<ToolResult> ListDirectoryAsync(JsonObject args, CancellationToken cancellationToken)
    {
        var path = OptionalString(args, "path") ?? "";

        if (RequireClient(out _) is { } notConnected)
        {
            return notConnected;
        }
        using var op = remoteClients.BeginOperation();

        var result = await fileSystemService.QueueOperationAsync("list", path, null, QuickOpTimeout, cancellationToken);
        if (result == null)
        {
            return TimedOut();
        }
        if (result.Error != null)
        {
            return ToolResult.Fail($"{result.Error}: {path}");
        }

        var entries = result.Entries ?? [];
        var sb = new StringBuilder();
        foreach (var entry in entries.OrderBy(e => e.Type != "dir").ThenBy(e => e.Name, StringComparer.OrdinalIgnoreCase))
        {
            sb.AppendLine(entry.Type == "dir" ? $"{"<DIR>",12}  {entry.Name}" : $"{entry.Size,12}  {entry.Name}");
        }
        sb.AppendLine($"{entries.Count(e => e.Type != "dir")} file(s), {entries.Count(e => e.Type == "dir")} dir(s)");
        return ToolResult.Ok(sb.ToString());
    }

    private async Task<ToolResult> ReadFileAsync(JsonObject args, CancellationToken cancellationToken)
    {
        var path = RequiredString(args, "path");
        var binary = ParseEncoding(args);
        var offset = Math.Max(0, OptionalInt(args, "offset") ?? 0);
        var limit = Math.Clamp(OptionalInt(args, "limit") ?? (binary ? DefaultBinaryLimit : DefaultTextLimit),
            1, binary ? MaxBinaryLimit : MaxTextLimit);

        if (RequireClient(out _) is { } notConnected)
        {
            return notConnected;
        }
        using var op = remoteClients.BeginOperation();

        // The client uploads the file to the transfer port under this name
        var transferName = $"cw9x-up-{IdGenerator.NewId()}.dat";
        var transferPath = Path.Combine(fileTransferService.Root, transferName);
        byte[] bytes;
        try
        {
            var result = await fileSystemService.QueueOperationAsync("send", path, transferName, TransferTimeout,
                cancellationToken);
            if (result == null)
            {
                return TimedOut();
            }
            if (result.Error != null)
            {
                return ToolResult.Fail($"{result.Error}: {path}");
            }
            if (!File.Exists(transferPath))
            {
                return ToolResult.Fail("The client reported success but the file did not arrive on the server.");
            }
            bytes = await File.ReadAllBytesAsync(transferPath, cancellationToken);
        }
        finally
        {
            TryDelete(transferPath);
        }

        if (binary)
        {
            var start = Math.Min(offset, bytes.Length);
            var count = Math.Min(limit, bytes.Length - start);
            var data = Convert.ToBase64String(bytes, start, count);
            return ToolResult.Ok(start + count < bytes.Length || start > 0
                ? $"[bytes {start}-{start + count} of {bytes.Length}; use offset to read more]\n{data}"
                : data);
        }

        var text = AnsiEncoding.GetString(bytes);
        var note = Array.IndexOf(bytes, (byte)0) >= 0 ? "[file contains NUL bytes; it may be binary, see encoding=base64]\n" : "";
        if (offset > 0 || text.Length > limit)
        {
            var start = Math.Min(offset, text.Length);
            var count = Math.Min(limit, text.Length - start);
            return ToolResult.Ok($"{note}[characters {start}-{start + count} of {text.Length}; use offset to read more]\n" +
                text.Substring(start, count));
        }
        return ToolResult.Ok(note + text);
    }

    private async Task<ToolResult> WriteFileAsync(JsonObject args, CancellationToken cancellationToken)
    {
        var path = RequiredString(args, "path");
        var content = RequiredString(args, "content", allowEmpty: true);
        var binary = ParseEncoding(args);
        var lineEndings = OptionalString(args, "line_endings") ?? "crlf";
        if (lineEndings is not ("crlf" or "keep"))
        {
            throw new ToolArgumentException("line_endings must be crlf or keep");
        }

        byte[] bytes;
        string preview;
        if (binary)
        {
            try
            {
                bytes = Convert.FromBase64String(content);
            }
            catch (FormatException)
            {
                return ToolResult.Fail("content is not valid base64");
            }
            preview = "(binary data)";
        }
        else
        {
            var text = lineEndings == "crlf" ? content.Replace("\r\n", "\n").Replace("\n", "\r\n") : content;
            bytes = AnsiEncoding.GetBytes(text);
            preview = text.Length > ApprovalPreviewChars ? text[..ApprovalPreviewChars] + "\r\n..." : text;
        }

        if (RequireClient(out var client) is { } notConnected)
        {
            return notConnected;
        }
        using var op = remoteClients.BeginOperation();

        if (!await ApproveAsync(client, "Write file", $"Write {bytes.Length} bytes to {path}\r\n\r\n{preview}",
                cancellationToken))
        {
            return Denied();
        }

        // The download port cannot send empty files, and tiny files are quicker inline
        if (bytes.Length == 0)
        {
            var inline = await fileSystemService.QueueOperationAsync("write", path, "", QuickOpTimeout,
                cancellationToken);
            return inline == null ? TimedOut()
                : inline.Error != null ? ToolResult.Fail($"{inline.Error}: {path}")
                : ToolResult.Ok($"Wrote 0 bytes to {path}");
        }

        // Staged in the transfer directory; the client fetches it over the download port
        var transferName = $"cw9x-dn-{IdGenerator.NewId()}.dat";
        var transferPath = Path.Combine(fileTransferService.Root, transferName);
        try
        {
            await File.WriteAllBytesAsync(transferPath, bytes, cancellationToken);
            var result = await fileSystemService.QueueOperationAsync("fetch", path, transferName, TransferTimeout,
                cancellationToken);
            if (result == null)
            {
                return TimedOut();
            }
            if (result.Error != null)
            {
                return ToolResult.Fail($"{result.Error}: {path}");
            }
        }
        finally
        {
            TryDelete(transferPath);
        }

        return ToolResult.Ok($"Wrote {bytes.Length} bytes to {path}");
    }

    private async Task<ToolResult> MakeDirectoryAsync(JsonObject args, CancellationToken cancellationToken)
    {
        var path = RequiredString(args, "path");

        if (RequireClient(out _) is { } notConnected)
        {
            return notConnected;
        }
        using var op = remoteClients.BeginOperation();

        var result = await fileSystemService.QueueOperationAsync("mkdir", path, null, QuickOpTimeout, cancellationToken);
        return result == null ? TimedOut()
            : result.Error != null ? ToolResult.Fail($"{result.Error}: {path}")
            : ToolResult.Ok($"Created {path}");
    }

    private ToolResult? RequireClient(out RemoteClient client)
    {
        var current = remoteClients.Current;
        if (current == null)
        {
            client = null!;
            return ToolResult.Fail(remoteClients.LastKnown == null
                ? "The retro machine is not connected. Ask the user to start ClaudeWin9x-Remote there and click Connect."
                : "The retro machine has stopped responding. Ask the user to check it is still running ClaudeWin9x-Remote.");
        }
        client = current;
        return null;
    }

    private async Task<bool> ApproveAsync(RemoteClient client, string tool, string description,
        CancellationToken cancellationToken)
    {
        // Re-read so a change to the checkbox since the call started is honoured
        if (remoteClients.Current?.AllowAll == true)
        {
            return true;
        }

        var approved = await approvalService.RequestApprovalAsync(client.SessionId, tool, description,
            ApprovalTimeout, cancellationToken);
        if (!approved)
        {
            logger.LogInformation("{Tool} denied on the retro machine", tool);
        }
        return approved;
    }

    private static ToolResult Denied() =>
        ToolResult.Fail("The user denied this on the retro machine (or did not answer within 120 seconds).");

    private static ToolResult TimedOut() =>
        ToolResult.Fail("The retro machine did not complete the request in time. It may be busy with an earlier command.");

    private void TryDelete(string path)
    {
        try
        {
            File.Delete(path);
        }
        catch (IOException ex)
        {
            logger.LogWarning("Could not delete transfer file {Path}: {Message}", path, ex.Message);
        }
        catch (UnauthorizedAccessException ex)
        {
            logger.LogWarning("Could not delete transfer file {Path}: {Message}", path, ex.Message);
        }
    }

    private static bool ParseEncoding(JsonObject args) => (OptionalString(args, "encoding") ?? "text") switch
    {
        "text" => false,
        "base64" => true,
        _ => throw new ToolArgumentException("encoding must be text or base64")
    };

    private static string RequiredString(JsonObject args, string name, bool allowEmpty = false)
    {
        var value = OptionalString(args, name);
        if (value == null || (!allowEmpty && value.Length == 0))
        {
            throw new ToolArgumentException($"{name} is required");
        }
        return value;
    }

    private static string? OptionalString(JsonObject args, string name) =>
        args[name] is JsonValue v && v.TryGetValue<string>(out var s) ? s : null;

    private static int? OptionalInt(JsonObject args, string name) => args[name] switch
    {
        JsonValue v when v.TryGetValue<int>(out var i) => i,
        JsonValue v when v.TryGetValue<double>(out var d) => (int)Math.Clamp(d, int.MinValue, int.MaxValue),
        JsonValue v when v.TryGetValue<string>(out var s) && int.TryParse(s, out var p) => p,
        _ => null
    };
}
