using System.Security.Cryptography;
using System.Text;
using System.Text.Json;
using System.Text.Json.Nodes;
using ClaudeWin9xServer.Infrastructure;

namespace ClaudeWin9xServer.Remote;

/// <summary>
/// Minimal Model Context Protocol server (Streamable HTTP transport, JSON responses only, no
/// sessions) so a Claude Code session on another machine can drive the retro client through
/// <see cref="RemoteTools"/>. Hand-rolled with JsonNode to stay trimming/AOT friendly.
/// </summary>
public class McpEndpoint(RemoteTools tools, ILogger<McpEndpoint> logger)
{
    private const string FallbackProtocolVersion = "2025-06-18";

    public static bool IsAuthorized(HttpContext context)
    {
        var header = context.Request.Headers.Authorization.FirstOrDefault();
        if (header == null || !header.StartsWith("Bearer ", StringComparison.OrdinalIgnoreCase) ||
            IniConfig.RemoteToken == null)
        {
            return false;
        }

        var provided = Encoding.UTF8.GetBytes(header["Bearer ".Length..].Trim());
        var expected = Encoding.UTF8.GetBytes(IniConfig.RemoteToken);
        return CryptographicOperations.FixedTimeEquals(provided, expected);
    }

    public async Task HandleAsync(HttpContext context)
    {
        if (!HttpMethods.IsPost(context.Request.Method))
        {
            // No server-initiated stream (GET) or session to end (DELETE)
            context.Response.StatusCode = StatusCodes.Status405MethodNotAllowed;
            context.Response.Headers.Allow = "POST";
            return;
        }

        JsonNode? request;
        try
        {
            request = await JsonNode.ParseAsync(context.Request.Body, cancellationToken: context.RequestAborted);
        }
        catch (JsonException)
        {
            await WriteAsync(context, Error(null, -32700, "Parse error"));
            return;
        }

        if (request is not JsonObject message)
        {
            await WriteAsync(context, Error(null, -32600, "Expected a single JSON-RPC message"));
            return;
        }

        var id = message["id"]?.DeepClone();
        var method = message["method"] is JsonValue m && m.TryGetValue<string>(out var name) ? name : null;

        // Notifications and responses get no reply
        if (id == null || method == null)
        {
            context.Response.StatusCode = StatusCodes.Status202Accepted;
            return;
        }

        var @params = message["params"] as JsonObject;

        JsonObject response;
        try
        {
            response = method switch
            {
                "initialize" => Result(id, Initialize(@params)),
                "ping" => Result(id, []),
                "tools/list" => Result(id, new JsonObject { ["tools"] = RemoteTools.Definitions() }),
                "tools/call" => Result(id, await CallToolAsync(@params, context.RequestAborted)),
                _ => Error(id, -32601, $"Method not found: {method}")
            };
        }
        catch (OperationCanceledException) when (context.RequestAborted.IsCancellationRequested)
        {
            logger.LogInformation("MCP request {Method} cancelled by caller", method);
            return;
        }

        await WriteAsync(context, response);
    }

    private static JsonObject Initialize(JsonObject? @params)
    {
        // Only tools are used, which are the same in every protocol revision, so accept the client's
        var version = @params?["protocolVersion"] is JsonValue v && v.TryGetValue<string>(out var requested)
            ? requested
            : FallbackProtocolVersion;

        return new JsonObject
        {
            ["protocolVersion"] = version,
            ["capabilities"] = new JsonObject { ["tools"] = new JsonObject() },
            ["serverInfo"] = new JsonObject
            {
                ["name"] = "claudewin9x",
                ["title"] = "ClaudeWin9x remote control",
                ["version"] = typeof(McpEndpoint).Assembly.GetName().Version?.ToString(3) ?? "1.0.0"
            },
            ["instructions"] = RemoteTools.Instructions()
        };
    }

    private async Task<JsonObject> CallToolAsync(JsonObject? @params, CancellationToken cancellationToken)
    {
        var name = @params?["name"] is JsonValue n && n.TryGetValue<string>(out var toolName) ? toolName : null;
        var arguments = @params?["arguments"] as JsonObject ?? [];

        ToolResult result;
        if (name == null)
        {
            result = ToolResult.Fail("Missing tool name");
        }
        else
        {
            logger.LogInformation("MCP tool call: {Tool}", name);
            try
            {
                result = await tools.CallAsync(name, arguments, cancellationToken);
            }
            catch (ToolArgumentException ex)
            {
                result = ToolResult.Fail(ex.Message);
            }
        }

        return new JsonObject
        {
            ["content"] = new JsonArray(new JsonObject { ["type"] = "text", ["text"] = result.Text }),
            ["isError"] = result.IsError
        };
    }

    private static JsonObject Result(JsonNode id, JsonObject result) =>
        new() { ["jsonrpc"] = "2.0", ["id"] = id, ["result"] = result };

    private static JsonObject Error(JsonNode? id, int code, string message) =>
        new() { ["jsonrpc"] = "2.0", ["id"] = id, ["error"] = new JsonObject { ["code"] = code, ["message"] = message } };

    private static async Task WriteAsync(HttpContext context, JsonObject response)
    {
        var bytes = Encoding.UTF8.GetBytes(response.ToJsonString());
        context.Response.ContentType = "application/json";
        context.Response.ContentLength = bytes.Length;
        await context.Response.Body.WriteAsync(bytes, context.RequestAborted);
    }
}
