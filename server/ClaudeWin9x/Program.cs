using System.Collections.Concurrent;
using System.Diagnostics.CodeAnalysis;
using System.Text;
using System.Text.Json;
using ClaudeWin9xServer.Endpoints;
using ClaudeWin9xServer.Infrastructure;
using ClaudeWin9xServer.Models.Requests;
using ClaudeWin9xServer.Models.Responses;
using ClaudeWin9xServer.Remote;
using ClaudeWin9xServer.Services;
using ClaudeWin9xServer.Services.Interfaces;

IniConfig.Load();

// Windows-1252 for text files on the retro machine
Encoding.RegisterProvider(CodePagesEncodingProvider.Instance);

var builder = WebApplication.CreateSlimBuilder(args);

if (IniConfig.RemoteEnabled)
{
    builder.WebHost.UseUrls($"http://0.0.0.0:{IniConfig.ApiPort}", $"http://0.0.0.0:{IniConfig.RemotePort}");
}
else
{
    builder.WebHost.UseUrls($"http://0.0.0.0:{IniConfig.ApiPort}");
}

builder.Services.ConfigureHttpJsonOptions(options =>
{
    options.SerializerOptions.PropertyNamingPolicy = JsonNamingPolicy.SnakeCaseLower;
    options.SerializerOptions.TypeInfoResolverChain.Insert(0, AppJsonSerializerContext.Default);
});

var pendingCommands = new ConcurrentDictionary<string, CommandRequest>();
var commandResults = new ConcurrentDictionary<string, CommandResult>();
var commandWaiters = new ConcurrentDictionary<string, TaskCompletionSource<CommandResult>>();
var pendingFileOps = new ConcurrentDictionary<string, FileOperation>();
var fileOpWaiters = new ConcurrentDictionary<string, TaskCompletionSource<FileOpResult>>();
var pendingApprovals = new ConcurrentDictionary<string, ToolApprovalRequest>();
var approvalWaiters = new ConcurrentDictionary<string, TaskCompletionSource<bool>>();

builder.Services.AddSingleton(pendingCommands);
builder.Services.AddSingleton(commandResults);
builder.Services.AddSingleton(commandWaiters);
builder.Services.AddSingleton(pendingFileOps);
builder.Services.AddSingleton(fileOpWaiters);
builder.Services.AddSingleton(pendingApprovals);
builder.Services.AddSingleton(approvalWaiters);

builder.Services.AddSingleton(sp => new SessionService(
    sp.GetRequiredService<ILogger<SessionService>>()
));
builder.Services.AddSingleton<ISessionService>(sp => sp.GetRequiredService<SessionService>());
builder.Services.AddHostedService(sp => sp.GetRequiredService<SessionService>());

builder.Services.AddSingleton<IApprovalService>(sp => new ApprovalService(
    sp.GetRequiredService<ConcurrentDictionary<string, ToolApprovalRequest>>(),
    sp.GetRequiredService<ConcurrentDictionary<string, TaskCompletionSource<bool>>>(),
    sp.GetRequiredService<ILogger<ApprovalService>>()
));

builder.Services.AddSingleton<ICommandService>(sp => new CommandService(
    sp.GetRequiredService<ConcurrentDictionary<string, CommandRequest>>(),
    sp.GetRequiredService<ConcurrentDictionary<string, CommandResult>>(),
    sp.GetRequiredService<ConcurrentDictionary<string, TaskCompletionSource<CommandResult>>>(),
    sp.GetRequiredService<IApprovalService>(),
    sp.GetRequiredService<ILogger<CommandService>>()
));

builder.Services.AddSingleton<IFileSystemService>(sp => new FileSystemService(
    sp.GetRequiredService<ConcurrentDictionary<string, FileOperation>>(),
    sp.GetRequiredService<ConcurrentDictionary<string, TaskCompletionSource<FileOpResult>>>(),
    sp.GetRequiredService<IApprovalService>(),
    sp.GetRequiredService<ILogger<FileSystemService>>()
));

builder.Services.AddSingleton(sp => new FileTransferService(
    IniConfig.DownloadPort,
    IniConfig.UploadPort,
    sp.GetRequiredService<ILogger<FileTransferService>>(),
    Path.GetTempPath()
));
builder.Services.AddHostedService(sp => sp.GetRequiredService<FileTransferService>());

builder.Services.AddSingleton(sp => new RemoteClientService(sp.GetRequiredService<ILogger<RemoteClientService>>()));
builder.Services.AddSingleton(sp => new RemoteTools(
    sp.GetRequiredService<RemoteClientService>(),
    sp.GetRequiredService<ICommandService>(),
    sp.GetRequiredService<IFileSystemService>(),
    sp.GetRequiredService<IApprovalService>(),
    sp.GetRequiredService<FileTransferService>(),
    sp.GetRequiredService<ILogger<RemoteTools>>()
));
builder.Services.AddSingleton(sp => new McpEndpoint(
    sp.GetRequiredService<RemoteTools>(),
    sp.GetRequiredService<ILogger<McpEndpoint>>()
));

var app = builder.Build();

// Disable chunked transfer encoding for compatibility
app.Use(async (context, next) =>
{
    context.Response.Headers["Transfer-Encoding"] = "";
    await next();
});

// The remote port only serves /mcp, with its own token; the retro clients' fixed key does not work there
app.Use(async (context, next) =>
{
    var onRemotePort = IniConfig.RemoteEnabled && context.Connection.LocalPort == IniConfig.RemotePort;
    var isMcp = context.Request.Path.StartsWithSegments("/mcp");

    if (onRemotePort != isMcp)
    {
        context.Response.StatusCode = 404;
        return;
    }

    if (onRemotePort)
    {
        if (!McpEndpoint.IsAuthorized(context))
        {
            context.Response.StatusCode = 401;
            context.Response.Headers.WWWAuthenticate = "Bearer";
            return;
        }
        await next();
        return;
    }

    var providedKey = context.Request.Headers["X-API-Key"].FirstOrDefault();
    if (providedKey != IniConfig.ApiKey)
    {
        context.Response.StatusCode = 401;
        await context.Response.WriteAsync("Unauthorized");
        return;
    }
    await next();
});

[UnconditionalSuppressMessage("Trimming", "IL2026:Members annotated with 'RequiresUnreferencedCodeAttribute' require dynamic access otherwise can break functionality when trimming application code", Justification = "Minimal APIs are expected to use reflection")]
[UnconditionalSuppressMessage("AOT", "IL3050:Calling members annotated with 'RequiresDynamicCodeAttribute' may break functionality when AOT compiling", Justification = "Minimal APIs require runtime code generation")]
static void ConfigureEndpoints(WebApplication app) => app.MapEndpoints();

ConfigureEndpoints(app);

if (IniConfig.RemoteEnabled)
{
    var mcp = app.Services.GetRequiredService<McpEndpoint>();
    app.Map("/mcp", (RequestDelegate)mcp.HandleAsync);
}

Console.WriteLine("===========================================");
Console.WriteLine("  ClaudeWin9x Server");
Console.WriteLine("  Written by Ryan Deering (github.com/ryandeering)");
Console.WriteLine($"  Listening on http://0.0.0.0:{IniConfig.ApiPort}");
Console.WriteLine("===========================================");
Console.WriteLine();

Console.WriteLine("Config:");
Console.WriteLine($"  api_port:         {IniConfig.ApiPort}");
Console.WriteLine($"  download_port:    {IniConfig.DownloadPort}");
Console.WriteLine($"  upload_port:      {IniConfig.UploadPort}");
Console.WriteLine($"  temp_dir:         {Path.GetTempPath()}");
if (IniConfig.RemoteEnabled)
{
    Console.WriteLine($"  remote_port:      {IniConfig.RemotePort} (MCP at http://<host>:{IniConfig.RemotePort}/mcp)");
}
else if (IniConfig.RemotePort != 0)
{
    Console.WriteLine($"  remote_port:      {IniConfig.RemotePort} DISABLED: remote_token must be at least {IniConfig.MinRemoteTokenLength} characters");
}
Console.WriteLine();

Console.WriteLine("Note: Claude CLI runs with --dangerously-skip-permissions.");
Console.WriteLine("      Commands to client are still gated for approval.");
Console.WriteLine();

Console.WriteLine("Endpoints:");
Console.WriteLine("  Claude Code: /start, /input, /output, /stop, /sessions, /heartbeat");
Console.WriteLine("  Commands:    /cmd/queue, /cmd/poll, /cmd/result, /cmd/status");
Console.WriteLine("  Filesystem:  /fs/list, /fs/read, /fs/write, /fs/poll, /fs/result");
Console.WriteLine("  Approvals:   /approval/poll, /approval/respond");
Console.WriteLine("  Remote:      /remote/connect, /remote/heartbeat, /remote/disconnect");
Console.WriteLine();

app.Run();
